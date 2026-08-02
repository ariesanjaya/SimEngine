#include "Sim/Core/Log.h"
#include "Sim/Core/Math.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/Widgets.h"
#include "Sim/Render/IViewportRenderer.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sim::editor {
namespace {

constexpr float kLookSensitivity = 0.005f;
constexpr float kFlySpeedMin = 0.05f;
constexpr float kFlySpeedMax = 500.0f;

/// Kamera editor bergaya orbit + fly.
///
/// Orbit disimpan sebagai yaw/pitch/jarak terhadap titik fokus, bukan sebagai
/// matriks. Alasannya: pengguna berpikir dalam "berputar mengelilingi objek"
/// dan "mendekat", dan menyimpan keadaan dalam bentuk itu membuat pembatasan
/// pitch, snapping sudut, dan perintah "focus ke seleksi" (E4) jadi sepele.
///
/// Gerakan terbang menggeser `focus`, bukan posisi kamera secara langsung.
/// Karena posisi kamera diturunkan dari focus, keduanya bergerak bersama —
/// dan begitu tombol kanan dilepas, orbit langsung berputar mengelilingi titik
/// baru di depan kamera alih-alih titik lama yang sudah tertinggal jauh.
struct OrbitCamera {
    Vec3 focus{0.0f, 0.0f, 0.0f};
    float distance = 12.0f;
    float yaw = -0.6f;
    float pitch = 0.45f;

    static constexpr float kPitchLimit = kHalfPi - 0.02f;

    /// Arah dari titik fokus menuju kamera.
    ///
    /// Tanda minus pada komponen X disengaja. Tanpa itu, yaw yang bertambah
    /// menggeser kamera ke +X sehingga arah pandang justru berayun ke kiri —
    /// kebalikan dari yang diharapkan saat menyeret mouse ke kanan. Konvensi
    /// yang dipakai di sini: pada yaw = 0 kamera memandang ke -Z, dan yaw yang
    /// bertambah memutar pandangan ke kanan.
    Vec3 Offset() const {
        return Vec3{-std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                    std::cos(pitch) * std::cos(yaw)};
    }

    Vec3 Forward() const { return -Offset(); }

    /// Sumbu kanan diturunkan dari yaw saja, bukan dari cross(forward, up).
    /// Pada pitch mendekati tegak lurus, cross(forward, up) mendekati nol dan
    /// arah strafe jadi tidak stabil; rumus ini tetap presisi di sudut mana pun.
    Vec3 Right() const { return Vec3{std::cos(yaw), 0.0f, std::sin(yaw)}; }

    void ApplyTo(render::Camera& camera) const {
        camera.position = focus + Offset() * distance;
        camera.rotation = glm::quatLookAt(Forward(), kUp);
    }

    /// deltaPitch positif berarti kamera naik dan pandangan menunduk.
    void Look(float deltaYaw, float deltaPitch) {
        yaw += deltaYaw;
        pitch = std::clamp(pitch + deltaPitch, -kPitchLimit, kPitchLimit);
    }

    void Pan(float deltaX, float deltaY) {
        const Vec3 right = Right();
        const Vec3 up = glm::cross(right, Forward());
        // Kecepatan pan diskalakan jarak: menyeret satu piksel harus memindah
        // dunia sejauh yang terlihat sama, sedekat atau sejauh apa pun kamera.
        const float scale = distance * 0.0018f;
        focus += right * (-deltaX * scale) + up * (deltaY * scale);
    }

    void Zoom(float steps) {
        // Perkalian, bukan penambahan: mendekat dari 100 m dan dari 1 m harus
        // terasa sama cepatnya.
        distance = std::clamp(distance * std::pow(0.88f, steps), 0.05f, 5000.0f);
    }

    void Fly(const Vec3& direction, float metersPerSecond, float deltaSeconds) {
        focus += direction * (metersPerSecond * deltaSeconds);
    }
};

class ViewportPanel final : public Panel {
public:
    ViewportPanel()
        : Panel(panel_id::kViewport, std::string(icons::kPanelViewport) + "  Viewport",
                PanelCategory::Scene) {}

    bool WantsZeroPadding() const override { return true; }

    // Viewport dikunci di dockspace. Kalau ditarik keluar, ia menjadi viewport
    // platform tersendiri: satu swapchain, satu acquire, dan satu present
    // tambahan per frame untuk gambar seukuran panel penuh — biaya yang jauh
    // lebih terasa dibanding panel berisi teks.
    bool IsDockLocked() const override { return true; }

    void OnDraw(EditorContext& context) override {
        render::IViewportRenderer* renderer = context.viewportRenderer;
        if (renderer == nullptr) {
            ImGui::TextDisabled("No renderer.");
            return;
        }

        const ImVec2 size = ImGui::GetContentRegionAvail();
        if (size.x < 8.0f || size.y < 8.0f) {
            return;  // panel terlalu kecil atau sedang dilipat
        }

        const auto width = static_cast<uint32_t>(size.x);
        const auto height = static_cast<uint32_t>(size.y);
        renderer->Resize(width, height);

        HandleInput();

        render::ViewportDesc desc;
        desc.width = width;
        desc.height = height;
        desc.mode = drawMode_;
        desc.showGrid = showGrid_;
        camera_.ApplyTo(desc.camera);
        desc.camera.orthographic = orthographic_;
        desc.camera.orthoHeight = camera_.distance;

        const render::ViewportScene scene;
        renderer->Render(desc, scene);

        const ImVec2 imagePos = ImGui::GetCursorScreenPos();
        const render::TextureHandle texture = renderer->ColorTarget();
        if (texture != render::kInvalidTexture) {
            // UV diambil dari renderer, bukan (1,1): target render boleh lebih
            // besar dari panel supaya menyeret pemisah dock tidak memicu
            // alokasi ulang tiap frame.
            const Vec2 uvMax = renderer->ColorTargetUvMax();
            ImGui::Image(static_cast<ImTextureID>(texture), size, ImVec2(0.0f, 0.0f),
                         ImVec2(uvMax.x, uvMax.y));
        } else {
            ImGui::Dummy(size);
        }

        DrawOverlays(imagePos, size, *renderer);
    }

private:
    void HandleInput() {
        ImGuiIO& io = ImGui::GetIO();
        const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

        // Mode terbang bertahan selama tombol kanan ditahan, walaupun kursor
        // sudah keluar dari panel. Kalau syaratnya "hovered" terus-menerus,
        // memutar pandangan cepat akan terputus di tengah jalan.
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            flying_ = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            flying_ = false;
        }

        if (flying_) {
            const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Right);
            // +delta.y: sumbu Y layar tumbuh ke bawah, dan pitch positif berarti
            // pandangan menunduk. Menyeret ke bawah = melihat ke bawah.
            camera_.Look(delta.x * kLookSensitivity, delta.y * kLookSensitivity);

            // Roda mengatur kecepatan terbang, bukan jarak orbit — konvensi yang
            // sama dengan editor lain, dan lebih berguna: saat sedang terbang
            // yang ingin diubah adalah laju, bukan zoom.
            if (io.MouseWheel != 0.0f) {
                flySpeed_ = std::clamp(flySpeed_ * std::pow(1.15f, io.MouseWheel), kFlySpeedMin,
                                       kFlySpeedMax);
            }
            ApplyFlyMovement(io.DeltaTime);
            return;
        }

        if (!hovered) {
            return;
        }

        if (io.MouseWheel != 0.0f) {
            camera_.Zoom(io.MouseWheel);
        }
        if (io.KeyAlt && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            camera_.Look(delta.x * kLookSensitivity, delta.y * kLookSensitivity);
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
            camera_.Pan(delta.x, delta.y);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            camera_.focus = Vec3(0.0f);
            camera_.distance = 12.0f;
        }
    }

    void ApplyFlyMovement(float deltaSeconds) {
        ImGuiIO& io = ImGui::GetIO();
        // Jangan rebut tombol saat pengguna sedang mengetik di panel lain,
        // kalau tidak mengetik "was" di kotak pencarian akan menerbangkan kamera.
        if (io.WantTextInput) {
            return;
        }

        Vec3 move{0.0f};
        const Vec3 forward = camera_.Forward();
        const Vec3 right = camera_.Right();

        if (ImGui::IsKeyDown(ImGuiKey_W)) {
            move += forward;
        }
        if (ImGui::IsKeyDown(ImGuiKey_S)) {
            move -= forward;
        }
        if (ImGui::IsKeyDown(ImGuiKey_D)) {
            move += right;
        }
        if (ImGui::IsKeyDown(ImGuiKey_A)) {
            move -= right;
        }
        // E/Q naik-turun mengikuti sumbu dunia, bukan sumbu kamera: saat kamera
        // menunduk, "naik" yang diharapkan tetap ke langit.
        if (ImGui::IsKeyDown(ImGuiKey_E)) {
            move += kUp;
        }
        if (ImGui::IsKeyDown(ImGuiKey_Q)) {
            move -= kUp;
        }

        const float lengthSquared = glm::dot(move, move);
        if (lengthSquared < 1e-6f) {
            return;
        }
        // Dinormalkan supaya bergerak diagonal (W+D) tidak lebih cepat daripada
        // lurus.
        move /= std::sqrt(lengthSquared);

        float speed = flySpeed_;
        if (io.KeyShift) {
            speed *= 4.0f;
        }
        if (io.KeyCtrl) {
            speed *= 0.25f;
        }
        camera_.Fly(move, speed, deltaSeconds);
    }

    void DrawOverlays(const ImVec2& imagePos, const ImVec2& size,
                      const render::IViewportRenderer& renderer) {
        const float pad = 10.0f;
        const float button = ImGui::GetFrameHeight();

        // (D1) alat transform di kiri-atas.
        ImGui::SetCursorScreenPos(ImVec2(imagePos.x + pad, imagePos.y + pad));
        ImGui::BeginGroup();
        ImGui::BeginDisabled();
        widgets::IconButton(icons::kSelect, "Select");
        widgets::IconButton(icons::kMove, "Move");
        widgets::IconButton(icons::kRotate, "Rotate");
        widgets::IconButton(icons::kScale, "Scale");
        ImGui::EndDisabled();
        ImGui::EndGroup();

        // (D2) mode tampilan di kanan-atas.
        ImGui::SetCursorScreenPos(ImVec2(imagePos.x + size.x - button - pad, imagePos.y + pad));
        ImGui::BeginGroup();
        if (widgets::IconButton(orthographic_ ? icons::kOrthographic : icons::kPerspective,
                                orthographic_ ? "Orthographic" : "Perspective")) {
            orthographic_ = !orthographic_;
        }
        const bool wireframe = drawMode_ == render::DrawMode::Wireframe;
        if (widgets::IconButton(wireframe ? icons::kShadingWireframe : icons::kShadingLit,
                                wireframe ? "Wireframe" : "Lit")) {
            drawMode_ = wireframe ? render::DrawMode::Lit : render::DrawMode::Wireframe;
        }
        if (widgets::IconButton(icons::kGrid, showGrid_ ? "Hide grid" : "Show grid")) {
            showGrid_ = !showGrid_;
        }
        ImGui::EndGroup();

        // Label renderer di kiri-bawah. Selama masih stub, ini penting: tanpa
        // penanda, mudah mengira grid polos berarti rendering rusak.
        char info[192];
        if (flying_) {
            std::snprintf(info, sizeof(info), "%s  |  %ux%u  |  fly %.1f m/s (WASD, Q/E, Shift)",
                          renderer.Name(), renderer.Width(), renderer.Height(),
                          static_cast<double>(flySpeed_));
        } else {
            std::snprintf(info, sizeof(info), "%s  |  %ux%u  |  dist %.1f m", renderer.Name(),
                          renderer.Width(), renderer.Height(),
                          static_cast<double>(camera_.distance));
        }
        const ImVec2 infoSize = ImGui::CalcTextSize(info);
        ImGui::SetCursorScreenPos(
            ImVec2(imagePos.x + pad, imagePos.y + size.y - infoSize.y - pad));
        ImGui::TextDisabled("%s", info);
    }

    OrbitCamera camera_;
    render::DrawMode drawMode_ = render::DrawMode::Lit;
    float flySpeed_ = 6.0f;
    bool flying_ = false;
    bool orthographic_ = false;
    bool showGrid_ = true;
};

}  // namespace

SIM_REGISTER_PANEL(ViewportPanel, 30)

}  // namespace sim::editor

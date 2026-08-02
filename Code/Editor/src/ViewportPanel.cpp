#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/Log.h"
#include "Sim/Core/Math.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Gizmo.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/SceneCommands.h"
#include "Sim/Editor/SceneView.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Editor/Widgets.h"
#include "Sim/Render/IViewportRenderer.h"
#include "Sim/Scene/Components.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace sim::editor {
namespace {

constexpr float kLookSensitivity = 0.005f;
constexpr float kFlySpeedMin = 0.05f;
constexpr float kFlySpeedMax = 500.0f;

/// Jarak seret (piksel) yang membedakan klik dari seleksi kotak. Di bawah ini
/// tetap dianggap klik, supaya tangan yang sedikit bergeser saat mengklik tidak
/// tiba-tiba membuka kotak seleksi.
constexpr float kDragThreshold = 4.0f;

/// Besar glyph ikon entity terhadap ukuran font teks.
constexpr float kIconScale = 1.6f;

/// Warna entity terpilih, sama dengan yang dipakai renderer untuk wireframe.
constexpr Vec4 kSelectionColor{1.0f, 0.62f, 0.20f, 1.0f};

/// Kamera editor bergaya orbit + fly.
///
/// Orbit disimpan sebagai yaw/pitch/jarak terhadap titik fokus, bukan sebagai
/// matriks. Alasannya: pengguna berpikir dalam "berputar mengelilingi objek"
/// dan "mendekat", dan menyimpan keadaan dalam bentuk itu membuat pembatasan
/// pitch, snapping sudut, dan perintah "focus ke seleksi" jadi sepele.
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
        if (renderer == nullptr || context.world == nullptr || context.selection == nullptr) {
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

        sceneView_.Build(*context.world, *context.selection);
        HandleCameraInput();

        render::ViewportDesc desc;
        desc.width = width;
        desc.height = height;
        desc.mode = drawMode_;
        desc.showGrid = showGrid_;
        camera_.ApplyTo(desc.camera);
        desc.camera.orthographic = orthographic_;
        desc.camera.orthoHeight = camera_.distance;

        renderer->Render(desc, sceneView_.Scene());

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

        // Urutan pengajuan item di bawah ini menentukan siapa yang berhak atas
        // sebuah klik, dan itu bukan detail yang bisa ditukar-tukar.
        //
        // Tombol overlay diajukan lebih dulu di antara item interaktif. ImGui
        // memberikan klik kepada item terhovering yang diajukan paling awal,
        // jadi tombol Rotate menang atas permukaan viewport yang menaunginya.
        // Urutan sebaliknya membuat dua hal salah sekaligus: tombolnya tidak
        // bereaksi, dan kliknya ikut terhitung sebagai klik pada scene sehingga
        // seleksi terhapus.
        // Hover overlay dilaporkan oleh widget-nya sendiri, bukan ditanyakan ke
        // ImGui::IsAnyItemHovered().
        //
        // Fungsi itu ikut memeriksa hover frame SEBELUMNYA
        // (`HoveredIdPreviousFrame`), dan permukaan viewport di bawah ini adalah
        // sebuah item juga. Begitu kursor masuk ke viewport — persis keadaan
        // ketika gizmo dipakai — jawabannya selalu "ya", gizmo diberi
        // `interactive = false`, dan menyeretnya tidak menggerakkan apa pun.
        const bool overlayHovered = DrawOverlays(context, imagePos, size, *renderer);

        // Permukaan viewport sebagai item sungguhan, bukan status mouse mentah:
        // dengan begitu ImGui sendiri yang memutuskan klik ini milik siapa.
        //
        // Kecuali ketika gizmo yang memegang kursor. ImGuizmo menolak memulai
        // manipulasi selama `IsAnyItemHovered()` atau `IsAnyItemActive()` benar
        // — dan permukaan ini menutupi seluruh viewport, jadi selama ia diajukan
        // gizmo TIDAK PERNAH bisa dipakai. Yang menentukan keadaan frame lalu,
        // karena gizmo baru digambar setelah baris ini; jeda satu frame itu
        // tidak terasa, sedangkan urutan sebaliknya mustahil: keduanya
        // saling menunggu.
        //
        // Tidak menyerah saat seretan lain sedang berlangsung — seleksi kotak
        // yang kebetulan melewati gizmo tidak boleh putus di tengah jalan, dan
        // aset yang sedang diseret tetap butuh sasaran jatuh.
        const bool yieldToGizmo = gizmoOwnsPointer_ && !boxSelecting_ &&
                                  ImGui::GetDragDropPayload() == nullptr;
        bool surfacePressed = false;
        bool surfaceHeld = false;
        if (!yieldToGizmo) {
            ImGui::SetCursorScreenPos(imagePos);
            ImGui::InvisibleButton("##viewport_surface", size, ImGuiButtonFlags_MouseButtonLeft);
            surfacePressed = ImGui::IsItemActivated();
            surfaceHeld = ImGui::IsItemActive();
            HandleAssetDrop(context, imagePos, size, desc.camera);
        }

        const float aspect = size.x / size.y;
        const Mat4 view = desc.camera.View();
        const Mat4 projection = desc.camera.Projection(aspect);

        // Ikon digambar sebelum gizmo supaya gizmo selalu di atasnya.
        DrawEntityIcons(projection * view, imagePos, size);

        // Gizmo tetap tergambar walau kursor sedang di atas tombol overlay, tapi
        // tidak boleh ikut merespons kliknya: ia membaca mouse langsung dan tidak
        // tahu apa pun tentang item ImGui.
        const bool gizmoBusy =
            DrawGizmoAndApply(context, imagePos, size, view, projection, !overlayHovered);
        gizmoOwnsPointer_ = gizmoBusy;
        HandleSelectionInput(context, imagePos, size, view, projection, gizmoBusy, surfacePressed,
                             surfaceHeld);
        HandleShortcuts(context);
    }

private:
    // --- kamera -------------------------------------------------------------

    void HandleCameraInput() {
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

    // --- menjatuhkan aset ---------------------------------------------------

    /// Menerima aset yang diseret dari Asset Browser dan membuat entity untuknya.
    void HandleAssetDrop(EditorContext& context, const ImVec2& imagePos, const ImVec2& size,
                         const render::Camera& camera) {
        if (!ImGui::BeginDragDropTarget()) {
            return;
        }
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SIM_ASSET");
        if (payload == nullptr || payload->DataSize != sizeof(Uuid)) {
            ImGui::EndDragDropTarget();
            return;
        }
        const Uuid guid = *static_cast<const Uuid*>(payload->Data);
        ImGui::EndDragDropTarget();

        const assets::AssetRecord* record =
            context.assets != nullptr ? context.assets->Find(guid) : nullptr;
        if (record == nullptr) {
            return;
        }
        // Hanya mesh yang bisa dijatuhkan ke scene. Menjatuhkan tekstur di sini
        // tidak punya arti yang jelas — tekstur dipasang ke field material,
        // bukan berdiri sendiri sebagai objek.
        if (record->type != assets::AssetType::Mesh) {
            context.notifications->Warning(std::string("Cannot place a ") +
                                           assets::ToString(record->type) + " in the scene");
            return;
        }

        const ImVec2 mouse = ImGui::GetMousePos();
        const Vec3 position = GroundPointUnderCursor(
            camera, size, Vec2(mouse.x - imagePos.x, mouse.y - imagePos.y));

        context.history->CloseMergeGroup();
        const Uuid entityGuid = Uuid::Generate();
        const std::string name = record->name;
        context.history->Execute(std::make_unique<LambdaCommand>(
            "Place " + name,
            [world = context.world, selection = context.selection, entityGuid, guid, position,
             name]() {
                const scene::Entity entity = world->CreateWithGuid(entityGuid, name);
                world->TryGet<scene::TransformComponent>(entity)->position = position;
                world->MarkTransformDirty(entity);
                scene::MeshRendererComponent renderer;
                renderer.mesh.guid = guid;
                world->Add<scene::MeshRendererComponent>(entity, renderer);
                selection->SelectOnly(ToSelectionId(entity));
            },
            [world = context.world, selection = context.selection, entityGuid]() {
                world->Destroy(world->FindByGuid(entityGuid));
                selection->Clear();
            }));
    }

    /// Titik pada bidang tanah (y = 0) di bawah kursor.
    ///
    /// Kalau sinarnya menjauh dari tanah — kamera menengadah — objek diletakkan
    /// beberapa meter di depan kamera. Menolak menempatkan apa pun dalam
    /// keadaan itu terasa seperti seretan yang gagal tanpa sebab.
    static Vec3 GroundPointUnderCursor(const render::Camera& camera, const ImVec2& size,
                                       const Vec2& point) {
        const float aspect = size.x / size.y;
        const Ray ray = ScreenPointToRay(camera.View(), camera.Projection(aspect),
                                         Vec2(size.x, size.y), point);
        constexpr float kMinSlope = 1e-4f;
        if (ray.direction.y < -kMinSlope) {
            const float distance = -ray.origin.y / ray.direction.y;
            if (distance > 0.0f) {
                return ray.origin + ray.direction * distance;
            }
        }
        return ray.origin + ray.direction * 10.0f;
    }

    // --- ikon entity --------------------------------------------------------

    /// Menggambar penanda lampu, kamera, dan node kosong sebagai glyph.
    ///
    /// Memakai font ikon yang sudah dimuat editor, bukan tekstur billboard.
    /// Hasilnya tajam di skala DPI mana pun, tidak menambah satu pun aset, dan
    /// ikonnya sama persis dengan yang dipakai Outliner untuk entity yang sama.
    void DrawEntityIcons(const Mat4& viewProjection, const ImVec2& imagePos, const ImVec2& size) {
        const auto& icons = sceneView_.Icons();
        if (icons.empty()) {
            return;
        }

        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImFont* font = ImGui::GetFont();
        const float glyphSize = ImGui::GetFontSize() * kIconScale;

        // Dipotong ke area gambar: tanpa ini ikon di luar tepi viewport akan
        // tergambar menumpuk panel tetangga.
        draw->PushClipRect(imagePos, ImVec2(imagePos.x + size.x, imagePos.y + size.y), true);
        for (const SceneView::EntityIcon& icon : icons) {
            Vec2 screen;
            if (!WorldToScreen(viewProjection, Vec2(imagePos.x, imagePos.y), Vec2(size.x, size.y),
                               icon.position, screen)) {
                continue;
            }
            const Vec4 color = icon.selected ? kSelectionColor : icon.color;
            const ImU32 packed = ImGui::ColorConvertFloat4ToU32(
                ImVec4(color.r, color.g, color.b, color.a));
            const ImVec2 extent = font->CalcTextSizeA(glyphSize, FLT_MAX, 0.0f, icon.glyph);
            const ImVec2 at(screen.x - extent.x * 0.5f, screen.y - extent.y * 0.5f);

            // Bayangan tipis di belakang glyph. Ikon melayang di atas grid dan
            // wireframe yang warnanya berdekatan; tanpa ini penanda gelap
            // hilang di latar gelap.
            draw->AddText(font, glyphSize, ImVec2(at.x + 1.0f, at.y + 1.0f),
                          IM_COL32(0, 0, 0, 140), icon.glyph);
            draw->AddText(font, glyphSize, at, packed, icon.glyph);
        }
        draw->PopClipRect();
    }

    // --- gizmo --------------------------------------------------------------

    /// Entity terpilih yang tidak punya leluhur yang juga terpilih.
    ///
    /// Hanya entity ini yang digerakkan gizmo. Keturunannya ikut bergerak
    /// dengan sendirinya lewat hierarki; menggerakkannya sekali lagi secara
    /// langsung akan menggandakan perpindahannya.
    std::vector<scene::Entity> TopLevelSelection(const EditorContext& context) const {
        std::vector<scene::Entity> entities;
        for (const uint64_t id : context.selection->Items()) {
            const scene::Entity entity = ToEntity(id);
            if (context.world->IsAlive(entity)) {
                entities.push_back(entity);
            }
        }
        std::vector<scene::Entity> result;
        for (const scene::Entity entity : entities) {
            const bool hasSelectedAncestor =
                std::any_of(entities.begin(), entities.end(), [&](scene::Entity other) {
                    return other != entity && context.world->IsDescendantOf(entity, other);
                });
            if (!hasSelectedAncestor) {
                result.push_back(entity);
            }
        }
        return result;
    }

    /// Menggambar gizmo dan menerapkan hasilnya. Mengembalikan true bila gizmo
    /// sedang dipakai atau ditunjuk, sehingga klik tidak boleh dianggap seleksi.
    bool DrawGizmoAndApply(EditorContext& context, const ImVec2& imagePos, const ImVec2& size,
                           const Mat4& view, const Mat4& projection, bool interactive) {
        if (operation_ == GizmoOperation::None) {
            return false;
        }
        const scene::Entity primary = ToEntity(context.selection->Primary());
        if (!context.world->IsAlive(primary)) {
            return false;
        }

        const Mat4 pivot = context.world->WorldMatrix(primary);
        const GizmoResult result =
            DrawGizmo(Vec2(imagePos.x, imagePos.y), Vec2(size.x, size.y), view, projection,
                      operation_, space_, snap_, pivot, interactive);

        if (result.active && !dragging_) {
            BeginDrag(context, pivot);
        }
        if (result.changed && dragging_) {
            ApplyDrag(context, result.transform);
        }
        if (!result.active && dragging_) {
            EndDrag(context);
        }
        return result.active || result.hovered;
    }

    void BeginDrag(const EditorContext& context, const Mat4& pivot) {
        dragging_ = true;
        pivotAtDragStart_ = pivot;
        dragItems_.clear();

        for (const scene::Entity entity : TopLevelSelection(context)) {
            const auto* transform = context.world->TryGet<scene::TransformComponent>(entity);
            if (transform == nullptr) {
                continue;
            }
            DragItem item;
            item.guid = context.world->GuidOf(entity);
            item.entity = entity;
            item.before = *transform;
            item.worldAtDragStart = context.world->WorldMatrix(entity);
            dragItems_.push_back(item);
        }
    }

    void ApplyDrag(EditorContext& context, const Mat4& newPivot) {
        if (dragItems_.empty()) {
            return;
        }
        // Selisih dihitung terhadap keadaan awal seretan, bukan terhadap frame
        // sebelumnya. Menumpuk selisih per frame akan menumpuk galat pembulatan
        // juga, dan hasil snapping tidak akan pernah persis kelipatan.
        const Mat4 delta = newPivot * glm::inverse(pivotAtDragStart_);

        std::vector<SetTransformsCommand::Item> items;
        items.reserve(dragItems_.size());

        for (const DragItem& drag : dragItems_) {
            if (!context.world->IsAlive(drag.entity)) {
                continue;
            }
            const Mat4 newWorld = delta * drag.worldAtDragStart;

            // Hasil gizmo selalu dalam ruang dunia, sedangkan yang disimpan
            // komponen adalah transform lokal. Untuk entity yang punya induk,
            // keduanya berbeda — dan melewatkan langkah ini membuat anak
            // melompat begitu induknya tidak di titik nol.
            const scene::Entity parent = context.world->ParentOf(drag.entity);
            const Mat4 local = context.world->IsAlive(parent)
                                   ? glm::inverse(context.world->WorldMatrix(parent)) * newWorld
                                   : newWorld;

            SetTransformsCommand::Item item;
            item.guid = drag.guid;
            item.before = drag.before;
            item.after = drag.before;
            DecomposeTransform(local, item.after.position, item.after.rotation, item.after.scale);
            items.push_back(item);
        }
        if (items.empty()) {
            return;
        }

        context.history->Execute(std::make_unique<SetTransformsCommand>(
            context.world, std::move(items), OperationLabel()));
    }

    void EndDrag(EditorContext& context) {
        dragging_ = false;
        dragItems_.clear();
        // Menutup grup merge di sini adalah yang membuat satu seretan menjadi
        // tepat satu entri undo: selama tombol ditahan setiap frame digabung ke
        // entri yang sama, dan begitu dilepas entri berikutnya berdiri sendiri.
        context.history->CloseMergeGroup();
    }

    std::string OperationLabel() const {
        switch (operation_) {
            case GizmoOperation::Translate:
                return "Move";
            case GizmoOperation::Rotate:
                return "Rotate";
            case GizmoOperation::Scale:
                return "Scale";
            case GizmoOperation::None:
                break;
        }
        return "Transform";
    }

    // --- seleksi ------------------------------------------------------------

    void HandleSelectionInput(EditorContext& context, const ImVec2& imagePos, const ImVec2& size,
                              const Mat4& view, const Mat4& projection, bool gizmoBusy,
                              bool surfacePressed, bool surfaceHeld) {
        if (flying_ || ImGui::GetIO().KeyAlt) {
            return;  // tombol kiri sedang milik kamera
        }

        if (surfacePressed && !gizmoBusy) {
            boxSelecting_ = true;
            boxStart_ = ImGui::GetMousePos();
        }
        if (!boxSelecting_) {
            return;
        }
        if (!surfaceHeld) {
            FinishSelection(context, imagePos, size, view, projection);
            boxSelecting_ = false;
            return;
        }

        const ImVec2 current = ImGui::GetMousePos();
        const float dragged = std::max(std::abs(current.x - boxStart_.x),
                                       std::abs(current.y - boxStart_.y));
        if (dragged > kDragThreshold) {
            // Kotak digambar di foreground drawlist supaya tetap terlihat di
            // atas gambar viewport tanpa mengganggu urutan widget lain.
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(boxStart_, current, IM_COL32(255, 158, 51, 40));
            draw->AddRect(boxStart_, current, IM_COL32(255, 158, 51, 220));
        }
    }

    void FinishSelection(EditorContext& context, const ImVec2& imagePos, const ImVec2& size,
                         const Mat4& view, const Mat4& projection) {
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 release = ImGui::GetMousePos();
        const float dragged =
            std::max(std::abs(release.x - boxStart_.x), std::abs(release.y - boxStart_.y));

        const bool additive = io.KeyCtrl || io.KeyShift;
        if (!additive) {
            context.selection->Clear();
        }

        if (dragged <= kDragThreshold) {
            // Ikon diuji lebih dulu dan menang atas geometri di belakangnya.
            // Ikon berukuran tetap dan kecil; kalau kalah dengan mesh besar
            // yang kebetulan menaunginya, lampu di dalam ruangan jadi mustahil
            // dipilih lewat viewport.
            const float radius = ImGui::GetFontSize() * kIconScale * 0.5f;
            scene::Entity hit =
                sceneView_.PickIcon(projection * view, Vec2(imagePos.x, imagePos.y),
                                    Vec2(size.x, size.y), Vec2(release.x, release.y), radius);
            if (!scene::IsValid(hit)) {
                const Ray ray =
                    ScreenPointToRay(view, projection, Vec2(size.x, size.y),
                                     Vec2(release.x - imagePos.x, release.y - imagePos.y));
                hit = sceneView_.Raycast(ray);
            }
            if (scene::IsValid(hit)) {
                const uint64_t id = ToSelectionId(hit);
                if (io.KeyCtrl && context.selection->Contains(id)) {
                    context.selection->Remove(id);
                } else {
                    context.selection->Add(id);
                }
            }
            return;
        }

        const ScreenRect rect =
            ScreenRect::FromCorners(Vec2(boxStart_.x, boxStart_.y), Vec2(release.x, release.y));
        const std::vector<scene::Entity> hits = sceneView_.RectSelect(
            projection * view, Vec2(imagePos.x, imagePos.y), Vec2(size.x, size.y), rect);
        for (const scene::Entity entity : hits) {
            context.selection->Add(ToSelectionId(entity));
        }
    }

    // --- pintasan -----------------------------------------------------------

    void HandleShortcuts(EditorContext& context) {
        if (flying_ || ImGui::GetIO().WantTextInput) {
            return;
        }
        if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !IsFocused()) {
            return;
        }

        // Q/W/E/R mengikuti kebiasaan editor lain. Tidak bentrok dengan WASD
        // terbang karena terbang hanya aktif selama tombol kanan ditahan, dan
        // jalur ini sudah keluar lebih awal saat itu terjadi.
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
            operation_ = GizmoOperation::None;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
            operation_ = GizmoOperation::Translate;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
            operation_ = GizmoOperation::Rotate;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
            operation_ = GizmoOperation::Scale;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            space_ = space_ == GizmoSpace::World ? GizmoSpace::Local : GizmoSpace::World;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            FocusSelection(context);
        }
    }

    void FocusSelection(const EditorContext& context) {
        std::vector<scene::Entity> entities;
        for (const uint64_t id : context.selection->Items()) {
            const scene::Entity entity = ToEntity(id);
            if (context.world->IsAlive(entity)) {
                entities.push_back(entity);
            }
        }

        Vec3 boundsMin;
        Vec3 boundsMax;
        if (entities.empty() || !sceneView_.BoundsOf(entities, boundsMin, boundsMax)) {
            camera_.focus = Vec3(0.0f);
            camera_.distance = 12.0f;
            return;
        }

        camera_.focus = (boundsMin + boundsMax) * 0.5f;
        // Jarak dipilih agar seluruh kotak muat di layar dengan sedikit ruang
        // sisa. Radius bola pembungkus dipakai, bukan sisi terpanjang, supaya
        // objek pipih tetap muat dari arah pandang mana pun.
        const float radius = glm::length(boundsMax - boundsMin) * 0.5f;
        camera_.distance = std::max(radius * 2.5f, 0.5f);
    }

    // --- overlay ------------------------------------------------------------

    /// Menggambar overlay viewport, dan melaporkan apakah kursor sedang berada
    /// di atas salah satunya.
    ///
    /// Jawabannya dikumpulkan dari tiap widget tepat setelah diajukan. Itu
    /// bukan gaya penulisan melainkan syarat kebenaran: satu-satunya pertanyaan
    /// yang boleh dijawab di sini adalah "kursor di atas overlay?", dan cara
    /// mana pun yang menanyakannya ke keadaan global ImGui akan ikut menghitung
    /// item lain — termasuk permukaan viewport itu sendiri.
    bool DrawOverlays(const EditorContext& context, const ImVec2& imagePos, const ImVec2& size,
                      const render::IViewportRenderer& renderer) {
        const float pad = 10.0f;
        const float button = ImGui::GetFrameHeight() * widgets::kViewportButtonScale;

        bool hovered = false;
        const auto track = [&hovered]() { hovered = hovered || ImGui::IsItemHovered(); };

        // (D1) alat transform di kiri-atas.
        ImGui::SetCursorScreenPos(ImVec2(imagePos.x + pad, imagePos.y + pad));
        ImGui::BeginGroup();
        OperationButton(icons::kSelect, "Select (Q)", GizmoOperation::None);
        track();
        OperationButton(icons::kMove, "Move (W)", GizmoOperation::Translate);
        track();
        OperationButton(icons::kRotate, "Rotate (E)", GizmoOperation::Rotate);
        track();
        OperationButton(icons::kScale, "Scale (R)", GizmoOperation::Scale);
        track();

        ImGui::Spacing();
        const bool local = space_ == GizmoSpace::Local;
        if (widgets::ViewportButton(local ? icons::kSpaceLocal : icons::kSpaceWorld,
                                    local ? "Local space (X)" : "World space (X)")) {
            space_ = local ? GizmoSpace::World : GizmoSpace::Local;
        }
        track();
        hovered = DrawSnapControl() || hovered;
        ImGui::EndGroup();

        // (D2) mode tampilan di kanan-atas.
        ImGui::SetCursorScreenPos(ImVec2(imagePos.x + size.x - button - pad, imagePos.y + pad));
        ImGui::BeginGroup();
        if (widgets::ViewportButton(orthographic_ ? icons::kOrthographic : icons::kPerspective,
                                    orthographic_ ? "Orthographic" : "Perspective")) {
            orthographic_ = !orthographic_;
        }
        track();
        const bool wireframe = drawMode_ == render::DrawMode::Wireframe;
        if (widgets::ViewportButton(wireframe ? icons::kShadingWireframe : icons::kShadingLit,
                                    wireframe ? "Wireframe" : "Lit")) {
            drawMode_ = wireframe ? render::DrawMode::Lit : render::DrawMode::Wireframe;
        }
        track();
        if (widgets::ViewportButton(icons::kGrid, showGrid_ ? "Hide grid" : "Show grid",
                                    showGrid_)) {
            showGrid_ = !showGrid_;
        }
        track();
        ImGui::EndGroup();

        // Label renderer di kiri-bawah. Selama masih stub, ini penting: tanpa
        // penanda, mudah mengira grid polos berarti rendering rusak.
        char info[224];
        if (flying_) {
            std::snprintf(info, sizeof(info), "%s  |  %ux%u  |  fly %.1f m/s (WASD, Q/E, Shift)",
                          renderer.Name(), renderer.Width(), renderer.Height(),
                          static_cast<double>(flySpeed_));
        } else {
            std::snprintf(info, sizeof(info), "%s  |  %ux%u  |  %zu selected  |  dist %.1f m",
                          renderer.Name(), renderer.Width(), renderer.Height(),
                          context.selection->Count(), static_cast<double>(camera_.distance));
        }
        const ImVec2 infoSize = ImGui::CalcTextSize(info);
        ImGui::SetCursorScreenPos(
            ImVec2(imagePos.x + pad, imagePos.y + size.y - infoSize.y - pad));
        ImGui::TextDisabled("%s", info);
        return hovered;
    }

    void OperationButton(const char* icon, const char* tooltip, GizmoOperation operation) {
        if (widgets::ViewportButton(icon, tooltip, operation_ == operation)) {
            operation_ = operation;
        }
    }

    /// True bila kursor berada di atas tombolnya, atau popup setelannya terbuka.
    ///
    /// Popup ikut dihitung karena gizmo membaca mouse langsung dan tidak tahu
    /// apa-apa tentang jendela ImGui: tanpa ini, menyeret slider "Move (m)" yang
    /// kebetulan berada di atas gizmo akan ikut menggeser entity.
    bool DrawSnapControl() {
        // Penandaan aktif diserahkan ke widget, bukan diurus di sini dengan
        // push/pop sendiri. Versi sebelumnya membaca ulang snap_.enabled setelah
        // tombolnya membalik nilai itu, sehingga push dan pop tidak berpasangan
        // dan ImGui menggugurkan proses. Menyerahkannya ke widget membuat
        // kesalahan seperti itu tidak mungkin terjadi lagi.
        if (widgets::ViewportButton(icons::kSnap, snap_.enabled ? "Snapping on" : "Snapping off",
                                    snap_.enabled)) {
            snap_.enabled = !snap_.enabled;
        }
        bool hovered = ImGui::IsItemHovered();
        // Klik kanan pada tombol yang sama membuka nilainya. Menaruhnya di popup
        // terpisah menjaga bilah alat tetap sempit tanpa menyembunyikan setelan.
        if (ImGui::BeginPopupContextItem("##snap")) {
            hovered = true;
            ImGui::TextDisabled("Snap increments");
            ImGui::Separator();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Move (m)", &snap_.translate, 0.05f, 0.001f, 100.0f, "%.3f");
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Rotate (deg)", &snap_.rotateDegrees, 1.0f, 0.1f, 180.0f, "%.1f");
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Scale", &snap_.scale, 0.01f, 0.001f, 10.0f, "%.3f");
            ImGui::EndPopup();
        }
        return hovered;
    }

    /// Keadaan satu entity saat seretan gizmo dimulai.
    struct DragItem {
        Uuid guid;
        scene::Entity entity = scene::kNullEntity;
        scene::TransformComponent before;
        Mat4 worldAtDragStart{1.0f};
    };

    OrbitCamera camera_;
    SceneView sceneView_;
    render::DrawMode drawMode_ = render::DrawMode::Lit;
    GizmoOperation operation_ = GizmoOperation::Translate;
    GizmoSpace space_ = GizmoSpace::World;
    GizmoSnap snap_;
    std::vector<DragItem> dragItems_;
    Mat4 pivotAtDragStart_{1.0f};
    ImVec2 boxStart_{0.0f, 0.0f};
    float flySpeed_ = 6.0f;
    bool flying_ = false;
    bool dragging_ = false;
    bool boxSelecting_ = false;
    /// Gizmo sedang ditunjuk atau dipakai pada frame LALU. Dipakai memutuskan
    /// apakah permukaan viewport diajukan sebagai item ImGui frame ini.
    bool gizmoOwnsPointer_ = false;
    bool orthographic_ = false;
    bool showGrid_ = true;
};

}  // namespace

SIM_REGISTER_PANEL(ViewportPanel, 30)

}  // namespace sim::editor

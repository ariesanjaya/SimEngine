#include "Sim/Scene/Components.h"

#include "Sim/Core/Log.h"
#include "Sim/Scene/ComponentRegistry.h"

#include <mutex>

namespace sim::scene {

ComponentRegistry& ComponentRegistry::Get() {
    static ComponentRegistry registry;
    return registry;
}

const ComponentOps* ComponentRegistry::Find(std::string_view typeName) const {
    for (const ComponentOps& ops : components_) {
        if (ops.type != nullptr && ops.type->name == typeName) {
            return &ops;
        }
    }
    return nullptr;
}

void RegisterCoreComponents() {
    // Sekali per proses. World memanggilnya di konstruktor, dan sebuah aplikasi
    // bisa punya beberapa World (editor + preview).
    static std::once_flag once;
    std::call_once(once, []() {
        reflect::TypeRegistry& types = reflect::TypeRegistry::Get();
        ComponentRegistry& components = ComponentRegistry::Get();

        // Urutan pendaftaran = urutan komponen di berkas level. Jangan diubah
        // tanpa alasan: mengubahnya membuat setiap level tersimpan menghasilkan
        // diff besar meski isinya tidak berubah.
        types.Type<NameComponent>("Name")
            .Field<&NameComponent::name>("name")
            .Tooltip("Nama yang tampil di Outliner");
        components.Register<NameComponent>(/*removable=*/false, /*addable=*/false);

        types.Type<TransformComponent>("Transform")
            .Field<&TransformComponent::position>("position")
            .Label("Translate")
            .Speed(0.01f)
            .Field<&TransformComponent::rotation>("rotation")
            .Label("Rotate")
            .Degrees()
            .Field<&TransformComponent::scale>("scale")
            .Label("Scale")
            .Speed(0.01f);
        components.Register<TransformComponent>(false, false);

        types.Type<VisibilityComponent>("Visibility")
            .Field<&VisibilityComponent::visible>("visible")
            .Field<&VisibilityComponent::locked>("locked")
            .Tooltip("Terkunci: tetap terlihat, tapi tidak bisa dipilih di viewport");
        components.Register<VisibilityComponent>();

        types.Type<StaticFlagComponent>("Static")
            .Field<&StaticFlagComponent::isStatic>("isStatic")
            .Label("Static")
            .Tooltip("Objek statis boleh di-bake ke struktur yang tidak bisa bergerak");
        components.Register<StaticFlagComponent>();

        types.Type<MeshRendererComponent>("MeshRenderer")
            .Field<&MeshRendererComponent::mesh>("mesh")
            .Label("Mesh Asset")
            .Field<&MeshRendererComponent::material>("material")
            .Label("Material")
            .Field<&MeshRendererComponent::castShadows>("castShadows")
            .Label("Cast Shadows")
            .Field<&MeshRendererComponent::receiveShadows>("receiveShadows")
            .Label("Receive Shadows")
            .Field<&MeshRendererComponent::lodBias>("lodBias")
            .Label("LOD Bias")
            .Range(-4.0f, 4.0f);
        components.Register<MeshRendererComponent>();

        // Didaftarkan supaya vektornya di ScriptComponent bisa diserialisasi dan
        // dibandingkan lewat jalur reflection yang sudah ada — termasuk diff
        // per-field yang dipakai undo dan multi-select.
        types.Type<ScriptProperty>("ScriptProperty")
            .Field<&ScriptProperty::name>("name")
            .Field<&ScriptProperty::kind>("kind")
            .EnumNames({"Number", "Bool", "Text"})
            .Field<&ScriptProperty::number>("number")
            .Field<&ScriptProperty::flag>("flag")
            .Field<&ScriptProperty::text>("text");

        types.Type<ScriptComponent>("Script")
            .Field<&ScriptComponent::script>("script")
            .Label("Script Asset")
            .Field<&ScriptComponent::loaded>("loaded")
            .Label("Loaded")
            .ReadOnly()
            .Field<&ScriptComponent::properties>("properties")
            // Disembunyikan dari grid generik: sebagai daftar struct yang bisa
            // ditambah dan dikurangi sendiri, isinya justru menyesatkan —
            // bentuknya ditentukan berkas skrip, bukan pengguna. Inspector
            // menggambarnya sebagai baris bertipe di tempat lain.
            .Hidden();
        components.Register<ScriptComponent>();

        types.Type<LightComponent>("Light")
            .Field<&LightComponent::type>("type")
            .EnumNames({"Directional", "Point", "Spot"})
            .Field<&LightComponent::color>("color")
            .Color()
            .Field<&LightComponent::intensity>("intensity")
            .Range(0.0f, 100.0f)
            .Field<&LightComponent::range>("range")
            .Range(0.0f, 1000.0f)
            .Tooltip("Tidak berlaku untuk lampu directional")
            .Field<&LightComponent::innerAngleRadians>("innerAngle")
            .Label("Inner Angle")
            .Degrees()
            .Range(0.0f, kPi)
            .Field<&LightComponent::outerAngleRadians>("outerAngle")
            .Label("Outer Angle")
            .Degrees()
            .Range(0.0f, kPi)
            .Field<&LightComponent::castShadows>("castShadows")
            .Label("Cast Shadows");
        components.Register<LightComponent>();

        types.Type<CameraComponent>("Camera")
            .Field<&CameraComponent::fovYRadians>("fovY")
            .Label("Field of View")
            .Degrees()
            .Range(0.0f, kPi)
            .Field<&CameraComponent::nearZ>("nearZ")
            .Label("Near")
            .Range(0.001f, 100.0f)
            .Field<&CameraComponent::farZ>("farZ")
            .Label("Far")
            .Range(1.0f, 100000.0f)
            .Field<&CameraComponent::orthographic>("orthographic")
            .Field<&CameraComponent::orthoHeight>("orthoHeight")
            .Label("Ortho Height")
            .Range(0.01f, 10000.0f);
        components.Register<CameraComponent>();

        std::vector<std::string> problems;
        if (!types.Validate(problems)) {
            for (const std::string& problem : problems) {
                SIM_ERROR("Scene", "Reflection problem: {}", problem);
            }
        }
        SIM_INFO("Scene", "{} component types registered", components.All().size());
    });
}

}  // namespace sim::scene

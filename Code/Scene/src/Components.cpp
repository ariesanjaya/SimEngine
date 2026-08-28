#include "Sim/Scene/Components.h"

#include "Sim/Core/Log.h"
#include "Sim/Scene/ComponentRegistry.h"
#include "Sim/Scene/WorldSettings.h"

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
            .Field<&MeshRendererComponent::materials>("materials")
            .Label("Materials")
            .Tooltip("Satu slot per ruas mesh; yang kosong memakai material bawaan editor")
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

        types.Type<GraphComponent>("Graph")
            .Field<&GraphComponent::graph>("graph")
            .Label("Graph Asset")
            .Field<&GraphComponent::loaded>("loaded")
            .Label("Loaded")
            .ReadOnly()
            .Field<&GraphComponent::properties>("properties")
            // Alasan yang sama dengan ScriptComponent: bentuk daftarnya
            // ditentukan variabel graph, bukan pengguna.
            .Hidden();
        components.Register<GraphComponent>();

        types.Type<AnimatorComponent>("Animator")
            .Field<&AnimatorComponent::clip>("clip")
            .Label("Clip")
            .Tooltip("Berkas .simanim atau .fbx; rangkanya datang dari aset mesh")
            .Field<&AnimatorComponent::playing>("playing")
            .Label("Playing")
            .Field<&AnimatorComponent::loop>("loop")
            .Label("Loop")
            .Field<&AnimatorComponent::speed>("speed")
            .Label("Speed")
            .Range(-4.0f, 4.0f);
        // `time` sengaja tidak didaftarkan: ia keadaan runtime, dan medan yang
        // berubah tiap frame di dalam berkas level berarti level yang mengotori
        // dirinya sendiri tanpa ada yang menyuntingnya.
        components.Register<AnimatorComponent>();

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
            .Field<&LightComponent::sourceRadius>("sourceRadius")
            .Label("Source Radius")
            .Range(0.001f, 5.0f)
            .Tooltip("Jari-jari lampu. Membatasi seberapa terang ia bisa jadi dari dekat")
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

        types.Type<SkyComponent>("Sky")
            .Field<&SkyComponent::source>("source")
            .EnumNames({"Atmosphere", "HDR Map"})
            .Tooltip("Atmosfer dihitung dari fisika; HDR Map satu cuplikan tekstur")
            .Field<&SkyComponent::intensity>("intensity")
            .Label("Sky Gain")
            .Range(0.0f, 100.0f)
            .Field<&SkyComponent::cameraHeightKm>("cameraHeightKm")
            .Label("Camera Height (km)")
            .Range(0.0f, 60.0f)
            .Field<&SkyComponent::aerialPerspective>("aerialPerspective")
            .Label("Aerial Perspective")
            .Field<&SkyComponent::aerialHaze>("aerialHaze")
            .Label("Haze")
            .Range(0.0f, 40.0f)
            .Field<&SkyComponent::hdriPath>("hdriPath")
            .Label("HDR File")
            .Field<&SkyComponent::hdriRotation>("hdriRotation")
            .Label("HDR Rotation")
            .Degrees()
            .Range(-kPi, kPi)
            .Field<&SkyComponent::hdriIntensity>("hdriIntensity")
            .Label("HDR Gain")
            .Range(0.0f, 10.0f);
        components.Register<SkyComponent>();

        // **Didaftarkan ke TypeRegistry, dan sengaja tidak ke ComponentRegistry.**
        // Yang kedua adalah daftar yang menentukan apa yang boleh menempel di
        // sebuah entity dan apa yang ikut tertulis ke `.simprefab`; World
        // Settings bukan keduanya. Yang pertama sudah cukup untuk membuat
        // PropertyGrid merendernya, serialisasi menulisnya, dan alat MCP
        // melihatnya — tanpa satu widget pun yang ditulis tangan.
        //
        // Nama enum di sini adalah **nama yang tertulis di berkas level**, jadi
        // mengubahnya mengubah arti berkas yang sudah ada. Skema JSON-nya ada di
        // docs/PLAN-IBL.md.
        types.Type<WorldSettings>("WorldSettings")
            .Field<&WorldSettings::indirect>("indirect")
            .Label("Indirect Lighting")
            .EnumNames({"None", "Precomputed", "RealTime"})
            .Tooltip("None mematikannya; Precomputed memanggang seluruhnya dan menuntut "
                     "matahari yang diam; RealTime menelusurinya tiap frame")
            .Field<&WorldSettings::environment>("environment")
            .Label("Environment")
            .EnumNames({"Sky", "File"})
            .Tooltip("Yang menyinari tingkat panggang: langit yang tergambar, "
                     "atau berkas HDR yang disebut Sky. Tidak berlaku untuk RealTime")
            .Field<&WorldSettings::exposureMode>("exposureMode")
            .Label("Exposure")
            .EnumNames({"Automatic", "Manual"})
            .Field<&WorldSettings::exposureCompensation>("exposureCompensation")
            .Label("Compensation")
            .Range(-5.0f, 5.0f)
            .Tooltip("Stop; berlaku pada kedua mode. Positif berarti lebih terang")
            .Field<&WorldSettings::extractSun>("extractSun")
            .Label("Extract Sun From File")
            .Tooltip("Keluarkan matahari dari berkas lingkungan supaya lampu directional "
                     "yang mengantarkannya — tanpa ini keduanya menyala sekaligus");

        types.Type<RigidBodyComponent>("RigidBody")
            .Field<&RigidBodyComponent::kind>("kind")
            .EnumNames({"Static", "Kinematic", "Dynamic"})
            .Tooltip("Static tidak pernah bergerak; Kinematic digerakkan transform, bukan gaya")
            .Field<&RigidBodyComponent::mass>("mass")
            .Label("Mass (kg)")
            .Tooltip("Nol berarti dihitung dari bentuk collider dan Density")
            .Range(0.0f, 100000.0f)
            .Field<&RigidBodyComponent::density>("density")
            .Label("Density (kg/m³)")
            .Range(1.0f, 25000.0f)
            .Field<&RigidBodyComponent::linearDamping>("linearDamping")
            .Label("Linear Damping")
            .Range(0.0f, 10.0f)
            .Field<&RigidBodyComponent::angularDamping>("angularDamping")
            .Label("Angular Damping")
            .Range(0.0f, 10.0f)
            .Field<&RigidBodyComponent::allowSleeping>("allowSleeping")
            .Label("Allow Sleeping");
        components.Register<RigidBodyComponent>();

        types.Type<ColliderComponent>("Collider")
            .Field<&ColliderComponent::shape>("shape")
            .EnumNames({"Box", "Sphere", "Capsule", "Plane", "Cylinder", "Whitebox", "Terrain"})
            .Tooltip("Plane tak hingga hanya untuk Static; Whitebox dan Terrain mengambil "
                     "bentuknya dari komponen bernama sama pada entity ini")
            .Field<&ColliderComponent::halfExtents>("halfExtents")
            .Label("Half Extents")
            .Field<&ColliderComponent::radius>("radius")
            .Label("Radius")
            .Range(0.001f, 1000.0f)
            .Field<&ColliderComponent::halfHeight>("halfHeight")
            .Label("Half Height")
            .Tooltip("Setengah bagian silinder kapsul, tanpa kedua tudungnya")
            .Range(0.0f, 1000.0f)
            .Field<&ColliderComponent::offset>("offset")
            .Label("Offset")
            .Field<&ColliderComponent::staticFriction>("staticFriction")
            .Label("Static Friction")
            .Range(0.0f, 2.0f)
            .Field<&ColliderComponent::dynamicFriction>("dynamicFriction")
            .Label("Dynamic Friction")
            .Range(0.0f, 2.0f)
            .Field<&ColliderComponent::restitution>("restitution")
            .Label("Restitution")
            .Tooltip("Nol tidak memantul; di atas satu energinya bertambah tiap pantulan")
            .Range(0.0f, 1.0f);
        components.Register<ColliderComponent>();

        types.Type<JointComponent>("Joint")
            .Field<&JointComponent::type>("type")
            .EnumNames({"Fixed", "Revolute", "Prismatic", "Spherical", "D6"})
            .Tooltip("Revolute engsel, Prismatic geseran, Spherical sendi bola")
            .Field<&JointComponent::connectedBody>("connectedBody")
            .Label("Connected Body")
            .Tooltip("Kosong berarti tersendi ke dunia, pada titik tetap di ruang")
            .Field<&JointComponent::anchor>("anchor")
            .Label("Anchor")
            .Field<&JointComponent::frame>("frame")
            .Label("Frame")
            .Degrees()
            .Tooltip("Sumbu sendi adalah +X bingkai ini")
            .Field<&JointComponent::limitEnabled>("limitEnabled")
            .Label("Limit")
            .Field<&JointComponent::lowerLimit>("lowerLimit")
            .Label("Lower")
            .Field<&JointComponent::upperLimit>("upperLimit")
            .Label("Upper")
            .Field<&JointComponent::collisionEnabled>("collisionEnabled")
            .Label("Collision")
            .Tooltip("Membiarkan kedua benda saling menabrak; biasanya membuatnya bergetar")
            .Field<&JointComponent::breakForce>("breakForce")
            .Label("Break Force")
            .Tooltip("Nol berarti tidak pernah patah")
            .Field<&JointComponent::breakTorque>("breakTorque")
            .Label("Break Torque");
        components.Register<JointComponent>();

        types.Type<VehicleComponent>("Vehicle")
            .Field<&VehicleComponent::chassisHalfExtents>("chassisHalfExtents")
            .Label("Chassis Half Extents")
            .Field<&VehicleComponent::chassisMass>("chassisMass")
            .Label("Chassis Mass (kg)")
            .Range(1.0f, 100000.0f)
            .Field<&VehicleComponent::centerOfMassOffset>("centerOfMassOffset")
            .Label("Center of Mass")
            .Tooltip("Hampir selalu di bawah pusat kotak; yang setinggi pusat mudah terguling")
            .Field<&VehicleComponent::wheelbase>("wheelbase")
            .Label("Wheelbase (m)")
            .Range(0.5f, 20.0f)
            .Field<&VehicleComponent::trackWidth>("trackWidth")
            .Label("Track Width (m)")
            .Range(0.3f, 10.0f)
            .Field<&VehicleComponent::axleHeight>("axleHeight")
            .Label("Axle Height")
            .Field<&VehicleComponent::wheelRadius>("wheelRadius")
            .Label("Wheel Radius")
            .Range(0.05f, 3.0f)
            .Field<&VehicleComponent::wheelWidth>("wheelWidth")
            .Label("Wheel Width")
            .Range(0.02f, 2.0f)
            .Field<&VehicleComponent::wheelMass>("wheelMass")
            .Label("Wheel Mass (kg)")
            .Range(0.5f, 500.0f)
            .Field<&VehicleComponent::suspensionTravel>("suspensionTravel")
            .Label("Suspension Travel")
            .Range(0.01f, 2.0f)
            .Field<&VehicleComponent::maxSteerAngle>("maxSteerAngle")
            .Label("Max Steer")
            .Degrees()
            .Range(0.0f, 1.5f)
            .Field<&VehicleComponent::peakDriveTorque>("peakDriveTorque")
            .Label("Drive Torque (N·m)")
            .Range(0.0f, 20000.0f)
            .Field<&VehicleComponent::maxBrakeTorque>("maxBrakeTorque")
            .Label("Brake Torque (N·m)")
            .Range(0.0f, 50000.0f)
            .Field<&VehicleComponent::maxHandbrakeTorque>("maxHandbrakeTorque")
            .Label("Handbrake Torque (N·m)")
            .Range(0.0f, 50000.0f)
            .Field<&VehicleComponent::tireFriction>("tireFriction")
            .Label("Tire Friction")
            .Range(0.0f, 2.0f)
            .Field<&VehicleComponent::drive>("drive")
            .EnumNames({"Front Wheel", "Rear Wheel", "All Wheel"});
        components.Register<VehicleComponent>();

        types.Type<WhiteboxComponent>("Whitebox")
            .Field<&WhiteboxComponent::whitebox>("whitebox")
            .Label("Whitebox Asset")
            .Tooltip("Topologi yang bisa disunting; segitiganya dibangun dari sini")
            .Field<&WhiteboxComponent::showEdges>("showEdges")
            .Label("Show Edges");
        components.Register<WhiteboxComponent>();

        types.Type<TerrainComponent>("Terrain")
            .Field<&TerrainComponent::terrain>("terrain")
            .Label("Terrain Asset")
            .Tooltip("Heightmap berubin beserta layer dan peta bobotnya")
            .Field<&TerrainComponent::showTiles>("showTiles")
            .Label("Show Tiles");
        components.Register<TerrainComponent>();

        types.Type<DecalComponent>("Decal")
            .Field<&DecalComponent::texture>("texture")
            .Label("Texture")
            .Tooltip("Dipetakan 0..1 sepanjang jejak decal; kosong berarti warnanya saja")
            .Field<&DecalComponent::color>("color")
            .Label("Color")
            .Field<&DecalComponent::lift>("lift")
            .Label("Lift")
            .Tooltip("Jarak angkat dari permukaan; nol menghasilkan z-fighting")
            .Range(0.0f, 1.0f)
            .Field<&DecalComponent::maxSteps>("maxSteps")
            .Label("Max Steps")
            .Tooltip("Batas petak per sumbu, supaya decal raksasa tidak menjadi "
                     "jutaan segitiga")
            .Range(1.0f, 256.0f);
        components.Register<DecalComponent>();

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

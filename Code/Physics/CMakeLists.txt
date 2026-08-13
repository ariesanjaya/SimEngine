sim_add_library(Physics
    SOURCES
        src/PhysicsScene.cpp
        src/PhysicsWorld.cpp
        src/Ragdoll.cpp
    PUBLIC_DEPS
        # **Yang tidak ada di daftar ini justru yang penting: Sim::Render.**
        # Modul ini menghasilkan transform; yang menggambarnya adalah
        # pemanggilnya. Aturan yang sama menjaga SimHeadless — dan karena itu
        # server dedicated — tetap bisa mensimulasikan tanpa perangkat grafis.
        Sim::Core
        # Arahnya disengaja: fisika yang melihat scene, bukan sebaliknya.
        # `Sim::Scene` dipakai importir aset, serialisasi, dan tool yang tidak
        # pernah mensimulasikan apa pun — membalik arahnya membuat semuanya ikut
        # menautkan PhysX. PUBLIC karena `PhysicsScene.h` menyebut
        # `scene::World` di tanda tangannya.
        Sim::Scene)

# PhysX hanya kalau ada; PhysicsWorld.cpp tetap ikut dibangun tanpanya dan
# menolak dengan pesan yang menyebut sebabnya, mengikuti pola UsdImport.cpp.
if(TARGET PhysX::PhysX)
    target_link_libraries(SimPhysics PRIVATE PhysX::PhysX)
    target_compile_definitions(SimPhysics PRIVATE
        SIM_WITH_PHYSX=1
        SIM_PHYSX_VERSION_STRING="${SIM_PHYSX_VERSION}")
    if(SIM_WITH_PHYSX_GPU)
        target_compile_definitions(SimPhysics PRIVATE SIM_WITH_PHYSX_GPU=1)
    endif()
endif()

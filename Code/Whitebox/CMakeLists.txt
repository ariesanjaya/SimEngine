sim_add_library(Whitebox
    SOURCES
        src/HalfEdgeMesh.cpp
        src/Operations.cpp
        src/Polygon.cpp
        src/WhiteboxMesh.cpp
    PUBLIC_DEPS
        # `Vec3` dan tidak lebih. **Yang tidak ada di daftar ini yang penting:**
        # Sim::Render dan Sim::Editor. Modul ini menghasilkan geometri; yang
        # menggambarnya dan yang menyuntingnya adalah pemanggilnya — aturan yang
        # sama dengan Sim::Physics.
        Sim::Core
        # `BuildMeshData` menghasilkan `assets::MeshData` — tipe pertukaran mesh
        # milik mesin ini. Header publik Sim::Assets tidak membocorkan FBX, USD,
        # maupun stb, jadi yang ikut terseret hanya Sim::Core dan Sim::Material.
        Sim::Assets)

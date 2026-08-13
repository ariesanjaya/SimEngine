sim_add_library(Whitebox
    SOURCES
        src/HalfEdgeMesh.cpp
        src/Polygon.cpp
    PUBLIC_DEPS
        # `Vec3` dan tidak lebih. **Yang tidak ada di daftar ini yang penting:**
        # Sim::Render dan Sim::Editor. Modul ini menghasilkan geometri; yang
        # menggambarnya dan yang menyuntingnya adalah pemanggilnya — aturan yang
        # sama dengan Sim::Physics.
        Sim::Core)

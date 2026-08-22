# SceneView: terjemahan `scene::World` → `render::ViewportScene`, beserta simpanan
# yang dibutuhkannya.
#
# **Modul ini ada karena player dan editor menjawab pertanyaan yang sama.**
# "Apa yang harus digambar untuk dunia ini" tidak berbeda antara viewport editor
# dan permainan yang dikirim: mesh yang sama diunggah, material yang sama
# dikompilasi, terrain dan whitebox yang sama dibangun. Sampai E9 jawabannya
# tinggal di `Sim::EditorFramework`, dan akibatnya `SimRuntime` menautkan ImGui,
# riwayat undo, dan `PanelManager` yang tidak pernah dipakainya — sebuah player
# yang membawa editornya sendiri.
#
# **Yang TIDAK ada di daftar ini yang penting:** Sim::EditorFramework,
# Sim::ImGuiIntegration, dan font ikon. Glyph penanda entity diisi pemanggilnya
# lewat `SceneView::IconGlyphs` — lihat alasannya di header.
sim_add_library(SceneView
    SOURCES
        src/MaterialPrograms.cpp
        src/SceneView.cpp
        src/Selection.cpp
        src/SkinnedPreview.cpp
        src/TerrainStore.cpp
        src/WhiteboxStore.cpp
    PUBLIC_DEPS
        Sim::Core
        # `Build` menerima `scene::World` dan mengisi `render::ViewportScene`;
        # keduanya muncul di tanda tangan publiknya.
        Sim::Scene
        Sim::Render
        # Mesh, tekstur, dan material dicari lewat GUID di database aset, lalu
        # dipanggangkan bila belum ada bentuk siap-GPU-nya.
        Sim::Assets
        Sim::Material
        # Pose skinning dihitung `SkinnedPreview`, yang menyimpan klip dan
        # rangkanya sendiri.
        Sim::Animation
        # `TerrainStore` dan `WhiteboxStore` memegang heightmap dan geometri
        # whitebox-nya, dan keduanya menyebut tipe modulnya di header.
        Sim::Terrain
        Sim::Whitebox)

sim_add_library(EditorFramework
    SOURCES
        src/Actions.cpp
        src/Command.cpp
        src/DockLayout.cpp
        src/EditorApp.cpp
        src/EditorScripting.cpp
        src/EditorShell.cpp
        src/Gizmo.cpp
        src/NodeGraph.cpp
        src/Notifications.cpp
        src/SceneCommands.cpp
        src/SceneView.cpp
        src/SkinnedPreview.cpp
        src/PanelManager.cpp
        src/PropertyGrid.cpp
        src/PanelRegistry.cpp
        src/ProjectLibrary.cpp
        src/Selection.cpp
        src/Widgets.cpp
    PUBLIC_DEPS
        Sim::Animation
        Sim::Assets
        Sim::Core
        # Dialog berkas sistem. Diagram dependensi di docs/ARCHITECTURE.md memang
        # menggambar panah Platform → EditorFramework; ini pemakai pertamanya.
        # Yang dipakai hanya `Sim/Platform/FileDialog.h`, dan header itu sengaja
        # tidak membocorkan satu pun tipe SDL.
        Sim::Platform
        Sim::Render
        Sim::Scene
        # Hanya inti ImGui: target ini tidak membawa header Vulkan maupun SDL,
        # sesuai aturan modul di docs/ARCHITECTURE.md.
        ImGui::ImGui
        IconFonts::Headers
    PRIVATE_DEPS
        # Hanya Gizmo.cpp yang memakainya, dan header Gizmo.h sengaja tidak
        # membocorkan satu pun tipe ImGuizmo — jadi panel tetap tidak tahu apa
        # yang dipakai di baliknya.
        ImGuizmo::ImGuizmo
        # Alasan yang sama: hanya NodeGraph.cpp yang memakainya, dan
        # NodeGraph.h tidak membocorkan satu pun tipe pustaka itu.
        ImGuiNodeEditor::ImGuiNodeEditor
        nlohmann_json::nlohmann_json)

# Lua bersifat opsional, dan EditorApp punya jalur Play/Stop yang memerlukannya.
# Definisinya dipasang di sini, bukan hanya di executable: tanpa ini Play()
# terkompilasi menjadi fungsi kosong di dalam modul ini sementara pemanggilnya
# di executable terlihat baik-baik saja — kesalahan yang tidak menghasilkan satu
# pun peringatan kompilasi.
if(SIM_WITH_LUA)
    target_link_libraries(SimEditorFramework PUBLIC Sim::Script)
    target_compile_definitions(SimEditorFramework PUBLIC SIM_WITH_LUA=1)
endif()

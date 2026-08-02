sim_add_library(EditorFramework
    SOURCES
        src/Actions.cpp
        src/Command.cpp
        src/DockLayout.cpp
        src/EditorApp.cpp
        src/EditorShell.cpp
        src/Gizmo.cpp
        src/Notifications.cpp
        src/SceneCommands.cpp
        src/SceneView.cpp
        src/PanelManager.cpp
        src/PropertyGrid.cpp
        src/PanelRegistry.cpp
        src/Selection.cpp
        src/Widgets.cpp
    PUBLIC_DEPS
        Sim::Core
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
        nlohmann_json::nlohmann_json)

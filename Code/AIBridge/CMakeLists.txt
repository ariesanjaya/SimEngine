sim_add_library(AIBridge
    SOURCES
        src/JsonRpc.cpp
        src/McpServer.cpp
        src/ResourceRegistry.cpp
        src/ToolRegistry.cpp
    PUBLIC_DEPS
        # MainThreadQueue: setiap handler yang menyentuh World/ImGui/Vulkan
        # wajib lewat sana, dan `McpServerConfig` menyebut timeout-nya.
        Sim::Core
    # PRIVATE: header publik AIBridge tidak membocorkan httplib maupun nlohmann.
    # httplib yang paling penting dikurung — ia menyeret header soket dan, di
    # platform lain, <windows.h>. Seam-nya karena itu teks JSON, bukan
    # `nlohmann::json`: format kawat MCP memang teks, jadi tidak ada yang
    # disederhanakan, hanya tidak diteruskan.
    PRIVATE_DEPS
        nlohmann_json::nlohmann_json
        httplib::httplib)

#pragma once

namespace sim::editor::panel_id {

// Id panel dipakai di tiga tempat: judul jendela ("Name###id"), penyusunan
// layout dock bawaan, dan berkas imgui.ini. Karena itu id tidak boleh berubah
// setelah dirilis — mengubahnya membuat layout tersimpan milik pengguna hilang.
inline constexpr const char* kOutliner = "outliner";
inline constexpr const char* kInspector = "inspector";
inline constexpr const char* kAssetBrowser = "assetbrowser";
inline constexpr const char* kViewport = "viewport";
inline constexpr const char* kConsole = "console";
inline constexpr const char* kLuaConsole = "luaconsole";
inline constexpr const char* kScriptEditor = "scripteditor";
inline constexpr const char* kGraphEditor = "grapheditor";
inline constexpr const char* kMaterialEditor = "materialeditor";
inline constexpr const char* kParticleEditor = "particleeditor";
inline constexpr const char* kTerrainEditor = "terraineditor";
inline constexpr const char* kVegetationEditor = "vegetationeditor";
inline constexpr const char* kStatistics = "statistics";
inline constexpr const char* kHistory = "history";
inline constexpr const char* kPreferences = "preferences";

}  // namespace sim::editor::panel_id

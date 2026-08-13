#pragma once

#include <IconsLucide.h>

/// Name ikon bermakna untuk seluruh editor.
///
/// Panel memakai `icons::kMove`, bukan `ICON_LC_MOVE_3D`. Lapisan tipis ini ada
/// karena dua alasan:
///
///  1. Mengganti set ikon (mis. ke Font Awesome) hanya menyentuh berkas ini,
///     bukan setiap panel.
///  2. Name bermakna memaksa konsistensi. Tanpa ini, "hapus" akan digambar
///     dengan trash di satu panel dan X di panel lain, dan ketidakcocokan itu
///     baru terlihat setelah tersebar ke mana-mana.
///
/// Set yang dipakai: Lucide 1.28 (ISC). Gaya garisnya sepadan dengan ketebalan
/// Inter, sehingga ikon tidak tampil lebih "berat" daripada teks di sebelahnya
/// — masalah yang muncul kalau memakai ikon solid bersama font teks biasa.
namespace sim::editor::icons {

// --- Alat transform (toolbar & overlay viewport) ---------------------------
inline constexpr const char* kSelect = ICON_LC_MOUSE_POINTER_2;
inline constexpr const char* kMove = ICON_LC_MOVE_3D;
inline constexpr const char* kRotate = ICON_LC_ROTATE_3D;
inline constexpr const char* kScale = ICON_LC_SCALE_3D;
inline constexpr const char* kSpaceWorld = ICON_LC_GLOBE;
inline constexpr const char* kSpaceLocal = ICON_LC_BOX;
inline constexpr const char* kSnap = ICON_LC_MAGNET;
inline constexpr const char* kPivot = ICON_LC_CROSSHAIR;

// --- Tampilan viewport -----------------------------------------------------
inline constexpr const char* kPerspective = ICON_LC_AXIS_3D;
inline constexpr const char* kOrthographic = ICON_LC_SQUARE_DASHED;
inline constexpr const char* kShadingLit = ICON_LC_SUN_MEDIUM;
inline constexpr const char* kShadingWireframe = ICON_LC_COMPONENT;
inline constexpr const char* kGrid = ICON_LC_GRID_3X3;
inline constexpr const char* kFocus = ICON_LC_TARGET;

// --- Kontrol simulasi ------------------------------------------------------
inline constexpr const char* kPlay = ICON_LC_PLAY;
inline constexpr const char* kPause = ICON_LC_PAUSE;
inline constexpr const char* kStop = ICON_LC_SQUARE;

// --- Panel -----------------------------------------------------------------
inline constexpr const char* kPanelOutliner = ICON_LC_LIST_TREE;
inline constexpr const char* kPanelInspector = ICON_LC_SLIDERS_HORIZONTAL;
inline constexpr const char* kPanelAssetBrowser = ICON_LC_FOLDER_TREE;
inline constexpr const char* kPanelViewport = ICON_LC_MONITOR;
inline constexpr const char* kPanelConsole = ICON_LC_TERMINAL;
inline constexpr const char* kPanelLuaConsole = ICON_LC_SQUARE_TERMINAL;
inline constexpr const char* kPanelStatistics = ICON_LC_ACTIVITY;
inline constexpr const char* kPanelTimeOfDay = ICON_LC_SUN;

// --- Tipe entity & komponen ------------------------------------------------
inline constexpr const char* kEntity = ICON_LC_BOX;
inline constexpr const char* kEntityGroup = ICON_LC_BLOCKS;
inline constexpr const char* kPrefab = ICON_LC_PACKAGE;
inline constexpr const char* kMesh = ICON_LC_SHAPES;
inline constexpr const char* kWhitebox = ICON_LC_BOXES;
inline constexpr const char* kLight = ICON_LC_LIGHTBULB;
inline constexpr const char* kSunLight = ICON_LC_SUN;
inline constexpr const char* kCamera = ICON_LC_VIDEO;
inline constexpr const char* kSky = ICON_LC_CLOUD_SUN;
inline constexpr const char* kTerrain = ICON_LC_MOUNTAIN_SNOW;
inline constexpr const char* kVegetation = ICON_LC_TREES;
/// Volume `.vdb` — asap, api, awan.
inline constexpr const char* kVolume = ICON_LC_CLOUD;
inline constexpr const char* kPhysics = ICON_LC_ATOM;
inline constexpr const char* kParticle = ICON_LC_SPARKLES;
inline constexpr const char* kWater = ICON_LC_WAVES;
inline constexpr const char* kTransform = ICON_LC_MOVE_3D;
inline constexpr const char* kScript = ICON_LC_FILE_CODE;
inline constexpr const char* kChevronUp = ICON_LC_CHEVRON_UP;
inline constexpr const char* kChevronDown = ICON_LC_CHEVRON_DOWN;

// --- Aset ------------------------------------------------------------------
inline constexpr const char* kFolder = ICON_LC_FOLDER;
inline constexpr const char* kFolderOpen = ICON_LC_FOLDER_OPEN;
inline constexpr const char* kAssetTexture = ICON_LC_IMAGE;
inline constexpr const char* kAssetMaterial = ICON_LC_PALETTE;
inline constexpr const char* kAssetMesh = ICON_LC_SHAPES;
inline constexpr const char* kAssetAnimation = ICON_LC_FILM;
inline constexpr const char* kAssetLevel = ICON_LC_EARTH;
inline constexpr const char* kAssetUnknown = ICON_LC_FILE_QUESTION;

// --- Editor khusus (menu Tools, E7) ---------------------------------------
inline constexpr const char* kMaterialEditor = ICON_LC_PALETTE;
inline constexpr const char* kParticleEditor = ICON_LC_SPARKLES;
inline constexpr const char* kTerrainEditor = ICON_LC_MOUNTAIN_SNOW;
inline constexpr const char* kVegetationEditor = ICON_LC_TREES;
inline constexpr const char* kAnimationEditor = ICON_LC_FILM;
inline constexpr const char* kNodeGraph = ICON_LC_GIT_BRANCH;
inline constexpr const char* kCurve = ICON_LC_SPLINE;
inline constexpr const char* kBrush = ICON_LC_BRUSH;
inline constexpr const char* kSculpt = ICON_LC_SHOVEL;
inline constexpr const char* kHole = ICON_LC_CIRCLE_DASHED;
inline constexpr const char* kLayers = ICON_LC_LAYERS;
inline constexpr const char* kScatter = ICON_LC_SPRAY_CAN;
inline constexpr const char* kPlant = ICON_LC_SPROUT;
inline constexpr const char* kErase = ICON_LC_ERASER;
inline constexpr const char* kSeed = ICON_LC_DICES;
inline constexpr const char* kBone = ICON_LC_BONE;
inline constexpr const char* kKeyframe = ICON_LC_DIAMOND;
inline constexpr const char* kEvent = ICON_LC_FLAG;
inline constexpr const char* kLoop = ICON_LC_REPEAT;
inline constexpr const char* kImport = ICON_LC_IMPORT;
inline constexpr const char* kExport = ICON_LC_UPLOAD;

// --- Aksi ------------------------------------------------------------------
inline constexpr const char* kAdd = ICON_LC_PLUS;
inline constexpr const char* kDelete = ICON_LC_TRASH_2;
inline constexpr const char* kDuplicate = ICON_LC_COPY;
inline constexpr const char* kSave = ICON_LC_SAVE;
inline constexpr const char* kOpen = ICON_LC_FOLDER_OPEN;
inline constexpr const char* kUndo = ICON_LC_UNDO_2;
inline constexpr const char* kRedo = ICON_LC_REDO_2;
inline constexpr const char* kSearch = ICON_LC_SEARCH;
inline constexpr const char* kFilter = ICON_LC_FUNNEL;
inline constexpr const char* kRefresh = ICON_LC_REFRESH_CW;
inline constexpr const char* kSettings = ICON_LC_SETTINGS;
inline constexpr const char* kClose = ICON_LC_X;
// Panah keluar/masuk, bukan kotak: kotak adalah lambang "maksimalkan" milik
// dekorasi jendela OS, dan panel mengambang di sini digambar sendiri oleh ImGui
// tanpa dekorasi itu. Panah menyatakan arah perubahannya, yang juga membuat
// pasangan maksimalkan/pulihkan terbaca tanpa harus mengingat mana yang mana.
inline constexpr const char* kMaximize = ICON_LC_MAXIMIZE_2;
inline constexpr const char* kRestore = ICON_LC_MINIMIZE_2;

// --- Keadaan ---------------------------------------------------------------
inline constexpr const char* kVisible = ICON_LC_EYE;
inline constexpr const char* kHidden = ICON_LC_EYE_OFF;
inline constexpr const char* kLocked = ICON_LC_LOCK;
inline constexpr const char* kUnlocked = ICON_LC_LOCK_OPEN;
inline constexpr const char* kExpanded = ICON_LC_CHEVRON_DOWN;
inline constexpr const char* kCollapsed = ICON_LC_CHEVRON_RIGHT;

// --- Tingkat log -----------------------------------------------------------
inline constexpr const char* kLogTrace = ICON_LC_ACTIVITY;
inline constexpr const char* kLogDebug = ICON_LC_BUG;
inline constexpr const char* kLogInfo = ICON_LC_INFO;
inline constexpr const char* kLogWarn = ICON_LC_TRIANGLE_ALERT;
inline constexpr const char* kLogError = ICON_LC_CIRCLE_ALERT;
inline constexpr const char* kLogCritical = ICON_LC_OCTAGON_ALERT;

// --- AI / MCP (track A) ----------------------------------------------------
inline constexpr const char* kAiAssistant = ICON_LC_BOT;
inline constexpr const char* kAiBridge = ICON_LC_DATABASE;
inline constexpr const char* kLua = ICON_LC_SCROLL_TEXT;

}  // namespace sim::editor::icons

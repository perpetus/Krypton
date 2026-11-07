/*
 * Krypton UI contract.
 *
 * The only interface between a browser host (Chromium/Krypton) and a platform
 * UI module (krypton_ui.dll on Windows, a framework on macOS later). Plain C
 * so the host never needs the module's language, compiler or C++ standard.
 *
 * Model: state in, intents out.
 *   - The host pushes browser state into the module through KrShellUI.
 *   - The module reports what the user asked for through KrBrowser.
 *
 * Rules for both sides:
 *   - Every call happens on the host's UI thread.
 *   - Pointers and strings passed into a call are valid only for that call.
 *     Copy anything you keep.
 *   - Strings are UTF-8 and length-delimited (KrStr); they are not required
 *     to be NUL-terminated.
 *   - Geometry is in DIPs, relative to the client area of the host window.
 *   - No exceptions, longjmps or other unwinding may cross this boundary.
 *   - Tables start with struct_size. New functions are only ever appended, so
 *     a caller must check that a slot lies inside struct_size before using it
 *     (see KR_HAS_SLOT). That lets old hosts and new modules work together.
 * - ABI v2 payload records also start with struct_size. Initialize it to the
 *   caller's sizeof(record). Unknown trailing fields are ignored; incomplete
 *   required prefixes are rejected without changing state. Within v2, new
 *   fields must be appended and read only after KR_HAS_FIELD succeeds.
 * - Arrays carry a byte element_size, normally sizeof(element). Every element
 *   has its own struct_size, which must fit inside that stride. Empty arrays
 *   may use NULL and a zero stride. Nonempty arrays require valid storage for
 *   count * element_size bytes. KrMenu.item_size is its nested item stride.
 * - KrStr, KrRect, colors, ids and KrThemePoint are fixed value types, not
 *   independently extensible records. Changing them requires an ABI bump.
 */
#ifndef KRYPTON_UI_ABI_H_
#define KRYPTON_UI_ABI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on any incompatible change. Appending a slot to a table is compatible
 * and does not need a bump. */
#define KR_UI_ABI_VERSION 2u

#if defined(_WIN32)
#define KR_UI_CALL __cdecl
#if defined(KR_UI_BUILDING)
#define KR_UI_API __declspec(dllexport)
#else
#define KR_UI_API
#endif
#else
#define KR_UI_CALL
#define KR_UI_API __attribute__((visibility("default")))
#endif

/* True when `table` is big enough to contain `slot`, for tables that begin
 * with struct_size. */
#define KR_HAS_SLOT(table, type, slot) \
  ((table)->struct_size >= offsetof(type, slot) + sizeof((table)->slot))

/* A payload follows the same complete-field rule as a callback table. */
#define KR_HAS_FIELD(record, type, field) KR_HAS_SLOT(record, type, field)

/* ---------------------------------------------------------------- basics -- */

typedef uint64_t KrTabId;
typedef uint64_t KrFolderId;
typedef uint64_t KrSpaceId;
typedef uint64_t KrActionId;
typedef uint64_t KrImageId; /* 0 means "no image" */

typedef struct KrStr {
  const char* data;
  size_t size;
} KrStr;

typedef struct KrRect {
  float x;
  float y;
  float width;
  float height;
} KrRect;

/* Packed 0xAARRGGBB. */
typedef uint32_t KrColor;

typedef enum KrResult {
  KR_OK = 0,
  KR_ERR_VERSION = 1,     /* host and module disagree on the ABI */
  KR_ERR_UNSUPPORTED = 2, /* platform or OS can't run this module */
  KR_ERR_INVALID_ARGUMENT = 3,
  KR_ERR_PLATFORM = 4 /* native UI framework failed to start */
} KrResult;

typedef enum KrLogLevel {
  KR_LOG_VERBOSE = 0,
  KR_LOG_INFO = 1,
  KR_LOG_WARNING = 2,
  KR_LOG_ERROR = 3
} KrLogLevel;

/* ------------------------------------------------------------ state in -- */

/* Pixels for favicons and action icons. Sent once per id with put_image and
 * referenced by id afterwards. 32-bit BGRA, premultiplied alpha. */
typedef struct KrImage {
  size_t struct_size;
  KrImageId id;
  int32_t width;
  int32_t height;
  int32_t stride; /* bytes per row */
  const uint8_t* pixels;
} KrImage;

enum KrTabFlags {
  KR_TAB_PINNED = 1u << 0,   /* shown in the space's pinned section */
  KR_TAB_FAVORITE = 1u << 1, /* shown in the favorites grid (all spaces) */
  KR_TAB_LOADING = 1u << 2,
  KR_TAB_AUDIBLE = 1u << 3,
  KR_TAB_MUTED = 1u << 4,
  KR_TAB_CRASHED = 1u << 5,
  KR_TAB_RECORDING = 1u << 6, /* camera or microphone in use */
  KR_TAB_CAPTURING = 1u << 7, /* screen or tab being shared */
  KR_TAB_ATTENTION = 1u << 8  /* wants attention, e.g. a pending dialog */
};

typedef struct KrTab {
  size_t struct_size;
  KrTabId id;
  KrSpaceId space;
  uint32_t flags; /* KrTabFlags */
  KrStr title;
  KrStr url;
  KrImageId favicon;
  /* Optional workspace folder; zero means an ordinary tab. */
  KrFolderId folder;
} KrTab;

/* Persistent workspace folders. Parent zero denotes a root. Order is among
 * siblings; folders are published parent-first and updated incrementally. */
typedef struct KrFolder {
  size_t struct_size;
  KrFolderId id;
  KrSpaceId space;
  KrFolderId parent;
  KrStr name;
  KrStr glyph;
  size_t order;
  bool collapsed;
} KrFolder;
enum KrFolderCommandKind {
  KR_FOLDER_CREATE,
  KR_FOLDER_RENAME,
  KR_FOLDER_COLLAPSE,
  KR_FOLDER_MOVE,
  KR_FOLDER_MOVE_TAB,
  KR_FOLDER_UNPACK,
  KR_FOLDER_DELETE,
  KR_FOLDER_NEW_TAB
};
typedef struct KrFolderCommand {
  size_t struct_size;
  uint32_t kind;
  KrFolderId folder;
  KrFolderId parent;
  KrTabId tab;
  KrSpaceId space;
  size_t index;
  KrStr name;
  KrStr glyph;
  bool collapsed;
} KrFolderCommand;

enum KrTabPreviewFlags {
  KR_TAB_PREVIEW_MEMORY_KNOWN = 1u << 0,
  KR_TAB_PREVIEW_SLEEPING = 1u << 1,
  KR_TAB_PREVIEW_SAMPLE = 1u << 2,
  KR_TAB_PREVIEW_MEMORY_ESTIMATED = 1u << 3,
  KR_TAB_PREVIEW_LABELS_PROVIDED = 1u << 4,
  KR_TAB_PREVIEW_NO_IMAGE = 1u << 5 /* browser intentionally hides preview */
};

/* Optional hover data. Images use put_image/drop_image. Missing memory data
 * is distinct from a measured zero. All values describe this tab, not the
 * entire browser process. The host captures/measures asynchronously. */
typedef struct KrTabPreview {
  size_t struct_size;
  KrTabId tab;
  KrImageId thumbnail;
  uint64_t memory_usage_bytes;
  uint32_t flags; /* KrTabPreviewFlags */
  /* Optional localized browser hover-card labels. When LABELS_PROVIDED is
   * set, all four fields must be present; empty labels deliberately hide
   * that text. Older hosts retain the UI's title/domain/status formatting. */
  KrStr title;
  KrStr domain;
  KrStr memory_text;
  KrStr status;
} KrTabPreview;

typedef struct KrSpace {
  size_t struct_size;
  KrSpaceId id;
  KrStr name;
  KrStr glyph;   /* short label or emoji shown in the space switcher */
  KrColor color; /* drives the window gradient while the space is active */
} KrSpace;

typedef enum KrSecurityLevel {
  KR_SECURITY_NONE = 0, /* internal pages, nothing to show */
  KR_SECURITY_SECURE = 1,
  KR_SECURITY_INFO = 2,    /* e.g. file:// or a downgraded connection */
  KR_SECURITY_WARNING = 3, /* not secure */
  KR_SECURITY_DANGEROUS = 4
} KrSecurityLevel;

typedef struct KrNavState {
  size_t struct_size;
  bool can_go_back;
  bool can_go_forward;
  bool is_loading;
  uint32_t security;  /* KrSecurityLevel */
  KrStr display_text; /* what the address pill shows, usually the host name */
  KrStr full_url;     /* what the address field edits */
} KrNavState;

enum KrLoadProgressFlags {
  KR_LOAD_PROGRESS_ENABLED = 1u << 0,
  KR_LOAD_PROGRESS_LOADING = 1u << 1,
  KR_LOAD_PROGRESS_SAME_DOCUMENT = 1u << 2
};
/* Active primary-frame HTTP(S) progress. Push after
 * set_active_tab/set_nav_state. navigation_id increases for each navigation;
 * progress is finite in [0,1]. */
typedef struct KrLoadProgress {
  size_t struct_size;
  KrTabId tab;
  uint64_t navigation_id;
  float progress;
  uint32_t flags;
} KrLoadProgress;

/* Anything clickable with an icon: address-bar page actions, site-setting
 * icons, extension buttons, pinned browser actions. The host decides which
 * exist; the module only draws them and reports clicks. */
typedef enum KrActionKind {
  KR_ACTION_PAGE = 0,         /* bookmark star, zoom, translate, ... */
  KR_ACTION_SITE_SETTING = 1, /* blocked popups, camera in use, ... */
  KR_ACTION_EXTENSION = 2,
  KR_ACTION_PINNED = 3,     /* pinned browser actions: history, print, ... */
  KR_ACTION_TOOLBAR = 4,    /* downloads, profile, app menu, ... */
  KR_ACTION_SITE_TOOLS = 5, /* unified site panel; stays in the address field */
  KR_ACTION_COPY_LINK = 6   /* current-page link; stays in the address field */
} KrActionKind;

enum KrActionFlags {
  KR_ACTION_VISIBLE = 1u << 0,
  KR_ACTION_ENABLED = 1u << 1,
  KR_ACTION_ACTIVE = 1u << 2,   /* toggled on, e.g. page bookmarked */
  KR_ACTION_ATTENTION = 1u << 3 /* highlight, e.g. update available */
};

typedef struct KrAction {
  size_t struct_size;
  KrActionId id;
  uint32_t kind;  /* KrActionKind */
  uint32_t flags; /* KrActionFlags */
  KrStr label;
  KrStr tooltip;
  KrImageId icon;
  KrStr glyph; /* used when icon is 0: a symbol-font glyph or short text */
  KrStr badge; /* extension badge text, empty for none */
  KrColor badge_color;
  float progress; /* 0..1 for progress rings (downloads), <0 for none */
  /* Optional text colour for `badge`; transparent or absent draws white. The
   * module draws a non-empty `badge` over `icon`, so `icon` must not
   * include one. */
  KrColor badge_text_color;
} KrAction;

typedef struct KrTheme {
  size_t struct_size;
  bool dark;
  bool incognito;
  bool high_contrast;
  KrColor accent;
} KrTheme;

enum KrThemeScheme {
  KR_THEME_SYSTEM = 0,
  KR_THEME_LIGHT = 1,
  KR_THEME_DARK = 2
};
enum KrThemeHarmony {
  KR_THEME_FLOATING = 0,
  KR_THEME_COMPLEMENTARY = 1,
  KR_THEME_SINGLE_ANALOGOUS = 2,
  KR_THEME_SPLIT_COMPLEMENTARY = 3,
  KR_THEME_ANALOGOUS = 4,
  KR_THEME_TRIADIC = 5
};
enum KrThemeColorMode {
  KR_THEME_POSITION = 0,
  KR_THEME_LIGHTNESS = 1,
  KR_THEME_GRAYSCALE = 2
};
enum KrWorkspaceThemeFlags { KR_THEME_CUSTOMIZED = 1u << 0 };
typedef struct KrThemePoint {
  float x, y; /* normalized canvas coordinates, independent of display scale */
  KrColor color;
} KrThemePoint;
typedef struct KrWorkspaceTheme {
  size_t struct_size;
  KrSpaceId space; /* zero means the current/default workspace */
  uint32_t flags;
  uint32_t scheme;
  uint32_t harmony;
  uint32_t mode;
  uint32_t point_count; /* 0..3; points[0] is the primary color */
  float opacity;
  float texture;   /* 0..1, quantized to sixteenths by the editor */
  float lightness; /* 0..100 */
  KrThemePoint points[3];
} KrWorkspaceTheme;

enum KrWindowFlags {
  KR_WINDOW_ACTIVE = 1u << 0,
  KR_WINDOW_MAXIMIZED = 1u << 1,
  KR_WINDOW_FULLSCREEN = 1u << 2,    /* browser full screen (F11) */
  KR_WINDOW_TAB_FULLSCREEN = 1u << 3 /* a page went full screen */
};

typedef struct KrWindowState {
  size_t struct_size;
  uint32_t flags;     /* KrWindowFlags */
  float scale_factor; /* physical pixels per DIP */
  KrRect client;      /* client area in DIPs, origin is always 0,0 */
} KrWindowState;

enum KrLayoutFlags {
  KR_LAYOUT_SIDEBAR_RIGHT = 1u << 0,
  KR_LAYOUT_SIDEBAR_COLLAPSED = 1u
      << 1, /* fully hidden; retained for old hosts */
  KR_LAYOUT_SIDEBAR_AUTOHIDE = 1u << 2, /* reveal on hover at the edge */
  KR_LAYOUT_TOOLBAR_SINGLE = 1u
      << 3, /* Only Sidebar; otherwise sidebar + top bar */
  KR_LAYOUT_TOOLBAR_COLLAPSED = 1u
      << 4, /* Collapsed Sidebar + top bar; wins over SINGLE */
  KR_LAYOUT_SIDEBAR_ICON_ONLY = 1u
      << 5, /* keep the sidebar's compact icon rail */
  KR_LAYOUT_COMPACT_MODE = 1u << 6,
  KR_LAYOUT_COMPACT_HIDE_SIDEBAR = 1u << 7,
  KR_LAYOUT_COMPACT_HIDE_TOOLBAR = 1u << 8,
  /* Hide targets apply with COMPACT_MODE. Without explicit targets, neither
   * bit hides the sidebar in Vertical, or top chrome in horizontal layouts.
   * Reveals overlay the page without changing its bounds. */
  /* Horizontal layouts are mutually exclusive. With none set, the existing
   * vertical-sidebar flags apply. Switching orientation may retain them. */
  KR_LAYOUT_TABS_CLASSIC = 1u << 9,
  KR_LAYOUT_TABS_COMPACT = 1u << 10,
  KR_LAYOUT_TABS_DYNAMIC = 1u << 11,
  KR_LAYOUT_CENTERED_ADDRESS = 1u << 12,
  KR_LAYOUT_MINIMAL_ADDRESS = 1u << 13,
  KR_LAYOUT_SQUARE_FRAME = 1u << 14, /* rounded frame is the default */
  KR_LAYOUT_COMPACT_TARGETS_EXPLICIT = 1u
      << 15, /* no hide targets: both pinned */
  KR_LAYOUT_WORKSPACE_SWIPE = 1u
      << 16, /* horizontal touchpad swipe on sidebar */
};

typedef struct KrLayoutPrefs {
  size_t struct_size;
  uint32_t flags; /* KrLayoutFlags */
  float sidebar_width;
} KrLayoutPrefs;

typedef enum KrSuggestionKind {
  KR_SUGGESTION_URL = 0,
  KR_SUGGESTION_SEARCH = 1,
  KR_SUGGESTION_HISTORY = 2,
  KR_SUGGESTION_BOOKMARK = 3,
  KR_SUGGESTION_OPEN_TAB = 4,
  KR_SUGGESTION_BANG = 5,
  KR_SUGGESTION_COMMAND = 6
} KrSuggestionKind;

enum KrSuggestionFlags {
  KR_SUGGESTION_DEFAULT = 1u << 0, /* what Enter picks */
  KR_SUGGESTION_DELETABLE = 1u << 1,
  KR_SUGGESTION_SELECTED = 1u << 2,  /* authoritative omnibox selection */
  KR_SUGGESTION_RICH_IMAGE = 1u << 3 /* larger Chromium suggestion image */
};

enum KrSuggestionTextStyle {
  KR_SUGGESTION_TEXT_URL = 1u << 0,
  KR_SUGGESTION_TEXT_MATCH = 1u << 1,
  KR_SUGGESTION_TEXT_DIM = 1u << 2,
  KR_SUGGESTION_TEXT_TOOLBELT = 1u << 3
};
typedef struct KrSuggestionClassification {
  size_t struct_size;
  uint32_t offset; /* UTF-16 code units, at a code point boundary */
  uint32_t style;  /* KrSuggestionTextStyle bits */
} KrSuggestionClassification;

typedef struct KrSuggestion {
  size_t struct_size;
  uint32_t kind;  /* KrSuggestionKind */
  uint32_t flags; /* KrSuggestionFlags */
  KrStr contents;
  KrStr description;
  KrImageId icon;
  /* Optional styled-text tail. Borrowed for this call and copied by the UI.
     Old records end after icon. */
  const KrSuggestionClassification* contents_class;
  size_t contents_class_count;
  size_t contents_class_stride;
  const KrSuggestionClassification* description_class;
  size_t description_class_count;
  size_t description_class_stride;
} KrSuggestion;

/* Menus arrive flattened: menus[0] is the root, and an item with
 * submenu >= 0 opens menus[submenu]. */
typedef enum KrMenuItemType {
  KR_MENU_ITEM_COMMAND = 0,
  KR_MENU_ITEM_CHECK = 1,
  KR_MENU_ITEM_RADIO = 2,
  KR_MENU_ITEM_SEPARATOR = 3,
  KR_MENU_ITEM_SUBMENU = 4,
  KR_MENU_ITEM_TITLE = 5,
  KR_MENU_ITEM_HEADER = 6,   /* panel header; submenu contains its actions */
  KR_MENU_ITEM_CONTROLS = 7, /* inline panel controls from submenu */
  KR_MENU_ITEM_GRID = 8,     /* four-column icon tiles from submenu */
  KR_MENU_ITEM_SECTION = 9,  /* heading with trailing actions from submenu */
  KR_MENU_ITEM_FOOTER = 10   /* footer buttons from submenu */
} KrMenuItemType;

enum KrMenuItemFlags {
  KR_MENU_ITEM_ENABLED = 1u << 0,
  KR_MENU_ITEM_CHECKED = 1u << 1,
  KR_MENU_ITEM_KEEP_OPEN = 1u << 2 /* panel command updates without dismissal */
};

/* Source component metadata is independent of the current menu renderer.
 * Preserve invisible rows and unused attributes. Source enum/color ids are
 * informational host values. Every nested pointer is borrowed only for the
 * show_menu call, just like the containing menu and its strings. */
enum KrMenuMetadataFlags {
  KR_MENU_META_VISIBLE = 1u << 0,
  KR_MENU_META_DYNAMIC = 1u << 1,
  KR_MENU_META_MAY_HAVE_MNEMONICS = 1u << 2,
  KR_MENU_META_MINOR_TEXT_IS_URL = 1u << 3,
  KR_MENU_META_MINOR_ICON_ON_RIGHT = 1u << 4,
  KR_MENU_META_FORCE_ACCELERATOR = 1u << 5,
  KR_MENU_META_ALERTED = 1u << 6,
  KR_MENU_META_NEW_FEATURE = 1u << 7,
  KR_MENU_META_BUTTON_GROUP_MEMBER = 1u << 8,
  KR_MENU_META_ACTIONABLE_SUBMENU = 1u << 9,
  KR_MENU_META_CUSTOM_FONT = 1u << 10
};

typedef struct KrKeyboardAccelerator {
  size_t struct_size;
  uint32_t key_code;
  int32_t modifiers;
  uint32_t key_state;
  int64_t time_ticks_us;
  int32_t source_device_id;
  bool interrupted_by_mouse_event;
} KrKeyboardAccelerator;

enum KrMenuFontFlags {
  KR_MENU_FONT_ANTIALIASING = 1u << 0,
  KR_MENU_FONT_SUBPIXEL_POSITIONING = 1u << 1,
  KR_MENU_FONT_AUTOHINTER = 1u << 2,
  KR_MENU_FONT_USE_BITMAPS = 1u << 3
};
typedef struct KrMenuFont {
  size_t struct_size;
  KrStr family;
  const KrStr* actual_families;
  size_t actual_family_count;
  int32_t size_pixels;
  int32_t style;
  int32_t weight;
  int32_t height;
  int32_t baseline;
  int32_t cap_height;
  uint32_t render_flags; /* KrMenuFontFlags */
  uint32_t hinting;
  uint32_t subpixel_rendering;
  float text_contrast;
  float text_gamma;
} KrMenuFont;

enum KrMenuColorRole {
  KR_MENU_COLOR_FOREGROUND = 0,
  KR_MENU_COLOR_SUBMENU_BACKGROUND = 1,
  KR_MENU_COLOR_SELECTED_BACKGROUND = 2
};
typedef struct KrMenuColor {
  size_t struct_size;
  uint32_t role; /* KrMenuColorRole */
  int32_t semantic_id;
  bool resolved; /* false retains the semantic id without inventing RGBA */
  KrColor color;
} KrMenuColor;

typedef struct KrMenuItemMetadata {
  size_t struct_size;
  int32_t source_type;
  int32_t source_command_id;
  uint64_t source_index;
  int32_t source_button_type; /* -1: ordinary row; otherwise host button type */
  uint64_t source_button_index;
  uint32_t flags; /* KrMenuMetadataFlags */
  int32_t radio_group;
  int32_t separator_type;
  uint32_t badge_type; /* 0: none, 1: new, 2: preview */
  KrStr secondary_label;
  KrStr minor_text;
  KrStr accessible_name;
  KrStr element_identifier;
  KrStr button_row_label;
  KrImageId minor_icon;
  const KrKeyboardAccelerator* keyboard_accelerator;
  const KrMenuFont* fonts; /* source fallback order; empty uses module font */
  size_t font_count;
  size_t font_size;
  const KrMenuColor* colors;
  size_t color_count;
  size_t color_size;
  /* Aggregate source font-list metrics, distinct from each font's metrics. */
  int32_t font_style;
  int32_t font_size_pixels;
  int32_t font_weight;
  int32_t font_height;
  int32_t font_baseline;
  int32_t font_cap_height;
} KrMenuItemMetadata;

typedef struct KrMenuItem {
  size_t struct_size;
  int32_t command_id;
  uint32_t type;  /* KrMenuItemType */
  uint32_t flags; /* KrMenuItemFlags */
  KrStr label;
  KrStr accelerator; /* display text only, e.g. "Ctrl+T" */
  KrImageId icon;
  int32_t submenu; /* index into the menus array, -1 for none */
  KrStr glyph;     /* UTF-8 native symbol glyph; empty uses icon. */
  /* Optional sized tail. Older ABI v2 producers end at glyph. */
  const KrMenuItemMetadata* metadata;
  /* Optional tile badge, copied during delivery. Empty hides the badge. */
  KrStr badge;
  KrColor badge_color;
} KrMenuItem;

typedef struct KrMenu {
  size_t struct_size;
  const KrMenuItem* items;
  size_t count;
  size_t item_size; /* byte stride, normally sizeof(KrMenuItem) */
} KrMenu;

/* Source aliases for existing menu entry points. All v2 menus use the same
 * extensible item record; no growable record is embedded before another field.
 */
typedef KrMenuItem KrMenuItemWithGlyph;
typedef KrMenu KrMenuWithGlyphs;

/* ----------------------------------------------------------- intents out -- */

typedef enum KrNavCommand {
  KR_NAV_BACK = 0,
  KR_NAV_FORWARD = 1,
  KR_NAV_RELOAD = 2,
  KR_NAV_STOP = 3,
  KR_NAV_HOME = 4
} KrNavCommand;

typedef enum KrDisposition {
  KR_OPEN_CURRENT_TAB = 0,
  KR_OPEN_NEW_TAB = 1,
  KR_OPEN_NEW_BACKGROUND_TAB = 2,
  KR_OPEN_NEW_WINDOW = 3
} KrDisposition;

typedef enum KrWindowCommand {
  KR_WINDOW_MINIMIZE = 0,
  KR_WINDOW_TOGGLE_MAXIMIZE = 1,
  KR_WINDOW_CLOSE = 2
} KrWindowCommand;

typedef enum KrMenuKind {
  KR_MENU_APP = 0,
  KR_MENU_TAB = 1, /* target: tab id */
  KR_MENU_BACK_HISTORY = 2,
  KR_MENU_FORWARD_HISTORY = 3,
  KR_MENU_RELOAD = 4,
  KR_MENU_ACTION = 5, /* target: action id */
  KR_MENU_SIDEBAR = 6,
  KR_MENU_SPACE = 7, /* target: space id */
  KR_MENU_SETTINGS = 8,
  KR_MENU_THEME = 9,
  KR_MENU_CREATE = 10, /* the sidebar's "+" button: new tab, window, ... */
  KR_MENU_PAGE = 11,
  KR_MENU_WORKSPACES = 12,
  KR_MENU_LIBRARY = 13,
  KR_MENU_SITE_DATA = 14,
  KR_MENU_SIDEBAR_LAYOUT = 15,
  KR_MENU_SECURITY = 16,
  KR_MENU_DOWNLOAD = 17,    /* target: KrDownloadId */
  KR_MENU_LIBRARY_ITEM = 18 /* target: KrLibraryItemId */
} KrMenuKind;

/* Anchors tell the host where native elements are, so its own popups
 * (permission prompts, extension popups, Page Info...) open from them. */
typedef enum KrAnchorKind {
  KR_ANCHOR_ELEMENT = 0, /* id: KrElement */
  KR_ANCHOR_ACTION = 1,  /* id: KrActionId */
  KR_ANCHOR_TAB = 2      /* id: KrTabId */
} KrAnchorKind;

typedef enum KrElement {
  KR_ELEMENT_ADDRESS = 1,
  KR_ELEMENT_SECURITY_CHIP = 2,
  KR_ELEMENT_BACK = 3,
  KR_ELEMENT_FORWARD = 4,
  KR_ELEMENT_RELOAD = 5,
  KR_ELEMENT_NEW_TAB = 6,
  KR_ELEMENT_APP_MENU = 7,
  KR_ELEMENT_SIDEBAR = 8,
  KR_ELEMENT_TOOLBAR = 9,
  KR_ELEMENT_SIDEBAR_TOGGLE = 10,
  KR_ELEMENT_LIBRARY = 11,
  KR_ELEMENT_CREATE = 12,
  KR_ELEMENT_WORKSPACE = 13,
  KR_ELEMENT_COMPACT_MODE = 14
} KrElement;

/* Optional host-owned appearance settings. Unknown flag bits are ignored. */
enum KrSettingsSection {
  KR_SETTINGS_GENERAL = 0,
  KR_SETTINGS_APPEARANCE = 1,
  KR_SETTINGS_PRIVACY = 2,
  KR_SETTINGS_SEARCH = 3,
  KR_SETTINGS_DOWNLOADS = 4,
  KR_SETTINGS_LANGUAGES = 5,
  KR_SETTINGS_ACCESSIBILITY = 6,
  KR_SETTINGS_ABOUT = 7,
  KR_SETTINGS_WORKSPACES = 8,
  KR_SETTINGS_CURRENT = 9 /* show_settings only: restore retained section */
};
enum KrColorScheme {
  KR_COLOR_SYSTEM = 0,
  KR_COLOR_LIGHT = 1,
  KR_COLOR_DARK = 2
};
enum KrMenuMaterial {
  KR_MENU_MATERIAL_DEFAULT = 0,
  KR_MENU_MATERIAL_SOLID = 1,
  KR_MENU_MATERIAL_MICA = 2,
  KR_MENU_MATERIAL_ACRYLIC = 3,
  KR_MENU_MATERIAL_ACRYLIC_THIN = 4
};
enum KrGlassMode {
  KR_GLASS_OFF = 0,
  KR_GLASS_PERFORMANCE = 1,
  KR_GLASS_QUALITY = 2
};
enum KrAppearanceFlags {
  KR_APPEARANCE_HIDE_HOVER_IMAGES = 1u << 0,
  KR_APPEARANCE_HIDE_HOVER_MEMORY = 1u << 1,
  /* Zero preserves the existing always-floating address editor. */
  KR_APPEARANCE_ATTACHED_SEARCH_BAR = 1u << 2,
  KR_APPEARANCE_SEARCH_BAR_FLOATING_ON_TYPE = 1u << 3,
  KR_APPEARANCE_SEARCH_BAR_MENU_BACKDROP = 1u << 4
};
/* Chrome icon set. Records without icon_pack mean FLUENT. */
enum KrIconPack {
  KR_ICON_PACK_FLUENT = 0, /* Fluent System Icons */
  KR_ICON_PACK_LUCIDE = 1,
  KR_ICON_PACK_PHOSPHOR = 2,
  KR_ICON_PACK_TABLER = 3,
  KR_ICON_PACK_RADIX = 4
};
/* Native settings: fixed semantic capabilities, never arbitrary pref names.
 * A snapshot revision must increase. All strings are copied during the call.
 * controls/managed use KrSettingsControl bits; actions use KrSettingsAction
 * bits. Open-browser actions MUST bypass native-settings URL interception. */
enum KrSettingsControl {
  KR_SETTINGS_DEFAULT_BROWSER,
  KR_SETTINGS_STARTUP,
  KR_SETTINGS_PROFILE,
  KR_SETTINGS_DOWNLOAD_LOCATION,
  KR_SETTINGS_ASK_DOWNLOAD,
  KR_SETTINGS_VERSION,
  KR_SETTINGS_SEARCH_ENGINE
};
enum KrBrowserSetting { KR_SETTING_STARTUP, KR_SETTING_ASK_DOWNLOAD };
enum KrSettingsAction {
  KR_SETTINGS_ACTION_MAKE_DEFAULT,
  KR_SETTINGS_ACTION_CHOOSE_DOWNLOAD_FOLDER,
  KR_SETTINGS_ACTION_MANAGE_STARTUP,
  KR_SETTINGS_ACTION_IMPORT,
  KR_SETTINGS_ACTION_PERFORMANCE,
  KR_SETTINGS_ACTION_SYSTEM,
  KR_SETTINGS_ACTION_RESET,
  KR_SETTINGS_ACTION_MANAGE_PROFILE,
  KR_SETTINGS_ACTION_ADD_PROFILE,
  KR_SETTINGS_ACTION_PROFILE_PICKER,
  KR_SETTINGS_ACTION_FONTS,
  KR_SETTINGS_ACTION_CLEAR_DATA,
  KR_SETTINGS_ACTION_COOKIES,
  KR_SETTINGS_ACTION_SECURITY,
  KR_SETTINGS_ACTION_PRIVACY_SERVICES,
  KR_SETTINGS_ACTION_MANAGE_SEARCH,
  KR_SETTINGS_ACTION_MORE_DOWNLOADS,
  KR_SETTINGS_ACTION_LANGUAGES,
  KR_SETTINGS_ACTION_ACCESSIBILITY,
  KR_SETTINGS_ACTION_ABOUT_BROWSER,
  KR_SETTINGS_ACTION_CREDITS,
  KR_SETTINGS_ACTION_LICENSE,
  KR_SETTINGS_ACTION_BROWSER_SETTINGS
};
typedef struct KrSettingsSnapshot {
  uint32_t struct_size;
  uint64_t revision, controls, managed, actions;
  uint32_t startup; /* 0 new tab, 1 continue session, 2 specific pages */
  bool ask_download_location;
  KrStr profile_name, download_location, product, version, build;
} KrSettingsSnapshot;
typedef struct KrSettingsEdit {
  uint32_t struct_size;
  uint64_t request_id;
  uint32_t setting, value;
} KrSettingsEdit;
typedef struct KrSettingsOperationResult {
  uint32_t struct_size;
  uint64_t request_id;
  KrResult result;
} KrSettingsOperationResult;

typedef struct KrAppearancePrefs {
  size_t struct_size;
  uint32_t color_scheme;  /* KrColorScheme */
  uint32_t menu_material; /* KrMenuMaterial */
  uint32_t flags;         /* KrAppearanceFlags */
  /* Starts after the legacy record's tail padding (24 bytes on x64). */
  uint64_t icon_pack;  /* KrIconPack; optional tail */
  uint64_t glass_mode; /* KrGlassMode; optional; absent means Off. */
} KrAppearancePrefs;

/* Live glass: BGRA8 premultiplied NT shared D3D11 texture, no CPU pixels.
 * Handles are borrowed during registration and duplicated by the module.
 * Producer signals ready; consumer signals release after its final GPU read.
 * Every accepted frame, including a superseded frame, signals release exactly
 * once. Resource ids cannot be reused until dropped. Source excludes all UI
 * islands. Bounds are host client DIPs and map the whole texture. */
enum KrGlassResourceKind { KR_GLASS_D3D11_BGRA8 = 1 };
typedef struct KrGlassBackdropResource {
  size_t struct_size;
  uint64_t id;
  uint32_t kind, width, height;
  uintptr_t texture, ready_fence, release_fence;
  uint64_t adapter_luid;
  KrRect bounds;
} KrGlassBackdropResource;
typedef struct KrGlassBackdropFrame {
  size_t struct_size;
  uint64_t resource_id, revision, ready_value, release_value;
} KrGlassBackdropFrame;
/* Empty regions stops production. A new geometry revision invalidates earlier
 * frames. Copy regions during the callback. Frames arrive on the UI thread;
 * registration/submission never wait for GPU work. */
typedef struct KrGlassBackdropRequest {
  size_t struct_size;
  uint64_t revision;
  const KrRect* regions;
  size_t region_count;
  float scale, sampling_margin;
  uint64_t adapter_luid;
} KrGlassBackdropRequest;

/* Integration extensions. All pointers are borrowed for the callback only. */
enum KrAddressFlags {
  KR_ADDRESS_OPEN = 1u << 0,
  KR_ADDRESS_FOCUSED = 1u << 1,
  KR_ADDRESS_NEW_TAB = 1u << 2
};
typedef struct KrAddressState {
  size_t struct_size;
  uint32_t flags;
  uint32_t request_id; /* zero when no suggestions are pending */
  KrStr text;
  /* Optional tail. Offsets count UTF-16 code units. A model update echoes
   * edit_revision and request_id; native text/selection/composition advances
   * the revision. All strings are borrowed during the callback only. */
  uint32_t selection_start;
  uint32_t selection_end;
  uint64_t edit_revision;
  bool composing;
} KrAddressState;

typedef struct KrAddressEdit {
  size_t struct_size;
  uint32_t request_id;
  uint64_t edit_revision;
  KrStr text;
  uint32_t selection_start;
  uint32_t selection_end;
} KrAddressEdit;

/* A semantic focus target uses the same identities as published anchors.
 * Focus does not activate a tab or invoke an action. */
typedef struct KrNativeFocus {
  size_t struct_size;
  uint32_t kind; /* KrAnchorKind */
  uint64_t id;
} KrNativeFocus;

enum KrGradientKind {
  KR_GRADIENT_LINEAR = 0,
  KR_GRADIENT_RADIAL = 1,
  KR_GRADIENT_DIAGONAL = 2 /* client top-left to bottom-right */
};
typedef struct KrGradientLayer {
  size_t struct_size;
  uint32_t kind;
  KrColor color;
  float angle;    /* degrees, clockwise from up */
  float center_x; /* radial center: client width fraction; y is zero */
  float start;    /* normalized opaque stop */
  float end;      /* normalized transparent stop */
} KrGradientLayer;
/* Layers compose in array order with source-over, in full client coordinates.
 * Linear endpoints use the line through the client center at angle, extended
 * by half the projection of the client rectangle along that line. Radial
 * radius reaches the farther bottom corner. Grain tiles in DIPs, source-over
 * after the layers, multiplied by texture. Do not retain grain.pixels. */
typedef struct KrWindowBackdrop {
  size_t struct_size;
  KrColor base;
  size_t layer_count;
  const KrGradientLayer* layers;
  size_t layer_size;
  float texture;
  KrImage grain; /* premultiplied BGRA; id zero; empty when texture is zero */
  /* Appended. Outline of the focused page in a split, resolved from the
   * workspace theme; transparent draws none. */
  KrColor pane_outline;
  /* Appended theme-resolved Glance controls and shadow ARGB colors. */
  KrColor glance_button;
  KrColor glance_button_hover;
  KrColor glance_button_icon;
  KrColor glance_shadow;
  KrColor glance_button_shadow;
  /* Appended KrWindowBackdropFlags. TRANSPARENT: paint no chrome backdrop
   * (base, layers and grain are fallbacks only); keep the frame unpainted
   * over the platform's system backdrop (Windows: DWM acrylic) and keep
   * the page opaque. Absent means zero. */
  uint32_t flags;
} KrWindowBackdrop;
enum KrWindowBackdropFlags { KR_WINDOW_BACKDROP_TRANSPARENT = 1u << 0 };
typedef struct KrShellMetrics {
  size_t struct_size;
  float minimum_client_width; /* DIPs; host adds its native frame insets */
  float minimum_client_height;
} KrShellMetrics;
enum KrWorkspaceEditKind {
  KR_WORKSPACE_UPSERT = 0, /* space.id == 0 creates; host allocates the id */
  KR_WORKSPACE_REMOVE = 1
};

/* Host side. Filled by the host, read by the module. */
typedef uint64_t KrTaskId;
typedef uint64_t KrPermissionId;
typedef enum KrTaskCategory {
  KR_TASK_TAB = 0,
  KR_TASK_EXTENSION = 1,
  KR_TASK_BROWSER = 2
} KrTaskCategory;
typedef struct KrBrowserTask {
  size_t struct_size;
  KrTaskId id;
  uint32_t process_id;
  uint32_t category;
  KrStr title;
  KrStr profile;
  KrImageId icon;
  uint64_t memory_bytes;
  uint64_t network_bytes_per_second;
  double cpu_percent; /* -1: unavailable; may exceed 100 on multiple cores */
  bool can_end;
} KrBrowserTask;
typedef enum KrPermissionKind {
  KR_PERMISSION_OTHER = 0,
  KR_PERMISSION_LOCATION = 1,
  KR_PERMISSION_CAMERA = 2,
  KR_PERMISSION_MICROPHONE = 3,
  KR_PERMISSION_NOTIFICATIONS = 4,
  KR_PERMISSION_CLIPBOARD = 5,
  KR_PERMISSION_POPUPS = 6,
  KR_PERMISSION_SOUND = 7,
  KR_PERMISSION_AUTOMATIC_DOWNLOADS = 8,
  KR_PERMISSION_USB = 9,
  KR_PERMISSION_BLUETOOTH = 10,
  KR_PERMISSION_SERIAL = 11
} KrPermissionKind;
typedef enum KrPermissionSetting {
  KR_PERMISSION_DEFAULT = 0,
  KR_PERMISSION_ALLOW = 1,
  KR_PERMISSION_BLOCK = 2,
  KR_PERMISSION_ASK = 3,
  KR_PERMISSION_SESSION = 4
} KrPermissionSetting;
typedef enum KrPermissionDecision {
  KR_PERMISSION_ALLOW_ONCE = 0,
  KR_PERMISSION_ALLOW_ALWAYS = 1,
  KR_PERMISSION_DENY = 2,
  KR_PERMISSION_DISMISS = 3
} KrPermissionDecision;
typedef struct KrSitePermission {
  size_t struct_size;
  KrPermissionId id;
  KrStr origin; /* canonical security origin supplied by the browser */
  KrStr label;
  KrStr description;
  uint32_t kind;
  uint32_t setting;
  uint32_t allowed_settings; /* bit (1 << KrPermissionSetting) */
  bool managed;
} KrSitePermission;
typedef struct KrPermissionRequest {
  size_t struct_size;
  uint32_t kind;
  KrStr label;
} KrPermissionRequest;
typedef struct KrPermissionPrompt {
  size_t struct_size;
  uint64_t request_id;
  KrStr origin;
  KrStr embedding_origin;
  KrStr message;
  const KrPermissionRequest* requests;
  size_t request_count;
  size_t request_size;
  KrRect anchor; /* client DIPs; use a published security/address anchor */
  bool allow_once;
  bool allow_always;
} KrPermissionPrompt;

typedef uint64_t KrPrivacyFeatureId;
typedef enum KrPrivacyFeatureGroup {
  KR_PRIVACY_GROUP_REMOTE = 0,
  KR_PRIVACY_GROUP_LOCAL_NETWORK = 1,
  KR_PRIVACY_GROUP_ON_DEVICE = 2
} KrPrivacyFeatureGroup;
typedef enum KrPrivacyRisk {
  KR_PRIVACY_RISK_NONE = 0,
  KR_PRIVACY_RISK_LOW = 1,
  KR_PRIVACY_RISK_MODERATE = 2,
  KR_PRIVACY_RISK_HIGH = 3
} KrPrivacyRisk;
enum KrPrivacyFeatureFlags {
  KR_PRIVACY_FEATURE_ENABLED = 1u << 0,
  KR_PRIVACY_FEATURE_NEEDS_RESTART = 1u << 1,
  KR_PRIVACY_FEATURE_RESTART_PENDING = 1u << 2,
  KR_PRIVACY_FEATURE_MANAGED = 1u << 3,
  KR_PRIVACY_FEATURE_ONBOARDING = 1u << 4
};
/* Owned by the host for the duration of the call; the module copies strings.
 * Ids are stable across releases. Saved choices change only on host upsert. */
typedef struct KrPrivacyFeature {
  size_t struct_size;
  KrPrivacyFeatureId id;
  uint32_t group;
  uint32_t risk;
  uint32_t flags;
  uint32_t order;
  KrStr title;
  KrStr summary;
  KrStr data_shared;
  KrStr risk_detail;
  KrStr when_off;
  KrStr manage_url;
} KrPrivacyFeature;

/* Downloads, sidebar media sessions and the library panel. Records are copied
 * during each call. Downloads keep host order: a new id appends, so the newest
 * download is last. Publish the downloads that the stack and the library show,
 * usually this session's and recent history. */
typedef uint64_t KrDownloadId;
typedef uint64_t KrMediaId;
typedef uint64_t KrLibraryItemId; /* unique across sections */
typedef enum KrDownloadState {
  KR_DOWNLOAD_IN_PROGRESS = 0,
  KR_DOWNLOAD_PAUSED = 1, /* stopped with resumable partial data */
  KR_DOWNLOAD_COMPLETE = 2,
  KR_DOWNLOAD_CANCELLED = 3,
  KR_DOWNLOAD_FAILED = 4
} KrDownloadState;
enum KrDownloadFlags {
  KR_DOWNLOAD_PROGRESS_KNOWN = 1u << 0,
  KR_DOWNLOAD_FILE_EXISTS = 1u << 1, /* the finished file can be shown */
  KR_DOWNLOAD_OPEN_WHEN_DONE = 1u << 2,
  KR_DOWNLOAD_BLOCKED = 1u << 3, /* blocked by reputation or policy */
  KR_DOWNLOAD_STARTED = 1u << 4  /* one-shot; not restored inventory */
};
typedef struct KrDownload {
  size_t struct_size;
  KrDownloadId id;
  uint32_t state; /* KrDownloadState */
  uint32_t flags; /* KrDownloadFlags */
  float progress; /* 0..1 with PROGRESS_KNOWN */
  KrStr file_name;
  KrStr status;   /* localized: transfer and speed, or size, site and date */
  KrImageId icon; /* file icon, or a finished picture's preview; 0: none */
} KrDownload;
typedef enum KrDownloadCommand {
  /* Finished: open. In progress: toggle open-when-done. Failed: retry when
   * possible, otherwise open the source page. Blocked: ask to unblock. */
  KR_DOWNLOAD_OPEN = 0,
  KR_DOWNLOAD_CANCEL = 1, /* also removes partial data */
  KR_DOWNLOAD_SHOW_IN_FOLDER = 2
} KrDownloadCommand;

enum KrMediaFlags {
  KR_MEDIA_PLAYING = 1u << 0,
  KR_MEDIA_MUTED = 1u << 1, /* the tab's audio is muted */
  KR_MEDIA_CAN_PLAY_PAUSE = 1u << 2,
  KR_MEDIA_CAN_PREVIOUS = 1u << 3,
  KR_MEDIA_CAN_NEXT = 1u << 4,
  KR_MEDIA_CAN_SEEK = 1u << 5,
  KR_MEDIA_CAN_PICTURE_IN_PICTURE = 1u << 6,
  KR_MEDIA_PICTURE_IN_PICTURE = 1u << 7, /* or page fullscreen: no card */
  KR_MEDIA_SHARING = 1u << 8, /* camera, microphone or screen capture */
  KR_MEDIA_MICROPHONE_MUTED = 1u << 9,
  KR_MEDIA_CAMERA_MUTED = 1u << 10,
  /* The capture can be muted from the card; without these the device
   * indicators show but cannot be clicked. */
  KR_MEDIA_CAN_TOGGLE_MICROPHONE = 1u << 11,
  KR_MEDIA_CAN_TOGGLE_CAMERA = 1u << 12
};
/* One active media session or capture in a tab. position is in seconds at the
 * time of this call and advances at playback_rate while PLAYING. A duration of
 * at least 900000 seconds (a live stream) hides the position row. */
typedef struct KrMediaSession {
  size_t struct_size;
  KrMediaId id;
  KrTabId tab;
  uint32_t flags; /* KrMediaFlags */
  KrStr title;
  KrStr artist;
  KrImageId icon; /* 0 uses the tab's favicon */
  double position;
  double duration;
  double playback_rate;
} KrMediaSession;
typedef enum KrMediaCommand {
  KR_MEDIA_PLAY_PAUSE = 0,
  KR_MEDIA_PREVIOUS = 1,
  KR_MEDIA_NEXT = 2,
  KR_MEDIA_TOGGLE_MUTE = 3,
  KR_MEDIA_TOGGLE_PICTURE_IN_PICTURE = 4,
  KR_MEDIA_CLOSE = 5, /* pause, or stop sharing; the host then removes it */
  KR_MEDIA_FOCUS = 6, /* show the tab that plays it */
  KR_MEDIA_TOGGLE_MICROPHONE = 7,
  KR_MEDIA_TOGGLE_CAMERA = 8
} KrMediaCommand;

typedef enum KrLibrarySection {
  KR_LIBRARY_MEDIA = 0,
  KR_LIBRARY_DOWNLOADS = 1, /* drawn from the published downloads */
  KR_LIBRARY_BOOSTS = 2,
  KR_LIBRARY_SPACES = 3, /* drawn from the published spaces and tabs */
  KR_LIBRARY_HISTORY = 4,
  KR_LIBRARY_ROUTING = 5
} KrLibrarySection;
enum KrLibrarySectionFlags {
  KR_LIBRARY_SECTION_HIDDEN = 1u << 0,  /* no tab, e.g. spaces in private */
  KR_LIBRARY_SECTION_ENABLED = 1u << 1, /* media: the user opted in */
  KR_LIBRARY_SECTION_LOADING = 1u << 2
};
typedef struct KrLibrarySectionState {
  size_t struct_size;
  uint32_t section; /* KrLibrarySection */
  uint32_t flags;   /* KrLibrarySectionFlags */
} KrLibrarySectionState;
enum KrLibraryItemFlags {
  KR_LIBRARY_ITEM_VIDEO = 1u << 0,  /* media tile gets a play badge */
  KR_LIBRARY_ITEM_FILE = 1u << 1,   /* image is a file icon, not a picture */
  KR_LIBRARY_ITEM_ENABLED = 1u << 2 /* boosts: switched on */
};
/* Media files, boosts and history rows. Consecutive items sharing a nonempty
 * group (a date or site) are drawn under one heading. */
typedef struct KrLibraryItem {
  size_t struct_size;
  KrLibraryItemId id;
  uint32_t section; /* MEDIA, BOOSTS, HISTORY or ROUTING */
  uint32_t flags;   /* KrLibraryItemFlags */
  KrStr title;
  KrStr subtitle;
  KrStr group;
  KrImageId image; /* picture, file icon or favicon; 0: none */
} KrLibraryItem;
typedef enum KrLibraryCommand {
  KR_LIBRARY_OPEN = 0,   /* open or reopen the item */
  KR_LIBRARY_ENABLE = 1, /* media opt-in; item is 0 */
  KR_LIBRARY_TOGGLE = 2, /* boosts: switch on or off */
  KR_LIBRARY_REMOVE = 3, /* history: forget the page */
  KR_LIBRARY_CREATE = 4  /* routing/boosts: create a record; item is 0 */
} KrLibraryCommand;

/* Native first-run onboarding. The module owns the moments and their motion;
 * the host owns every saved choice and confirms it through the usual updates
 * (set_layout_prefs, set_workspace_theme, privacy upserts). Import sources are
 * published while onboarding is visible. Shared search/default-browser services
 * may also publish while settings is visible. Records are copied. */
typedef uint64_t KrImportSourceId;
typedef uint64_t KrSearchEngineId;
typedef enum KrOnboardingStep {
  KR_ONBOARDING_STEP_IGNITION = 0,
  KR_ONBOARDING_STEP_SHAPE = 1,
  KR_ONBOARDING_STEP_COLOR = 2,
  KR_ONBOARDING_STEP_IMPORT = 3,
  KR_ONBOARDING_STEP_SEARCH = 4,
  KR_ONBOARDING_STEP_PRIVACY = 5,
  KR_ONBOARDING_STEP_DEFAULT_BROWSER = 6,
  KR_ONBOARDING_STEP_READY = 7
} KrOnboardingStep;
enum KrOnboardingFlags {
  KR_ONBOARDING_FIRST_RUN = 1u << 0 /* first window of a new profile */
};
typedef struct KrOnboardingRequest {
  size_t struct_size;
  uint32_t flags; /* KrOnboardingFlags */
  uint32_t step;  /* KrOnboardingStep to start at; unavailable steps skip */
} KrOnboardingRequest;
enum KrImportItems {
  KR_IMPORT_BOOKMARKS = 1u << 0,
  KR_IMPORT_HISTORY = 1u << 1,
  KR_IMPORT_PASSWORDS = 1u << 2,
  KR_IMPORT_SEARCH_ENGINES = 1u << 3,
  KR_IMPORT_AUTOFILL = 1u << 4,
  KR_IMPORT_EXTENSIONS = 1u << 5
};
typedef struct KrImportSource {
  size_t struct_size;
  KrImportSourceId id;
  uint32_t items; /* KrImportItems this source can provide */
  uint32_t order;
  KrStr name;
  KrImageId icon; /* 0: none */
} KrImportSource;
typedef enum KrImportState {
  KR_IMPORT_IDLE = 0,
  KR_IMPORT_RUNNING = 1,
  KR_IMPORT_SUCCEEDED = 2,
  KR_IMPORT_FAILED = 3
} KrImportState;
typedef struct KrImportProgress {
  size_t struct_size;
  KrImportSourceId source;
  uint32_t state; /* KrImportState */
  float fraction; /* 0..1; negative when unknown */
} KrImportProgress;
enum KrDefaultBrowserFlags {
  KR_DEFAULT_BROWSER_IS_DEFAULT = 1u << 0,
  KR_DEFAULT_BROWSER_CAN_SET = 1u << 1,
  KR_DEFAULT_BROWSER_MANAGED = 1u << 2, /* policy decides */
  KR_DEFAULT_BROWSER_PENDING = 1u << 3  /* Windows is asking the user */
};
typedef struct KrDefaultBrowserState {
  size_t struct_size;
  uint32_t flags; /* KrDefaultBrowserFlags */
} KrDefaultBrowserState;
enum KrSearchEngineFlags {
  KR_SEARCH_ENGINE_DEFAULT = 1u << 0,
  KR_SEARCH_ENGINE_MANAGED = 1u << 1 /* policy sets the default */
};
typedef struct KrSearchEngine {
  size_t struct_size;
  KrSearchEngineId id;
  uint32_t flags; /* KrSearchEngineFlags */
  uint32_t order;
  KrStr name;
  KrStr keyword;
  KrImageId icon; /* 0: none */
} KrSearchEngine;
typedef enum KrOnboardingOutcome {
  KR_ONBOARDING_COMPLETED = 0,
  KR_ONBOARDING_SKIPPED = 1,
  KR_ONBOARDING_CLOSED = 2 /* the window closed while it was showing */
} KrOnboardingOutcome;
typedef struct KrOnboardingResult {
  size_t struct_size;
  uint32_t outcome;   /* KrOnboardingOutcome */
  uint32_t last_step; /* KrOnboardingStep */
} KrOnboardingResult;

enum KrAddressKey {
  KR_ADDRESS_KEY_UP,
  KR_ADDRESS_KEY_DOWN,
  KR_ADDRESS_KEY_PAGE_UP,
  KR_ADDRESS_KEY_PAGE_DOWN,
  KR_ADDRESS_KEY_TAB,
  KR_ADDRESS_KEY_ESCAPE,
  KR_ADDRESS_KEY_DELETE_MATCH,
  KR_ADDRESS_KEY_CONTROL
};
enum KrAddressKeyModifiers {
  KR_ADDRESS_KEY_SHIFT = 1u << 0,
  KR_ADDRESS_KEY_CTRL = 1u << 1
};
/* Optional multi-page protocol. Capability bits describe implemented behavior,
 * not user preferences. Hosts must also check every required module slot.
 * Neither feature is exposed until both sides advertise COMPLETE support. */
typedef uint64_t KrSplitGroupId;
typedef enum KrMultiPageCapability {
  KR_MULTI_PAGE_PROTOCOL = 1u << 0,
  KR_MULTI_PAGE_LAYOUT = 1u << 1,
  KR_MULTI_PAGE_REDUCERS = 1u << 2,
  KR_MULTI_PAGE_SPLITS = 1u << 3,
  KR_MULTI_PAGE_GLANCE = 1u << 4,
  KR_MULTI_PAGE_COMPLETE = (1u << 5) - 1,
  /* Additional snap topologies and show_split_chooser_for_layout. COMPLETE
   * deliberately keeps its original meaning for older ABI-v2 peers. */
  KR_MULTI_PAGE_SNAP_LAYOUTS = 1u << 5
} KrMultiPageCapability;
typedef enum KrSplitLayout {
  KR_SPLIT_COLUMNS = 0,
  KR_SPLIT_ROWS = 1,
  KR_SPLIT_GRID = 2,
  KR_SPLIT_WIDE_LEFT = 3,
  KR_SPLIT_WIDE_RIGHT = 4,
  KR_SPLIT_WIDE_CENTER = 5,
  KR_SPLIT_MAIN_LEFT = 6,
  KR_SPLIT_MAIN_RIGHT = 7,
  KR_SPLIT_MAIN_TOP = 8,
  KR_SPLIT_MAIN_BOTTOM = 9
} KrSplitLayout;
typedef struct KrSplitMember {
  size_t struct_size;
  KrTabId tab;
} KrSplitMember;
typedef struct KrSplitWeight {
  size_t struct_size;
  float value;
} KrSplitWeight;
typedef struct KrSplitGroup {
  size_t struct_size;
  KrSplitGroupId id;
  KrSpaceId space;
  uint64_t revision;
  KrSplitLayout layout;
  KrTabId active_tab;
  KrSplitMember* members;
  size_t member_count;
  size_t member_size;
  KrSplitWeight* weights; /* three normalized binary-tree divider weights */
  size_t weight_count;
  size_t weight_size;
} KrSplitGroup;
typedef enum KrSplitEditKind {
  KR_SPLIT_ACTIVATE = 0,
  KR_SPLIT_SET_LAYOUT = 1,
  KR_SPLIT_INSERT = 2,
  KR_SPLIT_DETACH = 3,
  KR_SPLIT_MOVE = 4,
  KR_SPLIT_RESIZE = 5,
  KR_SPLIT_UNSPLIT = 6,
  KR_SPLIT_SET_WORKSPACE = 7
} KrSplitEditKind;
typedef struct KrSplitEdit {
  size_t struct_size;
  KrSplitEditKind kind;
  uint64_t revision;
  KrTabId tab;
  KrSpaceId space;
  size_t index;
  KrSplitLayout layout;
  float weight;
} KrSplitEdit;
typedef struct KrSplitCreateRequest {
  size_t struct_size;
  KrSplitGroupId id;
  KrSpaceId space;
  const KrSplitMember* members;
  size_t member_count;
  size_t member_size;
  KrTabId active_tab;
  KrSplitLayout layout;
} KrSplitCreateRequest;
typedef struct KrSplitCopyMember {
  size_t struct_size;
  KrTabId source;
  bool copy;
} KrSplitCopyMember;
typedef struct KrSplitCopyRequest {
  size_t struct_size;
  const KrTab* tabs;
  size_t count;
  size_t tab_size;
  KrSpaceId space;
} KrSplitCopyRequest;
typedef struct KrSplitCopyPlan {
  size_t struct_size;
  KrSplitCopyMember* members;
  size_t count;
  size_t member_size;
  bool pinned;
} KrSplitCopyPlan;
typedef struct KrPaneBounds {
  size_t struct_size;
  KrTabId tab;
  KrRect bounds;
  KrRect controls; /* optional browser-owned pane menu, within bounds */
} KrPaneBounds;
typedef struct KrDividerBounds {
  size_t struct_size;
  KrRect bounds;
  KrRect available;
  uint32_t index;
  bool horizontal; /* false: column divider, true: row divider */
} KrDividerBounds;
typedef struct KrSplitGeometry {
  size_t struct_size;
  KrPaneBounds* panes;
  size_t pane_count;
  size_t pane_size;
  KrDividerBounds* dividers;
  size_t divider_count;
  size_t divider_size;
} KrSplitGeometry;
typedef struct KrSplitLayoutRequest {
  size_t struct_size;
  const KrSplitGroup* group;
  KrRect page; /* synchronous client DIPs, independent of XAML rendering */
  float divider_width;
  bool right_to_left;
} KrSplitLayoutRequest;
typedef enum KrGlancePhase {
  KR_GLANCE_CLOSED = 0,
  KR_GLANCE_OPEN = 1,
  KR_GLANCE_CLOSING = 2
} KrGlancePhase;
typedef struct KrGlanceState {
  size_t struct_size;
  KrTabId parent;
  KrTabId child;
  uint64_t generation;
  KrGlancePhase phase;
  KrTabId departure;
} KrGlanceState;
typedef enum KrGlanceEditKind {
  KR_GLANCE_CREATE = 0,
  KR_GLANCE_REUSE = 1,
  KR_GLANCE_REQUEST_CLOSE = 2,
  KR_GLANCE_CANCEL_CLOSE = 3,
  KR_GLANCE_DID_CLOSE = 4,
  KR_GLANCE_EXPAND = 5,
  KR_GLANCE_SPLIT = 6
} KrGlanceEditKind;
typedef struct KrGlanceEdit {
  size_t struct_size;
  KrGlanceEditKind kind;
  KrTabId parent;
  KrTabId child;
  uint64_t generation;
  KrTabId departure;
  size_t parent_members;
} KrGlanceEdit;
typedef struct KrMultiPageMetrics {
  size_t struct_size;
  float divider_width;
  float glance_inset;
  float glance_toolbar_height;
  uint32_t motion_ms;
  float outline_width; /* appended; KrWindowBackdrop.pane_outline's stroke */
  /* Appended Zen-style Glance metrics; DIPs, scales and finite motion. */
  float glance_button_size;
  float glance_button_gap;
  float glance_controls_padding;
  float glance_icon_size;
  uint32_t glance_motion_ms;
  uint32_t glance_controls_ms;
  float glance_controls_offset;
  float glance_parent_scale;
  float glance_parent_opacity;
  float glance_shadow_blur;
  float glance_shadow_offset;
  float glance_button_shadow_blur;
  float glance_hover_scale;
  float glance_press_scale;
  float glance_disabled_opacity;
  bool glance_controls_on_left;
  uint32_t glance_button_motion_ms;
  float glance_close_speed;
  /* Appended compositor-only genie-lamp keyframes. */
  float glance_genie_source_scale_x;
  float glance_genie_source_scale_y;
  float glance_genie_neck_scale_x;
  float glance_genie_neck_scale_y;
  float glance_genie_bend_degrees;
  float glance_genie_travel;
  float glance_genie_first_fraction;
  float glance_genie_second_fraction;
  float glance_genie_settle_scale;
  float glance_genie_source_opacity;
} KrMultiPageMetrics;
typedef struct KrGlanceGeometry {
  size_t struct_size;
  KrRect frame;
  KrRect toolbar;
  KrRect page;
} KrGlanceGeometry;
typedef enum KrMultiPageCommandKind {
  KR_MULTI_PAGE_NEW_SPLIT = 0,
  KR_MULTI_PAGE_EDIT_SPLIT = 1,
  KR_MULTI_PAGE_CLOSE_GLANCE = 2,
  KR_MULTI_PAGE_EXPAND_GLANCE = 3,
  KR_MULTI_PAGE_SPLIT_GLANCE = 4,
  KR_MULTI_PAGE_OPEN_GLANCE = 5,
  KR_MULTI_PAGE_CHOOSE_SPLIT_LAYOUT = 6,
  KR_MULTI_PAGE_SET_SPLIT_LAYOUT = 7
} KrMultiPageCommandKind;
typedef struct KrMultiPageCommand {
  size_t struct_size;
  KrMultiPageCommandKind kind;
  KrTabId tab;
  KrSplitGroupId group;
  uint64_t revision;
  const KrSplitEdit* edit;
  const KrSplitMember* members;
  size_t member_count;
  size_t member_size;
  KrSplitLayout layout;
  KrStr
      url; /* context-menu HTTP(S) links; never infer intent from a download */
} KrMultiPageCommand;

/* Persistent user styling. Strings are borrowed only during the call. Colors
 * with alpha 0 inherit the site's appearance. compiled_css is UI-generated. */
typedef struct KrBoost {
  size_t struct_size;
  uint64_t id, revision;
  KrTabId tab;
  uint32_t serial;
  KrStr name, pattern, css, compiled_css, zapped, focused;
  KrColor background, foreground, accent;
  uint32_t font, size, letter_case, appearance, match;
  bool enabled;
  /* Optional tail: normalized color controls; presence follows struct_size. */
  float contrast, brightness, saturation;
  float palette_hue, palette_distance, secondary_rotation;
  bool invert_colors, color_enabled, auto_theme, palette_set;
} KrBoost;
typedef enum KrBoostEdit {
  KR_BOOST_PREVIEW = 0,
  KR_BOOST_COMMIT = 1,
  KR_BOOST_CANCEL = 2,
  KR_BOOST_REMOVE = 3,
  KR_BOOST_ZAP = 4,
  KR_BOOST_FOCUS = 5,
  KR_BOOST_SHARE = 6
} KrBoostEdit;

/* An editor draft. Workspace 0 keeps the most recently used workspace. */
typedef struct KrRoutingRule {
  size_t struct_size;
  uint64_t id, revision;
  KrStr pattern;
  KrSpaceId workspace;
  uint32_t match; /* 0 domain, 1 contains, 2 exact URL, 3 regular expression */
  bool enabled, new_tabs_only;
} KrRoutingRule;

typedef struct KrBrowser {
  size_t struct_size;
  void* self;

  void (*activate_tab)(void* self, KrTabId tab);
  void (*close_tab)(void* self, KrTabId tab);
  void (*new_tab)(void* self);
  void (*move_tab)(void* self, KrTabId tab, KrSpaceId space, size_t to_index);
  void (*set_tab_flags)(void* self,
                        KrTabId tab,
                        uint32_t mask,
                        uint32_t values);
  void (*switch_space)(void* self, KrSpaceId space);

  void (*navigate)(void* self, KrStr text, uint32_t disposition);
  void (*nav_command)(void* self, uint32_t command);
  void (*query_suggestions)(void* self, uint32_t request_id, KrStr text);
  void (*accept_suggestion)(void* self,
                            uint32_t request_id,
                            size_t index,
                            uint32_t disposition);

  void (*invoke_action)(void* self, KrActionId action, KrRect anchor);
  void (*request_menu)(void* self,
                       uint32_t request_id,
                       uint32_t kind,
                       uint64_t target,
                       KrRect anchor);
  void (*menu_command)(void* self, uint32_t request_id, int32_t command_id);
  void (*menu_closed)(void* self, uint32_t request_id);

  void (*window_command)(void* self, uint32_t command);
  void (*set_content_bounds)(void* self, KrRect bounds, float corner_radius);
  void (*set_anchor)(void* self,
                     uint32_t kind,
                     uint64_t id,
                     KrRect bounds,
                     bool visible);
  /* Retained for platform compatibility. Windows owns hit testing internally
   * and no longer calls this slot. */
  void (*set_drag_regions)(void* self, const KrRect* regions, size_t count);
  void (*set_layout_prefs)(void* self, const KrLayoutPrefs* prefs);
  void (*focus_page)(void* self);
  void (*log)(void* self, uint32_t level, KrStr message);
  /* Optional: paint the host-owned chrome backdrop in client coordinates,
   * from the top-left color to the bottom-right color. */
  void (*set_window_background)(void* self, KrColor start, KrColor end);
  /* Optional, demand-driven hover data. Reply through set_tab_preview using
   * this request id. Cancellation ends observation/capture for that request. */
  void (*request_tab_preview)(void* self, uint32_t request_id, KrTabId tab);
  void (*cancel_tab_preview)(void* self, uint32_t request_id);
  /* Preview while editing; commit=true on dismissal. Copy during the call.
   * Push confirmations/corrections back through set_workspace_theme. */
  void (*set_workspace_theme)(void* self,
                              const KrWorkspaceTheme* theme,
                              bool commit);
  /* Optional full backdrop, following the reference's layered gradients.
   * Older hosts retain the existing two-color set_window_background fallback.
   */
  void (*set_window_theme)(void* self,
                           const KrWorkspaceTheme* theme,
                           bool dark);
  /* Optional user intent. Copy the snapshot, save it and push confirmation
   * through KrShellUI.set_appearance_prefs and the existing set_theme. */
  void (*set_appearance_prefs)(void* self, const KrAppearancePrefs* prefs);
  /* Optional synchronous shortcut fallback while the module owns focus.
   * Windows: native_message points to a const MSG, borrowed for this call.
   * Return true only when consumed. Do not call pre_translate_message again;
   * defer browser mutations to the next task to avoid observer reentrancy. */
  bool (*unhandled_key)(void* self, const void* native_message);
  /* Address session notifications include open, text, focus and dismissal.
   * A request is cancelled before it is superseded or its editor closes. */
  void (*address_state_changed)(void* self, const KrAddressState* state);
  void (*cancel_suggestions)(void* self, uint32_t request_id);
  void (*accept_suggestion_snapshot)(void* self,
                                     uint32_t request_id,
                                     uint64_t generation,
                                     size_t index,
                                     uint32_t disposition);
  /* Copy the edit, persist off-thread and confirm through set_spaces. */
  void (*edit_workspace)(void* self, uint32_t kind, const KrSpace* space);
  void (*set_shell_metrics)(void* self, const KrShellMetrics* metrics);
  void (*set_window_backdrop)(void* self, const KrWindowBackdrop* backdrop);
  /* Optional browser-tool services. Publish incremental task/permission data
   * through KrShellUI. Stop metric collection when visibility becomes false.
   * Successful edits are confirmed by upsert/remove calls, not UI guesses. */
  void (*task_manager_visibility_changed)(void* self, bool visible);
  void (*activate_task)(void* self, KrTaskId task);
  KrResult (*end_tasks)(void* self, const KrTaskId* tasks, size_t count);
  void (*site_permissions_visibility_changed)(void* self,
                                              KrStr origin,
                                              bool visible);
  KrResult (*set_site_permission)(void* self,
                                  KrPermissionId id,
                                  uint32_t setting);
  KrResult (*reset_site_permissions)(void* self, KrStr origin);
  /* Exactly one response for a user decision/dismissal. Host cancellation via
   * dismiss_permission_prompt produces no response; stale ids are ignored. */
  void (*permission_prompt_response)(void* self,
                                     uint64_t request_id,
                                     uint32_t decision);
  /* Optional: user keyboard selection in the displayed suggestion snapshot.
   * May refer to a retained older generation. This never accepts/navigates. */
  void (*select_suggestion_snapshot)(void* self,
                                     uint32_t request_id,
                                     uint64_t generation,
                                     size_t index);
  /* Optional privacy catalog. Publish on demand and confirm edits by upsert.
   * Hiding the surface ends observation; future onboarding reuses this API. */
  void (*privacy_features_visibility_changed)(void* self, bool visible);
  KrResult (*set_privacy_feature)(void* self,
                                  KrPrivacyFeatureId id,
                                  bool enabled);
  void (*relaunch_browser)(void* self);
  /* Optional downloads, media and library intents. Results arrive through
   * the matching KrShellUI upserts. Context menus use request_menu with
   * KR_MENU_DOWNLOAD and KR_MENU_LIBRARY_ITEM. */
  void (*download_command)(void* self, KrDownloadId id, uint32_t command);
  void (*media_command)(void* self, KrMediaId id, uint32_t command);
  void (*media_seek)(void* self, KrMediaId id, double seconds);
  /* Publish a section's items, and observe changes, only while it is visible.
   * Hiding it ends observation; the module keeps the last items. */
  void (*library_visibility_changed)(void* self,
                                     uint32_t section,
                                     bool visible);
  void (*library_command)(void* self,
                          uint32_t section,
                          KrLibraryItemId item,
                          uint32_t command);
  /* Optional native onboarding. Observe import sources while visible. Shared
   * search/default-browser observation ends when both native surfaces hide. */
  void (*onboarding_visibility_changed)(void* self, bool visible);
  /* Progress arrives through set_import_progress; one import at a time. */
  KrResult (*start_import)(void* self, KrImportSourceId source, uint32_t items);
  /* Opens the Windows default-apps flow. Push the resulting state when the
   * window is reactivated; never poll. */
  KrResult (*request_default_browser)(void* self);
  KrResult (*set_default_search_engine)(void* self, KrSearchEngineId engine);
  /* Sent exactly once for each shown onboarding. The host records completion
   * and decides whether CLOSED counts as seen. */
  void (*onboarding_finished)(void* self, const KrOnboardingResult* result);
  /* Observe only while settings is visible. false precedes a section switch.
   * Publish capabilities/values before enabling newly supported controls. */
  void (*settings_visibility_changed)(void* self,
                                      uint32_t section,
                                      bool visible);
  KrResult (*edit_setting)(void* self, const KrSettingsEdit* edit);
  /* Semantic browser destinations/actions, executed in the owning profile.
   * Successful edits publish an authoritative snapshot BEFORE completion.
   * Every accepted request completes via settings_operation_result. */
  KrResult (*invoke_settings_action)(void* self,
                                     uint64_t request_id,
                                     uint32_t action);
  bool (*handle_address_key)(void* self, uint32_t key, uint32_t modifiers);
  /* Optional. Query only at setup/capability changes, never on idle ticks. */
  uint32_t (*multi_page_capabilities)(void* self);
  KrResult (*multi_page_command)(void* self, const KrMultiPageCommand* command);
  void(KR_UI_CALL* request_glass_backdrop)(
      void* self,
      const KrGlassBackdropRequest* request);
  /* Optional. Successful commands publish authoritative folder/tab updates. */
  KrResult(KR_UI_CALL* folder_command)(void* self,
                                       const KrFolderCommand* command);
  /* Optional persisted icon override. Empty value restores the site icon. */
  void (*set_tab_icon)(void* self, KrTabId tab, KrStr glyph);
  KrResult (*edit_boost)(void* self, uint32_t kind, const KrBoost* value);
  /* Optional event-driven, asynchronous file-removal refresh. */
  void (*refresh_downloads)(void* self);
  KrResult (*save_routing_rule)(void* self, const KrRoutingRule* value);
} KrBrowser;

/* Module side. Filled by kr_ui_create, read by the host. */
typedef struct KrShellUI {
  size_t struct_size;
  void* self;

  void (*destroy)(void* self);

  /* Reports changed content bounds synchronously, before returning. Native
   * rendering stays coalesced on the UI queue. set_content_bounds can therefore
   * run inside this call; the host must not assume notification is deferred. */
  void (*set_window_state)(void* self, const KrWindowState* state);
  void (*set_theme)(void* self, const KrTheme* theme);
  void (*set_layout_prefs)(void* self, const KrLayoutPrefs* prefs);

  void (*put_image)(void* self, const KrImage* image);
  void (*drop_image)(void* self, KrImageId image);

  void (*set_spaces)(void* self,
                     const KrSpace* spaces,
                     size_t count,
                     size_t element_size,
                     KrSpaceId active);
  void (*tab_inserted)(void* self, size_t index, const KrTab* tab);
  void (*tab_updated)(void* self, const KrTab* tab);
  void (*tab_moved)(void* self, KrTabId tab, size_t to_index);
  void (*tab_removed)(void* self, KrTabId tab);
  void (*set_active_tab)(void* self, KrTabId tab);

  void (*set_nav_state)(void* self, const KrNavState* state);
  void (*set_actions)(void* self,
                      const KrAction* actions,
                      size_t count,
                      size_t element_size);
  void (*set_suggestions)(void* self,
                          uint32_t request_id,
                          const KrSuggestion* items,
                          size_t count,
                          size_t element_size);
  /* Repeating a show call for an open request updates its rows in place. */
  void (*show_menu)(void* self,
                    uint32_t request_id,
                    const KrMenu* menus,
                    size_t menu_count,
                    size_t element_size);

  /* Ctrl+L / Ctrl+T: open the address field. */
  void (*focus_address)(void* self, bool select_all);

  /* Retained for compatibility. Windows preprocesses removed messages through
   * its per-thread hook; this slot returns false there to prevent duplicate
   * preprocessing by older host pumps. */
  bool (*pre_translate_message)(void* self, const void* native_message);

  /* Optional localization/customization. Copies both UTF-8 strings. An empty
   * text restores the default. Unknown keys return KR_ERR_INVALID_ARGUMENT.
   * Keys are documented in docs/ui-customization.md. */
  KrResult (*set_ui_text)(void* self, KrStr key, KrStr text);
  /* Optional. Native controls inherit this writing direction. Sidebar side
   * remains a separate layout preference. */
  void (*set_text_direction)(void* self, bool right_to_left);
  /* Optional: forward a host-owned surface's context request, in client DIPs.
   * The module asks the browser for the menu through request_menu as usual. */
  void (*request_context_menu)(void* self,
                               uint32_t kind,
                               uint64_t target,
                               KrRect anchor);
  /* Optional, same request/ownership rules as show_menu. */
  void (*show_menu_with_glyphs)(void* self,
                                uint32_t request_id,
                                const KrMenuWithGlyphs* menus,
                                size_t menu_count,
                                size_t element_size);
  /* Optional. Stale replies are ignored. Fields are copied during the call. */
  KrResult (*set_tab_preview)(void* self,
                              uint32_t request_id,
                              const KrTabPreview* preview);
  KrResult (*set_workspace_theme)(void* self, const KrWorkspaceTheme* theme);
  KrResult (*show_theme_editor)(void* self, KrSpaceId space);
  /* Optional rich panel. HEADER and CONTROLS rows refer to child menus.
   * Existing show_menu slots keep their native context-menu presentation. */
  void (*show_menu_panel)(void* self,
                          uint32_t request_id,
                          const KrMenuWithGlyphs* menus,
                          size_t menu_count,
                          size_t element_size);
  KrResult (*set_load_progress)(void* self, const KrLoadProgress* progress);
  KrResult (*set_appearance_prefs)(void* self, const KrAppearancePrefs* prefs);
  KrResult (*show_settings)(void* self, uint32_t section);
  /* Opens/replaces editor text and starts a fresh query, including empty text.
   * These calls never navigate. Dismissal notifies the host once. */
  KrResult (*set_address_text)(void* self, KrStr text, bool select_all);
  void (*dismiss_address)(void* self);
  /* Generation is nonzero and increases within one request. Old/stale replies
   * are ignored. Use only with accept_suggestion_snapshot on the host table. */
  KrResult (*set_suggestion_snapshot)(void* self,
                                      uint32_t request_id,
                                      uint64_t generation,
                                      const KrSuggestion* items,
                                      size_t count,
                                      size_t element_size);
  /* Cancels pending or visible presentation; menu_closed fires once. */
  void (*dismiss_menu)(void* self, uint32_t request_id);
  /* Zero creates a workspace; an existing id edits it. Delete is unavailable
   * for the first (default) workspace. Requires edit_workspace. */
  KrResult (*show_workspace_editor)(void* self, KrSpaceId space);
  /* Optional tiled panel capability. Same ownership as show_menu_panel;
   * additionally accepts GRID, SECTION and FOOTER rows and tile badges.
   * Check this slot before sending those rows to an older UI. */
  void (*show_menu_panel_with_tiles)(void* self,
                                     uint32_t request_id,
                                     const KrMenu* menus,
                                     size_t menu_count,
                                     size_t element_size);
  /* Optional native browser-tool surfaces. All payload strings are copied.
   * Repeated opens activate the existing window. Data updates are incremental.
   */
  KrResult (*show_task_manager)(void* self);
  KrResult (*upsert_task)(void* self, const KrBrowserTask* task);
  KrResult (*remove_task)(void* self, KrTaskId task);
  KrResult (*show_site_permissions)(void* self,
                                    KrStr origin); /* empty: all sites */
  KrResult (*upsert_site_permission)(void* self,
                                     const KrSitePermission* permission);
  KrResult (*remove_site_permission)(void* self, KrPermissionId permission);
  KrResult (*show_permission_prompt)(void* self,
                                     const KrPermissionPrompt* prompt);
  void (*dismiss_permission_prompt)(void* self, uint64_t request_id);
  /* Model-driven edit: no new query, focus change or state notification.
   * Stale requests/revisions and updates during composition are ignored.
   * Invalid UTF-8/selection bounds return KR_ERR_INVALID_ARGUMENT.
   * Text is copied during the call. */
  KrResult (*set_address_edit)(void* self, const KrAddressEdit* edit);
  /* Copies the target, reveals compact chrome and focuses after its render.
   * A later focus request supersedes an earlier one. */
  KrResult (*focus_native)(void* self, const KrNativeFocus* target);
  KrResult (*upsert_privacy_feature)(void* self,
                                     const KrPrivacyFeature* feature);
  KrResult (*remove_privacy_feature)(void* self, KrPrivacyFeatureId id);
  /* Optional downloads, media sessions and library. All records are copied.
   * Unknown ids on removal return KR_ERR_INVALID_ARGUMENT. */
  KrResult (*upsert_download)(void* self, const KrDownload* download);
  KrResult (*remove_download)(void* self, KrDownloadId id);
  KrResult (*upsert_media_session)(void* self, const KrMediaSession* session);
  KrResult (*remove_media_session)(void* self, KrMediaId id);
  KrResult (*set_library_section)(void* self,
                                  const KrLibrarySectionState* state);
  /* before == 0 appends; otherwise a new item is inserted before that one. */
  KrResult (*upsert_library_item)(void* self,
                                  const KrLibraryItem* item,
                                  KrLibraryItemId before);
  KrResult (*remove_library_item)(void* self, KrLibraryItemId id);
  /* Opens the library on a section, e.g. for "Show all downloads". */
  KrResult (*show_library)(void* self, uint32_t section);
  /* Optional native onboarding over this window's chrome. Returns
   * KR_ERR_UNSUPPORTED when the host table lacks onboarding_finished. */
  KrResult (*show_onboarding)(void* self, const KrOnboardingRequest* request);
  KrResult (*upsert_import_source)(void* self, const KrImportSource* source);
  KrResult (*remove_import_source)(void* self, KrImportSourceId id);
  KrResult (*set_import_progress)(void* self, const KrImportProgress* progress);
  KrResult (*set_default_browser_state)(void* self,
                                        const KrDefaultBrowserState* state);
  KrResult (*upsert_search_engine)(void* self, const KrSearchEngine* engine);
  KrResult (*remove_search_engine)(void* self, KrSearchEngineId id);
  KrResult (*set_settings_snapshot)(void* self,
                                    const KrSettingsSnapshot* snapshot);
  KrResult (*settings_operation_result)(
      void* self,
      const KrSettingsOperationResult* result);
  /* Optional transient native confirmation. Text is copied during the call. */
  KrResult (*show_notification)(void* self, KrStr text);
  KrResult (*set_suggestion_selection)(void* self,
                                       uint32_t request_id,
                                       uint64_t generation,
                                       size_t index);
  /* Optional append-only multi-page extension. Stateless layout/reducers are
   * usable without enabling native presentation. For output arrays, counts
   * are capacities on entry and actual counts on success. Initialize each
   * output element's struct_size. Unknown trailing bytes stay untouched.
   * Invalid inputs/capacities leave the entire output unchanged. A dissolved
   * reducer result has id=0 and member_count=0; no tabs are closed. */
  uint32_t (*multi_page_capabilities)(void* self);
  KrResult (*upsert_split_group)(void* self, const KrSplitGroup* group);
  KrResult (*remove_split_group)(void* self,
                                 KrSplitGroupId group,
                                 uint64_t revision);
  KrResult (*set_glance)(void* self, const KrGlanceState* glance);
  KrResult (*layout_split)(void* self,
                           const KrSplitLayoutRequest* request,
                           KrSplitGeometry* output);
  KrResult (*reduce_split)(void* self,
                           const KrSplitGroup* group,
                           const KrSplitEdit* edit,
                           KrSplitGroup* output);
  KrResult (*reduce_glance)(void* self,
                            const KrGlanceState* state,
                            const KrGlanceEdit* edit,
                            KrGlanceState* output);
  KrResult (*show_split_chooser)(void* self);
  KrResult (*make_split)(void* self,
                         const KrSplitCreateRequest* request,
                         KrSplitGroup* output);
  KrResult (*plan_split_copies)(void* self,
                                const KrSplitCopyRequest* request,
                                KrSplitCopyPlan* output);
  KrResult (*get_multi_page_metrics)(void* self, KrMultiPageMetrics* output);
  KrResult (*layout_glance)(void* self,
                            KrRect parent,
                            KrGlanceGeometry* output);
  /* Optional Zen-style full-height card and side controls. layout_glance
   * retains its original inset/toolbar geometry for older hosts. */
  KrResult (*layout_glance_card)(void* self,
                                 KrRect parent,
                                 KrGlanceGeometry* output);
  /* SNAP_LAYOUTS peers only. Opens the chooser with an explicit preset. */
  KrResult (*show_split_chooser_for_layout)(void* self, KrSplitLayout layout);
  KrResult(KR_UI_CALL* register_glass_backdrop)(
      void* self,
      const KrGlassBackdropResource* resource);
  KrResult(KR_UI_CALL* submit_glass_backdrop)(
      void* self,
      const KrGlassBackdropFrame* frame);
  KrResult(KR_UI_CALL* drop_glass_backdrop)(void* self, uint64_t resource_id);
  /* Visual layout gallery for an existing split. Anchor uses client DIPs. */
  KrResult(KR_UI_CALL* show_split_options)(void* self,
                                           KrTabId tab,
                                           KrRect anchor);
  KrResult(KR_UI_CALL* upsert_folder)(void* self, const KrFolder* folder);
  /* Removes the subtree and clears its memberships; it never closes tabs. */
  KrResult(KR_UI_CALL* remove_folder)(void* self, KrFolderId folder);
  /* Optional shared searchable icon/emoji editor for a live tab. */
  KrResult (*show_tab_icon_editor)(void* self, KrTabId tab, KrStr glyph);
  KrResult (*show_boost_editor)(void* self, const KrBoost* value);
  /* Close through the shared popup/focus lifecycle before a host editor opens. */
  KrResult (*dismiss_library)(void* self);
  KrResult (*show_routing_editor)(void* self, const KrRoutingRule* value);
} KrShellUI;

typedef struct KrCreateParams {
  size_t struct_size;
  uint32_t abi_version; /* KR_UI_ABI_VERSION the host was built with */
  void* parent_window;  /* HWND on Windows, NSView* on macOS */
  const KrBrowser* browser;
  /* Optional. Applied while creating native chrome, before the host shows its
   * window. Copied during kr_ui_create; null preserves the default theme. */
  const KrTheme* initial_theme;
} KrCreateParams;

/* Reports whether this module can run here without creating anything. */
KR_UI_API bool KR_UI_CALL kr_ui_is_supported(uint32_t abi_version);

/* Creates the UI inside params->parent_window and fills *out_ui. ABI v1 is not
 * supported; both host and module must rebuild against v2. Set
 * out_ui->struct_size to the host's sizeof(KrShellUI); at most that many bytes
 * are written. Zero selects the v2 base table ending at pre_translate_message.
 * On failure nothing is created. The host owns failure policy; Krypton requires
 * the module and aborts the affected tabbed window instead of falling back. */
KR_UI_API KrResult KR_UI_CALL kr_ui_create(const KrCreateParams* params,
                                           KrShellUI* out_ui);

/* Human-readable platform name, e.g. "windows-winui3". Static storage. */
KR_UI_API const char* KR_UI_CALL kr_ui_platform_name(void);

/* Optional terminal lifecycle export. Call on the creating UI thread after
 * destroying every shell and before the host UI thread exits. Drains/shuts down
 * a dispatcher owned by this module; never shuts down a host-owned dispatcher.
 * The Windows runtime cannot be recreated or the DLL unloaded afterwards.
 * Returns KR_ERR_INVALID_ARGUMENT while shells are still alive. */
KR_UI_API KrResult KR_UI_CALL kr_ui_shutdown(void);

typedef bool(KR_UI_CALL* KrUiIsSupportedFn)(uint32_t abi_version);
typedef KrResult(KR_UI_CALL* KrUiCreateFn)(const KrCreateParams* params,
                                           KrShellUI* out_ui);
typedef const char*(KR_UI_CALL* KrUiPlatformNameFn)(void);
typedef KrResult(KR_UI_CALL* KrUiShutdownFn)(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KRYPTON_UI_ABI_H_ */

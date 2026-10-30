/**
 * persistence — saving and loading tapp settings with a .proto schema.
 *
 * Build:  ./tapp-build examples/persistence
 *
 * Two things worth copying out of this file:
 *
 *   1. WHEN to save. Load in init(), save in deinit(): deinit() runs on every way out. (The
 *      descriptor's .memory_if hooks also exist — load() right after init(), save() when the
 *      device flushes settings — but neither runs on close, so deinit() is still the place.)
 *
 *   2. HOW to store it. A packed struct written straight to disk is fine until you add a field,
 *      at which point every previously saved file is garbage. A .proto costs ~4KB of nanopb
 *      runtime and buys you field numbers: old files keep loading after you extend the schema.
 *
 * Drop persistence.proto next to this file and tapp-build does the rest — protoc + nanopb, then
 * it links the generated descriptors and the nanopb runtime into the .tapp.
 *
 * THE UI is the device's own settings menu (a ui_menu_t, the right-docked panel setup → system
 * uses), so the controls are the standard ones: encoder scrolls, BTN1 selects (toggles a switch,
 * enters/confirms a value edit, runs an action), BTN4/BTN5 step the cursor, BTN4/BTN5 hold close.
 * The settings are params_t because the menu edits them in place; their callback only marks
 * the model dirty. The param TYPE picks the row's look: SelectType over a wide range prints the
 * integer, ParamValType draws the device's value bar, ToggleType its switch.
 */

#include "tapp_api.h"

#include <pb_encode.h>
#include <pb_decode.h>

#include "persistence.pb.h"

/* Already private: the firmware rewrites this to ".data/persistence/settings.pb" on the
 * card, so there is no folder to invent and no name to make unique. */
#define SETTINGS_PATH "/settings.pb"

/* Bump when a field changes meaning. Loads that disagree fall back to defaults rather than
 * reinterpreting old bytes. */
#define SETTINGS_VER 1

// The info card fills what the menu panel leaves (the panel's own fade takes the last ~18px).
#define CARD_X 8
#define CARD_Y (TAPP_CONTENT_Y + 8)

/* The menu panel: setup → system's geometry, the width the device gives rows with an icon, a
 * name AND a value — narrower and "volume" runs into its bar. Top level with the card, clear
 * of the hint band. */
#define MENU_X (SCREEN_WIDTH / 2)
#define MENU_W (SCREEN_WIDTH / 2 - 10)
#define CARD_W (MENU_X - 2 * CARD_X - 12)
#define CARD_H (SCREEN_HEIGHT - CARD_Y - 8)

typedef struct {
    params_t bpm;
    params_t volume;
    params_t invert;
    uint32_t launches;
    char label[sizeof(((PB_PersistSettings*)0)->label)];

    ui_menu_t* menu;
    bool loaded; // false => this run started from defaults
    bool dirty;  // something changed; worth writing on the way out
} persist_model_t;

// The menu fires this on every edit, with the ctx given to ui_menu_create — the model.
static void persist_changed(params_t* p, void* ctx) {
    (void)p;
    ((persist_model_t*)ctx)->dirty = true;
}

static void set_defaults(persist_model_t* m) {
    /* max >= 16 with no option names: the menu prints the raw integer ("120"). */
    m->bpm = (params_t){.name = "bpm", .type = ParamValSelectType, .val = 120, .target = 120,
                        .dflt = 120, .min = 40, .max = 300, .coarse = 1, .fine = 1};
    m->volume = (params_t)PARAM_RANGE("volume", 0.8f, 0.f, 1.f, 0.01f);
    m->volume.type = ParamValType;
    m->invert = (params_t){.name = "invert", .type = ParamValToggleType, .max = 1};
    m->bpm.callback = m->volume.callback = m->invert.callback = persist_changed;
    m->launches = 0;
    strncpy(m->label, "untitled", sizeof(m->label) - 1);
    m->label[sizeof(m->label) - 1] = '\0';
}

// Set a param from a loaded value, clamped to its own range.
static void param_load(params_t* p, float v) {
    p->val = p->target = clampf(v, p->min, p->max);
}

// ============================================================================
// Load / save
// ============================================================================

static bool settings_load(persist_model_t* m) {
    if(!storage_file_exists(SETTINGS_PATH)) return false;

    /* PB_PersistSettings_size is generated: the largest encoding the schema can produce. Sizing
     * the buffer from it means a full message always fits and the number tracks the schema. */
    uint8_t buf[PB_PersistSettings_size];
    uint32_t n = storage_read_file(SETTINGS_PATH, buf, sizeof(buf));
    if(n == 0) return false;

    PB_PersistSettings pb = PB_PersistSettings_init_zero;
    pb_istream_t in = pb_istream_from_buffer(buf, n);
    if(!pb_decode(&in, PB_PersistSettings_fields, &pb)) return false;

    if(pb.ver != SETTINGS_VER) return false;

    /* Clamp EVERYTHING. The file is on a removable card that a host can edit; a decode success
     * only says the bytes were well-formed protobuf, not that the values are sane. The ranges
     * are the params' own, so they live in one place. */
    param_load(&m->bpm, (float)pb.bpm);
    param_load(&m->volume, pb.volume);
    param_load(&m->invert, pb.invert ? 1.f : 0.f);
    m->launches = pb.launches;

    /* nanopb null-terminates a bounded string field, but the struct is only as big as max_size —
     * copy defensively so a hand-edited file cannot walk off the end. */
    strncpy(m->label, pb.label, sizeof(m->label) - 1);
    m->label[sizeof(m->label) - 1] = '\0';
    return true;
}

static bool settings_save(const persist_model_t* m) {
    PB_PersistSettings pb = PB_PersistSettings_init_zero;
    pb.ver = SETTINGS_VER;
    pb.bpm = (uint32_t)m->bpm.val;
    pb.volume = m->volume.val;
    pb.invert = m->invert.val > 0.5f;
    pb.launches = m->launches;
    strncpy(pb.label, m->label, sizeof(pb.label) - 1);

    uint8_t buf[PB_PersistSettings_size];
    pb_ostream_t out = pb_ostream_from_buffer(buf, sizeof(buf));
    if(!pb_encode(&out, PB_PersistSettings_fields, &pb)) return false;

    /* Test > 0, never == bytes_count: on device this returns the byte count, but the desktop
     * emulator returns 1 for success. Only "did anything get written" is portable. */
    return storage_write_file(SETTINGS_PATH, buf, out.bytes_written) > 0;
}

// ============================================================================
// Menu
// ============================================================================

/* Saving here as well as in deinit() is deliberate: pulling the battery never reaches
 * deinit(), so anything you only write on exit is a promise you cannot keep. */
static void persist_save_action(ui_menu_t* menu, ui_menu_node_t* entry) {
    (void)entry;
    persist_model_t* m = ui_menu_ctx(menu);
    if(settings_save(m)) {
        m->dirty = false;
        ui_notify_toast(NotifySuccess, "settings saved", 1500);
    } else {
        ui_notify_toast(NotifyError, "save failed", 2000);
    }
}

static void persist_menu(ui_menu_t* menu) {
    persist_model_t* m = ui_menu_ctx(menu);
    ui_menu_add(menu, (ui_menu_node_t){.name = "bpm", .icon = gfx_icon(GfxIconBpm),
                                       .type = MenuNodeParam, .param = &m->bpm});
    ui_menu_add(menu, (ui_menu_node_t){.name = "volume", .icon = gfx_icon(GfxIconVolume),
                                       .type = MenuNodeParam, .param = &m->volume});
    ui_menu_add(menu, (ui_menu_node_t){.name = "invert", .icon = gfx_icon(GfxIconSettings),
                                       .type = MenuNodeSwitch, .param = &m->invert});
    ui_menu_add(menu, (ui_menu_node_t){.name = "save", .icon = gfx_icon(GfxIconBookmark),
                                       .type = MenuNodeCustom, .callback = persist_save_action});
}

/* Closing the menu IS leaving the app; deinit() writes anything still dirty. */
static void persist_menu_closed(void* ctx, ui_menu_t* menu) {
    (void)ctx;
    (void)menu;
    os_app_exit();
}

// ============================================================================
// App
// ============================================================================

/* Only the info card: the menu paints itself over the right of the screen. "invert" is the
 * demo setting, so it flips the card — ours to draw, unlike the menu. */
static void persist_redraw(gfx_t* gfx, const os_app_t* app) {
    persist_model_t* m = os_app_get_model(app);
    const bool inv = m->invert.val > 0.5f;
    const gfx_coord_t x = CARD_X + 12;

    if(inv) gfx_draw_rect_fill_r(gfx, CARD_X, CARD_Y, CARD_W, CARD_H, 5);
    ui_draw_frame(gfx, CARD_X, CARD_Y, CARD_W, CARD_H, 5, false);
    gfx_set_color(gfx, inv ? GFX_COLOR_CLEAR : GFX_COLOR_SET);

    gfx_set_font(gfx, gfx_nunito_bold_18);
    gfx_draw_str(gfx, x, CARD_Y + 28, m->label);

    gfx_set_font(gfx, gfx_nunito_semibold_14);
    gfx_draw_strf(gfx, x, CARD_Y + 60, "launch #%u", (unsigned)m->launches);
    gfx_draw_str(gfx, x, CARD_Y + 80, m->loaded ? "restored from SD" : "defaults, no file");
    if(m->dirty) gfx_draw_str(gfx, x, CARD_Y + 100, "unsaved changes");

    gfx_set_font(gfx, DepartureMono_Regular_10);
    gfx_draw_str(gfx, x, CARD_Y + CARD_H - 14, SETTINGS_PATH);
}

static bool persist_init(os_app_t* app) {
    persist_model_t* m = os_app_get_model(app);

    set_defaults(m);
    m->loaded = settings_load(m);
    m->launches++;
    m->dirty = true; // the bumped launch counter is itself unsaved state

    m->menu = ui_menu_create(m, persist_menu);
    if(m->menu == NULL) return false;
    ui_menu_set_pos(m->menu, MENU_X, CARD_Y);
    ui_menu_set_width(m->menu, MENU_W);
    ui_menu_set_close_cb(m->menu, persist_menu_closed);
    ui_menu_show(m->menu);

    /* The main menu's own band: the menu widget takes BTN1 (select) and the BTN4/BTN5 presses
     * (cursor) and holds (close). A utility tapp's hints start hidden, so raise them. */
    const tapp_hint_cell_t hints[HintsActionsTotal] = {
        [HintPress1] = {"select", gfx_icon(GfxIconApply)},
        [HintPress4] = {NULL, gfx_icon(GfxIconPlayRev)},
        [HintPress5] = {NULL, gfx_icon(GfxIconPlay)},
        [HintHold4] = {"close", gfx_icon(GfxIconRemove), NULL, HINT_SPAN},
    };
    ui_hints_set(hints);
    ui_hints_show(true);
    return true;
}

static void persist_deinit(os_app_t* app) {
    persist_model_t* m = os_app_get_model(app);
    if(m->dirty) settings_save(m);
    if(m->menu != NULL) ui_menu_destroy(m->menu);
}

/* No on_input and no tick: the menu is modal and takes every button, and the app layer is
 * repainted under it on every frame the menu changes. */
TAPP_APP("Persistence", persist_model_t, persist_init, persist_deinit,
         .redraw = persist_redraw);

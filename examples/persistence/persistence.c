/**
 * persistence — saving and loading tapp settings with a .proto schema.
 *
 * Build:  ./tapp-build examples/persistence
 *
 * Two things worth copying out of this file:
 *
 *   1. WHEN to save. A tapp gets no automatic save hook — the .memory_if field of your descriptor
 *      is overwritten by the loader, and its save/load callbacks are NULL. So: load in init(),
 *      save in deinit(). Nothing else runs for you.
 *
 *   2. HOW to store it. A packed struct written straight to disk is fine until you add a field,
 *      at which point every previously saved file is garbage. A .proto costs ~4KB of nanopb
 *      runtime and buys you field numbers: old files keep loading after you extend the schema.
 *
 * Drop persistence.proto next to this file and tapp-build does the rest — protoc + nanopb, then
 * it links the generated descriptors and the nanopb runtime into the .tapp.
 */

#include "tapp_api.h"

#include <pb_encode.h>
#include <pb_decode.h>

#include "persistence.pb.h"

#define TOP_BAR 60 // the system hint band clears rows 0..59

#define SETTINGS_DIR  "/persistence"
#define SETTINGS_PATH "/persistence/settings.pb"

/* Bump when a field changes meaning. Loads that disagree fall back to defaults rather than
 * reinterpreting old bytes. */
#define SETTINGS_VER 1

typedef struct {
    uint32_t bpm;
    float volume;
    bool invert;
    uint32_t launches;
    char label[sizeof(((PB_PersistSettings*)0)->label)];

    bool loaded;   // false => this run started from defaults
    bool dirty;    // something changed; worth writing on the way out
    uint8_t field; // which setting the encoder edits
    char status[40];
} persist_model_t;

static void set_defaults(persist_model_t* m) {
    m->bpm = 120;
    m->volume = 0.8f;
    m->invert = false;
    m->launches = 0;
    strncpy(m->label, "untitled", sizeof(m->label) - 1);
    m->label[sizeof(m->label) - 1] = '\0';
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
     * only says the bytes were well-formed protobuf, not that the values are sane. */
    m->bpm = pb.bpm < 40u ? 40u : (pb.bpm > 300u ? 300u : pb.bpm);
    m->volume = pb.volume < 0.0f ? 0.0f : (pb.volume > 1.0f ? 1.0f : pb.volume);
    m->invert = pb.invert;
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
    pb.bpm = m->bpm;
    pb.volume = m->volume;
    pb.invert = m->invert;
    pb.launches = m->launches;
    strncpy(pb.label, m->label, sizeof(pb.label) - 1);

    uint8_t buf[PB_PersistSettings_size];
    pb_ostream_t out = pb_ostream_from_buffer(buf, sizeof(buf));
    if(!pb_encode(&out, PB_PersistSettings_fields, &pb)) return false;

    storage_mkdir(SETTINGS_DIR); // no-op if it already exists

    /* Test > 0, never == bytes_count: on device this returns the byte count, but the desktop
     * emulator returns 1 for success. Only "did anything get written" is portable. */
    return storage_write_file(SETTINGS_PATH, buf, out.bytes_written) > 0;
}

// ============================================================================
// App
// ============================================================================

static const char* const FIELD_NAME[3] = {"bpm", "volume", "invert"};

static void persist_redraw(gfx_t* gfx, const os_app_t* app) {
    persist_model_t* m = os_app_get_model(app);

    gfx_set_color(gfx, m->invert ? 0 : 1);
    if(m->invert) gfx_draw_rect_fill(gfx, 0, TOP_BAR, SCREEN_WIDTH, SCREEN_HEIGHT - TOP_BAR);
    gfx_set_color(gfx, m->invert ? 0 : 1);

    gfx_draw_str(gfx, 8, TOP_BAR + 8, m->status);

    for(uint8_t i = 0; i < 3; i++) {
        const gfx_uint_t y = TOP_BAR + 40 + i * 20;
        if(i == m->field) gfx_draw_str(gfx, 8, y, ">");

        if(i == 0) gfx_draw_strf(gfx, 26, y, "bpm      %u", (unsigned)m->bpm);
        else if(i == 1) gfx_draw_strf(gfx, 26, y, "volume   %d%%", (int)(m->volume * 100.0f));
        else gfx_draw_strf(gfx, 26, y, "invert   %s", m->invert ? "on" : "off");
    }

    gfx_draw_strf(gfx, 26, TOP_BAR + 100, "label    %s", m->label);

    gfx_draw_strf(gfx,
                  8,
                  TOP_BAR + 130,
                  "launch #%u  %s",
                  (unsigned)m->launches,
                  m->loaded ? "restored from SD" : "defaults (no file yet)");
    gfx_draw_str(gfx, 8, TOP_BAR + 150, SETTINGS_PATH);
}

static bool persist_tick(os_app_t* app) {
    (void)app;
    return false; // nothing animates; repaint only on input
}

static void persist_input(os_app_t* app, uint8_t btn, KeyStateEnum state) {
    persist_model_t* m = os_app_get_model(app);

    if(state != KEY_STATE_PRESSED && state != KEY_STATE_HOLD) return;

    if(btn == 0 && state == KEY_STATE_PRESSED) {
        m->field = (uint8_t)((m->field + 1u) % 3u);
        return;
    }

    if((btn == 1 || btn == 2) && state == KEY_STATE_PRESSED) {
        const int dir = (btn == 2) ? 1 : -1;
        switch(m->field) {
        case 0: {
            int v = (int)m->bpm + dir * 5;
            m->bpm = (uint32_t)(v < 40 ? 40 : (v > 300 ? 300 : v));
            break;
        }
        case 1: {
            float v = m->volume + (float)dir * 0.05f;
            m->volume = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            break;
        }
        default: m->invert = !m->invert; break;
        }
        m->dirty = true;
        return;
    }

    /* Saving here as well as in deinit() is deliberate: pulling the battery never reaches
     * deinit(), so anything you only write on exit is a promise you cannot keep. */
    if(btn == 3 && state == KEY_STATE_PRESSED) {
        const bool ok = settings_save(m);
        if(ok) m->dirty = false;
        snprintf(m->status, sizeof(m->status), ok ? "saved" : "SAVE FAILED");
        return;
    }

    if(btn == 4 && state == KEY_STATE_HOLD) os_app_exit();
}

static bool persist_init(os_app_t* app, va_list args) {
    (void)args;
    persist_model_t* m = os_app_get_model(app);

    set_defaults(m);
    m->loaded = settings_load(m);
    m->launches++;
    m->dirty = true; // the bumped launch counter is itself unsaved state
    m->field = 0;
    snprintf(m->status,
             sizeof(m->status),
             m->loaded ? "loaded %s" : "no file, using defaults",
             SETTINGS_PATH);

    static const tapp_hint_pair_t hints[5] = {
        {"field", 0},
        {"-", 0},
        {"+", 0},
        {"save", 0},
        {0, "exit"},
    };
    ui_hints_set_labels(hints);
    ui_hints_show(true);
    return true;
}

static bool persist_deinit(os_app_t* app) {
    persist_model_t* m = os_app_get_model(app);
    if(m->dirty) settings_save(m);
    return true;
}

static os_app_data_t persist_app_data = {
    .model = NULL,
    .model_size = sizeof(persist_model_t),
    .init = persist_init,
    .deinit = persist_deinit,
};

static os_app_t persist_app_descriptor = {
    .name = "Persistence",
    .type = AppFullscreenType,
    .data = &persist_app_data,
    .redraw = (os_app_redraw)persist_redraw,
    .tick = persist_tick,
    .on_input = persist_input,
};

os_app_t* tapp_get_descriptor(void) {
    return &persist_app_descriptor;
}

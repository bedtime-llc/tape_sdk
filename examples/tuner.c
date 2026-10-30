// tuner.c — Chromatic tuner TAPP (window mode)
//
// Listens to the device's external input, detects the pitch (NSDF /
// autocorrelation on the main thread), and shows the nearest note, the
// frequency and a cents needle.
//
// Nearly a DISPLAY: the one control is BTN1, switching between the line jack and
// the mics. Which input you are listening to is the single thing a tuner cannot
// pick for you, so it is worth a button and nothing else is.
//
// Window mode: it paints into a rect on top of the still-visible main menu
// rather than taking the screen (os_app_set_window() from init()). Two consequences
// the code below depends on:
//   - the statusbar belongs to the MENU underneath, and so does the hint band
//     until a tapp claims it with ui_hints_set(). The claim is PER CELL,
//     so naming BTN1 leaves BTN2..BTN5 showing the menu's own labels (dimmed,
//     because the menu is deaf behind a modal window) and the firmware's
//     [close] pill on the hold row;
//   - BTN4/BTN5 HOLD are the firmware's exit, checked before on_input() ever
//     sees them, so adding a handler cannot strand the user.
//
// Emu-testable: whistle / play an instrument into the mic.

#include <stdint.h>
#include <stdbool.h>
#include "tapp_api.h" // also declares the libc subset (memcpy, snprintf, sinf, ...)

// ----------------------------------------------------------------------------
// Config
// ----------------------------------------------------------------------------
#define SR          ((float)TAPP_SAMPLE_RATE)
#define RING_LEN    4096u            // power of two
#define RING_MASK   (RING_LEN - 1u)
#define WIN         2048u            // analysis window (~43 ms)
#define LAG_MIN     48u              // 48000/800 = 60 Hz .. 48000/48 = 1000 Hz
#define LAG_MAX     800u
#define NSDF_STRIDE 2u               // decimate inner loop to keep the emu snappy
#define DET_EVERY   2u               // run detection every Nth tick
#define LEVEL_GATE  3e-5f            // mean-square gate below which we show "--"
#define PICK_THRESH 0.55f            // first NSDF peak above this = fundamental
#define LOCK_CONF   0.80f            // confidence needed to display a locked note
#define HOLD_TICKS  10u              // keep the last note up ~500 ms (20 fps) after the signal drops

#define A4_HZ       440.0f           // reference pitch — fixed; nothing to tune it with

// Window rect. y=50 clears the hint band (its hold row ends around y48).
#define WIN_X       60
#define WIN_Y       50
#define WIN_W       280
#define WIN_H       156
#define WIN_CX      (WIN_X + WIN_W / 2)   // 200

// ----------------------------------------------------------------------------
// Model
// ----------------------------------------------------------------------------
typedef struct {
    float* ring;                 // os_malloc(RING_LEN) — mono mic samples
    volatile uint32_t wr;        // ring write index (frames)
    float win[WIN];              // analysis snapshot (heap-backed via the model)

    // last result (written by tick, read by redraw)
    float freq, cents, conf, level, cents_disp;
    int8_t note_idx, octave;     // 0..11 (C..B), -1 = none
    bool locked;

    uint8_t det_phase;           // detection-cadence counter
    uint8_t hold;                // ticks the last note stays up after the signal drops
    uint8_t in_src;              // 0 = line/aux, 1 = mic — os_audio_get_input()
    bool listening;              // engine_set_active() state, read by is_active()
} tuner_model_t;

// ----------------------------------------------------------------------------
// Inline math (no math.h)
// ----------------------------------------------------------------------------
// 2^x, ~0.1% accurate — copied from examples/my_app.c.
static inline float fast_exp2f(float x) {
    int xi = (int)x;
    float xf = x - xi;
    if (x < 0) { xi--; xf += 1.0f; }
    float p = 1.0f + xf * (0.6931472f + xf * (0.2402265f + xf * 0.0558f));
    union { float f; uint32_t i; } u = { .f = p };
    u.i += (uint32_t)xi << 23;
    return u.f;
}
static inline float absf(float x) { return x < 0 ? -x : x; }

static float midi_to_freq(float n, float a4) {
    return a4 * fast_exp2f((n - 69.0f) * (1.0f / 12.0f));
}

static const char* const NOTE_NAME[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// ----------------------------------------------------------------------------
// 5x7 block font (digits, A-G, '#', '-') for the big note readout.
// Row bits: bit4 = leftmost column .. bit0 = rightmost.
// ----------------------------------------------------------------------------
typedef struct { char c; uint8_t row[7]; } glyph_t;
static const glyph_t GLYPHS[] = {
    {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, {'3',{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D',{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}}, {'#',{0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00}},
    {'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
};
static void draw_big(gfx_t* g, int x, int y, const char* s, int scale) {
    for (; *s; s++) {
        if (*s == ' ') { x += 4 * scale; continue; }
        const glyph_t* gl = NULL;
        for (unsigned i = 0; i < sizeof(GLYPHS) / sizeof(GLYPHS[0]); i++)
            if (GLYPHS[i].c == *s) { gl = &GLYPHS[i]; break; }
        if (gl) {
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++)
                    if (gl->row[r] & (1 << (4 - c)))
                        gfx_draw_rect_fill(g, x + c * scale, y + r * scale, scale, scale);
        }
        x += 6 * scale;
    }
}

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// NSDF pitch detection (main thread)
// ----------------------------------------------------------------------------
static float nsdf_at(const float* w, uint32_t lag) {
    float ac = 0.f, en = 0.f;
    uint32_t n = WIN - lag;
    for (uint32_t i = 0; i < n; i += NSDF_STRIDE) {
        float a = w[i], b = w[i + lag];
        ac += a * b;
        en += a * a + b * b;
    }
    return en > 1e-9f ? (2.f * ac / en) : 0.f;
}

/* One detection pass. True = a note locked and the result committed; false leaves the
 * previous result untouched so tick() can age it out. */
static bool detect(tuner_model_t* m) {
    // Snapshot the most recent WIN samples (tolerate benign tearing).
    uint32_t w = m->wr;
    uint32_t start = w - WIN;
    for (uint32_t i = 0; i < WIN; i++)
        m->win[i] = m->ring[(start + i) & RING_MASK];

    // Mean-square level gate.
    float e0 = 0.f;
    for (uint32_t i = 0; i < WIN; i++) e0 += m->win[i] * m->win[i];
    m->level = e0 / (float)WIN;
    if (m->level < LEVEL_GATE) return false;

    // First NSDF peak above threshold = fundamental (kills octave-up errors).
    float prev = 0.f, prev2 = 0.f;
    uint32_t blag = 0;
    for (uint32_t lag = LAG_MIN; lag <= LAG_MAX; lag++) {
        float v = nsdf_at(m->win, lag);
        if (prev > prev2 && prev > v && prev > PICK_THRESH) { blag = lag - 1; break; }
        prev2 = prev; prev = v;
    }
    if (blag <= LAG_MIN || blag >= LAG_MAX) return false;

    // Parabolic interpolation around the peak for sub-sample lag.
    float y0 = nsdf_at(m->win, blag - 1), y1 = nsdf_at(m->win, blag), y2 = nsdf_at(m->win, blag + 1);
    float denom = y0 - 2.f * y1 + y2;
    float delta = (denom != 0.f) ? 0.5f * (y0 - y2) / denom : 0.f;
    if (y1 <= LOCK_CONF) return false;
    m->freq = SR / ((float)blag + delta);
    m->conf = y1;
    m->locked = true;

    // Nearest equal-tempered note by frequency ratio (no logf).
    const float a4 = A4_HZ;
    int best_n = 24; float best_err = 1e30f;
    for (int n = 24; n <= 96; n++) {
        float r = m->freq / midi_to_freq((float)n, a4);
        float e = r > 1.f ? r - 1.f : 1.f - r;
        if (e < best_err) { best_err = e; best_n = n; }
    }
    float fnote = midi_to_freq((float)best_n, a4);
    m->cents = 1731.234f * (m->freq / fnote - 1.f);   // 1200/ln2 * (ratio-1)
    m->note_idx = (int8_t)(best_n % 12);
    m->octave = (int8_t)(best_n / 12 - 1);
    return true;
}

// ----------------------------------------------------------------------------
// Audio: copy mic -> ring, silence the output (avoid feedback)
// ----------------------------------------------------------------------------
/* ctx is our model: the firmware installs the engine after init() succeeded (so the ring
 * exists) and unhooks it before deinit(). */
static void tuner_process(void* restrict ctx, mixer_t* restrict mix) {
    tuner_model_t* m = ctx;
    const float* in = mixer_get_in(mix);
    float* out = mixer_get_out(mix);
    uint32_t fs = mixer_get_fs(mix);
    uint32_t w = m->wr;
    for (uint32_t i = 0; i < fs; i += 2) {
        m->ring[w & RING_MASK] = 0.5f * (in[i] + in[i + 1]);
        w++;
        out[i] = 0.f; out[i + 1] = 0.f;
    }
    m->wr = w;
    /* Sleep evidence. We emit silence, so the firmware's output check sees nothing; this
     * says "this block's input was used". Gated on a LOCKED note, not on level: a hot mic
     * floor sits above LEVEL_GATE and would hold the device awake while we show "--".
     * locked is tick-side (<= 80 ms stale) — fine here: a window tapp is never covered,
     * so tick() runs for as long as we are open. */
    if (m->locked) mixer_rec_active(mix);
}
/* active/is_active are how the firmware starts, stops and idles an engine.
 * Omit is_active and the firmware reads us as ALWAYS active — full clock for as
 * long as we are open; answer 0 while listening and the audio DMA parks a few
 * seconds in and the tuner goes deaf. So: engine_set_active(true) in init un-parks
 * the DMA, is_active keeps it from parking, and the mixer_rec_active() heartbeat
 * in process() keeps the sleep gate honest while a note is present. */
static void tuner_eng_active(void* ctx, bool state) { ((tuner_model_t*)ctx)->listening = state; }
static uint_fast8_t tuner_eng_is_active(void* ctx) { return ((tuner_model_t*)ctx)->listening; }

static const engine_callbacks_t tuner_engine = {
    .process = tuner_process,
    .active = tuner_eng_active,
    .is_active = tuner_eng_is_active,
};

// ----------------------------------------------------------------------------
// Rendering — everything inside the window rect
// ----------------------------------------------------------------------------
static void draw_cents(gfx_t* g, const tuner_model_t* m) {
    const int x0 = WIN_X + 30, x1 = WIN_X + WIN_W - 30, y = 132, cx = (x0 + x1) / 2;
    gfx_set_color(g, 1);
    // in-tune zone (+/-5 cents)
    gfx_fill_rect_dithered(g, cx - 9, y, 18, 20, 3);
    gfx_draw_rect(g, x0, y, x1 - x0, 20);
    gfx_draw_vline(g, cx, y - 4, 28);
    for (int c = -40; c <= 40; c += 20) {
        int tx = cx + (c * (x1 - x0) / 2) / 50;
        gfx_draw_vline(g, tx, y + 14, 6);
    }
    if (m->locked) {
        int nx = cx + (int)(clampf(m->cents_disp, -50.f, 50.f) * (float)(x1 - x0) / 2.f / 50.f);
        gfx_draw_rect_fill(g, nx - 2, y + 1, 5, 18);
    }
}

static void tuner_redraw(gfx_t* gfx, const os_app_t* app) {
    tuner_model_t* m = os_app_get_model(app);

    /* The window's own panel, and it is not optional: the framebuffer is cleared
     * every dirty frame and the main menu paints underneath us, so anything we do
     * not cover shows through as menu pixels. */
    gfx_set_color(gfx, GFX_COLOR_CLEAR);
    gfx_draw_rect_fill_r(gfx, WIN_X, WIN_Y, WIN_W, WIN_H, 10);
    gfx_set_color(gfx, GFX_COLOR_SET);
    gfx_draw_rect_r(gfx, WIN_X, WIN_Y, WIN_W, WIN_H, 10);

    // Big nearest-note readout, centred: "A#4" is wider than "A4" or "--".
    char note[6];
    if (m->locked && m->note_idx >= 0)
        snprintf(note, sizeof(note), "%s%d", NOTE_NAME[m->note_idx], m->octave);
    else
        snprintf(note, sizeof(note), "--");
    /* draw_big advances 6*scale per glyph and inks 5 of those, so the inked width
     * is one scale short of the advance. */
    const int bw = (int)strlen(note) * 6 * 7 - 7;
    draw_big(gfx, WIN_CX - bw / 2, 68, note, 7);

    draw_cents(gfx, m);

    // Frequency left, reference right — both on the cents bar's margins.
    gfx_set_color(gfx, 1);
    if (m->locked) gfx_draw_strf(gfx, WIN_X + 30, 186, "%d.%d Hz",
                                 (int)m->freq, ((int)(m->freq * 10)) % 10);
    else           gfx_draw_str(gfx, WIN_X + 30, 186, "-- Hz");
    {
        static const char REF[] = "A=440";
        const uint16_t w = gfx_get_str_width(gfx, REF);
        gfx_draw_str(gfx, WIN_X + WIN_W - 30 - (int)w, 186, REF);
    }
}

// ----------------------------------------------------------------------------
// Tick: detection + needle easing
// ----------------------------------------------------------------------------
static bool tuner_tick(os_app_t* app) {
    tuner_model_t* m = os_app_get_model(app);

    if (++m->det_phase >= DET_EVERY) { m->det_phase = 0; if (detect(m)) m->hold = HOLD_TICKS; }
    // The last note outlives the signal by HOLD_TICKS, so a dying string does not snap to "--".
    if (m->hold && --m->hold == 0) m->locked = false;

    // Ease the needle toward the measured cents for an analog feel.
    if (m->locked) m->cents_disp += (m->cents - m->cents_disp) * 0.3f;
    return true;
}

// ----------------------------------------------------------------------------
// Input: BTN1 picks the source
// ----------------------------------------------------------------------------
/* BTN1 names the input and nothing else is ours. A window tapp's claim is PER CELL: a cell
 * left empty keeps the MENU's own label (drawn locked, since it is deaf behind us) and only a
 * LABEL takes the cell over — which is also why the label has to carry the state rather than
 * being a constant word. */
static void tuner_hints(const tuner_model_t* m) {
    const bool mic = (m->in_src == 1u);
    const tapp_hint_cell_t hints[HintsActionsTotal] = {
        [HintPress1] = {mic ? "mics" : "line", gfx_icon(mic ? GfxIconMic : GfxIconJack)},
    };
    ui_hints_set(hints);
}

static void tuner_input(os_app_t* app, uint8_t btn, KeyStateEnum state) {
    tuner_model_t* m = os_app_get_model(app);
    if (state != KEY_STATE_PRESSED || btn != BTN1) return;

    /* Switch FIRST, rebuild the band AFTER. os_audio_switch_input() blocks on a SAI re-init,
     * codec I2C and a settings save — keep that out of the middle of a band rebuild.
     *
     * DEVICE-WIDE and persistent: the same setting the main menu writes, and it outlives us.
     * That is why it is bound to a deliberate press and to nothing else. */
    m->in_src = os_audio_switch_input();

    /* The ring still holds ~85 ms of the old source. Say "--" until detect() has refilled it
     * rather than a note we are no longer listening to. The ring itself is left alone: wr is
     * the audio task's, and detect() always reads the most recent WIN samples. */
    m->locked = false;
    m->note_idx = -1;

    tuner_hints(m);
}

// ----------------------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------------------
static bool tuner_init(os_app_t* app) {
    /* A WINDOW over the main menu, not a fullscreen app — first, before any hint call. BTN1
     * is the only button we handle; BTN4/BTN5 HOLD are the firmware's exit and never reach
     * on_input. */
    os_app_set_window(WIN_X, WIN_Y, WIN_W, WIN_H);
    tuner_model_t* m = os_app_get_model(app); // zeroed
    m->ring = (float*)os_malloc(RING_LEN * sizeof(float));
    if (!m->ring) return false;
    memset(m->ring, 0, RING_LEN * sizeof(float));
    m->note_idx = -1;

    /* The source is DEVICE-WIDE and persistent, so read it rather than assuming line. No
     * drift poll in tick(): the menu behind a MODAL window cannot take input, so while we
     * are up BTN1 is the only thing that can move it. No ui_hints_show() either — in window
     * mode the band is already up (which is why WIN_Y clears it). */
    m->in_src = os_audio_get_input();
    tuner_hints(m);

    // Start listening. The engine is installed right after init() returns; a start asked for
    // here is kept until then and handed to tuner_eng_active().
    engine_set_active(true);
    return true;
}

// The firmware has already stopped and unhooked the engine when this runs.
static void tuner_deinit(os_app_t* app) {
    tuner_model_t* m = os_app_get_model(app);
    os_free(m->ring);
}

TAPP_APP("Tuner", tuner_model_t, tuner_init, tuner_deinit,
         .redraw = tuner_redraw,
         .tick = tuner_tick,
         .on_input = tuner_input,
         .engine_cb = &tuner_engine);

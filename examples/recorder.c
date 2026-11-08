// recorder.c — simple recorder TAPP: record the live input to tape, play it back.
//
// This is the reference for tape I/O from a tapp
//
//   process()  does ALL the tape I/O, and still never blocks. Playback and
//              recording both run through one tape_buffer_t — a pair of 4096-frame
//              DMA windows out of the tape's own pool, the same mechanism the
//              built-in tapedecks use. Play out of the resident window, overdub it
//              in place to record; the window is written back when it is evicted.
//   tick()     does the take bookkeeping — cues, marks, erase, meters. No SD.
//
//
// Recording only ever lands in FREE tape: marks are kept sorted and merged, so
// everything past the last mark's end is empty. If there is no room it refuses
// rather than wrapping over your first take.
//
// Controls: BTN1 play/stop   BTN2 rec/stop   BTN3 loop on/off
//           BTN4/BTN5 previous / next take
//           BTN1-hold volume hints (encoder adjusts, BTN4/BTN5 pick the cell)
//           BTN2-hold erase the selected take (cue, mark, AND audio)
//           BTN3-hold mic/line input (device-wide + persistent, hence a hold)
//           LEDs: flash red when a take clips

#include <stdint.h>
#include <stdbool.h>
#include "tapp_api.h"

TAPP_DECLARE_TYPE(AppTypeStandalone);

// ----------------------------------------------------------------------------
// Config
// ----------------------------------------------------------------------------
#define FRAMES_PER_POS 64u           // 1 tape position (SD sector) = 64 stereo frames
#define REC_FADE_FR 1440u            // ~30 ms — the anti-click ramp at both ends
#define WIN_NONE  0xFFFFFFFFu        // "the pipeline is empty" — process() re-primes
#define MIN_REC_FR  (2u * TAPP_SAMPLE_RATE) // refuse a take with less than 2 s of free tape
#define COMMIT_TRIM_POS 64u
#define CLIP_TH 0.95f                // the input limiter's ceiling: a hot mic take is clamped AT it, never past

enum { ST_STOP = 0, ST_REC, ST_PLAY };

// ----------------------------------------------------------------------------
// Model
// ----------------------------------------------------------------------------
typedef struct {
    volatile uint8_t state;          // ST_*
    volatile bool    stop_req;       // set by input, cleared once the fade finishes
    volatile float   rec_gain;       // 0..1 tap fade — the anti-click

    tape_buffer_t*    wb;            // the DMA window pair — claimed in init()
    volatile uint32_t frame;         // absolute tape frame under the head
    volatile uint32_t cur_win;       // window index `frame` last resolved to, or WIN_NONE

    uint32_t rec_tok;                // tape_rec_t token
    uint32_t rec_start;              // where this take began
    uint32_t rec_limit;              // first frame we must NOT write
    bool     no_room;                // last rec attempt refused
    bool     cue_fail;               // last take produced no navigable cue

    uint32_t play_start, play_end;   // the take being played
    bool     loop;                   // BTN3: replay the take instead of stopping

    // metering (process writes, redraw reads)
    volatile float peak_l, peak_r;
    volatile bool  clip;             // latched at 0 dBFS, cleared when a take starts
    bool led_clip;                   // process sets it on an over while recording; the LED pattern takes it
    float hold_l, hold_r; uint16_t hold_t;

    // volume hints (BTN1 hold; input reads, process applies)
    params_t vol_out;                // app-local PLAYBACK trim (IN is the device's)
    params_t vol_fx;                 // app-local FX send of the playback
    volatile bool vol_ui;            // hints shown while PLAY is held
    volatile bool erase_req;         // confirm popup said yes; tick performs it
    volatile bool tape_swapped;      // tape_changed said so; tick re-derives

    uint32_t take_idx;               // take (mark) navigation cursor
    uint32_t take_n;                 // takes (MARKS) on the tape, refreshed from tick
    uint32_t tape_cap;               // tape length in frames
    uint32_t free_fr;                // free tape past the last mark, in frames
    uint8_t  in_src;                 // 0=line 1=mic (the only sources), polled on the tick
    uint8_t  hint_state;             // state the hint labels were built for
    uint8_t  hint_src;               // source the hint switch was built for

    params_t tl_pos, tl_start, tl_end;

    char s_remain[24];               // "FREE mm:ss", drawn in the info column
    char s_take[16];                 // "TAKE n/m", ditto
} rec_model_t;

static void rec_seek(rec_model_t* m, uint32_t frame);
static void rec_tape_derive(rec_model_t* m, tape_t* t);
static void rec_hints(const rec_model_t* m);
static void rec_erase_take(rec_model_t* m);

// ----------------------------------------------------------------------------
// Audio
// ----------------------------------------------------------------------------
static inline void rec_win_request(rec_model_t* m, uint32_t win, bool write) {
    tape_buffer_update(m->wb, win * TAPE_WINDOW_POS, write);
}

// ctx is our model — the firmware hands it to every engine callback.
static void rec_process(void* restrict ctx, mixer_t* restrict mix) {
    rec_model_t* m = ctx;
    float* out = mixer_get_out(mix);
    const float* in = mixer_get_record(mix);
    const uint32_t fs = mixer_get_fs(mix);
    if (!out) return;

    const float g_target = (m->state == ST_REC && !m->stop_req) ? 1.0f : 0.0f;
    const float g_step = 1.0f / (float)REC_FADE_FR;
    const float vout = m->vol_out.val;
    const float vfx = m->vol_fx.val;
    float* send = mixer_get_send(mix);  // ACCUMULATE into it: the FX chain's input

    const bool rec = (m->state == ST_REC);
    const bool run = (rec || m->state == ST_PLAY) && m->wb;

    if (rec) mixer_rec_active(mix);

    float pl = 0.f, pr = 0.f;
    float* buf = run ? tape_buffer_get_ptr(m->wb) : 0;

    for (uint32_t i = 0; i < fs; i += 2) {
        float l = 0.f, r = 0.f;
        float ol = 0.f, orr = 0.f;

        uint32_t off = 0;
        if (run) {
            const uint32_t win = m->frame / TAPE_WINDOW_FR;
            if (win != m->cur_win) {
                if (m->cur_win == WIN_NONE || win != m->cur_win + 1u) {
                    rec_win_request(m, win, rec);
                }
                rec_win_request(m, win + 1u, rec);
                m->cur_win = win;
                buf = tape_buffer_get_ptr(m->wb);
            }
            off = (m->frame & (TAPE_WINDOW_FR - 1u)) * 2u;
        }

        if (m->state == ST_PLAY) {
            if (buf && m->frame < m->play_end) {
                l = buf[off] * vout; r = buf[off + 1] * vout;
                ol = l; orr = r;                  // playback IS ours to output
                m->frame++;
            }
        } else if (in) {
            l = in[i]; r = in[i + 1];
        }

        if (rec) {
            float g = m->rec_gain;
            g += (g_target > g) ? g_step : -g_step;
            g = clampf(g, 0.f, 1.f);
            m->rec_gain = g;
            if (buf && (g > 0.f || !m->stop_req) && m->frame < m->rec_limit) {
                buf[off] = l * g;
                buf[off + 1] = r * g;
                m->frame++;
            }
        }

        const float al = l < 0.f ? -l : l, ar = r < 0.f ? -r : r;
        if (al > pl) pl = al;
        if (ar > pr) pr = ar;

        // Always WRITE (not +=): out_bus still holds the previous block, and the
        // chain's monitor add lands on top of whatever we leave here.
        out[i] = ol; out[i + 1] = orr;
        if (send) { send[i] += ol * vfx; send[i + 1] += orr * vfx; }
    }

    m->peak_l = (pl > m->peak_l) ? pl : m->peak_l * 0.85f;
    m->peak_r = (pr > m->peak_r) ? pr : m->peak_r * 0.85f;
    if (pl >= CLIP_TH || pr >= CLIP_TH) { m->clip = true; if (rec) m->led_clip = true; }
}

/* Red for one step after an over while recording, dark otherwise. Every step writes, since
 * the LED driver latches. Runs on the OS LED task, so it keeps going under the main menu. */
static void rec_led_clip(const os_led_t* led, const void* ctx) {
    (void)led;
    rec_model_t* m = (rec_model_t*)ctx;
    const uint8_t r = m->led_clip ? 50 : 0;
    m->led_clip = false;
    for (uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, r, 0, 0);
}

static led_pattern_t rec_led_pattern = { .callback = rec_led_clip, .steps = 255, .loop = 1 };

/* A tape was loaded from the main menu (never mid-take: the firmware refuses that). Runs on the
 * tape task: stop playing it now, the tick re-derives the rest. */
static void rec_tape_changed(void* ctx) {
    rec_model_t* m = (rec_model_t*)ctx;
    if (m->state == ST_PLAY) m->state = ST_STOP;
    m->tape_swapped = true;
}

/* No is_active: process() runs every block, so stopped it meters the live input. Sleep still
 * comes - a stopped recorder writes silence and does not call mixer_rec_active(). */
static const engine_callbacks_t rec_engine = {
    .process = rec_process,
    .tape_changed = rec_tape_changed,
};

static void rec_set_state(rec_model_t* m, uint8_t state) {
    if (m->state == state) return;
    m->state = state;
}

// ----------------------------------------------------------------------------
// Tape placement — free space only
// ----------------------------------------------------------------------------
#define REC_GUARD_FR (128u * FRAMES_PER_POS)

static bool rec_free_region(tape_t* t, uint32_t* start_fr, uint32_t* limit_fr) {
    if (!t) return false;
    const uint32_t cap = tape_size(t) * FRAMES_PER_POS;
    const uint32_t n = tape_marks_count(t);
    uint32_t end = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = 0, e = 0;
        if (tape_marks_get(t, i, &s, &e) && e > end) end = e;
    }
    const uint32_t start = end ? (end + REC_GUARD_FR) : 0;
    if (start >= cap || (cap - start) < MIN_REC_FR) return false;
    *start_fr = start;
    *limit_fr = cap;
    return true;
}

static void rec_seek(rec_model_t* m, uint32_t frame) {
    m->frame = frame;
    m->cur_win = WIN_NONE;
}

static void rec_stream_invalidate(rec_model_t* m) {
    if (m->wb) tape_buffer_clear_all_buffers(m->wb);
    m->cur_win = WIN_NONE;
}

static void rec_stop_take(rec_model_t* m, tape_t* t) {
    if (m->wb) tape_buffer_flush(m->wb);

    uint32_t end_fr = m->frame;
    m->cue_fail = true;
    if (m->rec_tok) {

        const uint32_t end_pos = tape_rec_commit(
            (tape_rec_t*)(uintptr_t)m->rec_tok,
            m->frame / FRAMES_PER_POS + COMMIT_TRIM_POS);
        const uint32_t start_fr = m->rec_start + REC_FADE_FR;
        end_fr = end_pos * FRAMES_PER_POS;
        m->rec_tok = 0;

        const uint32_t before = tape_cues_count(t);
        m->cue_fail = !tape_cue_add(t, start_fr, end_fr) || tape_cues_count(t) <= before;

        m->take_n = tape_marks_count(t);
        for (uint32_t i = 0; i < m->take_n; i++) {
            uint32_t cs = 0, ce = 0;
            if (tape_marks_get(t, i, &cs, &ce) &&
                cs / FRAMES_PER_POS == start_fr / FRAMES_PER_POS) {
                m->take_idx = i;
                break;
            }
        }

        m->tl_start.val = m->tl_start.target = (float)(start_fr / FRAMES_PER_POS);
        m->tl_end.val = m->tl_end.target = (float)end_pos;
        tape_timeline_set_loop(true);
    }

    m->play_start = m->rec_start + REC_FADE_FR;
    m->play_end = end_fr;
    if (m->play_end > m->play_start) rec_seek(m, m->play_start);
    rec_set_state(m, ST_STOP);
    m->stop_req = false;
}

static void rec_start_take(rec_model_t* m) {
    tape_t* t = tape_get();
    if (!t || !m->wb) { m->no_room = true; return; }
    uint32_t start = 0, limit = 0;
    if (!rec_free_region(t, &start, &limit)) { m->no_room = true; return; }

    m->no_room = false;
    m->cue_fail = false;
    m->clip = false;
    m->rec_start = start;
    m->rec_limit = limit;
    m->rec_gain = 0.f;
    rec_seek(m, start);
    m->rec_tok = (uint32_t)(uintptr_t)tape_rec_begin(
        t, (start + REC_FADE_FR) / FRAMES_PER_POS, false);
    m->tl_start.val = m->tl_start.target = (float)(start / FRAMES_PER_POS);
    m->tl_end.val = m->tl_end.target = (float)(start / FRAMES_PER_POS);
    tape_timeline_set_loop(false);
    m->stop_req = false;
    rec_set_state(m, ST_REC);
}

 #define DIG_Y    (TAPP_CONTENT_Y + 14)           // 7-seg counter, 62..105
#define DIG_W    26
#define DIG_H    44
#define DIG_SPAN 188                             // see rec_redraw: 34+36+12+34+36+10+26
#define DIG_X    84
#define COL_R    392
#define MONO_W   9
#define COL_TAKE 71
#define COL_FREE 89
#define COL_CLIP 107
#define BADGE_X  8
#define BADGE_W  62
#define BADGE_H  22
#define BADGE_Y  (DIG_Y + (DIG_H - BADGE_H) / 2) // centred on the counter
#define RULE_Y   114
#define MET_X    8                               // 16 segments span 8..390 — flush with
#define MET_SW   22                              // the rule above (8..392), no gutters
#define MET_GAP  2
#define MET_PITCH (MET_SW + MET_GAP)
#define MET_SH   13
#define MET_L_Y  121                             // hold pips live in the 4 rows above
#define MET_R_Y  141
#define ALERT_H  20
#define ALERT_Y  146
#define ICON_R   15                               // record dot radius
#define ICON_SQ  30                              // stop square side
#define ICON_H   30                              // play triangle: odd, so the apex is one row
#define ICON_W   24

// 7-segment digits: the readout a digital recorder is mostly made of, and it
// avoids depending on which fonts a tapp can reach.
//        --a--
//       |f   b|
//        --g--
//       |e   c|
//        --d--
static const uint8_t SEG[10] = {
    /*0*/ 0x3F, /*1*/ 0x06, /*2*/ 0x5B, /*3*/ 0x4F, /*4*/ 0x66,
    /*5*/ 0x6D, /*6*/ 0x7D, /*7*/ 0x07, /*8*/ 0x7F, /*9*/ 0x6F,
};

static void draw_digit(gfx_t* g, int x, int y, int w, int h, int d) {
    if (d < 0 || d > 9) return;
    const uint8_t s = SEG[d];
    const int t = 3;                 // stroke
    const int hh = h / 2;
    if (s & 0x01) gfx_draw_rect_fill(g, x + t, y, w - 2 * t, t);              // a
    if (s & 0x02) gfx_draw_rect_fill(g, x + w - t, y + t, t, hh - t);         // b
    if (s & 0x04) gfx_draw_rect_fill(g, x + w - t, y + hh, t, hh - t);        // c
    if (s & 0x08) gfx_draw_rect_fill(g, x + t, y + h - t, w - 2 * t, t);      // d
    if (s & 0x10) gfx_draw_rect_fill(g, x, y + hh, t, hh - t);                // e
    if (s & 0x20) gfx_draw_rect_fill(g, x, y + t, t, hh - t);                 // f
    if (s & 0x40) gfx_draw_rect_fill(g, x + t, y + hh - t / 2, w - 2 * t, t); // g
}

 static void draw_tri(gfx_t* g, int x, int y, int h, int w) {
    for (int i = 0; i < h; i++) {
        const int d = (i < h / 2) ? i : (h - 1 - i);   // rows out from the nearest flat edge
        gfx_draw_hline(g, x, y + i, 1 + (2 * d * (w - 1)) / (h - 1));
    }
}


static void draw_transport(gfx_t* g, int x, int y, int w, int h, uint8_t state, bool lit) {
    const int cx = x + w / 2, cy = y + h / 2;
     gfx_set_color(g, 1);

    if (state == ST_REC) {
        gfx_draw_disc(g, cx, cy, ICON_R, GFX_DRAW_ALL);
    } else if (state == ST_PLAY) {
        draw_tri(g, cx - ICON_W / 2 - 1, cy - ICON_H / 2, ICON_H, ICON_W);
    } else {
        gfx_draw_rect_fill(g, cx - ICON_SQ / 2, cy - ICON_SQ / 2, ICON_SQ, ICON_SQ);
    }
}

#define METER_SEGS 16
static const float METER_TH[METER_SEGS] = {
    0.0056f, 0.0079f, 0.0112f, 0.0158f, 0.0224f, 0.0316f, 0.0447f, 0.0631f,
    0.0891f, 0.1259f, 0.1778f, 0.2512f, 0.3548f, 0.5012f, 0.7079f, 1.0000f,
};

static void draw_meter(gfx_t* g, int y, float peak, float hold) {
    for (int i = 0; i < METER_SEGS; i++) {
        const int sx = MET_X + i * MET_PITCH;
        if (peak >= METER_TH[i]) {
            gfx_draw_rect_fill(g, sx, y, MET_SW, MET_SH);
        } else {
            gfx_fill_rect_dithered(g, sx, y, MET_SW, MET_SH, 1);
        }
        // Peak hold marker.
        if (hold >= METER_TH[i] && (i == METER_SEGS - 1 || hold < METER_TH[i + 1])) {
            gfx_draw_rect_fill(g, sx, y - 4, MET_SW, 2);
        }
    }
}


static void rec_draw_info(gfx_t* gfx, const rec_model_t* m) {
    gfx_set_color(gfx, 1);
    gfx_set_font(gfx, DepartureMono_Regular_10);
    gfx_draw_str(gfx, COL_R - MONO_W * (int)strlen(m->s_take), COL_TAKE, m->s_take);
    gfx_draw_str(gfx, COL_R - MONO_W * (int)strlen(m->s_remain), COL_FREE, m->s_remain);
}

static void rec_redraw(gfx_t* gfx, const os_app_t* app) {
    rec_model_t* m = os_app_get_model(app);
    gfx_set_color(gfx, 1);

    const bool rec = (m->state == ST_REC);
    draw_transport(gfx, BADGE_X, BADGE_Y, BADGE_W, BADGE_H, m->state,
                   rec ? (((os_tick_get() / 400u) & 1u) == 0) : (m->state == ST_PLAY));

    // ---- counter: mm:ss.d of the take being recorded or played ----
    uint32_t fr = 0;
    if (rec) fr = m->frame - m->rec_start;
    else if (m->play_end > m->play_start) fr = m->frame - m->play_start;
    const uint32_t total_ds = fr / 4800u;
    const int mins = (int)(total_ds / 600u) % 100;
    const int secs = (int)((total_ds / 10u) % 60u);
    const int tenths = (int)(total_ds % 10u);

    int dx = DIG_X;
    draw_digit(gfx, dx, DIG_Y, DIG_W, DIG_H, mins / 10); dx += DIG_W + 8;
    draw_digit(gfx, dx, DIG_Y, DIG_W, DIG_H, mins % 10); dx += DIG_W + 10;
    gfx_draw_rect_fill(gfx, dx, DIG_Y + 12, 3, 3);            // colon
    gfx_draw_rect_fill(gfx, dx, DIG_Y + 28, 3, 3); dx += 12;
    draw_digit(gfx, dx, DIG_Y, DIG_W, DIG_H, secs / 10); dx += DIG_W + 8;
    draw_digit(gfx, dx, DIG_Y, DIG_W, DIG_H, secs % 10); dx += DIG_W + 10;
    gfx_draw_rect_fill(gfx, dx, DIG_Y + DIG_H - 3, 3, 3); dx += 10;   // decimal point
    draw_digit(gfx, dx, DIG_Y, DIG_W, DIG_H, tenths);

    // ---- mode glyphs above, TAKE/FREE in the column beside the counter ----
    rec_draw_info(gfx, m);

    gfx_draw_hline_dithered(gfx, 8, RULE_Y, SCREEN_WIDTH - 16, 3);

    // ---- meters ----
    draw_meter(gfx, MET_L_Y, m->peak_l, m->hold_l);
    draw_meter(gfx, MET_R_Y, m->peak_r, m->hold_r);

    // ---- alert plate ----
    const char* warn = 0;
    if (m->no_room)       warn = "NO FREE TAPE SPACE";
    else if (m->cue_fail) warn = "TAKE HAS NO CUE";
    if (warn) {
        gfx_set_font(gfx, gfx_nunito_semibold_14);
        const int tw = (int)gfx_get_str_width(gfx, warn);
        gfx_draw_rect_fill_r(gfx, 8, ALERT_Y, tw + 16, ALERT_H, 3);
        gfx_set_color(gfx, 0);
        gfx_draw_str(gfx, 16, ALERT_Y + 15, warn);
        gfx_set_color(gfx, 1);
    }
}

static void rec_hints(const rec_model_t* m) {
    const tapp_hint_cell_t hints[HintsActionsTotal] = {
        [HintPress1] = {"play", gfx_icon(GfxIconPlay), NULL, m->state == ST_PLAY ? HINT_ACTIVE : 0},
        [HintHold1] = {"vol", gfx_icon(GfxIconVolume)},
        [HintPress2] = {"rec", gfx_icon(GfxIconRec), NULL, m->state == ST_REC ? HINT_ACTIVE : 0},
        [HintHold2] = {"erase", gfx_icon(GfxIconErase)},
        [HintPress3] = {"loop", gfx_icon(GfxIconBack), NULL, m->loop ? HINT_ACTIVE : 0},
        [HintHold3] = {NULL, gfx_icon(GfxIconJack), gfx_icon(GfxIconMic), m->in_src == 1u ? HINT_ON : 0},
        [HintPress4] = {NULL, gfx_icon(GfxIconPlayRev)}, // prev / next take
        [HintPress5] = {NULL, gfx_icon(GfxIconPlay)},
    };
    ui_hints_set(hints);
}

// ----------------------------------------------------------------------------
// Tick — every blocking operation lives here
// ----------------------------------------------------------------------------
static bool rec_tick(os_app_t* app) {
    rec_model_t* m = os_app_get_model(app);
    tape_t* t = tape_get();

    if (m->tape_swapped && t) {
        m->tape_swapped = false;
        rec_stream_invalidate(m);
        m->play_start = m->play_end = 0;
        m->tl_start.val = m->tl_start.target = 0.f;
        m->tl_end.val = m->tl_end.target = 0.f;
        rec_seek(m, 0);
        tape_timeline_set_loop(false);
        rec_tape_derive(m, t);
    }

    if (m->state == ST_REC && t) {
        if (!m->stop_req && m->frame + REC_FADE_FR >= m->rec_limit) m->stop_req = true;
        if ((m->stop_req && m->rec_gain <= 0.001f) || m->frame >= m->rec_limit) {
            rec_stop_take(m, t);
        }
    } else if (m->state == ST_PLAY) {
        if (m->frame >= m->play_end) {
            if (m->loop && m->play_end > m->play_start) rec_seek(m, m->play_start);
            else rec_set_state(m, ST_STOP);
        }
    }

    // Peak hold, ~1 s.
    if (m->peak_l >= m->hold_l) { m->hold_l = m->peak_l; m->hold_t = 25; }
    if (m->peak_r >= m->hold_r) { m->hold_r = m->peak_r; m->hold_t = 25; }
    if (m->hold_t) { m->hold_t--; } else { m->hold_l *= 0.9f; m->hold_r *= 0.9f; }


    if (m->erase_req) { m->erase_req = false; rec_erase_take(m); }

    uint32_t s = 0, lim = 0;
    if (t && rec_free_region(t, &s, &lim)) {
        m->free_fr = lim - s;
        const uint32_t rem = m->free_fr / TAPP_SAMPLE_RATE;
        snprintf(m->s_remain, sizeof(m->s_remain), "FREE %02u:%02u",
                 (unsigned)(rem / 60u), (unsigned)(rem % 60u));
    } else {
        m->free_fr = 0;
        snprintf(m->s_remain, sizeof(m->s_remain), "FREE 00:00");
    }

    m->in_src = os_audio_get_input();

    if (t) m->take_n = tape_marks_count(t);
    if (m->take_n) {
        snprintf(m->s_take, sizeof(m->s_take), "TAKE %u/%u",
                 (unsigned)(m->take_idx + 1u), (unsigned)m->take_n);
    } else {
        snprintf(m->s_take, sizeof(m->s_take), "TAKE -/-");
    }

    if (!m->vol_ui && (m->hint_state != m->state || m->hint_src != m->in_src)) {
        m->hint_state = m->state;
        m->hint_src = m->in_src;
        rec_hints(m);
    }

    const uint32_t head = m->frame;
    m->tl_pos.prev = m->tl_pos.val;
    m->tl_pos.val = m->tl_pos.target = (float)(head / FRAMES_PER_POS);
    if (m->state == ST_REC) m->tl_end.val = m->tl_end.target = m->tl_pos.val;
    return true;
}

static void rec_select_take(rec_model_t* m, uint32_t idx) {
    tape_t* t = tape_get();
    uint32_t cs = 0, ce = 0;
    if (!t || !tape_marks_get(t, idx, &cs, &ce)) return;
    m->take_idx = idx;
    m->play_start = cs;
    m->play_end = ce;
    rec_seek(m, cs);
    m->tl_start.val = m->tl_start.target = (float)(cs / FRAMES_PER_POS);
    m->tl_end.val = m->tl_end.target = (float)(ce / FRAMES_PER_POS);
    tape_timeline_set_loop(true);
}

static void rec_goto_take(rec_model_t* m, bool back) {
    tape_t* t = tape_get();
    if (!t) return;
    const uint32_t n = tape_marks_count(t);
    if (!n) return;
    m->take_n = n;
    rec_select_take(m, back ? (m->take_idx + n - 1u) % n : (m->take_idx + 1u) % n);
}

// What hangs off the mounted tape: at init, and when it's swapped.
static void rec_tape_derive(rec_model_t* m, tape_t* t) {
    m->tape_cap = tape_size(t) * FRAMES_PER_POS;
    m->take_idx = 0;
    m->take_n = tape_marks_count(t);
    if (m->take_n) rec_goto_take(m, true);
}

static void rec_erase_confirm_cb(void* ctx, bool confirmed) {
    if (confirmed) ((rec_model_t*)ctx)->erase_req = true;
}

static void rec_erase_take(rec_model_t* m) {
    tape_t* t = tape_get();
    if (!t || m->state == ST_REC) return;
    uint32_t n = tape_marks_count(t);
    if (!n) return;
    if (m->take_idx >= n) m->take_idx = n - 1u;     // stale cursor guard
    uint32_t cs = 0, ce = 0;
    if (!tape_marks_get(t, m->take_idx, &cs, &ce)) return;
    // Stop BEFORE erasing: the selection is about to go stale, and process()
    // gates its playback reads on state == ST_PLAY.
    if (m->state == ST_PLAY) rec_set_state(m, ST_STOP);
    if (!tape_erase(t, cs, ce)) return;
    rec_stream_invalidate(m);
    n = tape_marks_count(t);
    m->take_n = n;
    if (!n) {
        m->play_start = m->play_end = 0;
        m->tl_start.val = m->tl_start.target = 0.f;
        m->tl_end.val = m->tl_end.target = 0.f;
        tape_timeline_set_loop(false);
    } else {
        if (m->take_idx >= n) m->take_idx = n - 1u;
        rec_select_take(m, m->take_idx);
    }
}


/* List the levels and the band does the rest: BTN4/BTN5 step them and the encoder edits
 * the selected one (the firmware's, not ours), with the running source's level appended. */
static void rec_vol_show(rec_model_t* m) {
    params_t* in_vol = mixer_get_input_vol(mixer_get());
    ui_hints_set_volume(1, 0, NULL, NULL, 0);
    ui_hints_set_volume(1, 1, "out", &m->vol_out, HintVolOutput);
    ui_hints_set_volume(2, 0, "fx send", &m->vol_fx, HintVolFx);
    if (in_vol) ui_hints_set_volume(3, 0, "in", in_vol, HintVolInput);
    else        ui_hints_set_volume(3, 0, NULL, NULL, 0);
    ui_hints_set_volume(4, 0, "<", NULL, 0);
    ui_hints_set_volume(5, 0, ">", NULL, 0);
    ui_hints_volume_select(1, 1);   // start on "out"
}

static void rec_vol_hide(rec_model_t* m) {
    m->vol_ui = false;
    rec_hints(m);
}

static void rec_input(os_app_t* app, uint8_t btn, KeyStateEnum st) {
    rec_model_t* m = os_app_get_model(app);

    if (btn == BTN_ENCODER) return;

    if (m->vol_ui) {
        if (st == KEY_STATE_RELEASED && btn == BTN1) rec_vol_hide(m);
        return;
    }

    if (st == KEY_STATE_PRESSED) {
        switch (btn) {
        case BTN1:
            if (m->state == ST_PLAY) { rec_set_state(m, ST_STOP); }
            else if (m->state == ST_STOP && m->play_end > m->play_start) {
                rec_seek(m, m->play_start);
                m->clip = false;
                rec_set_state(m, ST_PLAY);
            }
            break;
        case BTN2:
            if (m->state == ST_REC) m->stop_req = true;
            else if (m->state != ST_PLAY) rec_start_take(m);
            break;
        case BTN3:
            m->loop = !m->loop;
            break;

        case BTN4: rec_goto_take(m, true);  break;
        case BTN5: rec_goto_take(m, false); break;
        default: break;
        }
        m->hint_state = m->state;
        m->hint_src = m->in_src;

        rec_hints(m);
    } else if (st == KEY_STATE_HOLD) {

        if (btn == BTN1 && !m->vol_ui) { m->vol_ui = true; rec_vol_show(m); }
        if (btn == BTN2 && m->state != ST_REC && m->take_n) {
            ui_notify_confirm("Hold play to confirm\nerasing this take",
                              NULL, rec_erase_confirm_cb, m);
        }
        if (btn == BTN3) {
            m->in_src = os_audio_switch_input();
            m->hint_src = m->in_src;
            rec_hints(m);
        }
    }
}

// ----------------------------------------------------------------------------
// Params / lifecycle
// ----------------------------------------------------------------------------
static bool rec_init(os_app_t* app) {
    rec_model_t* m = os_app_get_model(app);

    m->wb = tape_buffer_init(tape_get());
    if (!m->wb) return false;
    m->cur_win = WIN_NONE;

    m->tl_pos = (params_t)PARAM_RANGE("pos", 0.f, 0.f, 1e9f, 1.f);
    m->tl_start = (params_t)PARAM_RANGE("start", 0.f, 0.f, 1e9f, 1.f);
    m->tl_end = (params_t)PARAM_RANGE("end", 0.f, 0.f, 1e9f, 1.f);
    m->vol_out = (params_t)PARAM_RANGE("out", 1.f, 0.f, 2.f, 0.02f);
    m->vol_out.type = ParamValDBType;
    m->vol_fx = (params_t)PARAM_RANGE("fx send", 0.f, 0.f, 1.f, 0.02f);
    m->vol_fx.type = ParamValDBType;
    m->in_src = os_audio_get_input();
    m->hint_src = m->in_src;

    tape_t* t = tape_get();
    if (t) {
        tape_timeline_setup(1, 8, 188, SCREEN_WIDTH - 16, 46);
        tape_timeline_attach_params(0, &m->tl_pos, &m->tl_start, &m->tl_end);
        tape_timeline_switch_track(0);
        tape_timeline_show_time_display(false);
        tape_timeline_set_loop(false);
        tape_timeline_show(true);
        tape_timeline_zoom_in();

        rec_tape_derive(m, t);
    }

    ui_statusbar_show(false);
    ui_hints_show(true);

    snprintf(m->s_take, sizeof(m->s_take), "TAKE -/-");
    snprintf(m->s_remain, sizeof(m->s_remain), "FREE 00:00");
    m->hint_state = m->state;
    rec_hints(m);

    os_led_set_pattern(&rec_led_pattern, m);
    engine_set_active(true);
    return true;
}

static void rec_deinit(os_app_t* app) {
    rec_model_t* m = os_app_get_model(app);
    if (m->state == ST_REC) {
        m->state = ST_STOP;
        m->rec_gain = 0.f;
        tape_t* t = tape_get();
        if (t) rec_stop_take(m, t);
    }
    tape_timeline_set_loop(false);
    tape_timeline_show(false);
    tape_buffer_deinit(m->wb);
}

TAPP_APP("Recorder", rec_model_t, rec_init, rec_deinit,
         .redraw = rec_redraw,
         .tick = rec_tick,
         .on_input = rec_input,
         .engine_cb = &rec_engine);

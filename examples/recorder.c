// recorder.c — simple recorder TAPP: record the live input to tape, play it back.
//
// This is the reference for tape I/O from a tapp
//
//   process()  never touches the SD card. It copies the live input into a RAM
//              ring on the way to the monitor output, and reads playback out of
//              a RAM buffer. That is the whole audio-thread contract.
//   tick()     does all the blocking work — draining the ring to tape_write()
//              while recording, refilling the playback buffers with tape_read()
//              while playing.
//
// Recording only ever lands in FREE tape: marks are kept sorted and merged, so
// everything past the last mark's end is empty. If there is no room it refuses
// rather than wrapping over your first take.
//
// Controls: BTN1 play/stop   BTN2 rec/stop   BTN3 loop on/off
//           BTN4/BTN5 previous / next take   BTN4-hold exit
//           BTN1-hold volume band (encoder adjusts, BTN4/BTN5 pick IN/OUT)
//           BTN2-hold erase the selected take (cue, mark, AND audio)
//           BTN3-hold mic/line input (device-wide + persistent, hence a hold)

#include <stdint.h>
#include <stdbool.h>
#include "tapp_api.h" // also declares the libc subset (memcpy, snprintf, sinf, ...)

// ----------------------------------------------------------------------------
// Config
// ----------------------------------------------------------------------------
#define SR             48000.0f
#define TOP_BAR        48
#define FRAMES_PER_POS 64u           // 1 tape position (SD sector) = 64 stereo frames
#define RING_FR   16384u             // record ring: 128 KB, ~341 ms against a 40 ms tick
#define PLAY_FR   4096u              // playback half-buffer (one SD DMA's worth)
#define REC_FADE_FR 1440u            // ~30 ms — the anti-click ramp at both ends
#define MIN_REC_FR  (2u * 48000u)    // refuse a take with less than 2 s of free tape
#define COMMIT_TRIM_POS 64u

enum { ST_STOP = 0, ST_REC, ST_PLAY };

// ----------------------------------------------------------------------------
// Model
// ----------------------------------------------------------------------------
typedef struct {
    volatile uint8_t state;          // ST_*
    volatile bool    stop_req;       // set by input, cleared once the fade finishes
    volatile float   rec_gain;       // 0..1 tap fade — the anti-click

    // record ring (process writes, tick drains)
    float*   ring;
    volatile uint32_t w_fr, r_fr;    // free-running frame cursors (wrap-safe)
    volatile uint32_t drops;         // frames lost to a full ring — must stay 0

    uint32_t rec_tok;                // tape_rec_t token
    uint32_t rec_frame;              // write head on tape, in frames
    uint32_t rec_start;              // where this take began
    uint32_t rec_limit;              // first frame we must NOT write
    bool     no_room;                // last rec attempt refused
    bool     cue_fail;               // last take produced no navigable cue

    // playback double buffer (tick fills, process reads)
    float*   play_buf;               // 2 * PLAY_FR frames
    volatile uint8_t  play_cur;      // half process is reading
    volatile bool     half_ready[2];
    uint32_t half_src[2];            // tape frame each half was read from
    volatile uint32_t play_idx;      // frame index within the current half
    volatile uint32_t play_frame;    // absolute tape frame being heard
    uint32_t play_start, play_end;   // the take being played
    uint32_t fill_next;              // next tape frame to read
    bool     loop;                   // BTN3: replay the take instead of stopping

    // metering (process writes, redraw reads)
    volatile float peak_l, peak_r;
    volatile bool  clip;             // latched at 0 dBFS, cleared when a take starts
    float hold_l, hold_r; uint16_t hold_t;

    // volume band (BTN1 hold; input reads, process applies)
    params_t vol_in, vol_out;        // app-local trims, 0..2.0, default 1.0
    uint8_t  vol_sel;                // 0 = IN, 1 = OUT
    volatile bool vol_ui;            // band shown while PLAY is held
    volatile bool erase_req;         // confirm popup said yes; tick performs it

    uint32_t take_idx;               // take (mark) navigation cursor
    uint32_t take_n;                 // takes (MARKS) on the tape, refreshed from tick
    uint32_t tape_cap;               // tape length in frames
    uint32_t free_fr;                // free tape past the last mark, in frames
    uint8_t  in_src;                 // 0=line 1=mic (the only sources)
    uint8_t  hint_state;             // state the hint labels were built for
    uint8_t  hint_src;               // source the hint labels were built for
    uint8_t  hint_takes;             // take-presence the hint LOCKS were built for

    params_t tl_pos, tl_start, tl_end;

    char s_remain[24];
} rec_model_t;

static rec_model_t* g_model = 0;

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void rec_play_seek(rec_model_t* m, uint32_t frame);
static void rec_hints(const rec_model_t* m);
static void rec_erase_take(rec_model_t* m);

// ----------------------------------------------------------------------------
// Audio — no SD access, ever.
// ----------------------------------------------------------------------------
static void rec_process(engine_t* engine, mixer_t* mix) {
    rec_model_t* m = engine_get_ctx(engine);
    if (!m) return;
    float* out = mixer_get_out(mix);
    const float* in = mixer_get_in(mix);
    const uint32_t fs = mixer_get_fs(mix);
    if (!out) return;

    const float g_target = (m->state == ST_REC && !m->stop_req) ? 1.0f : 0.0f;
    const float g_step = 1.0f / (float)REC_FADE_FR;
    const float vin = m->vol_in.val, vout = m->vol_out.val;

    float pl = 0.f, pr = 0.f;

    for (uint32_t i = 0; i < fs; i += 2) {
        float l = 0.f, r = 0.f;

        if (m->state == ST_PLAY && m->play_buf) {
            if (m->half_ready[m->play_cur]) {
                const float* s = m->play_buf + (size_t)m->play_cur * PLAY_FR * 2u
                                 + (size_t)m->play_idx * 2u;
                l = s[0] * vout; r = s[1] * vout;
                m->play_frame++;
                if (++m->play_idx >= PLAY_FR) {
                    m->half_ready[m->play_cur] = false;
                    m->play_cur ^= 1u;
                    m->play_idx = 0;
                }
            }
            // Not ready: output silence rather than stale audio, and do not
            // advance — tick() will catch up.
        } else if (in) {
            // Monitor the input while idle or recording.
            l = in[i] * vin; r = in[i + 1] * vin;
        }

        if (m->state == ST_REC && m->ring) {
            float g = m->rec_gain;
            g += (g_target > g) ? g_step : -g_step;
            g = clampf(g, 0.f, 1.f);
            m->rec_gain = g;

            if (g > 0.f || !m->stop_req) {
                const uint32_t w = m->w_fr;
                if (w - m->r_fr < RING_FR) {
                    float* slot = m->ring + (size_t)(w & (RING_FR - 1)) * 2u;
                    slot[0] = l * g;
                    slot[1] = r * g;
                    m->w_fr = w + 1;
                } else {
                    m->drops++;
                }
            }
        }

        const float al = l < 0.f ? -l : l, ar = r < 0.f ? -r : r;
        if (al > pl) pl = al;
        if (ar > pr) pr = ar;

        out[i] = l; out[i + 1] = r;
    }

    m->peak_l = (pl > m->peak_l) ? pl : m->peak_l * 0.85f;
    m->peak_r = (pr > m->peak_r) ? pr : m->peak_r * 0.85f;
    if (pl >= 1.0f || pr >= 1.0f) m->clip = true;
}

static void rec_eng_active(engine_t* e, bool state) { (void)e; (void)state; }
static uint_fast8_t rec_eng_is_active(engine_t* e) {
    (void)e;
    return 1;
}
static void eng_noop_core(os_core_t* c) { (void)c; }
static void eng_noop(engine_t* e) { (void)e; }

static const engine_callbacks_t rec_engine = {
    .init = eng_noop_core, .process = rec_process, .defaults = eng_noop, .cleanup = eng_noop_core,
    .active = rec_eng_active, .is_active = rec_eng_is_active,
};

// ----------------------------------------------------------------------------
// Tape placement — free space only
// ----------------------------------------------------------------------------
#define REC_GUARD_FR (128u * FRAMES_PER_POS)   // ~170 ms, same as the tapedecks's AUTO mode

static bool rec_free_region(tape_t* t, uint32_t* start_fr, uint32_t* limit_fr) {
    if (!t) return false;
    const uint32_t cap = tape_size(t) * FRAMES_PER_POS;   // tape_size() is POSITIONS
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

static void rec_flush(rec_model_t* m, tape_t* t, int budget, bool final) {
    for (int guard = 0; guard < budget && m->r_fr != m->w_fr; guard++) {
        if (m->rec_frame >= m->rec_limit) break;
        uint32_t n = m->w_fr - m->r_fr;
        const uint32_t to_end = RING_FR - (m->r_fr & (RING_FR - 1));
        if (n > to_end) n = to_end;
        if (n > PLAY_FR) n = PLAY_FR;
        const uint32_t room = m->rec_limit - m->rec_frame;
        if (n > room) n = room;
        if (!final) { n &= ~(FRAMES_PER_POS - 1u); if (!n) break; }
        const float* src = m->ring + (size_t)(m->r_fr & (RING_FR - 1)) * 2u;
        const uint32_t got = tape_write(t, m->rec_frame, src, n);
        m->r_fr += got;
        m->rec_frame += got;
        if (got < n) break;
    }
}

static void rec_stop_take(rec_model_t* m, tape_t* t) {
    rec_flush(m, t, 64, true);

    uint32_t end_fr = m->rec_frame;
    m->cue_fail = true;
    if (m->rec_tok) {

        const uint32_t end_pos = tape_rec_commit(
            (tape_rec_t*)(uintptr_t)m->rec_tok,
            m->rec_frame / FRAMES_PER_POS + COMMIT_TRIM_POS);
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

        // The take just made becomes the selection: shade everything outside it.
        m->tl_start.val = m->tl_start.target = (float)(start_fr / FRAMES_PER_POS);
        m->tl_end.val = m->tl_end.target = (float)end_pos;
        tape_timeline_set_loop(true);
    }

    m->play_start = m->rec_start + REC_FADE_FR;
    m->play_end = end_fr;
    if (m->play_end > m->play_start) rec_play_seek(m, m->play_start);
    m->state = ST_STOP;
    m->stop_req = false;
}

static void rec_start_take(rec_model_t* m) {
    tape_t* t = tape_get();
    if (!t || !m->ring) { m->no_room = true; return; }
    uint32_t start = 0, limit = 0;
    if (!rec_free_region(t, &start, &limit)) { m->no_room = true; return; }

    m->no_room = false;
    m->cue_fail = false;
    m->clip = false;
    m->rec_start = start;
    m->rec_frame = start;
    m->rec_limit = limit;
    m->w_fr = m->r_fr = 0;
    m->drops = 0;
    m->rec_gain = 0.f;
    m->rec_tok = (uint32_t)(uintptr_t)tape_rec_begin(
        t, (start + REC_FADE_FR) / FRAMES_PER_POS, false);
    m->tl_start.val = m->tl_start.target = (float)(start / FRAMES_PER_POS);
    m->tl_end.val = m->tl_end.target = (float)(start / FRAMES_PER_POS);
    tape_timeline_set_loop(false);
    m->stop_req = false;
    m->state = ST_REC;
}

static void rec_play_seek(rec_model_t* m, uint32_t frame) {
    m->half_ready[0] = m->half_ready[1] = false;
    m->play_cur = 0;
    m->play_idx = 0;
    m->play_frame = frame;
    m->fill_next = frame;
}

static void rec_fill_halves(rec_model_t* m, tape_t* t) {
    if (!m->play_buf) return;
    for (int h = 0; h < 2; h++) {
        const uint8_t idx = (uint8_t)((m->play_cur + h) & 1u);
        if (m->half_ready[idx]) continue;
        if (m->fill_next >= m->play_end) return;          // take finished
        uint32_t n = m->play_end - m->fill_next;
        if (n > PLAY_FR) n = PLAY_FR;
        float* dst = m->play_buf + (size_t)idx * PLAY_FR * 2u;
        const uint32_t got = tape_read(t, m->fill_next, dst, n);
        if (n < PLAY_FR) {                                // zero the tail
            memset(dst + (size_t)n * 2u, 0, (size_t)(PLAY_FR - n) * 2u * sizeof(float));
        }
        m->half_src[idx] = m->fill_next;
        m->fill_next += got ? got : n;
        m->half_ready[idx] = true;
    }
}

 
#define DIG_Y    (TOP_BAR + 14)                  // 7-seg counter, 62..105
#define DIG_W    26
#define DIG_H    44
#define DIG_SPAN 188                             // see rec_redraw: 34+36+12+34+36+10+26
#define DIG_X    84                              // between the badge and the info column
#define BADGE_X  8
#define BADGE_W  62
#define BADGE_H  22
#define BADGE_Y  (DIG_Y + (DIG_H - BADGE_H) / 2) // centred on the counter
#define COL_R    392                             // info column, right-aligned to here
#define COL_TAKE 81                              // baselines: which take, free tape —
#define COL_FREE 99                              // two rows centred on the counter block
#define RULE_Y   114
#define MET_X    8                               // 16 segments span 8..390 — flush with
#define MET_SW   22                              // the rule above (8..392), no gutters
#define MET_GAP  2
#define MET_PITCH (MET_SW + MET_GAP)
#define MET_SH   13
#define MET_L_Y  121                             // hold pips live in the 4 rows above
#define MET_R_Y  141
#define ALERT_H  20
// The plate OVERLAYS the lower meter row — same philosophy it always had (it used
// to cover the live waveform strip): a warning is rare and actionable, and the
// solid plate erases what is beneath. 146..165 stays inside the 167 budget.
#define ALERT_Y  146
// Transport glyphs, sized to sit inside the pill with room to breathe.
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
        gfx_draw_hline(g, (gfx_uint_t)x, (gfx_uint_t)(y + i),
                       (gfx_uint_t)(1 + (2 * d * (w - 1)) / (h - 1)));
    }
}


static void draw_transport(gfx_t* g, int x, int y, int w, int h, uint8_t state, bool lit) {
    const int cx = x + w / 2, cy = y + h / 2;
     gfx_set_color(g, 1);

    if (state == ST_REC) {
        gfx_draw_disc(g, (gfx_uint_t)cx, (gfx_uint_t)cy, ICON_R, GFX_DRAW_ALL);
    } else if (state == ST_PLAY) {
        // Optically centred, not arithmetically: a triangle's mass sits behind its apex, so
        // centring the bounding box leaves it looking a pixel or two right of centre.
        draw_tri(g, cx - ICON_W / 2 - 1, cy - ICON_H / 2, ICON_H, ICON_W);
    } else {
        gfx_draw_rect_fill(g, cx - ICON_SQ / 2, cy - ICON_SQ / 2, ICON_SQ, ICON_SQ);
    }
}

// Segment thresholds in amplitude, 3 dB apart from -45 dB to 0 dB. A table
// instead of a log so the meter needs no math library.
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


static void rec_redraw(gfx_t* gfx, const os_app_t* app) {
    rec_model_t* m = os_app_get_model(app);
    gfx_set_color(gfx, 1);

    const bool rec = (m->state == ST_REC);
    draw_transport(gfx, BADGE_X, BADGE_Y, BADGE_W, BADGE_H, m->state,
                   rec ? (((ui_get_frame(gfx) >> 3) & 1u) == 0) : (m->state == ST_PLAY));

    // ---- counter: mm:ss.d of the take being recorded or played ----
    uint32_t fr = 0;
    if (rec) fr = m->w_fr;
    else if (m->play_end > m->play_start) fr = m->play_frame - m->play_start;
    const uint32_t total_ds = fr / 4800u;                 // tenths of a second
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

    gfx_set_font(gfx, DepartureMono_Regular_10);
    char line[16];
    if (m->take_n) {
        snprintf(line, sizeof(line), "TAKE %u/%u",
                 (unsigned)(m->take_idx + 1u), (unsigned)m->take_n);
    } else {
        snprintf(line, sizeof(line), "TAKE -/-");
    }
    gfx_draw_str(gfx, COL_R - (int)gfx_get_str_width(gfx, line), COL_TAKE, line);
    gfx_draw_str(gfx, COL_R - (int)gfx_get_str_width(gfx, m->s_remain), COL_FREE, m->s_remain);

    gfx_draw_hline_dithered(gfx, 8, RULE_Y, SCREEN_WIDTH - 16, 3);

    // ---- meters ----
    draw_meter(gfx, MET_L_Y, m->peak_l, m->hold_l);
    draw_meter(gfx, MET_R_Y, m->peak_r, m->hold_r);

    // ---- alert plate ----
    char buf[28];
    const char* warn = 0;
    if (m->no_room)       warn = "NO FREE TAPE SPACE";
    else if (m->cue_fail) warn = "TAKE HAS NO CUE";
    else if (m->drops)    { snprintf(buf, sizeof(buf), "DROPPED %u FRAMES",
                                     (unsigned)m->drops); warn = buf; }
    if (warn) {
        gfx_set_font(gfx, gfx_nunito_semibold_14);
        const int tw = (int)gfx_get_str_width(gfx, warn);
        gfx_draw_rect_fill_r(gfx, 8, ALERT_Y, tw + 16, ALERT_H, 3);
        gfx_set_color(gfx, 0);
        gfx_draw_str(gfx, 16, ALERT_Y + 15, warn);
        gfx_set_color(gfx, 1);
    }
}

// ----------------------------------------------------------------------------
// Hints / status
// ----------------------------------------------------------------------------
static void rec_hints(const rec_model_t* m) {
    static tapp_hint_pair_t hints[5];
    hints[0] = (tapp_hint_pair_t){ m->state == ST_PLAY ? "stop" : "play", "vol" };
    hints[1] = (tapp_hint_pair_t){ m->state == ST_REC ? "stop" : "rec", "erase" };
    hints[2] = (tapp_hint_pair_t){ "loop",
                                   m->in_src == 1u ? "mic" :
                                   m->in_src == 0u ? "line" : "input" };
    hints[3] = (tapp_hint_pair_t){ "< prev", "exit" };
    hints[4] = (tapp_hint_pair_t){ "next >", NULL };
    ui_hints_set_labels(hints);

    // set_labels rebuilds the cells and clears active/locked state — re-assert
    // last. Unavailable actions render as dithered 
    ui_hints_set_active(HintPress3, m->loop);
    const bool no_takes = m->take_n == 0;
    ui_hints_set_locked(HintPress1, m->state == ST_REC ||
                        (m->state == ST_STOP && m->play_end <= m->play_start));
    ui_hints_set_locked(HintPress2, m->state == ST_PLAY);
    ui_hints_set_locked(HintHold2, m->state == ST_REC || no_takes);
    ui_hints_set_locked(HintPress4, no_takes);
    ui_hints_set_locked(HintPress5, no_takes);
}

// ----------------------------------------------------------------------------
// Tick — every blocking operation lives here
// ----------------------------------------------------------------------------
static bool rec_tick(os_app_t* app) {
    rec_model_t* m = os_app_get_model(app);
    tape_t* t = tape_get();

    if (m->state == ST_REC && t) {
        rec_flush(m, t, 4, false);
        if (!m->stop_req && m->rec_start + m->w_fr + REC_FADE_FR >= m->rec_limit) {
            m->stop_req = true;
        }

        if ((m->stop_req && m->rec_gain <= 0.001f) || m->rec_frame >= m->rec_limit) {
            rec_stop_take(m, t);
        }
    } else if (m->state == ST_PLAY && t) {
        rec_fill_halves(m, t);
        if (m->play_frame >= m->play_end) {
            if (m->loop && m->play_end > m->play_start) rec_play_seek(m, m->play_start);
            else m->state = ST_STOP;
        }
    }

    // Peak hold, ~1 s.
    if (m->peak_l >= m->hold_l) { m->hold_l = m->peak_l; m->hold_t = 25; }
    if (m->peak_r >= m->hold_r) { m->hold_r = m->peak_r; m->hold_t = 25; }
    if (m->hold_t) { m->hold_t--; } else { m->hold_l *= 0.9f; m->hold_r *= 0.9f; }

    // Erase deferred from the confirm popup's callback (widget context).
    if (m->erase_req) { m->erase_req = false; rec_erase_take(m); }

    // Remaining free tape.
    uint32_t s = 0, lim = 0;
    if (t && rec_free_region(t, &s, &lim)) {
        m->free_fr = lim - s;
        const uint32_t rem = m->free_fr / 48000u;
        snprintf(m->s_remain, sizeof(m->s_remain), "FREE %02u:%02u",
                 (unsigned)(rem / 60u), (unsigned)(rem % 60u));
    } else {
        m->free_fr = 0;
        snprintf(m->s_remain, sizeof(m->s_remain), "FREE 00:00");
    }
    if (t) m->take_n = tape_marks_count(t);

    // A take can also end on its own — tape full, or playback reaching the end —
    // and an erase can flip take-presence with no state change; the labels AND
    // the locked cells follow the model, not only the press that changed it.
    // Never while the volume band is up: set_labels would replace its cells.
    if (!m->vol_ui && (m->hint_state != m->state || m->hint_src != m->in_src ||
                       m->hint_takes != (m->take_n != 0))) {
        m->hint_state = m->state;
        m->hint_src = m->in_src;
        m->hint_takes = m->take_n != 0;
        rec_hints(m);
    }

    const uint32_t head = (m->state == ST_REC) ? (m->rec_start + m->w_fr) : m->play_frame;
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
    rec_play_seek(m, cs);
    m->tl_start.val = m->tl_start.target = (float)(cs / FRAMES_PER_POS);
    m->tl_end.val = m->tl_end.target = (float)(ce / FRAMES_PER_POS);
    tape_timeline_set_loop(true);
}

// Marks are kept sorted by start, so stepping is index +/- 1 with wrap and the
// order matches the timeline left-to-right.
static void rec_goto_take(rec_model_t* m, bool back) {
    tape_t* t = tape_get();
    if (!t) return;
    const uint32_t n = tape_marks_count(t);
    if (!n) return;
    m->take_n = n;
    rec_select_take(m, back ? (m->take_idx + n - 1u) % n : (m->take_idx + 1u) % n);
}

static void rec_erase_confirm_cb(void* ctx, void* confirmed) {
    if (confirmed) ((rec_model_t*)ctx)->erase_req = true;
}

// BTN2 hold: erase the selected take — audio, MARK and any overlapping cue,
// via tape_erase(). Bounds come from the take's MARK (the navigation unit), so
// a take without a cue erases just as well. Not while recording (erase refuses
// under an open take anyway). Erasing the last take genuinely reclaims free
// tape: the free region is everything past the last mark.
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
    if (m->state == ST_PLAY) m->state = ST_STOP;
    if (!tape_erase(t, cs, ce)) return;
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


static void rec_vol_step(params_t* p, int32_t acc) {
    float delta = p->fine * (float)acc;
    const float norm = (p->target - p->min) / (p->max - p->min);
    delta *= 0.1f + 0.9f * norm;
    float t = p->target + delta;
    t = (float)(int32_t)(t * 1000.f + 0.5f) * 0.001f;   // roundf isn't exported; t >= 0 here
    p->target = p->val = clampf(t, p->min, p->max);
}

static void rec_vol_show(rec_model_t* m) {
    ui_hints_set_volume(1, 0, "in",  &m->vol_in,  1);
    ui_hints_set_volume(2, 0, "out", &m->vol_out, 0);
    ui_hints_volume_select(m->vol_sel ? 2 : 1, 0);
}

static void rec_input(os_app_t* app, uint8_t btn, KeyStateEnum st) {
    rec_model_t* m = os_app_get_model(app);

    if (btn == 5) {
        // Encoder rotation (state is always RELEASED — never branch on it).
        // Drain the delta unconditionally so the accumulator can't dump a stale
        // jump into the first trim adjustment. Writing our own param->val is the
        // same architecture tapedecks uses — the cells redraw from it each frame.
        const int32_t d = os_controls_encoder_get_delta();
        if (m->vol_ui && d) {
            rec_vol_step(m->vol_sel ? &m->vol_out : &m->vol_in, d);
        }
        return;
    }

    if (st == KEY_STATE_PRESSED) {
        switch (btn) {
        case 0:                                   // play / stop
            if (m->state == ST_PLAY) { m->state = ST_STOP; }
            else if (m->state == ST_STOP && m->play_end > m->play_start) {
                rec_play_seek(m, m->play_start);
                m->clip = false;                  // the readout resets with the pass
                m->state = ST_PLAY;
            }
            break;
        case 1:                                   // rec / stop
            if (m->state == ST_REC) m->stop_req = true;   // fade out, tick closes it
            else if (m->state != ST_PLAY) rec_start_take(m);
            break;
        case 2:                                   // loop on/off for playback
            m->loop = !m->loop;
            break;
        // While the volume band is up, BTN4/BTN5 pick the cell instead of stepping cues
        case 3: if (m->vol_ui) { m->vol_sel = 0; ui_hints_volume_select(1, 0); }
                else rec_goto_take(m, true);
                break;
        case 4: if (m->vol_ui) { m->vol_sel = 1; ui_hints_volume_select(2, 0); }
                else rec_goto_take(m, false);
                break;
        default: break;
        }
        m->hint_state = m->state;
        m->hint_src = m->in_src;
        m->hint_takes = m->take_n != 0;

        if (!m->vol_ui) rec_hints(m);
    } else if (st == KEY_STATE_HOLD) {

        if (btn == 3) { os_app_exit(); return; }
        if (btn == 0) { m->vol_ui = true; rec_vol_show(m); }
        if (btn == 1 && m->state != ST_REC && m->take_n) {
            ui_notify_confirm("Hold play to confirm\nerasing this take",
                              NULL, rec_erase_confirm_cb, m);
        }
        if (btn == 2) {
            // Mic <-> line. This writes the DEVICE-WIDE input setting and it.
            m->in_src = os_audio_switch_input(true);
            m->hint_src = m->in_src;
            if (!m->vol_ui) rec_hints(m);
        }
    } else if (st == KEY_STATE_RELEASED) {
        if (btn == 0 && m->vol_ui) {
            m->vol_ui = false;
            rec_hints(m);
        }
    }
}

// ----------------------------------------------------------------------------
// Params / lifecycle
// ----------------------------------------------------------------------------
static void mk_param(params_t* p, const char* name, float val, float mn, float mx, float coarse) {
    memset(p, 0, sizeof(*p));
    *(const char**)&p->name = name;
    *(ParamTypeUI_t*)&p->type = ParamValExactType;
    *(bool*)&p->refresh = true;
    p->val = val; p->min = mn; p->max = mx; p->dflt = val; p->coarse = coarse; p->fine = coarse;
}

static bool rec_init(os_app_t* app, va_list args) {
    (void)args;
    rec_model_t* m = os_app_get_model(app);
    g_model = m;

    m->ring = (float*)os_malloc(RING_FR * 2u * sizeof(float));
    m->play_buf = (float*)os_malloc(2u * PLAY_FR * 2u * sizeof(float));
    if (!m->ring || !m->play_buf) return false;

    mk_param(&m->tl_pos,   "pos",   0.f, 0.f, 1e9f, 1.f);
    mk_param(&m->tl_start, "start", 0.f, 0.f, 1e9f, 1.f);
    mk_param(&m->tl_end,   "end",   0.f, 0.f, 1e9f, 1.f);
    mk_param(&m->vol_in,  "in",  1.f, 0.f, 2.f, 0.02f);
    mk_param(&m->vol_out, "out", 1.f, 0.f, 2.f, 0.02f);
    *(ParamTypeUI_t*)&m->vol_in.type = ParamValDBType;
    *(ParamTypeUI_t*)&m->vol_out.type = ParamValDBType;
    m->vol_sel = 0;
    m->state = ST_STOP;
    // Both, or the tick's hint drift check fires once for nothing on frame one.
    m->in_src = os_audio_get_input();
    m->hint_src = m->in_src;

    tape_t* t = tape_get();
    if (t) {
        m->tape_cap = tape_size(t) * FRAMES_PER_POS;
        tape_timeline_setup(1, 8, 188, SCREEN_WIDTH - 16, 46);
        tape_timeline_attach_params(0, &m->tl_pos, &m->tl_start, &m->tl_end);
        tape_timeline_switch_track(0);
        tape_timeline_show_time_display(false);
        tape_timeline_set_loop(false);
        tape_timeline_show(true);
        tape_timeline_zoom_in();

        m->take_n = tape_marks_count(t);
        if (m->take_n) { m->take_idx = 0; rec_goto_take(m, true); }
    }

    // The statusbar's own text is not drawn while the hint band is up, so the
    // transport state lives in the app's badge instead of in a string nobody sees.
    ui_statusbar_show(true);
    m->hint_state = m->state;
    m->hint_takes = m->take_n != 0;
    rec_hints(m);
    ui_hints_show(true);

    engine_set_callbacks(app, m);
    engine_set_active(true);
    return true;
}

static bool rec_deinit(os_app_t* app) {
    rec_model_t* m = os_app_get_model(app);
    g_model = 0;
    if (m->state == ST_REC) {
        m->state = ST_STOP;
        m->rec_gain = 0.f;
        tape_t* t = tape_get();
        if (t) rec_stop_take(m, t);
    }
    m->state = ST_STOP;

    tape_timeline_set_loop(false);
    tape_timeline_show(false);
    engine_set_active(false);
    engine_clear_callbacks(&rec_engine);
    if (m->ring) { os_free(m->ring); m->ring = 0; }
    if (m->play_buf) { os_free(m->play_buf); m->play_buf = 0; }
    return true;
}

static os_app_data_t rec_data = {
    .model = NULL, .model_size = sizeof(rec_model_t), .init = rec_init, .deinit = rec_deinit,
};

static os_app_t rec_app = {
    .name = "Recorder",
    .type = AppFullscreenType,
    .data = &rec_data,
    .redraw = (os_app_redraw)rec_redraw,
    .tick = rec_tick,
    .on_input = rec_input,
    .engine_cb = &rec_engine,
};

os_app_t* tapp_get_descriptor(void) { return &rec_app; }

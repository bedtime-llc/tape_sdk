// chopper.c — tape cut-up grain instrument TAPP.
//
// Loads a few large REGIONS of the tape's recorded audio (found via marks) into
// a RAM bank pool, then plays short GRAINS taken from anywhere inside those
// banks: random start, length, pitch, direction and pan, windowed so they never
// click. That is the difference from replaying whole slices — the material gets
// rearranged from the inside, and every knob stays live while it runs.
//
// Source is the tape's MARKS. A tape with no marks has nothing to find, and the
// app says so specifically rather than sitting silent.
//
// Banks load incrementally from tick() (tape_read blocks on SD), one chunk at a
// time, so the app never stalls the controls task. A bank being (re)filled is
// simply not eligible for new grains — the music keeps playing around it.
//
// Controls: BTN1=play/stop  BTN2=menu  BTN3=re-roll grains
//           BTN4=enc target back / hold exit   BTN5=enc target fwd / hold re-read
//           ENC drives the selected target.

#include <stdint.h>
#include <stdbool.h>
#include "tapp_api.h" // also declares the libc subset (memcpy, snprintf, sinf, ...)

/* Soft clipper. Trivial DSP lives in the app, not the API. */
static inline float soft_limit(float x) {
    return x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x * (1.5f - 0.5f * x * x);
}

// ----------------------------------------------------------------------------
// Config
// ----------------------------------------------------------------------------
#define SR          48000.0f
#define BANKS       4               // source banks
#define BANK_FR     32768u          // frames per bank (~0.68 s)
#define BANK_FL     (BANK_FR * 2u)
#define POOL_FL     (BANKS * BANK_FL)      // 4 * 65536 = 262144 floats = 1 MB
#define POOL_FL_MIN (BANKS * 8192u * 2u)   // fallback: 4 * 8192 fr = 256 KB
#define CHUNK_FR    4096u           // one SD DMA's worth
#define VOICES      6
#define STEPS       16
#define HIST        32              // grains remembered for the Markov memory
#define TOP_BAR     60              // the system hint band clears rows 0..59

// App state — each one gets its own message, so "nothing happens" never happens.
enum { CH_NO_TAPE = 0, CH_NO_MARKS, CH_LOADING, CH_READ_FAIL, CH_READY };

// Encoder targets — BTN4/BTN5 step through these.
enum { ENC_TEMPO = 0, ENC_DENS, ENC_GRAIN, ENC_PITCH, ENC_SPREAD, ENC_SCATTER, ENC_REPEAT,
       ENC_MEMORY, ENC_TOTAL };
static const char* const ENC_NAME[ENC_TOTAL] = {
    "tempo", "dens", "grain", "pitch", "spread", "scatter", "repeat", "memory"
};

// ----------------------------------------------------------------------------
// Inline math (no math.h)
// ----------------------------------------------------------------------------
static inline float fast_exp2f(float x) {
    int xi = (int)x; float xf = x - xi;
    if (x < 0) { xi--; xf += 1.0f; }
    float p = 1.0f + xf * (0.6931472f + xf * (0.2402265f + xf * 0.0558f));
    union { float f; uint32_t i; } u = { .f = p };
    u.i += (uint32_t)xi << 23;
    return u.f;
}
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float sinu(float phase01) {
    uint16_t deg = (uint16_t)((phase01 - (int)phase01) * 360.0f) % 360u;
    return isin(deg) * (1.0f / 32768.0f);
}

// ----------------------------------------------------------------------------
// Model
// ----------------------------------------------------------------------------
typedef struct {
    float    idx;       // fractional read index (frames) within the bank
    float    inc;       // per-frame increment (pitch; negative = reverse)
    float    pos;       // 0..1 through the grain, drives the window
    float    step;      // window advance per frame = 1/len
    uint8_t  bank;
    float    panl, panr;
    bool     active;
} voice_t;

// One played grain, kept so the app can replay its own phrases.
typedef struct {
    uint32_t start;                 // frame offset within the bank
    float    inc;                   // rate, negative = reverse
    float    len;                   // grain length in output frames
    uint8_t  bank;
    uint8_t  pan;                   // 0..255
} grain_rec_t;

typedef struct {
    float*   pool;                  // os_malloc — BANKS banks
    uint32_t bank_fr;               // frames per bank actually allocated
    uint32_t bank_len[BANKS];       // valid frames per bank (0 = empty)
    uint8_t  bank_ready[BANKS];     // 1 = safe for the audio thread to read
    uint32_t src_start[BANKS];      // tape frame each bank was read from
    uint8_t  src_mark[BANKS];       // which mark index that came from (for the strip)

    // incremental loader state (tick thread)
    uint8_t  load_bank;             // bank being filled, BANKS = idle
    uint32_t load_done;             // frames already read into it
    uint32_t load_want;             // frames wanted for it
    uint32_t load_src;              // tape frame to read from next

    uint8_t  state;                 // CH_*
    uint32_t n_marks;

    // sequencer (written by process, read by redraw)
    volatile uint8_t step;
    volatile uint8_t play_bank;
    uint32_t samp_in_step;
    uint32_t xs;                    // audio-thread RNG
    uint32_t xs_ui;                 // ui-thread RNG
    bool     playing;
    uint8_t  enc_target;

    // beat-repeat latch
    uint8_t  rep_left;
    float    rep_idx; float rep_inc; uint8_t rep_bank; float rep_len;

    voice_t  voices[VOICES];
    uint8_t  vnext;

    // Markov memory: the grains we have played, so we can quote ourselves.
    grain_rec_t hist[HIST];
    uint8_t  hist_w;                // ring write cursor
    uint8_t  hist_n;                // entries filled (saturates at HIST)
    uint8_t  phrase_pos;            // position within the phrase being replayed
    uint8_t  phrase_len;            // live phrase length, for the display

    params_t bpm, density, grain, pitch, spread, reverse, scatter, repeat, memory;
    ui_menu_t* menu;
} ch_model_t;

static ch_model_t* g_model = 0;

static inline uint32_t xrand(ch_model_t* m) { uint32_t x = m->xs; x ^= x << 13; x ^= x >> 17; x ^= x << 5; m->xs = x; return x; }
static inline float frand(ch_model_t* m) { return (xrand(m) >> 8) * (1.0f / 16777216.0f); }
static inline uint32_t urand(ch_model_t* m) { uint32_t x = m->xs_ui; x ^= x << 13; x ^= x >> 17; x ^= x << 5; m->xs_ui = x; return x; }

// ----------------------------------------------------------------------------
// Loading — incremental, from tick(). tape_read blocks on SD.
// ----------------------------------------------------------------------------
// Pick a fresh source window for `bank` from a random mark and arm the loader.
static void ch_arm_bank(ch_model_t* m, uint8_t bank) {
    tape_t* t = tape_get();
    if (!t) { m->state = CH_NO_TAPE; return; }
    m->n_marks = tape_marks_count(t);
    if (m->n_marks == 0) { m->state = CH_NO_MARKS; return; }

    // Random mark, random window inside it — the bank is a slab of source, and
    // grains later take sub-windows of THAT, which is where the cut-up lives.
    uint32_t s = 0, e = 0, chosen = 0;
    for (uint32_t tries = 0; tries < m->n_marks; tries++) {
        const uint32_t mk = urand(m) % m->n_marks;
        if (tape_marks_get(t, mk, &s, &e) && e > s) { chosen = mk; break; }
        s = e = 0;
    }
    if (e <= s) { m->state = CH_READ_FAIL; return; }
    m->src_mark[bank] = (uint8_t)chosen;

    const uint32_t region = e - s;
    const uint32_t win = region < m->bank_fr ? region : m->bank_fr;
    const uint32_t off = region > win ? (urand(m) % (region - win)) : 0;

    m->bank_ready[bank] = 0;          // audio thread stops reading it now
    m->bank_len[bank] = 0;
    m->src_start[bank] = s + off;
    m->load_bank = bank;
    m->load_done = 0;
    m->load_want = win;
    m->load_src = s + off;
    m->state = CH_LOADING;
}

// Read one chunk of the armed bank. Returns true while more work remains.
static bool ch_load_step(ch_model_t* m) {
    if (m->load_bank >= BANKS || !m->pool) return false;
    tape_t* t = tape_get();
    if (!t) { m->state = CH_NO_TAPE; m->load_bank = BANKS; return false; }

    uint32_t n = m->load_want - m->load_done;
    if (n > CHUNK_FR) n = CHUNK_FR;
    float* dst = m->pool + (size_t)m->load_bank * m->bank_fr * 2u + (size_t)m->load_done * 2u;
    const uint32_t got = tape_read(t, m->load_src, dst, n);
    m->load_done += got;
    m->load_src += got;

    if (got < n || m->load_done >= m->load_want) {
        // Done (or the read short-changed us — keep whatever arrived).
        m->bank_len[m->load_bank] = m->load_done;
        m->bank_ready[m->load_bank] = (m->load_done > 1024u) ? 1 : 0;
        m->load_bank = BANKS;
        // Overall state: ready if ANY bank holds usable audio.
        bool any = false;
        for (int b = 0; b < BANKS; b++) if (m->bank_ready[b]) any = true;
        m->state = any ? CH_READY : CH_READ_FAIL;
        return false;
    }
    return true;
}

// Queue a full reload of every bank (BTN5 hold).
static void ch_reload_all(ch_model_t* m) {
    for (int b = 0; b < BANKS; b++) { m->bank_ready[b] = 0; m->bank_len[b] = 0; }
    for (int v = 0; v < VOICES; v++) m->voices[v].active = false;
    m->rep_left = 0;
    m->hist_n = m->hist_w = m->phrase_pos = 0;   // the remembered grains are gone with the banks
    ch_arm_bank(m, 0);
}

// ----------------------------------------------------------------------------
// Grains
// ----------------------------------------------------------------------------
// Replay a remembered grain. Returns false if it has gone stale (its bank was
// reloaded under us) — the caller then makes a fresh one instead.
static bool ch_play_rec(ch_model_t* m, const grain_rec_t* g) {
    if (!m->bank_ready[g->bank]) return false;
    // A grain of `len` output frames walks len*|inc| SOURCE frames — check that,
    // not len, or a pitched-up grain looks in-bounds when it isn't.
    const float rate = g->inc < 0.f ? -g->inc : g->inc;
    if (g->start + (uint32_t)(g->len * rate) + 4u > m->bank_len[g->bank]) return false;
    voice_t* v = &m->voices[m->vnext];
    m->vnext = (uint8_t)((m->vnext + 1) % VOICES);
    v->bank = g->bank;
    v->inc = g->inc;
    v->idx = (float)g->start + (g->inc < 0.f ? g->len : 0.f);
    v->pos = 0.f;
    v->step = 1.0f / g->len;
    const float pan = (float)g->pan * (1.0f / 255.0f);
    v->panl = 1.0f - pan * 0.7f;
    v->panr = 0.3f + pan * 0.7f;
    v->active = true;
    m->play_bank = g->bank;
    return true;
}

static void ch_trigger(ch_model_t* m) {
    // Scan (don't sample) for a bank that is safe to read.
    int bank = -1;
    const int start = (int)(xrand(m) % BANKS);
    for (int i = 0; i < BANKS; i++) {
        const int b = (start + i) % BANKS;
        if (m->bank_ready[b] && m->bank_len[b] > 512u) { bank = b; break; }
    }
    if (bank < 0) return;

    voice_t* v = &m->voices[m->vnext];
    m->vnext = (uint8_t)((m->vnext + 1) % VOICES);

    const uint32_t blen = m->bank_len[bank];
    // Grain length: `grain` ms, jittered by scatter.
    float len = m->grain.val * (SR / 1000.0f);
    len *= 1.0f + m->scatter.val * (frand(m) - 0.5f);
    if (len < 128.f) len = 128.f;
    if (len > (float)blen * 0.9f) len = (float)blen * 0.9f;

    // Pitch: base + a quantized spread in semitones.
    float semis = m->pitch.val;
    if (m->spread.val > 0.001f) {
        static const int8_t IVAL[6] = { -12, -5, 0, 3, 7, 12 };
        if (frand(m) < m->spread.val) semis += IVAL[xrand(m) % 6];
    }
    const float rate = fast_exp2f(semis * (1.0f / 12.0f));
    const bool rev = frand(m) < m->reverse.val;

    // Start anywhere in the bank that leaves room for the grain.
    // A grain of `len` output frames walks len*rate source frames — the integer
    // cast has to happen after the multiply, not on the rate.
    const uint32_t room = (uint32_t)(len * (rate < 1.f ? 1.f : rate)) + 4u;
    const uint32_t span = (blen > room) ? (blen - room) : 1u;
    const float st = (float)(xrand(m) % span);

    v->bank = (uint8_t)bank;
    v->inc = rev ? -rate : rate;
    v->idx = rev ? st + len : st;
    v->pos = 0.f;
    v->step = 1.0f / len;
    const float pan = frand(m);                    // spread grains across the image
    v->panl = 1.0f - pan * 0.7f;
    v->panr = 0.3f + pan * 0.7f;
    v->active = true;
    m->play_bank = (uint8_t)bank;

    // Remember it. Fresh grains are what the Markov memory later quotes.
    grain_rec_t* g = &m->hist[m->hist_w];
    g->start = (uint32_t)st; g->inc = v->inc; g->len = len;
    g->bank = (uint8_t)bank; g->pan = (uint8_t)(pan * 255.0f);
    m->hist_w = (uint8_t)((m->hist_w + 1) % HIST);
    if (m->hist_n < HIST) m->hist_n++;
    m->phrase_pos = 0;

    // Arm a beat-repeat run: the next few triggers replay THIS grain verbatim.
    if (m->repeat.val > 0.001f && frand(m) < m->repeat.val) {
        m->rep_left = (uint8_t)(1 + (xrand(m) % 6));
        m->rep_idx = v->idx; m->rep_inc = v->inc; m->rep_bank = v->bank; m->rep_len = len;
    }
}

static void ch_trigger_repeat(ch_model_t* m) {
    if (!m->bank_ready[m->rep_bank]) { m->rep_left = 0; return; }
    voice_t* v = &m->voices[m->vnext];
    m->vnext = (uint8_t)((m->vnext + 1) % VOICES);
    v->bank = m->rep_bank; v->inc = m->rep_inc; v->idx = m->rep_idx;
    v->pos = 0.f; v->step = 1.0f / m->rep_len;
    v->panl = v->panr = 1.0f;
    v->active = true;
    m->play_bank = m->rep_bank;
    m->rep_left--;
}

// ----------------------------------------------------------------------------
// Audio
// ----------------------------------------------------------------------------
static void ch_process(engine_t* engine, mixer_t* mix) {
    ch_model_t* m = engine_get_ctx(engine);
    if (!m) return;
    float* out = mixer_get_out(mix);
    uint32_t fs = mixer_get_fs(mix);
    if (!out) return;

    const float spf = SR * 60.0f / (m->bpm.val * 4.0f); // 16th-note step
    const uint32_t bank_fr = m->bank_fr;
    const bool run = m->playing && m->pool;

    for (uint32_t i = 0; i < fs; i += 2) {
        // The clock runs even when there is nothing to play, so the step marker
        // keeps moving and the app never looks hung.
        if ((float)m->samp_in_step >= spf) {
            m->samp_in_step = 0;
            m->step = (uint8_t)((m->step + 1) & (STEPS - 1));
            if (run) {
                if (m->rep_left) {
                    ch_trigger_repeat(m);
                } else if (frand(m) < m->density.val) {
                    /* Markov memory: with probability `memory`, quote ourselves
                     * instead of cutting fresh. The phrase shrinks as the knob
                     * rises — 16 grains of loose motif at the bottom, a locked
                     * 2-grain loop at the top. */
                    const float mem = m->memory.val;
                    uint8_t plen = (uint8_t)(2.0f + (1.0f - mem) * 14.0f);
                    if (plen > m->hist_n) plen = m->hist_n;
                    m->phrase_len = plen;
                    bool played = false;
                    if (plen >= 2 && frand(m) < mem) {
                        const uint8_t back = (uint8_t)(plen - (m->phrase_pos % plen));
                        const uint8_t idx = (uint8_t)((m->hist_w + HIST - back) % HIST);
                        played = ch_play_rec(m, &m->hist[idx]);
                        if (played) m->phrase_pos++;
                    }
                    if (!played) ch_trigger(m);
                }
            }
        }
        m->samp_in_step++;

        float l = 0.f, r = 0.f;
        if (run) {
            for (int vi = 0; vi < VOICES; vi++) {
                voice_t* v = &m->voices[vi];
                if (!v->active) continue;
                // A bank can be pulled out from under us by a reload — drop the
                // voice rather than reading a half-written buffer.
                if (!m->bank_ready[v->bank]) { v->active = false; continue; }
                const int32_t fi = (int32_t)v->idx;
                if (fi < 0 || (uint32_t)(fi + 1) >= m->bank_len[v->bank] || v->pos >= 1.0f) {
                    v->active = false; continue;
                }
                const float* s = m->pool + (size_t)v->bank * bank_fr * 2u + (size_t)fi * 2u;
                const float fr = v->idx - (float)fi;
                // Hann window: no clicks at either edge, whatever the grain length.
                const float w = 0.5f - 0.5f * sinu(v->pos + 0.25f);
                const float sl = s[0] + (s[2] - s[0]) * fr;
                const float sr = s[1] + (s[3] - s[1]) * fr;
                l += sl * w * v->panl;
                r += sr * w * v->panr;
                v->idx += v->inc;
                v->pos += v->step;
            }
        }
        out[i]     = soft_limit(l * 0.7f);
        out[i + 1] = soft_limit(r * 0.7f);
    }
}

static void eng_noop_core(os_core_t* c) { (void)c; }
static void eng_noop(engine_t* e) { (void)e; }

/* active/is_active are how the firmware starts, stops and idles an engine.
 * Without is_active the device cannot tell whether we are making sound, so it
 * parks the audio DMA and may sleep underneath us — always supply both. */
static void ch_eng_active(engine_t* e, bool state) { (void)e; if (g_model) g_model->playing = state; }
static uint_fast8_t ch_eng_is_active(engine_t* e) { (void)e; return (g_model && g_model->playing) ? 1 : 0; }

static const engine_callbacks_t ch_engine = {
    .init = eng_noop_core, .process = ch_process, .defaults = eng_noop, .cleanup = eng_noop_core,
    .active = ch_eng_active, .is_active = ch_eng_is_active,
};

// ----------------------------------------------------------------------------
// Rendering
// ----------------------------------------------------------------------------
static void ch_redraw(gfx_t* gfx, const os_app_t* app) {
    ch_model_t* m = os_app_get_model(app);
    gfx_set_color(gfx, 1);

    gfx_draw_strf(gfx, 8, TOP_BAR + 2, "%s marks:%u BPM %d",
                  m->playing ? ">" : "=", (unsigned)m->n_marks, (int)(m->bpm.val + 0.5f));
    gfx_draw_strf(gfx, 200, TOP_BAR + 2, "ENC %s", ENC_NAME[m->enc_target]);
    if (m->state == CH_LOADING) {
        gfx_draw_str(gfx, 322, TOP_BAR + 2, "reading");
    } else if (m->memory.val > 0.02f && m->phrase_len >= 2) {
        gfx_draw_strf(gfx, 322, TOP_BAR + 2, "loop%u", (unsigned)m->phrase_len);
    }

    // Say exactly what is wrong, rather than drawing empty boxes.
    if (m->state != CH_READY && m->state != CH_LOADING) {
        const char* l1 = "no tape mounted";
        const char* l2 = "";
        if (m->state == CH_NO_MARKS) {
            l1 = "this tape has no marks";
            l2 = "record on the device, then BTN5-hold";
        } else if (m->state == CH_READ_FAIL) {
            l1 = "could not read the marked audio";
            l2 = "BTN5-hold to try again";
        }
        gfx_draw_str(gfx, 30, 130, (char*)l1);
        if (l2[0]) gfx_draw_str(gfx, 30, 152, (char*)l2);
        return;
    }

    // Bank pool: BANKS bars, height = how much source each holds.
    const int bx = 12, by = TOP_BAR + 30, bw = (SCREEN_WIDTH - 24) / BANKS, bh = 64;
    for (int i = 0; i < BANKS; i++) {
        const int x = bx + i * bw;
        gfx_set_color(gfx, 1);
        gfx_draw_rect(gfx, x, by, bw - 6, bh);
        if (m->bank_ready[i] && m->bank_len[i]) {
            const int fill = (int)((float)m->bank_len[i] / (float)m->bank_fr * (bh - 4));
            gfx_fill_rect_dithered(gfx, x + 2, by + bh - 2 - fill, bw - 10, fill,
                                   i == m->play_bank ? 6 : 3);
            // Which recording this bank is chopping — a bank whose mark is a
            // sliver in the strip below is still identifiable here.
            gfx_set_color(gfx, 1);
            gfx_draw_strf(gfx, x + 3, by - 3, "r%u", (unsigned)m->src_mark[i] + 1u);
        } else if (i == m->load_bank) {
            const int fill = (int)((float)m->load_done / (float)(m->load_want ? m->load_want : 1)
                                   * (bh - 4));
            gfx_fill_rect_dithered(gfx, x + 2, by + bh - 2 - fill, bw - 10, fill, 1);
        }
    }
    // ---- tape overview: every recording on the tape, and what we took from it ----
    // Map in POSITIONS, not frames. Frames would overflow uint32 (a 5-minute tape
    // is 14.4M frames, x376 px is 5.4e9) and 64-bit maths would pull in __udivdi3,
    // which the firmware does not export. tape_size() is already positions, and
    // positions*width tops out around 7.6e8 even on a 45-minute tape.
    {
        tape_t* t = tape_get();
        const uint32_t cap = t ? tape_size(t) : 0u;
        const int tx = 12, tw = SCREEN_WIDTH - 24, ty = TOP_BAR + 100, th = 12;
        gfx_set_color(gfx, 1);
        gfx_draw_rect(gfx, tx, ty, tw, th);
        if (cap) {
            // Every mark, so you can see the whole tape's contents at a glance.
            for (uint32_t i = 0; i < m->n_marks; i++) {
                uint32_t ms = 0, me = 0;
                if (!tape_marks_get(t, i, &ms, &me) || me <= ms) continue;
                int x0 = tx + (int)((ms / 64u) * (uint32_t)tw / cap);
                int x1 = tx + (int)((me / 64u) * (uint32_t)tw / cap);
                if (x1 <= x0) x1 = x0 + 2;                 // 2px floor, as the firmware does
                if (x1 > tx + tw) x1 = tx + tw;
                gfx_fill_rect_dithered(gfx, x0, ty + 2, x1 - x0, th - 4, 3);
            }
            // The window each bank is actually chopping, solid, brightest for the
            // bank you can hear right now.
            for (int b = 0; b < BANKS; b++) {
                if (!m->bank_ready[b] || !m->bank_len[b]) continue;
                const uint32_t bs = m->src_start[b] / 64u;
                const uint32_t be = (m->src_start[b] + m->bank_len[b]) / 64u;
                int x0 = tx + (int)(bs * (uint32_t)tw / cap);
                int x1 = tx + (int)(be * (uint32_t)tw / cap);
                if (x1 <= x0) x1 = x0 + 2;
                if (x1 > tx + tw) x1 = tx + tw;
                gfx_fill_rect_dithered(gfx, x0, ty + 1, x1 - x0, th - 2,
                                       b == m->play_bank ? 7 : 5);
                gfx_set_color(gfx, 1);
                gfx_draw_vline(gfx, x0, ty - 2, th + 4);   // tick marking the source
            }
        } else {
            gfx_draw_str(gfx, tx + 4, ty + th - 2, "no tape");
        }
    }

    // Live grain activity.
    const int gy = TOP_BAR + 118;
    for (int vi = 0; vi < VOICES; vi++) {
        if (!m->voices[vi].active) continue;
        const int x = bx + (int)(m->voices[vi].pos * (float)(SCREEN_WIDTH - 24));
        gfx_draw_rect_fill(gfx, x, gy + vi * 3, 4, 2);
    }

    // Step position bar.
    const int sx = 12, sy = 214, sw = SCREEN_WIDTH - 24;
    gfx_set_color(gfx, 1);
    gfx_draw_hline(gfx, sx, sy, sw);
    const int px = sx + (int)((float)m->step / (float)STEPS * (float)sw);
    gfx_draw_rect_fill(gfx, px, sy - 4, 3, 9);
}

// ----------------------------------------------------------------------------
// Encoder / hints
// ----------------------------------------------------------------------------
static void ch_enc_apply(ch_model_t* m, int32_t d) {
    switch (m->enc_target) {
    case ENC_TEMPO:   m->bpm.val     = clampf(m->bpm.val + (float)d, 60.f, 200.f); break;
    case ENC_DENS:    m->density.val = clampf(m->density.val + (float)d * 0.02f, 0.f, 1.f); break;
    case ENC_GRAIN:   m->grain.val   = clampf(m->grain.val + (float)d * 4.f, 10.f, 400.f); break;
    case ENC_PITCH:   m->pitch.val   = clampf(m->pitch.val + (float)d, -12.f, 12.f); break;
    case ENC_SPREAD:  m->spread.val  = clampf(m->spread.val + (float)d * 0.02f, 0.f, 1.f); break;
    case ENC_SCATTER: m->scatter.val = clampf(m->scatter.val + (float)d * 0.02f, 0.f, 1.f); break;
    case ENC_REPEAT:  m->repeat.val  = clampf(m->repeat.val + (float)d * 0.02f, 0.f, 1.f); break;
    case ENC_MEMORY:  m->memory.val  = clampf(m->memory.val + (float)d * 0.02f, 0.f, 1.f); break;
    default: break;
    }
}

static void ch_hints(ch_model_t* m) {
    static tapp_hint_pair_t hints[5];
    hints[0] = (tapp_hint_pair_t){ m->playing ? "stop" : "play", 0 };
    hints[1] = (tapp_hint_pair_t){ "menu", 0 };
    hints[2] = (tapp_hint_pair_t){ "roll", 0 };
    hints[3] = (tapp_hint_pair_t){ "<enc", "exit" };
    hints[4] = (tapp_hint_pair_t){ "enc>", "read" };
    ui_hints_set_labels(hints);
}

// ----------------------------------------------------------------------------
// Tick — the incremental loader lives here; tape_read blocks on SD.
// ----------------------------------------------------------------------------
static bool ch_tick(os_app_t* app) {
    ch_model_t* m = os_app_get_model(app);
    if (m->menu && ui_menu_is_visible(m->menu)) {
        // menu owns the encoder
    } else {
        const int32_t d = os_controls_encoder_get_delta();
        if (d) ch_enc_apply(m, d);
    }

    // Two chunks per tick: enough to fill a bank in ~4 ticks without stalling.
    for (int i = 0; i < 2; i++) {
        if (m->load_bank >= BANKS) {
            // Chain into the next unloaded bank, if any.
            uint8_t next = BANKS;
            for (uint8_t b = 0; b < BANKS; b++) if (!m->bank_ready[b]) { next = b; break; }
            if (next >= BANKS) break;
            ch_arm_bank(m, next);
            if (m->load_bank >= BANKS) break;   // arming failed (no tape/marks)
        }
        if (!ch_load_step(m)) break;
    }
    return true;
}

// ----------------------------------------------------------------------------
// Input
// ----------------------------------------------------------------------------
static void ch_input(os_app_t* app, uint8_t btn, KeyStateEnum st) {
    ch_model_t* m = os_app_get_model(app);
    if (m->menu && ui_menu_is_visible(m->menu)) { ui_menu_input(m->menu, btn, st); return; }

    if (st == KEY_STATE_PRESSED) {
        switch (btn) {
        case 0: m->playing = !m->playing; ch_hints(m); break;
        case 1: if (m->menu) ui_menu_show(m->menu); break;
        case 2:  // re-roll grains: new source positions, no SD access
            for (int v = 0; v < VOICES; v++) m->voices[v].active = false;
            m->rep_left = 0;
            m->xs ^= 0x5bf03635u;
            break;
        case 3: m->enc_target = (uint8_t)((m->enc_target + ENC_TOTAL - 1) % ENC_TOTAL); break;
        case 4: m->enc_target = (uint8_t)((m->enc_target + 1) % ENC_TOTAL); break;
        default: break;
        }
    } else if (st == KEY_STATE_HOLD) {
        if (btn == 3) os_app_exit();
        if (btn == 4) ch_reload_all(m);   // re-read fresh regions from the tape
    }
}

// ----------------------------------------------------------------------------
// Settings menu
// ----------------------------------------------------------------------------
static void ch_menu_build(ui_menu_t* menu) {
    ch_model_t* m = ui_menu_ctx(menu);
    if (!m) return;
    ui_menu_add(menu, UI_PARAM("BPM",     MenuNodeParam, &m->bpm));
    ui_menu_add(menu, UI_PARAM("density", MenuNodeParam, &m->density));
    ui_menu_add(menu, UI_PARAM("grain",   MenuNodeParam, &m->grain));
    ui_menu_add(menu, UI_PARAM("pitch",   MenuNodeParam, &m->pitch));
    ui_menu_add(menu, UI_PARAM("spread",  MenuNodeParam, &m->spread));
    ui_menu_add(menu, UI_PARAM("reverse", MenuNodeParam, &m->reverse));
    ui_menu_add(menu, UI_PARAM("scatter", MenuNodeParam, &m->scatter));
    ui_menu_add(menu, UI_PARAM("repeat",  MenuNodeParam, &m->repeat));
    ui_menu_add(menu, UI_PARAM("memory",  MenuNodeParam, &m->memory));
}

// ----------------------------------------------------------------------------
// Param helpers
// ----------------------------------------------------------------------------
static void mk_param(params_t* p, const char* name, float val, float mn, float mx, float coarse) {
    memset(p, 0, sizeof(*p));
    *(const char**)&p->name = name;
    *(ParamTypeUI_t*)&p->type = ParamValExactType;
    /* refresh gates param_update_fast(); without it val never follows target and
     * the value looks frozen while you turn the encoder in the menu. */
    *(bool*)&p->refresh = true;
    p->val = val; p->min = mn; p->max = mx; p->dflt = val; p->coarse = coarse; p->fine = coarse;
}

// ----------------------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------------------
static bool ch_init(os_app_t* app, va_list args) {
    (void)args;
    ch_model_t* m = os_app_get_model(app);
    g_model = m; // audio callbacks reach the model through this

    m->xs = 0xC0FFEEu;
    m->xs_ui = 0x1234567u;
    m->load_bank = BANKS;                 // idle

    // 1 MB of source if it fits, otherwise a quarter of that. os_malloc is
    // first-fit across DTCM -> OS_RAM -> PSRAM, and PSRAM is fine for read-only
    // audio playback from the audio task (gmplayer does exactly this).
    m->pool = (float*)os_malloc(POOL_FL * sizeof(float));
    m->bank_fr = BANK_FR;
    if (!m->pool) {
        m->pool = (float*)os_malloc(POOL_FL_MIN * sizeof(float));
        m->bank_fr = 8192u;
    }
    if (!m->pool) return false;

    mk_param(&m->bpm,     "BPM",     120.f, 60.f,  200.f, 1.f);
    mk_param(&m->density, "density", 0.65f, 0.f,   1.f,   0.05f);
    mk_param(&m->grain,   "grain",   90.f,  10.f,  400.f, 5.f);   // ms
    mk_param(&m->pitch,   "pitch",   0.f,   -12.f, 12.f,  1.f);   // semitones
    mk_param(&m->spread,  "spread",  0.35f, 0.f,   1.f,   0.05f);
    mk_param(&m->reverse, "reverse", 0.25f, 0.f,   1.f,   0.05f);
    mk_param(&m->scatter, "scatter", 0.30f, 0.f,   1.f,   0.05f);
    mk_param(&m->repeat,  "repeat",  0.15f, 0.f,   1.f,   0.05f);
    mk_param(&m->memory,  "memory",  0.f,   0.f,   1.f,   0.05f);

    m->menu = ui_menu_create(m, ch_menu_build);
    /* Wider than the 165px default: the value is drawn right-anchored to the
     * menu width, so narrow panels run the value into the entry name. */
    ui_menu_set_width(m->menu, 230);
    m->enc_target = ENC_TEMPO;
    m->playing = true;

    // Arm the first bank; tick() fills this one and chains through the rest.
    ch_arm_bank(m, 0);

    ui_statusbar_show(true);
    ch_hints(m);
    ui_hints_show(true);

    // Install OUR engine before activating it — see the note in infinigroove.c.
    engine_set_callbacks(app, m);
    engine_set_active(true);
    return true;
}

static bool ch_deinit(os_app_t* app) {
    ch_model_t* m = os_app_get_model(app);
    g_model = 0;
    m->playing = false;
    for (int b = 0; b < BANKS; b++) m->bank_ready[b] = 0;
    engine_set_active(false);
    engine_clear_callbacks(&ch_engine);
    if (m->pool) { os_free(m->pool); m->pool = 0; }
    if (m->menu) { ui_menu_destroy(m->menu); m->menu = 0; }
    return true;
}

static os_app_data_t ch_data = {
    .model = NULL, .model_size = sizeof(ch_model_t), .init = ch_init, .deinit = ch_deinit,
};

static os_app_t ch_app = {
    .name = "Chopper",
    .type = AppFullscreenType,
    .data = &ch_data,
    .redraw = (os_app_redraw)ch_redraw,
    .tick = ch_tick,
    .on_input = ch_input,
    .engine_cb = &ch_engine,
};

os_app_t* tapp_get_descriptor(void) { return &ch_app; }

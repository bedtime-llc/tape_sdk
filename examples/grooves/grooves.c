// grooves.c — generative 6-track groovebox SOURCE tapp: renders at 24k into its own
// scratch, upsamples into the mixer in_bus with ups2x_f32_add(), so the tapedecks
// record it like the radio. (The firmware does no rate conversion — a source tapp
// always gets the real 48k bus and owns its own internal rate.)
// Controls: BTN1=play/stop (custom glyph hint)   BTN2=shuffle   BTN3=param adjust on/off (all params)
//           BTN4/BTN5 press = enc target back/fwd (hold = system back/exit)
//           ENC drives the selected target: morph drift tempo dens tone key scale
//                                           tracks pump.
// With the tapedecks' WET switch on it also PROCESSES the live input: dec2x_f32
// brings it to 24k, the drum-driven sidechain ducks it (and the bass/pad) by the
// `pump` param

#include "tapp_api.h"

TAPP_DECLARE_TYPE(AppTypeSrc);

// ----------------------------------------------------------------------------
// Config
// ----------------------------------------------------------------------------
#define SR          24000.0f
/* 24k render scratch: half the 48k block, with headroom over the shipping block
 * size. n24 is clamped to this every block, so a larger block can never overrun. */
#define GB_SCRATCH24 256
#define STEPS       16           // max steps per track
#define GRID        4            // 4x4 morph node grid
#define NODES       (GRID * GRID)
#define TRACKS      7

/* Order is also the ENABLE order: the `tracks` knob brings them in left to right, so the kit
 * assembles drums-first and the bass lands last. */
enum { T_KICK = 0, T_SNARE, T_HAT, T_CLAP, T_METAL, T_GLITCH, T_BASS };

#define INF_SX    96
#define INF_SY    36
#define INF_X     70                            // 70..166 — center-left, clear of metro and params
#define INF_Y     40                            // 50..86, below the hints, above the grid
#define INF_TH     9                            // stroke thickness
#define INF_DOT_R  4                            // travelling marker radius (9px)
#define UI_HALFTONE 4                           // 50% halftone — shared by the figure-8 and metronome
#define INF_PAD   (INF_TH / 2 + 2)
/* Baked figure-8 stencils: band-local 1bpp bitmaps of the stroked path (base =
 * INF_TH, fat = INF_TH+4 for the selected halo). Rendered once at init and
 * blitted per frame — the live version re-stroked ~200 thick segments (600
 * when selected) every single frame. */
#define FIG_W   (INF_SX + 2 * INF_PAD + 1)
#define FIG_H   (INF_SY + 2 * INF_PAD + 1)
#define FIG_BPR ((FIG_W + 7) / 8)

// Encoder targets — BTN4/BTN5 step through these
enum { ENC_MORPH = 0, ENC_DRIFT, ENC_TRACKS, ENC_PUMP, ENC_REL, ENC_DENS, ENC_TONE, ENC_KEY,
    ENC_SCALE, ENC_TEMPO, ENC_TOTAL };

/* One params_t per encoder target, edited the way the main menu edits its rows:
 * the encoder writes the TARGET (param_write_delta_coarse), the tick eases val
 * toward it (param_update_val), the panel draws val. File-scope because the
 * const fields (name/type) need compile-time init. MORPH is the exception —
 * its state stays the raw wrapping uint16 in the model (a closed loop can't
 * clamp), so its slot is only a named placeholder gb_enc_apply skips. */
/* All knob edits go through the FINE writer (param_write_delta_val: target +=
 * fine * detents) — the old coarse steps moved values too fast per detent, and
 * morph in particular visibly hopped. Morph is a REAL param now: val 0..1 is
 * the loop fraction (wraps — see gb_enc_apply), phase = val * 65536. */
static params_t gb_prm[ENC_TOTAL] = {
    [ENC_MORPH] = { .name = "morph", .type = ParamValScaledType, .coarse = 1.f / 128.f,
                    .fine = 1.f / 256.f,
                    .min = 0.f,  .max = 1.f,   .dflt = 0.125f, .val = 0.125f, .target = 0.125f,
                    .refresh = true },
    [ENC_DRIFT] = { .name = "drift", .type = ParamValScaledType, .coarse = 0.02f, .fine = 0.01f,
                    .min = 0.f,  .max = 1.f,   .dflt = 0.f,   .val = 0.f,   .target = 0.f,   .refresh = true },
    [ENC_TRACKS]= { .name = "tracks", .type = ParamValExactType, .coarse = .01f, .fine = 0.01f,
        .min = 1.f,  .max = (float)TRACKS,
        .dflt = (float)TRACKS, .val = (float)TRACKS, .target = (float)TRACKS,
        .refresh = true },
    [ENC_PUMP]  = { .name = "pump",  .type = ParamValScaledType, .coarse = 0.02f, .fine = 0.01f,
        .min = 0.f,  .max = 1.f,   .dflt = 0.75f, .val = 0.75f, .target = 0.75f,
        .refresh = true },
    /* Sidechain RELEASE, split out of `pump` so depth and length are independent — pump used to
     * derive both and you could not have a deep short duck or a shallow long one. 60..600 ms; the
     * 0.333 default is exactly the 240 ms that pump 0.75 used to give, so nothing moves until it
     * is touched. Named "rel" rather than "release": the BTN3 hint cell is narrow and the longest
     * existing name is "tracks" at 6 chars. */
    [ENC_REL]   = { .name = "rel",   .type = ParamValScaledType, .coarse = 0.02f, .fine = 0.01f,
        .min = 0.f,  .max = 1.f,   .dflt = 0.333f, .val = 0.333f, .target = 0.333f,
        .refresh = true },
    [ENC_DENS]  = { .name = "dens",  .type = ParamValScaledType, .coarse = 0.02f, .fine = 0.01f,
        .min = 0.f,  .max = 1.f,   .dflt = 0.85f, .val = 0.85f, .target = 0.85f, .refresh = true },
    [ENC_TONE]  = { .name = "tone",  .type = ParamValScaledType, .coarse = 0.02f, .fine = 0.01f,
        .min = 0.f,  .max = 1.f,   .dflt = 0.5f,  .val = 0.5f,  .target = 0.5f,  .refresh = true },
    [ENC_KEY]   = { .name = "key",   .type = ParamValExactType,  .coarse = 1.f, .fine = .02f,
        .min = 24.f, .max = 48.f,  .dflt = 33.f,  .val = 33.f,  .target = 33.f,  .refresh = true },
    [ENC_SCALE] = { .name = "scale", .type = ParamValSelectType, .coarse = 1.f, .fine = .02f,
        .min = 0.f,  .max = 3.f,   .dflt = 0.f,   .val = 0.f,   .target = 0.f,   .refresh = true,
        .text_opt = true, .option_name = { "minor", "dorian", "phrygian", "major" } },
    [ENC_TEMPO] = { .name = "tempo", .type = ParamValExactType,  .coarse = 1.f, .fine = 0.5f,
                    .min = 60.f, .max = 200.f, .dflt = 112.f, .val = 112.f, .target = 112.f, .refresh = true },
};


#define HZ2INC(f)  ((uint32_t)((f) * 178956.97f))   /* f * 2^32 / SR */
#define I32_TO_F   (1.0f / 2147483648.0f)

static inline float fast_exp2f(float x) {
    int xi = (int)x; float xf = x - xi;
    if (x < 0) { xi--; xf += 1.0f; }
    float p = 1.0f + xf * (0.6931472f + xf * (0.2402265f + xf * 0.0558f));
    union { float f; uint32_t i; } u = { .f = p };
    u.i += (uint32_t)xi << 23;
    return u.f;
}


static inline float fsin_i32(int32_t ph) {
    const float u = (float)ph * I32_TO_F;            /* -1..1 == theta/pi */
    const float y = u * (4.0f - 4.0f * fabsf(u));
    return y * (0.775f + 0.225f * fabsf(y));
}

static inline float saw_u32(uint32_t ph) { return (float)ph * (2.0f / 4294967296.0f) - 1.0f; }

static inline float sat1(float x) {
    x = fminf(fmaxf(x, -1.5f), 1.5f);
    return x * (1.0f - 0.14814815f * x * x);         /* x - x^3/6.75 */
}

#define SVF(in, lp, bp, f, q) do {                   \
    const float _hp = (in) - (lp) - (bp) * (q);      \
    (bp) += (f) * _hp;                               \
    (lp) += (f) * (bp);                              \
} while (0)
static inline float svf_f(float hz) {                /* 2*sin(pi*fc/SR), no sinf */
    return fminf(2.0f * fsin_i32((int32_t)(uint32_t)(hz * 89478.49f)), 1.2f);
}

static inline float blep_u32(uint32_t ph, uint32_t inc, float rinc) {
    if (ph < inc) { const float t = (float)ph * rinc; return t * (2.0f - t) - 1.0f; }
    const uint32_t d = 0u - ph;
    if (d < inc)  { const float u = 1.0f - (float)d * rinc; return u * u; }
    return 0.0f;
}

static inline float decay_coef(float ms) { return 1.0f - 1.0f / (fmaxf(ms, 0.05f) * (SR / 1000.0f)); }

#define SC_FLOOR 0.06f
static inline float dnz(float x) { return (x < 1e-15f && x > -1e-15f) ? 0.0f : x; }
static float note_to_freq(float midi) { return 440.0f * fast_exp2f((midi - 69.0f) * (1.0f / 12.0f)); }

static const int8_t SCALE[4][7] = {
    { 0, 2, 3, 5, 7, 8, 10 },   // natural minor
    { 0, 2, 3, 5, 7, 9, 10 },   // dorian
    { 0, 1, 3, 5, 7, 8, 10 },   // phrygian
    { 0, 2, 4, 5, 7, 9, 11 },   // major
};
static const char* const NOTE_NAME[12] = { "C",  "C#", "D",  "D#", "E",  "F",
                                           "F#", "G",  "G#", "A",  "A#", "B" };

static inline int deg2semi(int sc, int deg) {
    int oct = deg / 7, d = deg % 7;
    if (d < 0) { d += 7; oct--; }
    return SCALE[sc][d] + oct * 12;
}

#define MIX_POLY    0.35f
#define MIX_SWING   0.10f
#define BUS_TRIM    0.80f

/* Morph automation: one motion lane, recorded by holding BTN3 and looped on release.
 * ONE SAMPLE PER UI TICK, no timestamps — the controls poll only wakes early on BTN1-BTN5
 * edges (os_system.c pin-scopes the EXTI wake on purpose, so a leaked encoder line cannot
 * "collapse the poll cadence to per-edge"), so turning the knob does NOT shorten tick():
 * record and replay run at the same steady ~40ms and the loop keeps the gesture's length. */
enum { AUTO_OFF = 0, AUTO_REC, AUTO_PLAY };
#define AUTO_MAX    512                     /* ~20 s at the 40 ms tick; 1 KB in the model */

static const float LVL[TRACKS]       = { 0.95f, 0.50f, 0.20f, 0.42f, 0.30f, 0.24f, 0.62f };
static const float DENS_DFLT[TRACKS] = { 0.85f, 0.55f, 0.70f, 0.42f, 0.48f, 0.38f, 0.62f };
static const float HAT_RATIO[6] = { 1.0f, 1.4471f, 1.6170f, 1.9265f, 2.5028f, 2.6637f };

// ----------------------------------------------------------------------------
// Model
// ----------------------------------------------------------------------------
typedef struct { uint16_t n[4]; float w[4]; } morph_w_t;

typedef struct {
    volatile uint8_t step;                  // global 16th step 0..15
    volatile uint8_t tstep[TRACKS];         // per-track step (polymeter)
    uint32_t samp_left, frac_q8;            // integer step clock + Q8 remainder
    uint32_t sub_left, sub_len;             // ratchet sub-clock
    uint8_t  ratchet_sub, last_fire;        // last_fire: bit per track, for ratchets
    uint8_t  ratchets_cur;                  // this step's count — param, or a morph one-shot
    int8_t   ratchet_dir;                   // +/-1: does this roll pitch up or down
    uint32_t xs;                            // xorshift state, audio thread
    uint32_t xs_ui;                         // xorshift state, ui thread (separate: no race)

    volatile bool playing;                  // written from both contexts
    /* Morph position = gb_prm[ENC_MORPH].val * 65536 + morph_drift (mod 65536 —
     * the path is a closed loop, wraps). Each context is the SOLE writer of its
     * half: UI (encoder param) owns the param, audio (drift, clock_next) owns
     * morph_drift — a shared read-modify-write here once let the audio thread
     * silently revert encoder detents. See morph_phase(). */
    uint16_t morph_drift;                   // audio-owned drift accumulator
    uint8_t  enc_target;
    bool     adj_off;               // BTN3 tap: the knob leaves EVERY param alone (bar greyed)
    uint8_t pat [2][TRACKS][NODES][STEPS];  // 0..255 intensity
    int8_t  tune[2][TRACKS][NODES][STEPS];  // drums: character  pitched: scale degree
    uint8_t plen[2][TRACKS];                // polymeter loop length
    /* Per-NODE drum voice character, morph-interpolated like everything else. Separate
     * from tune[] because tune is per-STEP articulation (accent vs ghost) and deliberately
     * couples pitch to decay the way a beater does. These are the two axes that must move
     * INDEPENDENTLY, so a region of the grid can be low AND long — an 808 boom — which a
     * single coupled axis cannot express. */
    int8_t  npit[2][TRACKS][NODES];         // base pitch
    int8_t  ndec[2][TRACKS][NODES];         // decay length
    volatile uint8_t live;
    volatile uint8_t regen_req;
    uint32_t regen_seed;

    // synth voices ---------------------------------------------------------
    uint32_t k_ph;  float k_f, k_f0, k_env, k_click, k_dec, k_drv;
    uint32_t s_ph1, s_ph2, s_inc1, s_inc2;
    float    s_nenv, s_tenv, s_tr, s_ndec, s_lp, s_bp, s_bpf, s_nmix, s_fd, s_q;
    uint32_t h_ph[6], h_inc[6];  float h_env, h_dec, h_lp, h_bp, h_f, h_fd, h_q;
    uint32_t b_ph, b_sph, b_inc, b_sinc;
    float    b_rinc, b_env, b_fenv, b_amp, b_lp, b_bp, b_q, b_fd, b_dec, b_sub;
    /* CLAP/TOM — a burst train (the clap) over a short pitched body (the tom). `c` morphs between
     * the two, so one voice covers hand-clap and rototom without a second set of state. */
    uint32_t cl_ph, cl_inc;
    float    cl_env, cl_dec, cl_bpf, cl_lp, cl_bp, cl_benv, cl_bdec, cl_mix;
    uint32_t cl_gap, cl_t;                  // burst spacing and countdown
    uint8_t  cl_n;                          // bursts left in the train
    /* FM METAL — two-op FM through a bandpass. Kept SMOOTH: a modest, non-integer ratio and an
     * index that decays, so it rings rather than clangs. */
    uint32_t mt_pc, mt_pm, mt_incc, mt_incm;
    float    mt_env, mt_dec, mt_idx, mt_idec, mt_f, mt_lp, mt_bp;
    /* GLITCH — sample-and-hold noise (a rate crush) through a resonant bandpass. PLEASANT: the
     * hold rate is tuned to the voice's pitch, so the aliasing lands on harmonics, not hash. */
    uint32_t gl_hold, gl_t;
    float    gl_env, gl_dec, gl_val, gl_f, gl_lp, gl_bp, gl_q;

    float   dcx_l, dcy_l, dcx_r, dcy_r;
    float   dens[TRACKS];                   // per-track density — the dens PARAM is a macro over these
    float   dens_prev;                      // last dens knob value, for the delta below

    /* Morph motion lane. Zeroed with the model at every launch, so AUTO_OFF + empty is the
     * free initial state. Records/replays the UI-owned half of the morph position ONLY —
     * morph_drift stays the audio thread's, so `drift` keeps advancing on top of a playing
     * lane exactly as it does on top of the knob. */
    uint8_t  auto_state;                    // AUTO_OFF / AUTO_REC / AUTO_PLAY
    uint8_t  auto_moved;                    // the knob actually moved during this take
    uint16_t auto_len, auto_idx;
    uint16_t auto_buf[AUTO_MAX];            // morph position, phase units (val * 65536)

    uint8_t fig_base[FIG_H * FIG_BPR];      // baked INF_TH path stroke (band-local bits)
    uint8_t fig_fat[FIG_H * FIG_BPR];       // baked INF_TH+4 stroke (selected halo)
    uint16_t arc_lut[256];                  // uniform arc fraction -> raw lemniscate t (gb_init)
    ui_roll_text_t enc_roll;                // mechanical roll for the enc-target name
    ui_roll_text_t val_roll;                // mechanical roll for the key/scale VALUE text
    char key_txt[8];                        // persistent note-name text the value roll diffs against
    /* The selected value as text, for the hold cell's LABEL. The cell paints itself, but a
     * HintCustom degrades to a plain HintString wherever another band seeds from ours, and
     * there a number is the honest fallback. Model storage: hint labels are REFERENCED. */
    char val_txt[10];
    /* 24k render scratch + its upsampler state. The state MUST persist across
     * blocks (it carries the interpolator seam) — a model field, zeroed once. */
    float scratch24[GB_SCRATCH24];
    ups2x_f32_t ups;
    /* Live input, brought down to our 24k working rate so the drum-driven duck
     * can be applied sample-accurately alongside the groove; the sum goes back
     * up in the single ups2x_f32_add below. Only non-zero when the user has the
     * tapedecks' WET switch on — otherwise the firmware parks the input and
     * hands us a zeroed bus. */
    float in24[GB_SCRATCH24];
    dec2x_f32_t dec;
    /* ONE sidechain envelope shared by pad, bass and the live input. Slammed by
     * the drum triggers, released slower than the kick's own amplitude decay —
     * that longer tail is what reads as a pump rather than a click. */
    float sc_env, sc_dec;
} gb_model_t;

// ----------------------------------------------------------------------------
// RNG
// ----------------------------------------------------------------------------
static inline uint32_t xrand(gb_model_t* m) { // audio thread
    uint32_t x = m->xs; x ^= x << 13; x ^= x >> 17; x ^= x << 5; m->xs = x; return x;
}
static inline float frand(gb_model_t* m) { return (xrand(m) >> 8) * (1.0f / 16777216.0f); }
static inline float nrand(gb_model_t* m) { return (float)(xrand(m) >> 8) * (2.0f / 16777216.0f) - 1.0f; }
static inline uint32_t urand(gb_model_t* m) { // ui thread — separate state
    uint32_t x = m->xs_ui; x ^= x << 13; x ^= x >> 17; x ^= x << 5; m->xs_ui = x; return x;
}

// ----------------------------------------------------------------------------
// Pattern generation
// ----------------------------------------------------------------------------
static void euclid(uint8_t* out, int pulses, int rot) {
    for (int i = 0; i < STEPS; i++) out[i] = 0;
    if (pulses <= 0) return;
    if (pulses > STEPS) pulses = STEPS;
    int err = 0;
    for (int i = 0; i < STEPS; i++) {
        err += pulses;
        if (err >= STEPS) { err -= STEPS; out[(i + rot) & (STEPS - 1)] = 1; }
    }
}

static const uint8_t PULSES[TRACKS][GRID] = {
    {  2,  4,  6,  8 },   // kick
    {  2,  2,  3,  5 },   // snare
    {  4,  7, 11, 15 },   // hat
    {  1,  2,  3,  5 },   // clap   - sparse, answers the snare
    {  2,  3,  5,  7 },   // metal  - tuned, carries the line the lead used to
    {  3,  5,  7, 11 },   // glitch - busiest, fills the gaps
    {  3,  5,  8, 11 },   // bass
};
/* Rotations pull the new voices OFF the kick/snare grid so they interlock rather than double. */
static const uint8_t ROT0[TRACKS] = { 0, 4, 0, 6, 2, 3, 0 };


static int8_t drum_tune(gb_model_t* m, int tr, int node, int s) {
    int c;
    if      ((s & 3) == 0) c =  28;                       // downbeat: accent
    else if ((s & 1) == 0) c =   4;                       // 8th: neutral
    else                   c = -30;                       // 16th: ghost
    if (s >= 12)                              c += 10;    // bar-end fill lift
    if (tr == T_SNARE && (s == 4 || s == 12)) c  = 34;    // the backbeat rules
    c += (int)(urand(m) % 13) - 6;
    c += (node / GRID) * 3 - 4;                           // syncopated rows sit darker
    return (int8_t)(c < -64 ? -64 : c > 63 ? 63 : c);
}

static void gen_node(gb_model_t* m, int buf, int tr, int node) {
    const int gx = node % GRID, gy = node / GRID;
    uint8_t hit[STEPS];
    euclid(hit, PULSES[tr][gx], ROT0[tr] + (gy ? (int)(urand(m) & 3) : 0));

    for (int d = 0; d < gy * 2; d++) {
        const int s = (int)(urand(m) & (STEPS - 1));
        if (hit[s]) { hit[s] = 0; hit[(s + 1) & (STEPS - 1)] = 1; }
    }

    m->npit[buf][tr][node] = (int8_t)((gy - 1) * -34 + (int)(urand(m) % 25) - 12);
    m->ndec[buf][tr][node] = (int8_t)((gx - 1) * 34 + (int)(urand(m) % 25) - 12);

    uint8_t* pd = m->pat[buf][tr][node];
    int8_t*  td = m->tune[buf][tr][node];

    const int lo = -5;
    const int hi =  7;
    int deg = lo + (int)(urand(m) % (uint32_t)(hi - lo + 1));

    for (int s = 0; s < STEPS; s++) {
        if (!hit[s])            pd[s] = 0;
        else if ((s & 3) == 0)  pd[s] = 255;                              // on the beat
        else if (s & 1)         pd[s] = 120 + (uint8_t)(urand(m) % 60);   // ghost
        else                    pd[s] = 200;

        /* Clap is an untuned drum, so it takes the beater character like the other three. Metal,
         * glitch and bass are tuned and walk a scale degree instead. */
        if (tr <= T_CLAP) {
            td[s] = drum_tune(m, tr, node, s);
        } else {
            const uint32_t r = urand(m);
            deg += (int)(r % 7) - 3;
            if (deg < lo) deg = lo; else if (deg > hi) deg = hi;
            const bool lift = (((r >> 16) & 7) == 0) && tr != T_BASS;
            td[s] = (int8_t)(deg + (lift ? 7 : 0));
        }
    }
    if (tr == T_BASS) { pd[0] = 255; td[0] = 0; }
}

static const uint8_t POLY_LEN[4] = { 8, 12, 14, 16 };
static void gen_lens(gb_model_t* m, int buf) {
    const uint32_t odds = (uint32_t)(MIX_POLY * 255.0f);
    for (int t = 0; t < TRACKS; t++)
        m->plen[buf][t] = ((urand(m) & 0xFF) < odds) ? POLY_LEN[urand(m) & 3] : STEPS;
    m->plen[buf][T_KICK] = STEPS;
}

static void relen(gb_model_t* m) {
    const int buf = m->live;
    const uint32_t odds = (uint32_t)(MIX_POLY * 255.0f);
    for (int t = 0; t < TRACKS; t++)
        m->plen[buf][t] = ((xrand(m) & 0xFF) < odds) ? POLY_LEN[xrand(m) & 3] : STEPS;
    m->plen[buf][T_KICK] = STEPS;   // the kick keeps the bar honest
}

static void gb_regen(gb_model_t* m, uint32_t seed) {
    const int buf = m->live ^ 1;
    m->xs_ui = seed ? seed : 0x9e3779b9u;
    for (int t = 0; t < TRACKS; t++)
        for (int n = 0; n < NODES; n++) gen_node(m, buf, t, n);
    gen_lens(m, buf);
    m->regen_req = 1;
}

// ----------------------------------------------------------------------------
// Morph: figure-8 over the node grid
// ----------------------------------------------------------------------------
/* RAW Bernoulli-lemniscate parametrization. Its speed along the curve is NOT
 * uniform: the derivative vanishes at the lobe tips and peaks at the crossing,
 * so equal phase steps move the point sub-pixel in one place and several px in
 * another — the knob felt "stuck then jumping". Every consumer goes through
 * morph_arc() below, which re-maps phase by ARC LENGTH; only the init-time
 * LUT/path bakes sample this directly. */
static void morph_xy_raw(uint16_t morph, float* fx, float* fy) {
    const uint32_t t = (uint32_t)morph << 16;
    const float s = fsin_i32((int32_t)t);
    const float c = fsin_i32((int32_t)(t + 0x40000000u));   // cos
    const float d = 1.0f / (1.0f + s * s);
    *fx = 0.5f + 0.5f * c * d;
    *fy = 0.5f + 1.4142136f * s * c * d;
}

/* The one morph-position read: param val (UI-owned loop fraction) + the
 * audio-owned drift accumulator, wrapped into the uint16 phase domain. */
static uint16_t morph_phase(const gb_model_t* m) {
    /* val can sit just OUTSIDE 0..1 for a tick around a seam crossing (see
     * gb_enc_apply) — go through int32 so the cast wraps instead of saturating. */
    return (uint16_t)((int32_t)(gb_prm[ENC_MORPH].val * 65536.0f) + (int32_t)m->morph_drift);
}

/* Uniform phase -> raw parameter, via the arc-length LUT baked in gb_init.
 * One knob detent = the same distance ANYWHERE on the loop. */
static uint16_t morph_arc(const gb_model_t* m, uint16_t phase) {
    const uint32_t idx = phase >> 8;   // 256 buckets, lerp inside
    const uint32_t fr  = phase & 0xFFu;
    const uint32_t t0 = m->arc_lut[idx];
    const uint32_t t1 = (idx == 255) ? (uint32_t)m->arc_lut[0] + 65536u : m->arc_lut[idx + 1];
    return (uint16_t)(t0 + (((t1 - t0) * fr) >> 8));
}

/* ---- baked figure-8 stencils ------------------------------------------- */

/* Stamp a filled disc into a band-local 1bpp stencil (bake-time only). */
static void gb_stamp(uint8_t* s, int cx, int cy, int r) {
    for (int dy = -r; dy <= r; dy++) {
        const int yy = cy + dy;
        if ((unsigned)yy >= FIG_H) continue;
        const int dx = (int)isqrt((uint32_t)(r * r - dy * dy));
        int x0 = cx - dx, x1 = cx + dx;
        if (x0 < 0) x0 = 0;
        if (x1 >= FIG_W) x1 = FIG_W - 1;
        uint8_t* row = &s[yy * FIG_BPR];
        for (int x = x0; x <= x1; x++) row[x >> 3] |= (uint8_t)(1u << (x & 7));
    }
}

/* Stroke the closed path into both stencils: a disc stamped every ~0.5px of
 * loop = a round-capped stroke, same coverage as the old per-frame
 * gfx_draw_line_thick chain. */
static void gb_bake_fig(gb_model_t* m) {
    memset(m->fig_base, 0, sizeof(m->fig_base));
    memset(m->fig_fat, 0, sizeof(m->fig_fat));
    for (int i = 0; i < 512; i++) {
        float nx, ny; morph_xy_raw((uint16_t)(i * 128), &nx, &ny);
        const int px = INF_PAD + (int)(nx * INF_SX + 0.5f);
        const int py = INF_PAD + (int)(ny * INF_SY + 0.5f);
        gb_stamp(m->fig_base, px, py, INF_TH / 2);
        gb_stamp(m->fig_fat, px, py, (INF_TH + 4) / 2);
    }
}

/* Stencil blit: the baked figures are XBM bitmaps (LSB first, rows byte-padded), and a draw
 * callback starts with TRANSPARENT blits — only the SET bits paint, in the current colour. */
static void gb_blit(gfx_t* gfx, const uint8_t* s) {
    gfx_draw_xbm(gfx, INF_X - INF_PAD, INF_Y - INF_PAD, FIG_W, FIG_H, s);
}

static void morph_weights(const gb_model_t* m, uint16_t morph, morph_w_t* w) {
    float nx, ny; morph_xy_raw(morph_arc(m, morph), &nx, &ny);
    const float fx = nx * (GRID - 1), fy = ny * (GRID - 1);
    int x0 = (int)fx; if (x0 > GRID - 2) x0 = GRID - 2;
    int y0 = (int)fy; if (y0 > GRID - 2) y0 = GRID - 2;
    const float ax = fx - (float)x0, ay = fy - (float)y0;
    const float bx = 1.0f - ax,      by = 1.0f - ay;
    w->n[0] = (uint16_t)(y0 * GRID + x0); w->w[0] = bx * by;
    w->n[1] = (uint16_t)(w->n[0] + 1);    w->w[1] = ax * by;
    w->n[2] = (uint16_t)(w->n[0] + GRID); w->w[2] = bx * ay;
    w->n[3] = (uint16_t)(w->n[2] + 1);    w->w[3] = ax * ay;
}

/* ---- morph as a modulator of dens/tone ------------------------------------------------------
 * The figure-8 position offsets the two knobs by +/-15% of their range, so ONE knob moves the
 * arrangement: the pattern breathes denser and thinner, the kit retunes, as morph travels. The
 * knob stays the centre the deviation is measured from.
 *
 * Different axes on purpose — x drives density, y drives tone — so the two do not move in lockstep
 * and the lobes of the 8 are not simply "more of everything".
 *
 * Both MUST be read through these, never from gb_prm directly: tone is read independently by
 * fire_step and gb_process, and dens by fire_step, steps_to_next and gb_redraw. They agree today
 * only because they happen to read the same field; with a morph term they would disagree, and the
 * drawn grid would stop matching what is heard. */
#define MORPH_MOD 0.30f          /* +/-0.15 around the knob. NOTE for tone: tone_semis scales by
                                  * 48, so this is +/-7.2 SEMITONES of drum transpose and, since
                                  * tone drives the varispeed, a 1.5x swing in drum length too.
                                  * 0.12f would give a more conservative +/-3 semitones. */

static void morph_mod_xy(const gb_model_t* m, float* dx, float* dy) {
    float nx, ny; morph_xy_raw(morph_arc(m, morph_phase(m)), &nx, &ny);
    *dx = (nx - 0.5f) * MORPH_MOD;
    *dy = (ny - 0.5f) * MORPH_MOD;
}

/* PER-VOICE tone. Each voice reads the figure-8 at its own rotation, so morph does not transpose
 * the kit as a block — the voices drift against each other, one going up while another goes down,
 * and the kit keeps re-tuning itself as the knob travels. Spread over HALF the loop because fy
 * completes two periods per lap, so half a lap is one full sweep of the offset and the seven
 * voices land on seven genuinely different points.
 *
 * Everything downstream follows automatically: tone_stretch and tone_dsc are called with the
 * voice's own value, so a voice tuned lower also rings longer and darkens further. */
static float gb_tone_tr(const gb_model_t* m, int tr) {
    const uint16_t ph = (uint16_t)(morph_phase(m) + (uint16_t)(tr * (32768 / TRACKS)));
    float nx, ny; morph_xy_raw(morph_arc(m, ph), &nx, &ny);
    (void)nx;
    return clampf(gb_prm[ENC_TONE].val + (ny - 0.5f) * MORPH_MOD, 0.f, 1.f);
}

static float gb_dens(const gb_model_t* m, int tr) {
    float dx, dy; morph_mod_xy(m, &dx, &dy);
    (void)dy;
    return clampf(m->dens[tr] + dx, 0.f, 1.f);
}

static void step_cell(const gb_model_t* m, const morph_w_t* w, int tr, int st,
                      float* inten, float* tune) {
    const int buf = m->live;
    float si = 0.0f, sT = 0.0f, best = -1.0f;
    int   bestv = 0;
    for (int k = 0; k < 4; k++) {
        const float iv = (float)m->pat[buf][tr][w->n[k]][st] * w->w[k];
        si += iv;
        sT += iv * (float)m->tune[buf][tr][w->n[k]][st];
        if (iv > best) { best = iv; bestv = m->tune[buf][tr][w->n[k]][st]; }
    }
    *inten = si * (1.0f / 255.0f);
    if (tr >= T_METAL) *tune = (float)bestv;   /* tuned voices: the raw degree, unsmoothed */
    else              *tune = (si > 0.5f) ? (sT / si) : 0.0f;
}

static int steps_to_next(const gb_model_t* m, const morph_w_t* w, int tr, int from) {
    const int   L   = m->plen[m->live][tr] ? m->plen[m->live][tr] : STEPS;
    const float thr = 1.0f - gb_dens(m, tr);   /* loop-invariant — morph does not move mid-scan */
    for (int g = 1; g <= L; g++) {
        float inten, tn;
        step_cell(m, w, tr, (from + g) % L, &inten, &tn);
        if (inten > thr) return g;
    }
    return L;   /* nothing else fires this loop — ring for the whole thing */
}

static float node_char(const int8_t tab[TRACKS][NODES], const morph_w_t* w, int tr) {
    float s = 0.f;
    for (int k = 0; k < 4; k++) s += (float)tab[tr][w->n[k]] * w->w[k];
    return clampf(s * (1.0f / 48.0f), -1.f, 1.f);
}

static inline float drum_pitch(float base_hz, float semis, float jitter) {
    const int st = (int)(semis + (semis < 0.f ? -0.5f : 0.5f));
    return base_hz * fast_exp2f((float)st * (1.0f / 12.0f)) * jitter;
}

static inline float tone_semis(float tone) { return (tone - 0.5f) * 48.0f; }

static inline float drum_octave(gb_model_t* m, float bias, uint32_t odds) {
    if ((xrand(m) >> 24) >= odds) return 0.0f;
    return ((frand(m) < bias) ? -12.0f : 12.0f);
}

/* ---- `tone` as VARISPEED ---------------------------------------------------------------------
 * A sampler pitches down by reading slower, so a hit two octaves down is four times as LONG and its
 * whole spectrum descends with it. These drums had absolute-ms decays and additive-in-tone filters,
 * so the knob moved pitch and nothing else. That is the difference between "sampled down" and
 * "detuned", and no amount of harmonic help substitutes for it.
 *
 * Both are unity for tone >= 0.5, so the upper half of the knob is bit-identical. Keyed on the KNOB,
 * not on per-hit pitch: keying on pitch would also catch drum_octave's random hits and make ~1 in 8
 * of them twice as long as their neighbours at the DEFAULT setting. */
static inline float tone_stretch(float tone) {          /* time  x1 .. x4 */
    return fmaxf(1.0f, fast_exp2f(-tone_semis(tone) * (1.0f / 12.0f)));
}
static inline float tone_dsc(float tone) {              /* cutoff x1 .. x0.25 */
    return fminf(1.0f, fast_exp2f(tone_semis(tone) * (1.0f / 12.0f)));
}
/* Ceiling for a stretched tail, relative to the grid rather than a fixed wall: unbounded, a
 * max-settings kick reaches 2 s, which is 27 steps at 200 BPM. 803 ms at 112, 450 ms at 200. */
static inline float drum_cap(void) {
    return fminf(900.0f, (15000.0f / fmaxf(gb_prm[ENC_TEMPO].val, 1.0f)) * 6.0f);
}

// ----------------------------------------------------------------------------
// Voice triggers
// ----------------------------------------------------------------------------
static void trig_kick(gb_model_t* m, float vel, float c, float tone, float np, float nd) {
    const float j = 1.0f + (frand(m) - 0.5f) * 0.03f;
    m->k_f0    = drum_pitch(42.0f, np * 8.0f + c * 7.0f + tone_semis(tone)
                                  + drum_octave(m, 0.85f, 40u), j);
    m->k_f     = m->k_f0 * (3.2f + 1.6f * c);
    /* How far below centre the KNOB is — deliberately NOT derived from k_f0. Keying off the pitch
     * also catches the random octave-down kicks, which sounds like a good idea until 1 kick in 8
     * comes out brighter than its neighbours at the DEFAULT setting (drum_octave above drops
     * 13% of them an octave whatever `tone` says). The knob is what the player moves; it alone
     * should change the timbre. Exactly 0 for tone >= 0.5, so the upper half is untouched on
     * EVERY hit — keying on k_f0 could not promise even that, since the +/-1.5% jitter `j` puts
     * f0 just under 42 on about half of them. */
    const float lo = clampf((0.5f - tone) * 2.0f, 0.f, 1.f);
    m->k_ph    = 0x40000000u;
    m->k_env   = vel;
    /* Click backed off as the body sinks. It was a constant, so once the fundamental left the
     * audible band it simply became the whole sound. Unity at centre and above. */
    m->k_click = vel * (0.16f + 0.10f * (c + 1.0f)) * (1.0f - 0.55f * lo);
    m->sc_env  = 1.0f;                 // full pump on every kick
    m->k_dec   = decay_coef(clampf((150.0f + 110.0f * c) * (1.0f + nd * 0.8f)
                                   * (0.92f + 0.16f * frand(m)) * tone_stretch(tone),
                                   5.0f, drum_cap()));
    /* Pitch it down like a SAMPLE. A sample survives -2 octaves because it is broadband: every
     * harmonic descends with the fundamental and plenty stays audible. This body is ONE sine, so
     * transposing it left nothing up there — that, not anything about hearing, is why the bottom
     * of the knob was a bare click.
     *
     * sat1 manufactures odd harmonics, and 3f0/5f0 DO descend with f0 (the lowest audible partial
     * runs 42 -> 89 -> 63 -> 44 -> 31 Hz across the knob) while being spaced by f0, so the ear
     * reconstructs the missing fundamental. Octave partials at 2f0/4f0 were tried and are wrong:
     * their weight has to rise as f0 falls, and that rise cancels the transposition exactly —
     * the audible pitch stops descending, and the loudest partial can even jump an octave UP.
     *
     * 2.6 puts the 3rd harmonic near -12 dBc. Not higher: 5.9 squares the sine into a buzzer. */
    m->k_drv   = 1.7f + 0.9f * lo;     /* 1.7 at centre and above, 2.6 at the bottom */
}

static void trig_snare(gb_model_t* m, float vel, float c, float tone, float np, float nd) {
    const float j = 1.0f + (frand(m) - 0.5f) * 0.03f;
    const float f = drum_pitch(180.0f, np * 8.0f + c * 7.0f + tone_semis(tone)
                                      + drum_octave(m, 0.5f, 30u), j);
    m->s_inc1 = HZ2INC(f);  m->s_inc2 = HZ2INC(f * 1.79f);
    m->s_ph1  = 0; m->s_ph2 = 0;
    m->s_nenv = vel;
    /* Partial pump — fmaxf so a snare landing on a kick never LOWERS the duck. */
    m->sc_env = fmaxf(m->sc_env, 0.5f);
    m->s_tenv = vel * (0.85f - 0.35f * c);
    m->s_tr   = vel;
    m->s_ndec = decay_coef(clampf((26.0f + 17.0f * (c + 1.0f)) * (1.0f + nd * 0.7f)
                                  * (0.92f + 0.16f * frand(m))   /* the kick had this; snare did not */
                                  * tone_stretch(tone), 3.0f, drum_cap()));
    /* Cutoff MULTIPLIED by the pitch ratio, not offset by it. The old `+ 900*tone` left the noise
     * band at 1200-3400 Hz while the tonal partials dropped to 45 Hz — the noise half never
     * descended, which is most of why the low kit sounded thin and disconnected from its pitch.
     *
     * Base now also takes `np` (so morph moves the filter the way it already moves pitch and
     * decay — the filters were the one part of the voice the node axes never reached), `vel` (an
     * accent reads BRIGHTER, not merely louder) and a per-hit +/-8% jitter so repeated steps stop
     * being bit-identical. */
    m->s_bpf  = svf_f((1200.0f + 1100.0f * (c + 1.0f) + 900.0f * tone)
                      * (1.0f + np * 0.35f) * (0.70f + 0.42f * vel)
                      * (0.92f + 0.16f * frand(m)) * tone_dsc(tone));
    /* Filter ENVELOPE depth. The cutoff used to be static for the whole hit; it now rides the
     * noise envelope that is already decaying, so the band sweeps down as the hit dies — crack
     * then body, instead of one flat colour. No new envelope state: s_nenv is right there.
     * Depth from `nd`, the morph decay axis, so long hits sweep further. */
    m->s_fd   = 1.6f * (1.0f + nd * 0.6f) * (0.8f + 0.4f * frand(m));
    /* Damping falls as the knob goes up: the band gets narrower and rings, so a pitched-up kit
     * squelches instead of sitting flat. 0.45 (the old constant) at centre and below. */
    m->s_q    = clampf(0.45f - 0.22f * clampf((tone - 0.5f) * 2.0f, 0.f, 1.f), 0.20f, 0.45f);
    m->s_nmix = 0.55f + 0.225f * (c + 1.0f);
}

static void trig_hat(gb_model_t* m, float vel, float c, bool open, float tone,
                     float np, float nd) {
    const float f0 = drum_pitch(263.0f, np * 7.0f + c * 6.0f + tone_semis(tone)
                                       + drum_octave(m, 0.5f, 30u),
                               1.0f + (frand(m) - 0.5f) * 0.05f);
    for (int k = 0; k < 6; k++) m->h_inc[k] = HZ2INC(f0 * HAT_RATIO[k]);
    m->h_env = vel;
    /* fmaxf on the base: an open hat at c = -1 computes 90 + 120*(-1) = -30 ms, which only ever
     * worked because decay_coef clamps internally — multiplying a negative by the stretch keeps it
     * negative, so pin it before it is scaled. */
    m->h_dec = decay_coef(clampf(fmaxf(open ? (90.0f + 120.0f * c) : (16.0f + 11.0f * (c + 1.0f)),
                                       2.0f) * (1.0f + nd * 0.7f) * tone_stretch(tone),
                                 2.0f, drum_cap()));
    /* Same three additions as the snare: np so morph reaches the filter, vel so accents open it,
     * and per-hit jitter. */
    m->h_f   = svf_f((5200.0f + 2200.0f * c + 2200.0f * tone)
                     * (1.0f + np * 0.30f) * (0.70f + 0.42f * vel)
                     * (0.92f + 0.16f * frand(m)) * tone_dsc(tone));
    /* Filter envelope off h_env — the hat's only envelope. This is what turns "shh" into "tss". */
    m->h_fd  = 1.3f * (1.0f + nd * 0.6f) * (0.8f + 0.4f * frand(m));
    m->h_q   = clampf(0.55f - 0.26f * clampf((tone - 0.5f) * 2.0f, 0.f, 1.f), 0.26f, 0.55f);
}

static void trig_bass(gb_model_t* m, int semi, bool acc, float tone, float np, float nd,
                      float gap_ms) {
    const float f = note_to_freq((float)semi);
    m->b_inc  = HZ2INC(f);
    m->b_rinc = 1.0f / (float)m->b_inc;
    m->b_sinc = m->b_inc >> 1;
    const float fsub = f * 0.5f;
    m->b_sub  = 0.5f + 0.45f * clampf((fsub - 26.0f) * (1.0f / 16.0f), 0.f, 1.f);
    m->b_env  = 1.0f; m->b_fenv = 1.0f;
    m->b_amp  = acc ? 1.0f : 0.72f;
    m->b_q    = clampf(((acc ? 0.28f : 0.55f) - np * 0.20f) * (0.82f + frand(m) * 0.36f),
                       0.18f, 0.85f);
    m->b_fd   = (acc ? (0.42f + 0.25f * tone) : (0.26f + 0.18f * tone))
                * (1.0f + np * 0.55f) * (0.75f + frand(m) * 0.5f);
    const float base_ms = 200.0f * (1.0f + nd * 0.75f);
    m->b_dec  = decay_coef(clampf(fmaxf(base_ms, gap_ms * 0.9f), 60.0f, 900.0f));
}

/* ---- CLAP / TOM ------------------------------------------------------------------------------
 * A burst train of filtered noise (hand clap) sitting on a short pitched body (rototom). `c` slides
 * between them, so the two ends of the character axis give two different instruments off one voice
 * and one set of state. Everything varispeeds with the rest of the kit. */
static void trig_clap(gb_model_t* m, float vel, float c, float tone, float np, float nd) {
    const float st = tone_stretch(tone);
    m->cl_mix  = clampf(0.5f - c * 0.55f, 0.f, 1.f);      /* 1 = all clap, 0 = all tom */
    m->cl_env  = vel;
    m->cl_dec  = decay_coef(clampf((34.0f + 26.0f * (c + 1.0f)) * (1.0f + nd * 0.7f)
                                   * (0.9f + 0.2f * frand(m)) * st, 3.0f, drum_cap()));
    m->cl_bpf  = svf_f((1150.0f + 900.0f * (c + 1.0f) + 700.0f * tone)
                       * (1.0f + np * 0.35f) * (0.70f + 0.42f * vel) * tone_dsc(tone));
    /* Body: a tom, tuned off the same node axis the other drums use. */
    const float bf = drum_pitch(150.0f, np * 9.0f + c * 6.0f + tone_semis(tone), 1.0f);
    m->cl_inc  = HZ2INC(bf);
    m->cl_ph   = 0x40000000u;
    m->cl_benv = vel * (1.0f - m->cl_mix) * 1.15f;
    m->cl_bdec = decay_coef(clampf((90.0f + 70.0f * (c + 1.0f)) * st, 5.0f, drum_cap()));
    /* Burst train: 3-4 slaps ~9 ms apart, stretching with everything else. */
    m->cl_n    = (uint8_t)(3 + (xrand(m) & 1u));
    m->cl_gap  = (uint32_t)(SR * 0.009f * st * (0.8f + 0.4f * frand(m)));
    m->cl_t    = m->cl_gap;
}

/* ---- FM METAL --------------------------------------------------------------------------------
 * Two-op FM through a bandpass. Deliberately SMOOTH: the ratio sits just off an integer so it beats
 * rather than clangs, and the index decays FASTER than the amplitude so each hit starts bright and
 * settles into a clean ring instead of buzzing all the way out. Tuned - it takes the scale degree,
 * so it carries the melodic line the lead used to. */
static void trig_metal(gb_model_t* m, float vel, float c, int semi, float tone, float np, float nd) {
    const float st = tone_stretch(tone);
    const float f  = note_to_freq((float)semi) * fast_exp2f(tone_semis(tone) * (1.0f / 12.0f));
    m->mt_incc = HZ2INC(f);
    m->mt_incm = HZ2INC(f * (2.0f + 0.41f + np * 0.35f));   /* off-integer = beating, not clang */
    m->mt_pc = m->mt_pm = 0;
    m->mt_env  = vel;
    m->mt_dec  = decay_coef(clampf((150.0f + 190.0f * (c + 1.0f)) * (1.0f + nd * 0.8f)
                                   * (0.9f + 0.2f * frand(m)) * st, 5.0f, drum_cap()));
    m->mt_idx  = 1.5f + 1.7f * vel + np * 0.6f;
    m->mt_idec = decay_coef(clampf(55.0f * (1.0f + nd * 0.5f) * st, 4.0f, drum_cap()));
    m->mt_f    = svf_f((2400.0f + 1500.0f * (c + 1.0f)) * (0.70f + 0.42f * vel) * tone_dsc(tone));
}

/* ---- GLITCH ----------------------------------------------------------------------------------
 * Sample-and-hold noise - a rate crush - through a resonant bandpass. Kept PLEASANT by tying the
 * hold period to the voice's own pitch: the crush then folds down onto harmonics of that pitch
 * rather than spraying inharmonic hash, so it reads as a tuned burst. */
static void trig_glitch(gb_model_t* m, float vel, float c, int semi, float tone, float np, float nd) {
    const float st = tone_stretch(tone);
    const float f  = note_to_freq((float)(semi + 12)) * fast_exp2f(tone_semis(tone) * (1.0f / 12.0f));
    const uint32_t h = (uint32_t)(SR / fmaxf(f, 20.0f));
    m->gl_hold = h ? h : 1u;                              /* hold period = one cycle of f */
    m->gl_t    = 0;
    m->gl_val  = 0.f;
    m->gl_env  = vel;
    m->gl_dec  = decay_coef(clampf((22.0f + 34.0f * (c + 1.0f)) * (1.0f + nd * 0.8f)
                                   * (0.85f + 0.3f * frand(m)) * st, 3.0f, drum_cap()));
    m->gl_f    = svf_f(fminf(f * (2.0f + np * 0.8f), 7000.0f) * (0.70f + 0.42f * vel)
                       * tone_dsc(tone));
    m->gl_q    = clampf(0.42f - np * 0.16f, 0.16f, 0.75f);
}

// ----------------------------------------------------------------------------
// Sequencer
// ----------------------------------------------------------------------------
static void fire_step(gb_model_t* m, const morph_w_t* w) {
    const int   sc   = (int)gb_prm[ENC_SCALE].val & 3;
    const int   root = (int)gb_prm[ENC_KEY].val;
    const int   buf  = m->live;
    const float step_ms = 15000.0f / fmaxf(gb_prm[ENC_TEMPO].val, 1.0f);
    uint8_t fired = 0;

    for (int t = 0; t < TRACKS; t++) {
        float inten, tn;
        step_cell(m, w, t, m->tstep[t], &inten, &tn);
        if (inten <= 1.0f - gb_dens(m, t)) {
            continue;
        }
        fired |= (uint8_t)(1u << t);

        /* Each voice gets its own point on the morph loop — see gb_tone_tr. */
        const float tone = gb_tone_tr(m, t);
        const bool  acc = inten > 0.78f;
        const float vel = 0.65f + 0.35f * fminf(inten, 1.0f);
        const float c   = clampf(tn * (1.0f / 64.0f), -1.f, 1.f);
        const int   deg = (int)(tn + (tn < 0.0f ? -0.5f : 0.5f));

        switch (t) {
        case T_KICK:  trig_kick (m, vel, c, tone, node_char(m->npit[buf], w, t),
                                                 node_char(m->ndec[buf], w, t)); break;
        case T_SNARE: trig_snare(m, vel, c, tone, node_char(m->npit[buf], w, t),
                                                 node_char(m->ndec[buf], w, t)); break;
        case T_HAT:   trig_hat  (m, vel, c, acc, tone, node_char(m->npit[buf], w, t),
                                                       node_char(m->ndec[buf], w, t)); break;
        case T_BASS:  trig_bass (m, root + deg2semi(sc, deg), acc, tone,
                                 node_char(m->npit[buf], w, t), node_char(m->ndec[buf], w, t),
                                 (float)steps_to_next(m, w, t, m->tstep[t]) * step_ms); break;
        case T_CLAP:  trig_clap (m, vel, c, tone, node_char(m->npit[buf], w, t),
                                                 node_char(m->ndec[buf], w, t)); break;
        /* The two tuned percussion voices take the scale degree, so they carry the melodic line
         * the lead and pad used to — an octave apart so they do not crowd each other. */
        case T_METAL: trig_metal(m, vel, c, root + 19 + deg2semi(sc, deg), tone,
                                 node_char(m->npit[buf], w, t), node_char(m->ndec[buf], w, t)); break;
        case T_GLITCH:trig_glitch(m, vel, c, root + 12 + deg2semi(sc, deg), tone,
                                 node_char(m->npit[buf], w, t), node_char(m->ndec[buf], w, t)); break;
        default: break;
        }
    }
    m->last_fire = fired;
}

static void clock_next(gb_model_t* m, uint32_t spf_q8, int32_t sw_q8,
                       uint16_t drift, morph_w_t* w) {
    m->step = (uint8_t)((m->step + 1) & 15);

    if (m->step == 0 && m->regen_req) {
        m->regen_req = 0;
        m->live ^= 1u;
        for (int t = 0; t < TRACKS; t++) m->tstep[t] = 0;
    } else {
        const int buf = m->live;
        for (int t = 0; t < TRACKS; t++) {
            const uint8_t L = m->plen[buf][t] ? m->plen[buf][t] : STEPS;
            uint8_t s = (uint8_t)(m->tstep[t] + 1);
            m->tstep[t] = (s >= L) ? 0 : s;
        }
    }

    if (m->step == 0) relen(m);

    m->morph_drift = (uint16_t)(m->morph_drift + drift);
    morph_weights(m, morph_phase(m), w);

    const uint32_t tot = spf_q8 + (uint32_t)((m->step & 1) ? -sw_q8 : sw_q8) + m->frac_q8;
    m->samp_left = tot >> 8;
    m->frac_q8   = tot & 0xFFu;
    if (m->samp_left == 0) m->samp_left = 1;

    uint8_t rc = 1;
    if ((m->step & 1) && (xrand(m) >> 24) < 11u) rc = 2;
    m->ratchets_cur = rc;
    m->ratchet_dir  = (xrand(m) & 0x10000u) ? 1 : -1;
    m->sub_len     = (rc > 1) ? (m->samp_left / rc) : 0xFFFFFFFFu;
    if (m->sub_len == 0) m->sub_len = 1;
    m->sub_left    = m->sub_len;
    m->ratchet_sub = 0;
}

// ----------------------------------------------------------------------------
// Audio: generate into in_bus — the decks record it, MixerInputVolParam applies
// ----------------------------------------------------------------------------
// ctx is our model — the firmware hands it to every engine callback.
static void gb_process(void* restrict ctx, mixer_t* restrict mix) {
    gb_model_t* m = ctx;
    float* const bus = mixer_get_in(mix);   // the 48k record bus
    if (!bus) return;
    const uint32_t fs = mixer_get_fs(mix);  // 48k samples (L+R)
    /* We render at 24k into our own scratch: half as many samples, and the
     * upsampler doubles them back into the bus at the end. */
    uint32_t n24 = fs >> 1;
    if (n24 > GB_SCRATCH24) n24 = GB_SCRATCH24;
    float* const out = m->scratch24;

    if (!m->playing) return;

    float* const sends = mixer_get_fx(mix);
    if (sends) {
        for (uint32_t i = 0; i < fs; i++) { bus[i] += sends[i]; sends[i] = 0.f; }
    }

    /* Input + sends down to our working rate, BEFORE anything writes the bus.
     * Pass n24 * 2 rather than fs so the n24 clamp above also bounds this write. */
    dec2x_f32(&m->dec, bus, m->in24, n24 * 2);

    // ---- block-rate constants -------------------------------------------
    /* The BUS character (crush, drive) stays on the bare knob: it is one global colour for the
     * whole kit, not something that should wobble per voice. The voices take gb_tone_tr. */
    const float tone = clampf(gb_prm[ENC_TONE].val, 0.f, 1.f);

    /* Varispeed — see tone_stretch() above. Unity for tone >= 0.5. Per VOICE now: the kick's
     * click and pitch sweep follow the kick's own tone, the snare's ring and crack the snare's. */
    const float st_k = tone_stretch(gb_tone_tr(m, T_KICK));
    const float st_s = tone_stretch(gb_tone_tr(m, T_SNARE));
    /* The knob is now dirty at BOTH ends and clean only in the middle. Down low the grit is the
     * SP's 12-bit; up high it is drive and resonance — pitched-up drums were the one place the kit
     * stayed clinically tight, and tight is not the point. `hi` is 0 at centre, 1 at the top. */
    const float hi      = clampf((tone - 0.5f) * 2.0f, 0.f, 1.f);
    /* Halved from the first pass — full-depth 12-bit read as fizzy rather than gritty. The SP's
     * charm is a bed of grit under the hit, not hash over it. */
    const float crush   = fmaxf(clampf((0.5f - tone) * 2.0f, 0.f, 1.f) * 0.55f, hi * 0.28f);
    const float dirt    = 1.0f + 1.15f * hi;                        /* pre-crush drive */

    const float kcd  = decay_coef(2.5f * st_k);   // kick click
    const float kpd  = decay_coef(10.f * st_k);   // kick pitch sweep — the big one
    const float sdt  = decay_coef(90.f * st_s);   // snare tone
    const float srd  = decay_coef(1.5f * st_s);   // snare transient
    const float bfd  = decay_coef(90.f);    // bass filter (not a drum — unscaled)
    const float b_f0 = svf_f(70.0f + 180.0f * gb_tone_tr(m, T_BASS));   /* bass follows its own */

    const float    drf      = gb_prm[ENC_DRIFT].val;
    const uint32_t spf_q8   = (uint32_t)(SR * 60.0f * 256.0f / (gb_prm[ENC_TEMPO].val * 4.0f));
    const float    sw_amt   = clampf(MIX_SWING + drf * 0.15f, 0.f, 1.f);
    const int32_t  sw_q8    = (int32_t)(sw_amt * 0.33f * (float)spf_q8);
    const uint16_t drift    = (uint16_t)(drf * drf * 4096.0f);

    /* Voice count: 1 = bare kick, 6 = everything, added in T_ order. Gating the
     * gains (not fire_step) keeps every muted track's polymeter running, so
     * unmuting drops back in on the grid. Block-rate — free in the inner loop. */
    const int nt = (int)(gb_prm[ENC_TRACKS].val + 0.5f);
    const float g_k = (nt > T_KICK)  ? LVL[T_KICK]  : 0.f;
    const float g_s = (nt > T_SNARE) ? LVL[T_SNARE] : 0.f;
    const float g_h = (nt > T_HAT)   ? LVL[T_HAT]   : 0.f;
    const float g_b = (nt > T_BASS)  ? LVL[T_BASS]  : 0.f;
    const float g_cl = (nt > T_CLAP)   ? LVL[T_CLAP]   : 0.f;
    const float g_mt = (nt > T_METAL)  ? LVL[T_METAL]  : 0.f;
    const float g_gl = (nt > T_GLITCH) ? LVL[T_GLITCH] : 0.f;
    const float pmp   = clampf(gb_prm[ENC_PUMP].val, 0.f, 1.f);
    const float d_in  = 2.6f * pmp;            // 0 .. 2.6 (pins at SC_FLOOR past 1)
    const float d_pad = fmaxf(0.55f, d_in);    // 0.55 .. 2.6  (the ringing metal voice)
    const float d_bas = fmaxf(0.25f, d_in);    // 0.25 .. 2.6
    /* Release is its own knob now — `pump` is depth only. 60..600 ms; rel 0.333 is the 240 ms that
     * pump 0.75 used to produce, so the default sound is unchanged. */
    m->sc_dec = decay_coef(60.0f + 540.0f * clampf(gb_prm[ENC_REL].val, 0.f, 1.f));

    morph_w_t w; morph_weights(m, morph_phase(m), &w);


    for (uint32_t i = 0; i < n24; i += 2) {
        // ---- clock: step + ratchet sub-hits ----
        if (--m->samp_left == 0) {
            clock_next(m, spf_q8, sw_q8, drift, &w);
            fire_step(m, &w);
        } else if (--m->sub_left == 0) {
            m->sub_left = m->sub_len;
            if (++m->ratchet_sub < m->ratchets_cur) {   // retrigger DRUMS only — bass/lead/pad never ratchet
                /* Retriggers keep the node's VOICE (np/nd) so a ratchet sounds like the
                 * same drum, and drop the per-step accent to 0 so it reads as a repeat
                 * rather than six accents in a row.
                 *
                 * The pitch walks across the repeats — that is what makes a roll rather
                 * than the same hit stuttered. It rides on np, not on the accent, so the
                 * decay and click stay put and only the tuning moves; drum_pitch() snaps it
                 * to semitones, so the walk lands on notes. Direction comes from the step
                 * hash, so some rolls climb and some fall, repeatably. */
                const int rb = m->live;
                const float rp = (float)m->ratchet_dir * (float)m->ratchet_sub * 0.55f;  // ~4 semitones a repeat
                if (m->last_fire & (1u << T_KICK))
                    trig_kick(m, 0.8f, 0.0f, tone, node_char(m->npit[rb], &w, T_KICK) + rp,
                                                   node_char(m->ndec[rb], &w, T_KICK));
                if (m->last_fire & (1u << T_SNARE))
                    trig_snare(m, 0.8f, 0.0f, tone, node_char(m->npit[rb], &w, T_SNARE) + rp,
                                                    node_char(m->ndec[rb], &w, T_SNARE));
                if (m->last_fire & (1u << T_HAT))
                    trig_hat(m, 0.7f, 0.0f, false, tone, node_char(m->npit[rb], &w, T_HAT) + rp,
                                                         node_char(m->ndec[rb], &w, T_HAT));
            }
        }

        // ---- KICK ----
        m->k_f  = m->k_f0 + (m->k_f - m->k_f0) * kpd;
        m->k_ph += HZ2INC(m->k_f); 
        float kick = fsin_i32((int32_t)m->k_ph) * m->k_env;
        kick += nrand(m) * m->k_click;
        kick  = sat1(kick * m->k_drv);   /* k_drv carries the low-pitch body — see trig_kick */
        m->sc_env *= m->sc_dec;
        const float sc    = m->sc_env;
        const float duck  = fmaxf(1.0f - d_pad * sc, SC_FLOOR);
        const float duckb = fmaxf(1.0f - d_bas * sc, SC_FLOOR);
        m->k_env *= m->k_dec;  m->k_click *= kcd;

        // ---- SNARE ----
        const float nz = nrand(m);
        /* Cutoff rides the decaying noise envelope: the band sweeps down across the hit. */
        SVF(nz, m->s_lp, m->s_bp, fminf(m->s_bpf * (1.0f + m->s_fd * m->s_nenv), 1.2f), m->s_q);
        m->s_ph1 += m->s_inc1;  m->s_ph2 += m->s_inc2;
        const float tonal = (fsin_i32((int32_t)m->s_ph1) * 0.6f +
                             fsin_i32((int32_t)m->s_ph2) * 0.4f) * m->s_tenv;
        const float snare = m->s_bp * m->s_nmix * m->s_nenv + tonal * 0.55f + nz * m->s_tr * 0.35f;
        m->s_nenv *= m->s_ndec;  m->s_tenv *= sdt;  m->s_tr *= srd;

        // ---- HAT ----
        uint32_t hacc = 0;
        for (int k = 0; k < 6; k++) { m->h_ph[k] += m->h_inc[k]; hacc += m->h_ph[k] >> 31; }
        const float metal = (float)hacc * (1.0f / 3.0f) - 1.0f;
        SVF(metal, m->h_lp, m->h_bp, fminf(m->h_f * (1.0f + m->h_fd * m->h_env), 1.2f), m->h_q);
        const float hat = m->h_bp * m->h_env;
        m->h_env *= m->h_dec;

        // ---- BASS ----
        m->b_ph  += m->b_inc;
        m->b_sph += m->b_sinc;
        const float bsaw = saw_u32(m->b_ph) - blep_u32(m->b_ph, m->b_inc, m->b_rinc);
        const float bx   = bsaw + fsin_i32((int32_t)m->b_sph) * m->b_sub;
        SVF(bx, m->b_lp, m->b_bp, fminf(b_f0 + m->b_fenv * m->b_fd, 1.2f), m->b_q);
        const float bass = sat1(m->b_lp * m->b_env * m->b_amp * 1.35f);
        m->b_env *= m->b_dec;  m->b_fenv *= bfd;

        // ---- CLAP / TOM ----
        if (m->cl_n) {                                  /* burst train: re-fire the noise env */
            if (m->cl_t) m->cl_t--;
            else { m->cl_env = fmaxf(m->cl_env, 0.8f); m->cl_t = m->cl_gap; m->cl_n--; }
        }
        float clap = nrand(m) * m->cl_env * m->cl_mix;
        SVF(clap, m->cl_lp, m->cl_bp, m->cl_bpf, 0.38f);
        clap = m->cl_bp * 1.5f;
        m->cl_ph += m->cl_inc;                          /* tom body under the slaps */
        clap += fsin_i32((int32_t)m->cl_ph) * m->cl_benv;
        m->cl_env *= m->cl_dec;  m->cl_benv *= m->cl_bdec;

        // ---- FM METAL ----
        m->mt_pm += m->mt_incm;
        const float mmod = fsin_i32((int32_t)m->mt_pm) * m->mt_idx;
        m->mt_pc += m->mt_incc;
        /* Index decays faster than amplitude: bright attack settling to a clean ring, not a buzz. */
        float mtl = fsin_i32((int32_t)(m->mt_pc + (uint32_t)(mmod * 3.0e8f))) * m->mt_env;
        SVF(mtl, m->mt_lp, m->mt_bp, m->mt_f, 0.30f);
        /* The one new voice that RINGS, so it inherits the pad's side of the sidechain — otherwise
         * `d_pad` and its duck died with the pad and the pump knob would only move bass and input. */
        mtl = m->mt_bp * duck;
        m->mt_env *= m->mt_dec;  m->mt_idx *= m->mt_idec;

        // ---- GLITCH ----
        if (m->gl_t) m->gl_t--;
        else { m->gl_val = nrand(m); m->gl_t = m->gl_hold; }   /* S&H at one cycle of the pitch */
        float glitch = m->gl_val * m->gl_env;
        SVF(glitch, m->gl_lp, m->gl_bp, m->gl_f, m->gl_q);
        glitch = m->gl_bp * 1.4f;
        m->gl_env *= m->gl_dec;

        /* SP-1200 12-bit, on the drums only — they are what the machine sampled. Mixed in by
         * `crush` rather than switched, so tone >= 0.5 is bit-exact rather than merely inaudible,
         * and the grit arrives with the varispeed instead of as a separate mode. One quantiser on
         * the sum, not three per voice; `side` below stays clean, which keeps the noise mono. */
        float drums = g_k * kick + g_s * snare + g_h * hat
                    + g_cl * clap + g_mt * mtl + g_gl * glitch;
        /* Push the bus into the soft clip before quantising, so at high tone the kit grits up and
         * loses its edges instead of staying surgical.
         *
         * MIXED IN by `hi`, not applied outright: sat1 is NOT transparent at unity drive
         * (sat1(1.0) = 0.852, sat1(0.5) = 0.481), so calling it unconditionally put the whole drum
         * bus through a soft clipper that was never there. It compresses the biggest peaks hardest
         * — the kick — and a pitched-down kick is 4x longer, so it sat on the clipper continuously
         * and audibly lost its low end. At hi == 0 this is now exactly `drums`. */
        drums += (sat1(drums * dirt) - drums) * hi;
        const float dq = drums * 2048.0f;
        drums += ((float)(int32_t)(dq + (dq < 0.f ? -0.5f : 0.5f)) * (1.0f / 2048.0f) - drums)
                 * crush;

        /* Width off the voices with the most air in them; the kick and the bass stay dead centre. */
        const float mid  = drums + g_b * duckb * bass;
        const float side = 0.10f * g_s * snare - 0.22f * g_h * hat
                         + 0.20f * g_cl * clap - 0.26f * g_gl * glitch + 0.16f * g_mt * mtl;
        float l = mid - side;
        float r = mid + side;

        // DC pole kept from the 48k version — cutoff doubles to ~24 Hz at 24k, fine.
        const float yl = l - m->dcx_l + 0.99843f * m->dcy_l; m->dcx_l = l; m->dcy_l = yl; l = yl;
        const float yr = r - m->dcx_r + 0.99843f * m->dcy_r; m->dcx_r = r; m->dcy_r = yr; r = yr;
        const float g_in = fmaxf(1.0f - d_in * sc, SC_FLOOR);
        l += m->in24[i]     * g_in;
        r += m->in24[i + 1] * g_in;

        out[i]     = fminf(fmaxf(sat1(l * BUS_TRIM), -1.0f), 1.0f);
        out[i + 1] = fminf(fmaxf(sat1(r * BUS_TRIM), -1.0f), 1.0f);
    }

    /* 24k -> 48k into the record bus. ups2x_f32_add ACCUMULATES and we own the
     * bus, so clear it first — the live input is NOT lost by this: it was
     * decimated into in24 at the top of the block and is already mixed (ducked)
     * into the 24k sum being upsampled here. */
    for (uint32_t i = 0; i < fs; i++) bus[i] = 0.f;
    ups2x_f32_add(&m->ups, out, bus, n24);


    m->sc_env = dnz(m->sc_env);
    m->k_env = dnz(m->k_env);   m->k_click = dnz(m->k_click);
    m->s_nenv = dnz(m->s_nenv); m->s_tenv = dnz(m->s_tenv); m->s_tr = dnz(m->s_tr);
    m->s_lp = dnz(m->s_lp);     m->s_bp = dnz(m->s_bp);
    m->h_env = dnz(m->h_env);   m->h_lp = dnz(m->h_lp);     m->h_bp = dnz(m->h_bp);
    m->b_env = dnz(m->b_env);   m->b_fenv = dnz(m->b_fenv);
    m->b_lp = dnz(m->b_lp);     m->b_bp = dnz(m->b_bp);
    m->cl_env = dnz(m->cl_env); m->cl_benv = dnz(m->cl_benv);
    m->cl_lp = dnz(m->cl_lp);   m->cl_bp = dnz(m->cl_bp);
    m->mt_env = dnz(m->mt_env); m->mt_idx = dnz(m->mt_idx);
    m->mt_lp = dnz(m->mt_lp);   m->mt_bp = dnz(m->mt_bp);
    m->gl_env = dnz(m->gl_env); m->gl_lp = dnz(m->gl_lp);   m->gl_bp = dnz(m->gl_bp);
    m->dcy_l = dnz(m->dcy_l);   m->dcy_r = dnz(m->dcy_r);
}

// ----------------------------------------------------------------------------
// Voice reset / transport
// ----------------------------------------------------------------------------
static void voices_silence(gb_model_t* m) {
    m->k_env = m->k_click = 0.f;
    m->s_nenv = m->s_tenv = m->s_tr = m->s_lp = m->s_bp = 0.f;
    m->h_env = m->h_lp = m->h_bp = 0.f;
    m->b_env = m->b_fenv = m->b_lp = m->b_bp = 0.f;
    m->cl_env = m->cl_benv = m->cl_lp = m->cl_bp = 0.f;  m->cl_n = 0;
    m->mt_env = m->mt_idx = m->mt_lp = m->mt_bp = 0.f;
    m->gl_env = m->gl_lp = m->gl_bp = m->gl_val = 0.f;
    m->dcx_l = m->dcy_l = m->dcx_r = m->dcy_r = 0.f;
    m->sc_env = 0.f;
}

static void gb_set_playing(gb_model_t* m, bool state) {
    m->playing = state;
    if (!state) voices_silence(m);
}

static void gb_eng_active(void* ctx, bool state) { gb_set_playing(ctx, state); }
static uint_fast8_t gb_eng_is_active(void* ctx) { return ((gb_model_t*)ctx)->playing; }

static const engine_callbacks_t gb_engine = {
    .process = gb_process,
    .active = gb_eng_active,
    .is_active = gb_eng_is_active,
};

// ----------------------------------------------------------------------------
// Rendering — 6 rows x 16 steps across the full 400x240 panel
// ----------------------------------------------------------------------------
#define GRID_X0   10
#define CELL_W    21
#define CELL_PIT  24
#define ROW_H     CELL_W
#define ROW_PIT   CELL_PIT
#define GRID_Y0   (238 - (5 * ROW_PIT + ROW_H))

static void gb_params_draw(gfx_t* gfx, gb_model_t* m);

static void gb_redraw(gfx_t* gfx, const os_app_t* app) {
    gb_model_t* m = os_app_get_model(app);
    morph_w_t w; morph_weights(m, morph_phase(m), &w);
    const int buf = m->live;
    const int nt = (int)(gb_prm[ENC_TRACKS].val + 0.5f);

    // ---- step matrix ----
    for (int row = 0; row < TRACKS; row++) {
        const int gy = GRID_Y0 + row * ROW_PIT;
        const int len = m->plen[buf][row] ? m->plen[buf][row] : STEPS;
        gfx_set_color(gfx, 1);

        const float thr = 1.0f - gb_dens(m, row);
        for (int st = 0; st < len; st++) {
            const int gx = GRID_X0 + st * CELL_PIT;
            float inten, tn;
            step_cell(m, &w, row, st, &inten, &tn);
            gfx_draw_rect(gfx, gx, gy, CELL_W, ROW_H);
            if (inten > thr) {
                // Shade by how far past the threshold it is, so velocity shows.
                const float head = thr < 1.0f ? (1.0f - thr) : 1.0f;
                int sh = 1 + (int)((inten - thr) / head * 6.0f);
                if (sh < 1) sh = 1; else if (sh > 7) sh = 7;
                gfx_fill_rect_dithered(gfx, gx + 2, gy + 2, CELL_W - 4, ROW_H - 4, (uint8_t)sh);
            }
        }
        if (m->playing) {   // XOR playhead
            gfx_set_color(gfx, 2);
            const int px = GRID_X0 + m->tstep[row] * CELL_PIT;
            if (row >= nt)
                gfx_fill_rect_dithered(gfx, px, gy, CELL_W, ROW_H, UI_HALFTONE);
            else
                gfx_draw_rect_fill(gfx, px, gy, CELL_W, ROW_H);
            gfx_set_color(gfx, 1);
        }
    }

    gfx_set_color(gfx, 1);
    gb_blit(gfx, m->fig_base);

    gfx_set_color(gfx, 0);
    gfx_fill_rect_dithered(gfx, INF_X - INF_PAD, INF_Y - INF_PAD,
                           INF_SX + 2 * INF_PAD + 1, INF_SY + 2 * INF_PAD + 1, UI_HALFTONE+1);
    gfx_set_color(gfx, 1);

    if (m->enc_target == ENC_MORPH) {
        gfx_set_color(gfx, 0);
        gb_blit(gfx, m->fig_fat);
        gfx_set_color(gfx, 1);
        gb_blit(gfx, m->fig_base);
    }

    {
        float nx, ny;
        morph_xy_raw(morph_arc(m, morph_phase(m)), &nx, &ny);
        const gfx_coord_t mx = INF_X + (int)(nx * INF_SX + 0.5f);
        const gfx_coord_t my = INF_Y + (int)(ny * INF_SY + 0.5f);
        gfx_set_color(gfx, 1);
        gfx_draw_disc(gfx, mx, my, INF_DOT_R + 1, 0x0F);
        gfx_set_color(gfx, 0);
        gfx_draw_disc(gfx, mx, my, INF_DOT_R, 0x0F);
        gfx_set_color(gfx, 1);
    }

    gb_params_draw(gfx, m);
}

// ----------------------------------------------------------------------------
// Param panel — metronome and stuff
// ----------------------------------------------------------------------------

static void draw_tri(gfx_t* g, int x, int y, int h, int w) {
    for (int i = 0; i < h; i++) {
        const int d = (i < h / 2) ? i : (h - 1 - i);
        gfx_draw_hline(g, x, y + i, 1 + (2 * d * (w - 1)) / (h - 1));
    }
}

static void draw_tri_up(gfx_t* g, int cx, int y, int h, int w, int wt) {
    for (int i = 0; i < h; i++) {
        const int ww = wt + (i * (w - wt)) / (h - 1);
        gfx_draw_hline(g, cx - ww / 2, y + i, ww);
    }
}

static void draw_metro(gfx_t* gfx, int cx, int y, bool playing, float bpm) {
    gfx_set_color(gfx, 1);
    draw_tri_up(gfx, cx, y + 4, 28, 29, 5);
    gfx_set_color(gfx, 0);
    draw_tri_up(gfx, cx, y + 12, 16, 13, 5);
    gfx_fill_rect_dithered(gfx, cx - 16, y, 33, 33, UI_HALFTONE+2);
    gfx_set_color(gfx, 1);

    int dx = -6;
    if (playing) {
        const uint32_t per = (uint32_t)(60000.0f / (bpm > 1.f ? bpm : 1.f));
        const uint32_t p   = per ? (os_tick_get() % per) : 0u;
        const uint32_t ph  = ((p << 16) / (per ? per : 1u)) << 16;
        dx = (int)(fsin_i32((int32_t)ph) * 9.0f);
    }
    gfx_draw_line_thick(gfx, cx, y + 26, cx + dx, y + 6, 4);
}

/* Value text, CENTERED under the BTN3 cell. The roll is right-aligned, so the
 * centered right edge comes from the settled string's width. */
static void gb_val_text(gfx_t* gfx, gb_model_t* m, gfx_coord_t x, gfx_coord_t y, gfx_coord_t w,
                        const char* s) {
    const uint16_t sw = gfx_get_str_width(gfx, s);
    ui_roll_text_draw(gfx, x + (w + sw) / 2, y + 17, &m->val_roll, s);
}

/* Our own value bar, rather than ui_draw_value_bar(): that one is a shared firmware component
 * drawn as a full capsule (outline r=11, knob r=8, both clamping to half the height), and the
 * main menu's own bars use it too — the rounding there is not grooves' to change.
 *
 * The two radii differ on purpose. The outline goes through gfx_draw_rect_r's real arc, so 4
 * is a light round. The knob goes through gfx_draw_rect_fill_r, whose corner table does NOT
 * match that arc: r=4 there is a 1px notch two rows deep that reads as damage rather than a
 * curve, so the knob takes 3 — the same single-pixel cut the statusbar pill uses. */
static void gb_value_bar(gfx_t* gfx, gfx_coord_t x, gfx_coord_t y, gfx_coord_t w, gfx_coord_t h,
                         uint32_t pc) {
    const gfx_coord_t ih = (h > 2) ? h - 2 : 1;
    const gfx_coord_t span = (w > 2) ? w - 2 : 1;
    gfx_draw_rect_r(gfx, x, y, w, h, 3);
    /* A CELL FILLING UP, not a knob sliding along a track: the fill is the outline's interior
     * inset by 1px on every side, so at full it reaches the border all round.
     *
     * No minimum-width floor either — a floor is what a knob needs so it stays a shape at
     * zero, and a fill wants the opposite: empty means empty. 1px is forced only when the
     * value is non-zero but rounds below a pixel, so a small setting never reads as none. */
    gfx_coord_t fill = (gfx_coord_t)(((uint32_t)span * pc) / 1000u);
    if (!fill && pc) fill = 1;
    if (fill) gfx_draw_rect_fill_r(gfx, x + 1, y + 1, fill, ih, 3);
}

/* The selected value as a short string, for the cell LABEL — see val_txt. Key and scale are
 * already words; everything else is a percentage of its own range, which is what the bar shows
 * graphically. Morph has no value cell at all (that box is its rec button). */
static const char* gb_val_label(gb_model_t* m) {
    const params_t* const p = &gb_prm[m->enc_target];
    if (m->enc_target == ENC_KEY) {
        const int mid = (int)p->val;
        snprintf(m->val_txt, sizeof(m->val_txt), "%s%d", NOTE_NAME[mid % 12], mid / 12 - 1);
    } else if (m->enc_target == ENC_SCALE) {
        snprintf(m->val_txt, sizeof(m->val_txt), "%s", p->option_name[(int)p->val & 3]);
    } else {
        const float span = p->max - p->min;
        const unsigned pc = (span > 0.f) ? (unsigned)((p->val - p->min) / span * 100.f + 0.5f) : 0u;
        snprintf(m->val_txt, sizeof(m->val_txt), "%u%%", pc);
    }
    return m->val_txt;
}

/* BTN3's HOLD cell for every target but MORPH: the selected value, as a bar — or as rolling
 * text where the value is a word (key, scale).
 *
 * A real band cell, not pixels the app paints into the band's rows. Drawing it from redraw()
 * worked only while the app happened to be the last thing to paint there; open the fx menu
 * over grooves and the band repaints without us, so this stopped being refreshed along with
 * everything else the app had put in those rows. x/y/w come from the firmware, so it cannot
 * drift from the cell above it either.
 *
 * MORPH never reaches here — gb_hints() clears the callback there, because that box is the
 * motion lane's rec button and the figure-8 is morph's value display. */
static void gb_hint_val(gfx_t* gfx, gfx_coord_t x, gfx_coord_t y, gfx_coord_t w, void* user) {
    gb_model_t* m = (gb_model_t*)user;
    params_t* const p = &gb_prm[m->enc_target];
    // A painter starts in GFX_COLOR_SET with the default font, clipped to its cell.
    if (m->enc_target == ENC_KEY) {
        const int mid = (int)p->val;
        snprintf(m->key_txt, sizeof(m->key_txt), "%s%d", NOTE_NAME[mid % 12], mid / 12 - 1);
        gb_val_text(gfx, m, x, y, w, m->key_txt);
    } else if (m->enc_target == ENC_SCALE) {
        gb_val_text(gfx, m, x, y, w, p->option_name[(int)p->val & 3]);
    } else {
        ui_roll_text_snap(&m->val_roll, "");
        /* Show what the voices actually GET, not the bare knob. tone and dens are offset by the
         * morph position, so drawing p->val left the bar frozen while the sound moved — the one
         * modulation in the tapp you could hear but not see. */
        float shown = p->val;
        /* No single number represents a per-voice tone; show the KICK's, the anchor voice. */
        if      (m->enc_target == ENC_TONE) shown = gb_tone_tr(m, T_KICK);
        else if (m->enc_target == ENC_DENS) shown = clampf(p->val + (gb_dens(m, 0) - m->dens[0]),
                                                           p->min, p->max);
        const float span = p->max - p->min;
        const uint32_t pc = (span > 0.f) ? (uint32_t)((shown - p->min) / span * 1000.f) : 0u;
        /* 4px inset each side so the bar does not run into the cell's own edges (unless the
         * cell is degenerate). */
        const gfx_coord_t bw = (w > 8) ? w - 8 : w;
        const gfx_coord_t bx = (w > 8) ? x + 4 : x;
        gb_value_bar(gfx, bx, y + 5, bw, 12, pc);
    }
    gb_val_label(m);   // keep the label a seeding band would inherit in step with the cell
}

static void gb_params_draw(gfx_t* gfx, gb_model_t* m) {
    gfx_set_color(gfx, 1);
    draw_metro(gfx, 26, 32, m->playing, gb_prm[ENC_TEMPO].val);
    gfx_set_font(gfx, gfx_nunito_semibold_14);
    {
        /* Centred on a digit PLACEHOLDER, not on the rendered string, so the number does not
         * shift sideways as the tempo changes within a digit count. */
        const int bpm = (int)(gb_prm[ENC_TEMPO].val + 0.5f);
        const uint16_t bw = gfx_get_str_width(gfx, bpm > 99 ? "000" : "00");
        gfx_draw_strf(gfx, 26 - bw / 2, 85, "%d", bpm); // metro ends y67 + 4px pad
    }

    /* The value display itself is a BAND CELL now (gb_hint_val on HintHold3), not something
     * drawn here into the band's rows — see that callback. Morph keeps the roll snapped empty
     * so returning to key/scale flips in from blank rather than from a stale word. */
    if (m->enc_target == ENC_MORPH) ui_roll_text_snap(&m->val_roll, "");
}

// ----------------------------------------------------------------------------
// Encoder
// ----------------------------------------------------------------------------
static void gb_enc_apply(gb_model_t* m, int32_t d) {
    if (m->enc_target == ENC_MORPH) {
        /* Fine steps, but WRAP instead of clamping at 0/1 — the path is a
         * closed loop. On a seam crossing shift target AND val together by a
         * full turn so any pending ease survives it. */
        params_t* p = &gb_prm[ENC_MORPH];
        p->target += p->fine * (float)d;
        if (p->target >= 1.0f)     { p->target -= 1.0f; p->val -= 1.0f; }
        else if (p->target < 0.0f) { p->target += 1.0f; p->val += 1.0f; }
        return;
    }
    param_write_delta_val(&gb_prm[m->enc_target], d);
}

// ----------------------------------------------------------------------------
// Hints
// ----------------------------------------------------------------------------
/* The cell's plate is lit while playing (HINT_ACTIVE, see gb_hints), so the glyph and the
 * word draw in the CLEAR colour on it then. */
static void gb_hint_transport(gfx_t* gfx, gfx_coord_t x, gfx_coord_t y, gfx_coord_t w, void* user) {
    (void)w;
    gb_model_t* m = (gb_model_t*)user;
    gfx_set_color(gfx, m->playing ? GFX_COLOR_CLEAR : GFX_COLOR_SET);
    const int gy = y + 11;
    if (m->playing) draw_tri(gfx, x + 8, gy - 6, 13, 11);
    else            gfx_draw_rect_fill(gfx, x + 8, gy - 5, 10, 10);
    gfx_draw_str(gfx, x + 34, y + 17, m->playing ? "stop" : "play");
}


static void gb_hint_param(gfx_t* gfx, gfx_coord_t x, gfx_coord_t y, gfx_coord_t w, void* user) {
    gb_model_t* m = (gb_model_t*)user;
    const char* const nm = gb_prm[m->enc_target].name;
    const uint16_t sw = gfx_get_str_width(gfx, nm);
    ui_roll_text_draw(gfx, x + (w + sw) / 2, y + 17, &m->enc_roll, nm);
}

static void gb_hints(gb_model_t* m) {
    /* BTN1 press is custom-drawn (gb_hint_transport) and lit while playing. BTN3's press row is
     * custom-drawn too (gb_hint_param). BTN3's hold is the one cell that changes job with the
     * target: under MORPH it is the motion lane's rec button (lit while the lane is live), and
     * everywhere else it is the VALUE display, custom-drawn by gb_hint_val. The value text is
     * there so a band seeding from ours has a number to show where it cannot draw our bar.
     *
     * BTN4/5: both rows are SYSTEM-owned for input tapps (fx-menu prev/next + back/exit
     * icons) — anything stated there is ignored. */
    const bool morph = (m->enc_target == ENC_MORPH);
    const tapp_hint_cell_t hints[HintsActionsTotal] = {
        [HintPress1] = {NULL, NULL, NULL, m->playing ? HINT_ACTIVE : 0},
        [HintPress2] = {"shuffle"},
        /* One PARAM pill — the name over its value bar — except under morph, where the hold
         * cell is the rec button and the pill shrinks back to give it its own row. The label
         * is what a seeding band shows; the painter (gb_hint_param) wins while we own it. */
        [HintPress3] = {gb_prm[m->enc_target].name, NULL, NULL, morph ? 0 : HINT_PARAM},
        [HintHold3] = {morph ? "rec" : gb_val_label(m), &asset_mini_app_icon_16_16, NULL,
                       morph ? (m->auto_state != AUTO_OFF ? HINT_ACTIVE : 0)
                             : (m->adj_off ? HINT_LOCKED : 0)},
    };
    ui_hints_set(hints);
    /* The value cell paints itself — except under morph, where the cell is the rec button and
     * has to draw as a normal labelled hint: CLEARED there, or the painter would cover "rec". */
    ui_hints_set_gfx_cb(HintHold3, morph ? NULL : gb_hint_val, morph ? NULL : m);
}

// ----------------------------------------------------------------------------
// Tick
// ----------------------------------------------------------------------------
static bool gb_tick(os_app_t* app) {
    gb_model_t* m = os_app_get_model(app);
    /* get_delta DRAINS the accumulator, so this stays its only consumer — gb_input must not
     * read it too, or whoever runs first eats the other's motion. */
    const int32_t d = os_controls_encoder_get_delta();
    if (d && !m->adj_off) {
        gb_enc_apply(m, d);
        /* Only MORPH motion during a TAKE counts: the rec cell is offered for no other
         * target, so turning something else mid-hold must not arm a flat lane. */
        if (m->auto_state == AUTO_REC && m->enc_target == ENC_MORPH) m->auto_moved = 1;
    }
    param_update_val(&gb_prm[m->enc_target]);   // menu parity: ease val toward target

    /* Morph lane, stepped AFTER the ease: recording then captures the value actually heard,
     * and on playback this write is the last word for the tick. */
    if (m->auto_state == AUTO_REC) {
        /* val sits just OUTSIDE 0..1 for a tick around a seam crossing (gb_enc_apply) — the
         * int32 hop is morph_phase's idiom, so the cast wraps instead of saturating. */
        if (m->auto_len < AUTO_MAX)             // full: stop appending, keep recording
            m->auto_buf[m->auto_len++] = (uint16_t)(int32_t)(gb_prm[ENC_MORPH].val * 65536.0f);
    } else if (m->auto_state == AUTO_PLAY && m->playing) {
        /* Transport-gated, and FROZEN rather than rewound while stopped: gb_set_playing only
         * silences the voices, it does not reset m->step, so the pattern resumes where it left
         * off — the lane resumes with it. Stopped, morph is back under the knob. */
        if (++m->auto_idx >= m->auto_len) m->auto_idx = 0;
        const float v = (float)m->auto_buf[m->auto_idx] * (1.0f / 65536.0f);
        gb_prm[ENC_MORPH].val = gb_prm[ENC_MORPH].target = v;   // both: no ease to fight
    }

    /* dens is a MACRO over the per-track array (each track keeps its own
     * default): spread the param's movement as a delta, clamped per track. */
/* In the MODEL, not a function static. As a static it initialised once per image load while
     * m->dens[] is re-seeded from DENS_DFLT[] by gb_init on every launch — so relaunching after
     * moving the knob left a stale previous-value behind and the first move applied a wrong delta. */
    const float dv = gb_prm[ENC_DENS].val;
    if (dv != m->dens_prev) {
        const float s = dv - m->dens_prev;
        for (int t = 0; t < TRACKS; t++) m->dens[t] = clampf(m->dens[t] + s, 0.f, 1.f);
        m->dens_prev = dv;
    }

    return true;
}

// ----------------------------------------------------------------------------
// Input
// ----------------------------------------------------------------------------
static void gb_input(os_app_t* app, uint8_t btn, KeyStateEnum st) {
    gb_model_t* m = os_app_get_model(app);
    if (btn == BTN_ENCODER) return;   // the encoder is read in tick()

    if (btn == BTN2) {
        if (st != KEY_STATE_PRESSED) return;
        gb_regen(m, os_tick_get() ^ (uint32_t)(uintptr_t)m);
        ui_statusbar_hint_event(HintPress2);
        return;
    }

    /* BTN3 HOLD = the morph motion lane. A hold arrives as HOLD (once, at 270ms, finger still
     * down) then RELEASED — with NO PRESSED, while a tap is PRESSED+RELEASED together at the
     * release edge. So arming on HOLD and firing only on RELEASED-while-armed can never be
     * confused with a tap — which switches param adjustment on/off, one switch for every
     * param (the knob is drained but applies nothing, the bar greys). Only BTN4/5 holds are swallowed by
     * the input-tapp exit gesture; BTN3's reaches us intact. */
    if (btn == BTN3) {
        if (st == KEY_STATE_HOLD && m->enc_target == ENC_MORPH) {
            m->auto_state = AUTO_REC; m->auto_len = 0; m->auto_idx = 0; m->auto_moved = 0;
        } else if (st == KEY_STATE_RELEASED && m->auto_state == AUTO_REC) {
            /* A take with no knob movement is the OFF gesture; one with movement replaces
             * whatever was playing. Both fall out of the same release. */
            if (m->auto_moved && m->auto_len) {
                m->auto_state = AUTO_PLAY;
                /* One BEFORE the start, so the tick's pre-increment lands on 0 and the loop
                 * begins where the gesture began. auto_len >= 1 is guaranteed by the test. */
                m->auto_idx = (uint16_t)(m->auto_len - 1);
            } else {
                m->auto_state = AUTO_OFF;
            }
        } else if (st == KEY_STATE_PRESSED) {
            m->adj_off = !m->adj_off;
        } else {
            return;
        }
        gb_hints(m);   // the rec cell lights while the lane is live; the bar greys while off
        return;
    }

    if (st != KEY_STATE_RELEASED) return;

    switch (btn) {
    case BTN1:
        /* engine_set_active() runs gb_eng_active synchronously, so m->playing is current by
         * the time gb_hints() lights the plate from it. */
        engine_set_active(!m->playing);
        gb_hints(m);
        break;
    /* BTN2 and BTN3 handled above. The target change re-runs gb_hints so the morph-only
     * "rec" cell appears and disappears with the selection. */
    case BTN4: m->enc_target = (uint8_t)((m->enc_target + ENC_TOTAL - 1) % ENC_TOTAL);
            gb_hints(m); break;
    case BTN5: m->enc_target = (uint8_t)((m->enc_target + 1) % ENC_TOTAL);
            gb_hints(m); break;
    default: break;
    }
}

// ----------------------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------------------

static bool gb_init(os_app_t* app) {
    gb_model_t* m = os_app_get_model(app); // zeroed: not playing

    m->xs = 0x9e3779b9u;

    for (int t = 0; t < TRACKS; t++) m->dens[t] = DENS_DFLT[t];
    m->dens_prev = gb_prm[ENC_DENS].val;   /* seed WITH the array above, or the first knob move
                                            * applies the whole knob value as a delta */

    gb_regen(m, os_tick_get() ^ (uint32_t)(uintptr_t)m);
    m->live ^= 1u; m->regen_req = 0;
    m->k_f = m->k_f0 = 42.f;
    m->b_inc = HZ2INC(55.f); m->b_rinc = 1.0f / (float)m->b_inc; m->b_sinc = m->b_inc >> 1;
    m->b_q = 0.55f; m->b_fd = 0.30f;
    m->h_dec = decay_coef(32.f); m->h_f = svf_f(6000.f);
    m->s_bpf = svf_f(2500.f); m->s_nmix = 0.7f; m->s_ndec = decay_coef(40.f);
    m->s_q = 0.45f; m->h_q = 0.55f;
    m->b_dec = decay_coef(200.f);
    m->cl_bpf = svf_f(2000.f); m->cl_dec = decay_coef(45.f); m->cl_bdec = decay_coef(120.f);
    m->cl_inc = HZ2INC(150.f); m->cl_mix = 0.5f; m->cl_gap = (uint32_t)(SR * 0.009f);
    m->mt_f = svf_f(3200.f); m->mt_dec = decay_coef(260.f); m->mt_idec = decay_coef(55.f);
    m->mt_incc = HZ2INC(330.f); m->mt_incm = HZ2INC(330.f * 2.41f);
    m->gl_f = svf_f(2600.f); m->gl_q = 0.42f; m->gl_dec = decay_coef(40.f);
    m->gl_hold = (uint32_t)(SR / 330.f);
    {
        float px, py, L = 0.f;
        morph_xy_raw(0, &px, &py);
        for (int i = 1; i <= 4096; i++) {
            float nx, ny; morph_xy_raw((uint16_t)(i * 16), &nx, &ny);
            const float dx = (nx - px) * INF_SX, dy = (ny - py) * INF_SY;
            L += sqrtf(dx * dx + dy * dy);
            px = nx; py = ny;
        }
        float acc = 0.f;
        int k = 0;
        m->arc_lut[0] = 0;
        morph_xy_raw(0, &px, &py);
        for (int i = 1; i <= 4096 && k < 255; i++) {
            float nx, ny; morph_xy_raw((uint16_t)(i * 16), &nx, &ny);
            const float dx = (nx - px) * INF_SX, dy = (ny - py) * INF_SY;
            acc += sqrtf(dx * dx + dy * dy);
            px = nx; py = ny;
            while (k < 255 && acc >= L * (float)(k + 1) * (1.0f / 256.0f))
                m->arc_lut[++k] = (uint16_t)(i * 16);
        }
        while (k < 255) m->arc_lut[++k] = 0xFFF0;
    }

    gb_bake_fig(m);

    m->enc_target = ENC_MORPH;
    m->samp_left = 1; m->sub_len = 0xFFFFFFFFu; m->sub_left = 0xFFFFFFFFu;
    { morph_w_t w; morph_weights(m, morph_phase(m), &w); fire_step(m, &w); }

    /* Land stopped: the model arrives not playing and BTN1 starts the transport. Silence the
     * voices fire_step() above just armed — the same state a BTN1 stop leaves behind. */
    gb_set_playing(m, false);

    gb_hints(m);
    /* Painters stay until removed. Registered after the first band so the cells exist. */
    ui_hints_set_gfx_cb(HintPress1, gb_hint_transport, m);
    ui_hints_set_gfx_cb(HintPress3, gb_hint_param, m);
    ui_hints_show(true);
    return true;
}

/* No deinit: the firmware stops and unhooks the engine itself, and the model is all we own. */
TAPP_APP("grooves", gb_model_t, gb_init, NULL,
         .redraw = gb_redraw,
         .tick = gb_tick,
         .on_input = gb_input,
         .engine_cb = &gb_engine);

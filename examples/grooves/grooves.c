// grooves.c — generative 6-track groovebox SOURCE tapp: renders at 24k into its own
// scratch, upsamples into the mixer in_bus with ups2x_f32_add(), so the tapedecks
// record it like the radio. (The firmware does no rate conversion — a source tapp
// always gets the real 48k bus and owns its own internal rate.)
// Controls: BTN1=play/stop (custom glyph hint)   BTN2=shuffle   BTN3=param readout (no action)
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
#define TRACKS      6
#define TOP_BAR     26

enum { T_KICK = 0, T_SNARE, T_HAT, T_BASS, T_LEAD, T_PAD };

#define INF_SX    96
#define INF_SY    36
#define INF_X     70                            // 70..166 — center-left, clear of metro and params
#define INF_Y     40                            // 50..86, below the hint band, above the grid
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
enum { ENC_MORPH = 0, ENC_DRIFT, ENC_TRACKS, ENC_PUMP,  ENC_DENS, ENC_TONE, ENC_KEY, ENC_SCALE,
    ENC_TEMPO, ENC_TOTAL };

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

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

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

static const float LVL[TRACKS]       = { 0.95f, 0.50f, 0.20f, 0.62f, 0.32f, 0.22f };
static const float DENS_DFLT[TRACKS] = { 0.85f, 0.55f, 0.70f, 0.62f, 0.50f, 0.75f };
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
    uint32_t k_ph;  float k_f, k_f0, k_env, k_click, k_dec, k_drv, k_h2, k_h4, k_nrm;
    uint32_t s_ph1, s_ph2, s_inc1, s_inc2;
    float    s_nenv, s_tenv, s_tr, s_ndec, s_lp, s_bp, s_bpf, s_nmix;
    uint32_t h_ph[6], h_inc[6];  float h_env, h_dec, h_lp, h_bp, h_f;
    uint32_t b_ph, b_sph, b_inc, b_sinc;
    float    b_rinc, b_env, b_fenv, b_amp, b_lp, b_bp, b_q, b_fd, b_dec, b_sub;
    uint32_t l_pm, l_ps, l_incm, l_incs;
    float    l_rincs, l_ratio, l_f, l_tgt, l_env, l_dec, l_lp, l_la;
    uint8_t  l_prev;                        // lead played last step -> glide
    uint32_t p_ph[6], p_inc[6], p_lfo;
    float    p_rinc[6], p_f[3], p_tgt[3], p_env, p_gate, p_ac;
    float    p_det, p_atk, p_rel, p_bright;   // per-chord voicing, from the node axes
    float    p_lpL, p_bpL, p_lpR, p_bpR, p_hpL, p_hpR, p_fc;

    float   dcx_l, dcy_l, dcx_r, dcy_r;
    float   dens[TRACKS];                   // per-track density — the dens PARAM is a macro over these

    uint8_t fig_base[FIG_H * FIG_BPR];      // baked INF_TH path stroke (band-local bits)
    uint8_t fig_fat[FIG_H * FIG_BPR];       // baked INF_TH+4 stroke (selected halo)
    uint16_t arc_lut[256];                  // uniform arc fraction -> raw lemniscate t (gb_init)
    ui_roll_text_t enc_roll;                // mechanical roll for the enc-target name
    ui_roll_text_t val_roll;                // mechanical roll for the key/scale VALUE text
    char key_txt[8];                        // persistent note-name text the value roll diffs against
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

static gb_model_t* g_model = 0;

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
    {  3,  5,  8, 11 },   // bass
    {  2,  3,  5,  7 },   // lead
    {  1,  2,  2,  4 },   // pad
};
static const uint8_t ROT0[TRACKS] = { 0, 4, 0, 0, 2, 0 };

static const int8_t PROG[4][4] = {
    { 0, 5, 3, 4 },   // i  VI  IV  V
    { 0, 3, 4, 4 },   // i  IV  V   V
    { 0, 6, 5, 4 },   // i VII  VI  V
    { 0, 2, 5, 3 },   // i III  VI  IV
};

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

    const int lo = (tr == T_LEAD) ? -2 : -5;
    const int hi = (tr == T_LEAD) ? 12 :  7;
    int deg = lo + (int)(urand(m) % (uint32_t)(hi - lo + 1));

    for (int s = 0; s < STEPS; s++) {
        if (!hit[s])            pd[s] = 0;
        else if ((s & 3) == 0)  pd[s] = 255;                              // on the beat
        else if (s & 1)         pd[s] = 120 + (uint8_t)(urand(m) % 60);   // ghost
        else                    pd[s] = 200;

        if (tr <= T_HAT) {
            td[s] = drum_tune(m, tr, node, s);
        } else if (tr == T_PAD) {
            td[s] = PROG[node & 3][(s >> 2) & 3];
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

/* Transparent stencil blit: SET bits only, as hline runs in the CURRENT color
 * (there is no transparent xbm blit in the API — gfx_draw_xbm overwrites the
 * whole box). Runs keep it cheap: a few hlines per row. */
static void gb_blit(gfx_t* gfx, const uint8_t* s) {
    for (int row = 0; row < FIG_H; row++) {
        const uint8_t* b = &s[row * FIG_BPR];
        const gfx_uint_t y = (gfx_uint_t)(INF_Y - INF_PAD + row);
        int x = 0;
        while (x < FIG_W) {
            if (b[x >> 3] == 0) { x = (x & ~7) + 8; continue; }
            if (!(b[x >> 3] & (1u << (x & 7)))) { x++; continue; }
            const int x0 = x;
            while (x < FIG_W && (b[x >> 3] & (1u << (x & 7)))) x++;
            gfx_draw_hline(gfx, (gfx_uint_t)(INF_X - INF_PAD + x0), y, (gfx_uint_t)(x - x0));
        }
    }
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
    if (tr >= T_BASS) *tune = (float)bestv;
    else              *tune = (si > 0.5f) ? (sT / si) : 0.0f;
}

static int steps_to_next(const gb_model_t* m, const morph_w_t* w, int tr, int from) {
    const int L = m->plen[m->live][tr] ? m->plen[m->live][tr] : STEPS;
    for (int g = 1; g <= L; g++) {
        float inten, tn;
        step_cell(m, w, tr, (from + g) % L, &inten, &tn);
        if (inten > 1.0f - m->dens[tr]) return g;
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

// ----------------------------------------------------------------------------
// Voice triggers
// ----------------------------------------------------------------------------
static void trig_kick(gb_model_t* m, float vel, float c, float tone, float np, float nd) {
    const float j = 1.0f + (frand(m) - 0.5f) * 0.03f;
    m->k_f0    = drum_pitch(42.0f, np * 8.0f + c * 7.0f + tone_semis(tone)
                                  + drum_octave(m, 0.85f, 40u), j);
    m->k_f     = m->k_f0 * (3.2f + 1.6f * c);
    const float lo = clampf((42.0f - m->k_f0) * (1.0f / 31.5f), 0.f, 1.f);   /* 0 at 42Hz, 1 at 10.5 */
    m->k_h2    = 0.55f * lo;
    m->k_h4    = 0.40f * lo * lo;
    m->k_nrm   = 1.0f / (1.0f + m->k_h2 + m->k_h4);
    m->k_ph    = 0x40000000u;
    m->k_env   = vel;
    m->k_click = vel * (0.16f + 0.10f * (c + 1.0f)) * (1.0f - 0.55f * lo);
    m->sc_env  = 1.0f;                 // full pump on every kick
    m->k_dec   = decay_coef((150.0f + 110.0f * c) * (1.0f + nd * 0.8f)
                            * (0.92f + 0.16f * frand(m)));
     m->k_drv   = 1.7f + 0.7f * lo;
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
    m->s_ndec = decay_coef((26.0f + 17.0f * (c + 1.0f)) * (1.0f + nd * 0.7f));
    m->s_bpf  = svf_f(1200.0f + 1100.0f * (c + 1.0f) + 900.0f * tone);
    m->s_nmix = 0.55f + 0.225f * (c + 1.0f);
}

static void trig_hat(gb_model_t* m, float vel, float c, bool open, float tone,
                     float np, float nd) {
    const float f0 = drum_pitch(263.0f, np * 7.0f + c * 6.0f + tone_semis(tone)
                                       + drum_octave(m, 0.5f, 30u),
                               1.0f + (frand(m) - 0.5f) * 0.05f);
    for (int k = 0; k < 6; k++) m->h_inc[k] = HZ2INC(f0 * HAT_RATIO[k]);
    m->h_env = vel;
    m->h_dec = decay_coef((open ? (90.0f + 120.0f * c) : (16.0f + 11.0f * (c + 1.0f)))
                          * (1.0f + nd * 0.7f));
    m->h_f   = svf_f(5200.0f + 2200.0f * c + 2200.0f * tone);
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

static void trig_lead(gb_model_t* m, int semi, bool acc, bool glide, float tone,
                      float np, float nd) {
    m->l_tgt = note_to_freq((float)semi);
    if (!glide) m->l_f = m->l_tgt;                       
    m->l_env   = 1.0f;
    m->l_dec   = decay_coef((acc ? 260.0f : 130.0f) * (1.0f + nd * 0.8f));
    m->l_la    = clampf((acc ? 0.55f : 0.28f) * (1.0f + np * 0.45f)
                        * (0.8f + frand(m) * 0.4f), 0.08f, 0.85f);
    m->l_ratio = clampf(1.0f + tone * 2.5f + (acc ? 0.45f : 0.0f) + np * 0.9f
                        + (frand(m) - 0.5f) * 0.7f, 1.0f, 4.2f);
}

static void trig_pad(gb_model_t* m, int deg, int sc, int base, float np, float nd) {
    m->p_det    = 0.0035f * (1.0f + np * 0.8f) * (0.7f + frand(m) * 0.6f);
    m->p_bright = np;
    m->p_atk    = 1.736e-4f / (1.0f + nd * 0.8f);
    m->p_rel    = 2.98e-5f  / (1.0f + nd * 0.8f);
    for (int v = 0; v < 3; v++) {
        int semi = base + deg2semi(sc, deg + v * 2);
        while (semi > base + 14) semi -= 12;
        while (semi < base)      semi += 12;
        m->p_tgt[v] = note_to_freq((float)semi);
    }
}

// ----------------------------------------------------------------------------
// Sequencer
// ----------------------------------------------------------------------------
static void fire_step(gb_model_t* m, const morph_w_t* w) {
    const int   sc   = (int)gb_prm[ENC_SCALE].val & 3;
    const int   root = (int)gb_prm[ENC_KEY].val;
    const float tone = clampf(gb_prm[ENC_TONE].val, 0.f, 1.f);
    const int   buf  = m->live;
    const float step_ms = 15000.0f / fmaxf(gb_prm[ENC_TEMPO].val, 1.0f);
    uint8_t fired = 0;

    for (int t = 0; t < TRACKS; t++) {
        float inten, tn;
        step_cell(m, w, t, m->tstep[t], &inten, &tn);
        if (inten <= 1.0f - m->dens[t]) {
            if (t == T_LEAD) m->l_prev = 0;
            continue;
        }
        fired |= (uint8_t)(1u << t);

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
        case T_LEAD:  trig_lead (m, root + 19 + deg2semi(sc, deg), acc, m->l_prev != 0, tone,
                                 node_char(m->npit[buf], w, t), node_char(m->ndec[buf], w, t));
                      m->l_prev = 1; break;
        case T_PAD:   trig_pad  (m, deg, sc, root + 24,
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
static void gb_process(engine_t* engine, mixer_t* mix) {
    gb_model_t* m = engine_get_ctx(engine);
    if (!m) return;
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
    const float tone = clampf(gb_prm[ENC_TONE].val, 0.f, 1.f);
    const float kcd  = decay_coef(2.5f);    // kick click
    const float kpd  = decay_coef(10.f);    // kick pitch sweep
    const float sdt  = decay_coef(90.f);    // snare tone
    const float srd  = decay_coef(1.5f);    // snare transient
    const float bfd  = decay_coef(90.f);    // bass filter
    const float b_f0 = svf_f(70.0f + 180.0f * tone);

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
    const float g_l = (nt > T_LEAD)  ? LVL[T_LEAD]  : 0.f;
    const float g_p = (nt > T_PAD)   ? LVL[T_PAD]   : 0.f;
    const float pmp   = clampf(gb_prm[ENC_PUMP].val, 0.f, 1.f);
    const float d_in  = 2.6f * pmp;            // 0 .. 2.6 (pins at SC_FLOOR past 1)
    const float d_pad = fmaxf(0.55f, d_in);    // 0.55 .. 2.6
    const float d_bas = fmaxf(0.25f, d_in);    // 0.25 .. 2.6
    m->sc_dec = decay_coef(180.0f + 80.0f * pmp);

    morph_w_t w; morph_weights(m, morph_phase(m), &w);

    const uint32_t frames = n24 >> 1;
    m->p_lfo += (uint32_t)(frames * 19676u);                // ~0.11 Hz
    const float lfo = fsin_i32((int32_t)m->p_lfo) * 0.5f + 0.5f;
    m->p_fc = svf_f(900.0f + 700.0f * lfo + 500.0f * tone + 600.0f * m->p_bright);
    m->p_ac = (m->p_gate > m->p_env) ? m->p_atk : m->p_rel;

    m->l_f += (m->l_tgt - m->l_f) * 0.058f;                 // portamento, ~46 ms
    m->l_incm  = HZ2INC(m->l_f);
    m->l_incs  = HZ2INC(m->l_f * m->l_ratio);
    m->l_rincs = m->l_incs ? 1.0f / (float)m->l_incs : 0.0f;

    for (int v = 0; v < 3; v++) {                           // pad chord glide, ~41 ms
        m->p_f[v] += (m->p_tgt[v] - m->p_f[v]) * 0.0645f;
        m->p_inc[2 * v]     = HZ2INC(m->p_f[v] * (1.0f - m->p_det));
        m->p_inc[2 * v + 1] = HZ2INC(m->p_f[v] * (1.0f + m->p_det));
        m->p_rinc[2 * v]     = m->p_inc[2 * v]     ? 1.0f / (float)m->p_inc[2 * v]     : 0.0f;
        m->p_rinc[2 * v + 1] = m->p_inc[2 * v + 1] ? 1.0f / (float)m->p_inc[2 * v + 1] : 0.0f;
    }

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
        float kick = fsin_i32((int32_t)m->k_ph)
                   + fsin_i32((int32_t)(m->k_ph << 1)) * m->k_h2
                   + fsin_i32((int32_t)(m->k_ph << 2)) * m->k_h4;
        kick *= m->k_nrm * m->k_env;
        kick += nrand(m) * m->k_click;
        kick  = sat1(kick * m->k_drv);
        m->sc_env *= m->sc_dec;
        const float sc    = m->sc_env;
        const float duck  = fmaxf(1.0f - d_pad * sc, SC_FLOOR);
        const float duckb = fmaxf(1.0f - d_bas * sc, SC_FLOOR);
        m->k_env *= m->k_dec;  m->k_click *= kcd;

        // ---- SNARE ----
        const float nz = nrand(m);
        SVF(nz, m->s_lp, m->s_bp, m->s_bpf, 0.45f);
        m->s_ph1 += m->s_inc1;  m->s_ph2 += m->s_inc2;
        const float tonal = (fsin_i32((int32_t)m->s_ph1) * 0.6f +
                             fsin_i32((int32_t)m->s_ph2) * 0.4f) * m->s_tenv;
        const float snare = m->s_bp * m->s_nmix * m->s_nenv + tonal * 0.55f + nz * m->s_tr * 0.35f;
        m->s_nenv *= m->s_ndec;  m->s_tenv *= sdt;  m->s_tr *= srd;

        // ---- HAT ----
        uint32_t hacc = 0;
        for (int k = 0; k < 6; k++) { m->h_ph[k] += m->h_inc[k]; hacc += m->h_ph[k] >> 31; }
        const float metal = (float)hacc * (1.0f / 3.0f) - 1.0f;
        SVF(metal, m->h_lp, m->h_bp, m->h_f, 0.55f);
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

        // ---- LEAD ----
        const uint32_t lpm = m->l_pm + m->l_incm;
        if (lpm < m->l_incm) m->l_ps = (uint32_t)((float)lpm * m->l_ratio);
        else                 m->l_ps += m->l_incs;
        m->l_pm = lpm;
        const float lsaw = saw_u32(m->l_ps) - blep_u32(m->l_ps, m->l_incs, m->l_rincs);
        m->l_lp += (lsaw - m->l_lp) * m->l_la;
        const float lead = m->l_lp * m->l_env;
        m->l_env *= m->l_dec;

        // ---- PAD  ----
        float padl = 0.f, padr = 0.f;
        for (int k = 0; k < 6; k++) {
            m->p_ph[k] += m->p_inc[k];
            const float s = saw_u32(m->p_ph[k]) - blep_u32(m->p_ph[k], m->p_inc[k], m->p_rinc[k]);
            if (k & 1) padr += s; else padl += s;
        }
        padl *= (1.0f / 3.0f);  padr *= (1.0f / 3.0f);
        SVF(padl, m->p_lpL, m->p_bpL, m->p_fc, 0.7f);
        SVF(padr, m->p_lpR, m->p_bpR, m->p_fc, 0.7f);
        m->p_hpL += (m->p_lpL - m->p_hpL) * 0.0249f;
        m->p_hpR += (m->p_lpR - m->p_hpR) * 0.0249f;
        m->p_env += (m->p_gate - m->p_env) * m->p_ac;
        const float pg = m->p_env * duck;
        padl = (m->p_lpL - m->p_hpL) * pg;
        padr = (m->p_lpR - m->p_hpR) * pg;

        const float mid  = g_k * kick + g_b * duckb * bass + g_s * snare + g_h * hat + g_l * lead;
        const float side = 0.10f * g_s * snare - 0.22f * g_h * hat + 0.26f * g_l * lead;
        float l = mid - side + g_p * padl;
        float r = mid + side + g_p * padr;

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
    m->l_env = dnz(m->l_env);   m->l_lp = dnz(m->l_lp);
    m->p_env = dnz(m->p_env);
    m->p_lpL = dnz(m->p_lpL);   m->p_bpL = dnz(m->p_bpL);
    m->p_lpR = dnz(m->p_lpR);   m->p_bpR = dnz(m->p_bpR);
    m->p_hpL = dnz(m->p_hpL);   m->p_hpR = dnz(m->p_hpR);
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
    m->l_env = m->l_lp = 0.f;  m->l_prev = 0;
    m->p_env = 0.f; m->p_gate = 0.f;
    m->p_lpL = m->p_bpL = m->p_lpR = m->p_bpR = m->p_hpL = m->p_hpR = 0.f;
    m->dcx_l = m->dcy_l = m->dcx_r = m->dcy_r = 0.f;
    m->sc_env = 0.f;
}

static void gb_set_playing(gb_model_t* m, bool state) {
    m->playing = state;
    if (state) m->p_gate = 1.0f;
    else       voices_silence(m);
}

static void gb_eng_active(engine_t* e, bool state) { (void)e; if (g_model) gb_set_playing(g_model, state); }
static uint_fast8_t gb_eng_is_active(engine_t* e) { (void)e; return (g_model && g_model->playing) ? 1 : 0; }
static void eng_noop_core(os_core_t* c) { (void)c; }
static void eng_noop(engine_t* e) { (void)e; }

static const engine_callbacks_t gb_engine = {
    .init = eng_noop_core, .process = gb_process, .defaults = eng_noop, .cleanup = eng_noop_core,
    .active = gb_eng_active, .is_active = gb_eng_is_active,
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

        const float thr = 1.0f - m->dens[row];
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
        const gfx_uint_t mx = (gfx_uint_t)(INF_X + (int)(nx * INF_SX + 0.5f));
        const gfx_uint_t my = (gfx_uint_t)(INF_Y + (int)(ny * INF_SY + 0.5f));
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
#define VAL_X     199                           
#define VAL_W     93
#define VAL_Y     26

static void draw_tri(gfx_t* g, int x, int y, int h, int w) {
    for (int i = 0; i < h; i++) {
        const int d = (i < h / 2) ? i : (h - 1 - i);
        gfx_draw_hline(g, (gfx_uint_t)x, (gfx_uint_t)(y + i),
                       (gfx_uint_t)(1 + (2 * d * (w - 1)) / (h - 1)));
    }
}

static void draw_tri_up(gfx_t* g, int cx, int y, int h, int w, int wt) {
    for (int i = 0; i < h; i++) {
        const int ww = wt + (i * (w - wt)) / (h - 1);
        gfx_draw_hline(g, (gfx_uint_t)(cx - ww / 2), (gfx_uint_t)(y + i), (gfx_uint_t)ww);
    }
}

static void draw_metro(gfx_t* gfx, int cx, int y, bool playing, float bpm) {
    gfx_set_color(gfx, 1);
    draw_tri_up(gfx, cx, y + 4, 28, 29, 5);
    gfx_set_color(gfx, 0);
    draw_tri_up(gfx, cx, y + 12, 16, 13, 5);
    gfx_fill_rect_dithered(gfx, (gfx_uint_t)(cx - 16), (gfx_uint_t)y, 33, 33, UI_HALFTONE+2);
    gfx_set_color(gfx, 1);

    int dx = -6;
    if (playing) {
        const uint32_t per = (uint32_t)(60000.0f / (bpm > 1.f ? bpm : 1.f));
        const uint32_t p   = per ? (os_tick_get() % per) : 0u;
        const uint32_t ph  = ((p << 16) / (per ? per : 1u)) << 16;
        dx = (int)(fsin_i32((int32_t)ph) * 9.0f);
    }
    gfx_draw_line_thick(gfx, (gfx_uint_t)cx, (gfx_uint_t)(y + 26),
                             (gfx_uint_t)(cx + dx), (gfx_uint_t)(y + 6), 4);
}

/* Value text, CENTERED under the BTN3 cell. The roll is right-aligned, so the
 * centered right edge comes from the settled string's width. */
static void gb_val_text(gfx_t* gfx, gb_model_t* m, const char* s) {
    gfx_set_font(gfx, gfx_nunito_semibold_14);
    const gfx_uint_t sw = gfx_get_str_width(gfx, s);
    ui_roll_text_draw(gfx, (gfx_uint_t)(VAL_X + (VAL_W + sw) / 2), VAL_Y + 17, &m->val_roll, s);
}

static void gb_params_draw(gfx_t* gfx, gb_model_t* m) {
    params_t* const p = &gb_prm[m->enc_target];
    gfx_set_color(gfx, 1);
    draw_metro(gfx, 26, 32, m->playing, gb_prm[ENC_TEMPO].val);
    gfx_set_font(gfx, gfx_nunito_semibold_14);
    {
        const int bpm = (int)(gb_prm[ENC_TEMPO].val + 0.5f);
        const gfx_uint_t bw = gfx_get_str_width(gfx, bpm > 99 ? "000" : "00");
        gfx_draw_strf(gfx, (gfx_uint_t)(26 - bw / 2), 85, "%d", bpm); // metro ends y67 + 4px pad
    }

    if (m->enc_target == ENC_MORPH) {
        ui_roll_text_snap(&m->val_roll, "");
    } else if (m->enc_target == ENC_KEY) {
        const int mid = (int)p->val;
        snprintf(m->key_txt, sizeof(m->key_txt), "%s%d", NOTE_NAME[mid % 12], mid / 12 - 1);
        gb_val_text(gfx, m, m->key_txt);
    } else if (m->enc_target == ENC_SCALE) {
        gb_val_text(gfx, m, p->option_name[(int)p->val & 3]);
    } else {
        ui_roll_text_snap(&m->val_roll, "");
        const float span = p->max - p->min;
        const uint32_t pc = (span > 0.f) ? (uint32_t)((p->val - p->min) / span * 1000.f) : 0u;
        ui_draw_value_bar(gfx, VAL_X, VAL_Y + 4, VAL_W, 14, pc, 0, 1000);
    }
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
static void gb_hint_transport(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, void* user) {
    (void)w;
    gb_model_t* m = (gb_model_t*)user;
    if (!m) return;
    gfx_set_color(gfx, m->playing ? 0 : 1);
    const int gy = (int)y + 11;
    if (m->playing) draw_tri(gfx, (int)x + 8, gy - 6, 13, 11);
    else            gfx_draw_rect_fill(gfx, (gfx_uint_t)(x + 8), (gfx_uint_t)(gy - 5), 10, 10);
    gfx_set_font(gfx, gfx_nunito_semibold_14); // cb contract: set your own font for text
    gfx_draw_str(gfx, (gfx_uint_t)(x + 34), (gfx_uint_t)(y + 17), m->playing ? "stop" : "play");
    gfx_set_color(gfx, 1);
}


static void gb_hint_param(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, void* user) {
    gb_model_t* m = (gb_model_t*)user;
    if (!m) return;
    gfx_set_color(gfx, 1);
    gfx_set_font(gfx, gfx_nunito_semibold_14); // cb contract: set your own font for text
    const char* const nm = gb_prm[m->enc_target].name;
    const gfx_uint_t sw = gfx_get_str_width(gfx, nm);
    ui_roll_text_draw(gfx, (gfx_uint_t)(x + (w + sw) / 2), (gfx_uint_t)(y + 17), &m->enc_roll, nm);
}

static void gb_hints(gb_model_t* m) {
    static tapp_hint_pair_t hints[5];
    hints[0] = (tapp_hint_pair_t){ "", 0 };         // custom-drawn (gb_hint_transport)
    hints[1] = (tapp_hint_pair_t){ "shuffle", 0 };
    hints[2] = (tapp_hint_pair_t){ "", 0 };         // custom-drawn (gb_hint_param)
    /* BTN4/5: both rows are SYSTEM-owned for input tapps (fx-menu prev/next +
     * back/exit icons) — labels here are ignored. */
    hints[3] = (tapp_hint_pair_t){ 0, 0 };
    hints[4] = (tapp_hint_pair_t){ 0, 0 };
    ui_hints_set_labels(hints);
}

// ----------------------------------------------------------------------------
// Tick
// ----------------------------------------------------------------------------
static bool gb_tick(os_app_t* app) {
    gb_model_t* m = os_app_get_model(app);
    const int32_t d = os_controls_encoder_get_delta();
    if (d) gb_enc_apply(m, d);
    param_update_val(&gb_prm[m->enc_target]);   // menu parity: ease val toward target

    /* dens is a MACRO over the per-track array (each track keeps its own
     * default): spread the param's movement as a delta, clamped per track. */
    static float dens_prev = 0.85f;             // == gb_prm[ENC_DENS].dflt
    const float dv = gb_prm[ENC_DENS].val;
    if (dv != dens_prev) {
        const float s = dv - dens_prev;
        for (int t = 0; t < TRACKS; t++) m->dens[t] = clampf(m->dens[t] + s, 0.f, 1.f);
        dens_prev = dv;
    }

    return true;
}

// ----------------------------------------------------------------------------
// Input
// ----------------------------------------------------------------------------
static void gb_input(os_app_t* app, uint8_t btn, KeyStateEnum st) {
    gb_model_t* m = os_app_get_model(app);
    if (btn >= 5) return;

    if (btn == 1) {
        if (st != KEY_STATE_PRESSED) return;
        gb_regen(m, os_tick_get() ^ (uint32_t)(uintptr_t)m);
        ui_statusbar_hint_event(HintPress2);
        return;
    }

    if (st != KEY_STATE_RELEASED) return;

    switch (btn) {
    case 0:
        engine_set_active(!m->playing);
        ui_hints_set_active(HintPress1, m->playing); // lit plate while playing
        break;
    /* BTN2 handled above; BTN3 is a readout cell, not a button. */
    case 3: m->enc_target = (uint8_t)((m->enc_target + ENC_TOTAL - 1) % ENC_TOTAL); break;
    case 4: m->enc_target = (uint8_t)((m->enc_target + 1) % ENC_TOTAL); break;
    default: break;
    }
}

// ----------------------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------------------

static bool gb_init(os_app_t* app, va_list args) {
    (void)args;
    gb_model_t* m = os_app_get_model(app);
    g_model = m; // audio callbacks reach the model through this

    m->xs = 0x9e3779b9u;
    m->playing = false;

    for (int t = 0; t < TRACKS; t++) m->dens[t] = DENS_DFLT[t];

    gb_regen(m, os_tick_get() ^ (uint32_t)(uintptr_t)m);
    m->live ^= 1u; m->regen_req = 0;
    m->k_f = m->k_f0 = 42.f;
    m->b_inc = HZ2INC(55.f); m->b_rinc = 1.0f / (float)m->b_inc; m->b_sinc = m->b_inc >> 1;
    m->b_q = 0.55f; m->b_fd = 0.30f;
    m->l_f = m->l_tgt = 330.f; m->l_ratio = 1.6f; m->l_la = 0.28f;
    m->l_incs = HZ2INC(m->l_f * m->l_ratio); m->l_rincs = 1.0f / (float)m->l_incs;
    m->l_incm = HZ2INC(m->l_f); m->l_dec = decay_coef(130.f);
    m->h_dec = decay_coef(32.f); m->h_f = svf_f(6000.f);
    m->s_bpf = svf_f(2500.f); m->s_nmix = 0.7f; m->s_ndec = decay_coef(40.f);
    m->p_fc = svf_f(1200.f);
    m->b_dec = decay_coef(200.f);
    trig_pad(m, 0, 0, 33 + 24, 0.f, 0.f);
    for (int v = 0; v < 3; v++) m->p_f[v] = m->p_tgt[v];
    m->p_gate = 1.0f;
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

    gb_hints(m);
    ui_hints_set_gfx_cb(HintPress1, gb_hint_transport, m);
    ui_hints_set_gfx_cb(HintPress3, gb_hint_param, m);
    ui_hints_show(true);
    engine_set_callbacks(app, m);
    engine_set_active(true);
    /* engine_set_active() dispatches our .active callback (gb_eng_active), which
     * would START the transport. Land stopped instead: the engine slot stays
     * installed and dispatching, and gb_process renders silence while stopped —
     * the same state a BTN1 stop leaves behind. */
    gb_set_playing(m, false);
    ui_hints_set_active(HintPress1, false);     // plate lights only while playing
    return true;
}

static bool gb_deinit(os_app_t* app) {
    gb_model_t* m = os_app_get_model(app);
    g_model = 0;
    m->playing = false;
    engine_set_active(false);
    engine_clear_callbacks(&gb_engine);
    return true;
}

static os_app_data_t gb_data = {
    .model = NULL, .model_size = sizeof(gb_model_t), .init = gb_init, .deinit = gb_deinit,
};

static os_app_t gb_app = {
    .name = "grooves",
    .type = AppFullscreenType,
    .data = &gb_data,
    .redraw = (os_app_redraw)gb_redraw,
    .tick = gb_tick,
    .on_input = gb_input,
    .engine_cb = &gb_engine,
};

os_app_t* tapp_get_descriptor(void) { return &gb_app; }

// infinigroove.c — generative 6-track groovebox TAPP.
// Controls: BTN1=play/stop, hold reveals the hints
//           BTN4=enc target back / hold exit   BTN5=enc target fwd / hold randomize
//           ENC drives the selected target: morph drift tempo dens tone key scale.

#include "tapp_api.h"

// ----------------------------------------------------------------------------
// Config
// ----------------------------------------------------------------------------
#define SR          48000.0f
#define STEPS       16           // max steps per track
#define GRID        4            // 4x4 morph node grid
#define NODES       (GRID * GRID)
#define TRACKS      6
#define TOP_BAR     26

enum { T_KICK = 0, T_SNARE, T_HAT, T_BASS, T_LEAD, T_PAD };

#define INF_SX    120
#define INF_SY    42
#define INF_X     ((400 - INF_SX) / 2)          // 140..260 — centred on the panel
#define INF_Y     36                            // 36..78, between statusbar and grid
#define INF_TH     11                           // stroke thickness
#define INF_DOT_R  4                            // travelling marker radius (9px)
#define UI_HALFTONE 4                           // 50% halftone — shared by the figure-8 and metronome
#define INF_PAD   (INF_TH / 2 + 2)
#define PATH_N    200                           // 2px sampling of a 330px path needs 160
#define PATH_GAP2  4

// Encoder targets — BTN4/BTN5 step through these
enum { ENC_MORPH = 0, ENC_DRIFT, ENC_TEMPO, ENC_DENS, ENC_TONE, ENC_KEY, ENC_SCALE, ENC_TOTAL };
static const char* const ENC_NAME[ENC_TOTAL] = {
    "morph", "drift", "tempo", "dens", "tone", "key", "scale"
};


#define HZ2INC(f)  ((uint32_t)((f) * 89478.485f))   /* f * 2^32 / SR */
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
    return fminf(2.0f * fsin_i32((int32_t)(uint32_t)(hz * 44739.24f)), 1.2f);
}

static inline float blep_u32(uint32_t ph, uint32_t inc, float rinc) {
    if (ph < inc) { const float t = (float)ph * rinc; return t * (2.0f - t) - 1.0f; }
    const uint32_t d = 0u - ph;
    if (d < inc)  { const float u = 1.0f - (float)d * rinc; return u * u; }
    return 0.0f;
}

static inline float decay_coef(float ms) { return 1.0f - 1.0f / (fmaxf(ms, 0.05f) * (SR / 1000.0f)); }
static inline float dnz(float x) { return (x < 1e-15f && x > -1e-15f) ? 0.0f : x; }
static float note_to_freq(float midi) { return 440.0f * fast_exp2f((midi - 69.0f) * (1.0f / 12.0f)); }

static const int8_t SCALE[4][7] = {
    { 0, 2, 3, 5, 7, 8, 10 },   // natural minor
    { 0, 2, 3, 5, 7, 9, 10 },   // dorian
    { 0, 1, 3, 5, 7, 8, 10 },   // phrygian
    { 0, 2, 4, 5, 7, 9, 11 },   // major
};
static const char* const SCALE_NAME[4] = { "minor", "dorian", "phrygian", "major" };
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
    uint16_t morph;                         // WRAPS — the path is a closed loop
    uint8_t  enc_target;
    uint8_t  held_mask;                     // bit per button: a hold fired, so the release is not a tap
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
    uint32_t k_ph;  float k_f, k_f0, k_env, k_click, k_dec;
    uint32_t s_ph1, s_ph2, s_inc1, s_inc2;
    float    s_nenv, s_tenv, s_tr, s_ndec, s_lp, s_bp, s_bpf, s_nmix;
    uint32_t h_ph[6], h_inc[6];  float h_env, h_dec, h_lp, h_bp, h_f;
    uint32_t b_ph, b_sph, b_inc, b_sinc;
    float    b_rinc, b_env, b_fenv, b_amp, b_lp, b_bp, b_q, b_fd, b_dec;
    uint32_t l_pm, l_ps, l_incm, l_incs;
    float    l_rincs, l_ratio, l_f, l_tgt, l_env, l_dec, l_lp, l_la;
    uint8_t  l_prev;                        // lead played last step -> glide
    uint32_t p_ph[6], p_inc[6], p_lfo;
    float    p_rinc[6], p_f[3], p_tgt[3], p_env, p_gate, p_ac;
    float    p_det, p_atk, p_rel, p_bright;   // per-chord voicing, from the node axes
    float    p_lpL, p_bpL, p_lpR, p_bpR, p_hpL, p_hpR, p_fc;

    float   dcx_l, dcy_l, dcx_r, dcy_r;
    float   bpm, root, tone, drift;
    float   dens[TRACKS];
    uint8_t scale;                          

    uint8_t path_x[PATH_N], path_y[PATH_N], path_n;
    uint16_t sb_enc_x, sb_bar_x, sb_bar_w;
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
            td[s] = (int8_t)(deg + ((((r >> 16) & 7) == 0) ? 7 : 0));
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
static void morph_xy(uint16_t morph, float* fx, float* fy) {
    const uint32_t t = (uint32_t)morph << 16;
    const float s = fsin_i32((int32_t)t);
    const float c = fsin_i32((int32_t)(t + 0x40000000u));   // cos
    const float d = 1.0f / (1.0f + s * s);
    *fx = 0.5f + 0.5f * c * d;
    *fy = 0.5f + 1.4142136f * s * c * d;
}

static void morph_weights(uint16_t morph, morph_w_t* w) {
    float nx, ny; morph_xy(morph, &nx, &ny);
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

static float node_char(const int8_t tab[TRACKS][NODES], const morph_w_t* w, int tr) {
    float s = 0.f;
    for (int k = 0; k < 4; k++) s += (float)tab[tr][w->n[k]] * w->w[k];
    return clampf(s * (1.0f / 48.0f), -1.f, 1.f);
}

static inline float drum_pitch(float base_hz, float semis, float jitter) {
    const int st = (int)(semis + (semis < 0.f ? -0.5f : 0.5f));
    return base_hz * fast_exp2f((float)st * (1.0f / 12.0f)) * jitter;
}

static inline float drum_octave(gb_model_t* m, float bias, uint32_t odds) {
    if ((xrand(m) >> 24) >= odds) return 0.0f;
    return ((frand(m) < bias) ? -12.0f : 12.0f);
}

// ----------------------------------------------------------------------------
// Voice triggers
// ----------------------------------------------------------------------------
static void trig_kick(gb_model_t* m, float vel, float c, float np, float nd) {
    const float j = 1.0f + (frand(m) - 0.5f) * 0.03f;
    m->k_f0    = drum_pitch(42.0f, np * 8.0f + c * 7.0f
                                  + drum_octave(m, 0.85f, 40u), j);
    m->k_f     = m->k_f0 * (3.2f + 1.6f * c);
    m->k_ph    = 0x40000000u;
    m->k_env   = vel;
    m->k_click = vel * (0.16f + 0.10f * (c + 1.0f));
    m->k_dec   = decay_coef((150.0f + 110.0f * c) * (1.0f + nd * 0.8f)
                            * (0.92f + 0.16f * frand(m)));
}

static void trig_snare(gb_model_t* m, float vel, float c, float tone, float np, float nd) {
    const float j = 1.0f + (frand(m) - 0.5f) * 0.03f;
    const float f = drum_pitch(180.0f, np * 8.0f + c * 7.0f
                                      + drum_octave(m, 0.5f, 30u), j);
    m->s_inc1 = HZ2INC(f);  m->s_inc2 = HZ2INC(f * 1.79f);
    m->s_ph1  = 0; m->s_ph2 = 0;
    m->s_nenv = vel;
    m->s_tenv = vel * (0.85f - 0.35f * c);
    m->s_tr   = vel;
    m->s_ndec = decay_coef((26.0f + 17.0f * (c + 1.0f)) * (1.0f + nd * 0.7f));
    m->s_bpf  = svf_f(1200.0f + 1100.0f * (c + 1.0f) + 900.0f * tone);
    m->s_nmix = 0.55f + 0.225f * (c + 1.0f);
}

static void trig_hat(gb_model_t* m, float vel, float c, bool open, float tone,
                     float np, float nd) {
    const float f0 = drum_pitch(263.0f, np * 7.0f + c * 6.0f + drum_octave(m, 0.5f, 30u),
                               1.0f + (frand(m) - 0.5f) * 0.05f);
    for (int k = 0; k < 6; k++) m->h_inc[k] = HZ2INC(f0 * HAT_RATIO[k]);
    m->h_env = vel;
    m->h_dec = decay_coef((open ? (90.0f + 120.0f * c) : (16.0f + 11.0f * (c + 1.0f)))
                          * (1.0f + nd * 0.7f));
    m->h_f   = svf_f(5200.0f + 2200.0f * c + 2200.0f * tone);
}

static void trig_bass(gb_model_t* m, int semi, bool acc, float tone, float np, float nd) {
    const float f = note_to_freq((float)semi);
    m->b_inc  = HZ2INC(f);
    m->b_rinc = 1.0f / (float)m->b_inc;
    m->b_sinc = m->b_inc >> 1;
    m->b_env  = 1.0f; m->b_fenv = 1.0f;
    m->b_amp  = acc ? 1.0f : 0.72f;
    m->b_q    = clampf(((acc ? 0.28f : 0.55f) - np * 0.20f) * (0.82f + frand(m) * 0.36f),
                       0.18f, 0.85f);                                  // Q <= 5.6: squelch, not howl
    m->b_fd   = (acc ? (0.42f + 0.25f * tone) : (0.26f + 0.18f * tone))
                * (1.0f + np * 0.55f) * (0.75f + frand(m) * 0.5f);
    m->b_dec  = decay_coef(200.0f * (1.0f + nd * 0.75f));   // 50 .. 350 ms: pluck to sustain
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
    const int   sc   = m->scale & 3;
    const int   root = (int)m->root;
    const float tone = clampf(m->tone, 0.f, 1.f);
    const int   buf  = m->live;
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
        case T_KICK:  trig_kick (m, vel, c, node_char(m->npit[buf], w, t),
                                             node_char(m->ndec[buf], w, t)); break;
        case T_SNARE: trig_snare(m, vel, c, tone, node_char(m->npit[buf], w, t),
                                                 node_char(m->ndec[buf], w, t)); break;
        case T_HAT:   trig_hat  (m, vel, c, acc, tone, node_char(m->npit[buf], w, t),
                                                       node_char(m->ndec[buf], w, t)); break;
        case T_BASS:  trig_bass (m, root + deg2semi(sc, deg), acc, tone,
                                 node_char(m->npit[buf], w, t), node_char(m->ndec[buf], w, t)); break;
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

    m->morph = (uint16_t)(m->morph + drift);
    morph_weights(m->morph, w);

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
// Audio: generate into out_bus
// ----------------------------------------------------------------------------
static void gb_process(engine_t* engine, mixer_t* mix) {
    gb_model_t* m = engine_get_ctx(engine);
    if (!m) return;
    float* out = mixer_get_out(mix);
    if (!out) return;
    const uint32_t fs = mixer_get_fs(mix);

    if (!m->playing) { for (uint32_t i = 0; i < fs; i++) out[i] = 0.f; return; }

    // ---- block-rate constants -------------------------------------------
    const float tone = clampf(m->tone, 0.f, 1.f);
    const float kcd  = decay_coef(2.5f);    // kick click
    const float kpd  = decay_coef(10.f);    // kick pitch sweep
    const float sdt  = decay_coef(90.f);    // snare tone
    const float srd  = decay_coef(1.5f);    // snare transient
    const float bfd  = decay_coef(90.f);    // bass filter
    const float b_f0 = svf_f(70.0f + 180.0f * tone);

    const uint32_t spf_q8   = (uint32_t)(SR * 60.0f * 256.0f / (m->bpm * 4.0f));
    const float    sw_amt   = clampf(MIX_SWING + m->drift * 0.15f, 0.f, 1.f);
    const int32_t  sw_q8    = (int32_t)(sw_amt * 0.33f * (float)spf_q8);
    const uint16_t drift    = (uint16_t)(m->drift * m->drift * 4096.0f);

    const float g_k = LVL[T_KICK], g_s = LVL[T_SNARE];
    const float g_h = LVL[T_HAT],  g_b = LVL[T_BASS];
    const float g_l = LVL[T_LEAD], g_p = LVL[T_PAD];

    morph_w_t w; morph_weights(m->morph, &w);

    const uint32_t frames = fs >> 1;
    m->p_lfo += (uint32_t)(frames * 9838u);                 // ~0.11 Hz
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

    for (uint32_t i = 0; i < fs; i += 2) {
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
                    trig_kick(m, 0.8f, 0.0f, node_char(m->npit[rb], &w, T_KICK) + rp,
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
        m->k_ph += (uint32_t)(m->k_f * 89478.485f);
        float kick = fsin_i32((int32_t)m->k_ph) * m->k_env;
        kick += nrand(m) * m->k_click;
        kick  = sat1(kick * 1.7f);                     
        const float duck  = 1.0f - 0.55f * m->k_env;   
        const float duckb = 1.0f - 0.25f * m->k_env;
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
        const float bx   = bsaw + fsin_i32((int32_t)m->b_sph) * 0.5f;
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

        const float yl = l - m->dcx_l + 0.99843f * m->dcy_l; m->dcx_l = l; m->dcy_l = yl; l = yl;
        const float yr = r - m->dcx_r + 0.99843f * m->dcy_r; m->dcx_r = r; m->dcy_r = yr; r = yr;

        out[i]     = fminf(fmaxf(sat1(l * BUS_TRIM), -1.0f), 1.0f);
        out[i + 1] = fminf(fmaxf(sat1(r * BUS_TRIM), -1.0f), 1.0f);
    }


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

static void gb_redraw(gfx_t* gfx, const os_app_t* app) {
    gb_model_t* m = os_app_get_model(app);
    morph_w_t w; morph_weights(m->morph, &w);
    const int buf = m->live;

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
            gfx_draw_rect_fill(gfx, GRID_X0 + m->tstep[row] * CELL_PIT, gy, CELL_W, ROW_H);
            gfx_set_color(gfx, 1);
        }
    }

    gfx_set_color(gfx, 1);
    for (int i = 0; i < m->path_n; i++) {
        const int j = (i + 1 == m->path_n) ? 0 : i + 1;
        gfx_draw_line_thick(gfx, (gfx_uint_t)(INF_X + m->path_x[i]),
                                 (gfx_uint_t)(INF_Y + m->path_y[i]),
                                 (gfx_uint_t)(INF_X + m->path_x[j]),
                                 (gfx_uint_t)(INF_Y + m->path_y[j]), INF_TH);
    }

    gfx_set_color(gfx, 0);
    gfx_fill_rect_dithered(gfx, INF_X - INF_PAD, INF_Y - INF_PAD,
                           INF_SX + 2 * INF_PAD + 1, INF_SY + 2 * INF_PAD + 1, UI_HALFTONE+1);
    gfx_set_color(gfx, 1);

    {
        float nx, ny; morph_xy(m->morph, &nx, &ny);
        const gfx_uint_t mx = (gfx_uint_t)(INF_X + (int)(nx * INF_SX + 0.5f));
        const gfx_uint_t my = (gfx_uint_t)(INF_Y + (int)(ny * INF_SY + 0.5f));
        gfx_set_color(gfx, 1);
        gfx_draw_disc(gfx, mx, my, INF_DOT_R + 1, 0x0F);
        gfx_set_color(gfx, 0);
        gfx_draw_disc(gfx, mx, my, INF_DOT_R, 0x0F);
        gfx_set_color(gfx, 1);
    }
}

// ----------------------------------------------------------------------------
// Statusbar items
// ----------------------------------------------------------------------------
#define SB_W      240
#define SB_GAP     16
#define SB_X_METRO (2 + 13 + SB_GAP)
#define SB_X_BPM  (SB_X_METRO + 15 + 8)
#define SB_BAR_H   14
#define SB_BAR_MAX 64
#define SB_BAR_MIN 20

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
    draw_tri_up(gfx, cx, y + 2, 14, 15, 3);
    gfx_set_color(gfx, 0);
    draw_tri_up(gfx, cx, y + 6, 8, 7, 3);
    gfx_fill_rect_dithered(gfx, (gfx_uint_t)(cx - 8), (gfx_uint_t)y, 17, 17, UI_HALFTONE+2);
    gfx_set_color(gfx, 1);

    int dx = -3;
    if (playing) {
        const uint32_t per = (uint32_t)(60000.0f / (bpm > 1.f ? bpm : 1.f));
        const uint32_t p   = per ? (os_tick_get() % per) : 0u;
        const uint32_t ph  = ((p << 16) / (per ? per : 1u)) << 16;
        dx = (int)(fsin_i32((int32_t)ph) * 4.5f);
    }
    gfx_draw_line_thick(gfx, (gfx_uint_t)cx, (gfx_uint_t)(y + 13),
                             (gfx_uint_t)(cx + dx), (gfx_uint_t)(y + 3), 3);
}

static void gb_statusbar(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, void* user) {
    (void)w;
    gb_model_t* m = (gb_model_t*)user;
    if (!m) return;
    gfx_set_color(gfx, 1);
    if (m->playing) draw_tri(gfx, x + 2, y + 2, 15, 13);
    else            gfx_draw_rect_fill(gfx, x + 2, y + 3, 13, 13);
    draw_metro(gfx, x + SB_X_METRO + 7, y, m->playing, m->bpm);

    float ev = 0.f;
    switch (m->enc_target) {
    case ENC_MORPH: ev = m->morph * (1.0f / 65535.0f); break;
    case ENC_DRIFT: ev = m->drift; break;
    case ENC_TEMPO: ev = (m->bpm - 60.f) * (1.0f / 140.f); break;
    case ENC_DENS:  ev = m->dens[T_KICK]; break;
    case ENC_TONE:  ev = m->tone; break;
    case ENC_KEY:   ev = (m->root - 24.f) * (1.0f / 24.f); break;
    default: break;
    }

    gfx_set_font(gfx, gfx_nunito_semibold_14);

    if (!m->sb_bar_w) {
        gfx_uint_t wid = 0;
        for (int i = 0; i < ENC_TOTAL; i++) {
            const gfx_uint_t nw = gfx_get_str_width(gfx, ENC_NAME[i]);
            if (nw > wid) wid = nw;
        }
        m->sb_enc_x = (uint16_t)(SB_X_BPM + gfx_get_str_width(gfx, "200") + SB_GAP);
        m->sb_bar_x = (uint16_t)(m->sb_enc_x + wid + SB_GAP);
        int avail = SB_W - 2 - (int)m->sb_bar_x;
        if (avail > SB_BAR_MAX) avail = SB_BAR_MAX;
        m->sb_bar_w = (uint16_t)(avail > SB_BAR_MIN ? avail : SB_BAR_MIN);
    }

    gfx_draw_strf(gfx, (gfx_uint_t)(x + SB_X_BPM), (gfx_uint_t)(y + 15), "%d",
                  (int)(m->bpm + 0.5f));

    gfx_draw_str(gfx, (gfx_uint_t)(x + m->sb_enc_x), (gfx_uint_t)(y + 15), ENC_NAME[m->enc_target]);

    if (m->enc_target == ENC_MORPH) {
        /* No bar: the figure-8 on the panel goes solid instead (see gb_redraw). */
    } else if (m->enc_target == ENC_KEY || m->enc_target == ENC_SCALE) {
        const int mid = (int)m->root;
        if (m->enc_target == ENC_KEY)
            gfx_draw_strf(gfx, (gfx_uint_t)(x + m->sb_bar_x), (gfx_uint_t)(y + 15), "%s%d",
                          NOTE_NAME[mid % 12], mid / 12 - 1);
        else
            gfx_draw_str(gfx, (gfx_uint_t)(x + m->sb_bar_x), (gfx_uint_t)(y + 15),
                         SCALE_NAME[m->scale & 3]);
    } else {
        ui_draw_value_bar(gfx, (gfx_uint_t)(x + m->sb_bar_x), (gfx_uint_t)(y + 2),
                          m->sb_bar_w, SB_BAR_H,
                          (uint32_t)(clampf(ev, 0.f, 1.f) * 1000.f), 0, 1000);
    }
}

// ----------------------------------------------------------------------------
// Encoder
// ----------------------------------------------------------------------------
static void gb_enc_apply(gb_model_t* m, int32_t d) {
    switch (m->enc_target) {
    // No clamp: the path is a closed loop, so the knob wraps with it.
    case ENC_MORPH: m->morph = (uint16_t)(m->morph + (uint16_t)(d * 512)); break;
    case ENC_DRIFT: m->drift = clampf(m->drift + (float)d * 0.02f, 0.f, 1.f); break;
    case ENC_TEMPO: m->bpm = clampf(m->bpm + (float)d, 60.f, 200.f); break;
    case ENC_DENS: {
        const float s = (float)d * 0.02f;
        for (int t = 0; t < TRACKS; t++) m->dens[t] = clampf(m->dens[t] + s, 0.f, 1.f);
    } break;
    case ENC_TONE:  m->tone = clampf(m->tone + (float)d * 0.02f, 0.f, 1.f); break;
    case ENC_KEY:   m->root = clampf(m->root + (float)d, 24.f, 48.f); break;
    case ENC_SCALE: m->scale = (uint8_t)((m->scale + (d > 0 ? 1u : 3u)) & 3u); break;
    default: break;
    }
}

// ----------------------------------------------------------------------------
// Hints
// ----------------------------------------------------------------------------
static void gb_hints(gb_model_t* m) {
    static tapp_hint_pair_t hints[5];
    hints[0] = (tapp_hint_pair_t){ m->playing ? "stop" : "play", "hints" };
    hints[1] = (tapp_hint_pair_t){ 0, 0 };
    hints[2] = (tapp_hint_pair_t){ 0, 0 };
    hints[3] = (tapp_hint_pair_t){ "< enc", "exit" };
    hints[4] = (tapp_hint_pair_t){ "enc >", "rnd" };
    ui_hints_set_labels(hints);
}

// ----------------------------------------------------------------------------
// Tick
// ----------------------------------------------------------------------------
static bool gb_tick(os_app_t* app) {
    gb_model_t* m = os_app_get_model(app);
    const int32_t d = os_controls_encoder_get_delta();
    if (d) gb_enc_apply(m, d);

    return true;
}

// ----------------------------------------------------------------------------
// Input
// ----------------------------------------------------------------------------
static void gb_input(os_app_t* app, uint8_t btn, KeyStateEnum st) {
    gb_model_t* m = os_app_get_model(app);
    if (btn >= 5) return;
    const uint8_t bit = (uint8_t)(1u << btn);

    if (st == KEY_STATE_HOLD) {
        m->held_mask |= bit;
        switch (btn) {
        case 0: ui_hints_show(true); break;
        case 3: os_app_exit(); break;
        case 4:
            gb_regen(m, os_tick_get() ^ (uint32_t)(uintptr_t)m);
            m->morph = (uint16_t)(m->morph + (uint16_t)urand(m));
            break;
        default: break;
        }
        return;
    }

    if (st != KEY_STATE_RELEASED) return;

    if (m->held_mask & bit) {
        m->held_mask &= (uint8_t)~bit;
        if (btn == 0) ui_hints_show(false);
        return;
    }

    switch (btn) {
    case 0: gb_set_playing(m, !m->playing); gb_hints(m); break;
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
    m->morph = 0x2000;
    m->playing = true;

    m->bpm = 112.f; m->root = 33.f; m->tone = 0.45f; m->drift = 0.f;
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
        int n = 0, lx = -99, ly = -99;
        for (int i = 0; i < 4096 && n < PATH_N; i++) {
            float nx, ny; morph_xy((uint16_t)(i * 16), &nx, &ny);
            const int px = (int)(nx * INF_SX + 0.5f);
            const int py = (int)(ny * INF_SY + 0.5f);
            const int dx = px - lx, dy = py - ly;
            if (dx * dx + dy * dy < PATH_GAP2) continue;
            m->path_x[n] = (uint8_t)px; m->path_y[n] = (uint8_t)py; n++;
            lx = px; ly = py;
        }
        m->path_n = (uint8_t)n;
    }

    m->enc_target = ENC_MORPH;
    m->samp_left = 1; m->sub_len = 0xFFFFFFFFu; m->sub_left = 0xFFFFFFFFu;
    { morph_w_t w; morph_weights(m->morph, &w); fire_step(m, &w); }

    ui_statusbar_show(true);
    ui_statusbar_set_gfx_cb(gb_statusbar, SB_W, m);
    gb_hints(m);
    ui_hints_show(false);
    engine_set_callbacks(app, m);
    engine_set_active(true);
    return true;
}

static bool gb_deinit(os_app_t* app) {
    gb_model_t* m = os_app_get_model(app);
    g_model = 0;
    m->playing = false;
    ui_statusbar_set_gfx_cb(0, 0, 0);
    engine_set_active(false);
    engine_clear_callbacks(&gb_engine);
    return true;
}

static os_app_data_t gb_data = {
    .model = NULL, .model_size = sizeof(gb_model_t), .init = gb_init, .deinit = gb_deinit,
};

static os_app_t gb_app = {
    .name = "Infinigroove",
    .type = AppFullscreenType,
    .data = &gb_data,
    .redraw = (os_app_redraw)gb_redraw,
    .tick = gb_tick,
    .on_input = gb_input,
    .engine_cb = &gb_engine,
};

os_app_t* tapp_get_descriptor(void) { return &gb_app; }

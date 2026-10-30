#include <stdint.h>
#include "tapp_api.h" // also declares the libc subset (snprintf, memcpy, sinf, ...)

// Fast 2^x approximation using IEEE 754 float bit manipulation
// Accurate to ~0.1% which is sufficient for audio frequency calculation
static inline float fast_exp2f(float x) {
    // 2^x = 2^(int(x)) * 2^(frac(x))
    // Use polynomial approximation for fractional part
    int xi = (int)x;
    float xf = x - xi;
    if(x < 0) {
        xi--;
        xf += 1.0f;
    }

    // Polynomial approximation for 2^xf where xf in [0,1)
    // 2^x ≈ 1 + x*(0.693 + x*(0.240 + x*0.056))
    float p = 1.0f + xf * (0.6931472f + xf * (0.2402265f + xf * 0.0558f));

    // Multiply by 2^xi using bit manipulation
    union {
        float f;
        uint32_t i;
    } u = {.f = p};
    u.i += (uint32_t)xi << 23;
    return u.f;
}

typedef struct {
    int inc;
    uint16_t c;
    const midi_bus_t* bus; // the firmware's MIDI bus, read inline in process()
    uint32_t cursor;       // our position in its ring
} example_model_t;

// ctx is our model: the firmware hands every engine callback the model it allocated for us.
static void example_engine_process(void* restrict ctx, mixer_t* restrict mix) {
    example_model_t* m = ctx;
    // MIDI is not a callback: poll the bus. A note-on retunes the counter.
    uint32_t w;
    while(midi_poll(m->bus, &m->cursor, &w)) {
        if(midi_note_on(w)) {
            float freq = 440.0f * fast_exp2f((midi_data1(w) - 69.0f) / 12.0f);
            m->inc = (int)(freq * 65536 / TAPP_SAMPLE_RATE);
        }
    }
    float* out = mixer_get_out(mix);
    uint32_t fs = mixer_get_fs(mix);
    for(uint32_t i = 0; i < fs; i += 2) {
        m->c += m->inc;
        float val = ((float)m->c / (UINT16_MAX)) - 1.0f;
        out[i] = val;
        out[i + 1] = val;
    }
}
static uint_fast8_t example_engine_is_active(void* ctx) {
    (void)ctx;
    return 1; // always sounding: keep the audio path awake while we are loaded
}

// Declaring it in the descriptor is all it takes: the firmware installs it after init()
// and unhooks it before deinit().
static const engine_callbacks_t example_engine_callbacks = {
    .process = example_engine_process,
    .is_active = example_engine_is_active,
};

static bool example_init(os_app_t* app) {
    example_model_t* m = os_app_get_model(app); // zeroed
    m->bus = midi_bus(true); // true: channel-voice MIDI is ours while we are loaded
    m->cursor = m->bus->head; // from now on, not the ring's backlog
    return true;
}

static void example_redraw(gfx_t* gfx, const os_app_t* app) {
    example_model_t* m = os_app_get_model(app);
    gfx_draw_strf(gfx, 10, TAPP_CONTENT_Y + 10, "Counter: %i", m->inc);
}

static bool example_tick(os_app_t* app) {
    example_model_t* m = os_app_get_model(app);
    int32_t diff = os_controls_encoder_get_delta();
    if(diff != 0) {
        m->inc += diff;
    }
    return true;
}

static void example_input(os_app_t* app, uint8_t btn, KeyStateEnum state) {
    example_model_t* m = os_app_get_model(app);
    if(state == KEY_STATE_PRESSED) {
        switch(btn) {
        case BTN1:
            m->inc++;
            break;
        case BTN2:
            m->inc--;
            break;
        }
    }
    // We declare no TAPP_DECLARE_TYPE, so this is a utility tapp: the firmware reserves
    // none of our buttons and the exit is ours to bind.
    if(btn == BTN4 && state == KEY_STATE_HOLD) os_app_exit();
}

TAPP_APP("example", example_model_t, example_init, NULL,
         .redraw = example_redraw,
         .tick = example_tick,
         .on_input = example_input,
         .engine_cb = &example_engine_callbacks);

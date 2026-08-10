/**
 * @file led_patterns.c
 * @brief Showcase LED patterns.
 *
 */

#include "tapp_api.h"

#include "led_patterns.h"

static inline void
hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b) {
    if(s == 0) {
        *r = *g = *b = v;
        return;
    }
    uint8_t region = h / 43;
    uint8_t remainder = (h - region * 43) * 6;
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
    switch(region) {
    case 0:
        *r = v;
        *g = t;
        *b = p;
        break;
    case 1:
        *r = q;
        *g = v;
        *b = p;
        break;
    case 2:
        *r = p;
        *g = v;
        *b = t;
        break;
    case 3:
        *r = p;
        *g = q;
        *b = v;
        break;
    case 4:
        *r = t;
        *g = p;
        *b = v;
        break;
    default:
        *r = v;
        *g = p;
        *b = q;
        break;
    }
}

// Triangle wave: 0 → peak → 0 over 'period' steps
static inline uint8_t tri_wave(uint8_t step, uint8_t period, uint8_t peak) {
    uint8_t half = period >> 1;
    uint8_t phase = step % period;
    return (phase < half) ? (uint8_t)((uint16_t)phase * peak / half) :
                            (uint8_t)((uint16_t)(period - phase) * peak / half);
}

// Simple pseudo-random from step (deterministic, no stdlib)
static inline uint8_t led_hash(uint8_t x) {
    x ^= x << 3;
    x ^= x >> 5;
    x ^= x << 4;
    return x;
}

// --- Rainbow: HSV hue rotates across 4 LEDs ---
static void led_rainbow_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t hue = os_led_get_step(led) + i * 64;
        uint8_t r, g, b;
        hsv_to_rgb(hue, 255, 50, &r, &g, &b);
        hal_led_set_rgb(i, r, g, b);
    }
}

// --- Breathe RGB: all 4 cycle through R→G→B ---
static void led_breathe_rgb_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t phase = os_led_get_step(led);
    uint8_t sector = phase / 85; // 0=R, 1=G, 2=B
    uint8_t within = phase % 85;
    uint8_t rising = (uint8_t)((uint16_t)within * 50 / 85);
    uint8_t falling = 50 - rising;
    uint8_t r = 0, g = 0, b = 0;
    switch(sector) {
    case 0:
        r = falling;
        g = rising;
        break;
    case 1:
        g = falling;
        b = rising;
        break;
    default:
        b = falling;
        r = rising;
        break;
    }
    for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, r, g, b);
}

// --- Comet: bright head chases with dimming tail ---
static void led_comet_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    // Bounce: 0→1→2→3→2→1→0 over 6 phases
    uint8_t phase = (os_led_get_step(led) >> 2) % 6;
    uint8_t head = (phase < 4) ? phase : (6 - phase);
    for(uint8_t i = 0; i < 4; i++) {
        int8_t dist = (int8_t)head - (int8_t)i;
        if(dist < 0) dist = -dist;
        uint8_t v;
        switch(dist) {
        case 0:
            v = 60;
            break;
        case 1:
            v = 20;
            break;
        case 2:
            v = 5;
            break;
        default:
            v = 0;
            break;
        }
        hal_led_set_rgb(i, v, v, v);
    }
}

// --- Sparkle: random LED flashes white on dim warm base ---
static void led_sparkle_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t flash_led = led_hash(os_led_get_step(led)) & 3;
    for(uint8_t i = 0; i < 4; i++) {
        if(i == flash_led && (os_led_get_step(led) & 3) == 0) {
            hal_led_set_rgb(i, 60, 60, 50);
        } else {
            hal_led_set_rgb(i, 3, 2, 1);
        }
    }
}

// --- Fire: warm red/orange flicker, per-LED phase offset ---
static void led_fire_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t noise = led_hash(os_led_get_step(led) + i * 37);
        uint8_t r = 30 + (noise & 31); // 30-61
        uint8_t g = 8 + ((noise >> 2) & 15); // 8-23
        uint8_t b = (noise >> 5) & 3; // 0-3
        hal_led_set_rgb(i, r, g, b);
    }
}

// --- Ocean: blue/cyan wave propagating across LEDs ---
static void led_ocean_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t phase = os_led_get_step(led) + i * 40;
        uint8_t v = tri_wave(phase, 128, 40);
        hal_led_set_rgb(i, 0, v >> 2, v);
    }
}

// --- Pulse: single bright LED walks, others dark ---
static void led_pulse_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t active = (os_led_get_step(led) >> 4) & 3;
    uint8_t brightness = tri_wave(os_led_get_step(led) & 15, 16, 60);
    for(uint8_t i = 0; i < 4; i++) {
        if(i == active)
            hal_led_set_rgb(i, brightness, brightness, brightness);
        else
            hal_led_set_rgb(i, 0, 0, 0);
    }
}

// --- Candle: soft warm yellow/orange gentle flicker ---
static void led_candle_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t noise = led_hash(os_led_get_step(led) * 3 + i * 53);
        uint8_t r = 25 + (noise & 15); // 25-40
        uint8_t g = 12 + ((noise >> 2) & 7); // 12-19
        uint8_t b = 1 + ((noise >> 5) & 1); // 1-2
        hal_led_set_rgb(i, r, g, b);
    }
}


led_pattern_t led_pattern_rainbow = {
    .callback = &led_rainbow_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_breathe_rgb = {
    .callback = &led_breathe_rgb_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_comet = {
    .callback = &led_comet_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_sparkle = {
    .callback = &led_sparkle_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_fire = {
    .callback = &led_fire_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_ocean = {
    .callback = &led_ocean_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_pulse = {
    .callback = &led_pulse_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_candle = {
    .callback = &led_candle_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};


// --- Aurora: slow green/cyan/purple shimmer, phase-shifted ---
static void led_aurora_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t phase = os_led_get_step(led) + i * 50;
        uint8_t g = tri_wave(phase, 200, 35);
        uint8_t b = tri_wave((uint8_t)(phase + 80), 180, 25);
        uint8_t r = tri_wave((uint8_t)(phase + 160), 220, 15);
        hal_led_set_rgb(i, r, g, b);
    }
}

// --- Heartbeat: quick double-pulse then pause ---
static void led_heartbeat_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t phase = os_led_get_step(led) % 40;
    uint8_t r = 0;
    if(phase < 3)
        r = 60;
    else if(phase < 5)
        r = 10;
    else if(phase < 8)
        r = 50;
    else if(phase < 12)
        r = (uint8_t)(50 - (phase - 8) * 12);
    for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, r, 0, 0);
}

// --- Police: LEDs 0-1 red, 2-3 blue, alternating ---
static void led_police_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    bool phase = (os_led_get_step(led) & 15) < 8;
    for(uint8_t i = 0; i < 4; i++) {
        bool is_left = i < 2;
        bool on = is_left ? phase : !phase;
        if(on) {
            if(is_left)
                hal_led_set_rgb(i, 60, 0, 0);
            else
                hal_led_set_rgb(i, 0, 0, 60);
        } else {
            hal_led_set_rgb(i, 0, 0, 0);
        }
    }
}

// --- Lava: deep red/orange slow morph ---
static void led_lava_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t noise = led_hash(os_led_get_step(led) + i * 71);
        uint8_t slow = led_hash((os_led_get_step(led) >> 2) + i * 23);
        uint8_t r = 20 + (slow & 31);
        uint8_t g = 3 + (noise & 7);
        hal_led_set_rgb(i, r, g, 0);
    }
}

// --- Ice: cool white/blue gentle shimmer ---
static void led_ice_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t phase = os_led_get_step(led) + i * 45;
        uint8_t v = 8 + tri_wave(phase, 160, 20);
        uint8_t b = v + 10;
        hal_led_set_rgb(i, v >> 1, v, b);
    }
}

// --- Sunset: warm gradient shifting across LEDs ---
static void led_sunset_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t hue = (uint8_t)(os_led_get_step(led) + i * 8);
        // Map to warm range: red → orange → yellow → amber
        uint8_t r = 40 - tri_wave(hue, 200, 15);
        uint8_t g = 5 + tri_wave((uint8_t)(hue + 60), 180, 20);
        hal_led_set_rgb(i, r, g, 0);
    }
}

// --- Morse SOS: ···−−−··· in white ---
static void led_morse_sos_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    // SOS: 3 short, 3 long, 3 short = 30 units + gaps
    // Each unit = ~3 steps. Total cycle = ~90 steps
    static const uint8_t sos_pattern[] = {
        1, 0, 1, 0, 1, 0, 0, 0, // S: dit dit dit + letter gap
        1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, // O: dah dah dah + letter gap
        1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0 // S: dit dit dit + word gap
    };
    uint8_t idx = (os_led_get_step(led) * 3 / 4) % sizeof(sos_pattern);
    uint8_t v = sos_pattern[idx] ? 50 : 0;
    for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, v, v, v);
}

// --- Radar: single LED rotates with fade trail ---
static void led_radar_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t head = (os_led_get_step(led) >> 4) & 3;
    uint8_t sub = os_led_get_step(led) & 15;
    uint8_t head_v = 10 + (uint8_t)((uint16_t)sub * 50 / 15);
    for(uint8_t i = 0; i < 4; i++) {
        int8_t dist = (int8_t)((head - i + 4) & 3);
        uint8_t v;
        switch(dist) {
        case 0:
            v = head_v;
            break;
        case 1:
            v = 15;
            break;
        case 2:
            v = 4;
            break;
        default:
            v = 0;
            break;
        }
        hal_led_set_rgb(i, 0, v, 0);
    }
}

// --- Bounce: white ball bounces with ease ---
static void led_bounce_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    // Bounce over 48 steps: 0→1→2→3→2→1→0
    uint8_t phase = os_led_get_step(led) % 48;
    uint8_t pos;
    if(phase < 24)
        pos = phase / 6;
    else
        pos = (48 - phase) / 6;
    if(pos > 3) pos = 3;
    for(uint8_t i = 0; i < 4; i++) {
        if(i == pos)
            hal_led_set_rgb(i, 55, 55, 55);
        else
            hal_led_set_rgb(i, 0, 0, 0);
    }
}

// --- Matrix: green random drops ---
static void led_matrix_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t h = led_hash(os_led_get_step(led) * 7 + i * 41);
        uint8_t g = (h < 40) ? (30 + (h & 31)) : (h < 80 ? 8 : 1);
        hal_led_set_rgb(i, 0, g, 0);
    }
}

// --- Campfire: deep orange base with occasional flare ---
static void led_campfire_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t noise = led_hash((os_led_get_step(led) >> 1) + i * 29);
        uint8_t r = 20 + (noise & 15);
        uint8_t g = 8 + ((noise >> 3) & 7);
        // Occasional flare
        if((noise & 0xF0) == 0xF0) {
            r = 55;
            g = 25;
        }
        hal_led_set_rgb(i, r, g, 0);
    }
}

// --- Ocean deep: very deep blue, barely visible ---
static void led_ocean_deep_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t phase = os_led_get_step(led) + i * 55;
        uint8_t b = 2 + tri_wave(phase, 200, 8);
        hal_led_set_rgb(i, 0, b >> 2, b);
    }
}

// --- Neon: bright magenta/pink pulse ---
static void led_neon_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t v = tri_wave(os_led_get_step(led), 128, 55);
    for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, v, 0, v >> 1);
}

// --- Thunder: dark with occasional bright flash ---
static void led_thunder_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t h = led_hash(os_led_get_step(led));
    uint8_t v = 0;
    if(h > 245)
        v = 70; // rare bright flash
    else if(h > 240)
        v = 30; // dim flash
    else
        v = 1; // near-dark base
    for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, v, v, v);
}

// --- Breathe warm: warm white gentle breathe ---
static void led_breathe_warm_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t v = tri_wave(os_led_get_step(led), 200, 35);
    for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, v, v, v >> 1);
}

// --- Chase dual: two LEDs chase in opposite directions ---
static void led_chase_dual_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t a = (os_led_get_step(led) >> 4) & 3;
    uint8_t b = (3 - a) & 3;
    for(uint8_t i = 0; i < 4; i++) {
        if(i == a)
            hal_led_set_rgb(i, 50, 20, 0);
        else if(i == b)
            hal_led_set_rgb(i, 0, 20, 50);
        else
            hal_led_set_rgb(i, 0, 0, 0);
    }
}

// --- Twinkle: each LED independent random twinkle ---
static void led_twinkle_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        // Different prime multipliers give each LED its own rhythm
        uint8_t h = led_hash(os_led_get_step(led) * (3 + i * 2) + i * 97);
        uint8_t v = (h < 60) ? (10 + h) : 2;
        hal_led_set_rgb(i, v, v, v);
    }
}

// --- Gradient shift: smooth color gradient slides across ---
static void led_gradient_shift_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t hue = os_led_get_step(led) + i * 30;
        uint8_t r, g, b;
        hsv_to_rgb(hue, 220, 40, &r, &g, &b);
        hal_led_set_rgb(i, r, g, b);
    }
}

// --- Wave green: green sine wave propagating ---
static void led_wave_green_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t phase = os_led_get_step(led) + i * 32;
        uint8_t v = tri_wave(phase, 100, 45);
        hal_led_set_rgb(i, 0, v, v >> 3);
    }
}

// --- Ember: dim red base with occasional flare ---
static void led_ember_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t h = led_hash(os_led_get_step(led) + i * 61);
        uint8_t r = 6;
        uint8_t g = 1;
        if(h > 230) {
            uint8_t flare_led = (h >> 2) & 3;
            if(i == flare_led) {
                r = 40;
                g = 15;
            }
        }
        hal_led_set_rgb(i, r, g, 0);
    }
}


led_pattern_t led_pattern_aurora = {
    .callback = &led_aurora_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_heartbeat = {
    .callback = &led_heartbeat_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_police = {
    .callback = &led_police_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_lava = {
    .callback = &led_lava_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_ice = {
    .callback = &led_ice_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_sunset = {
    .callback = &led_sunset_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_morse_sos = {
    .callback = &led_morse_sos_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_radar = {
    .callback = &led_radar_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_bounce = {
    .callback = &led_bounce_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_matrix = {
    .callback = &led_matrix_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_campfire = {
    .callback = &led_campfire_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_ocean_deep = {
    .callback = &led_ocean_deep_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_neon = {
    .callback = &led_neon_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_thunder = {
    .callback = &led_thunder_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_breathe_warm = {
    .callback = &led_breathe_warm_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_chase_dual = {
    .callback = &led_chase_dual_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_twinkle = {
    .callback = &led_twinkle_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_gradient_shift = {
    .callback = &led_gradient_shift_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_wave_green = {
    .callback = &led_wave_green_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_ember = {
    .callback = &led_ember_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};

// --- Plasma: multi-frequency color mixing ---
static void led_plasma_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t r = tri_wave((uint8_t)(os_led_get_step(led) + i * 37), 97, 45);
        uint8_t g = tri_wave((uint8_t)(os_led_get_step(led) * 2 + i * 61), 127, 35);
        uint8_t b = tri_wave((uint8_t)(os_led_get_step(led) + i * 83 + 50), 157, 40);
        hal_led_set_rgb(i, r, g, b);
    }
}

// --- Pendulum: non-linear bounce with ease ---
static void led_pendulum_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    // 64-step cycle, quadratic ease at edges
    uint8_t phase = os_led_get_step(led) & 63;
    uint8_t half = (phase < 32) ? phase : (63 - phase);
    // Quadratic: maps 0-31 to 0-3 with acceleration
    uint16_t pos_x16 = (uint16_t)half * half * 3 / (31 * 31); // 0-3
    uint8_t pos = (uint8_t)(pos_x16);
    uint8_t frac = (uint8_t)(((uint16_t)half * half * 48 / (31 * 31)) - pos * 16);
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t v = 0;
        if(i == pos)
            v = 50 - (frac * 3);
        else if(i == pos + 1 && pos < 3)
            v = frac * 3;
        hal_led_set_rgb(i, v, v, v);
    }
}

// --- Firefly: independent per-LED bioluminescent glow ---
static void led_firefly_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        // Each LED gets unique period from hash of its index
        uint8_t period = 40 + (led_hash(i * 73) & 63); // 40-103
        uint8_t phase = (os_led_get_step(led) + led_hash(i * 31 + 17) * 3) % period;
        uint8_t v = tri_wave(phase, period, 40);
        hal_led_set_rgb(i, v >> 1, v, v >> 2); // greenish-yellow glow
    }
}

// --- Waterfall: blue cascade with splash ---
static void led_waterfall_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t phase = os_led_get_step(led) % 32;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t v = 0;
        if(phase < 16) {
            // Cascade: LED lights when "water" reaches it
            uint8_t trigger = i * 4;
            if(phase >= trigger && phase < trigger + 6) {
                v = (phase - trigger < 3) ? 50 : (uint8_t)(50 - (phase - trigger - 3) * 16);
            }
        } else {
            // Splash at bottom: all LEDs ripple from LED 3
            uint8_t ripple = phase - 16;
            uint8_t dist = 3 - i;
            if(ripple >= dist * 2 && ripple < dist * 2 + 4) {
                v = 30 - (ripple - dist * 2) * 8;
            }
        }
        hal_led_set_rgb(i, 0, v >> 2, v);
    }
}

// --- Strobe RGB: fast alternating R/G/B ---
static void led_strobe_rgb_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t phase = os_led_get_step(led) % 6;
    uint8_t r = 0, g = 0, b = 0;
    if(phase < 2)
        r = 55;
    else if(phase < 4)
        g = 55;
    else
        b = 55;
    // Off frame between colors
    if(phase & 1) {
        r = 0;
        g = 0;
        b = 0;
    }
    for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, r, g, b);
}

// --- Galaxy: dim base, occasional bright star ---
static void led_galaxy_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t h = led_hash(os_led_get_step(led) * 5 + i * 47);
        uint8_t base = 2;
        if(h > 248)
            base = 55; // bright star
        else if(h > 235)
            base = 20; // dim star
        hal_led_set_rgb(i, base, base, (uint8_t)(base + 1));
    }
}

// --- Snake: colored head with HSV-shifted fading trail ---
static void led_snake_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t head = (os_led_get_step(led) >> 3) & 3;
    uint8_t hue_base = os_led_get_step(led) * 2;
    for(uint8_t i = 0; i < 4; i++) {
        int8_t dist = (int8_t)((head - i + 4) & 3);
        uint8_t r, g, b;
        uint8_t v;
        switch(dist) {
        case 0:
            v = 50;
            break;
        case 1:
            v = 25;
            break;
        case 2:
            v = 8;
            break;
        default:
            v = 2;
            break;
        }
        hsv_to_rgb((uint8_t)(hue_base + dist * 40), 255, v, &r, &g, &b);
        hal_led_set_rgb(i, r, g, b);
    }
}

// --- Breathe purple: deep purple breathing ---
static void led_breathe_purple_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t v = tri_wave(os_led_get_step(led), 200, 40);
    for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, v, 0, v >> 1);
}

// --- Drip: LED falls 0→3, splash, pause ---
static void led_drip_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t phase = os_led_get_step(led) % 40;
    for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, 0, 0, 0);
    if(phase < 16) {
        // Fall: accelerating (0,0,0,0 → 4,4,4,4 steps per LED)
        uint8_t pos;
        if(phase < 6)
            pos = 0;
        else if(phase < 10)
            pos = 1;
        else if(phase < 13)
            pos = 2;
        else
            pos = 3;
        hal_led_set_rgb(pos, 30, 30, 50);
        if(pos > 0) hal_led_set_rgb(pos - 1, 5, 5, 10);
    } else if(phase < 24) {
        // Splash at LED 3, spreading
        uint8_t sp = phase - 16;
        for(uint8_t i = 0; i < 4; i++) {
            uint8_t dist = (3 - i);
            if(sp >= dist && sp < dist + 4) {
                uint8_t v = (uint8_t)(35 - (sp - dist) * 9);
                hal_led_set_rgb(i, v >> 1, v >> 1, v);
            }
        }
    }
    // phase 24-39: pause (all dark)
}

// --- Prism: rainbow spread, slowly rotating ---
static void led_prism_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t hue = (uint8_t)(os_led_get_step(led) + i * 64);
        uint8_t r, g, b;
        hsv_to_rgb(hue, 200, 35, &r, &g, &b);
        hal_led_set_rgb(i, r, g, b);
    }
}

// ============================================================================
// REACTIVE PATTERNS (ctx = const float* audio level, fallback when NULL)
// ============================================================================

static inline float led_get_level(const os_led_t* led, const void* ctx) {
    if(ctx) return *(const float*)ctx;
    return (float)tri_wave(os_led_get_step(led), 128, 100) / 100.f;
}

// --- VU fire: fire intensity scales with audio ---
static void led_vu_fire_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level(led, ctx);
    uint8_t intensity = (uint8_t)(level * 40.f);
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t noise = led_hash(os_led_get_step(led) + i * 37);
        uint8_t r = intensity + (noise & 15);
        uint8_t g = (uint8_t)(level * 12.f) + ((noise >> 2) & 7);
        uint8_t b = (noise >> 6) & 1;
        hal_led_set_rgb(i, r, g, b);
    }
}

// --- VU pulse: all LEDs pulse, warm→hot with intensity ---
static void led_vu_pulse_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level(led, ctx);
    uint8_t v = (uint8_t)(level * 60.f);
    uint8_t r = v;
    uint8_t g = (uint8_t)(v * 0.4f);
    uint8_t b = (level > 0.8f) ? (uint8_t)((level - 0.8f) * 100.f) : 0;
    for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, r, g, b);
}

// --- VU rainbow: rotation speed proportional to audio ---
static void led_vu_rainbow_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level(led, ctx);
    // Speed: 1 at silence, up to 8 at full
    uint8_t speed = 1 + (uint8_t)(level * 7.f);
    uint8_t base_hue = (uint8_t)(os_led_get_step(led) * speed);
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t r, g, b;
        hsv_to_rgb((uint8_t)(base_hue + i * 64), 255, 40, &r, &g, &b);
        hal_led_set_rgb(i, r, g, b);
    }
}

// --- VU spark: sparkle density proportional to audio ---
static void led_vu_spark_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level(led, ctx);
    uint8_t threshold = (uint8_t)(255 - level * 200.f); // lower = more sparks
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t h = led_hash(os_led_get_step(led) * 7 + i * 41);
        if(h > threshold) {
            uint8_t v = 20 + (uint8_t)(level * 40.f);
            hal_led_set_rgb(i, v, v, v);
        } else {
            hal_led_set_rgb(i, 2, 2, 1);
        }
    }
}

// --- VU breathe: peak brightness tracks audio ---
static void led_vu_breathe_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level(led, ctx);
    uint8_t peak = 5 + (uint8_t)(level * 50.f);
    uint8_t v = tri_wave(os_led_get_step(led), 128, peak);
    for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, v, v, v >> 1);
}

// --- VU spectrum: 4 LEDs as simulated frequency bands ---
static void led_vu_spectrum_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level(led, ctx);
    // Simulate: bass(red) → low-mid(yellow) → hi-mid(green) → treble(cyan)
    // Each band has different sensitivity curve
    uint8_t noise = led_hash(os_led_get_step(led));
    float bands[4];
    bands[0] = level * 1.2f + (float)(noise & 7) * 0.005f; // bass: loud
    bands[1] = level * 0.9f + (float)((noise >> 2) & 7) * 0.005f; // low-mid
    bands[2] = level * 0.6f + (float)((noise >> 4) & 7) * 0.008f; // hi-mid
    bands[3] = level * 0.3f + (float)((noise >> 6) & 3) * 0.01f; // treble
    static const uint8_t colors[4][3] = {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}};
    for(uint8_t i = 0; i < 4; i++) {
        float b = bands[i];
        if(b > 1.f) b = 1.f;
        uint8_t v = (uint8_t)(b * 50.f);
        hal_led_set_rgb(i, v * colors[i][0], v * colors[i][1], v * colors[i][2]);
    }
}

// --- VU plasma: plasma speed and saturation driven by audio ---
static void led_vu_plasma_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level(led, ctx);
    uint8_t speed = 1 + (uint8_t)(level * 4.f);
    uint8_t sat = 100 + (uint8_t)(level * 155.f);
    uint8_t phase = os_led_get_step(led) * speed;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t r = tri_wave((uint8_t)(phase + i * 37), 97, 40);
        uint8_t g = tri_wave((uint8_t)(phase * 2 + i * 61), 127, 30);
        uint8_t b = tri_wave((uint8_t)(phase + i * 83 + 50), 157, 35);
        // Scale by saturation
        r = (uint8_t)((uint16_t)r * sat >> 8);
        g = (uint8_t)((uint16_t)g * sat >> 8);
        b = (uint8_t)((uint16_t)b * sat >> 8);
        hal_led_set_rgb(i, r, g, b);
    }
}

// --- VU comet: speed proportional to audio ---
static void led_vu_comet_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level(led, ctx);
    uint8_t speed = 1 + (uint8_t)(level * 6.f);
    uint8_t phase = ((uint8_t)(os_led_get_step(led) * speed) >> 2) % 6;
    uint8_t head = (phase < 4) ? phase : (6 - phase);
    uint8_t brightness = 15 + (uint8_t)(level * 45.f);
    for(uint8_t i = 0; i < 4; i++) {
        int8_t dist = (int8_t)head - (int8_t)i;
        if(dist < 0) dist = -dist;
        uint8_t v;
        switch(dist) {
        case 0:
            v = brightness;
            break;
        case 1:
            v = brightness >> 2;
            break;
        case 2:
            v = brightness >> 4;
            break;
        default:
            v = 0;
            break;
        }
        hal_led_set_rgb(i, v, v, v);
    }
}

// --- VU aurora: color richness scales with audio ---
static void led_vu_aurora_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level(led, ctx);
    uint8_t intensity = 5 + (uint8_t)(level * 35.f);
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t phase = os_led_get_step(led) + i * 50;
        uint8_t g = (uint8_t)((uint16_t)tri_wave(phase, 200, intensity) * 1);
        uint8_t b = (uint8_t)((uint16_t)tri_wave((uint8_t)(phase + 80), 180, intensity) * 3 >> 2);
        uint8_t r = (uint8_t)((uint16_t)tri_wave((uint8_t)(phase + 160), 220, intensity) >> 1);
        hal_led_set_rgb(i, r, g, b);
    }
}

// --- VU strobe: rate proportional to audio, off when silent ---
static void led_vu_strobe_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level(led, ctx);
    if(level < 0.05f) {
        for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, 0, 0, 0);
        return;
    }
    // Strobe period: shorter when louder (16 at quiet → 2 at loud)
    uint8_t period = 16 - (uint8_t)(level * 14.f);
    if(period < 2) period = 2;
    bool on = (os_led_get_step(led) % period) < (period >> 1);
    uint8_t v = on ? (uint8_t)(30 + level * 30.f) : 0;
    for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, v, v, v);
}

// --- New creative pattern structs ---

led_pattern_t led_pattern_plasma = {
    .callback = &led_plasma_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_pendulum = {
    .callback = &led_pendulum_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_firefly = {
    .callback = &led_firefly_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_waterfall = {
    .callback = &led_waterfall_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_strobe_rgb = {
    .callback = &led_strobe_rgb_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_galaxy = {
    .callback = &led_galaxy_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_snake = {
    .callback = &led_snake_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_breathe_purple = {
    .callback = &led_breathe_purple_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_drip = {
    .callback = &led_drip_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_prism = {
    .callback = &led_prism_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_fire = {
    .callback = &led_vu_fire_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_pulse = {
    .callback = &led_vu_pulse_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_rainbow = {
    .callback = &led_vu_rainbow_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_spark = {
    .callback = &led_vu_spark_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_breathe = {
    .callback = &led_vu_breathe_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_spectrum = {
    .callback = &led_vu_spectrum_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_plasma = {
    .callback = &led_vu_plasma_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_comet = {
    .callback = &led_vu_comet_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_aurora = {
    .callback = &led_vu_aurora_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_strobe = {
    .callback = &led_vu_strobe_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};

// --- Tide: smooth sinusoidal color waves, 90° phase per LED ---
static void led_tide_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint16_t base = (uint16_t)os_led_get_step(led) << 8;
    for(uint8_t i = 0; i < 4; i++) {
        uint16_t phase = base + i * 16384u;
        int16_t s = lutsin_q15(phase);
        uint8_t v = (uint8_t)(((int32_t)s + 32768) * 48 >> 16) + 2;
        int16_t c = lutcos_q15((uint16_t)(base / 3 + i * 8192u));
        uint8_t hue = (uint8_t)(((int32_t)c + 32768) >> 8);
        uint8_t r, g, b;
        hsv_to_rgb(hue, 220, v, &r, &g, &b);
        hal_led_set_rgb(i, r, g, b);
    }
}

// --- Bloom: periodic flash → exponential warm decay ---
static void led_bloom_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t phase = os_led_get_step(led) & 63;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t p = (phase >= i * 2) ? (phase - i * 2) : 0;
        int16_t dx = (int16_t)((uint32_t)p * 32767 / 63);
        int16_t env = lutexp_neg_q15(dx);
        uint8_t v = (uint8_t)((uint32_t)env * 55 >> 15);
        hal_led_set_rgb(i, v, (uint8_t)(v * 3 / 4), (uint8_t)(v / 4));
    }
}

// --- Emberglow: tanh-compressed sine, never off (tube-amp warmth) ---
static void led_emberglow_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    for(uint8_t i = 0; i < 4; i++) {
        uint16_t phase = (uint16_t)(os_led_get_step(led) * 180 + i * 12000u);
        int16_t raw = lutsin_q15(phase);
        int16_t sat = lutsat_q15((int16_t)(raw >> 1));
        uint8_t v = (uint8_t)(((int32_t)sat + 32768) * 37 >> 16) + 8;
        hal_led_set_rgb(i, v, (uint8_t)(v * 2 / 5), (uint8_t)(v / 10));
    }
}

// --- Chromatic: R/G/B on independent sine frequencies (prismatic) ---
static void led_chromatic_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint16_t base = (uint16_t)(os_led_get_step(led) << 8);
    for(uint8_t i = 0; i < 4; i++) {
        uint16_t off = i * 10000u;
        int16_t sr = lutsin_q15((uint16_t)(base + off));
        int16_t sg = lutsin_q15((uint16_t)(base * 3 / 2 + off + 21845u));
        int16_t sb = lutsin_q15((uint16_t)(base * 2 + off + 43690u));
        uint8_t r = (uint8_t)(((int32_t)sr + 32768) * 45 >> 16);
        uint8_t g = (uint8_t)(((int32_t)sg + 32768) * 45 >> 16);
        uint8_t b = (uint8_t)(((int32_t)sb + 32768) * 45 >> 16);
        hal_led_set_rgb(i, r, g, b);
    }
}

// --- Gravity: ball falls 0→3 with acceleration, rises slowly ---
static void led_gravity_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint8_t cycle = os_led_get_step(led) % 80;
    uint8_t t;
    bool falling;
    if(cycle < 40) {
        t = cycle;
        falling = true;
    } else {
        t = 80 - cycle;
        falling = false;
    }
    uint16_t pos_x256 = (uint16_t)t * t * 3 / 20;
    uint8_t pos = (uint8_t)(pos_x256 >> 6);
    if(pos > 3) pos = 3;
    uint8_t frac = (pos_x256 >> 2) & 15;
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t v = 0;
        if(i == pos)
            v = 50 - frac * 3;
        else if(i == pos + 1 && pos < 3)
            v = frac * 3;
        if(falling)
            hal_led_set_rgb(i, v, (uint8_t)(v * 2 / 3), 0);
        else
            hal_led_set_rgb(i, 0, (uint8_t)(v * 2 / 3), v);
    }
}

// --- Interference: two sines at golden ratio + noise perturbation ---
static void led_interference_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    uint16_t base = (uint16_t)(os_led_get_step(led) << 8);
    for(uint8_t i = 0; i < 4; i++) {
        int16_t a = lutsin_q15((uint16_t)(base + i * 16384u));
        int16_t b = lutsin_q15((uint16_t)(base * 41 / 25 + i * 9000u));
        int32_t combined = ((int32_t)a + b) >> 1;
        uint8_t noise = led_hash(os_led_get_step(led) + i * 53);
        combined += ((int16_t)noise - 128) * 64;
        if(combined > 32767) combined = 32767;
        if(combined < -32768) combined = -32768;
        uint8_t v = (uint8_t)((combined + 32768) * 50 >> 16);
        uint8_t hue = (uint8_t)(os_led_get_step(led) + i * 60);
        uint8_t r, g, bl;
        hsv_to_rgb(hue, 180, v, &r, &g, &bl);
        hal_led_set_rgb(i, r, g, bl);
    }
}

// --- Thermal: color temperature drifts warm↔cool over slow cycle ---
static void led_thermal_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    /* x256 = one full warm<->cool cycle over the 256 steps; x100 covered ~39% of a
     * period so the drift was nearly static and jumped at the wrap. Range widened
     * from ~35/15/35 to something actually readable as a colour shift. */
    uint16_t phase = (uint16_t)(os_led_get_step(led) * 256);
    int16_t s = lutsin_q15(phase);
    uint8_t warmth = (uint8_t)(((int32_t)s + 32768) >> 8);
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t w = (uint8_t)(warmth + i * 15);
        uint8_t r = (uint8_t)(90 - (uint16_t)w * 70 / 255);
        uint8_t g = 30;
        uint8_t b = (uint8_t)(15 + (uint16_t)w * 75 / 255);
        hal_led_set_rgb(i, r, g, b);
    }
}

// --- Radiate: center-out bloom, magenta center, cyan edges ---
static void led_radiate_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    static const uint8_t dist[4] = {3, 1, 1, 3};
    uint8_t phase = os_led_get_step(led) % 48;
    for(uint8_t i = 0; i < 4; i++) {
        int16_t time_since = (int16_t)phase - (int16_t)dist[i] * 6;
        if(time_since < 0) time_since = 0;
        if(time_since > 30) time_since = 30;
        int16_t dx = (int16_t)((uint32_t)time_since * 32767 / 30);
        int16_t env = lutexp_neg_q15(dx);
        uint8_t v = (uint8_t)((uint32_t)env * 50 >> 15);
        if(dist[i] == 1)
            hal_led_set_rgb(i, v, 0, (uint8_t)(v * 3 / 4));
        else
            hal_led_set_rgb(i, 0, (uint8_t)(v * 3 / 4), v);
    }
}

// --- Syncopation: irregular musical rhythm with varying intensities ---
static void led_syncopation_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    static const uint8_t rhythm[32] = {
        0xF4, 0x00, 0x00, 0x32, 0x00, 0xC3, 0x00, 0x00, 0x64, 0x00, 0x93,
        0x00, 0x00, 0xF5, 0x00, 0x32, 0x00, 0xC4, 0x00, 0x00, 0x61, 0x00,
        0xF3, 0x00, 0x00, 0x00, 0x92, 0x64, 0x00, 0x00, 0xF5, 0x00,
    };
    uint8_t idx = os_led_get_step(led) & 31;
    uint8_t r = rhythm[idx];
    uint8_t mask = r & 0x0F;
    uint8_t bright = (r >> 4) * 10;
    for(uint8_t i = 0; i < 4; i++) {
        if(mask & (1 << i))
            hal_led_set_rgb(i, bright, (uint8_t)(bright * 3 / 5), 0);
        else
            hal_led_set_rgb(i, 0, 0, 0);
    }
}

// --- Mist: barely-there ambient blue-white, ultra-slow sine ---
static void led_mist_cb(const os_led_t* led, const void* ctx) {
    (void)ctx;
    /* x256 walks exactly one full cycle across the 256 steps (x40 covered only ~15%
     * of a period, so it barely moved), and the level range was [1,8] out of 255 —
     * indistinguishable from off. [1,80] still reads as a soft mist. */
    uint16_t base = (uint16_t)(os_led_get_step(led) * 256);
    for(uint8_t i = 0; i < 4; i++) {
        uint16_t phase = (uint16_t)(base + i * 11000u);
        int16_t s = lutsin_q15(phase);
        uint8_t v = (uint8_t)(((int32_t)s + 32768) * 80 >> 16) + 1;
        hal_led_set_rgb(i, (uint8_t)(v / 2), (uint8_t)(v * 3 / 4), v);
    }
}


static inline float led_get_level_q15(const os_led_t* led, const void* ctx) {
    if(ctx) return *(const float*)ctx;

    /* No audio source: synthesise a PERCUSSIVE envelope, not a smooth sine.
     *
     * The v2 patterns below are attack detectors — "vu particl" needs level to rise
     * by >0.15 in a single step and "vu impact" by >0.12. A full-circle sine over
     * 256 steps moves at most 0.0123 per step, so with one of those neither gate can
     * ever fire: both patterns sat dark forever and looked like dead entries. A
     * retriggering decay gives them the transients they were written for, and still
     * reads as a level for the metering patterns. */
    const uint8_t beat = (uint8_t)(os_led_get_step(led) & 31u); // hit every 32 steps
    if(beat == 0) return 1.0f;                                  // attack
    const float v = 1.0f - (float)beat / 32.f;
    return v * v; // squared -> snappier tail
}

// --- VU Log: logarithmic compressed metering ---
static void led_vu_log_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level_q15(led, ctx);
    int16_t raw = (int16_t)(level * 32767.f);
    int16_t compressed = lutsat_q15(raw);
    /* raw is UNIPOLAR [0,32767], so compressed is too — biasing it by 32768 as if it
     * were bipolar pinned clvl to [0.5,1.0], which held LEDs 0 and 1 permanently at
     * full brightness and left only the top two responding. */
    float clvl = (float)compressed / 32767.f;
    for(uint8_t i = 0; i < 4; i++) {
        float threshold = (float)i / 4.f;
        float above = clvl - threshold;
        if(above < 0.f) above = 0.f;
        if(above > 0.25f) above = 0.25f;
        uint8_t v = (uint8_t)(above * 200.f);
        uint8_t r = (i >= 2) ? v : (uint8_t)(v / 3);
        uint8_t g = (i <= 2) ? v : (uint8_t)(v / 3);
        hal_led_set_rgb(i, r, g, 0);
    }
}

// --- VU Trigger: binary threshold with hold ---
static void led_vu_trigger_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level_q15(led, ctx);
    static uint8_t on_state = 0;
    if(level > 0.3f) on_state = 20;
    if(on_state > 0) {
        on_state--;
        for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, 50, 50, 55);
    } else {
        hal_led_set_rgb(0, 3, 0, 0);
        for(uint8_t i = 1; i < 4; i++) hal_led_set_rgb(i, 0, 0, 0);
    }
}

// --- VU Envelope: fast attack, slow release, sticky peak ---
static void led_vu_envelope_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level_q15(led, ctx);
    static float envelope = 0.f;
    static float peak = 0.f;
    static uint8_t peak_age = 0;
    if(level > envelope)
        envelope = envelope * 0.2f + level * 0.8f;
    else
        envelope = envelope * 0.98f;
    if(level > peak) {
        peak = level;
        peak_age = 0;
    } else if(peak_age < 255)
        peak_age++;
    if(peak_age > 20) {
        int16_t dx = (int16_t)((uint32_t)(peak_age - 20) * 32767 / 235);
        int16_t decay = lutexp_neg_q15(dx);
        peak = peak * ((float)decay / 32767.f);
    }
    uint8_t env_leds = (uint8_t)(envelope * 3.99f);
    uint8_t peak_led = (uint8_t)(peak * 3.99f);
    if(peak_led > 3) peak_led = 3;
    for(uint8_t i = 0; i < 4; i++) {
        if(i == peak_led && peak > 0.05f)
            hal_led_set_rgb(i, 50, 10, 0);
        else if(i <= env_leds) {
            uint8_t v = (uint8_t)(envelope * 40.f);
            hal_led_set_rgb(i, 0, v, (uint8_t)(v / 3));
        } else
            hal_led_set_rgb(i, 0, 0, 0);
    }
}

// --- VU Spectral: simulated bass/treble split ---
static void led_vu_spectral_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level_q15(led, ctx);
    static float bass = 0.f;
    static float treble = 0.f;
    bass = bass * 0.95f + level * 0.05f;
    treble = treble * 0.6f + level * 0.4f;
    float treble_only = treble - bass;
    if(treble_only < 0.f) treble_only = 0.f;
    uint8_t bv = (uint8_t)(bass * 55.f);
    hal_led_set_rgb(0, bv, (uint8_t)(bv / 4), 0);
    hal_led_set_rgb(1, (uint8_t)(bv * 3 / 4), (uint8_t)(bv / 3), 0);
    uint8_t tv = (uint8_t)(treble_only * 70.f);
    hal_led_set_rgb(2, (uint8_t)(tv / 4), tv, (uint8_t)(tv * 3 / 4));
    hal_led_set_rgb(3, (uint8_t)(tv / 3), tv, tv);
}

// --- VU Strobe Link: strobe rate ∝ audio, FM'd sine ---
static void led_vu_strobe_link_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level_q15(led, ctx);
    if(level < 0.03f) {
        for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, 2, 2, 2);
        return;
    }
    uint16_t freq = (uint16_t)(1 + level * 15.f);
    uint16_t phase = (uint16_t)(os_led_get_step(led) * freq) << 8;
    int16_t s = lutsin_q15(phase);
    uint8_t on = (s > 0) ? 1 : 0;
    uint8_t v = on ? (uint8_t)(25 + level * 35.f) : 0;
    uint8_t r = v;
    uint8_t g = (uint8_t)(v * (1.f - level * 0.8f));
    uint8_t b = (uint8_t)(v * level);
    for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, r, g, b);
}

// --- VU Particles: transients spawn exp-decaying particles ---
static void led_vu_particles_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level_q15(led, ctx);
    static uint8_t age[4] = {255, 255, 255, 255};
    static uint8_t phue[4] = {0, 0, 0, 0};
    static float prev_level = 0.f;
    bool transient = (level > prev_level + 0.15f) && (level > 0.2f);
    prev_level = level;
    if(transient) {
        uint8_t oldest = 0;
        for(uint8_t i = 1; i < 4; i++)
            if(age[i] > age[oldest]) oldest = i;
        age[oldest] = 0;
        phue[oldest] = os_led_get_step(led) * 7;
    }
    for(uint8_t i = 0; i < 4; i++) {
        if(age[i] < 200) {
            int16_t dx = (int16_t)((uint32_t)age[i] * 32767 / 200);
            int16_t env = lutexp_neg_q15(dx);
            uint8_t v = (uint8_t)((uint32_t)env * 55 >> 15);
            uint8_t r, g, b;
            hsv_to_rgb(phue[i], 200, v, &r, &g, &b);
            hal_led_set_rgb(i, r, g, b);
            age[i]++;
        } else {
            hal_led_set_rgb(i, 0, 0, 0);
        }
    }
}

// --- VU Warmth: quiet=cool blue, loud=warm amber ---
static void led_vu_warmth_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level_q15(led, ctx);
    static float smooth = 0.f;
    smooth = smooth * 0.85f + level * 0.15f;
    float w = smooth;
    if(w > 1.f) w = 1.f;
    uint8_t base = 20;
    uint8_t r = (uint8_t)(base * (0.4f + w * 0.6f));
    uint8_t g = (uint8_t)(base * (0.7f - w * 0.1f));
    uint8_t b = (uint8_t)(base * (1.0f - w * 0.8f));
    for(uint8_t i = 0; i < 4; i++) {
        uint16_t ph = (uint16_t)(os_led_get_step(led) * 200 + i * 16384u);
        int16_t s = lutsin_q15(ph);
        int8_t mod = (int8_t)((int32_t)s * 3 >> 15);
        hal_led_set_rgb(i, (uint8_t)(r + mod), (uint8_t)(g + mod / 2), (uint8_t)(b - mod));
    }
}

// --- VU Wave: level maps to smooth interpolated LED position ---
static void led_vu_wave_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level_q15(led, ctx);
    static float smooth_w = 0.f;
    if(level > smooth_w)
        smooth_w = smooth_w * 0.5f + level * 0.5f;
    else
        smooth_w = smooth_w * 0.92f + level * 0.08f;
    float pos = smooth_w * 3.f;
    for(uint8_t i = 0; i < 4; i++) {
        float dist = pos - (float)i;
        if(dist < 0.f) dist = -dist;
        float br = 1.f - dist;
        if(br < 0.f) br = 0.f;
        br *= br;
        uint8_t v = (uint8_t)(br * 50.f);
        uint8_t rf = (uint8_t)(i * 15);
        uint8_t gf = (uint8_t)(45 - i * 10);
        hal_led_set_rgb(i, (uint8_t)(v * rf / 50), (uint8_t)(v * gf / 50), 0);
    }
}

// --- VU Gate: audio gates a rotating HSV ring ---
static void led_vu_gate_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level_q15(led, ctx);
    static uint16_t anim_phase = 0;
    static float gate_env = 0.f;
    if(level > 0.08f)
        gate_env = gate_env * 0.3f + 0.7f;
    else
        gate_env = gate_env * 0.93f;
    if(gate_env > 0.1f) anim_phase += (uint16_t)(gate_env * 600.f);
    for(uint8_t i = 0; i < 4; i++) {
        uint16_t ph = (uint16_t)(anim_phase + i * 16384u);
        int16_t s = lutsin_q15(ph);
        uint8_t v = (uint8_t)(((int32_t)s + 32768) * 50 >> 16);
        v = (uint8_t)((float)v * gate_env);
        uint8_t hue = (uint8_t)((anim_phase >> 8) + i * 64);
        uint8_t r, g, b;
        hsv_to_rgb(hue, 255, v, &r, &g, &b);
        hal_led_set_rgb(i, r, g, b);
    }
}

// --- VU Impact: transient → white-to-blue exponential bloom ---
static void led_vu_impact_cb(const os_led_t* led, const void* ctx) {
    float level = led_get_level_q15(led, ctx);
    static float prev_imp = 0.f;
    static uint8_t bloom_age = 255;
    static uint8_t bloom_intensity = 0;
    float delta = level - prev_imp;
    prev_imp = level;
    if(delta > 0.12f && bloom_age > 5) {
        bloom_age = 0;
        bloom_intensity = (uint8_t)(level * 55.f);
        if(bloom_intensity < 20) bloom_intensity = 20;
    }
    if(bloom_age < 120) {
        float fade = (float)bloom_age / 120.f;
        static const uint8_t delay[4] = {3, 0, 0, 3};
        for(uint8_t i = 0; i < 4; i++) {
            uint8_t a = (bloom_age > delay[i]) ? (bloom_age - delay[i]) : 0;
            int16_t dx = (int16_t)((uint32_t)a * 32767 / 120);
            int16_t env = lutexp_neg_q15(dx);
            uint8_t vi = (uint8_t)((uint32_t)env * bloom_intensity >> 15);
            uint8_t ri = (uint8_t)((float)vi * (1.f - fade * 0.9f));
            uint8_t gi = (uint8_t)((float)vi * (1.f - fade * 0.7f));
            hal_led_set_rgb(i, ri, gi, vi);
        }
        bloom_age++;
    } else {
        for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, 0, 0, 0);
    }
}

// --- Pattern structs ---

led_pattern_t led_pattern_tide = {
    .callback = &led_tide_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_bloom = {
    .callback = &led_bloom_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_emberglow = {
    .callback = &led_emberglow_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_chromatic = {
    .callback = &led_chromatic_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_gravity = {
    .callback = &led_gravity_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_interference = {
    .callback = &led_interference_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_thermal = {
    .callback = &led_thermal_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_radiate = {
    .callback = &led_radiate_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_syncopation = {
    .callback = &led_syncopation_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_mist = {
    .callback = &led_mist_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_log = {
    .callback = &led_vu_log_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_trigger = {
    .callback = &led_vu_trigger_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_envelope = {
    .callback = &led_vu_envelope_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_spectral = {
    .callback = &led_vu_spectral_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_strobe_link = {
    .callback = &led_vu_strobe_link_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_particles = {
    .callback = &led_vu_particles_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_warmth = {
    .callback = &led_vu_warmth_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_wave = {
    .callback = &led_vu_wave_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_gate = {
    .callback = &led_vu_gate_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};
led_pattern_t led_pattern_vu_impact = {
    .callback = &led_vu_impact_cb,
    .loop = 1,
    .steps = 255,
    .rate = 50,
};

// ============================================================================
// Browsable list
// ============================================================================

const led_demo_t led_demos[] = {
    { "rainbow", &led_pattern_rainbow },
    { "breathe", &led_pattern_breathe_rgb },
    { "comet", &led_pattern_comet },
    { "sparkle", &led_pattern_sparkle },
    { "fire", &led_pattern_fire },
    { "ocean", &led_pattern_ocean },
    { "pulse", &led_pattern_pulse },
    { "candle", &led_pattern_candle },
    { "aurora", &led_pattern_aurora },
    { "heartbeat", &led_pattern_heartbeat },
    { "police", &led_pattern_police },
    { "lava", &led_pattern_lava },
    { "ice", &led_pattern_ice },
    { "sunset", &led_pattern_sunset },
    { "morse sos", &led_pattern_morse_sos },
    { "radar", &led_pattern_radar },
    { "bounce", &led_pattern_bounce },
    { "matrix", &led_pattern_matrix },
    { "campfire", &led_pattern_campfire },
    { "deep ocean", &led_pattern_ocean_deep },
    { "neon", &led_pattern_neon },
    { "thunder", &led_pattern_thunder },
    { "warm", &led_pattern_breathe_warm },
    { "chase dual", &led_pattern_chase_dual },
    { "twinkle", &led_pattern_twinkle },
    { "gradient", &led_pattern_gradient_shift },
    { "wave green", &led_pattern_wave_green },
    { "ember", &led_pattern_ember },
    { "plasma", &led_pattern_plasma },
    { "pendulum", &led_pattern_pendulum },
    { "firefly", &led_pattern_firefly },
    { "waterfall", &led_pattern_waterfall },
    { "strobe rgb", &led_pattern_strobe_rgb },
    { "galaxy", &led_pattern_galaxy },
    { "snake", &led_pattern_snake },
    { "breathe pp", &led_pattern_breathe_purple },
    { "drip", &led_pattern_drip },
    { "prism", &led_pattern_prism },
    { "vu fire", &led_pattern_vu_fire },
    { "vu pulse", &led_pattern_vu_pulse },
    { "vu rainbow", &led_pattern_vu_rainbow },
    { "vu spark", &led_pattern_vu_spark },
    { "vu breathe", &led_pattern_vu_breathe },
    { "vu spectrm", &led_pattern_vu_spectrum },
    { "vu plasma", &led_pattern_vu_plasma },
    { "vu comet", &led_pattern_vu_comet },
    { "vu aurora", &led_pattern_vu_aurora },
    { "vu strobe", &led_pattern_vu_strobe },
    { "tide", &led_pattern_tide },
    { "bloom", &led_pattern_bloom },
    { "emberglow", &led_pattern_emberglow },
    { "chromatic", &led_pattern_chromatic },
    { "gravity", &led_pattern_gravity },
    { "interfere", &led_pattern_interference },
    { "thermal", &led_pattern_thermal },
    { "radiate", &led_pattern_radiate },
    { "syncopate", &led_pattern_syncopation },
    { "mist", &led_pattern_mist },
    { "vu log", &led_pattern_vu_log },
    { "vu trigger", &led_pattern_vu_trigger },
    { "vu envelop", &led_pattern_vu_envelope },
    { "vu spectra", &led_pattern_vu_spectral },
    { "vu stb lnk", &led_pattern_vu_strobe_link },
    { "vu particl", &led_pattern_vu_particles },
    { "vu warmth", &led_pattern_vu_warmth },
    { "vu wave", &led_pattern_vu_wave },
    { "vu gate", &led_pattern_vu_gate },
    { "vu impact", &led_pattern_vu_impact },
};

const uint16_t led_demo_count = (uint16_t)(sizeof(led_demos) / sizeof(led_demos[0]));

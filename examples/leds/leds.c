/**
 * @file leds.c
 * @brief Browse and play the device's LED patterns.
 *
 * A tour of the OS LED pattern engine. Scroll the list with the encoder, press
 * BTN1 to play, BTN2 to stop.
 *
 * The interesting part is how little this file does. It never touches an LED.
 * It hands a led_pattern_t to os_led_set_pattern() and the OS LED task drives
 * the animation on its own timer -- tick() below does nothing but read the
 * encoder. That is why patterns keep animating smoothly no matter how slow
 * your redraw is.
 *
 * The patterns themselves live in led_patterns.c.
 */

#include "tapp_api.h"

#include "led_patterns.h"

#define TOP_BAR 60 // the system hint band clears rows 0..59
#define ROW_H   20
#define ROWS    8 // (240 - 60) / 20, leaving a little slack at the bottom
#define LIST_X  16

typedef struct {
    uint16_t sel;     // highlighted row
    uint16_t top;     // first visible row
    int32_t playing;  // index of the running pattern, -1 for none
} leds_model_t;

static void leds_all_off(void) {
    for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, 0, 0, 0);
}

static void leds_play(leds_model_t* m, uint16_t idx) {
    // ctx NULL: the reactive patterns fall back to an internal triangle wave,
    // so they animate here without an audio source to drive them.
    os_led_set_pattern(led_demos[idx].pattern, NULL);
    m->playing = (int32_t)idx;
}

static void leds_stop(leds_model_t* m) {
    os_led_stop_current();
    // The KTD2052 latches: stopping the pattern leaves whatever colour was
    // written last sitting on the strip, so clear it explicitly.
    leds_all_off();
    m->playing = -1;
}

static bool leds_init(os_app_t* app, va_list args) {
    leds_model_t* m = os_app_get_model(app);
    m->sel = 0;
    m->top = 0;
    m->playing = -1;

    static const tapp_hint_pair_t hints[5] = {
        {"play", 0}, {"stop", 0}, {0, 0}, {0, "exit"}, {0, 0},
    };
    ui_hints_set_labels(hints);
    ui_hints_show(true);

    leds_play(m, 0); // something on the strip straight away
    return true;
}

static bool leds_deinit(os_app_t* app) {
    leds_model_t* m = os_app_get_model(app);
    leds_stop(m); // never hand the device back with the strip stuck lit
    return true;
}

static void leds_redraw(gfx_t* gfx, const os_app_t* app) {
    leds_model_t* m = os_app_get_model(app);

    char buf[40];

    gfx_set_color(gfx, 1);
    snprintf(buf, sizeof(buf), "%d/%d", (int)m->sel + 1, (int)led_demo_count);
    gfx_draw_str(gfx, LIST_X, TOP_BAR, buf);

    for(uint16_t r = 0; r < ROWS; r++) {
        const uint16_t idx = m->top + r;
        if(idx >= led_demo_count) break;

        const gfx_uint_t y = TOP_BAR + 22 + r * ROW_H;
        const bool sel = (idx == m->sel);

        if(sel) {
            gfx_set_color(gfx, 1);
            gfx_draw_rect_fill(gfx, LIST_X - 4, y - 2, 200, ROW_H);
        }
        // Inverted text on the selected row, normal elsewhere.
        gfx_set_color(gfx, sel ? 0 : 1);
        snprintf(buf, sizeof(buf), "%s%s", (idx == (uint16_t)m->playing) ? "> " : "  ",
                 led_demos[idx].name);
        gfx_draw_str(gfx, LIST_X, y, buf);
    }
}

static bool leds_tick(os_app_t* app) {
    leds_model_t* m = os_app_get_model(app);

    const int32_t d = os_controls_encoder_get_delta();
    if(d != 0) {
        int32_t s = (int32_t)m->sel + d;
        if(s < 0) s = 0;
        if(s >= (int32_t)led_demo_count) s = led_demo_count - 1;
        m->sel = (uint16_t)s;

        // Keep the selection inside the visible window.
        if(m->sel < m->top) m->top = m->sel;
        else if(m->sel >= m->top + ROWS) m->top = m->sel - (ROWS - 1);
    }
    return true;
}

static void leds_input(os_app_t* app, uint8_t btn, KeyStateEnum state) {
    leds_model_t* m = os_app_get_model(app);

    if(state == KEY_STATE_PRESSED) {
        switch(btn) {
        case 0:
            leds_play(m, m->sel);
            break;
        case 1:
            leds_stop(m);
            break;
        }
    }
    if(btn == 3 && state == KEY_STATE_HOLD) os_app_exit();
}

static os_app_data_t leds_data = {
    .model = NULL,
    .model_size = sizeof(leds_model_t),
    .init = leds_init,
    .deinit = leds_deinit,
};

static os_app_t leds_app = {
    .name = "LEDs",
    .type = AppFullscreenType,
    .data = &leds_data,
    .redraw = (os_app_redraw)leds_redraw,
    .tick = leds_tick,
    .on_input = leds_input,
};

os_app_t* tapp_get_descriptor(void) {
    return &leds_app;
}

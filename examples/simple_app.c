/**
 * Simple TAPP Example - Minimal boilerplate!
 *
 * NO manifest needed - build script generates it automatically!
 * Just define your app and build with: tapp-build simple_app.c
 */

#include "tapp_api.h"

// App model
typedef struct {
    int counter;
} my_app_model_t;

// Forward declarations
static void my_app_redraw(gfx_t* gfx, const os_app_t* app);
static bool my_app_tick(os_app_t* app);
static bool my_app_init(os_app_t* app, va_list args);
static bool my_app_deinit(os_app_t* app);
static void my_app_input(os_app_t* app, uint8_t btn, KeyStateEnum state);

#define TOP_BAR 60 // the system hint band clears rows 0..59

// Implementations
static void my_app_redraw(gfx_t* gfx, const os_app_t* app) {
    my_app_model_t* model = os_app_get_model(app);

    // Draw animated boxes. Everything starts below TOP_BAR: the system hint band
    // owns the top of the screen (see my_app_init).
    gfx_set_color(gfx, 1);
    gfx_draw_rect_fill(gfx, model->counter % 112, TOP_BAR + 10, 16, 16);

    gfx_set_color(gfx, 0);
    gfx_draw_rect_fill(gfx, 112 - (model->counter % 112), TOP_BAR + 38, 16, 16);

    gfx_set_color(gfx, 1);
    gfx_draw_str(gfx, 2, TOP_BAR + 60, "counter runs on tick()");
}

static bool my_app_tick(os_app_t* app) {
    my_app_model_t* model = os_app_get_model(app);
    model->counter++;
    return true; // counter drives the animation — ask for a repaint
}

static bool my_app_init(os_app_t* app, va_list args) {
    (void)args;
    my_app_model_t* model = os_app_get_model(app);
    model->counter = 0;

    ui_statusbar_show(true);

    /* Button hints live in the SYSTEM band at the top of the screen, where the
     * physical buttons are. One label pair per button (press, hold); NULL or ""
     * leaves that action blank. The firmware owns the hint objects and draws them
     * with the same styling as the built-in apps — never draw your own hint bar,
     * and keep your content below TOP_BAR so the band does not cover it. */
    static const tapp_hint_pair_t hints[5] = {
        {"+1", 0}, {0, 0}, {0, 0}, {0, "exit"}, {0, 0},
    };
    ui_hints_set_labels(hints);
    ui_hints_show(true);
    return true;
}

static bool my_app_deinit(os_app_t* app) {
    (void)app;
    return true;
}

static void my_app_input(os_app_t* app, uint8_t btn, KeyStateEnum state) {
    my_app_model_t* model = os_app_get_model(app);

    if(btn == 0 && state == KEY_STATE_PRESSED) {
        // BTN1 pressed - increment counter
        model->counter += 10;
    }

    if(btn == 3 && state == KEY_STATE_HOLD) {
        // BTN4 held - exit app
        os_app_exit();
    }
}

// App data (MUST use static initializers!)
static os_app_data_t my_app_data = {
    .model = NULL,
    .model_size = sizeof(my_app_model_t),
    .init = my_app_init,
    .deinit = my_app_deinit,
};

// App descriptor (MUST use static initializers!)
// NOTE: .name is extracted by build script for manifest!
static os_app_t my_app_descriptor = {
    .name = "Simple App",
    .type = AppFullscreenType,
    .data = &my_app_data,
    .redraw = (os_app_redraw)my_app_redraw,
    .tick = my_app_tick,
    .on_input = my_app_input,
};

// Entry point (REQUIRED)
os_app_t* tapp_get_descriptor(void) {
    return &my_app_descriptor;
}

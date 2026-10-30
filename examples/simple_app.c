/**
 * Simple TAPP Example - Minimal boilerplate!
 *
 * NO manifest needed - build script generates it automatically!
 * Just define your app and build with: tapp-build simple_app.c
 */

#include "tapp_api.h"

// App model - allocated zeroed by the firmware before init()
typedef struct {
    int counter;
} my_app_model_t;

static void my_app_redraw(gfx_t* gfx, const os_app_t* app) {
    my_app_model_t* model = os_app_get_model(app);

    // Draw animated boxes. Everything starts at TAPP_CONTENT_Y: the system hints
    // own the top of the screen whenever the user reveals them (see my_app_init).
    // A draw callback starts in GFX_COLOR_SET.
    gfx_draw_rect_fill(gfx, model->counter % 112, TAPP_CONTENT_Y + 10, 16, 16);

    gfx_set_color(gfx, GFX_COLOR_CLEAR);
    gfx_draw_rect_fill(gfx, 112 - (model->counter % 112), TAPP_CONTENT_Y + 38, 16, 16);

    gfx_set_color(gfx, GFX_COLOR_SET);
    gfx_draw_str(gfx, 2, TAPP_CONTENT_Y + 60, "counter runs on tick()");
}

static bool my_app_tick(os_app_t* app) {
    my_app_model_t* model = os_app_get_model(app);
    model->counter++;
    return true; // counter drives the animation — ask for a repaint
}

static bool my_app_init(os_app_t* app) {
    (void)app;
    /* Button labels live in the SYSTEM hints at the top of the screen, where the
     * physical buttons are. One cell per action (the press and the hold of each
     * button); cells you leave out stay blank. The firmware owns the hint objects
     * and draws them with the same styling as the built-in apps — never draw your
     * own hint bar, and keep your content below TAPP_CONTENT_Y.
     * This tapp declares no app_type, so it is a "utility": all ten cells are
     * ours, including both HOLD cells — label the one we bind. The hints start
     * hidden and no firmware gesture reveals them for a utility tapp, so raise
     * them ourselves. BTN4/BTN5 sit in a narrow cell, keep those labels short. */
    static const tapp_hint_cell_t hints[HintsActionsTotal] = {
        [HintPress1] = {"+10"},
        [HintHold4] = {"exit"},
    };
    ui_hints_set(hints);
    ui_hints_show(true);
    return true;
}

static void my_app_input(os_app_t* app, uint8_t btn, KeyStateEnum state) {
    my_app_model_t* model = os_app_get_model(app);

    if(btn == BTN1 && state == KEY_STATE_PRESSED) {
        model->counter += 10;
    }

    // BTN4 HOLD exits. A tapp that declares no app_type is a "utility" — the
    // firmware claims none of its buttons, so every hold lands here and the way
    // out is whatever you bind. This one is the convention, not a requirement.
    if(btn == BTN4 && state == KEY_STATE_HOLD) {
        os_app_exit();
    }
}

// The whole descriptor in one statement. The name is what the menu shows, and
// tapp-build reads it from here for the manifest. No deinit: nothing to free.
TAPP_APP("Simple App", my_app_model_t, my_app_init, NULL,
         .redraw = my_app_redraw,
         .tick = my_app_tick,
         .on_input = my_app_input);

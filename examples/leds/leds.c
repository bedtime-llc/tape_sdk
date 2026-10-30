/**
 * @file leds.c
 * @brief Browse and play the device's LED patterns.
 *
 * A tour of the OS LED pattern engine. Scroll the list with the encoder and
 * press to play; the first entry stops everything.
 *
 * WINDOW MODE: this paints into a rect on top of the still-visible main menu
 * (os_app_set_window() from init()) rather than taking the screen. Two things that follow from it and that the code below depends on:
 *
 *   - redraw() paints the window's own panel first. The framebuffer is
 *     cleared every dirty frame and the menu paints underneath us, so whatever
 *     we do not cover shows through as menu pixels.
 *   - ui_menu_t is a widget of its OWN; the firmware clips redraw() to the
 *     rect but NOT that menu, so its geometry has to be made to fit by hand.
 *     It is shown in init(), after os_app_set_window() attached the window, so
 *     it stacks on top of us and, being modal, takes every button — which is
 *     what we want here: this tapp needs no on_input() at all.
 *
 * BTN4/BTN5 HOLD close the window (the firmware's, in window mode). The menu's
 * own close row still exits too.
 *
 * The patterns themselves live in led_patterns.c.
 */

#include "tapp_api.h"

#include "led_patterns.h"

/* Window rect. y=50 clears the hint band (its hold row ends around y48).
 * The menu panel is sized to land exactly on our top/left/right edges: a
 * MenuTypeWidget frame is visible*27 + header 28 + 20, plus the 2px halo the
 * active frame's nested outlines add — 4*27 + 28 + 20 = 156, +2 each side. */
#define WIN_X        94
#define WIN_Y        50
#define WIN_W        212
#define WIN_H        186

#define MENU_W       188
#define MENU_X       106
#define MENU_Y       62
#define MENU_VISIBLE 5

#define STATUS_X     106
#define STATUS_Y     228   // baseline, in the strip below the menu panel

typedef struct {
    ui_menu_t* menu;
    int32_t playing; // index of the running pattern, -1 for none
} leds_model_t;

static void leds_all_off(void) {
    for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, 0, 0, 0);
}

static void leds_stop(leds_model_t* m) {
    os_led_stop_current();
    leds_all_off();
    m->playing = -1;
}

static void leds_pick(ui_menu_t* menu, ui_menu_node_t* entry) {
    leds_model_t* m = ui_menu_ctx(menu);
    const uint32_t slot = UI_PICK_IDX(entry);
    if(slot == 0) {
        leds_stop(m);
        return;
    }

    const uint32_t idx = slot - 1;
    if(idx >= led_demo_count) return;
    // ctx NULL: the reactive patterns fall back to an internal level source,
    // so they animate here without an audio source to drive them.
    os_led_set_pattern(led_demos[idx].pattern, NULL);
    m->playing = (int32_t)idx;
}

static void leds_menu_build(ui_menu_t* menu) {
    ui_menu_add(menu, UI_PICK("- off -", leds_pick, 0));
    for(uint16_t i = 0; i < led_demo_count; i++) {
        ui_menu_add(menu, UI_PICK(led_demos[i].name, leds_pick, i + 1));
    }
}

static void leds_menu_closed(void* ctx, ui_menu_t* menu) {
    (void)ctx;
    (void)menu;
    os_app_exit();
}

static bool leds_init(os_app_t* app) {

    os_app_set_window(WIN_X, WIN_Y, WIN_W, WIN_H);
    leds_model_t* m = os_app_get_model(app);
    m->playing = -1;

    m->menu = ui_menu_create(m, leds_menu_build);
    if(m->menu == NULL) return false;

    ui_menu_set_type(m->menu, MenuTypeWidget);
    ui_menu_set_rows(m->menu, MENU_VISIBLE);
    ui_menu_set_pos(m->menu, MENU_X, MENU_Y);
    ui_menu_set_width(m->menu, MENU_W);
    ui_menu_set_close_cb(m->menu, leds_menu_closed);
    ui_menu_show(m->menu);

    const tapp_hint_cell_t hints[HintsActionsTotal] = {
        [HintPress1] = {"select", gfx_icon(GfxIconApply)},
        [HintPress4] = {NULL, gfx_icon(GfxIconPlayRev)},
        [HintPress5] = {NULL, gfx_icon(GfxIconPlay)},
    };
    ui_hints_set(hints);
    return true;
}

static void leds_deinit(os_app_t* app) {
    leds_model_t* m = os_app_get_model(app);
    leds_stop(m);
    if(m->menu != NULL) ui_menu_destroy(m->menu);
}

static void leds_redraw(gfx_t* gfx, const os_app_t* app) {
    leds_model_t* m = os_app_get_model(app);

    gfx_set_color(gfx, GFX_COLOR_CLEAR);
    gfx_draw_rect_fill_r(gfx, WIN_X, WIN_Y, WIN_W, WIN_H, 10);
    gfx_set_color(gfx, GFX_COLOR_SET);
    gfx_draw_rect_r(gfx, WIN_X, WIN_Y, WIN_W, WIN_H, 10);

    if(m->playing >= 0 && m->playing < (int32_t)led_demo_count) {
        gfx_draw_strf(gfx, STATUS_X, STATUS_Y, "playing: %s", led_demos[m->playing].name);
    } else {
        gfx_draw_str(gfx, STATUS_X, STATUS_Y, "stopped");
    }
}

/* No on_input: the menu is modal and takes every button while it is up, and closing it
 * exits the app. BTN4/BTN5 HOLD are the firmware's close in window mode. */
TAPP_APP("LEDs", leds_model_t, leds_init, leds_deinit, .redraw = leds_redraw);

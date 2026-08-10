/**
 * @file leds.c
 * @brief Browse and play the device's LED patterns.
 *
 * A tour of the OS LED pattern engine. Scroll the list with the encoder and
 * press to play; the first entry stops everything.
 *
 * The patterns themselves live in led_patterns.c.
 */

#include "tapp_api.h"

#include "led_patterns.h"

#define MENU_X       14
#define MENU_Y       74
#define MENU_W       244
#define MENU_VISIBLE 5

#define STATUS_X 278
#define STATUS_Y 96

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
    if(m == NULL || entry == NULL) return;

    const uint32_t slot = (uint32_t)(uintptr_t)entry->user_data;
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
    if(ui_menu_ctx(menu) == NULL) return;

    ui_menu_add(menu,
                (ui_menu_node_t){.name = "- off -",
                                 .type = MenuNodeCustom,
                                 .callback = leds_pick,
                                 .user_data = (void*)(uintptr_t)0});

    for(uint16_t i = 0; i < led_demo_count; i++) {
        ui_menu_add(menu,
                    (ui_menu_node_t){.name = (char*)led_demos[i].name,
                                     .type = MenuNodeCustom,
                                     .callback = leds_pick,
                                     .user_data = (void*)(uintptr_t)(i + 1)});
    }
}

static void leds_menu_closed(void* ctx, ui_menu_t* menu) {
    (void)ctx;
    (void)menu;
    os_app_exit();
}

static bool leds_init(os_app_t* app, va_list args) {
    leds_model_t* m = os_app_get_model(app);
    m->playing = -1;

    m->menu = ui_menu_create(m, leds_menu_build);
    if(m->menu == NULL) return false;

    ui_menu_set_type(m->menu, MenuTypeWidget);
    ui_menu_set_header(m->menu, true);
    ui_menu_set_name(m->menu, "led patterns");
    ui_menu_set_visible(m->menu, MENU_VISIBLE);
    ui_menu_set_pos(m->menu, MENU_X, MENU_Y);
    ui_menu_set_width(m->menu, MENU_W);
    ui_menu_set_close_cb(m->menu, leds_menu_closed);
    ui_menu_show(m->menu);

    return true;
}

static bool leds_deinit(os_app_t* app) {
    leds_model_t* m = os_app_get_model(app);
    leds_stop(m); 
    if(m->menu != NULL) {
        ui_menu_destroy(m->menu);
        m->menu = NULL;
    }
    return true;
}

static void leds_redraw(gfx_t* gfx, const os_app_t* app) {
    leds_model_t* m = os_app_get_model(app);

    gfx_set_color(gfx, 1);
    if(m->playing >= 0 && m->playing < (int32_t)led_demo_count) {
        gfx_draw_str(gfx, STATUS_X, STATUS_Y, "playing");
        gfx_draw_str(gfx, STATUS_X, STATUS_Y + 18, led_demos[m->playing].name);
    } else {
        gfx_draw_str(gfx, STATUS_X, STATUS_Y, "stopped");
    }
}

static void leds_input(os_app_t* app, uint8_t btn, KeyStateEnum state) {
    leds_model_t* m = os_app_get_model(app);

    if(m->menu != NULL && ui_menu_is_visible(m->menu)) {
        ui_menu_input(m->menu, btn, state);
        return;
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
    .on_input = leds_input,
};

os_app_t* tapp_get_descriptor(void) {
    return &leds_app;
}

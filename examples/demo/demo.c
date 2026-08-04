/**
 * Sprite Demo - TAPP with Bundled Assets
 *
 * Demonstrates:
 * - Loading images from bundled assets
 * - Drawing sprites with transparency
 * - Simple animation
 * - New simplified build system (no manifest boilerplate!)
 *
 * Build: ./tapp-build examples/demo
 */

#include "tapp_api.h"

// App state
typedef struct {
    int x, y; // Sprite position
    int dx, dy; // Velocity
    uint32_t frame; // Animation frame
    uint8_t shade;
    uint8_t mode;
} SpriteModel;

// Static app data
static os_app_data_t app_data;

// ============================================================================
// App Callbacks
// ============================================================================

static bool sprite_alloc(os_app_t* app, va_list args) {
    (void)args;
    ui_hints_show(false);

    SpriteModel* m = (SpriteModel*)app->data->model;
    m->x = 64;
    m->y = 32;
    m->dx = 1;
    m->dy = 1;
    m->frame = 0;
    m->shade = 0;
    m->mode = 0;

    return true;
}

static bool sprite_free(os_app_t* app) {
    // ui_hints_show(true);
    return true;
}

static bool sprite_tick(os_app_t* app) {
    SpriteModel* m = (SpriteModel*)app->data->model;

    // Update sprite position
    m->x += m->dx;
    m->y += m->dy;

    // Bounce off edges
    if(m->x <= 0 || m->x >= 400 - asset_dvd_logo.width) {
        m->dx = -m->dx;
        m->shade = !m->shade;
    }
    if(m->y <= 0 || m->y >= 240 - asset_dvd_logo.height) {
        m->dy = -m->dy;
        m->shade = !m->shade;
    }

    m->frame++;
    return false; // Redraw happens continuously
}

static void sprite_redraw(gfx_t* gfx, const os_app_t* app) {
    SpriteModel* m = os_app_get_model(app);
    gfx_set_color(gfx, 1);

    if(!m->mode) {
        ui_draw_img(gfx, m->x, m->y, &asset_dvd_logo);
        gfx_set_color(gfx, 0);
        gfx_fill_rect_dithered(
            gfx, m->x, m->y, asset_dvd_logo.width, asset_dvd_logo.height, m->shade);
    } else {
        gfx_set_color(gfx, 1);
        ui_draw_anim(gfx, 100, 20, m->frame / 2, &asset_dance_party);
    }
}

void sprite_controls(os_app_t* app, uint8_t button_id, KeyStateEnum state) {

    switch(button_id) {
    case 0:
        if(state == KEY_STATE_PRESSED) {
            SpriteModel* m = (SpriteModel*)app->data->model;
            // Reverse X direction
            m->dx = -m->dx;
        }
        break;
    case 1:
        if(state == KEY_STATE_PRESSED) {
            SpriteModel* m = (SpriteModel*)app->data->model;
            // Reverse Y direction
            m->dy = -m->dy;
        }

        break;
    case 2:
        if(state == KEY_STATE_PRESSED) {
            SpriteModel* m = (SpriteModel*)app->data->model;
            // Reverse Y direction
            m->mode = !m->mode;
        }

        break;

    default:
        if(state == KEY_STATE_HOLD) {
            os_app_exit();
        }
        break;
    }
}

// App descriptor (MUST use static initializers!)
static os_app_t sprite_app_descriptor = {
    .name = "Demo",
    .type = AppFullscreenType,
    .data = &app_data,
    .redraw = (os_app_redraw)&sprite_redraw,
    .tick = &sprite_tick,
    .on_input = &sprite_controls,
};

// App data (MUST use static initializers!)
static os_app_data_t app_data = {
    .model = NULL,
    .model_size = sizeof(SpriteModel),
    .init = &sprite_alloc,
    .deinit = &sprite_free,
};

// ============================================================================
// Entry Point (REQUIRED)
// ============================================================================

os_app_t* tapp_get_descriptor(void) {
    return &sprite_app_descriptor;
}

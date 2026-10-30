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

// ============================================================================
// App Callbacks
// ============================================================================

static bool sprite_init(os_app_t* app) {
    ui_hints_show(false);

    SpriteModel* m = os_app_get_model(app); // zeroed
    m->x = 64;
    m->y = 32;
    m->dx = 1;
    m->dy = 1;
    return true;
}

static bool sprite_tick(os_app_t* app) {
    SpriteModel* m = os_app_get_model(app);

    // Update sprite position
    m->x += m->dx;
    m->y += m->dy;

    // Bounce off edges
    if(m->x <= 0 || m->x >= SCREEN_WIDTH - asset_dvd_logo.width) {
        m->dx = -m->dx;
        m->shade = !m->shade;
    }
    if(m->y <= 0 || m->y >= SCREEN_HEIGHT - asset_dvd_logo.height) {
        m->dy = -m->dy;
        m->shade = !m->shade;
    }

    m->frame++;
    // Both the bouncing logo and the dance-party animation move with no input at all, so every
    // frame is one we need painted. Returning false here would freeze the demo into a still.
    return true;
}

static void sprite_redraw(gfx_t* gfx, const os_app_t* app) {
    SpriteModel* m = os_app_get_model(app);

    if(!m->mode) {
        ui_draw_img(gfx, m->x, m->y, &asset_dvd_logo);
        gfx_set_color(gfx, GFX_COLOR_CLEAR);
        gfx_fill_rect_dithered(
            gfx, m->x, m->y, asset_dvd_logo.width, asset_dvd_logo.height, m->shade);
    } else {
        ui_draw_anim(gfx, 100, 20, m->frame / 2, &asset_dance_party);
    }
}

static void sprite_controls(os_app_t* app, uint8_t button_id, KeyStateEnum state) {
    SpriteModel* m = os_app_get_model(app);
    if(state == KEY_STATE_PRESSED) {
        switch(button_id) {
        case BTN1: m->dx = -m->dx; break;     // reverse X direction
        case BTN2: m->dy = -m->dy; break;     // reverse Y direction
        case BTN3: m->mode = !m->mode; break; // logo / dance party
        }
    }
    // We declare no app_type, so we are a "utility" tapp: the firmware claims
    // none of our buttons and every hold lands here. Bind our own way out.
    if(button_id == BTN4 && state == KEY_STATE_HOLD) os_app_exit();
}

TAPP_APP("Demo", SpriteModel, sprite_init, NULL,
         .redraw = sprite_redraw,
         .tick = sprite_tick,
         .on_input = sprite_controls);

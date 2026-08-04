#pragma once

/**
 * @file tapp_api.h
 * @brief TAPP SDK - API for external applications loaded from SD card
 *
 * @mainpage TAPP SDK API Reference
 *
 * This header provides the complete interface for TAPP (Tape Application) development.
 * Type definitions are binary-compatible with firmware's native types.
 *
 * @section arch_sec Architecture
 *
 * - External apps are ELF32 relocatable objects (.tapp files)
 * - Apps are loaded dynamically from SD card at runtime
 * - Symbol resolution is done via firmware's exported symbol table
 * - Apps have restricted access to firmware features (security boundary)
 * - Apps use structures that are binary-compatible with os_app_t
 *
 * @section usage_sec Basic Usage
 *
 * @code{.c}
 * #include "tapp_api.h"
 *
 * // Your app must export this function
 * os_app_t* tapp_get_descriptor(void) {
 *     static os_app_t my_app = {
 *         .name = "My App",
 *         .type = AppFullscreenType,
 *         // ... other fields
 *     };
 *     return &my_app;
 * }
 * @endcode
 *
 * @section api_categories API Categories
 *
 * - @ref grp_memory - Memory allocation
 * - @ref grp_graphics - Drawing primitives
 * - @ref grp_app - Application lifecycle
 * - @ref grp_storage - File I/O
 * - @ref grp_menu - Menu system
 * - @ref grp_audio - Audio processing
 * - @ref grp_math - Math utilities
 * - @ref grp_input - Input handling
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>

// When building firmware, use native headers
// When building external apps, use type definitions below
#ifdef FIRMWARE_BUILD
#include "os_app_types.h"
#include "ui_components.h"
#else
// ============================================================================
// Type Definitions for External Apps
// ============================================================================
// NOTE: These types mirror firmware types in os_app_types.h and ui_components.h
// They are kept in sync via build-time checks to prevent mismatches
#define PARAM_MAX_OPT_NAME 16

typedef enum {
    ParamValType,
    ParamValScaledType,
    ParamValExactType,
    ParamValEDivType,
    ParamValNegativeType,
    ParamValBlockTimeType,
    ParamValDataSizeType,
    ParamValDBType,
    ParamValSelectType,
    ParamValToggleType,
    // filt
    ParamFilterType,
    ParamFilterSelectType,
    ParamFilterFreqType,
    ParamValUITotal,
    ParamValDummy,
} ParamTypeUI_t;

typedef struct params params_t;

struct params {

    const uint16_t id;
    const char* name;
    const ParamTypeUI_t type;

    float val;
    float min;
    float max;
    float dflt;
    float prev;

    float coarse;
    float fine;
    float snap;

    float target;


    void (*callback)(params_t* param, void* ctx);

    const char* option_name[PARAM_MAX_OPT_NAME];

    /* MUST be true for any param you put in a menu: it gates param_update_fast(),
     * and without it `val` never follows `target` — the menu draws `val`, so the
     * number sits frozen while the encoder turns. */
    const bool refresh;
    bool text_opt;
    
};

 

// Opaque firmware types
typedef struct gfx_t gfx_t;
typedef uint16_t gfx_uint_t;
typedef struct os_core_s os_core_t;
typedef struct os_controls_s os_controls_t;
typedef struct console_command_s console_command_t;
typedef struct ui_menu ui_menu_t;
typedef struct ui_notify ui_notify_t;

// Graphics image types — opaque; produced by the asset pipeline, see examples/demo/assets/
typedef struct gfx_img_t gfx_img_t;
typedef struct gfx_anim_t gfx_anim_t;

struct gfx_img_t {
    const uint16_t width;
    const uint16_t height;
    const uint8_t frame_count;
    const uint8_t frame_rate;
    const uint8_t** frames;
};

struct gfx_anim_t {
    const gfx_img_t* img;
    uint8_t frame;
    uint32_t tick;
};

// UI menu types
typedef enum {
    MenuNodeEntry,
    MenuNodeSwitch,
    MenuNodeCycle,
    MenuNodeParam,
    MenuNodeCustom,
    MenuNodeTotalTypes,
} MenuNodeTypeEnum;

typedef enum {
    MenuTypeDefault,
    MenuTypeLarge,
    MenuTypeWidget,
    MenuTypeWidgetGfx,
    MenuTypeCustom,
    MenuTotalTypes,
} MenuTypeEnum;

// Forward declare for menu node
typedef struct ui_menu_node ui_menu_node_t;

// Menu callback types
typedef void (*ui_menu_node_action_cb)(ui_menu_t* menu, ui_menu_node_t* entry);
typedef void (*ui_menu_close_callback)(void* ctx, ui_menu_t* menu);
typedef void (*ui_menu_sub_cb)(ui_menu_t* menu);
typedef void (*ui_menu_redraw_cb)(gfx_t* gfx, ui_menu_t* menu);
typedef void (*ui_menu_side_redraw_cb)(gfx_t* gfx, void* ctx);

// Menu node structure (for ui_menu_add)
struct ui_menu_node {
    char* name;
    bool name_allocated;
    const gfx_img_t* icon;
    bool icon_allocated;
    MenuNodeTypeEnum type;
    ui_menu_node_action_cb callback;
    ui_menu_side_redraw_cb ui_gfx_cb;
    ui_menu_sub_cb sub_menu;
    params_t* param;
    void* user_data;
    bool user_data_allocated;
};

// Audio engine types (needed for audio callbacks)
typedef struct engine_struct engine_t;
typedef struct mixer_s mixer_t;  // Opaque type - use getters below

// Mixer buffer accessors - stable ABI (use these instead of direct field access).
// mixer_t's layout is deliberately not published: go through these and your tapp keeps
// working across firmware revisions.
float* mixer_get_in(mixer_t* m);    // Get input buffer pointer
float* mixer_get_out(mixer_t* m);   // Get output buffer pointer (write audio here!)
float* mixer_get_fx(mixer_t* m);    // Get FX send buffer pointer
uint32_t mixer_get_fs(mixer_t* m);  // Get frame size (total samples, L+R interleaved)

// Engine callbacks structure (must match firmware engine_callbacks_t)
typedef struct engine_callbacks_s {
    void (*init)(os_core_t* core);
    void (*process)(engine_t* restrict engine, mixer_t* restrict mix);
    void (*defaults)(engine_t* engine);
    void (*cleanup)(os_core_t* core);
    void (*active)(engine_t* engine, bool state);
    uint_fast8_t (*is_active)(engine_t* engine);
    void (*sync)(engine_t* engine);
    void (*set_sync)(engine_t* engine, bool state);
    bool (*is_synced)(engine_t* engine);
    void (*set_mode)(engine_t* engine, uint_fast8_t mode);
    uint_fast8_t (*get_mode)(engine_t* engine);
    void (*midi_cc)(engine_t* engine, uint32_t channel, uint32_t control, uint32_t val);
    void (*midi_note)(engine_t* engine, uint32_t ch, bool state, uint32_t n, uint32_t vel);
    void (*midi_pitch_bend)(engine_t* engine, uint32_t channel, int32_t value);
    void (*midi_aftertouch)(engine_t* engine, uint32_t channel, uint32_t value);
    void (*midi_prog_change)(engine_t* engine, uint32_t channel, uint32_t program);
    void (*midi_transport)(engine_t* engine, uint8_t command);
} engine_callbacks_t;

// Key state enum (must match KeyStateEnum in firmware)
typedef enum {
    KEY_STATE_RELEASED = 0,
    KEY_STATE_PRESSED = 1,
    KEY_STATE_HOLD = 2,
    KEY_STATE_TOTAL = 3
} KeyStateEnum;

// App type enum (must match AppTypeEnum in firmware)
typedef enum {
    AppWidgetType = 0,
    AppFullscreenType = 1,
    AppTypeTotal = 2,
} AppTypeEnum;

// Total number of control events (must match firmware)
#define TotalEventsNum 6

// Forward declare app types
typedef struct os_app os_app_t;
typedef struct os_app_data os_app_data_t;

// Callback function typedefs (must match firmware)
typedef bool (*os_app_model_init)(os_app_t* app, va_list args);
typedef bool (*os_app_model_deinit)(os_app_t* app);
typedef bool (*os_app_tick)(os_app_t* app);
typedef void (*os_app_redraw)(gfx_t* gfx, const os_app_t* app);
typedef void (*os_app_storage_mem_cb)(os_app_t* app);
typedef bool (*os_app_storage_file_cb)(const char* full_path);
typedef void (*os_app_ctrl_cb)(os_app_t*, os_controls_t*, KeyStateEnum);
typedef bool (*on_pause_cb)(os_app_t* app);
typedef bool (*on_resume_cb)(os_app_t* app);

// Control input callbacks (must match ctrl_input_callbacks in firmware)
typedef struct {
    void (*funcs[TotalEventsNum])(os_app_t*, KeyStateEnum);
} ctrl_input_callbacks;

// Memory interface structure (must match os_app_memory_if_t in firmware)
typedef struct {
    os_app_storage_mem_cb save;
    os_app_storage_mem_cb load;
    os_app_storage_file_cb open;
    char* ext;
} os_app_memory_if_t;

// App data structure (must match os_app_data in firmware)
// IMPORTANT: Field names changed from alloc/free to init/deinit
struct os_app_data {
    void* model;
    size_t model_size;
    os_app_model_init init;   // Init called after model allocation
    os_app_model_deinit deinit;  // Deinit called before model deallocation
};

// Input callback typedef (new unified API)
typedef void (*os_app_input_cb)(os_app_t* app, uint8_t button_id, KeyStateEnum state);

// App structure (must match os_app_t in firmware)
// IMPORTANT: This must match the exact memory layout of firmware's os_app_t
struct os_app {
    // Core fields
    char* name;
    AppTypeEnum type;
    os_app_data_t* data;
    os_app_redraw redraw;
    os_app_tick tick;

    // Input handling
    os_app_input_cb on_input;              // Recommended: single input handler
    
    on_pause_cb on_pause;
    on_resume_cb on_resume;
    void* on_shutdown;    // on_shutdown_cb — firmware-only, keeps the layout aligned

    // Runtime context (managed by firmware)
    void* ctx;            // App context (not accessible to external apps)
    bool active;
    bool needs_redraw;    // written by firmware
    bool service;         // written by firmware

    // Extended features (optional)
    const gfx_img_t* icon;
    const os_app_memory_if_t* memory_if;
    const engine_callbacks_t* engine_cb;
    void* engine_ctx;
    const console_command_t* commands;
    size_t commands_size;
    const ui_menu_node_t* app_menu;
};
#endif // !FIRMWARE_BUILD

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// External App Data Initialization Helper
// ============================================================================

/**
 * @brief Initialize os_app_data_t structure for external apps
 *
 * This inline function ensures external apps always provide model_size correctly.
 * It's type-safe and provides compile-time checking.
 *
 * Usage in external app:
 *   typedef struct { int counter; } my_model_t;
 *
 *   static os_app_data_t my_data;
 *
 *   void init_app_data(void) {
 *       os_app_data_init(&my_data, sizeof(my_model_t), my_init, my_deinit);
 *   }
 *
 * Or use designated initializers directly:
 *   static os_app_data_t my_data = {
 *       .model = NULL,
 *       .model_size = sizeof(my_model_t),
 *       .init = my_init,
 *       .deinit = my_deinit,
 *   };
 *
 * @param data Pointer to os_app_data_t to initialize
 * @param model_size Size of the model structure
 * @param init_fn Init function (called after allocation)
 * @param deinit_fn Deinit function (called before deallocation)
 */
static inline void os_app_data_init(
    os_app_data_t* data,
    size_t model_size,
    os_app_model_init init_fn,
    os_app_model_deinit deinit_fn
) {
    data->model = NULL;
    data->model_size = model_size;
    data->init = init_fn;
    data->deinit = deinit_fn;
}

// ============================================================================
// Graphics and UI Components (from ui_components.h)
// ============================================================================
// External apps have access to all ui_components.h wrapper functions
// gfx_* functions are the low-level graphics primitives, ui_* macros provide aliases

#ifndef FIRMWARE_BUILD

// Screen dimensions
#define SCREEN_WIDTH  400
#define SCREEN_HEIGHT 240
#define SCREEN_CENTER_X 200
#define SCREEN_CENTER_Y 120

#define REELS_SCALE 60

#define X_CENTER ((SCREEN_WIDTH >> 2) + 10)
#define Y_CENTER 95


// gfx function declarations
void gfx_draw_pixel(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y);
void gfx_draw_hline(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w);
void gfx_draw_vline(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t h);
void gfx_draw_hvline(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t len, uint8_t dir);
void gfx_draw_line(gfx_t* gfx, gfx_uint_t x1, gfx_uint_t y1, gfx_uint_t x2, gfx_uint_t y2);
void gfx_draw_line_thick(gfx_t* gfx, gfx_uint_t x1, gfx_uint_t y1, gfx_uint_t x2, gfx_uint_t y2, uint8_t thickness);
void gfx_draw_bezier(gfx_t* gfx, gfx_uint_t x0, gfx_uint_t y0, gfx_uint_t x1, gfx_uint_t y1, gfx_uint_t x2, gfx_uint_t y2);
void gfx_draw_rect(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h);
void gfx_draw_rect_r(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, gfx_uint_t r);
void gfx_draw_rect_fill(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h);
void gfx_draw_rect_fill_r(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, gfx_uint_t r);
// Dithered fill (4x4 Bayer): shade 0 = nearly black ... 7 = nearly white
void gfx_fill_rect_dithered(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, uint8_t shade);
void gfx_draw_hline_dithered(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, uint8_t shade);
void gfx_draw_vline_dithered(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t h, uint8_t shade);
void gfx_fill_circle_dithered(gfx_t* gfx, gfx_uint_t cx, gfx_uint_t cy, gfx_uint_t r, uint8_t shade);
void gfx_fill_rect_r_dithered(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, gfx_uint_t r, uint8_t shade);
void gfx_fill_rect_gradient_v_dithered_bayer(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, uint8_t shade_top, uint8_t shade_bottom);
void gfx_fill_rect_gradient_h_dithered_bayer(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, uint8_t shade_left, uint8_t shade_right);
void gfx_draw_circle(gfx_t* gfx, gfx_uint_t x0, gfx_uint_t y0, gfx_uint_t r, uint8_t opt);
void gfx_draw_disc(gfx_t* gfx, gfx_uint_t x0, gfx_uint_t y0, gfx_uint_t r, uint8_t opt);
void gfx_draw_circle_dotted(gfx_t* gfx, gfx_uint_t x0, gfx_uint_t y0, gfx_uint_t r, uint8_t opt);
void gfx_set_color(gfx_t* gfx, uint8_t color);
gfx_uint_t gfx_draw_str(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, const char* str);
gfx_uint_t gfx_draw_strf(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, const char* fmt, ...);
void gfx_draw_glyph(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, uint16_t encoding);
void gfx_set_font(gfx_t* gfx, const uint8_t* font);
gfx_uint_t gfx_get_str_width(gfx_t* gfx, const char* str);
void gfx_draw_xbm(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, const uint8_t* bitmap);
void gfx_draw_xbm_rotated(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, const uint8_t* bitmap, float angle);
void gfx_draw_xbm_scaled(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, const uint8_t* bitmap, gfx_uint_t w, gfx_uint_t h, gfx_uint_t scale);
void gfx_set_clip(gfx_t* gfx, gfx_uint_t x0, gfx_uint_t y0, gfx_uint_t x1, gfx_uint_t y1);
void gfx_clip_reset(gfx_t* gfx);

// UI helper functions
void ui_draw_img(gfx_t* gfx, uint16_t x, uint16_t y, const gfx_img_t* img);
void ui_draw_anim(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t frame, const gfx_img_t* img);
void ui_draw_img_scaled(gfx_t* gfx, uint16_t x, uint16_t y, const gfx_img_t* img, uint_fast8_t scale);
void ui_draw_version_tag(gfx_t* gfx, uint16_t x, uint16_t y, const char* str);
void ui_draw_qrcode(gfx_t* restrict gfx, uint16_t x, uint16_t y, uint16_t size, const uint8_t* qr);
void ui_draw_time_entry(gfx_t* gfx, uint16_t x, uint16_t y, float len, uint16_t idx_l);

bool ui_ease_position(uint16_t* pos, uint16_t target);
bool ui_ease_position_mod(uint16_t* pos, uint16_t target, float mod);

void ui_draw_scrollbar(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t height, uint16_t pos, uint16_t total);

void ui_draw_on_circle(gfx_t* gfx, uint16_t x, uint16_t y, float value, uint_fast8_t scale, uint_fast8_t size);

void ui_draw_frame(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r, bool active);
void ui_draw_action(gfx_t* gfx, uint16_t x, uint16_t y, bool hold, const char* name);
uint16_t ui_draw_action_act(gfx_t* gfx, uint16_t x, uint16_t y, bool hold, const char* name, bool active, bool tall);

void ui_draw_leet_text(gfx_t* gfx, uint_fast8_t x, uint_fast8_t y, uint16_t count, const char* text);

void ui_draw_noise_zone(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint_fast8_t pixel_size);

void ui_draw_buffer_dots(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t width, uint16_t height, const float* buffer, size_t size);

void ui_string_upper(const char* original_str, char* uppper, int8_t len);
void ui_string_lower(const char* original_str, char* upper, int8_t len);

void ui_draw_frame_wback(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r, bool active);
void ui_draw_value_bar(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t val, uint32_t min, uint32_t max);
void ui_draw_value_bar_smallf(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, float val, float min, float max);

void ui_draw_battery(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t charge, bool charging);

void ui_draw_3layer_box(gfx_t* restrict gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t round);
void ui_draw_icon_centered(gfx_t* restrict gfx, uint16_t x, uint16_t y, const gfx_img_t* icon);
uint16_t ui_draw_text_centered_with_icon(gfx_t* restrict gfx, uint16_t y, const char* text, const gfx_img_t* icon);

// Time formatting utilities
typedef enum {
    TIME_FORMAT_COMPACT,     // "MM:SS.ss" or "HH:MM:SS.ss" - for timeline
    TIME_FORMAT_VERBOSE,     // "Xh Ym Zs" - for menu display
} TimeFormatStyle;

void ui_format_time(char* restrict buff, uint32_t size, float val, TimeFormatStyle style);

void ui_draw_shade_pattern(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint_fast8_t shade, uint_fast8_t scale);

void ui_draw_volume_mono(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t width, uint16_t height, const float* values, bool frame);
void ui_draw_volume_stereo(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t width, uint16_t height, const float* values, bool frame);

uint32_t ui_draw_str_multi_line(gfx_t* restrict gfx, const char *str, uint_fast8_t max_chr_per_line, uint32_t x, uint32_t y);
uint32_t ui_count_string_lines(const char* str, uint_fast8_t max_chr_per_line);
void ui_draw_dialogue_multiline_string(gfx_t* restrict gfx, uint_fast8_t x, uint_fast8_t y, uint16_t count, uint16_t max_chars_per_line, const char* restrict text);

void ui_draw_dialogue(gfx_t* restrict gfx, uint16_t x, uint16_t y, uint16_t width, uint16_t height);

void ui_draw_button(gfx_t* restrict gfx, const uint16_t x, const uint16_t y, const uint16_t w, const char* text, const gfx_img_t* icon, const bool pressed);
void ui_draw_button_inverted(gfx_t* restrict gfx, const uint16_t x, const uint16_t y, const uint16_t w, const char* text, const gfx_img_t* icon, const bool pressed);
void ui_draw_tape_speed(gfx_t* gfx, const uint16_t x, const uint16_t y, const float speed, float progress);

void ui_draw_switch(gfx_t* restrict gfx, uint16_t x, uint16_t y, bool val, bool detailed);

#endif // !FIRMWARE_BUILD

// ============================================================================
// Memory Management (FreeRTOS)
// ============================================================================

/**
 * @defgroup grp_memory Memory Management
 * @brief Dynamic memory allocation from FreeRTOS heap
 * @{
 */

/**
 * @brief Allocate memory from FreeRTOS heap
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL if out of memory
 * @note Memory is limited - always check return values
 *
 * @code{.c}
 * void* buffer = os_malloc(1024);
 * if (buffer) {
 *     // Use buffer
 *     os_free(buffer);
 * }
 * @endcode
 */
void* os_malloc(size_t size);

/**
 * @brief Free memory allocated with os_malloc
 * @param ptr Pointer returned by os_malloc (NULL is safely ignored)
 */
void os_free(void* ptr);

/** @} */ // end of grp_memory

// ============================================================================
// Graphics API - Direct access to ui_components.h and gfx
// ============================================================================

/**
 * @defgroup grp_graphics Graphics API
 * @brief Drawing primitives and image rendering for 400x240 display
 *
 * The graphics API provides functions for drawing to the monochrome display.
 * All drawing operations require a `gfx_t*` context passed to your redraw callback.
 *
 * @section gfx_color Color Model
 * The display is 1-bit monochrome. Use gfx_set_color() to set:
 * - `0` = White (background)
 * - `1` = Black (foreground)
 *
 * @section gfx_coords Coordinate System
 * Origin (0,0) is at top-left. X increases right, Y increases down.
 * Screen size: 400x240 pixels.
 *
 * @{
 */

/**
 * @brief Get current frame counter (for animations)
 * @param gfx Graphics context
 * @return Frame counter value (increments each display refresh)
 */
uint32_t ui_get_frame(gfx_t* gfx);

/* NOTE: no img_animation_* here — the firmware has no such API. Use
 * ui_draw_anim(), which takes the frame index directly. */

/**
 * @name Fonts
 * The only fonts the firmware exports. Pass one to gfx_set_font().
 *
 * These three are the firmware's own working set (proportional regular,
 * proportional bold, monospace) and are the ones the browser emulator can
 * render. Every other font in the firmware is deliberately unexported: each
 * exported name is ABI we can never rename or drop.
 * @{
 */
extern const uint8_t gfx_nunito_semibold_14[];  /**< Default UI body font */
extern const uint8_t gfx_nunito_bold_18[];      /**< Headings / emphasis */
extern const uint8_t DepartureMono_Regular_10[]; /**< Monospace — counters, numeric readouts */
/** @} */

/** @} */ // end of grp_graphics

// ============================================================================
// App Control
// ============================================================================

/**
 * @defgroup grp_app Application Control
 * @brief Application lifecycle management
 *
 * These functions control the application lifecycle and provide access
 * to the application's data model.
 *
 * @section app_lifecycle Lifecycle
 * 1. Firmware allocates memory for model (model_size bytes)
 * 2. init() callback is called if provided
 * 3. App runs with tick() and redraw() callbacks
 * 4. deinit() callback is called before exit
 * 5. Firmware frees model memory
 *
 * @{
 */

/**
 * @brief Exit the current app and return to launcher
 *
 * Call this to cleanly exit your application. The deinit() callback
 * will be called before the app is unloaded.
 */
void os_app_exit(void);

/**
 * @brief Close the given app
 * @param app App to close
 */
void os_app_close(os_app_t* app);

/**
 * @brief Get app's model data
 * @param app App context
 * @return Pointer to model, or NULL if not allocated
 *
 * @code{.c}
 * my_model_t* m = (my_model_t*)os_app_get_model(app);
 * m->counter++;
 * @endcode
 */
void* os_app_get_model(const os_app_t* app);

/** @} */ // end of grp_app

// ============================================================================
// File Browser API
// ============================================================================

/**
 * @defgroup grp_storage Storage API
 * @brief File I/O and browser operations
 *
 * The storage API provides sandboxed file I/O for reading and writing
 * files on the SD card. All paths are relative to the SD card root.
 *
 * @section storage_paths Path Format
 * - Paths start with "/" (root of SD card)
 * - Example: "/myapp/data.bin"
 * - Maximum path length: 255 characters
 *
 * @{
 */

/**
 * @brief Callback type for file browser selection
 * @param path Full path to selected file
 * @return true if file was loaded successfully
 */
typedef bool (*browser_callback_t)(const char* path);

/**
 * @brief Open file browser to select a file
 * @param start_path Starting directory path (e.g., "/" for root)
 * @param file_extension File extension filter (e.g., ".gb", ".txt")
 *                       Set to NULL to show all files
 * @param callback Callback function called when file is selected
 * @return true if browser launched successfully
 *
 * The browser will filter files by the given extension and call the callback
 * when the user selects a file. The callback receives the full file path.
 *
 * @code{.c}
 * bool on_file_selected(const char* path) {
 *     // Load the file
 *     storage_read_file(path, buffer, sizeof(buffer));
 *     return true;
 * }
 *
 * browser_open("/", ".txt", on_file_selected);
 * @endcode
 */
bool browser_open(const char* start_path, const char* file_extension, browser_callback_t callback);

// ============================================================================
// Safe File I/O (Storage API)
// ============================================================================

/**
 * @brief Read entire file into buffer
 * @param path File path (max 255 chars)
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 * @return Number of bytes read, or 0 on error
 *
 * @code{.c}
 * uint8_t data[1024];
 * uint32_t bytes = storage_read_file("/config.bin", data, sizeof(data));
 * if (bytes > 0) {
 *     // Process data
 * }
 * @endcode
 */
uint32_t storage_read_file(const char* path, void* buffer, size_t buffer_size);

/**
 * @brief Write entire file from buffer (overwrites existing)
 * @param path File path (max 255 chars)
 * @param buffer Data to write
 * @param size Number of bytes to write
 * @return Number of bytes written, or 0 on error
 */
uint32_t storage_write_file(const char* path, const void* buffer, size_t size);

/**
 * @brief Append data to existing file
 * @param path File path (max 255 chars)
 * @param buffer Data to append
 * @param size Number of bytes to append
 * @return Number of bytes written, or 0 on error
 */
uint32_t storage_append_file(const char* path, const void* buffer, size_t size);

/**
 * @brief Delete a file
 * @param path File path (max 255 chars)
 * @return true on success, false if file doesn't exist or error
 */
bool storage_delete_file(const char* path);

/**
 * @brief Create directory (and parent directories if needed)
 * @param path Directory path (max 255 chars)
 * @return true on success (also returns true if directory already exists)
 */
bool storage_mkdir(const char* path);

/**
 * @brief Check if file exists
 * @param path File path (max 255 chars)
 * @return true if file exists
 */
bool storage_file_exists(const char* path);

/**
 * @brief Get file size in bytes
 * @param path File path (max 255 chars)
 * @return File size in bytes, or 0 if not found
 */
uint32_t storage_file_size(const char* path);

/** @} */ // end of grp_storage

// ============================================================================
// Math Utilities
// ============================================================================

/**
 * @defgroup grp_math Math Utilities
 * @brief Fast math functions optimized for embedded use
 *
 * @{
 */

/**
 * @brief Fast integer square root
 * @param n Input value
 * @return Integer square root of n
 *
 * @code{.c}
 * uint32_t r = isqrt(10000);  // Returns 100
 * @endcode
 */
uint32_t isqrt(uint32_t n);

/**
 * @brief Fast sine approximation
 * @param angle Angle in degrees (0-360)
 * @return Sine value scaled to int16_t range (-32768 to 32767)
 *
 * To convert to float: `float sf = isin(angle) / 32768.0f;`
 */
int16_t isin(uint16_t angle);

/**
 * @brief Fast cosine approximation
 * @param angle Angle in degrees (0-360)
 * @return Cosine value scaled to int16_t range (-32768 to 32767)
 *
 * To convert to float: `float cf = icos(angle) / 32768.0f;`
 */
int16_t icos(uint16_t angle);

/** @} */ // end of grp_math

// ============================================================================
// UI Menu API (Minimal)
// ============================================================================

/**
 * @defgroup grp_menu Menu System
 * @brief Pre-built menu UI components
 *
 * The menu system provides a complete solution for building settings
 * screens and navigation hierarchies.
 *
 * @section menu_types Menu Entry Types
 * - **Custom** - Action triggered on selection
 * - **Submenu** - Navigate to nested menu
 * - **Param** - Edit a parameter value
 *
 * @section menu_example Basic Example
 * @code{.c}
 * static void settings_menu(ui_menu_t* menu) {
 *     ui_menu_add_custom(menu, "Reset", NULL, reset_action, NULL);
 * }
 *
 * static void root_menu(ui_menu_t* menu) {
 *     ui_menu_add_custom(menu, "Play", NULL, play_action, NULL);
 *     ui_menu_add_submenu(menu, "Settings", NULL, settings_menu, NULL);
 * }
 *
 * // In init
 * ui_menu_t* menu = ui_menu_init(app, root_menu);
 * @endcode
 *
 * @{
 */

#ifndef FIRMWARE_BUILD

// ============================================================================
// Menu Convenience Macros
// ============================================================================

// Entry with submenu
#define UI_ENTRY(_n, sub) \
    ((ui_menu_node_t){.name = (_n), .type = MenuNodeEntry, .sub_menu = (sub)})

// Entry with submenu and explicit context (flows to submenu as ctx)
#define UI_ENTRY_CTX(_n, sub, ctx) \
    ((ui_menu_node_t){.name = (_n), .type = MenuNodeEntry, .sub_menu = (sub), .user_data = (ctx)})

// Custom action callback
#define UI_ACTION(_n, cb) \
    ((ui_menu_node_t){.name = (_n), .type = MenuNodeCustom, .callback = (cb)})

// Parameter entry
#define UI_PARAM(_n, ptype, _p) \
    ((ui_menu_node_t){.name = (_n), .type = (ptype), .param = (_p)})

// Variants with icon
#define UI_ACTION_ICON(_n, _i, _cb) \
    ((ui_menu_node_t){.name = (_n), .icon = (_i), .type = MenuNodeCustom, .callback = (_cb)})

#define UI_ENTRY_ICON(_n, _i, _sub) \
    ((ui_menu_node_t){.name = (_n), .icon = (_i), .type = MenuNodeEntry, .sub_menu = (_sub)})

// Attach user_data to any entry
#define UI_DATA(entry, data) \
    ({ ui_menu_node_t _e = (entry); _e.user_data = (data); _e; })

// ============================================================================
// Menu Core API
// ============================================================================

/**
 * @brief Create a menu instance
 * @param app_ctx App context (typically os_app_t*)
 * @param root_builder Root menu builder callback
 * @return Pointer to menu instance, or NULL on error
 */
ui_menu_t* ui_menu_create(void* app_ctx, ui_menu_sub_cb root_builder);

/**
 * @brief Destroy a menu instance
 * @param menu Menu instance
 */
void ui_menu_destroy(ui_menu_t* menu);

/**
 * @brief Add an entry to the menu
 * @param menu Menu instance
 * @param entry Menu node entry
 */
void ui_menu_add(ui_menu_t* menu, ui_menu_node_t entry);

/**
 * @brief Get menu context
 * @param menu Menu instance
 * @return Context pointer passed to ui_menu_create
 */
void* ui_menu_ctx(ui_menu_t* menu);

/**
 * @brief Rebuild current menu view
 * @param menu Menu instance
 */
void ui_menu_rebuild(ui_menu_t* menu);

/**
 * @brief Unified input handler for menus
 * @param menu Menu instance
 * @param button_id Button ID (0-5)
 * @param state Button state
 */
void ui_menu_input(ui_menu_t* menu, uint8_t button_id, KeyStateEnum state);

/**
 * @brief Show menu (attach to widget layer)
 * @param menu Menu instance
 */
void ui_menu_show(ui_menu_t* menu);

/**
 * @brief Hide menu (detach from widget layer)
 * @param menu Menu instance
 */
void ui_menu_hide(ui_menu_t* menu);

/**
 * @brief Check if menu is visible
 * @param menu Menu instance
 * @return true if visible
 */
bool ui_menu_is_visible(ui_menu_t* menu);

// ============================================================================
// Menu Configuration
// ============================================================================

void ui_menu_set_type(ui_menu_t* menu, MenuTypeEnum type);
void ui_menu_set_close_cb(ui_menu_t* menu, ui_menu_close_callback close_cb);
void ui_menu_set_redraw_cb(ui_menu_t* menu, ui_menu_redraw_cb redraw);
void ui_menu_set_header(ui_menu_t* menu, bool enable);
void ui_menu_set_visible(ui_menu_t* menu, uint16_t count);
void ui_menu_set_name(ui_menu_t* menu, const char* name);

// Position and width (wrappers around ui_obj_set_x/y/width on the menu's base obj)
void ui_menu_set_pos(ui_menu_t* menu, uint16_t x, uint16_t y);
void ui_menu_set_width(ui_menu_t* menu, uint16_t w);

// Draw the menu (call from your redraw callback when using MenuTypeCustom/WidgetGfx)
void ui_menu_gfx_redraw(gfx_t* gfx, ui_menu_t* menu);

// ============================================================================
// Menu Query
// ============================================================================

MenuTypeEnum ui_menu_type(ui_menu_t* menu);
bool ui_menu_is_editing(ui_menu_t* menu);
bool ui_menu_loading(ui_menu_t* menu);

// ============================================================================
// Menu Navigation
// ============================================================================

void ui_menu_pop(ui_menu_t* menu);
void ui_menu_entry_enter(ui_menu_t* menu, ui_menu_node_t* entry);
void ui_menu_set_wait(ui_menu_t* menu);

/**
 * @brief One button's hint labels. NULL or "" leaves that action blank.
 *
 * Labels are REFERENCED, not copied — use string literals or storage that
 * outlives the app (e.g. a field of your model).
 */
typedef struct {
    const char* press;
    const char* hold;
} tapp_hint_pair_t;

/**
 * @brief Install this app's button hints into the system hint band.
 * @param labels array of 5, in physical button order BTN1..BTN5
 *
 * The band renders at the TOP of the screen, where the buttons are, with the
 * same styling and slide-in as the built-in apps. Call this in init() followed
 * by ui_hints_show(true); do not draw your own hint bar.
 *
 * The firmware owns the hint objects, so nothing internal crosses the ABI —
 * you only hand over the strings.
 */
void ui_hints_set_labels(const tapp_hint_pair_t* labels);

/**
 * @brief Show or hide UI hints
 * @param stat true to show hints, false to hide
 */
void ui_hints_show(bool stat);

/**
 * @brief Show or hide the whole system statusbar (battery, status items, hints)
 * @param state true to show, false to hide
 *
 * When shown and hints are not active it displays the idle status items
 * (battery etc). TAPPs should call ui_statusbar_show(true) in init() so the
 * system statusbar stays visible, matching the rest of the device.
 * @note Device-only: the desktop emulator does not render the statusbar.
 */
void ui_statusbar_show(bool state);

#endif // !FIRMWARE_BUILD

/** @} */ // end of grp_menu

// ============================================================================
// Audio Engine Functions
// ============================================================================

/**
 * @defgroup grp_audio Audio Engine
 * @brief Real-time audio processing
 *
 * The audio engine allows TAPPs to process audio in real-time.
 * Audio callbacks run in a high-priority context.
 *
 * @section audio_buffers Audio Buffers
 * Use the mixer getter functions for stable ABI:
 * - `mixer_get_out(mix)` - Output buffer (interleaved stereo: L, R, L, R, ...)
 * - `mixer_get_in(mix)` - Input buffer
 * - `mixer_get_fx(mix)` - FX send buffer
 * - `mixer_get_fs(mix)` - Frame size (total samples, L+R combined)
 * - Sample rate: 48kHz
 *
 * @section audio_example Example
 * @code{.c}
 * static void my_process(engine_t* engine, mixer_t* mix) {
 *     float* out = mixer_get_out(mix);
 *     uint32_t fs = mixer_get_fs(mix);
 *     for (uint32_t i = 0; i < fs; i += 2) {
 *         out[i] *= 0.5f;      // Left
 *         out[i+1] *= 0.5f;    // Right
 *     }
 * }
 *
 * static engine_callbacks_t my_engine = {
 *     .process = my_process,
 * };
 * @endcode
 *
 * @{
 */

/**
 * @brief Get callback context from engine (to access your model in callbacks)
 * @param engine Engine pointer passed to your callback
 * @return Your callback context (typically your model pointer)
 *
 * Use this in your engine callbacks to access your app's data:
 * @code{.c}
 * static void my_process(engine_t* engine, mixer_t* mix) {
 *     my_model_t* m = engine_get_ctx(engine);
 *     // Now use m->your_data
 * }
 * @endcode
 */
void* engine_get_ctx(engine_t* engine);

/**
 * @brief Set audio engine callbacks for the app
 * @param app App instance
 * @param cb_ctx Callback context (usually model pointer)
 *
 * The app's `engine_cb` field must point to a valid `engine_callbacks_t`.
 */
void engine_set_callbacks(os_app_t* app, void* cb_ctx);

/**
 * @brief Set audio engine active state
 * @param state true to enable processing, false to disable
 */
void engine_set_active(bool state);

/**
 * @brief Clear audio engine callbacks
 *
 * @warning Call this in your deinit() callback before the TAPP unloads
 * to prevent the engine from calling freed TAPP code.
 */
void engine_clear_callbacks(const engine_callbacks_t* cb);

/* NOTE: there is deliberately no audio_pause_rx/tx or audio_resume_rx/tx here.
 * SAI DMA is an OS-level control, not part of the engine abstraction, and the
 * firmware already pauses/resumes RX+TX around an engine-app swap (os_app_launch)
 * — the resume lands after your init() returns. Use engine_set_active(). */

/** @} */ // end of grp_audio

// ============================================================================
// Input Handling
// ============================================================================

/**
 * @defgroup grp_input Input Handling
 * @brief Button and encoder input
 *
 * @section input_callback Input Callback
 * Use the `on_input` callback in your app descriptor:
 *
 * @code{.c}
 * void my_input(os_app_t* app, uint8_t btn, KeyStateEnum state) {
 *     if (btn == 3 && state == KEY_STATE_HOLD) {
 *         os_app_exit();
 *     }
 * }
 * @endcode
 *
 * @section input_buttons Button IDs
 * | ID | Name | Common Use |
 * |----|------|------------|
 * | 0 | BTN1 | Select |
 * | 1 | BTN2 | - |
 * | 2 | BTN3 | - |
 * | 3 | BTN4 | Back/Exit |
 * | 4 | BTN5 | - |
 * | 5 | ENC | Encoder button |
 *
 * @{
 */

/**
 * @brief Get encoder rotation delta since last call
 * @return Signed delta (positive = clockwise, negative = counter-clockwise)
 *
 * @code{.c}
 * int32_t diff = os_controls_encoder_get_delta();
 * if (diff != 0) {
 *     model->value += diff;
 * }
 * @endcode
 */
int32_t os_controls_encoder_get_delta(void);

/** @} */ // end of grp_input

// ============================================================================
// Tape API
// ============================================================================

/**
 * @defgroup grp_tape Tape API
 * @brief Access to tape state and transport controls
 *
 * The tape API provides access to the global tape storage and transport state.
 * TAPPs can query tape state and control playback/recording.
 *
 * @section tape_state Tape States
 * - TAPE_STATE_UNMOUNTED (0) - No tape loaded
 * - TAPE_STATE_IDLE (1) - Tape loaded but not playing/recording
 * - TAPE_STATE_PLAYING (2) - Tape is playing
 * - TAPE_STATE_RECORDING (3) - Tape is recording
 *
 * @{
 */

#ifndef FIRMWARE_BUILD
// Opaque tape type for external apps
typedef struct tape_storage tape_t;

// Tape state enum (must match firmware tape_state_t)
typedef enum {
    TAPE_STATE_UNMOUNTED = 0,
    TAPE_STATE_IDLE = 1,
    TAPE_STATE_PLAYING = 2,
    TAPE_STATE_RECORDING = 3,
} tape_state_t;
#endif

/**
 * @brief Get the global tape instance
 * @return Pointer to tape (never NULL after system init)
 */
tape_t* tape_get(void);

/**
 * @brief Get current tape state
 * @param tape Tape instance from tape_get()
 * @return Current state (TAPE_STATE_UNMOUNTED, TAPE_STATE_IDLE, etc.)
 */
tape_state_t tape_get_state(const tape_t* tape);

/**
 * @brief Check if tape is idle (not playing or recording)
 * @param tape Tape instance from tape_get()
 * @return true if tape is idle
 */
bool tape_is_idle(const tape_t* tape);

/**
 * @brief Get tape length in tape positions (SD sectors)
 * @param tape Tape instance from tape_get()
 * @return Tape length in positions. 1 position = 1 sector = 64 stereo frames,
 *         so multiply by 64 to compare against tape_read()/tape_write() frames.
 */
uint32_t tape_size(const tape_t* tape);

// Opaque recording handle
typedef struct tape_rec tape_rec_t;

/**
 * @brief Begin recording session
 * @param tape Tape instance from tape_get()
 * @param pos Starting position in tape POSITIONS (SD sectors), NOT frames.
 *            tape_get_position() returns frames — divide by 64.
 * @param reverse True for reverse direction
 * @return Recording handle, or NULL if already recording
 *
 * Undo capture is automatic. Marks created on commit.
 *
 * @note This only brackets the take (undo, timeline, mark). It writes NO audio —
 * your engine callback has replaced the one that normally does. Stream your
 * output to the tape with tape_write() between begin and commit.
 */
tape_rec_t* tape_rec_begin(tape_t* tape, uint32_t pos, bool reverse);

/**
 * @brief Commit recording session
 * @param rec Recording handle from tape_rec_begin()
 * @param end_pos End position of recorded region
 * @return Final end position
 *
 * Creates marks for recorded region, commits undo state.
 */
uint32_t tape_rec_commit(tape_rec_t* rec, uint32_t end_pos);

/**
 * @brief Abort recording (discard without creating marks)
 * @param rec Recording handle
 */
void tape_rec_abort(tape_rec_t* rec);

/**
 * @brief Check if undo is available for last recording
 * @param tape Tape instance from tape_get()
 * @return true if undo data is valid
 */
bool tape_rec_undo_available(const tape_t* tape);

/**
 * @brief Execute undo - restore original audio from last recording
 * @param tape Tape instance from tape_get()
 * @return true on success
 */
bool tape_rec_undo_execute(tape_t* tape);

// ----------------------------------------------------------------------------
// Tape "creative kernel" - minimal read access for sampling/cutup TAPPs.
// Positions are in STEREO FRAMES (1 frame = L+R; tape_size() is also frames).
// Device-only: the desktop emulator has no tape (these return empty there).
// ----------------------------------------------------------------------------

/**
 * @brief Number of recording marks on the tape
 * @param tape Tape instance from tape_get()
 * @return Mark count (0 if none / no tape)
 */
uint32_t tape_marks_count(const tape_t* tape);

/**
 * @brief Get the start/end of mark `idx`
 * @param tape Tape instance from tape_get()
 * @param idx Mark index (0 .. tape_marks_count()-1)
 * @param start_frame Output: region start, in stereo frames
 * @param end_frame Output: region end, in stereo frames
 * @return true if idx is valid and outputs were written
 */
bool tape_marks_get(const tape_t* tape, uint32_t idx, uint32_t* start_frame, uint32_t* end_frame);

/**
 * @brief Read recorded audio into a RAM buffer (stereo-interleaved L,R,...)
 * @param tape Tape instance from tape_get()
 * @param pos_frame Start position in stereo frames
 * @param dst Destination buffer (must hold 2*n_frames floats)
 * @param n_frames Number of stereo frames to read
 * @return Number of stereo frames actually read
 *
 * @warning Blocks on SD DMA. Call from tick()/init() (main thread), NEVER
 * from the audio process() callback. Sequence the slices yourself in process().
 */
uint32_t tape_read(const tape_t* tape, uint32_t pos_frame, float* dst, uint32_t n_frames);

/**
 * @brief Write audio to the tape (stereo-interleaved L,R,...)
 * @param tape Tape instance from tape_get()
 * @param pos_frame Start position in stereo frames
 * @param src Source buffer (2*n_frames floats)
 * @param n_frames Number of stereo frames to write
 * @return Number of stereo frames actually written
 *
 * Clamped to the tape length and addressed from the tape's own data area, so it
 * cannot reach the WAV header or any other file. Partial sectors are
 * read-modify-written.
 *
 * This is how a TAPP records: ring your process() output in RAM and drain it
 * here from tick(), bracketed by tape_rec_begin()/tape_rec_commit() so a mark is
 * created. Undo covers only what tape_rec_begin() armed — writes outside a
 * begin/commit pair overwrite tape audio permanently.
 *
 * @warning Blocks on SD DMA. Call from tick()/init() (main thread), NEVER
 * from the audio process() callback.
 */
uint32_t tape_write(tape_t* tape, uint32_t pos_frame, const float* src, uint32_t n_frames);

/**
 * @brief Current tape playhead position, in stereo frames
 * @param tape Tape instance from tape_get()
 * @return Playhead position (frames)
 */
uint32_t tape_get_position(const tape_t* tape);

/** @} */ // end of grp_tape

// ============================================================================
// Tape Timeline API
// ============================================================================

/**
 * @defgroup grp_timeline Tape Timeline
 * @brief Control tape timeline visibility and display
 * @{
 */

/**
 * @brief Show or hide the tape timeline widget
 * @param state true to show, false to hide
 */
void tape_timeline_show(bool state);

/**
 * @brief Show or hide the time display on the timeline
 * @param state true to show, false to hide
 */
void tape_timeline_show_time_display(bool state);

/**
 * @brief Configure timeline geometry and track count
 * @param tracks Number of tracks to display (1-4)
 * @param x X position on screen
 * @param y Y position on screen
 * @param w Width in pixels
 * @param h Height in pixels
 */
void tape_timeline_setup(uint8_t tracks, int_fast16_t x, int_fast16_t y, uint_fast16_t w, uint_fast16_t h);

/**
 * @brief Zoom out to show entire tape
 */
void tape_timeline_zoom_out(void);

/**
 * @brief Zoom in to follow playhead
 */
void tape_timeline_zoom_in(void);

/**
 * @brief Zoom to fit loop region in view
 */
void tape_timeline_zoom_out_to_loop(void);

/**
 * @brief Enable or disable loop mode visualization
 * @param state true to enable loop shading, false to disable
 */
void tape_timeline_set_loop(bool state);

/**
 * @brief Switch to a different track
 * @param tr Track index (0-3)
 */
void tape_timeline_switch_track(uint8_t tr);

/**
 * @brief Bind the global tape playhead/loop/waveform to the timeline tracks
 * @param tracks Number of tracks to bind (match tape_timeline_setup)
 *
 * Call after tape_timeline_setup(...) and before tape_timeline_show(true) so the
 * real firmware timeline widget renders the actual tape playhead/waveform (where a
 * TAPP's recording lands). Without this the widget has no track params and won't
 * display. Device-only (no tape in the emulator's non-combined build).
 */
void tape_timeline_attach_default(uint8_t tracks);

/** @} */ // end of grp_timeline

// ============================================================================
// TAPP Entry Point
// ============================================================================

/**
 * @brief Entry point that external apps must implement
 * @return Pointer to app descriptor (os_app_t*)
 *
 * This function is called by the ELF loader to get the app's descriptor.
 * The descriptor contains all callbacks and metadata for the app.
 *
 * IMPORTANT:
 * - The returned pointer must remain valid for the app's lifetime
 * - Typically, you should return a pointer to a static os_app_t struct
 * - Use the native os_app_t type for compatibility
 */
__attribute__((weak)) os_app_t* tapp_get_descriptor(void);

#ifdef __cplusplus
}
#endif

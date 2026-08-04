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
 * - @ref grp_ui - Buttons, meters, frames, images
 * - @ref grp_app - Application lifecycle
 * - @ref grp_led - RGB LEDs
 * - @ref grp_storage - File I/O
 * - @ref grp_menu - Menu system
 * - @ref grp_audio - Audio processing
 * - @ref grp_math - Math utilities
 * - @ref grp_input - Input handling
 */

/* Only clang's freestanding builtin headers — tapp-build passes -nostdlibinc, so there is no
 * system libc on the include path. Everything else a tapp may call is declared below. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef FIRMWARE_BUILD
/* The firmware links a real libc; the freestanding subset below must not shadow it — its
 * isnan/isinf macros would rewrite the system declarations into builtins and fail to compile. */
#include <string.h>
#else

// ============================================================================
// Freestanding C library subset  (external tapp builds only)
// ============================================================================
/*
 * There is no libc to link against. A .tapp is a relocatable object whose undefined symbols are
 * resolved by the firmware at load time, so the rule is:
 *
 *     declare only what the firmware actually exports.
 *
 * Anything omitted is a compile error at the call site (tapp-build passes
 * -Werror=implicit-function-declaration) instead of a "missing import" when the device loads the
 * tapp. That matters most for a standalone SDK checkout, where the import gate cannot run — it
 * needs the firmware export table, which is not there.
 *
 * NO DOUBLES. The FPU is FPv5-D16 so doubles work, but at roughly half the throughput of float and
 * double the register pressure — unacceptable on the audio path. Three things enforce it:
 *   1. -Werror=double-promotion: a bare `1.1` in a float expression is an error. Write `1.1f`.
 *   2. Only float entry points are declared here, so `sin(x)` cannot compile.
 *   3. tools/verify-tapp.sh disassembles the finished .tapp and rejects any .f64 instruction —
 *      the backstop for what the flags cannot catch (an explicit `double acc;` promotes nothing).
 *
 * Two kinds of declaration below, and the difference is load-bearing:
 *   extern        — the firmware exports it; stays undefined in the .tapp, bound at load.
 *   static inline — NOT exported, but FPv5 has a single instruction for it, so the builtin expands
 *                   in place and costs no import. Verified with llvm-nm that none emit a libcall.
 * Moving one of the inlines to extern silently creates an unresolvable import.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* ---- string / memory (exported) ----------------------------------------------------------
 * Absent on purpose: strcat, strncat, strdup, strtok, strcasecmp, strspn, strcspn, strpbrk,
 * memccpy — none are exported. */
void* memchr(const void* s, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* s, int c, size_t n);

char* strchr(const char* s, int c);
int strcmp(const char* a, const char* b);
char* strcpy(char* dst, const char* src);
size_t strlen(const char* s);
int strncasecmp(const char* a, const char* b, size_t n);
int strncmp(const char* a, const char* b, size_t n);
char* strncpy(char* dst, const char* src, size_t n);
char* strndup(const char* s, size_t n);
size_t strnlen(const char* s, size_t n);
char* strrchr(const char* s, int c);
char* strstr(const char* hay, const char* needle);

/* ---- formatting (exported) ----------------------------------------------------------------
 * snprintf is the ONLY formatting entry point. printf / sprintf / vsnprintf / puts / fprintf /
 * fopen are not exported — a tapp calling them links fine and then fails to load. File I/O goes
 * through the storage_* API further down this header. */
int snprintf(char* buf, size_t n, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

/* ---- stdlib (exported) ---------------------------------------------------------------------
 * Prefer os_malloc/os_free below — they are tracked against the app's budget. Absent on purpose:
 * calloc, exit, abort, atof, atol, bsearch, getenv, system. */
void* malloc(size_t n);
void free(void* p);
void* realloc(void* p, size_t n);
void qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*));
int rand(void);
long random(void);
int atoi(const char* s);
float atoff(const char* s); /* float-returning atof; the double atof is not exported */
long strtol(const char* s, char** end, int base);
unsigned long strtoul(const char* s, char** end, int base);
float strtof(const char* s, char** end);

/* ---- math, single precision only (exported) ------------------------------------------------
 * The double entry points (sin, cos, pow, exp, log, sqrt, ...) are not exported — use the `f`
 * variants. Absent on purpose (no export AND no instruction): asinf, acosf, log2f, cbrtf, tanhf,
 * ldexpf, copysignf. logf is derived from the exported log10f below. */
float sinf(float x);
float cosf(float x);
float tanf(float x);
float sinhf(float x);
float coshf(float x);
float asinhf(float x);
float atanf(float x);
float atan2f(float y, float x);
float expf(float x);
float exp2f(float x);
float expm1f(float x);
float log10f(float x);
float log1pf(float x);
float powf(float x, float y);
float pow10f(float x);
float fmodf(float x, float y);
float hypotf(float x, float y);
float frexpf(float x, int* exp);
float scalbnf(float x, int n);
long lrintf(float x);
int finitef(float x);
float nanf(const char* tag);

#ifdef __cplusplus
}
#endif

/* ---- single FPv5 instruction, no import needed ---------------------------------------------- */

/*
 * sqrtf must NOT go through __builtin_sqrtf. Without -fno-math-errno (implied by -ffast-math) the
 * builtin may lower to a *call* to sqrtf — i.e. to this very function — which at -O0 is infinite
 * recursion (verified: one `bl <sqrtf>` inside <sqrtf>). tapp-build always passes -ffast-math so it
 * happens to be safe, but a header must not depend on that, and there is no libm to fall back to.
 */
#if defined(__ARM_FP) && (__ARM_FP & 4) /* single-precision VFP present */
static inline float sqrtf(float x) {
    float r;
    __asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
    return r;
}
#else
static inline float sqrtf(float x) { return __builtin_sqrtf(x); }
#endif

static inline float fabsf(float x) { return __builtin_fabsf(x); }
static inline float fminf(float x, float y) { return __builtin_fminf(x, y); }
static inline float fmaxf(float x, float y) { return __builtin_fmaxf(x, y); }
static inline float ceilf(float x) { return __builtin_ceilf(x); }
static inline float floorf(float x) { return __builtin_floorf(x); }
static inline float roundf(float x) { return __builtin_roundf(x); }
static inline float truncf(float x) { return __builtin_truncf(x); }
static inline float rintf(float x) { return __builtin_rintf(x); }

/* natural log via the exported base-10 one (ln(x) = log10(x) * ln(10)) */
static inline float logf(float x) { return log10f(x) * 2.302585093f; }

static inline int abs(int x) { return __builtin_abs(x); }
static inline long labs(long x) { return __builtin_labs(x); }

/*
 * ---- classification: compiler builtins, no imports ----
 *
 * CAUTION: tapp-build compiles with -ffast-math, which implies -ffinite-math-only — the compiler is
 * then entitled to assume NaN and infinity never occur, so isnan()/isinf() fold to constant false
 * and INFINITY/NAN comparisons are undefined. clang warns (-Wnan-infinity-disabled) at each use.
 * These exist for source compatibility, not as a working guard: to reject bad input, range-check it
 * (e.g. `x > 0.0f && x < 1e30f`) rather than testing for NaN.
 */
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x) __builtin_signbit(x)

#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define HUGE_VALF __builtin_inff()

/*
 * Float-suffixed on purpose: with -Werror=double-promotion a double-typed M_PI would make
 * `x * M_PI` a hard error rather than the intended f32 multiply.
 */
#define M_PI 3.14159265358979323846f
#define M_PI_2 1.57079632679489661923f
#define M_PI_4 0.78539816339744830962f
#define M_1_PI 0.31830988618379067154f
#define M_2_PI 0.63661977236758134308f
#define M_TWOPI 6.28318530717958647692f /* non-standard, kept for existing app code */
#define M_E 2.71828182845904523536f
#define M_LOG2E 1.44269504088896340736f
#define M_LOG10E 0.43429448190325182765f
#define M_LN2 0.69314718055994530942f
#define M_LN10 2.30258509299404568402f
#define M_SQRT2 1.41421356237309504880f
#define M_SQRT1_2 0.70710678118654752440f

/*
 * struct tm exists only as a TYPE. The firmware exports no time entry point at all — not time(),
 * mktime(), localtime(), gmtime(), difftime(), clock() or strftime() — so none are declared:
 * calling one must be a compile error, not a load-time missing import. Portable emulator cores
 * include <time.h> just to name this in an RTC-setter prototype (e.g. Peanut-GB's gb_set_rtc).
 */
struct tm {
    int tm_sec;   /* seconds after the minute [0,60] */
    int tm_min;   /* minutes after the hour [0,59] */
    int tm_hour;  /* hours since midnight [0,23] */
    int tm_mday;  /* day of the month [1,31] */
    int tm_mon;   /* months since January [0,11] */
    int tm_year;  /* years since 1900 */
    int tm_wday;  /* days since Sunday [0,6] */
    int tm_yday;  /* days since January 1 [0,365] */
    int tm_isdst; /* daylight saving flag */
};

#endif /* !FIRMWARE_BUILD */

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
typedef bool (*on_pause_cb)(os_app_t* app);
typedef bool (*on_resume_cb)(os_app_t* app);

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
    /* console_command_t* — the DEBUG-only dev console, firmware-only. Present
     * unconditionally so the tapp ABI is identical in DEBUG and release builds:
     * app_menu below sits after these, and gating them would move it. */
    const void* commands;
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
// Graphics API — drawing primitives (from gfx.h)
// ============================================================================

/**
 * @defgroup grp_graphics Graphics API
 * @brief Drawing primitives for the 400x240 monochrome display
 *
 * Every call takes the `gfx_t*` your redraw callback was handed. There is no cursor and no
 * transform stack: each call takes absolute screen coordinates and paints in the current
 * draw colour.
 *
 * @section gfx_color Colour model
 *
 * The panel is 1-bit and the frame starts cleared to black, so drawing means turning pixels
 * *on*. gfx_set_color() selects what a draw call does to the pixels it touches:
 *
 * - `0` — background: clears them (black)
 * - `1` — foreground: sets them (white). This is the normal drawing colour.
 * - `2` — XOR: inverts whatever is already there, which is how you overlay a cursor or a
 *   playhead without erasing what is underneath.
 *
 * The colour is sticky: it persists across calls and into the next frame. If you change it,
 * set it back to 1 before you return.
 *
 * @section gfx_coords Coordinate system
 *
 * Origin (0,0) is the top-left pixel, x grows right, y grows down, and the screen is
 * ::SCREEN_WIDTH x ::SCREEN_HEIGHT = 400x240. Coordinates are `gfx_uint_t` (uint16_t) and
 * every primitive clips against the display and the clip window, so drawing partly
 * off-screen is safe.
 *
 * @section gfx_shades Dithering — grey on a 1-bit panel
 *
 * The `*_dithered*` calls paint an ordered 8x8 Bayer pattern in the current colour instead
 * of a solid fill, which the eye reads as grey. `shade` is 0-7 and picks how many pixels
 * get painted:
 *
 * | shade      | 0  | 1   | 2   | 3   | 4   | 5   | 6   | 7   |
 * | ---------- | -- | --- | --- | --- | --- | --- | --- | --- |
 * | pixels set | 6% | 12% | 25% | 37% | 50% | 62% | 75% | 87% |
 *
 * In the usual colour 1 that runs from nearly black to nearly white. The pattern is anchored
 * to screen coordinates rather than to the shape, so neighbouring fills of the same shade
 * line up seamlessly.
 *
 * @section gfx_example A minimal redraw
 *
 * @code{.c}
 * static void my_redraw(gfx_t* gfx, const os_app_t* app) {
 *     my_model_t* m = app->data->model;
 *
 *     gfx_set_font(gfx, gfx_nunito_bold_18);
 *     gfx_draw_str(gfx, 20, 40, "hello");
 *
 *     gfx_fill_rect_dithered(gfx, 20, 60, 360, 20, 3);  // grey bar
 *     gfx_draw_rect_r(gfx, 20, 60, 360, 20, 4);         // outline over it
 *
 *     gfx_set_color(gfx, 2);                            // XOR the playhead in
 *     gfx_draw_vline(gfx, 20 + m->cursor, 60, 20);
 *     gfx_set_color(gfx, 1);                            // leave it as you found it
 * }
 * @endcode
 *
 * @{
 */

#ifndef FIRMWARE_BUILD

/**
 * @name Screen geometry
 * @{
 */
#define SCREEN_WIDTH  400   /**< Display width in pixels */
#define SCREEN_HEIGHT 240   /**< Display height in pixels */
#define SCREEN_CENTER_X 200 /**< Horizontal centre */
#define SCREEN_CENTER_Y 120 /**< Vertical centre */

#define REELS_SCALE 60

#define X_CENTER ((SCREEN_WIDTH >> 2) + 10)
#define Y_CENTER 95
/** @} */

/**
 * @name Circle quadrant flags
 *
 * Bitmask for the `opt` argument of gfx_draw_circle(), gfx_draw_disc() and
 * gfx_draw_circle_dotted(). OR them together to draw part of a circle — a rounded corner is
 * one quadrant — or pass ::GFX_DRAW_ALL for the whole thing.
 * @{
 */
#define GFX_DRAW_UPPER_RIGHT 0x01
#define GFX_DRAW_UPPER_LEFT  0x02
#define GFX_DRAW_LOWER_LEFT  0x04
#define GFX_DRAW_LOWER_RIGHT 0x08
#define GFX_DRAW_ALL \
    (GFX_DRAW_UPPER_RIGHT | GFX_DRAW_UPPER_LEFT | GFX_DRAW_LOWER_RIGHT | GFX_DRAW_LOWER_LEFT)
/** @} */

/**
 * @brief Paint a single pixel in the current colour
 * @param gfx Graphics context
 * @param x X coordinate
 * @param y Y coordinate
 */
void gfx_draw_pixel(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y);

/**
 * @brief Horizontal line running right from (x, y)
 * @param gfx Graphics context
 * @param x Left end
 * @param y Row
 * @param w Length in pixels
 * @note This is the fastest primitive there is — it writes whole framebuffer bytes at a
 * time. Building a fill out of hlines beats building it out of pixels by a wide margin.
 */
void gfx_draw_hline(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w);

/**
 * @brief Vertical line running down from (x, y)
 * @param gfx Graphics context
 * @param x Column
 * @param y Top end
 * @param h Length in pixels
 */
void gfx_draw_vline(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t h);

/**
 * @brief Axis-aligned line in one of four directions
 * @param gfx Graphics context
 * @param x Start X
 * @param y Start Y
 * @param len Length in pixels
 * @param dir 0 = right, 1 = down, 2 = left, 3 = up
 * @note `dir` 0 and 1 are gfx_draw_hline() / gfx_draw_vline() exactly; 2 and 3 are the same
 * lines drawn backwards from (x, y). Call the hline/vline pair directly unless the direction
 * is a runtime value.
 */
void gfx_draw_hvline(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t len, uint8_t dir);

/**
 * @brief Line between two arbitrary points (Bresenham)
 * @param gfx Graphics context
 * @param x1 First point X
 * @param y1 First point Y
 * @param x2 Second point X
 * @param y2 Second point Y
 */
void gfx_draw_line(gfx_t* gfx, gfx_uint_t x1, gfx_uint_t y1, gfx_uint_t x2, gfx_uint_t y2);

/**
 * @brief Line with a thickness, drawn as a stack of parallel lines
 * @param gfx Graphics context
 * @param x1 First point X
 * @param y1 First point Y
 * @param x2 Second point X
 * @param y2 Second point Y
 * @param thickness Width in pixels; 1 is gfx_draw_line()
 */
void gfx_draw_line_thick(gfx_t* gfx, gfx_uint_t x1, gfx_uint_t y1, gfx_uint_t x2, gfx_uint_t y2, uint16_t thickness);

/**
 * @brief Quadratic Bezier curve through 16 straight segments
 * @param gfx Graphics context
 * @param x0 Start X
 * @param y0 Start Y
 * @param x1 Control point X — the curve is pulled toward it but does not touch it
 * @param y1 Control point Y
 * @param x2 End X
 * @param y2 End Y
 */
void gfx_draw_bezier(gfx_t* gfx, gfx_uint_t x0, gfx_uint_t y0, gfx_uint_t x1, gfx_uint_t y1, gfx_uint_t x2, gfx_uint_t y2);

/**
 * @brief Rectangle outline
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Width in pixels
 * @param h Height in pixels
 */
void gfx_draw_rect(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h);

/**
 * @brief Rectangle outline with rounded corners
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Width in pixels
 * @param h Height in pixels
 * @param r Corner radius; must not exceed half the shorter side
 */
void gfx_draw_rect_r(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, gfx_uint_t r);

/**
 * @brief Solid rectangle
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Width in pixels
 * @param h Height in pixels
 * @note Filling with colour 0 is how you erase a region before redrawing it.
 */
void gfx_draw_rect_fill(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h);

/**
 * @brief Solid rectangle with rounded corners
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Width in pixels
 * @param h Height in pixels
 * @param r Corner radius; must not exceed half the shorter side
 */
void gfx_draw_rect_fill_r(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, gfx_uint_t r);

/**
 * @brief Rectangle filled with a dither pattern instead of solid colour
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Width in pixels
 * @param h Height in pixels
 * @param shade 0-7, see @ref gfx_shades — 0 is the sparsest, 7 the densest
 */
void gfx_fill_rect_dithered(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, uint8_t shade);

/**
 * @brief Dithered horizontal line
 * @param gfx Graphics context
 * @param x Left end
 * @param y Row
 * @param w Length in pixels
 * @param shade 0-7, see @ref gfx_shades
 */
void gfx_draw_hline_dithered(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, uint8_t shade);

/**
 * @brief Dithered vertical line
 * @param gfx Graphics context
 * @param x Column
 * @param y Top end
 * @param h Length in pixels
 * @param shade 0-7, see @ref gfx_shades
 */
void gfx_draw_vline_dithered(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t h, uint8_t shade);

/**
 * @brief Dithered filled circle
 * @param gfx Graphics context
 * @param cx Centre X
 * @param cy Centre Y
 * @param r Radius in pixels
 * @param shade 0-7, see @ref gfx_shades
 */
void gfx_fill_circle_dithered(gfx_t* gfx, gfx_uint_t cx, gfx_uint_t cy, gfx_uint_t r, uint8_t shade);

/**
 * @brief Dithered rounded rectangle
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Width in pixels
 * @param h Height in pixels
 * @param r Corner radius
 * @param shade 0-7, see @ref gfx_shades
 */
void gfx_fill_rect_r_dithered(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, gfx_uint_t r, uint8_t shade);

/**
 * @brief Rectangle with a vertical dither gradient
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Width in pixels
 * @param h Height in pixels
 * @param shade_top Shade at the top row, 0-7
 * @param shade_bottom Shade at the bottom row, 0-7
 * @note Equal top and bottom shades give a flat fill, so this doubles as
 * gfx_fill_rect_dithered() when you are animating a gradient in and out.
 */
void gfx_fill_rect_gradient_v_dithered_bayer(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, uint8_t shade_top, uint8_t shade_bottom);

/**
 * @brief Rectangle with a horizontal dither gradient
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Width in pixels
 * @param h Height in pixels
 * @param shade_left Shade at the left column, 0-7
 * @param shade_right Shade at the right column, 0-7
 */
void gfx_fill_rect_gradient_h_dithered_bayer(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, uint8_t shade_left, uint8_t shade_right);

/**
 * @brief Circle outline, whole or by quadrant
 * @param gfx Graphics context
 * @param x0 Centre X
 * @param y0 Centre Y
 * @param r Radius in pixels
 * @param opt Quadrant mask — ::GFX_DRAW_ALL, or an OR of the ::GFX_DRAW_UPPER_RIGHT family
 */
void gfx_draw_circle(gfx_t* gfx, gfx_uint_t x0, gfx_uint_t y0, gfx_uint_t r, uint8_t opt);

/**
 * @brief Filled circle, whole or by quadrant
 * @param gfx Graphics context
 * @param x0 Centre X
 * @param y0 Centre Y
 * @param r Radius in pixels
 * @param opt Quadrant mask — ::GFX_DRAW_ALL, or an OR of the ::GFX_DRAW_UPPER_RIGHT family
 */
void gfx_draw_disc(gfx_t* gfx, gfx_uint_t x0, gfx_uint_t y0, gfx_uint_t r, uint8_t opt);

/**
 * @brief Circle outline drawn as a dotted ring
 * @param gfx Graphics context
 * @param x0 Centre X
 * @param y0 Centre Y
 * @param r Radius in pixels
 * @param opt Quadrant mask — ::GFX_DRAW_ALL, or an OR of the ::GFX_DRAW_UPPER_RIGHT family
 */
void gfx_draw_circle_dotted(gfx_t* gfx, gfx_uint_t x0, gfx_uint_t y0, gfx_uint_t r, uint8_t opt);

/**
 * @brief Select what subsequent draw calls do to the pixels they touch
 * @param gfx Graphics context
 * @param color 0 = clear (black), 1 = set (white), 2 = XOR
 * @note Sticky — it survives into the next frame. See @ref gfx_color.
 */
void gfx_set_color(gfx_t* gfx, uint8_t color);

/**
 * @brief Draw a string in the current font and colour
 * @param gfx Graphics context
 * @param x Left edge of the first glyph
 * @param y Text baseline — glyphs sit *above* this row, so y is the bottom of the line
 * @param str NUL-terminated ASCII/UTF-8 string
 * @return Width drawn, in pixels — add it to x to continue on the same line
 */
gfx_uint_t gfx_draw_str(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, const char* str);

/**
 * @brief printf-style gfx_draw_str()
 * @param gfx Graphics context
 * @param x Left edge of the first glyph
 * @param y Text baseline
 * @param fmt printf format string
 * @return Width drawn, in pixels
 * @warning The formatted result is built in a shared 128-byte buffer and truncated to fit.
 * Do not call it from an audio or timer callback while the UI thread may also be drawing.
 * @note `%f` costs far more than the integer conversions. For a value you redraw every
 * frame, scale to an int yourself: `gfx_draw_strf(gfx, x, y, "%d%%", (int)(v * 100))`.
 */
gfx_uint_t gfx_draw_strf(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, const char* fmt, ...);

/**
 * @brief Draw one glyph by character code
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Baseline
 * @param encoding Character code (ASCII, or a Unicode code point in a font that has it)
 * @return Advance width of that glyph in pixels
 */
uint16_t gfx_draw_glyph(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, uint16_t encoding);

/**
 * @brief Select the font for subsequent text calls
 * @param gfx Graphics context
 * @param font One of the exported font blobs — ::gfx_nunito_semibold_14,
 *             ::gfx_nunito_bold_18 or ::DepartureMono_Regular_10
 * @note Sticky, like the colour. Setting a font is cheap; measuring text is not, so
 * hoist gfx_get_str_width() out of loops rather than the gfx_set_font() call.
 */
void gfx_set_font(gfx_t* gfx, const uint8_t* font);

/**
 * @brief Measure a string in the current font without drawing it
 * @param gfx Graphics context
 * @param str NUL-terminated string
 * @return Width in pixels
 * @code{.c}
 * // centre a label
 * const gfx_uint_t w = gfx_get_str_width(gfx, label);
 * gfx_draw_str(gfx, SCREEN_CENTER_X - (w >> 1), 120, label);
 * @endcode
 */
gfx_uint_t gfx_get_str_width(gfx_t* gfx, const char* str);

/**
 * @brief Blit a 1-bit XBM bitmap
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Bitmap width in pixels
 * @param h Bitmap height in pixels
 * @param bitmap XBM data: one bit per pixel, LSB first, each row padded to a whole byte
 * @note The blit is opaque — 0 bits are painted in the opposite colour, so the bitmap's
 * bounding box is fully overwritten. Prefer ui_draw_img(), which takes a ::gfx_img_t from
 * the asset pipeline and knows its own dimensions.
 */
void gfx_draw_xbm(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, const uint8_t* bitmap);

/**
 * @brief Blit a bitmap rotated about its own centre
 * @param gfx Graphics context
 * @param x Left edge of the unrotated bounding box
 * @param y Top edge of the unrotated bounding box
 * @param w Bitmap width in pixels
 * @param h Bitmap height in pixels
 * @param bitmap XBM data
 * @param angle Rotation in radians, clockwise
 * @note Per-pixel and transparent, unlike gfx_draw_xbm(): only set bits are painted, and
 * corners can spill outside the original w x h box.
 * @warning This variant reads the bitmap MSB first — the opposite of gfx_draw_xbm() and
 * gfx_draw_xbm_scaled(). Asset-pipeline bitmaps come out mirrored within each byte, so check
 * on device before you build a UI on it.
 */
void gfx_draw_xbm_rotated(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, const uint8_t* bitmap, float angle);

/**
 * @brief Blit a bitmap shrunk by an integer factor
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param bitmap XBM data — note this comes *before* the dimensions here, unlike gfx_draw_xbm()
 * @param w Source bitmap width in pixels
 * @param h Source bitmap height in pixels
 * @param scale Divisor: 2 draws at half size, 3 at a third. 1 is 1:1.
 * @warning `scale` shrinks, it does not magnify — there is no scale-up entry point in the
 * TAPP API. Sampling is nearest-neighbour, so thin 1px features can vanish entirely.
 */
void gfx_draw_xbm_scaled(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, const uint8_t* bitmap, gfx_uint_t w, gfx_uint_t h, gfx_uint_t scale);

/**
 * @brief Restrict drawing to a rectangular window
 * @param gfx Graphics context
 * @param x0 Left edge of the window
 * @param y0 Top edge of the window
 * @param x1 Right edge of the window (a coordinate, not a width)
 * @param y1 Bottom edge of the window (a coordinate, not a height)
 * @note Corner-to-corner, not x/y/w/h. The window is sticky and applies to every primitive,
 * so a forgotten clip makes the rest of your UI silently disappear — always pair it with
 * gfx_clip_reset().
 * @code{.c}
 * gfx_set_clip(gfx, 10, 40, 390, 200);   // scrolling list body
 * draw_rows(gfx, m);
 * gfx_clip_reset(gfx);
 * @endcode
 */
void gfx_set_clip(gfx_t* gfx, gfx_uint_t x0, gfx_uint_t y0, gfx_uint_t x1, gfx_uint_t y1);

/**
 * @brief Drop the clip window and draw to the whole screen again
 * @param gfx Graphics context
 */
void gfx_clip_reset(gfx_t* gfx);

#endif // !FIRMWARE_BUILD

/**
 * @brief Get the current frame counter (for animations)
 * @param gfx Graphics context
 * @return Frame counter value, incremented on each display refresh
 * @note This is the clock to drive animation from — a tapp has no wall time. Divide it
 * down for slower motion: `ui_draw_anim(gfx, x, y, ui_get_frame(gfx) >> 2, &my_anim)`.
 */
uint32_t ui_get_frame(gfx_t* gfx);

/**
 * @name Fonts
 * The only fonts the firmware exports. Pass one to gfx_set_font().
 *
 * These three are the firmware's own working set (proportional regular, proportional bold,
 * monospace) and are the ones the browser emulator can render. Every other font in the
 * firmware is deliberately unexported: each exported name is ABI we can never rename or drop.
 * @{
 */
extern const uint8_t gfx_nunito_semibold_14[];  /**< Default UI body font */
extern const uint8_t gfx_nunito_bold_18[];      /**< Headings / emphasis */
extern const uint8_t DepartureMono_Regular_10[]; /**< Monospace — counters, numeric readouts */
/** @} */

/** @} */ // end of grp_graphics

// ============================================================================
// UI Components — composite widgets (from ui_components.h)
// ============================================================================

/**
 * @defgroup grp_ui UI Components
 * @brief Ready-made widgets — buttons, meters, frames, images — drawn in the firmware's style
 *
 * These sit one level above @ref grp_graphics: each is a small pile of primitives the
 * firmware's own screens use, exported so a tapp looks like it belongs on the device.
 * They take the same `gfx_t*` and obey the same colour and clip state.
 *
 * Sizes are mostly fixed by the design rather than by your arguments — ui_draw_button() is
 * always 24px tall, ui_draw_scrollbar()'s track always 8px wide — so `x`/`y` place a known
 * shape rather than defining one. Where a widget does take `w`/`h`, they are the outer bounds.
 *
 * @section ui_images Images and animations
 *
 * ::gfx_img_t comes out of the asset pipeline: drop a PNG or GIF in your tapp's `assets/`
 * folder and `tapp-build` compiles it into a `gfx_img_t` plus an `extern` declaration in
 * the generated assets header. A GIF becomes a multi-frame image; ui_draw_anim() takes the
 * frame index directly and wraps it, so drive it from ui_get_frame().
 *
 * @code{.c}
 * ui_draw_img(gfx, 20, 20, &asset_logo);
 * ui_draw_anim(gfx, 120, 60, ui_get_frame(gfx) >> 2, &asset_dance_party);
 * @endcode
 *
 * @{
 */

#ifndef FIRMWARE_BUILD

/**
 * @brief Draw the first frame of an image at (x, y)
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param img Image from the asset pipeline
 * @note Opaque, like gfx_draw_xbm(): the image's whole bounding box is overwritten.
 */
void ui_draw_img(gfx_t* gfx, uint16_t x, uint16_t y, const gfx_img_t* img);

/**
 * @brief Draw one frame of a multi-frame image
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param frame Frame index; wrapped modulo the image's frame count, so a free-running
 *              counter is fine
 * @param img Image from the asset pipeline
 * @note There is no img_animation_* API and no playback clock — you pick the frame each
 * redraw. `ui_get_frame(gfx) >> n` is the usual source; bigger `n` is slower.
 */
void ui_draw_anim(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t frame, const gfx_img_t* img);

/**
 * @brief Draw an image shrunk by an integer factor
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param img Image from the asset pipeline
 * @param scale Divisor: 2 is half size, 3 a third. 1 is 1:1.
 * @warning Shrinks only — see gfx_draw_xbm_scaled(), which this wraps.
 */
void ui_draw_img_scaled(gfx_t* gfx, uint16_t x, uint16_t y, const gfx_img_t* img, uint16_t scale);

/**
 * @brief Render a pre-encoded QR code
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param size Pixels per QR module — the code ends up `size * modules` square
 * @param qr Buffer in qrcodegen format
 * @warning The encoder is not part of the TAPP API: only this renderer is exported, so you
 * have to produce the qrcodegen buffer yourself (bundle the encoder in your tapp) or bake
 * it in as a constant.
 */
void ui_draw_qrcode(gfx_t* restrict gfx, uint16_t x, uint16_t y, uint16_t size, const uint8_t* qr);

/**
 * @brief Draw an indexed duration row — "3  1m 24s", or "3  <1s"
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Text baseline
 * @param len Duration in seconds
 * @param idx_l Index printed before the duration
 */
void ui_draw_time_entry(gfx_t* gfx, uint16_t x, uint16_t y, float len, uint16_t idx_l);

/**
 * @brief Move a position half-way toward a target — one step of an ease-out
 * @param pos Position to advance, updated in place
 * @param target Where it is heading
 * @return true once `*pos` has arrived (and nothing was changed)
 * @note Call it once per frame and redraw while it returns false. Integer halving, so it
 * lands exactly rather than creeping.
 * @code{.c}
 * if(!ui_ease_position(&m->scroll_y, m->scroll_target)) m->dirty = true;
 * @endcode
 */
bool ui_ease_position(uint16_t* pos, uint16_t target);

/**
 * @brief ui_ease_position() with an adjustable step
 * @param pos Position to advance, updated in place
 * @param target Where it is heading
 * @param mod Fraction of the half-step to take: 1.0 matches ui_ease_position(), smaller is
 *            slower. Progress of at least one unit per call is guaranteed.
 * @return true once `*pos` has arrived
 */
bool ui_ease_position_mod(uint16_t* pos, uint16_t target, float mod);

/**
 * @brief Vertical scrollbar with a proportional thumb
 * @param gfx Graphics context
 * @param x Left edge (the track is 8px wide)
 * @param y Top edge
 * @param height Track height in pixels
 * @param pos Zero-based index of the selected item
 * @param total Number of items; 0 draws the empty track
 */
void ui_draw_scrollbar(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t height, uint16_t pos, uint16_t total);

/**
 * @brief Draw a dot positioned around a circle — a knob indicator
 * @param gfx Graphics context
 * @param x Centre X of the circle
 * @param y Centre Y of the circle
 * @param value Position around the circle, 0.0-1.0
 * @param scale Radius the dot orbits at
 * @param size Radius of the dot itself
 */
void ui_draw_on_circle(gfx_t* gfx, uint16_t x, uint16_t y, float value, uint_fast8_t scale, uint_fast8_t size);

/**
 * @brief Rounded outline, with a double ring when selected
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Width in pixels
 * @param h Height in pixels
 * @param r Corner radius
 * @param active true adds two outer rings — the firmware's selection cue. They grow
 *               *outward*, so leave 2px of margin around the frame.
 */
void ui_draw_frame(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r, bool active);

/**
 * @brief Button-hint row: a label with a press/hold marker to its left
 * @param gfx Graphics context
 * @param x Left edge of the label
 * @param y Text baseline
 * @param hold true draws the "hold" bar, false the "press" dot
 * @param name Label text
 */
void ui_draw_action(gfx_t* gfx, uint16_t x, uint16_t y, bool hold, const char* name);

/**
 * @brief ui_draw_action() that can render selected, and reports its width
 * @param gfx Graphics context
 * @param x Left edge of the label
 * @param y Text baseline
 * @param hold true draws the "hold" bar, false the "press" dot
 * @param name Label text
 * @param active true fills a rounded plate behind it and inverts the contents
 * @param tall true makes the plate 22px taller, for two-line rows
 * @return Width of the label text in pixels — use it to lay the next action out
 */
uint16_t ui_draw_action_act(gfx_t* gfx, uint16_t x, uint16_t y, bool hold, const char* name, bool active, bool tall);

/**
 * @brief Fill a region with random pixels — TV static
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Width in pixels
 * @param h Height in pixels
 * @param pixel_size Vertical step between noise rows; 1 is every row
 * @warning Currently draws nothing: the firmware offsets x by the display width, putting
 * every pixel outside the clip window. Build static out of gfx_draw_pixel() until that is
 * fixed.
 */
void ui_draw_noise_zone(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint_fast8_t pixel_size);

/**
 * @brief Plot a float buffer as a dotted waveform
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param width Plot width in pixels; the buffer is resampled to fit
 * @param height Plot height in pixels — also sets the amplitude scale
 * @param buffer Sample values, roughly -1.0 to 1.0. NULL is ignored.
 * @param size Number of samples in the buffer
 */
void ui_draw_buffer_dots(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t width, uint16_t height, const float* buffer, size_t size);

/**
 * @brief Copy a string to upper case
 * @param original_str Source string
 * @param uppper Destination buffer
 * @param len Size of the destination buffer including the terminator
 */
void ui_string_upper(const char* original_str, char* uppper, int8_t len);

/**
 * @brief Copy a string to lower case
 * @param original_str Source string
 * @param upper Destination buffer
 * @param len Size of the destination buffer including the terminator
 */
void ui_string_lower(const char* original_str, char* upper, int8_t len);

/**
 * @brief ui_draw_frame() over a cleared background — an opaque panel
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Width in pixels
 * @param h Height in pixels
 * @param r Corner radius
 * @param active true adds the outer selection rings
 * @note Use this for anything floating over other content; ui_draw_frame() alone leaves
 * whatever was underneath showing through.
 */
void ui_draw_frame_wback(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r, bool active);

/**
 * @brief Horizontal bar meter over an integer range
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Outer width in pixels
 * @param h Outer height in pixels
 * @param val Current value
 * @param min Range minimum
 * @param max Range maximum
 */
void ui_draw_value_bar(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t val, uint32_t min, uint32_t max);

/**
 * @brief Slim float bar meter, drawn in the lower half of its box
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Outer width in pixels
 * @param h Outer height in pixels — the bar itself is about an eighth of it
 * @param val Current value
 * @param min Range minimum
 * @param max Range maximum
 */
void ui_draw_value_bar_smallf(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, float val, float min, float max);

/**
 * @brief Rounded box with a black / white / black border stack
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Width in pixels
 * @param h Height in pixels
 * @param round Corner radius
 * @note Leaves the draw colour at 1.
 */
void ui_draw_3layer_box(gfx_t* restrict gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t round);

/**
 * @brief Draw an icon offset by half its own size
 * @param gfx Graphics context
 * @param x Reference X
 * @param y Reference Y
 * @param icon Image to draw; NULL is ignored
 * @note The offset is added, not subtracted: the icon lands with its top-left half a
 * width/height *past* (x, y). Pass the top-left of the box you want it centred in.
 */
void ui_draw_icon_centered(gfx_t* restrict gfx, uint16_t x, uint16_t y, const gfx_img_t* icon);

/**
 * @brief Draw a string centred on the screen, nudged right if it has an icon
 * @param gfx Graphics context
 * @param y Text baseline
 * @param text Label text
 * @param icon Icon that will sit to the left of the text; NULL for text alone
 * @return X the text was drawn at — draw the icon relative to it
 */
uint16_t ui_draw_text_centered_with_icon(gfx_t* restrict gfx, uint16_t y, const char* text, const gfx_img_t* icon);

/** @brief Output style for ui_format_time() */
typedef enum {
    TIME_FORMAT_COMPACT,     /**< "MM:SS.ss" or "HH:MM:SS.ss" — timeline readouts */
    TIME_FORMAT_VERBOSE,     /**< "Xh Ym Zs" — menu labels */
} TimeFormatStyle;

/**
 * @brief Format a tape position as a time string
 * @param buff Destination buffer
 * @param size Size of the destination buffer
 * @param val Position in tape positions (SD sectors), as tape_get_position() / 64 returns
 * @param style ::TIME_FORMAT_COMPACT or ::TIME_FORMAT_VERBOSE
 * @note Compact output drops leading zero fields — a sub-minute value comes out "SS.ss".
 */
void ui_format_time(char* restrict buff, uint32_t size, float val, TimeFormatStyle style);

/**
 * @brief Fill a rectangle with a line/dot texture instead of a dither pattern
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Width in pixels
 * @param h Height in pixels
 * @param pattern Texture: 1 vertical stripes, 2 horizontal stripes, 3 cross-hatch,
 *                4 checkerboard, 5 dot grid, 6 diagonal, 7 diagonal cross, 8 fine
 *                cross-hatch, 9 large checker, 10 staggered dots, 11 vertical dashes,
 *                12 horizontal dashes, 13 dense dots, 14 stipple, 15 solid
 * @param density 1-14, spacing between texture elements — bigger is tighter. 0 draws
 *                nothing at all.
 * @note Distinct from the dithered fills: this is a geometric texture at your chosen
 * spacing, not a grey level. Clipped to the screen internally.
 */
void ui_draw_shade_pattern(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint_fast8_t pattern, uint_fast8_t density);

/**
 * @brief Single horizontal level bar
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param width Full-scale width in pixels
 * @param height Box height; the bar is half of it
 * @param values Two floats 0.0-1.0; the louder of the two is drawn
 * @param frame Unused, kept for symmetry with ui_draw_volume_stereo()
 */
void ui_draw_volume_mono(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t width, uint16_t height, const float* values, bool frame);

/**
 * @brief Two stacked level bars, left over right
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param width Full-scale width in pixels
 * @param height Box height; each bar is a third of it
 * @param values Two floats 0.0-1.0 — `values[0]` left, `values[1]` right
 * @param frame true clears a backing plate first, so the meter stays legible over content
 */
void ui_draw_volume_stereo(gfx_t* gfx, uint16_t x, uint16_t y, uint16_t width, uint16_t height, const float* values, bool frame);

/**
 * @brief Draw word-wrapped text
 * @param gfx Graphics context
 * @param str Text to draw; "\n" forces a break
 * @param max_chr_per_line Wrap width in characters
 * @param x Left edge
 * @param y Baseline of the first line
 * @return Baseline y of the last line drawn
 * @note Wraps on character count, not pixel width, so pick the limit for the font you have
 * selected.
 * @warning `max_chr_per_line` must stay at or below 100 — the firmware assembles each line in
 * a 101-byte stack buffer and does not bounds-check the limit you pass.
 */
uint32_t ui_draw_str_multi_line(gfx_t* restrict gfx, const char *str, uint_fast8_t max_chr_per_line, uint32_t x, uint32_t y);

/**
 * @brief Count the lines ui_draw_str_multi_line() would produce
 * @param str Text to measure
 * @param max_chr_per_line Wrap width in characters
 * @return Number of lines
 * @note Use it to size a panel before you draw the text into it.
 */
uint32_t ui_count_string_lines(const char* str, uint_fast8_t max_chr_per_line);

/**
 * @brief Word-wrapped text revealed a character at a time — the typewriter effect
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Baseline of the first line
 * @param count How many characters to reveal; raise it each frame to type the text out
 * @param max_chars_per_line Wrap width in characters
 * @param text Text to draw
 */
void ui_draw_dialogue_multiline_string(gfx_t* restrict gfx, uint_fast8_t x, uint_fast8_t y, uint16_t count, uint16_t max_chars_per_line, const char* restrict text);

/**
 * @brief Draw an empty speech bubble — rounded panel plus a tail below it
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param width Bubble width in pixels
 * @param height Bubble height in pixels; the tail hangs about 15px below that
 * @note Draws the container only. Follow it with ui_draw_dialogue_multiline_string() for
 * the text.
 */
void ui_draw_dialogue(gfx_t* restrict gfx, uint16_t x, uint16_t y, uint16_t width, uint16_t height);

/**
 * @brief Filled button, 24px tall, with centred text or an icon
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Button width in pixels
 * @param text Label, centred; ignored when `icon` is non-NULL
 * @param icon Icon to draw instead of the label, or NULL
 * @param pressed true shifts the face 2px up-left for the pressed look
 */
void ui_draw_button(gfx_t* restrict gfx, const uint16_t x, const uint16_t y, const uint16_t w, const char* text, const gfx_img_t* icon, const bool pressed);

/**
 * @brief ui_draw_button() with the fill and outline swapped
 * @param gfx Graphics context
 * @param x Left edge
 * @param y Top edge
 * @param w Button width in pixels
 * @param text Label, centred; ignored when `icon` is non-NULL
 * @param icon Icon to draw instead of the label, or NULL
 * @param pressed true shifts the face 2px up-left for the pressed look
 */
void ui_draw_button_inverted(gfx_t* restrict gfx, const uint16_t x, const uint16_t y, const uint16_t w, const char* text, const gfx_img_t* icon, const bool pressed);

/**
 * @brief On/off toggle
 * @param gfx Graphics context
 * @param x Reference X — the switch body starts 25px to the right of it
 * @param y Reference Y — the body sits *above* it, from y-17 to y+2
 * @param val Switch state
 * @param detailed true marks the body O for off and I for on
 */
void ui_draw_switch(gfx_t* restrict gfx, uint16_t x, uint16_t y, bool val, bool detailed);

#endif // !FIRMWARE_BUILD

/** @} */ // end of grp_ui

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

/**
 * @name Timing
 * @{
 */

/**
 * @brief Milliseconds since boot
 *
 * The only clock available to a TAPP. Use it for animation and timeouts:
 * @code{.c}
 * uint32_t now = os_tick_get();
 * if (now - m->last_step >= 120) { m->last_step = now; step(m); }
 * @endcode
 */
uint32_t os_tick_get(void);

/* NOTE: os_delay()/os_delay_until() are deliberately not exposed. Your tick(),
 * redraw() and on_input() all run on the OS controls task, so blocking inside one
 * freezes buttons and the encoder for the entire device, not just your app. Time
 * things by comparing os_tick_get() across ticks, as above. */

/** @} */

/** @} */ // end of grp_app

// ============================================================================
// LEDs
// ============================================================================

/**
 * @defgroup grp_led LEDs
 * @brief The four RGB LEDs — driven directly, or through the OS pattern engine
 *
 * The device has 4 RGB LEDs, indexed 0-3. The OS runs its own animations on them, so stop
 * whatever is playing before you drive them yourself:
 *
 * @code{.c}
 * os_led_stop_current();               // once, in init()
 * hal_led_set_rgb(0, 100, 70, 20);     // then from tick()
 * @endcode
 *
 * @section led_patterns Patterns versus direct writes
 *
 * Writing from tick() is the simple route and costs you nothing but the call. For anything
 * that animates on its own clock, hand the OS a ::led_pattern_t instead: the LED task steps
 * it on its own timer, so the animation keeps running at a steady rate even when your tick()
 * is busy, and it survives a frame you skip.
 *
 * @note Nothing restores the OS patterns when your app exits. Put the device back the way you
 * found it in deinit() — either write the LEDs yourself or call os_led_stop_current().
 *
 * @{
 */

/**
 * @brief Set one LED's colour
 * @param led_num LED index, 0-3
 * @param red,green,blue 0-255
 * @note The driver latches: an LED keeps its last written colour until you
 * change it. Nothing restores OS patterns when your app exits, so set the LEDs
 * back or call os_led_stop_current() in deinit().
 */
void hal_led_set_rgb(const uint8_t led_num, const uint8_t red, const uint8_t green,
                     const uint8_t blue);

/**
 * @brief Stop the OS LED pattern currently playing
 */
void os_led_stop_current(void);

#ifndef FIRMWARE_BUILD
/* Pattern playback --------------------------------------------------------
 * The same engine the firmware's own LED animations run on. You describe a
 * pattern as a step count plus a callback; the OS LED task advances it on its
 * own timer, so nothing has to happen in your tick().
 *
 *   static void my_cb(const os_led_t* led, const void* ctx) {
 *       uint8_t s = os_led_get_step(led);
 *       for(uint8_t i = 0; i < 4; i++) hal_led_set_rgb(i, s, 0, 255 - s);
 *   }
 *   static led_pattern_t my_pattern = {
 *       .callback = my_cb, .steps = 255, .rate = 50, .loop = 1,
 *   };
 *   os_led_set_pattern(&my_pattern, NULL);   // once, in init()
 */

/** Opaque LED task handle. The struct body is firmware-private; the only thing
 *  a tapp can do with the pointer is pass it to os_led_get_step(). */
typedef struct os_led_t os_led_t;

/** Invoked once per step, on the OS LED task (not your app's task). */
typedef void (*led_step_callback)(const os_led_t* led, const void* ctx);

typedef struct led_pattern_t led_pattern_t;

struct led_pattern_t {
    led_step_callback callback; /**< step handler (required) */
    const void* ctx;            /**< passed straight through to the callback */
    const size_t steps;         /**< steps before the pattern completes */
    uint32_t rate;              /**< ms per step (0 = OS default) */
    led_pattern_t* next;        /**< optional pattern to chain into on completion */
    bool loop;                  /**< restart on completion instead of stopping */
};

/**
 * @brief Read the step counter from inside a pattern callback
 */
uint8_t os_led_get_step(const os_led_t* led);

/**
 * @brief Start a pattern, replacing whatever is currently playing
 * @param ctx when non-NULL, overrides the pattern's own ctx for this run
 * @note The pattern outlives the call — give it static storage, not a local.
 */
void os_led_set_pattern(led_pattern_t* pattern, const void* ctx);
#endif /* !FIRMWARE_BUILD */

/** @} */ // end of grp_led

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

#ifndef FIRMWARE_BUILD
/* Q15 lookup-table math ---------------------------------------------------
 * Table-driven fixed-point primitives, the same ones the firmware DSP uses.
 * Cheaper than the float calls above and exact across builds. Q15 means
 * -32768..32767 represents -1.0..+1.0.
 *
 * Unlike isin()/icos(), which take degrees, these take a full-turn phase: the
 * uint16_t wraps naturally at one cycle, so you can just keep adding to it. */

/** sin(2*pi * phase/65536) in Q15. Interpolated, so smooth at any rate. */
int16_t lutsin_q15(uint16_t phase);

/** cos(2*pi * phase/65536) in Q15. */
int16_t lutcos_q15(uint16_t phase);

/** exp(-x) for x in [0, 32767] -> [32767, 0]. Handy for decay envelopes. */
int16_t lutexp_neg_q15(int16_t x);

/** Soft saturation curve — a tanh-like squash of a Q15 value. */
int16_t lutsat_q15(int16_t x);
#endif /* !FIRMWARE_BUILD */

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
 * @brief Put your app's text in the system statusbar (left side)
 * @param text string to show; NULL or "" removes the item
 *
 * The pointer is REFERENCED, not copied. Point it at a buffer your app owns and
 * just rewrite that buffer whenever the text changes — the statusbar follows with
 * no further calls. Call this ONCE, in init().
 *
 * The firmware owns the statusbar object, so nothing internal crosses the ABI, and
 * the item is hidden automatically when your app loses focus and dropped when it
 * exits. The right-hand side is reserved for battery/USB.
 */
void ui_statusbar_set_text(const char* text);

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
// Parameters
// ============================================================================

/**
 * @defgroup grp_params Parameters
 * @brief Smoothed, range-clamped values — the unit ui_menu edits and the encoder drives
 *
 * A params_t holds a target and a smoothed current value. You set the target;
 * param_update_fast() walks `val` toward it and returns true on the frames it
 * changed, which is your cue to redraw. Set `refresh = true` or `val` never
 * follows `target`.
 *
 * @section param_encoder Driving a param from the encoder
 * @code{.c}
 * // in tick()
 * param_write_delta_coarse(&m->cutoff, os_controls_encoder_get_delta());
 * if (param_update_fast(&m->cutoff, m)) m->dirty = true;
 *
 * // in redraw()
 * gfx_draw_strf(gfx, 10, 40, "cutoff %d%%", (int)(param_val_percent(&m->cutoff) * 100));
 * @endcode
 *
 * Put the same params_t in a ui_menu_node_t's `param` field and the menu edits it
 * for you — the two paths are interchangeable.
 *
 * @note There is no param_init(): it hands back a slot from a firmware-owned pool.
 * Declare params_t values in your own model and fill the fields directly.
 * @{
 */

/** @brief Set the value immediately (clamped to min/max), skipping smoothing */
void param_set(params_t* param, const float value);

/** @brief Set the value with no clamp and no smoothing — fast path for audio code */
void param_set_rawfast(params_t* param, const float value);

/** @brief Set the target; `val` then eases toward it on param_update_fast() */
void param_set_target(params_t* param, const float value);

/** @brief Add to the target (clamped) */
void param_inc(params_t* param, const float value);

/** @brief Subtract from the target (clamped) */
void param_dec(params_t* param, const float value);

/** @brief Advance `val` toward `target`; true if it moved this call (redraw cue) */
bool param_update_fast(params_t* param, void* ctx);

/** @brief Advance `val` toward `target` without firing the callback */
bool param_update_val(params_t* param);

/** @brief Did the value change since the last check? */
bool param_updated(const params_t* param);

/** @brief Apply an encoder delta using the param's `coarse` step */
void param_write_delta_coarse(params_t* param, int32_t acc);

/** @brief Apply an encoder delta using the param's `fine` step */
void param_write_delta_val(params_t* param, int32_t acc);

/** @brief Called whenever the value changes; receives the param and your ctx */
void param_set_callback(params_t* param, void (*callback)(params_t* self, void* context));

/** @brief Raw current value, before range mapping */
float param_raw(const params_t* param);

/** @brief Current value rescaled from the param's range into [min, max] */
float param_map(const params_t* param, const float min, const float max);

/** @brief Current value as 0.0-1.0 across the param's own range */
float param_val_percent(const params_t* param);

/** @brief Do two params hold the same value? */
bool param_same(const params_t* p1, const params_t* p2);

/** @} */ // end of grp_params

// ============================================================================
// Notifications
// ============================================================================

/**
 * @defgroup grp_notify Notifications
 * @brief Toasts and modal dialogues drawn over your app
 *
 * The firmware owns one notification. Every function here acts on it, so there is
 * nothing to allocate or free — pass NULL wherever a ui_notify_t* is asked for.
 *
 * @warning A **modal** notification swallows input: while it is up the firmware
 * consumes button events before your on_input() sees them and drains the encoder
 * accumulator, so the first os_controls_encoder_get_delta() afterwards reads 0.
 * Do not drive your UI from on_input() while a modal is showing.
 * @{
 */

/** @brief Toast: header + hint + optional icon. @param timeout_ms 0 = stay until hidden */
void ui_notify_info(const char* header, const char* hint, const gfx_img_t* icon,
                    uint16_t timeout_ms);

/** @brief Error toast */
void ui_notify_error(const char* message, uint16_t timeout_ms);

/** @brief Success toast */
void ui_notify_success(const char* message, uint16_t timeout_ms);

/** @brief Warning toast */
void ui_notify_warning(const char* message, uint16_t timeout_ms);

/**
 * @brief Yes/no dialogue; @p callback fires on confirm
 * @code{.c}
 * static void on_erase(void* ctx, void* unused) { (void)unused; wipe(ctx); }
 * ui_notify_confirm("Erase all?", NULL, on_erase, m);
 * @endcode
 */
void ui_notify_confirm(const char* message, const gfx_img_t* icon,
                       void (*callback)(void*, void*), void* callback_ctx);

/** @brief Text dialogue with a callback */
void ui_notify_dialogue(const char* text, void (*callback)(void*, void*), void* callback_ctx);

/** @brief Popup whose callback reports which way it was dismissed */
void ui_notify_popup_with_cb(const char* header, const char* hint, const gfx_img_t* icon,
                             void (*callback)(void*, bool), void* callback_ctx);

/** @brief Hide the current notification. Pass NULL. */
void ui_notify_hide(ui_notify_t* n);

/** @brief Is a notification showing? */
bool ui_notify_is_active(void);

/** @brief Is this notification visible? Pass NULL for the current one. */
bool ui_notify_is_visible(const ui_notify_t* n);

/** @brief Make the next notification modal (swallows input — see the warning above) */
void ui_notify_set_modal(bool modal);

/** @brief Show or hide the confirm button */
void ui_notify_set_show_confirm_btn(bool show);

/** @brief Move the notification on screen */
void ui_notify_set_pos_global(const uint16_t x, const uint16_t y);

/** @} */ // end of grp_notify

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

/**
 * @brief Get the global engine handle
 *
 * Your process() callback is handed an engine_t*, but init()/tick() are not —
 * use this when you need it outside the audio callback.
 */
engine_t* engine_get(void);

/**
 * @brief Is the audio engine currently processing? (non-zero = yes)
 *
 * Returns uint_fast8_t, not bool, to match the firmware ABI — same as the
 * is_active member of engine_callbacks_t above.
 */
uint_fast8_t engine_is_active(void);

/**
 * @brief Get the global mixer handle
 *
 * The counterpart to engine_get(): your process() callback is handed a mixer_t*,
 * but init()/tick() have no other way to reach it. Use the accessors above for
 * buffers; mixer_t itself stays opaque.
 */
mixer_t* mixer_get(void);

/* NOTE: the mixer ROUTING setters — mixer_monitor_enable, mixer_mix_wet_enable,
 * mixer_output_resample_enable — are deliberately not exposed. They flip fields on
 * the one global mixer with nothing to restore them when your app exits, so a tapp
 * that sets one (or crashes holding it) leaves the whole device mis-routed; the
 * firmware's own hw_test saves and restores them by hand for exactly that reason.
 * mixer_mix_wet_enable additionally calls os_audio_update_chain(), the OS-level
 * control the note below already rules out. Mix into mixer_get_out() instead. */

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
 * | 5 | ENCODER_I | Rotation notification — see below, NOT a button |
 *
 * There is no encoder push button. ID 5 fires when the encoder is *turned*, and
 * the firmware sends it with a zero-initialised state, so `state` is always
 * `KEY_STATE_RELEASED`. Never branch on the state for ID 5 — either ignore the
 * event and poll os_controls_encoder_get_delta() from tick(), or use ID 5 purely
 * as a "something moved" hint and read the delta in response.
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

/* NOTE: os_controls_set_hold_timeout() is deliberately not exposed. The hold
 * threshold is device-wide state shared with the OS and every other app; a tapp
 * changing it would silently retune the whole UI, and nothing restores it on
 * exit. Handle your own long-press by timing KEY_STATE_PUSH with os_tick_get(). */

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
 * @brief Add a cue (a navigable point of interest) spanning [start, end) frames
 * @return false if the span is empty or shorter than one 64-frame sector
 *
 * Cues are what the device's skip-to-cue navigation steps through. Duplicates
 * within one sector are merged, the list is kept in tape order, and the save is
 * flagged for you. A cue at position 0 is perfectly legal.
 */
bool tape_cue_add(tape_t* tape, uint32_t start_frame, uint32_t end_frame);

/**
 * @brief Number of cues on the tape
 */
uint32_t tape_cues_count(tape_t* tape);

/**
 * @brief Read cue `idx` (0..tape_cues_count-1), in stereo frames
 * @return false if idx is out of range
 *
 * Cues are stored in tape order, so stepping between them is index +/- 1.
 */
bool tape_cue_get(tape_t* tape, uint32_t idx, uint32_t* start_frame, uint32_t* end_frame);

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

/**
 * @brief Bind YOUR params to a timeline track, instead of the tapehead singleton
 * @param track Track index (0-3), within the count given to tape_timeline_setup()
 * @param pos   Playhead position, in tape positions (frames / 64)
 * @param start Loop/selection start
 * @param end   Loop/selection end
 *
 * Use this rather than tape_timeline_attach_default() when your TAPP owns the
 * engine: the tapehead singleton stops advancing then, so a default-attached
 * timeline sits frozen. Drive the three params yourself from tick() and the widget
 * follows.
 *
 * Call after tape_timeline_setup() and before tape_timeline_show(true). The
 * params must outlive the timeline — put them in your model, not on the stack —
 * and the visible span is taken from the currently loaded tape, so there is
 * nothing to size by hand.
 *
 * @code{.c}
 * tape_timeline_setup(1, 8, 206, SCREEN_WIDTH - 16, 28);
 * tape_timeline_attach_params(0, &m->tl_pos, &m->tl_start, &m->tl_end);
 * tape_timeline_switch_track(0);
 * tape_timeline_show(true);
 * @endcode
 */
void tape_timeline_attach_params(uint8_t track, params_t* pos, params_t* start, params_t* end);

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

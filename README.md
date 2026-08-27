# TAPP SDK - *tape!* application development kit

Build applications for *tape!* using this standalone SDK.

**[API reference →](https://sdk.b.edti.me)**

## What is TAPP?

TAPP (*tape!* application) is a format for dynamically loaded applications that run on *tape!* hardware. TAPPs are compiled ELF32 relocatable objects that the firmware loads from SD card at runtime.

## Quick start - no install

**[Build a tapp in your browser →](https://tapp.b.edti.me)**

Drop a `.c` file (or a whole folder) on the page and it compiles, verifies and hands you a `.tapp`.
Nothing to install: clang and ld.lld are compiled to WebAssembly and run in the tab.

It is not a preview or a simulator of the build - it runs **the same clang 18.1.8 with the same
flags** as `tapp-build` below, so the `.tapp` you download is the artifact you would have built
locally, and the same one the device loads.

Press **run** and it executes your `.tapp` then and there, on the real ARM binary: the emulator runs
the actual instructions through a WebAssembly-compiled CPU emulator, with audio and the 400×240
display. So you can go from source to hearing it without a device.

Reach for the local toolchain when you want it in a script, in CI, or offline.

## Local toolchain

### Prerequisites

- **LLVM**: `clang` + `ld.lld` (version 15+) - the only supported toolchain
- **ImageMagick**: For converting PNG images to 1-bit bitmaps (optional, only if using assets)

`tapp-build` needs clang and `ld.lld` **together**. On macOS, Homebrew's llvm is keg-only, so
`command -v clang` finds Apple's clang - which cannot target ARM bare metal and ships no `ld.lld`.
`tapp-build` probes the kegs automatically; you do not need to add them to `PATH`.

Install on macOS:
```bash
brew install llvm imagemagick
```

Install on Linux:
```bash
sudo apt-get install clang lld llvm imagemagick
```

Install on Windows - use **WSL** and follow the Linux instructions verbatim. MSYS2/MINGW64 does not
package a bare-metal-capable clang + `ld.lld`, so it cannot build tapps.

### Build Your First TAPP

```bash
make init my_app     # scaffolds my_app/my_app.c
./tapp-build my_app  # -> my_app.tapp
```

`make init` writes a working app you can build and run straight away - a counter with button hints
and a BTN4-hold exit. It creates a **folder** rather than a bare `.c`, so you can drop in more
sources, an `inc/` folder or an `assets/` folder later without moving anything. It refuses to
overwrite an existing directory.

Then **deploy**: create an "apps" folder via disk mode on your *tape!*, copy `my_app.tapp` into it,
and the app appears in the setup menu.

The rest of this section explains what the generated file contains.

1. **Write your app** (`my_app.c`):

```c
#include "tapp_api.h"

typedef struct {
    int counter;
} my_model_t;

static bool my_init(os_app_t* app, va_list args) {
    my_model_t* m = os_app_get_model(app);
    m->counter = 0;
    return true;
}

static bool my_deinit(os_app_t* app) {
    return true;
}

static void my_redraw(gfx_t* gfx, const os_app_t* app) {
    my_model_t* m = os_app_get_model(app);
    gfx_set_color(gfx, 1);
    gfx_draw_strf(gfx, 10, 30, "Counter: %d", m->counter);
}

static bool my_tick(os_app_t* app) {
    my_model_t* m = os_app_get_model(app);
    int32_t diff = os_controls_encoder_get_delta();
    if (diff != 0) {
        m->counter += diff;
    }
    return true;
}

static void my_input(os_app_t* app, uint8_t btn, KeyStateEnum state) {
    my_model_t* m = os_app_get_model(app);
    if (btn == 0 && state == KEY_STATE_PRESSED) {
        m->counter++;
    }
    if (btn == 3 && state == KEY_STATE_HOLD) {
        os_app_exit();
    }
}

static os_app_data_t my_data = {
    .model = NULL,
    .model_size = sizeof(my_model_t),
    .init = my_init,
    .deinit = my_deinit,
};

static os_app_t my_app = {
    .name = "My App",
    .type = AppFullscreenType,
    .data = &my_data,
    .redraw = (os_app_redraw)my_redraw,
    .tick = my_tick,
    .on_input = my_input,
};

os_app_t* tapp_get_descriptor(void) {
    return &my_app;
}
```

2. **Build**:

```bash
./tapp-build my_app.c
```

3. **Deploy**:

Create "apps" folder via disk mode on your *tape!*, and copy `my_app.tapp` - apps entry will appear in setup menu

## SDK Structure

```
sdk/
├── tapp_api.h          # API header (types and functions)
├── tapp-build          # Build script - compiles and links a .tapp
├── Makefile            # make init (scaffold), example, all, check, clean
├── tools/
│   ├── verify-tapp.sh      # Standalone gate checker (tapp-build runs it automatically)
│   ├── api_exports.py      # What tapp_api.h exports; verify-tapp.sh's import gate reads it
│   ├── tapp_skeleton.c.in  # Template `make init` copies (@NAME@ is substituted)
│   └── discard.ld          # Linker script; drops clang's .ARM.exidx (the loader rejects it)
├── examples/           # A flat <name>.c, or a <name>/ folder — tapp-build takes either
│   ├── simple_app.c    # Minimal example (gfx + input)
│   ├── my_app.c        # Audio synthesis + MIDI example
│   ├── tuner.c         # Chromatic tuner + circle of fifths
│   ├── chopper.c       # Tape-marks cutup composer
│   ├── recorder.c      # Recording to tape + live waveform
│   ├── grooves/        # Morphing 6-track generative groovebox (an audio source)
│   ├── leds/           # LED patterns (multi-file)
│   ├── persistence/    # Saving settings with a .proto schema
│   └── demo/           # Assets demo (sprites + animation)
└── README.md           # This file
```

## Input Handling

### Hardware Controls

- **5 buttons**: BTN1-BTN5
- **1 rotary encoder** (rotation only, no push)

### Control Events

| Event | ID | Description |
|-------|-----|-------------|
| BTN1_I | 0 | Play button |
| BTN2_I | 1 | Record button |
| BTN3_I | 2 | Cue button |
| BTN4_I | 3 | Menu button (hold to exit) |
| BTN5_I | 4 | Wake button |
| ENCODER_I | 5 | Encoder rotation |

### Button States

```c
KEY_STATE_RELEASED  // Button released
KEY_STATE_PRESSED   // Button just pressed
KEY_STATE_HOLD      // Button held down
```

### Using on_input (Recommended)

```c
static void my_input(os_app_t* app, uint8_t btn, KeyStateEnum state) {
    if (btn == 0 && state == KEY_STATE_PRESSED) {
        // BTN1 pressed
    }
    if (btn == 3 && state == KEY_STATE_HOLD) {
        os_app_exit();  // Exit on BTN4 hold
    }
}

static os_app_t my_app = {
    // ...
    .on_input = my_input,
};
```

### Reading Encoder

```c
int32_t diff = os_controls_encoder_get_delta();
// diff > 0: clockwise
// diff < 0: counter-clockwise
```

## API Reference

### Graphics Functions

```c
// Color (0=white, 1=black)
void gfx_set_color(gfx_t* gfx, uint8_t color);

// Primitives
void gfx_draw_pixel(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y);
void gfx_draw_line(gfx_t* gfx, gfx_uint_t x1, gfx_uint_t y1, gfx_uint_t x2, gfx_uint_t y2);
void gfx_draw_hline(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w);
void gfx_draw_vline(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t h);

// Rectangles
void gfx_draw_rect(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h);
void gfx_draw_rect_fill(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h);

// Circles
void gfx_draw_circle(gfx_t* gfx, gfx_uint_t x0, gfx_uint_t y0, gfx_uint_t r, uint8_t opt);
void gfx_draw_disc(gfx_t* gfx, gfx_uint_t x0, gfx_uint_t y0, gfx_uint_t r, uint8_t opt);

// Text
gfx_uint_t gfx_draw_str(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, const char* str);
gfx_uint_t gfx_draw_strf(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, const char* fmt, ...);

// Images
void ui_draw_img(gfx_t* gfx, uint16_t x, uint16_t y, const gfx_img_t* img);
void gfx_draw_xbm(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, const uint8_t* bitmap);

// Dithered fill (shade 0 = nearly black ... 7 = nearly white)
void gfx_fill_rect_dithered(gfx_t* gfx, gfx_uint_t x, gfx_uint_t y, gfx_uint_t w, gfx_uint_t h, uint8_t shade);
```

### Memory Management

```c
void* os_malloc(size_t size);
void os_free(void* ptr);
```

**Model size:** the firmware allocates your model from fast internal RAM at launch and
**refuses the launch** if it doesn't fit — up to ~64 KB is reliable today, ~100 KB already
fails with "Model N KB > free RAM" on screen. Keep the model to state/UI (a few KB) and
allocate big buffers (sample memory, delay lines, FFT tables) in `init()` with `os_malloc()`.
`os_malloc()` has no such limit — large blocks may come from external PSRAM, which is
write-back cached and fine at audio rate (delay lines, sample buffers). Internal RAM still
has the lowest worst-case latency, so keep your very hottest per-sample state small;
smaller allocations are likelier to land internal.

### App Control

```c
void os_app_exit(void);
void* os_app_get_model(const os_app_t* app);
```

### File I/O

```c
uint32_t storage_read_file(const char* path, void* buffer, size_t buffer_size);
uint32_t storage_write_file(const char* path, const void* buffer, size_t size);
bool storage_file_exists(const char* path);
uint32_t storage_file_size(const char* path);
```

### Persisting settings

Nothing saves for you: the `memory_if` field of your descriptor is overwritten by the loader and
its callbacks are NULL, so load in `init()` and save in `deinit()`. Never call storage from
`process()` — it blocks on the SD card. Test the write result as `> 0`, not `== size`;

For a couple of values, write a struct behind a magic word. For anything you expect to extend, use
a `.proto`: protobuf keys fields by number, so a file written by an older build still loads after
you add a field. Drop the schema next to your `.c` and build the **folder** —

```bash
./tapp-build myapp          # myapp/myapp.c + myapp/settings.proto
```

`tapp-build` runs nanopb, then compiles and links the generated descriptors and the nanopb runtime
(~4KB) into your `.tapp`. Include the result as `"settings.pb.h"`; it is generated into a temp
directory that is already on the include path. Bound every string/repeated field with
`[(nanopb).max_size = N]` / `[(nanopb).max_count = N]`, or nanopb emits callback fields instead of
plain C members.

nanopb is **not bundled** — add it yourself if you use `.proto`:

```bash
git submodule add https://github.com/nanopb/nanopb.git nanopb
pip install protobuf grpcio-tools
```

or point `NANOPB_DIR` at an existing checkout. Inside the firmware tree it is found automatically.

The [browser builder](https://tapp.b.edti.me) compiles `.proto` too — it runs protoc as wasm and
then nanopb's own generator, so the `.pb.c` it produces is byte-identical to the CLI's, and nothing
needs installing. See `examples/persistence/` and the Persistence page of the API docs.

### Math Utilities

```c
uint32_t isqrt(uint32_t n);
int16_t isin(uint16_t angle);   // Angle in degrees (0-360), returns -32768..32767
int16_t icos(uint16_t angle);   // Angle in degrees (0-360), returns -32768..32767
```

### Screen Constants

```c
#define SCREEN_WIDTH  400
#define SCREEN_HEIGHT 240
```

## Audio Engine

Real-time audio processing for TAPPs.

### Architecture

- **Sample rate**: 48kHz
- **Buffers**: interleaved stereo (L, R, L, R, ...)
- **Block size**: a build-time constant — take it from `mixer_get_fs(mix)` on every
  block and never hardcode a number. `fs` is the count of *samples* in the buffer
  (L+R combined), so a stereo loop steps `i += 2` and runs `fs / 2` frames.

### Setting Up Audio

```c
static void my_process(engine_t* engine, mixer_t* mix) {
    float* out = mixer_get_out(mix);
    uint32_t fs = mixer_get_fs(mix);

    for (uint32_t i = 0; i < fs; i += 2) {
        float sample = generate_sample();
        out[i] = sample;      // Left
        out[i + 1] = sample;  // Right
    }
}

static engine_callbacks_t my_engine = {
    .process = my_process,
};

static os_app_t my_app = {
    .name = "Synth",
    .engine_cb = &my_engine,  // that's it — auto-registered with your model as context
    // ...
};
```

Setting `.engine_cb` is enough: the firmware registers your callbacks (with the model
as context) once your `init()` returns, and `process()` starts running. Call
`engine_set_callbacks(app, ctx)` yourself only when you need one of two things:

- **a different context** than the model — e.g. a sub-struct of it, or
- **to call `engine_set_active()` from inside `init()`** — auto-registration lands
  *after* `init()` returns, and a `set_active` with nothing installed yet is dropped
  (it logs a warning to the console rather than failing silently).

In that case the order is install first, activate second:

```c
engine_set_callbacks(app, model);   // first
engine_set_active(true);            // then
```

### Mixer Buffers

```c
float* mixer_get_in(mixer_t* m);   // Input buffer
float* mixer_get_out(mixer_t* m);  // Output buffer (write here!)
float* mixer_get_fx(mixer_t* m);   // FX send buffer
uint32_t mixer_get_fs(mixer_t* m); // Frame size
```

## Source tapps

A standalone tapp *takes over* the tape engine while it runs: it owns the audio path.

Add one line at file scope and your tapp becomes a **source** instead: a source tapp can be loaded instead of fx or radido.
The tapedecks keep running underneath you, your audio lands on the record path, and can be recorded straight to tape, process
WET input and tapehead sends.

```c
#include "tapp_api.h"

TAPP_DECLARE_TYPE(AppTypeSrc);   // <- the entire opt-in
```

No declaration at all means `AppTypeStandalone` — the classic engine-replacing tapp.

### Writing the audio

```c
static void my_process(engine_t* engine, mixer_t* mix) {
    my_model_t* m = engine_get_ctx(engine);
    if (!m) return;

    float* out = mixer_get_in(mix);          // the INPUT bus — what the decks record
    const uint32_t fs = mixer_get_fs(mix);

    for (uint32_t i = 0; i < fs; i += 2) {
        const float s = next_sample(m);
        out[i]     = s;                      // overwrite; don't +=
        out[i + 1] = s;
    }
}
```

Three rules, and they are the whole difference from a standalone tapp:

1. **Write `mixer_get_in()`**, not `mixer_get_out()`. That is the bus the tapedecks
   record from.
2. **Fetch the pointer and `fs` inside `process()`, every block** rather than
   caching them at init — the mixer owns those buffers, not you.
3. **Overwrite the whole buffer**, don't accumulate into it. On entry it holds zeros
   — unless the user has WET on, in which case it holds the live input at full rate,
   ready for you to transform in place.

The device's input-level knob scales what you produce, exactly as it does the radio.

### The decks' FX sends

`mixer_get_fx()` hands you whatever the four tapedecks are feeding their **Send**
into — one block behind (2.667 ms), and all zeros when no deck has Send up.
It is a separate buffer from `mixer_get_in()`, so you can treat the live input and
the tape sends differently: duck one with the other, reverb only the sends, or
ignore them entirely.

You **own** that buffer for the length of your `process()` call, exactly as the
built-in FX chain does, and whatever you leave in it is what the user hears:

| what you do | what the user hears |
|---|---|
| nothing | the sends pass through dry — ignoring them doesn't mute them |
| overwrite it | your processed version, instead of the dry sends |
| mix it into your output, then zero it | heard once, through you |
| mix it in and *don't* zero it | heard twice |

One hazard: mixing the sends into `mixer_get_in()` closes a real feedback loop —
sends → you → `in_bus` → recorded to tape → played back → sends. Route them to
your output, or use them as a control signal, but think before folding them into
the signal the decks are recording.

### Working at 24 kHz (or 12 kHz)

You always get the real 48 kHz bus — the firmware does no rate conversion for you.
Halving your own rate halves your per-block DSP budget, which is what the built-in
radio does and a good default for a synth or groovebox. Render into your own
half-length scratch and convert on the way out:

```c
typedef struct {
    float       scratch[256];   // 24 kHz render, L,R interleaved
    ups2x_f32_t ups;            // upsampler state — persists across blocks
    /* ... */
} my_model_t;

static void my_process(engine_t* engine, mixer_t* mix) {
    my_model_t* m = engine_get_ctx(engine);
    float* const bus = mixer_get_in(mix);
    const uint32_t fs = mixer_get_fs(mix);
    const uint32_t n24 = fs >> 1;               // half as many samples at 24 kHz

    for (uint32_t i = 0; i < n24; i += 2) {     // render at 24 kHz
        const float s = next_sample(m);
        m->scratch[i] = s;
        m->scratch[i + 1] = s;
    }

    for (uint32_t i = 0; i < fs; i++) bus[i] = 0.f;   // ups2x ADDS — clear to replace
    ups2x_f32_add(&m->ups, m->scratch, bus, n24);
}
```

Keep the `ups2x_f32_t` in your model and pass the same one every block — it carries
the interpolator across the block seam. For **12 kHz**, cascade two stages through a
24 kHz intermediate (two states, two calls). Going to PROCESS the live input at your
lower rate? `dec2x_f32()` is the matching decimator.

Both directions cost top end: a 24 kHz render dulls above roughly 10 kHz, and
cascaded from 12 kHz it starts around 2.5 kHz — so treat 12 kHz as a lo-fi/pad rate,
and stay at 48 kHz when sparkle matters.

### Play and stop

Drive your transport through the engine, never through a private flag:

```c
static void my_active(engine_t* e, bool on)   { (void)e; if (g_model) g_model->playing = on; }
static uint_fast8_t my_is_active(engine_t* e) { (void)e; return (g_model && g_model->playing) ? 1 : 0; }

// in your button handler:
engine_set_active(!m->playing);        // NOT m->playing = !m->playing
```

`engine_set_active()` calls your own `.active` callback, so the behaviour is
identical — but it also tells the firmware, which is the part that matters. Flipping
only your own flag leaves the engine's view of you stale, and playback can stop
responding until the tapp is reloaded.

Your callbacks are auto-registered after `init()` returns, so if you want to set the
transport state *during* `init()` you must install them yourself first — a
`set_active` that arrives before anything is installed is dropped (with a console
warning):

```c
engine_set_callbacks(app, m);   // first
engine_set_active(false);       // then (or true to come up playing)
```

### You keep playing in the background

Leaving your screen does **not** stop an input tapp: the user's exit gesture just
puts your UI away while you stay loaded, sounding and recordable — that is the whole
point. So do not copy the standalone habit of calling `engine_set_active(false)` from
`on_pause`; for an input tapp that silences you the moment the user steps out to the
menu. The user unloads you from the input-source picker when they are done.

### The UI you get

The firmware enforces a house style for source tapps, so you can rely on it:

- **No statusbar.** `ui_statusbar_*` calls are inert for the session.
- **The hint band is always visible** — `ui_hints_show(false)` is refused.
- **BTN4 / BTN5 *hold* belong to the system**: back to the menu, and out to the main
  screen. Don't assign them; put your actions on BTN1–BTN3 and on the encoder.

### How someone runs your tapp

From the main menu, on the source slot: **hold the `switch` hint**. The picker that
opens lists *fx* (the plain input path), *radio*, and every input tapp on the card —
pick yours and it loads. Source tapps deliberately do **not** appear in the regular
app launcher; the source picker is their home. Your `assets/app_icon.png` shows there
and in the menu slot (see [App icon](#app-icon) — 76x76, animation allowed).

### MIDI

Declare any `midi_*` handler in your `engine_callbacks_t` and channel-voice MIDI
(notes, CC, pitch bend, aftertouch, program change) is routed to you while you are
loaded. MIDI clock and transport always stay with the tapedecks, so the tape remains
DAW-syncable. Declare nothing and the decks keep all of it. See `tapp_api.h` for the
handler signatures.

### A complete minimal input tapp

```c
#include "tapp_api.h"

TAPP_DECLARE_TYPE(AppTypeSrc);                  // audio source

typedef struct {
    volatile bool playing;                      // written from both contexts
    uint32_t phase;
} my_model_t;

static my_model_t* g_model;                     // audio callbacks reach the model through this

static void my_process(engine_t* engine, mixer_t* mix) {
    my_model_t* m = engine_get_ctx(engine);
    if (!m) return;
    float* out = mixer_get_in(mix);
    const uint32_t fs = mixer_get_fs(mix);
    for (uint32_t i = 0; i < fs; i += 2) {
        m->phase += 178956971u;                 // ~1 kHz at 24 kHz: f * 2^32 / SR
        const float s = (m->phase < 2147483648u) ? 0.2f : -0.2f;   // square
        out[i] = s;
        out[i + 1] = s;
    }
}

static void my_active(engine_t* e, bool on)   { (void)e; if (g_model) g_model->playing = on; }
static uint_fast8_t my_is_active(engine_t* e) { (void)e; return (g_model && g_model->playing) ? 1 : 0; }

static const engine_callbacks_t my_engine = {
    .process = my_process, .active = my_active, .is_active = my_is_active,
};

static bool my_init(os_app_t* app, va_list args) {
    (void)args;
    g_model = os_app_get_model(app);
    engine_set_callbacks(app, g_model);         // install...
    engine_set_active(true);                    // ...then start sounding
    return true;
}

static bool my_deinit(os_app_t* app) { (void)app; g_model = 0; return true; }

static void my_redraw(gfx_t* gfx, const os_app_t* app) {
    const my_model_t* m = os_app_get_model((os_app_t*)app);
    gfx_set_color(gfx, 1);
    gfx_draw_str(gfx, 20, 120, (m && m->playing) ? "sounding" : "stopped");
}

static os_app_data_t my_data = {
    .model = NULL, .model_size = sizeof(my_model_t),
    .init = my_init, .deinit = my_deinit,
};

static os_app_t my_app = {
    .name = "My Source",
    .type = AppFullscreenType,
    .data = &my_data,
    .redraw = (os_app_redraw)my_redraw,
    .engine_cb = &my_engine,
};

os_app_t* tapp_get_descriptor(void) { return &my_app; }
```

`examples/grooves/` is the full-size version of this: a generative groovebox that
renders six voices into the input bus at 24 kHz.

## Using Assets (Images)

### 1. Create assets folder

```bash
mkdir assets
```

### 2. Add PNG images

Place your images in `assets/`. They will be automatically converted to 1-bit bitmaps.

```
my_app.c
assets/
├── logo.png
└── sprite.png
```

### 3. Reference in code

```c
#include "my_app_assets.h"  // Auto-generated

static void my_redraw(gfx_t* gfx, const os_app_t* app) {
    ui_draw_img(gfx, 10, 10, &asset_logo);
    ui_draw_img(gfx, 50, 50, &asset_sprite);
}
```

Asset naming:
- `assets/my_image.png` → `asset_my_image`
- `assets/cool-sprite.png` → `asset_cool_sprite`

### App icon

Provide `assets/app_icon.png` or `app_icon.gif` (animated). tapp-build converts it to a
1-bit asset whose name starts with `asset_app_icon`, which the firmware reads for menus.

Any `app_icon*` basename works — `app_icon_76_76_53.gif` is the same icon, and the
`<w>_<h>_<frames>` tail is a naming convention the build ignores (the real width, height
and frame count always come from the image itself). If you ship more than one `app_icon*`
asset, the shortest name wins.

Size: **exactly 76x76 px**, matching the built-in menu icons. Color images are dithered to
mono; animated GIFs up to **64 frames** (760 B/frame — a 53-frame icon is about 40 KB of
heap for as long as the menu holds it).

Shown full-size in the main-menu cell, looping while your tapp is the active source.

### List icon

Provide `assets/mini_app_icon.png` (or `.gif` — frame 0 is used) for the **16x16** icon
beside your tapp's name in the input-source list.

This is a **separate drawing**, not a scaled copy: the list used to downscale the big icon,
and 76x76 art sampled down to row height came out as mush, so a row now shows an icon only
when you drew one at that size. Without it the row is simply text, which is the better of
the two looks if you have nothing legible to put in 16x16.

The name is `mini_app_icon`, not `app_icon_mini`, so that neither asset's name is a prefix
of the other — the firmware matches both by prefix, and an `app_icon_mini` would be a
candidate for the big icon too.

## App Structure Requirements

### Required Components

1. **Model struct** - Your app's state
2. **Init/deinit functions** - Setup and cleanup
3. **Redraw function** - Drawing logic
4. **App data** - Links model and functions (with `model_size`!)
5. **App descriptor** - Main structure
6. **Entry point** - `tapp_get_descriptor()`

### IMPORTANT: Use Static Initializers

Always use static initialization for structs:

```c
// CORRECT
static os_app_data_t my_data = {
    .model = NULL,
    .model_size = sizeof(my_model_t),
    .init = my_init,
    .deinit = my_deinit,
};

// WRONG (will crash!)
static os_app_data_t my_data;
os_app_t* tapp_get_descriptor(void) {
    my_data.model_size = sizeof(my_model_t);  // Runtime modification!
}
```

## Standard Library Functions

Available:
- `memcmp`, `memcpy`, `memset`, `memmove`
- `strncmp`, `strncpy`, `strncat`, `strlen`, `strrchr`
- `snprintf`, `vsnprintf`

Use API alternatives for:
- Malloc → `os_malloc()`
- Free → `os_free()`
- Math → `isin()`, `icos()`, `isqrt()`

## Troubleshooting

### "Symbol not found" Error
Only use functions from the API. Check `tapp_api.h` for available functions.

### App Crashes on Load
1. Use static initializers (no runtime pointer modification!)
2. Ensure `tapp_get_descriptor()` entry point exists
3. Set `model_size` in app data

### "Model N KB > free RAM" / "Model alloc failed"
Your `model_size` is too large for contiguous free internal RAM (~64 KB is the reliable
bound). Shrink the model; move large buffers to `os_malloc()` in `init()` — see
"Memory Management" above.

### Blank Screen
1. Set draw color: `gfx_set_color(gfx, 1)`
2. Check screen bounds (400x240)
3. Implement redraw function

## Version Compatibility

**API Version**: see `@version` at the top of `tapp_api.h`, or the `TAPP_API_VERSION_MAJOR` /
`TAPP_API_VERSION_MINOR` macros beside it — both are generated, so the header always states the
version it actually provides. (This line used to name a number, and sat at 1.0 for four releases.)
**Hardware Target**: 1

## Examples

- `examples/simple_app.c` - minimal working example (gfx + input only).
- `examples/my_app.c` - audio synthesis + MIDI.
- `examples/demo/` - demo with assets and features.
- `examples/persistence/` - settings that survive a power cycle. A `.proto` schema
  (`persistence.proto`) encoded with nanopb and written with `storage_write_file`; loads in
  `init()`, saves in `deinit()`, versions the blob and clamps everything it reads back. The
  reference for the "Persisting settings" section above.

Three larger, musically-substantial, **system-integrated** examples - each uses the
`params_t` settings pattern, the system UI surfaces and the audio engine; all three
are verified against the real firmware before release:

- **`examples/tuner.c`** - chromatic tuner. `process()` downmixes `mixer_get_in`
  into a RAM ring; `tick()` runs NSDF pitch detection (no `math.h`); `redraw`
  shows a circle of fifths (`isin`/`icos`) + a big block-font note + a cents
  needle. A `ui_menu` edits the reference-A4 param.
- **`examples/grooves/`** - generative 6-track groovebox. The reference for both
  **[source tapps](#source-tapps)** (`TAPP_DECLARE_TYPE(AppTypeSrc)` — it renders at
  24 kHz into its own scratch, upsamples into the input bus with `ups2x_f32_add()`,
  and the tapedecks record it) and **inline polyphonic synthesis
  without `math.h`**. ONE morph knob (encoder) walks a **figure-8** across a 4×4 grid
  of generated 16-step patterns, bilinear-interpolating the four nearest nodes into a
  live pattern; the knob wraps, because the path is a closed loop, and the phase is
  arc-length-remapped through a LUT baked at init so every detent moves the same
  distance along the curve. Six distinct voices render in `process()`: swept-sine
  kick, resonant-BP snare, six-square 808 hat, polyBLEP saw bass, hard-sync lead and
  a detuned 3-note pad. Patterns carry a per-step `tune` byte alongside intensity —
  pitch for the melodic tracks, character for the drums, so no two kicks are alike —
  and tracks run at their own loop lengths (polymeter). Shows the uint32 phase
  accumulator + polyBLEP + Chamberlin SVF idiom, an integer step clock with a Q8
  remainder, and double-buffered pattern tables so UI-thread generation can never
  tear under `process()`. Its settings are a `params_t` per encoder target (morph,
  drift, tempo, density, tone, key, scale) edited with `.fine` steps — BTN4/BTN5
  step the target, no menu — and its BTN1/BTN3 hint cells are custom-drawn
  (`ui_hints_set_gfx_cb`) with a rolling readout. No tape involvement — for that,
  see `recorder.c`.
- **`examples/chopper.c`** - tape-marks cutup composer. Reads the current tape's
  recorded-region **marks** (`tape_marks_count`/`tape_marks_get`), chops random
  slices of recorded audio into a RAM pool with `tape_read` (in `tick()`, off
  the audio thread), and a step sequencer retriggers them (pitch / reverse /
  decay) in `process()`. With no marks (a fresh tape) it shows a "record on the
  tape first" state. `ui_menu` edits BPM / density / slice length / reverse
  prob / pitch.

### The `params_t` + `ui_menu_*` settings pattern

Hold a `params_t` per adjustable setting in your model, seed it with static
values (`.val/.min/.max/.coarse/.type` - `ParamValExactType` for a number,
`ParamValSelectType` + `.option_name[]` for a cycle), build a menu in `init`,
and route input to it while it's open:

```c
// in the model: params_t bpm, density; ui_menu_t* menu;
static void mk_param(params_t* p, const char* n, float v, float lo, float hi, float step) {
    memset(p, 0, sizeof(*p));
    *(const char**)&p->name = n; *(ParamTypeUI_t*)&p->type = ParamValExactType;
    p->val = v; p->min = lo; p->max = hi; p->dflt = v; p->coarse = step; p->fine = step;
}
static void build(ui_menu_t* menu) {          // called by ui_menu_create
    my_model_t* m = ui_menu_ctx(menu);
    ui_menu_add(menu, UI_PARAM("BPM",     MenuNodeParam, &m->bpm));
    ui_menu_add(menu, UI_PARAM("density", MenuNodeParam, &m->density));
}
// init:   m->menu = ui_menu_create(m, build);   ui_statusbar_show(true);
// input:  if (m->menu && ui_menu_is_visible(m->menu)) { ui_menu_input(m->menu, btn, st); return; }
//         if (btn == 1 && st == KEY_STATE_PRESSED) ui_menu_show(m->menu);
// deinit: ui_menu_destroy(m->menu);
```

The menu edits your `params_t.val` live; your `process()`/`redraw` just read it.

### In-app hint bar

Prefer the system surfaces when they fit — they cost you no panel space and they look
like the rest of the device.

`ui_hints_set_labels()` gives you the real button-hint band, and each cell can carry
more than a word:

```c
ui_hints_set_labels(hints);                        // your five press/hold labels
ui_hints_set_icon(HintPress2, &asset_my_icon);     // an icon in a label cell
ui_hints_set_active(HintPress1, m->playing);       // light the cell while a state holds
ui_hints_set_gfx_cb(HintPress1, my_cell_draw, m);  // or draw the cell yourself
```

`ui_hints_set_gfx_cb(action, cb, ctx)` hands your callback the cell's rectangle so you
can draw a transport glyph, a meter, a tiny readout — `examples/grooves/` draws its
play/stop plate and its parameter readout this way. Draw only inside the cell you were
given. For text that should *change* rather than blink, `ui_roll_text_draw()` /
`ui_roll_text_snap()` roll it over mechanically, split-flap style (grooves rolls its
encoder-target name).

Standalone tapps additionally get `ui_statusbar_set_gfx_cb(cb, width, ctx)` — a
`width x TAPP_STATUSBAR_H` cell of the system statusbar. Source tapps have no statusbar,
so that call is inert for them (see [Source tapps](#source-tapps)); the hint band above is
their surface.

If you do want a self-contained strip anyway, build one from the primitives below:

```c
static void draw_hint(gfx_t* g, int x, const char* label, bool hold) {
    if (!label || !label[0]) return;
    gfx_set_color(g, 1);
    if (hold) gfx_draw_rect_fill_r(g, x, 226, 7, 5, 1); // hold marker
    else      gfx_draw_disc(g, x + 3, 228, 2, 0x0F);    // press marker
    gfx_draw_str(g, x + 11, 224, label);
}
// gfx_draw_hline(g, 0, 220, SCREEN_WIDTH); then draw_hint(g, 4, "play", false); ...
```

### *tape!* mounted tape access

Four read-access primitives let a TAPP sample / cut up the current tape (units =
**stereo frames**, 1 frame = L+R):

```c
tape_t*  tape_get(void);                                   // the tape singleton
uint32_t tape_marks_count(const tape_t*);                  // # recorded-region marks
bool     tape_marks_get(const tape_t*, uint32_t idx,
                        uint32_t* start_frame, uint32_t* end_frame);
uint32_t tape_read(const tape_t*, uint32_t pos_frame,
                   float* dst, uint32_t n_frames);          // stereo-interleaved; BLOCKS on SD
uint32_t tape_get_position(const tape_t*);                 // current playhead, frames
```

`tape_read` blocks on the SD read - call it from `tick()`/`init`, **never from
`process()`**. Also available: `tape_get_state`/`tape_is_idle`/`tape_size`,
`tape_rec_begin`/`commit`/`abort`, `tape_timeline_*` (see `tapp_api.h`). Tape
transport features are device-only; on the desktop the default tape has 0 marks,
so tape apps show their empty-tape UI (they don't fault).

## License

MIT - see [LICENSE](LICENSE). Copyright (c) 2026 Bedtime LLC.

Apps you build with this SDK are yours; the licence covers the SDK itself.

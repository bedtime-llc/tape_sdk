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
and a BTN4-hold exit. It declares no type, so it is a **utility** tapp and owns every button (see
[Who owns BTN4 / BTN5 hold](#who-owns-btn4--btn5-hold)). It creates a
**folder** rather than a bare `.c`, so you can drop in more
sources, an `inc/` folder or an `assets/` folder later without moving anything. It refuses to
overwrite an existing directory.

Then **deploy**: create an "apps" folder via disk mode on your *tape!*, copy `my_app.tapp` into it,
and it shows up in the **utils** list — a **BTN1 hold on the `setup` entry** of the main menu,
where every utility tapp lives. (A tapp that declares `AppTypeStandalone` is reached from the
other list instead: a **BTN1 hold on the `tape` entry**, the main-app switcher, hint **apps**. Which list a tapp appears in follows from its declared role — see
[Who owns BTN4 / BTN5 hold](#who-owns-btn4--btn5-hold).)

The rest of this section explains what the generated file contains.

1. **Write your app** (`my_app.c`):

```c
#include "tapp_api.h"

typedef struct {
    int counter;
} my_model_t;   // allocated ZEROED by the firmware before init(), freed after deinit()

static void my_redraw(gfx_t* gfx, const os_app_t* app) {
    my_model_t* m = os_app_get_model(app);
    // Every draw callback starts in GFX_COLOR_SET with the default font.
    gfx_draw_strf(gfx, 10, TAPP_CONTENT_Y + 20, "Counter: %d", m->counter);
}

// Return true on any frame you need repainted — that return is what schedules redraw().
// Returning false when nothing changed is the point: an idle screen then costs nothing.
// If your screen animates, return true while it is animating.
static bool my_tick(os_app_t* app) {
    my_model_t* m = os_app_get_model(app);
    int32_t diff = os_controls_encoder_get_delta();
    if (diff != 0) {
        m->counter += diff;
        return true;
    }
    return false;
}

static void my_input(os_app_t* app, uint8_t btn, KeyStateEnum state) {
    my_model_t* m = os_app_get_model(app);
    if (btn == BTN1 && state == KEY_STATE_PRESSED) {
        m->counter++;
    }
}

// The whole descriptor: name, model type, init, deinit (NULL: nothing to set up or free),
// then the callbacks. It also emits the tapp_get_descriptor() entry point.
TAPP_APP("My App", my_model_t, NULL, NULL,
         .redraw = my_redraw,
         .tick = my_tick,
         .on_input = my_input);
```

`init` is `bool init(os_app_t* app)` — return false to abort the launch — and `deinit` is
`void deinit(os_app_t* app)`; both are optional.

2. **Build**:

```bash
./tapp-build my_app.c
```

3. **Deploy**:

Create "apps" folder via disk mode on your *tape!* and copy `my_app.tapp` into it. To run it, open
the **utils** list — **hold BTN1 on the `setup` entry** of the main menu. A tapp that declares
`AppTypeStandalone` is in the other list: **hold BTN1 on the `tape` entry** (hint **apps**),
which is the main-app switcher and lists **tapedecks** first while a tapp runs, so it is also how you
get back to the decks. See [Who owns BTN4 / BTN5 hold](#who-owns-btn4--btn5-hold).

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
│   ├── tuner.c         # Chromatic tuner (example window utility tapp)
│   ├── chopper.c       # Tape-marks cutup composer
│   ├── recorder.c      # Tape recording via streaming DMA windows (a standalone tapp)
│   ├── grooves/        # Morphing 6-track generative groovebox (an audio source)
│   ├── slicer/         # grooves' sequencer playing slices of the current tape (an audio source)
│   ├── leds/           # LED patterns (multi-file) (example window utility tapp)
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
| BTN1 | 0 | Play button |
| BTN2 | 1 | Record button |
| BTN3 | 2 | Cue button |
| BTN4 | 3 | Menu button (hold: yours, unless you declare a type — then quick hints / main menu) |
| BTN5 | 4 | Wake button (hold: yours, unless you declare a type — then close hints) |
| BTN_ENCODER | 5 | Encoder rotation (state always RELEASED; read `os_controls_encoder_get_delta()`) |

`hint_press(btn)` / `hint_hold(btn)` name a button's two hint cells.

### Button States

```c
KEY_STATE_RELEASED  // Button released
KEY_STATE_PRESSED   // Button just pressed
KEY_STATE_HOLD      // Button held down
```

### Using on_input (Recommended)

```c
static void my_input(os_app_t* app, uint8_t btn, KeyStateEnum state) {
    if (btn == BTN1 && state == KEY_STATE_PRESSED) {
        // BTN1 pressed
    }
}

TAPP_APP("My App", my_model_t, NULL, NULL, .on_input = my_input /* , ... */);
```

Whether BTN4/BTN5 *hold* reaches `on_input` depends on the role you declare (next section).

### Who owns BTN4 / BTN5 hold

**It depends on `TAPP_DECLARE_TYPE` — and the default is "you do".**

**Utility tapps — declare nothing.** Games and tools. The firmware imposes no gesture and
reserves no hint cell: all ten cells are yours, both holds reach `on_input`, and **you
provide the way out** by calling `os_app_exit()`. A BTN4 hold is the convention the bundled
examples use, but nothing forces it — bind those holds to your own controls if your app
needs them (a game using them to move) and offer an exit from a menu row instead.

```c
static void my_input(os_app_t* app, uint8_t btn, KeyStateEnum state) {
    if (btn == BTN4 && state == KEY_STATE_HOLD) os_app_exit();
}
```

**Window-mode utility tapps are the one exception.** A utility that declares a rect paints
*over* the main menu instead of taking the screen, and the menu behind it cannot take input
while it is up — so there **BTN4 and BTN5 hold close the window**, and your `on_input` never
sees those two holds. Their presses are still yours. See [Window mode](#window-mode).

**Declared tapps hand those two holds to the firmware.** `TAPP_DECLARE_TYPE(AppTypeStandalone)`
and `TAPP_DECLARE_TYPE(AppTypeSrc)` both mean "I am part of the tape workflow", and both give
up the BTN4/BTN5 holds in exchange for the system gestures below — your `on_input` never sees
them (BTN4/BTN5 *presses* still reach you). The BTN4/BTN5 **hold** hint cells become the
system's too, so whatever you state for either of those two holds in `ui_hints_set()` is
ignored. A **source** tapp gets the back and exit icons drawn there, because its hints stay up
all session and those two gestures are how you leave. A **standalone** tapp gets the menu and
close icons when the firmware revealed the band for you, and a single spanning **menu** pill
when your own hints are up (both holds open the menu then). Their **press** cells are yours
(see [In-app hint bar](#in-app-hint-bar)).

**Standalone tapps** (`TAPP_DECLARE_TYPE(AppTypeStandalone)`) follow the tapedecks
quick-hints idiom. At launch
the statusbar is on and the hints are hidden; the firmware has already installed
default hints with the system cells before `init()`, so `ui_hints_set()` just fills
in your cells. Call `ui_hints_show(true)` in `init()` if you want the hints up
permanently.

- Hints hidden, and you have hints of your own: BTN4 or BTN5 hold shows them, and they
  stay up.
- Hints hidden, and you set **no** hints at all: that hold goes **straight to the main
  menu**. Showing hints that are nothing but the firmware's own two icons would be
  ceremony, so the firmware skips it.
- Hints up: **BTN4 hold opens the main menu** over your still-running tapp — audio keeps
  going, your `tick()` pauses while the menu has focus, and `on_pause`/`on_resume` are
  called around it. **BTN5 hold puts the hints away** if the firmware revealed them; a hold
  brings them back. On a band you keep up with `ui_hints_show(true)` there is nothing to put
  away, so BTN5 hold opens the main menu too.
- A short press of BTN4 or BTN5 while system-revealed hints are up reaches you **if you
  have drawn on that cell** — a label, an icon, or both. Leave the cell blank and the
  same press puts the hints away instead, and is not delivered to you. So a tapp that
  wants prev/next on the arrows simply labels those two cells and gets the presses;
  one that does not gets a free dismiss gesture.
- `ui_hints_set()` and `ui_hints_set_gfx_cb()` reach every cell **except** those two hold
  cells, which stay the firmware's.
- Every session starts from clean hints: nothing carries over from the tapp that ran
  before you.
- Leaving: from the main menu, hold BTN1 on **tape** to open the main-app switcher; it lists
  **tapedecks** first while a tapp runs, and picking it (or another tapp) swaps the main
  app. `os_app_exit()` still works for a programmatic exit — e.g. from a row of your own
  menu.

**Source tapps** (`TAPP_DECLARE_TYPE(AppTypeSrc)`): hints always on, no statusbar; BTN4
hold = back to the menu, BTN5 hold = out to the main screen.
There **both** rows of BTN4/BTN5 belong to the firmware. See [Source tapps](#source-tapps).

While your own `ui_menu_t` is open (`ui_menu_show`) it is modal: BTN4/BTN5 holds go to
that menu (back / close), as before.

The bundled `examples/` are written to this model: they set labels but do not call
`ui_hints_show(true)`, so their hints start hidden and a hold shows them, and none of them
carries an in-app exit gesture any more.

### Reading Encoder

```c
int32_t diff = os_controls_encoder_get_delta();
// diff > 0: clockwise
// diff < 0: counter-clockwise
```

## API Reference

### Graphics Functions

Coordinates are `gfx_coord_t` (signed 16-bit): a shape that starts off-screen is clipped, not
dropped. Every draw callback starts with `GFX_COLOR_SET`, transparent bitmaps, the default font
and a clip window it cannot widen, and leaves nothing behind for the next one.

```c
// Colour: GFX_COLOR_CLEAR (0), GFX_COLOR_SET (1), GFX_COLOR_XOR (2)
void gfx_set_color(gfx_t* gfx, uint8_t color);

// Primitives
void gfx_draw_pixel(gfx_t* gfx, gfx_coord_t x, gfx_coord_t y);
void gfx_draw_line(gfx_t* gfx, gfx_coord_t x1, gfx_coord_t y1, gfx_coord_t x2, gfx_coord_t y2);
void gfx_draw_hline(gfx_t* gfx, gfx_coord_t x, gfx_coord_t y, gfx_coord_t w);
void gfx_draw_vline(gfx_t* gfx, gfx_coord_t x, gfx_coord_t y, gfx_coord_t h);

// Rectangles
void gfx_draw_rect(gfx_t* gfx, gfx_coord_t x, gfx_coord_t y, gfx_coord_t w, gfx_coord_t h);
void gfx_draw_rect_fill(gfx_t* gfx, gfx_coord_t x, gfx_coord_t y, gfx_coord_t w, gfx_coord_t h);

// Circles
void gfx_draw_circle(gfx_t* gfx, gfx_coord_t x0, gfx_coord_t y0, uint16_t r, uint8_t opt);
void gfx_draw_disc(gfx_t* gfx, gfx_coord_t x0, gfx_coord_t y0, uint16_t r, uint8_t opt);

// Text (returns the width drawn)
uint16_t gfx_draw_str(gfx_t* gfx, gfx_coord_t x, gfx_coord_t y, const char* str);
uint16_t gfx_draw_strf(gfx_t* gfx, gfx_coord_t x, gfx_coord_t y, const char* fmt, ...);

// Images: 1 bits in the current colour, 0 bits left alone (gfx_set_bitmap_mode for opaque)
void ui_draw_img(gfx_t* gfx, gfx_coord_t x, gfx_coord_t y, const gfx_img_t* img);
void gfx_draw_xbm(gfx_t* gfx, gfx_coord_t x, gfx_coord_t y, gfx_coord_t w, gfx_coord_t h, const uint8_t* bitmap);
void gfx_set_bitmap_mode(gfx_t* gfx, uint8_t is_transparent);

// Dithered fill (shade 0 = nearly black ... 7 = nearly white)
void gfx_fill_rect_dithered(gfx_t* gfx, gfx_coord_t x, gfx_coord_t y, gfx_coord_t w, gfx_coord_t h, uint8_t shade);
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

Nothing saves for you on close: the optional `memory_if` hooks on your descriptor run at launch
(`load`, after `init()`) and when the device flushes settings (`save`), never at exit — so load in
`init()` and save in `deinit()`. Never call storage from
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
int16_t lutsin_q15(uint16_t phase);  // sin(2*pi * phase/65536) in Q15; the phase wraps at a turn
int16_t lutcos_q15(uint16_t phase);  // likewise cos — sinf()/cosf() are there too
```

### Screen Constants

```c
#define SCREEN_WIDTH  400
#define SCREEN_HEIGHT 240
```

## Audio Engine

Real-time audio processing for TAPPs.

### Architecture

- **Sample rate**: 48kHz (`TAPP_SAMPLE_RATE`)
- **Buffers**: interleaved stereo (L, R, L, R, ...)
- **Block size**: a build-time constant — take it from `mixer_get_fs(mix)` on every
  block and never hardcode a number. `fs` is the count of *samples* in the buffer
  (L+R combined), so a stereo loop steps `i += 2` and runs `fs / 2` frames
  (`mixer_frames(mix)`); a block is at most `TAPP_BLOCK_FRAMES_MAX` frames.

### Setting Up Audio

```c
// ctx is your model (os_app_get_model(app)) — every engine callback gets it.
static void my_process(void* ctx, mixer_t* mix) {
    my_model_t* m = ctx;
    float* out = mixer_get_out(mix);
    uint32_t fs = mixer_get_fs(mix);

    for (uint32_t i = 0; i < fs; i += 2) {
        float sample = generate_sample(m);
        out[i] = sample;      // Left
        out[i + 1] = sample;  // Right
    }
}

static const engine_callbacks_t my_engine = {
    .process = my_process,
};

TAPP_APP("Synth", my_model_t, my_init, NULL,
         .redraw = my_redraw,
         .engine_cb = &my_engine);   // that's it
```

Declaring `.engine_cb` is the whole registration: the firmware installs your callbacks
right after `init()` returns, hands each of them your model as `ctx`, and stops and
unhooks them before your `deinit()` runs. `engine_set_active()` works from anywhere,
`init()` included — a call made there is kept and delivered to your `active` callback at
the install.

### Mixer Buffers

```c
float* mixer_get_in(mixer_t* m);   // Input buffer
float* mixer_get_out(mixer_t* m);  // Output buffer (write here!)
float* mixer_get_fx(mixer_t* m);   // FX send buffer
uint32_t mixer_get_fs(mixer_t* m); // samples per block (L+R counted separately)
uint32_t mixer_frames(mixer_t* m); // frames per block = mixer_get_fs() / 2
```

## Window mode

A **utility** tapp can paint into a rect **on top of the main menu** instead of taking the
screen — the browser-window look. Ask for it **first** in `init()`, before any `ui_hints_*` call
(the firmware sizes your hint columns and captures the menu's cells the moment you ask):

```c
static bool my_init(os_app_t* app) {
    os_app_set_window(100, 60, 200, 170);   // the device's browser geometry
    return true;
}
```

That is the whole opt-in. The firmware validates the rect (y below 48 is the hint band, too small
or off-screen is refused) and that you are a utility tapp, logs a refusal, and a refused tapp
simply runs fullscreen — which is also what a tapp that never asks gets.

What you get:

- The main menu **stays visible around you** and keeps painting. Your launcher list closes,
  the menu it sat on does not.
- `redraw()` is **clipped to the rect**. Coordinates are still screen coordinates — there is
  no per-window origin — but anything outside is cut off, so you cannot smear the menu.
- BTN1–BTN3 presses reach `on_input()` as usual, and so do BTN4/BTN5 **presses**.
- A `ui_notify` popup you raise still lands correctly on top of you and takes input back.

What you give up:

- **BTN4 and BTN5 HOLD close the window.** That is the trade: the menu behind you is deaf
  while you are up, so the firmware keeps a guaranteed way out rather than trusting every
  tapp to offer one. A fullscreen utility tapp keeps both holds.
- **The statusbar is the menu's** — do not call `ui_statusbar_show()`. The **hint band is the
  menu's too, until you claim it, and the claim is PER CELL**: say nothing and the menu keeps
  its whole band, dimmed (its gestures cannot reach it past your modal window), with one
  firmware `[close]` pill on the hold row. Call `ui_hints_set()` and only the cells you
  give a label become yours; every cell left empty keeps the menu's own label, still dimmed. So naming one button costs you nothing on the other four — name what
  your buttons do and leave the rest. The `[close]` pill stays the firmware's either way, and
  your cells are informational (no press flash, no cell callbacks — you get the raw buttons in
  `on_input()`).
- You still occupy the image arena's main slot, so opening one while a **standalone** tapp
  is running closes that tapp and reloads it when you exit, exactly as a fullscreen utility
  does. The small window understates what happens underneath; keep that in mind.

Two things:

- **Read the encoder from `on_input()`, not `tick()`.** `os_app_input()` drains the accumulator with
  `os_controls_encoder_get_delta()` right after handing the event to the modal widget. A fullscreen
  tapp never reaches that branch, so polling from `tick()` works there — but a window is always
  modal, so `tick()` always reads 0. Handle `BTN_ENCODER` in `on_input()` instead.
- **Your own `ui_menu_t` is not clipped to the rect.** The firmware clips your `redraw()`; a widget
  you attach is a peer of the window, not a child. Size and place it by hand (`ui_menu_set_pos()`
  and `ui_menu_set_width()`, in either order). A menu you show from `init()` attaches after the
  window and therefore stacks above it, which is what you want: being modal, it takes the input.

`examples/window_app.c` is a complete one and the example that CLAIMS the band, `examples/tuner.c`
is a near-display that claims a SINGLE cell (BTN1, line/mics) and leaves the other four showing the
menu's labels — the claim is per-cell — and `examples/leds/` is a window whose whole UI is its own
`ui_menu_t` and claims nothing, so it runs entirely over the menu's own dimmed hints.

## Chrome by role

The firmware derives the statusbar, hint band and tape timeline from your **role**. You never
inherit what ran before you, and you never have to put anything back on the way out.

| role                 | statusbar | hint band           | timeline | BTN4/BTN5 holds |
|----------------------|-----------|---------------------|----------|-----------------|
| utility              | on        | hidden              | off      | yours           |
| utility, window mode | menu's    | menu's, unless you claim it | menu's | close the window |
| standalone           | on        | hidden until a hold | off      | firmware's      |
| source               | off       | shown, unlatched    | off      | firmware's      |

Those are **defaults, not prohibitions**. `ui_hints_show()`, `ui_statusbar_show()` and
`tape_timeline_show()` each override one — and **your call is remembered**: the firmware
records it against your image and replays it whenever it re-derives the screen, so one call
in `init()` holds for the whole session. No `on_resume()` nursing, and a BTN5 hold will not
take down a band you asked for.

Recommendations:

- **Draw from `TAPP_CONTENT_Y` (48) down.** The band lives above it whenever it is up,
  including when a hold reveals it over you.
- **Say what you want once, in `init()`.** The exception is a **source** tapp, whose `init()`
  runs off-screen — say it from your first `tick()` or `on_resume()` instead, or you will
  tear down the chrome of whatever is actually on screen.
- **Restate your band on every state change**, not once: one `ui_hints_set()` call states
  every cell, and the lit/lock animations and a switch's knob carry across calls, so the
  rebuild-per-press idiom is cheap.
- **Do not draw your own hint bar.** The band is a real widget with a slide animation and
  press feedback; a hand-drawn strip gets painted over the moment anything reveals it.
- **The timeline is opt-in and per-image.** `tape_timeline_show(true)` survives a menu
  round-trip; a menu **submenu** still hides it, because a submenu takes the screen rather
  than overlaying it.

## Source tapps

A standalone tapp takes the tape engine's **main slot** while it runs, displacing the tapedecks.

Add one line at file scope and your tapp becomes a **source** instead: it takes the engine's
**input slot**, loaded in place of fx, radio or slicer. Whatever holds the main slot keeps running
underneath you — the tapedecks, or a standalone tapp — your audio lands on the record path and
can be recorded straight to tape, process WET input and tapehead sends.

The two are different slots, so **one of each can run at the same time**: a standalone tapp
and a source tapp are both resident and both audible. Two of the same kind cannot; loading a
second of either replaces the first. A standalone can also feed the source: accumulate into
`mixer_get_send()` and the source reads it as `mixer_get_fx()` one block later, exactly like a
tapedeck's send knob (see [Source tapps](#source-tapps) for what the source does with it).

One thing to know when both run: the audio path switches to the source chain, which does not
run the firmware FX graph, external audio sync or the USB mic fold-in. That is what a radio
session already does. The source tapp is the processor in that arrangement.

```c
#include "tapp_api.h"

TAPP_DECLARE_TYPE(AppTypeSrc);   // <- the entire opt-in
```

The engine-replacing tapp is `TAPP_DECLARE_TYPE(AppTypeStandalone)` — also a declaration. No
declaration at all means **utility**: a game or tool that owns every button (see
[Who owns BTN4 / BTN5 hold](#who-owns-btn4--btn5-hold)).

### Writing the audio

```c
static void my_process(void* ctx, mixer_t* mix) {
    my_model_t* m = ctx;                     // your model

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

static my_model_t* g_model;                     // = os_app_get_model(app) in init()

static void my_process(engine_t* engine, mixer_t* mix) {
    my_model_t* m = g_model;
    if (!m) return;
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

A source tapp comes up in a house style — **no statusbar, hints up** — and you can rely
on getting that without asking. Both halves of it are *defaults*, though, not prohibitions:

- **`ui_statusbar_show(true)` brings the bar back**, which is what you want on anything
  meant to be left running: the battery stays visible, and in idle mode the bar paints only
  its own item cells, so your picture shows through around them.
- **`ui_hints_show(false)` takes the band away.** The band owns the top 60 rows, and a
  fullscreen soundscape or visualiser — where the picture *is* the app — should not have to
  give them up. Hiding it changes no gesture (BTN4/BTN5 hold are still the system's), but it
  does remove the only place those gestures are ever named, so do it only when the app reads
  without labels.
- Either way, **refocus re-applies the house style**, so re-assert whatever you actually want
  from `on_resume()` as well as from `init()`.
- **BTN4 / BTN5 *hold* belong to the system**: back to the menu, and out to the main
  screen — both background you. Here **both rows** of the BTN4/BTN5 hint cells are the
  system's (prev/next arrows on press, back/exit icons on hold; your labels for those two
  buttons are ignored in both rows). Presses still reach you, so a prev/next-style action
  fits there; put everything else on BTN1–BTN3 and the encoder. The same two holds are
  system-owned on standalone tapps as well — though there only the *hold* row is the
  firmware's, see
  [Who owns BTN4 / BTN5 hold](#who-owns-btn4--btn5-hold).

### How someone runs your tapp

From the main menu, on its first cell — the source slot: **hold BTN1 there** (hint
**source**). The picker that opens lists *fx* (the plain input path), *radio*, *slicer*, and
every input tapp on the card — pick yours and it loads. Source tapps deliberately do **not**
appear in the **apps** or **utils** lists;
the source picker is their home, and it is also where the user unloads them. Your `assets/app_icon.png` shows there
and in the menu slot (see [App icon](#app-icon) — 76x76, animation allowed).

### MIDI

Call `midi_bus(true)` once in `init()` and channel-voice MIDI (notes, CC, pitch
bend, aftertouch, program change) is yours instead of the tapedecks' while you are
loaded; `midi_bus(false)` reads without taking the notes. Clock, sync pulses and
transport reach every reader either way. While a source tapp is loaded, the decks
ignore Start / Continue / Stop and leave the transport to it. Call nothing and the
decks keep all of it. See `midi_bus()` in `tapp_api.h` for the readers.

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
    my_model_t* m = g_model;                    // never engine_get_ctx() per block
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

Note there is no `.tick` here, and `m->playing` is written by the **audio callback** — not by
input. That is safe only because nothing else starts or stops the sound: the moment some
other thread can change what the screen shows, you need a tick returning `true` while it
changes, or the display will sit on a stale value until the next button press.

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

**Draw it either way round.** tapp-build decides which side of the picture is ink by
looking at the four corners, not at the file format: dark corners mean light-on-dark
artwork and it inverts, light or transparent corners mean the image is already
ink-on-paper and it does not. So a white glyph on black and a black glyph on white both
come out right, and a transparent background counts as white. Colour and greyscale
images are dithered to 1 bit on the way.

### Fonts

Drop a `.ttf`, `.otf` or `.bdf` in `assets/` and it becomes a font you can pass straight to
`gfx_set_font()`. **The point size comes from the filename** — each trailing `_<digits>` group is
one size, the same convention the firmware's own fonts use:

```
assets/
├── mono_12.ttf       → asset_mono_12
└── iso_16_20.otf     → asset_iso_16  and  asset_iso_20   (two sizes, one file)
```

```c
gfx_set_font(gfx, asset_mono_12);
gfx_draw_str(gfx, 10, 80, "hello");
```

A `.ttf` with no size in its name is an error, not a guess — rename it. A `.bdf` is already
rasterized, so it needs no size.

Fonts are built for ASCII (32-127) and land in read-only memory: roughly 1.5-2.5 KB of your tapp's
image each, and nothing at all from the 96 KB RAM arena. Set `TAPP_FONT_RANGE` to widen the range
(`TAPP_FONT_RANGE=32-255 ./tapp-build my_app` for Latin-1, at about double the size).

The first build that has a font in `assets/` compiles the font converter and caches it under
`~/.cache/tapp/`, which takes a few seconds once. It needs no tools you do not already have — the
converter ships inside `tapp-build` itself and is built with the same clang that builds your tapp.
The browser builder runs the very same converter compiled to wasm, so a font is identical in the
preview and on the device.

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
4. **Tick function** - Per-frame update. **Its return value is what schedules `redraw()`** —
   return `true` on any frame you need painted. Required if anything on your screen moves on
   its own; an app with no tick only repaints on input, focus change, or an animating
   menu/popup.
5. **App data** - Links model and functions (with `model_size`!)
6. **App descriptor** - Main structure
7. **Entry point** - `tapp_get_descriptor()`

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

### Frozen or Stale Screen
Your animation runs for one frame and stops, or the display only updates when you press a
button. **`tick()` is returning false.** The firmware paints `redraw()` only on frames your
tick asked for, plus the ones it grants you anyway (input, focus change, an animating menu or
popup) — so return `true` while anything is moving.

Watch for the self-reinforcing version: `ui_get_frame()` counts *paints*, so once you stop
asking for frames it stops counting and can never restart. **Drive animation from
`os_tick_get()`** — a real millisecond clock that runs whether or not you paint —
e.g. `ui_draw_anim(gfx, x, y, (uint16_t)(os_tick_get() / 80), &anim)`.

`ui_roll_text_draw()` has the same shape: it advances the roll only as it paints. Use
`ui_roll_t` instead (`ui_roll_create` / `ui_roll_set` / `ui_roll_draw`) — it is a widget, so
the firmware ticks it and it asks for its own repaints; your `tick()` can return false
throughout and the label still lands.

Testing with a menu open will hide all of this: an animating menu grants frames, so the app
behind it looks alive while it is not. Close the menu and let go of the buttons.

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
- `examples/window_app.c` - **[window mode](#window-mode)**: paints over the main menu
  instead of taking the screen. Shows the `.type = AppWidgetType` + `win_*` declaration,
  the clip contract, and which buttons you keep.
- `examples/tuner.c` - chromatic tuner, and the minimal window: NSDF pitch detection, a big
  block-font note, a cents needle — and **one control**, BTN1 switching the input between line
  and mics, which is also the reference for a **per-cell** band claim. No menu; the firmware's
  BTN4/BTN5 hold is still the way out.
- `examples/leds/` - a window whose entire UI is its own `ui_menu_t` (see the two gotchas
  above), browsing and playing the OS LED patterns.
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
  shows a big block-font note + a cents needle. A4 is a fixed `#define`; the only
  control is BTN1, which calls `os_audio_switch_input()` and re-states its own
  hint cell — label and `gfx_icon()` glyph both following the source.
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

- **`examples/slicer/`** - grooves' sequencer and UI as a **source tapp**, every voice playing
  **slices of the current tape** through grooves' envelopes and filters — there is no synth,
  so a track with no slices is silent. Windows from separate places on the tape are cut at
  their attacks; every drum slice is dealt to the drum tracks by brightness (zero-crossing
  rate, darkest to the kick); track 6, the bass, plays the slices of its own long window on
  the scale degrees and track 7, the pad, the slices of another — long tuned notes, slow in
  and out, ducked by the pump (pitched slices tuned by NSDF to natural minor on the bass's own
  root). There is no visible grid: each row is a hi-res lane (32 substeps a step) that draws
  every hit where it lands, as the slice it plays, tall as it is loud — offsets, stutters and
  rolls (which may change sample or reverse), kick/snare pickups — each at its true length:
  solid while its gate is open, the tail the gate cut off ghosted up to the next hit, a rest
  still echoing (dithered) the slice it would play, dotted lines between the tracks, a 4 px XOR
  playhead sweeping each lane. **rand** swaps the new patterns in on the very next 16th. A note can HOLD over several steps (drawn as one long hit): the pad to its next note, the bass 1-4
  steps, a drum now and then — and a note reaching exactly to the next one may TIE into it
  legato (no re-attack; the bass and pad slide to the new note). Any hit may also catch a
  GLITCH (its slice sample-and-held). Two params (BTN4/BTN5 pick): **grid** walks a figure-8
  over a grid of generated patterns — the PATTERN only (which steps play, how hard and long,
  the ties, a density rolled per pattern); **morph** is the SOUND only — a REAL 3D spectral morph, Kyma Morph3D-style:
  each cell's cube holds 4 of its own slices and 4 of a partner track's (the kick's bass, the hat's
  pad, the snare's metal — far enough apart to really morph) and the knob walks a closed 3D path through
  it (dwelling on the corners — plain slices — with morph zones between); every slice is analysed
  at load into SMS frames (1024-point analysis every 512 samples: its 16 strongest partials + 16 noise bands; partials correspond as Kyma's morph tracks do — by harmonic number against each frame's own fundamental when every corner is pitched, by frequency band otherwise — TRACKED at resynthesis and eased between frames, the cube gliding ~250 ms behind the knob) and
  a morphing voice is resynthesised from its 8 corners — partial i's frequency and level
  interpolated across them, so partials GLIDE from one sound into the next, over interpolated
  noise — each pitched corner on its OWN nearest scale note (so the morph glides pitch as well as timbre, in key at every corner; MORPH_PITCH 0 puts them all on the hit's note), live while it rings, the raw attack kept and the plain
  slice blended back in near its corner. Chops are stored 16-bit, so windows run 1.5–2.9 s
  — and the sound (each voice's character; which drum
  cells sound their own consonant interval — revealed cell by cell, never a detune; which
  play rate-fitted to last exactly their note instead of in tune; where a third of the cells
  start and end inside their slice; which play reversed; which bass/pad/metal notes glide in
  from the last one; the sidechain pump — all rolled per pattern). Every hit is gated at its
  note's end; the bass and pad cut long phrases (1.7 and 2 s windows); morph has its own
  automation lane (BTN3 hold).
  **rand** (BTN2) rolls new patterns and a new tempo (80–140 BPM). Slices stream in from
  `tick()` (chopper's loader) into an idle second set that flips in when complete, so a
  reload never interrupts playback. The kit follows BACKBEAT rules (snare on 2 and 4 with
  ghosts, kicks syncopated but never on the backbeat, hats accented on the "&"); steps carry
  sparse ratchets, sub-steps and skip conditions, every drum cell has its own pitch (each
  track's spread rolled per pattern), every bar re-rolls a few of
  them per track, and every 4th bar ends in a snare/clap fill; the grid breathes through chopper's
  per-beat feel template plus per-track push/pull, and velocity is squared so ghosts sit
  ~13 dB under accents. The decks' FX send (`mixer_get_fx()`) is mixed into the record
  path with the live input and pumped by the drum sidechain, as in grooves. Controls: BTN1
  play / hold + encoder = tempo (the radio's bpm cell); BTN2 tap "rand" re-rolls the
  patterns, hold reloads slices; BTN3 hold records a motion lane for the selected param.

### The `params_t` + `ui_menu_*` settings pattern

Hold a `params_t` per adjustable setting in your model, seed it with `PARAM_RANGE()`
(`ParamValExactType` for a number; for a cycle set `ParamValSelectType` and
`.option_name[]`), build a menu in `init`, and show it from a button. A shown menu is modal:
it takes every button until it closes, so there is no input to route.

```c
// in the model: params_t bpm, density; ui_menu_t* menu;
static void build(ui_menu_t* menu) {          // runs at create and on every ui_menu_show
    my_model_t* m = ui_menu_ctx(menu);
    ui_menu_add(menu, UI_PARAM("BPM",     MenuNodeParam, &m->bpm));
    ui_menu_add(menu, UI_PARAM("density", MenuNodeParam, &m->density));
}
// init:   m->bpm = (params_t)PARAM_RANGE("BPM", 120.f, 60.f, 200.f, 1.f);
//         m->density = (params_t)PARAM_RANGE("density", 0.5f, 0.f, 1.f, 0.05f);
//         m->menu = ui_menu_create(m, build);
// input:  if (btn == BTN2 && st == KEY_STATE_PRESSED) ui_menu_show(m->menu);
// deinit: ui_menu_destroy(m->menu);
```

The menu edits your `params_t.val` live; your `process()`/`redraw` just read it.

### In-app hint bar

Prefer the system surfaces when they fit — they cost you no panel space and they look
like the rest of the device.

`ui_hints_set()` gives you the real button hints — the whole band in one call, one
`tapp_hint_cell_t` per press and hold cell — and each cell can carry more than a word:

```c
const tapp_hint_cell_t cells[HintsActionsTotal] = {
    [HintPress1] = {"play", gfx_icon(GfxIconPlay), NULL, m->playing ? HINT_ACTIVE : 0}, // lit
    [HintHold1]  = {"loop"},
    [HintPress2] = {"rec", &asset_my_icon},                                             // icon
    [HintPress3] = {NULL, gfx_icon(GfxIconJack), gfx_icon(GfxIconMic), m->mic ? HINT_ON : 0},
};
ui_hints_set(cells);                               // call again whenever your state changes
ui_hints_set_gfx_cb(HintHold3, my_cell_draw, m);   // or draw a cell yourself
```

Labels are referenced, not copied: pass literals or text that lives in your model.

**Which cells are yours.** BTN1–BTN3 get the wide (100 px) columns; BTN4 and BTN5 are the
narrow (50 px) ones — keep those two labels to about five characters. A **utility** tapp
owns all ten cells. If you **declare** a type, the BTN4/BTN5 **hold** row becomes the
firmware's: whatever you state for those two cells is ignored (see
[Who owns BTN4 / BTN5 hold](#who-owns-btn4--btn5-hold)). Either way you
have three wide label columns and two narrow ones, not five wide ones.

**The row grid.** Every band on the device uses the same one, so nothing moves or resizes
when one replaces another:

| columns   | width  |
|-----------|--------|
| BTN1–BTN3 | 100 px |
| BTN4/BTN5 | 50 px  |

`3 + 3*100 + 2*50 = 403` is the row's nominal end — each cell draws its body 5 px narrower
than its column, so the last lit pixel is 397 and the trailing margin mirrors the 3 px lead.
The **BTN4/BTN5 pair is pinned to the right edge**, so those two stay under their buttons
however wide the columns before them are; columns that fall short just leave a gap before
the pair. 50 px is a floor rather than a preference — it is the narrowest width the switch
pill renders correctly at.

**Switches and volume cells.** A cell with an `icon_on` is the device's own two-position
toggle pill (the REC band's line/mic, dry/wet): `icon` is the OFF glyph, `HINT_ON` puts the
knob on the right, and `HINT_WIDE` asks for the roomier 65 px version, which deliberately
leaves the grid. `ui_hints_set_volume()` installs the
hold-PLAY volume cells — 60 px each, growing to 120 px while selected. Four of them plus
the pinned pair is exactly the row, and installing two or three is a first-class layout:
the nav cells stay under BTN4/BTN5 either way.

**Param cells.** `HINT_PARAM` on a BTN1–BTN3 *press* cell makes it one tall pill over both
rows: the press content (a param's name) on top, the *hold* cell's content (its value — a
label or a painter's bar) drawn on the same plate underneath. Clear the flag and the pill
shrinks back to one row and the hold cell returns as its own pill; set it and it grows — both
eased, never snapped. A lit (`HINT_ACTIVE`) param pill is lit as a whole, so a hold painter
drawn on it should use `GFX_COLOR_CLEAR` then. `examples/grooves/`, `slicer/` and
`chopper.c` use it for their BTN3 name-over-value cell.

**An icon with no label is centred.** A cell with an icon and no label centres the glyph,
which is what the two narrow columns want: a word plus a glyph crowd each other in 50 px, and the arrows read on their own.
Give the cell a label as well and the icon moves left to make room for it.

`ui_hints_set_gfx_cb(action, cb, ctx)` hands your callback the cell's rectangle so you
can draw a transport glyph, a meter, a tiny readout — `examples/grooves/` draws its
play/stop plate and its parameter readout this way. Draw only inside the cell you were
given. For text that should *change* rather than blink, `ui_roll_text_draw()` /
`ui_roll_text_snap()` roll it over mechanically, split-flap style (grooves rolls its
encoder-target name). The roll takes several frames, so keep `tick()` returning `true`
until it lands — stash `ui_roll_text_draw()`'s return value in your model and hand it back
from the tick, or the label freezes part-flipped.

Standalone tapps additionally get `ui_statusbar_set_gfx_cb(cb, width, ctx)` — a
`width x TAPP_STATUSBAR_H` cell of the system statusbar. Source tapps have no statusbar,
so that call is inert for them (see [Source tapps](#source-tapps)); the hints above are
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
```

`tape_read` blocks on the SD read - call it from `tick()`/`init`, **never from
`process()`**. It is the right tool for bulk offline work (loading slices into RAM,
rendering a bounce), and the wrong one for a transport.

For a transport, use the **streaming windows** — the mechanism the built-in tapedecks
run on, and the only tape I/O that is safe from `process()`:

```c
tape_buffer_t* wb = tape_buffer_init(tape_get());   // a pair of 4096-frame DMA windows
tape_buffer_update(wb, pos, /* write */ false);      // hits, or posts one SD command
float* w = tape_buffer_get_ptr(wb);                  // 4096 stereo frames, read/overdub
tape_buffer_deinit(wb);                              // in deinit()
```

Nothing blocks; the DMA lands straight in the window. Recording needs no ring and no
drain — pass `write=true`, overdub the window in place, and it is written back when it
is evicted (`tape_buffer_flush()` at the end of the take catches what is still
resident). Prefetch **one window ahead**: it is a two-slot pipeline, so you request the
window *after* the one you are about to play, and `tape_buffer_get_ptr()` hands back
the one you requested last time. The pool is small and shared, so check for `NULL`.
`examples/recorder.c` is the reference; see the "Streaming windows" block in
`tapp_api.h` for the two constraints that shape the design.

Also available: `tape_get_state`/`tape_size`,
`tape_rec_begin`/`commit`/`abort`, `tape_timeline_*` (see `tapp_api.h`). Tape
transport features are device-only; on the desktop the default tape has 0 marks,
so tape apps show their empty-tape UI (they don't fault).

## License

MIT - see [LICENSE](LICENSE). Copyright (c) 2026 Bedtime LLC.

Apps you build with this SDK are yours; the licence covers the SDK itself.

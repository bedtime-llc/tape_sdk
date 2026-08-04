# Minimal libc for TAPP builds

These headers replace the system/newlib C library headers when building a `.tapp` with clang
(`tapp-build` passes `-nostdlibinc -isystem <sdk>/libc`). They are **declarations only** - there is no
libc to link against. A `.tapp` is a relocatable object whose undefined symbols are resolved by the
firmware at load time, from the table in `lib/tapp/api/tapp_api_table.c`.

## The rule

> Declare only what the firmware actually exports.

That is the whole design. Because `tapp-build` also passes `-Werror=implicit-function-declaration`,
calling something that isn't declared here is a **compile error naming the exact line**, instead of the
app building fine and then failing to load on the device with a missing-import error.

So the omissions are deliberate, not oversights:

| Header | Notably absent | Use instead |
|---|---|---|
| `stdio.h` | `printf`, `sprintf`, `vsnprintf`, `puts`, `fopen` | `snprintf`, and the `storage_*` API in `tapp_api.h` |
| `string.h` | `strcat`, `strdup`, `strtok` | `strncpy` / `strncmp` / `strstr` |
| `stdlib.h` | `calloc`, `exit`, `atof`, `bsearch` | `os_malloc` / `os_free` from `tapp_api.h` |
| `math.h` | every **double** entry point (`sin`, `cos`, `pow`, `exp`, `log`, `sqrt`) | the `f` variants |
| `math.h` | `fmodf`, `atanf`, `atan2f`, `asinf`, `acosf`, `tanhf` | no firmware export and no FPv5 instruction |

## No doubles

A `.tapp` must contain **zero 64-bit floating point**. The FPU is FPv5-D16 so doubles do work, but at
roughly half the throughput of `float` and with double the register pressure - unacceptable on the audio
path. Three things enforce it:

1. `-Werror=double-promotion` - a bare `1.1` in a float expression is a compile error. Write `1.1f`.
2. Only `float` entry points are declared here, so `sin(x)` cannot compile.
3. `tools/verify-tapp.sh` disassembles the finished `.tapp` and **rejects any `.f64` instruction**.
   This is the backstop that catches what the compiler flags cannot - an explicitly declared
   `double acc;` promotes nothing and passes every warning, but shows up here.

`tapp-build` runs check 3 automatically and deletes the output if it fails, so a `.tapp` that exists is
double-free by construction.

## Two kinds of declaration

- **`extern`** - the firmware exports it; the symbol stays undefined in the `.tapp` and is bound at load.
- **`static inline`** - the firmware does *not* export it, but Cortex-M7's FPv5 FPU has a single
  instruction for it, so the builtin expands in place and costs no import. Verified with `llvm-nm` that
  none of these emit a libcall: `sqrtf fabsf fminf fmaxf ceilf floorf roundf truncf rintf`
  (`vsqrt`/`vabs`/`vminnm`/`vmaxnm`/`vrintX`). `logf` is the one exception - it is derived from the
  exported `log10f`.

## Keeping it in sync

The export table is *generated from a firmware build*, so its contents move. If a tapp fails to load
with a missing import that is declared here, or a function you need is exported but missing here, check
against a table from the firmware build you are targeting:

```sh
# names the firmware currently exports
grep -oE '/\* [A-Za-z_][A-Za-z0-9_]* \*/' ../../api/tapp_api_table.c | sed 's|/\* ||;s| \*/||' | sort -u

# what a built tapp actually needs, and whether it is all resolvable
../tools/verify-tapp.sh --exports ../../api/tapp_api_table.c myapp.tapp
```

`tapp-build` passes `-nostdlibinc` so these headers are the only ones in scope. That is the point:
a full libc (newlib) declares far more than the firmware exports, which moves the failure from
compile time to load time on the device.

#!/usr/bin/env python3
"""The export allowlist, derived from tapp_api.h.

`tapp_api.h` is the contract: what it declares is what the firmware exports and what a tapp may
import. This module is the one place that turns the header into that set of names, so nothing has
to be kept in sync with anything.

  tools/api_exports.py                 every exported symbol, one per line
  tools/api_exports.py --header X.h    parse a specific header
  tools/api_exports.py --count

Used by tools/verify-tapp.sh for its import gate, and imported by the firmware's
lib/tapp/builder/generate_api_table.py to build the on-device symbol table. Sharing the parser is
the point: two implementations of "what does the header export" would disagree eventually, and the
disagreement shows up as a tapp that verifies clean and then fails to load.

The header must be seen exactly as a tapp's compiler sees it — FIRMWARE_BUILD and __cplusplus
UNDEFINED. Anything else silently adds or drops exports.

Stdlib only, python3.
"""

import argparse
import re
import sys
from pathlib import Path

# =============================================================================
# Implementation aliases
# =============================================================================
# Symbols whose exported IMPLEMENTATION differs from the declared name. The hash
# is still the hash of the declared name, so tapps resolve "memcpy" as always and
# need no rebuild — only the address the loader hands them changes.
#
# libc mem* accept arbitrary pointers by contract, and newlib implements that with
# unaligned word accesses: legal and fast on Normal memory. Tapp data lives in
# PSRAM, which is memory-mapped OCTOSPI and cannot do unaligned accesses at all —
# they UsageFault under the Device-like mapping the FX require, and silently land
# "with holes" under a non-cacheable one. The *4 variants only ever issue
# naturally-aligned accesses. See memcpy4/memset4/memmove4 in lib/sys/sys_utils.c.
#
# DELIBERATELY NARROW: aliasing all three (memcpy+memset+memmove) made tapp loading
# hang before the UI appeared — silently, with no crash report. The cause is not
# understood; memset4/memmove4 survived inspection and their disassembly is correct.
# Only memcpy is aliased here because it is the one actually implicated: it was the
# faulting PC in both unaligned-access reports. memset4/memmove4 stay defined in
# sys_utils.c (gc-sections drops them while unreferenced) and get re-enabled ONE AT
# A TIME once the hang is explained. Do not widen this map speculatively.
#
# The lut*_q15 entries are a DIFFERENT class and carry none of the caution above:
# they name the same code, not a different implementation. sys_utils.c wraps four
# `static FORCE_INLINE` LUT readers in non-inline exportable functions; those used
# an __asm__("lutsin_q15") rename, which collided with the name being wrapped and
# made all four compile to `b.n .` — a shipped infinite loop. Exporting the wrapper
# under its own symbol removes the collision; the declared name, and therefore the
# hash every built tapp already resolves, is untouched.
IMPL_ALIAS = {
    'memcpy': 'memcpy4',
    'lutsin_q15': 'lutsin_q15_extern',
    'lutcos_q15': 'lutcos_q15_extern',
    'lutsat_q15': 'lutsat_q15_extern',
    'lutexp_neg_q15': 'lutexp_neg_q15_extern',
}


def impl_symbol(name: str) -> str:
    """Linker symbol that backs a declared API name."""
    return IMPL_ALIAS.get(name, name)


NEVER_EXPORT_PREFIXES = (
    'hal_mpu_',       # MPU config — the sandbox itself
    'flash_',         # internal flash write/erase/verify
    'board_flash_',   # board flash read/flush
    'tapp_',          # loader/runtime, incl. the SVC gateway (tapp_gw_*)
    'elf_',           # ELF loader internals
    'ui_obj_',        # raw pointers into a versionless 80-byte widget struct
)

NEVER_EXPORT_EXACT = {
    'backup_bootloader', 'restore_bootloader', 'restore_backup',
    'os_controls_get',   # hands out the controls mutex + event queue
    'os_settings_get',   # mutable pointer to the entire device config
    'os_audio_get',      # OS-level SAI/DMA state
    'os_led_get',        # LED task queue + task id
    'os_app_close',      # frees the image on the caller's stack — tapps call os_app_exit()
}


def is_never_export(name: str) -> bool:
    return name in NEVER_EXPORT_EXACT or name.startswith(NEVER_EXPORT_PREFIXES)


# =============================================================================
# Header parsing
# =============================================================================

_C_KEYWORDS = {
    'if', 'else', 'for', 'while', 'switch', 'return', 'sizeof', 'typedef',
    'struct', 'union', 'enum', 'static', 'inline', 'extern', 'const',
    'volatile', 'unsigned', 'signed', 'void', 'char', 'short', 'int', 'long',
    'float', 'double', 'defined', 'do', 'case', 'break', 'continue', 'goto',
    'register', 'restrict', 'asm', '_Static_assert', '_Noreturn',
}

# The tapp compiles with neither defined. Everything else is unknown, and an
# unknown condition keeps BOTH branches (over-include is caught by the drift
# check and the linker; under-include would silently shrink the published API).
_KNOWN_MACROS = {'FIRMWARE_BUILD': False, '__cplusplus': False}

_TRAILING_IDENT = re.compile(r'[A-Za-z_]\w*$')

# `extern const uint8_t gfx_nunito_bold_18[];` — fonts and other read-only blobs.
_DATA_DECL = re.compile(
    r'\bextern\s+const\s+[A-Za-z_]\w*\s+([A-Za-z_]\w*)\s*\[\s*\]\s*;')

# Provided BY the tapp, not resolved from the firmware.
_TAPP_PROVIDED = {'tapp_get_descriptor'}


def _strip_comments(src: str) -> str:
    """Drop comments and string/char literals, preserving line structure."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * src.count('\n', i, j))  # keep directive lines aligned
            i = j
        elif c == '/' and src.startswith('//', i):
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif c in '"\'':
            q, i = c, i + 1
            while i < n and src[i] != q:
                i += 2 if src[i] == '\\' else 1
            i += 1
            out.append(' ')
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def _preprocess(src: str) -> str:
    """Remove directives; evaluate only the conditionals whose answer we know."""
    out, stack = [], []
    for line in src.split('\n'):
        t = line.strip()
        if t.startswith('#'):
            w = t[1:].strip().split()
            k = w[0] if w else ''
            if k in ('ifdef', 'ifndef', 'if'):
                cond = None
                if k == 'ifdef' and len(w) > 1:
                    cond = _KNOWN_MACROS.get(w[1])
                elif k == 'ifndef' and len(w) > 1:
                    v = _KNOWN_MACROS.get(w[1])
                    cond = None if v is None else (not v)
                elif k == 'if':
                    m = re.fullmatch(r'defined\s*\(?\s*(\w+)\s*\)?', ' '.join(w[1:]))
                    if m:
                        cond = _KNOWN_MACROS.get(m.group(1))
                stack.append(cond)
            elif k == 'else' and stack:
                stack[-1] = None if stack[-1] is None else (not stack[-1])
            elif k == 'elif' and stack:
                stack[-1] = None
            elif k == 'endif' and stack:
                stack.pop()
            out.append('')  # a directive never contributes a declarator
            continue
        out.append('' if any(c is False for c in stack) else line)
    return '\n'.join(out)


def _drop_attributes(src: str) -> str:
    """Delete __attribute__((...)) with BALANCED parens.

    A one-level regex is why snprintf was invisible to the old parser: its
    __attribute__((format(printf, 3, 4))) nests, so the naive pattern stopped at
    the inner ')' and left a tail that broke the declaration.
    """
    out, i = [], 0
    while True:
        j = src.find('__attribute__', i)
        if j < 0:
            out.append(src[i:])
            return ''.join(out)
        out.append(src[i:j])
        k = src.find('(', j)
        if k < 0:
            i = j + len('__attribute__')
            continue
        d = 0
        while k < len(src):
            if src[k] == '(':
                d += 1
            elif src[k] == ')':
                d -= 1
                if d == 0:
                    k += 1
                    break
            k += 1
        i = k


def _drop_bodies(src: str) -> str:
    """Replace every top-level {...} with '@;'.

    The header carries ~15 `static inline` wrappers (sqrtf, fabsf, abs, ...).
    The '@' makes a DEFINITION unparseable so it is not mistaken for a
    declaration, and the ';' stops it swallowing the next real prototype.
    Without this the parser harvests __builtin_* out of the bodies.
    """
    out, d = [], 0
    for ch in src:
        if ch == '{':
            d += 1
        elif ch == '}':
            if d > 0:
                d -= 1
                if d == 0:
                    out.append('@;')
        elif d == 0:
            out.append(ch)
    return ''.join(out)


def default_header() -> Path:
    """tapp_api.h beside this tools/ directory."""
    return Path(__file__).resolve().parent.parent / 'tapp_api.h'


def sdk_contract(header: Path, verbose: bool = False) -> tuple[set[str], set[str]]:
    """(functions, data) a tapp may import — THE EXPORT ALLOWLIST.

    Everything not here stays inside the firmware.
    """
    hdr = Path(header)
    if not hdr.exists():
        print(f"Error: SDK header missing: {hdr}\n"
              "       git -C lib submodule update --init tapp/builder/sdk",
              file=sys.stderr)
        sys.exit(1)

    raw = hdr.read_text().replace('\\\n', '')  # join line continuations
    src = _preprocess(_strip_comments(raw))
    data = set(_DATA_DECL.findall(src))
    src = _drop_bodies(_drop_attributes(src))

    fns: set[str] = set()
    for chunk in src.split(';'):
        c = chunk.strip()
        if not c.endswith(')'):
            continue                      # definition remnant / non-function decl
        if re.search(r'\btypedef\b', c):
            continue                      # typedef'd function pointers are types
        d, i = 0, len(c) - 1              # walk back over the parameter list
        while i >= 0:
            if c[i] == ')':
                d += 1
            elif c[i] == '(':
                d -= 1
                if d == 0:
                    break
            i -= 1
        if i < 0:
            continue
        m = _TRAILING_IDENT.search(c[:i].strip())
        if not m:
            # e.g. `void (*f(int))(void)` — a function returning a function
            # pointer. Not used today; warn rather than drop it silently.
            print(f"Warning: {hdr.name}: unparsed declaration: "
                  f"{' '.join(c.split())[:100]}", file=sys.stderr)
            continue
        n = m.group(0)
        if n in _C_KEYWORDS or n.startswith('__'):
            continue                      # never a real export
        fns.add(n)

    fns -= _TAPP_PROVIDED

    if verbose:
        print(f"  {hdr.name}: {len(fns)} functions + {len(data)} data symbols")
    return fns, data


def exported_symbols(header: Path = None) -> set[str]:
    """Every name a tapp may import, blocklist applied."""
    fns, data = sdk_contract(header or default_header())
    return {n for n in (fns | data) if not is_never_export(n)}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--header', type=Path, default=None,
                    help='header to parse (default: ../tapp_api.h)')
    ap.add_argument('--count', action='store_true', help='print the count only')
    args = ap.parse_args()

    names = sorted(exported_symbols(args.header))
    if not names:
        print('no exported symbols found — is the header intact?', file=sys.stderr)
        return 1
    print(len(names) if args.count else '\n'.join(names))
    return 0


if __name__ == '__main__':
    sys.exit(main())

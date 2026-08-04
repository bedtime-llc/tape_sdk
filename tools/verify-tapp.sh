#!/usr/bin/env bash
#
# verify-tapp.sh — check that a .tapp will actually load on the device (and in the emulator)
#
#   ./tools/verify-tapp.sh app.tapp [more.tapp ...]
#   ./tools/verify-tapp.sh --exports /path/to/tapp_api_table.c app.tapp
#
# A .tapp is a plain ELF32 ARM ET_REL object. The firmware loader is strict in ways a linker is not,
# so "it linked" is not "it loads". This reproduces the loader's checks:
#
#   1. shape          ELF32 / little-endian / EM_ARM / ET_REL
#   2. relocations    only the types lib/tapp/elf/elf_file.c implements (see FW_OK below)
#   3. unwind tables  no .ARM.exidx — clang emits it and its R_ARM_PREL31 is not supported
#   4. manifest       a .tapp_manifest section with magic "TAPP"
#   5. entry point    a defined tapp_get_descriptor symbol
#   6. imports        (optional) every undefined symbol is in the firmware's export table
#
# NOTE: no `set -o pipefail` here — `cmd | grep -q` SIGPIPEs the producer and would fail every check.

set -u

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

# Prefer LLVM binutils (they read ARM objects on any host); fall back to arm-none-eabi.
pick() { for c in "$@"; do command -v "$c" >/dev/null 2>&1 && { echo "$c"; return; }; done; }
READELF=$(pick llvm-readelf llvm-readelf-18 arm-none-eabi-readelf readelf)
NM=$(pick llvm-nm llvm-nm-18 arm-none-eabi-nm nm)
OBJDUMP=$(pick llvm-objdump llvm-objdump-18 arm-none-eabi-objdump objdump)

# llvm-objdump will not decode FPv5-D16 double-precision instructions unless told the CPU — it
# prints "<unknown>" instead, which would make the f64 scan below silently pass. GNU objdump
# decodes them without help and rejects --mcpu.
case "$OBJDUMP" in
    *llvm-objdump*) OBJDUMP_ARGS="--mcpu=cortex-m7" ;;
    *)              OBJDUMP_ARGS="" ;;
esac
[ -n "${READELF:-}" ] || { echo "no readelf found (install llvm or gcc-arm-none-eabi)" >&2; exit 2; }
[ -n "${NM:-}" ]      || { echo "no nm found (install llvm or gcc-arm-none-eabi)" >&2; exit 2; }

# Relocation types the FIRMWARE loader implements (lib/tapp/elf/elf_file.c, apply_relocation).
FW_OK='^R_ARM_(NONE|ABS32|REL32|THM_CALL|THM_JUMP24|TARGET1|TARGET2|THM_MOVW_ABS_NC|THM_MOVT_ABS|THM_MOVW_PREL_NC|THM_MOVT_PREL)$'
# Narrower set the desktop EMULATOR implements. Advisory only: a tapp outside this set
# still runs on hardware, it just can't be previewed. -mno-movt is what keeps builds inside it.
EMU_OK='^R_ARM_(NONE|ABS32|REL32|THM_CALL|THM_JUMP24|TARGET1|THM_JUMP19)$'

EXPORTS_SRC=""
EXPORTS_EXPLICIT=0
if [ "${1:-}" = "--exports" ]; then EXPORTS_SRC="${2:-}"; EXPORTS_EXPLICIT=1; shift 2; fi
[ "$#" -gt 0 ] || { echo "usage: $0 [--exports tapp_api_table.c] <file.tapp> ..." >&2; exit 2; }

# Find the firmware export table automatically when not told where it is. Without it the import
# gate cannot run, and a run that silently skips a gate but still prints "all checks passed" is the
# same false confidence this tool exists to remove. This SDK lives at
# <firmware>/lib/tapp/builder/sdk, so from tools/ the table is three levels up under api/.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -z "$EXPORTS_SRC" ] && [ -f "$SCRIPT_DIR/../../../api/tapp_api_table.c" ]; then
    EXPORTS_SRC="$SCRIPT_DIR/../../../api/tapp_api_table.c"
fi

EXPORTS_LIST=""
if [ -n "$EXPORTS_SRC" ]; then
    if [ ! -f "$EXPORTS_SRC" ]; then
        # An explicit bad path is a usage error; a missing auto-detected one just means standalone.
        [ "$EXPORTS_EXPLICIT" -eq 1 ] && { echo "exports file not found: $EXPORTS_SRC" >&2; exit 2; }
        EXPORTS_SRC=""
    else
        EXPORTS_LIST=$(mktemp)
        # The table is a .rodata asm blob; each row carries the symbol name in a /* comment */.
        grep -oE '/\* [A-Za-z_][A-Za-z0-9_]* \*/' "$EXPORTS_SRC" \
            | sed 's|/\* ||;s| \*/||' | sort -u > "$EXPORTS_LIST"
        trap 'rm -f "$EXPORTS_LIST"' EXIT
    fi
fi

RC=0
SKIPPED=0
for TAPP in "$@"; do
    echo "── $(basename "$TAPP") ──"
    if [ ! -f "$TAPP" ]; then echo -e "  ${RED}✗${NC} no such file"; RC=1; continue; fi
    ok=1
    hdr=$("$READELF" -h "$TAPP" 2>/dev/null)

    # 1. shape.
    #
    # If this fails, STOP. Every later gate works by asking readelf/objdump about a structure that
    # is not there, and each would answer "nothing found" — which the checks below would report as
    # a pass. A source file would come back "✓ no relocations, ✓ no exidx, ✓ no 64-bit floats".
    # A vacuous pass on the doubles gate is exactly the failure this tool exists to prevent.
    if echo "$hdr" | grep -q 'ELF32' \
       && echo "$hdr" | grep -qi "2's complement, little endian" \
       && echo "$hdr" | grep -qiE 'Type:.*REL \(Relocatable' \
       && echo "$hdr" | grep -qi 'Machine:.*ARM'; then
        echo -e "  ${GREEN}✓${NC} ELF32 little-endian ARM relocatable"
    else
        echo -e "  ${RED}✗${NC} not an ELF32 LE ARM ET_REL object"
        # Say what it actually is, so the fix is obvious rather than a guess.
        magic=$(head -c 4 "$TAPP" | od -An -tx1 2>/dev/null | tr -d ' \n')
        case "$TAPP" in
            *.c|*.h|*.cpp)
                echo -e "     that is a source file — build it first:"
                echo -e "       ./tapp-build $TAPP" ;;
            *)
                if [ "$magic" = "7f454c46" ]; then
                    echo -e "     it is an ELF, but not a 32-bit little-endian ARM relocatable:"
                    echo "$hdr" | grep -iE '^\s*(Class|Data|Type|Machine):' | sed 's/^/       /'
                    echo -e "     a .tapp must be linked with 'ld.lld -r' (ET_REL), not a full link"
                else
                    echo -e "     not an ELF file at all (starts ${magic:-empty}); expected 7f454c46"
                fi ;;
        esac
        echo -e "  ${YELLOW}!${NC} remaining checks skipped — they would report false passes"
        RC=1
        echo
        continue
    fi

    # 2. relocations
    types=$("$READELF" -r --wide "$TAPP" 2>/dev/null | grep -oE 'R_ARM_[A-Z0-9_]+' | sort -u)
    bad_fw=$(echo "$types" | grep -vE "$FW_OK")
    bad_emu=$(echo "$types" | grep -vE "$EMU_OK")
    if [ -z "$bad_fw" ]; then
        echo -e "  ${GREEN}✓${NC} relocations: $(echo $types | tr '\n' ' ')"
    else
        echo -e "  ${RED}✗${NC} device loader cannot handle: $(echo $bad_fw | tr '\n' ' ')"; ok=0
    fi
    [ -n "$bad_emu" ] && echo -e "  ${YELLOW}!${NC} emulator cannot handle: $(echo $bad_emu | tr '\n' ' ') (add -mno-movt)"

    # 3. unwind tables
    if [ "$("$READELF" -S --wide "$TAPP" 2>/dev/null | grep -ci 'ARM\.exidx')" -eq 0 ]; then
        echo -e "  ${GREEN}✓${NC} no .ARM.exidx unwind tables"
    else
        echo -e "  ${RED}✗${NC} .ARM.exidx present — link with -T discard.ld"; ok=0
    fi

    # 4. manifest: section present, and its first word is the "TAPP" magic 0x50504154
    if "$READELF" -S --wide "$TAPP" 2>/dev/null | grep -q '\.tapp_manifest'; then
        # `readelf -x` prints: "0xADDR  w0 w1 w2 w3  |ascii|". Keep only the four hex words per line
        # so we get the section as one clean hex string, independent of the ASCII gutter's format.
        hex=$("$READELF" -x .tapp_manifest "$TAPP" 2>/dev/null \
              | awk '/^ *0x/ { print $2 $3 $4 $5 }' | tr -d '\n')
        # struct: magic u32, version u32, {major,minor,target,reserved} u16 x4, stack u32,
        # app_version u32, char name[32]  -> name starts at byte 24 (hex char 48).
        if [ "${hex:0:8}" = "54415050" ]; then
            # decode name[32] up to the NUL. printf '%b' is portable; awk strtonum is gawk-only.
            esc=""
            for p in $(echo "${hex:48:64}" | sed 's/../& /g'); do
                [ "$p" = "00" ] && break
                esc="${esc}\\x${p}"
            done
            name=$(printf '%b' "$esc")
            echo -e "  ${GREEN}✓${NC} manifest present, app name: \"${name}\""
        else
            echo -e "  ${RED}✗${NC} .tapp_manifest has wrong magic (got ${hex:0:8}, want 54415050)"; ok=0
        fi
    else
        echo -e "  ${RED}✗${NC} no .tapp_manifest section"; ok=0
    fi

    # 5. entry point — weak (W) in practice, so accept T/t/W/w
    if "$NM" "$TAPP" 2>/dev/null | grep -qE ' [TtWw] (tapp_get_descriptor|_start)$'; then
        echo -e "  ${GREEN}✓${NC} entry point defined"
    else
        echo -e "  ${RED}✗${NC} no defined tapp_get_descriptor"; ok=0
    fi

    # 6. no 64-bit floating point, anywhere.
    #
    # Doubles are banned in tapps: the FPU is FPv5-D16, so double math is real but roughly half the
    # throughput of f32 and doubles register pressure on the audio hot path. -Werror=double-promotion
    # catches float->double promotion at compile time but NOT an explicitly declared `double`, so the
    # only reliable check is on the emitted instructions.
    if [ -n "${OBJDUMP:-}" ]; then
        disasm=$("$OBJDUMP" -d $OBJDUMP_ARGS "$TAPP" 2>/dev/null)
        undec=$(echo "$disasm" | grep -c '<unknown>')
        f64=$(echo "$disasm" | grep -oE '\bv[a-z0-9]+\.f64' | sort | uniq -c | sort -rn)
        # A disassembler that cannot decode the FPU would report zero f64 for a file full of it.
        if [ "$undec" -gt 0 ]; then
            echo -e "  ${RED}✗${NC} f64 scan unreliable: $undec undecodable instructions ($OBJDUMP)"; ok=0
        elif [ -z "$f64" ]; then
            echo -e "  ${GREEN}✓${NC} no 64-bit float instructions"
        else
            echo -e "  ${RED}✗${NC} 64-bit float instructions present (use float/f-suffixed literals):"
            echo "$f64" | sed 's/^/      /'
            ok=0
        fi
    fi
    # Double softfloat helpers would also mean doubles; they are unexported so §7 catches them too,
    # but name them explicitly because the generic "not exported" message is unhelpful here.
    dsyms=$("$NM" -u "$TAPP" 2>/dev/null | awk '{print $NF}' \
            | grep -E '^(__aeabi_(d|f2d|i2d|ui2d|l2d|ul2d)|__(add|sub|mul|div|neg|cmp|eq|ne|lt|le|gt|ge)df|__float[a-z]*df|__fixdf|__extendsfdf|__truncdfsf)' || true)
    if [ -n "$dsyms" ]; then
        echo -e "  ${RED}✗${NC} double-precision runtime calls: $(echo $dsyms | tr '\n' ' ')"; ok=0
    fi

    # 7. imports (optional)
    if [ -n "$EXPORTS_LIST" ]; then
        undef=$("$NM" -u "$TAPP" 2>/dev/null | awk '{print $NF}' | sort -u)
        n=$(echo "$undef" | grep -c .)
        missing=$(comm -23 <(echo "$undef") "$EXPORTS_LIST")
        if [ -z "$missing" ]; then
            echo -e "  ${GREEN}✓${NC} imports: $n/$n resolvable"
        else
            echo -e "  ${RED}✗${NC} $(echo $missing | wc -w | tr -d ' ') of $n imports not exported by firmware:"
            echo "$missing" | sed 's/^/      /'
            ok=0
        fi
    else
        # Never stay silent about a gate that did not run — see the note at EXPORTS_SRC above.
        echo -e "  ${YELLOW}!${NC} imports NOT checked (no firmware export table found)"
        echo -e "     pass --exports <firmware>/lib/tapp/api/tapp_api_table.c to enable it"
        SKIPPED=1
    fi

    echo "  size: $(wc -c < "$TAPP" | tr -d ' ') bytes"
    [ $ok -eq 1 ] || RC=1
    echo
done

if [ $RC -ne 0 ]; then
    echo -e "${RED}✗ verification failed${NC}"
elif [ $SKIPPED -ne 0 ]; then
    # Honest wording: not every gate ran, so this is not the same claim as a full pass.
    echo -e "${GREEN}✓ checks passed${NC}, ${YELLOW}but the import gate was skipped${NC}"
else
    echo -e "${GREEN}✓ all checks passed${NC}"
fi
exit $RC

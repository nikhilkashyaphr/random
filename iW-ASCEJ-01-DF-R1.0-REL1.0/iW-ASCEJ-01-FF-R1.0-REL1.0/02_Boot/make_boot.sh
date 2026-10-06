#!/usr/bin/env bash
#===============================================================================
# make_boot.sh (v2) - build BOOT.BIN for the ZU47DR RFSoC bare-metal application
#                     Vitis 2025.2 / SDT flow
#
# Usage:
#   ./make_boot.sh [WORKSPACE_DIR]
#
# Override auto-discovery for any artifact:
#   PMUFW=/path/pmufw.elf ./make_boot.sh <workspace>
#   (FSBL=, BIT=, APP= work the same way)
#
# Flags:
#   --list                 show what would be selected, then exit
#   --allow-qemu-pmufw     permit a QEMU pmufw.elf (NOT for hardware)
#
# v2 fixes:
#   * ALL diagnostics go to stderr. v1 printed the "Multiple candidates" banner
#     to stdout, where command substitution folded it into the variable - which
#     then reached readelf as a filename.
#   * candidates ranked by explicit path priority, not by mtime
#   * qemu/ and resources/ artifacts excluded: emulation stubs, not boot images
#===============================================================================
set -euo pipefail

WS_ARG=""; LIST_ONLY=0; ALLOW_QEMU_PMUFW=0
for a in "$@"; do
    case "$a" in
        --list)             LIST_ONLY=1 ;;
        --allow-qemu-pmufw) ALLOW_QEMU_PMUFW=1 ;;
        -*) echo "unknown flag: $a" >&2; exit 2 ;;
        *)  WS_ARG="$a" ;;
    esac
done

WS="${WS_ARG:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
OUT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIF_OUT="$OUT/boot_generated.bif"
BIN="$OUT/BOOT.BIN"

# ---- every message goes to stderr; stdout is reserved for resolved paths -----
log() { printf '%s\n' "$*" >&2; }
red() { printf '\033[31m%s\033[0m\n' "$*" >&2; }
grn() { printf '\033[32m%s\033[0m\n' "$*" >&2; }
ylw() { printf '\033[33m%s\033[0m\n' "$*" >&2; }

log "Workspace: $WS"
log ""

# qemu/ = QEMU emulation binaries.  resources/ = platform staging copies.
# Neither belongs in a hardware boot image.
is_excluded() { case "$1" in */qemu/*|*/resources/*) return 0;; *) return 1;; esac; }

rank() {
    case "$1" in
        */export/*/sw/boot/*) echo 0 ;;   # canonical exported boot artifacts
        */build/*)            echo 1 ;;   # component build output
        */export/*)           echo 2 ;;
        *)                    echo 3 ;;
    esac
}

pick() {   # $1 = allow_excluded(0|1), $2 = description, $3.. = -name patterns
    local allow="$1"; shift
    local desc="$1"; shift
    local all good excl best bestrank r n
    all=$(find "$WS" -type f \( "$@" \) 2>/dev/null | grep -v '/_ide/' | sort -u || true)
    if [ -z "$all" ]; then red "MISSING: $desc"; return 1; fi

    good=""; excl=""
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        if is_excluded "$f"; then excl+="$f"$'\n'; else good+="$f"$'\n'; fi
    done <<< "$all"

    if [ -z "${good//[[:space:]]/}" ]; then
        if [ "$allow" = "1" ] && [ -n "${excl//[[:space:]]/}" ]; then
            ylw "  $desc: no clean candidate; --allow-qemu-pmufw given, using an"
            ylw "  emulation/staging copy. Verify it before shipping."
            good="$excl"; excl=""
        else
            red "MISSING: $desc  (only emulation/staging copies exist)"
            while IFS= read -r f; do [ -n "$f" ] && log "        excluded: $f"; done <<< "$excl"
            return 1
        fi
    fi

    best=""; bestrank=99
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        r=$(rank "$f")
        if [ "$r" -lt "$bestrank" ]; then bestrank="$r"; best="$f"; fi
    done <<< "$good"

    n=$(printf '%s' "$good" | grep -c . || true)
    if [ "$n" -gt 1 ]; then
        ylw "  $n candidates for $desc - chose by path priority:"
        while IFS= read -r f; do
            [ -z "$f" ] && continue
            if [ "$f" = "$best" ]; then log "      -> $f"; else log "         $f"; fi
        done <<< "$good"
    fi
    while IFS= read -r f; do [ -n "$f" ] && log "      (ignored: emulation/staging) $f"; done <<< "$excl"

    printf '%s\n' "$best"
}

MISS=0
FSBL="${FSBL:-$(pick 0 "FSBL elf"      -name 'zynqmp_fsbl.elf' -o -name 'fsbl_a53.elf' -o -name 'fsbl.elf')}" || MISS=1
PMUFW="${PMUFW:-$(pick "$ALLOW_QEMU_PMUFW" "PMU firmware" -name 'zynqmp_pmufw.elf' -o -name 'pmufw.elf')}"      || MISS=1
BIT="${BIT:-$(pick 0 "PL bitstream"   -name '*.bit')}"                                                         || MISS=1
APP="${APP:-$(pick 0 "application elf" -name 'app_component.elf')}"                                            || MISS=1
log ""

if [ "$MISS" = "1" ]; then
    red "Cannot build BOOT.BIN - see MISSING entries above."
    cat >&2 <<'HINT'

  PMU FIRMWARE MISSING (only qemu/ copies found)?
  ---------------------------------------------------------------------------
  A pmufw.elf under any qemu/ directory is a QEMU EMULATION binary. It is not
  built against your XSA and must not go into a hardware boot image.

  A real PMU firmware exists only if the platform component was created with
  "Generate boot artifacts" ticked. When it is, you get BOTH:
        <platform>/zynqmp_fsbl/build/fsbl.elf
        <platform>/zynqmp_pmufw/build/pmufw.elf
  and the export mirrors both into:
        <platform>/export/<platform>/sw/boot/

  If sw/boot/ has fsbl.elf but NO pmufw.elf, the FSBL domain was generated and
  the PMU firmware domain was not.

  Fix (preferred - no platform rebuild needed):
     File > New Component > Application
       Platform  : platform_ZU47
       Processor : psu_pmu_0
       Template  : "ZynqMP PMU Firmware"
     Build it, then point this script at the result:
       PMUFW=<ws>/zynqmp_pmufw/build/pmufw.elf ./make_boot.sh <ws>

  Fix (thorough): recreate the platform component with "Generate boot
  artifacts" ticked and rebuild. The option cannot be added afterwards.

  Last resort, accepting the risk:
       ./make_boot.sh <ws> --allow-qemu-pmufw
  ---------------------------------------------------------------------------

  BITSTREAM MISSING?
     unzip -o <design>.xsa '*.bit' -d <platform>/hw/
     An XSA exported pre-synthesis contains no .bit - re-export from Vivado
     with "Include bitstream".

  APPLICATION MISSING?
     Build the app component first.
HINT
    exit 1
fi

if is_excluded "$PMUFW" && [ "$ALLOW_QEMU_PMUFW" != "1" ]; then
    red "REFUSING: selected PMU firmware is an emulation/staging copy:"
    red "    $PMUFW"
    log "  Build a real PMU firmware (see --help notes), or pass --allow-qemu-pmufw."
    exit 1
fi

grn "Selected artifacts:"
log "  FSBL    : $FSBL"
log "  PMUFW   : $PMUFW"
log "  Bitstr. : $BIT"
log "  App     : $APP"
log ""

if [ "$LIST_ONLY" = "1" ]; then exit 0; fi

#--- validation ----------------------------------------------------------------
warn=0
mach() { readelf -h "$1" 2>/dev/null | awk -F: '/Machine/{print $2}' | xargs; }

if command -v readelf >/dev/null 2>&1; then
    m=$(mach "$FSBL");  case "$m" in *AArch64*) grn "  [ok] FSBL is AArch64";;
        *) red "  [!!] FSBL machine '$m', expected AArch64"; warn=1;; esac
    m=$(mach "$APP");   case "$m" in *AArch64*) grn "  [ok] Application is AArch64";;
        *) red "  [!!] Application machine '$m', expected AArch64"; warn=1;; esac
    # PMU firmware runs on the MicroBlaze PMU, never on the A53.
    m=$(mach "$PMUFW"); case "$m" in *MicroBlaze*|*Xilinx*) grn "  [ok] PMU firmware is MicroBlaze";;
        *) red "  [!!] PMU firmware machine '$m', expected MicroBlaze"; warn=1;; esac

    entry=$(readelf -h "$APP" | awk -F: '/Entry point/{print $2}' | xargs)
    if [ $(( entry )) -ge 4293918720 ]; then
        red "  [!!] Application entry $entry is in OCM - link into DDR"; warn=1
    else
        grn "  [ok] Application entry $entry is in DDR"
    fi
fi

bitsz=$(stat -c%s "$BIT")
if [ "$bitsz" -lt 1000000 ]; then
    red "  [!!] Bitstream only $bitsz bytes - too small for a ZU47DR"; warn=1
else
    grn "  [ok] Bitstream $(( bitsz / 1024 / 1024 )) MB"
fi

[ "$warn" = "1" ] && { log ""; ylw "Warnings above - review before flashing."; }
log ""

#--- bootgen -------------------------------------------------------------------
cat > "$BIF_OUT" <<EOF
the_ROM_image:
{
    [bootloader, destination_cpu=a53-0] $FSBL
    [pmufw_image] $PMUFW
    [destination_device=pl] $BIT
    [destination_cpu=a53-0, exception_level=el-3] $APP
}
EOF
log "BIF: $BIF_OUT"

command -v bootgen >/dev/null 2>&1 || {
    red "bootgen not on PATH - source the Vitis settings first:"
    log "    source /path/to/Vitis/2025.2/settings64.sh"
    exit 1
}

bootgen -arch zynqmp -image "$BIF_OUT" -o "$BIN" -w on
log ""
grn "BOOT.BIN written: $BIN  ($(stat -c%s "$BIN") bytes)"
cat >&2 <<'NEXT'

Next steps
  SD   : copy BOOT.BIN alone to the root of a FAT32 partition, set mode pins to
         SD, power-cycle.
  QSPI : program_flash -f BOOT.BIN -offset 0 \
                       -flash_type qspi-x8-dual_parallel \
                       -fsbl <fsbl.elf> -cable type xilinx_tcf
  UART : PS UART0, 115200 8N1.
NEXT

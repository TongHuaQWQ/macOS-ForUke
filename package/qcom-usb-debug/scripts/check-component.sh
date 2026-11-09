#!/usr/bin/env bash
# check-component.sh — everything that can be verified without the kernel build.
#
# The kernel archives only compile on a macOS runner (Xcode + Apple MIG), so the component's own
# mistakes used to surface 25 minutes later in CI. This runs the checks that need nothing but a host C
# compiler, in seconds:
#
#   1. the patch copies are byte-identical to the component (scripts/check-patch-copies.py)
#   2. no file-local helper is used above its declaration in the sources that cannot be compiled here
#   3. every freestanding unit compiles clean with gcc *and* clang at -Wall -Wextra -Werror
#   4. tests/check.py — the sanitised models, the freestanding cross-compiles and the import audits
#
# Usage: scripts/check-component.sh [--fast] [--output DIR]
#   --fast    skip step 4 (the sanitised suite); useful before every push
#   --output  where tests/check.py writes (default: <repo>/.cache/component-checks)
#
# Exit code is non-zero if any step fails, so it is safe to call from CI.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"     # package/qcom-usb-debug
ROOT="$(cd "$HERE/../.." && pwd)"                            # the device repository
FAST=0
OUT=""

while [ $# -gt 0 ]; do
  case "$1" in
    --fast)   FAST=1; shift ;;
    --output) OUT="${2:-}"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) printf 'unknown argument: %s\n' "$1" >&2; exit 2 ;;
  esac
done

fail=0
step() { printf '\n== %s\n' "$1"; }
bad()  { printf '   FAIL: %s\n' "$1" >&2; fail=1; }
ok()   { printf '   ok: %s\n' "$1"; }

# --- 1. patch copies -------------------------------------------------------------------------------
step "patch copies"
if [ -f "$ROOT/scripts/check-patch-copies.py" ]; then
  if python3 "$ROOT/scripts/check-patch-copies.py"; then ok "patch copies match the component"; else bad "patch copies differ"; fi
else
  printf '   skipped: %s/scripts/check-patch-copies.py not found\n' "$ROOT"
fi

# --- 2. declaration order ---------------------------------------------------------------------------
# kernel/QcomXnuUsbDebug.c needs an XNU tree, so a helper used above its definition only fails in CI.
# That is what broke the b106 build (append_hex32), and it is a text check away from being caught here.
step "declaration order"
if python3 "$HERE/scripts/check-declaration-order.py" \
     "$HERE/kernel/QcomXnuUsbDebug.c" "$HERE/usb/Dwc3.c"; then
  ok "no helper is used before its declaration"
else
  bad "a helper is used before its declaration"
fi

# --- 3. freestanding compiles ----------------------------------------------------------------------
# One command per (compiler, unit). These are the units that end up inside the kernel, plus the two SoC
# backends, so a typo, an unused variable or an implicit declaration is caught here rather than there.
PROFILE="$HERE/soc/Sm8550.h"
units=("usb/Dwc3.c" "soc/QcomUsbQualify.c" "soc/QcomUsbDmaDomain.c")
compilers=()
for cc in gcc clang; do
  if command -v "$cc" >/dev/null 2>&1; then compilers+=("$cc"); fi
done
step "freestanding compiles (${compilers[*]:-none})"
if [ ${#compilers[@]} -eq 0 ]; then
  bad "no host C compiler found"
else
  for cc in "${compilers[@]}"; do
    # macOS ships clang as gcc; do not report the same compiler twice.
    if [ "$cc" = "gcc" ] && [ ${#compilers[@]} -gt 1 ] && \
       "$cc" --version 2>/dev/null | head -1 | grep -qi clang; then
      printf '   skipped gcc (it is clang here)\n'
      continue
    fi
    for unit in "${units[@]}"; do
      args=(-std=c11 -Wall -Wextra -Werror -I"$HERE" -c "$HERE/$unit" -o /dev/null)
      case "$unit" in
        soc/*) args+=(-DQUD_SOC_HEADER="\"$PROFILE\"") ;;
      esac
      if out=$("$cc" "${args[@]}" 2>&1); then
        ok "$cc $unit"
      else
        printf '%s\n' "$out" >&2
        bad "$cc $unit"
      fi
    done
  done
fi

# --- 4. the sanitised suite ------------------------------------------------------------------------
step "sanitised suite (tests/check.py)"
if [ "$FAST" = "1" ]; then
  printf '   skipped: --fast\n'
else
  target="${TARGET_CLANG:-}"
  if [ -z "$target" ] && command -v xcrun >/dev/null 2>&1; then target="$(xcrun -f clang 2>/dev/null)"; fi
  if [ -z "$target" ] && command -v clang >/dev/null 2>&1; then target="$(command -v clang)"; fi
  if [ -z "$OUT" ]; then
    OUT="$ROOT/.cache/component-checks"
  fi
  if [ -z "$target" ]; then
    bad "no clang for the freestanding cross-compiles (set TARGET_CLANG)"
  else
    rm -rf "$OUT"
    mkdir -p "$(dirname "$OUT")"
    if python3 "$HERE/tests/check.py" --output "$OUT" --target-clang "$target"; then
      ok "suite passed: $OUT/check-report.json"
    else
      bad "suite failed"
    fi
  fi
fi

if [ "$fail" = "0" ]; then
  printf '\ncomponent checks passed\n'
else
  printf '\ncomponent checks FAILED\n' >&2
fi
exit "$fail"

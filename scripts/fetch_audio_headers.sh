#!/usr/bin/env bash
set -euo pipefail

# Fetch the real, single-header miniaudio and dr_wav libraries into
# bench/audio_target/vendor/ so that audio_target_full.cpp can compile
# and `opt` can analyse the actual library source rather than opaque
# `extern "C"` declarations.
#
# URLs are the canonical, public-domain release of these single-header
# libraries (mackron's projects). If either one fails to download the
# script aborts without overwriting prior copies.
#
# Run once before invoking scripts/eval_audio_full.sh.

VENDOR_DIR="$(cd "$(dirname "$0")"/../bench/audio_target/vendor && pwd)"
mkdir -p "$VENDOR_DIR"

fetch() {
  local url="$1" out="$2"
  if [[ -s "$out" ]]; then
    echo "[fetch] $out exists; skipping (delete to re-fetch)"
    return
  fi
  echo "[fetch] $url -> $out"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url" -o "$out"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$out" "$url"
  else
    echo "error: curl/wget missing" >&2
    exit 1
  fi
}

fetch "https://raw.githubusercontent.com/mackron/dr_libs/master/dr_wav.h" \
      "$VENDOR_DIR/dr_wav.h"
fetch "https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h" \
      "$VENDOR_DIR/miniaudio.h"

# Sanity: each file must contain the public-domain preamble (a `/*
# Choice of public domain` block) so a 404 page or HTML error reply
# cannot silently masquerade as the header.
for f in "$VENDOR_DIR"/dr_wav.h "$VENDOR_DIR"/miniaudio.h; do
  if ! rg -q 'Choice of public domain' "$f"; then
    echo "error: $f does not look like a mackron single-header \
library (missing 'Choice of public domain' preamble; got \
$(stat -c%s "$f") bytes)" >&2
    exit 1
  fi
done
echo "[fetch] OK.  dr_wav.h + miniaudio.h ready under $VENDOR_DIR/"
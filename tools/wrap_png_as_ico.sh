#!/usr/bin/env bash
#
# wrap_png_as_ico.sh -- wrap a PNG into a minimal single-image .ico container
# (the Vista+ "PNG-compressed icon" format) using only printf/cat -- no image
# library required. Windows decodes the embedded PNG for its true dimensions
# and auto-scales it for smaller UI (taskbar, title bar, etc.).
#
# Usage: wrap_png_as_ico.sh <source.png> <dest.ico>
set -euo pipefail

src="${1:?Usage: $0 <source.png> <dest.ico>}"
dst="${2:?Usage: $0 <source.png> <dest.ico>}"
[[ -f "$src" ]] || { echo "ERROR: source PNG not found: $src" >&2; exit 1; }

size=$(wc -c < "$src" | tr -d ' ')

# u32 little-endian byte count, as four \xHH escapes for printf.
b0=$(( size & 0xFF )); b1=$(( (size >> 8) & 0xFF ))
b2=$(( (size >> 16) & 0xFF )); b3=$(( (size >> 24) & 0xFF ))
size_le=$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' "$b0" "$b1" "$b2" "$b3")

{
  # ICONDIR: reserved=0 (u16), type=1 (u16, "icon"), count=1 (u16)
  printf '\x00\x00\x01\x00\x01\x00'
  # ICONDIRENTRY: width=0(=256) height=0(=256) colorCount=0 reserved=0
  #   planes=1 (u16) bitCount=32 (u16)
  printf '\x00\x00\x00\x00\x01\x00\x20\x00'
  # bytesInRes (u32 LE) -- the embedded PNG's byte length
  printf -- "$size_le"
  # imageOffset (u32 LE) -- always 22: a fixed 6-byte ICONDIR + one 16-byte entry
  printf '\x16\x00\x00\x00'
  cat "$src"
} > "$dst"

echo "wrote $dst ($(wc -c < "$dst" | tr -d ' ') bytes)"

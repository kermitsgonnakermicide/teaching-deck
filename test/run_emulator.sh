#!/usr/bin/env bash
# Emulator test: build the deck, boot it in PPSSPP (headless-ish on Xvfb),
# and collect proof: the on-PSP status log (ms0:/teachingdeck.log) + screenshot.
#
# Usage:   test/run_emulator.sh [path-to-EBOOT.PBP]
# Output:  docs/emulator-* artifacts (screenshot + deck log)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EBOOT="${1:-$ROOT/psp-app/EBOOT.PBP}"
WORK="/tmp/ppsspp-emu-test"
MEMSTICK="$WORK/.config/ppsspp"
FBS_DISPLAY=":99"

echo "==> building deck (if needed)"
make -q -C "$ROOT/psp-app" || make -C "$ROOT/psp-app"

echo "==> preparing virtual memstick"
rm -rf "$WORK"
mkdir -p "$MEMSTICK/PSP/GAME/teachingdeck" "$MEMSTICK/PSP/SAVEDATA" "$MEMSTICK/PSP/SYSTEM"
cp "$EBOOT" "$MEMSTICK/PSP/GAME/teachingdeck/"

echo "==> starting Xvfb on $FBS_DISPLAY"
Xvfb $FBS_DISPLAY -screen 0 1024x768x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1.5

echo "==> launching PPSSPP"
if [ -z "${PPSSPP:-}" ]; then
    if [ -x /tmp/squashfs-root/bin/PPSSPPSDL ]; then
        PPSSPP=/tmp/squashfs-root/bin/PPSSPPSDL
    else
        PPSSPP="$(command -v PPSSPPSDL || command -v PPSSPP || true)"
    fi
fi
[ -n "$PPSSPP" ] && [ -x "$PPSSPP" ] || { echo "!! PPSSPP not found (set PPSSPP=... or APPIMAGE_EXTRACT)"; exit 1; }
HOME="$WORK" DISPLAY=$FBS_DISPLAY SDL_VIDEODRIVER=x11 \
  "$PPSSPP" --graphics=software --windowed --log="$WORK/ppsspp.log" \
  "$EBOOT" >"$WORK/ppsspp-stdout.log" 2>&1 &
EMU_PID=$!
sleep 18

echo "==> capturing deck status log"
DECK_LOG="$MEMSTICK/teachingdeck.log"
if [ -f "$DECK_LOG" ]; then
    cat "$DECK_LOG"
    cp "$DECK_LOG" "$ROOT/docs/emulator-deck.log"
    echo "==> deck log saved to docs/emulator-deck.log"
else
    echo "!! no deck log found (app may not have started)"
fi

echo "==> screenshotting the emulator window"
WIN="$(DISPLAY=$FBS_DISPLAY xwininfo -root -tree 2>/dev/null \
       | grep -oE '0x[0-9a-f]+ "Teaching Deck"' | awk '{print $1}' | head -1)"
if [ -n "$WIN" ]; then
    DISPLAY=$FBS_DISPLAY xwd -id "$WIN" -silent -out "$WORK/deck.xwd"
    ffmpeg -y -v error -i "$WORK/deck.xwd" "$ROOT/docs/emulator-boot.png"
    echo "==> screenshot saved to docs/emulator-boot.png"
else
    echo "!! no Teaching Deck window found"
fi

echo "==> shutting down"
kill "$EMU_PID" 2>/dev/null || true
sleep 2
kill "$XVFB_PID" 2>/dev/null || true
echo "==> done"
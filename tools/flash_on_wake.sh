#!/bin/bash
# Flash the board as soon as its USB-Serial-JTAG port re-enumerates.
#
# The firmware deep sleeps between refreshes, so /dev/ttyACM0 only exists for
# the 15-30 s of a wake - long enough to start a flash, too short to be sure of
# starting one by hand. The moment a flash is started, esptool resets the chip
# into the ROM bootloader, which stays enumerated for as long as the write
# takes, so the window no longer matters.
#
# A monitor reading the same tty would consume esptool's responses, so the
# capture watcher is stopped first, and restarted after the reset.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${GLASS_TTY:-/dev/ttyACM0}"
ELF="$ROOT/firmware/build/weather_glass.elf"
LOG="${GLASS_FLASH_LOG:-/tmp/glass_flash.log}"
WAKE_LOG="${GLASS_CAPTURE_LOG:-/tmp/glass_wake.log}"

[ -f "$ELF" ] || { echo "missing $ELF" >&2; exit 1; }

log() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }

log "waiting for $PORT"
while [ ! -e "$PORT" ]; do sleep 0.05; done
log "port appeared"
sleep 0.3

if pkill -f 'tools/capture_wake.py' 2>/dev/null; then
    log "stopped capture watcher"
    sleep 0.2
fi

log "flashing $(basename "$ELF")"
cd "$ROOT/firmware" || exit 1
espflash flash -c esp32s3 -p "$PORT" -B 460800 -m dio -f 80mhz -s 16mb \
    build/weather_glass.elf >>"$LOG" 2>&1
rc=$?
log "espflash rc=$rc"
if [ "$rc" -ne 0 ]; then
    log "flash failed; not retrying automatically"
    exit "$rc"
fi

# The new image starts on its own. Capture its log; with a USB host attached it
# is expected to keep the port up rather than sleep after one cycle.
sleep 1
nohup setsid "$ROOT/.venv/bin/python" "$ROOT/tools/capture_wake.py" \
    "$WAKE_LOG" 86400 >>"$LOG" 2>&1 < /dev/null &
log "capture started pid $!"

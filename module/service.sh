#!/system/bin/sh
# TouchKiller supervisor - late_start service (non-blocking, runs in parallel
# with boot). The daemon itself does nothing until the enable flag is set, so
# there is no need for the blocking post-fs-data stage.

MODDIR=${0%/*}
LOGDIR=/data/adb/touchkiller
BIN="$MODDIR/touchkillerd"

mkdir -p "$LOGDIR"

# Wait for the data partition / boot to settle before touching
# /data/local/tmp, which may not exist yet on first boot after a wipe.
resetprop -w sys.boot_completed 0 >/dev/null 2>&1
sleep 2
mkdir -p /data/local/tmp

# Recreate the flags if /data/local/tmp was cleared, so the interface is
# always present and writable by `adb shell` (uid 2000) without su.
for f in /data/local/tmp/touchkill_enable /data/local/tmp/touchkill_auto; do
  [ -f "$f" ] || echo 0 > "$f"
  chmod 0666 "$f" 2>/dev/null
  chown 2000:2000 "$f" 2>/dev/null
done

echo "$(date '+%Y-%m-%d %H:%M:%S') supervisor: starting" >> "$LOGDIR/supervisor.log"

# Respawn loop: if the daemon is killed (OOM, crash, manual kill) it comes
# back within ~2s and re-reads its config. The daemon holds an flock on its
# pid file, so a duplicate instance exits on its own.
while true; do
  if ! pgrep -f "$BIN" >/dev/null 2>&1; then
    "$BIN" -f >> "$LOGDIR/supervisor.log" 2>&1 &
  fi
  sleep 2
done &

#!/system/bin/sh
# TouchKiller v1 - runs before system_server starts.
# chmod 000 on the touch event node prevents system_server from
# ever opening it — works on all devices, no driver knowledge needed.

LOG=/data/local/tmp/touchkiller.log
echo "TouchKiller: $(date)" > "$LOG"

FOUND=0
for dev in /dev/input/event*; do
  if getevent -lp "$dev" 2>/dev/null | grep -q "ABS_MT_POSITION_X"; then
    chmod 000 "$dev"
    echo "Disabled: $dev" >> "$LOG"
    FOUND=1
  fi
done

[ $FOUND -eq 0 ] && echo "ERROR: no touch device found" >> "$LOG"
exit 0

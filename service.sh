#!/system/bin/sh
# Wait for boot to fully complete (input devices populated)
while [ "$(getprop sys.boot_completed)" != "1" ]; do
  sleep 1
done
sleep 3

LOG=/cache/disabletouch.log
echo "disabletouch: starting $(date)" > "$LOG"

# Auto-detect touch device(s) by capability and kill read perms.
# Touchscreens report ABS_MT_POSITION (multitouch) or BTN_TOUCH.
for dev in /dev/input/event*; do
  if getevent -lp "$dev" 2>/dev/null | grep -qE "ABS_MT_POSITION_X|ABS_MT_POSITION|BTN_TOUCH"; then
    chmod 000 "$dev"
    echo "disabled: $dev" >> "$LOG"
  fi
done

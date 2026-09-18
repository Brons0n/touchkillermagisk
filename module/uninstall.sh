#!/system/bin/sh
# Runs when the module is removed. Make absolutely sure we do not leave a
# device with a dead touchscreen.
echo 0 > /data/local/tmp/touchkill_enable 2>/dev/null
echo 0 > /data/local/tmp/touchkill_auto 2>/dev/null
pkill -f /data/adb/modules/touchkiller/touchkillerd 2>/dev/null
rm -f /data/local/tmp/touchkill_status
rm -rf /data/adb/touchkiller

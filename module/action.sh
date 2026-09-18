#!/system/bin/sh
# Magisk app "Action" button: toggle the manual flag.
MODDIR=${0%/*}
EN=/data/local/tmp/touchkill_enable
if [ "$(cat $EN 2>/dev/null)" = "1" ]; then
  "$MODDIR/touchkill" off
else
  "$MODDIR/touchkill" on
fi
"$MODDIR/touchkill" status

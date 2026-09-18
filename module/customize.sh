#!/system/bin/sh
# TouchKiller installer - runs inside Magisk's install environment.
# Available here: $MODPATH $ARCH $API $IS64BIT $MAGISK_VER_CODE $BOOTMODE

ui_print " "
ui_print "  TouchKiller $(grep_prop version "$MODPATH/module.prop")"
ui_print "  ------------------------------------------"
ui_print "  device : $(getprop ro.product.manufacturer) $(getprop ro.product.model)"
ui_print "  android: $(getprop ro.build.version.release) (API $API)"
ui_print "  arch   : $ARCH"
ui_print "  magisk : $MAGISK_VER_CODE"
ui_print " "

if [ "$API" -lt 28 ]; then
  ui_print "  ! Android 9 (API 28) or newer is required."
  abort   "  ! Aborting."
fi

# --- pick the right prebuilt -----------------------------------------------
case "$ARCH" in
  arm64) ABI=arm64-v8a ;;
  arm)   ABI=armeabi-v7a ;;
  *)
    ui_print "  ! Unsupported arch: $ARCH"
    ui_print "  ! Only arm64-v8a and armeabi-v7a binaries are bundled."
    abort   "  ! Aborting."
    ;;
esac

BIN="$MODPATH/bin/$ABI/touchkillerd"
[ -f "$BIN" ] || abort "  ! Missing bundled binary for $ABI"

ui_print "- Installing $ABI daemon"
rm -f "$MODPATH/touchkillerd"
cp -f "$BIN" "$MODPATH/touchkillerd"
rm -rf "$MODPATH/bin"            # drop the ABI we do not need

# --- runtime state dir ------------------------------------------------------
mkdir -p /data/adb/touchkiller
if [ ! -f /data/adb/touchkiller/config ]; then
  cat > /data/adb/touchkiller/config <<'EOF'
# TouchKiller daemon config. Edit, then: su -c 'killall touchkillerd'
# (service.sh's supervisor restarts it within ~2s and re-reads this file.)

# Fallback poll interval in ms. inotify handles manual toggles instantly;
# this is also the auto-detect scan cadence.
poll_ms=1000

# Value of auto mode when /data/local/tmp/touchkill_auto does not exist.
# 0 = manual only (default, safest). 1 = auto-detect on by default.
auto_default=0

# How often to re-scan for newly appeared touch nodes while grabbed (ms).
rescan_ms=5000

# Also grab stylus/digitizer nodes (Galaxy Note S Pen etc).
grab_stylus=1

# Grab nodes whose bustype is BUS_VIRTUAL. Off by default because scrcpy's
# --uhid / --otg modes and other injectors create virtual touch nodes that
# must NOT be grabbed.
allow_virtual=0

verbose=0
EOF
fi
chmod 0600 /data/adb/touchkiller/config

# --- control flags ----------------------------------------------------------
# Start disabled so a flash never leaves a device with dead touch.
for f in /data/local/tmp/touchkill_enable /data/local/tmp/touchkill_auto; do
  [ -f "$f" ] || echo 0 > "$f"
  chmod 0666 "$f"
  chown 2000:2000 "$f" 2>/dev/null
done
# 0666 so an unprivileged `adb shell` (uid 2000) can toggle without su.

# --- permissions ------------------------------------------------------------
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/touchkillerd" 0 0 0755
set_perm "$MODPATH/service.sh"   0 0 0755
set_perm "$MODPATH/touchkill"    0 0 0755
[ -f "$MODPATH/action.sh" ] && set_perm "$MODPATH/action.sh" 0 0 0755

ui_print " "
ui_print "- Detected input devices:"
"$MODPATH/touchkillerd" -s 2>&1 | while read -r line; do ui_print "    $line"; done

ui_print " "
ui_print "- Installed. Reboot, then:"
ui_print "    adb shell 'echo 1 > /data/local/tmp/touchkill_enable'   # touch OFF"
ui_print "    adb shell 'echo 0 > /data/local/tmp/touchkill_enable'   # touch ON"
ui_print " "

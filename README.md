# TouchKiller

A Magisk module that disables the physical touchscreen on Android devices. Useful for phones running scrcpy where accidental touch input is a problem.

## What it does

Kills the touchscreen at boot by removing read access to the input event node before system_server starts. scrcpy and ADB input injection are unaffected since they bypass the raw input device entirely.

## Requirements

- Rooted Android device with Magisk installed
- scrcpy or ADB for device control after touch is disabled

## Tested devices

- Pixel 5 (redfin)
- Pixel 6 Pro (raven)
- Samsung Note 8
- OnePlus 8T (kebab)

## Install

1. Download the latest zip from [Releases](../../releases)
2. Open Magisk → Modules → Install from storage
3. Select the zip and reboot

## Uninstall

Remove the module in Magisk and reboot. Touch is restored automatically — nothing is permanently modified.

## Debug

If touch isn't disabled after install, check the log:

```bash
adb shell su -c 'cat /data/local/tmp/touchkiller.log'
```

## How it works

`post-fs-data.sh` runs during early boot before system_server starts. It finds the touchscreen input event node by detecting `ABS_MT_POSITION_X` capability and sets permissions to `000`. Since system_server never gets to open the node, touch is dead for the entire session. On reboot without the module, ueventd restores default permissions automatically.

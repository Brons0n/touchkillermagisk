# TouchKiller

A Magisk module that makes the **physical touchscreen dead on demand**, so a
remote session (scrcpy, Laixi, any InputManager-based driver) can drive a
phone-farm device without a stray finger on the glass fighting the automation.

No `/system` modification. One statically-linked native daemon plus shell
scripts, controlled by a flag file that an unprivileged `adb shell` can write.

> **v2 replaces the v1 `chmod 000` approach.** v1 set the touch event node to
> mode `000` in `post-fs-data.sh` so `system_server` could never open it. That
> works, but it is boot-time and all-or-nothing: changing state meant a reboot,
> and a device left in that state had no runtime way back. v2 keeps the same
> "don't touch the driver" principle but holds an `EVIOCGRAB` instead, which is
> togglable at runtime in ~80 ms, reversible without a reboot, and releases
> automatically if the daemon ever dies. The capability-based node detection is
> carried over from v1 and extended (stylus nodes, multiple nodes, virtual-node
> exclusion).
>
> Migrating from v1: flash v2 over it, then remove the old module. The module
> id is unchanged (`touchkiller`), so Magisk will replace it in place.

---

## How it works

`touchkillerd` holds an `EVIOCGRAB` ioctl on every touchscreen / stylus evdev
node. A grab is exclusive: while it is held the kernel delivers those events
to nobody else — including `system_server`'s `InputReader` — so the panel is
electrically alive but functionally dead. Dropping the grab (or closing the
fd) restores delivery instantly, mid-stream, with no driver reset.

Remote control still works because scrcpy's server (and Laixi's, which is a
repackaged scrcpy server — see below) injects through
`InputManager.injectInputEvent` at the framework level. That path never
touches the evdev node, so it is unaffected by the grab. **Verified on a
Pixel 6 Pro**: with the panel grabbed, an injected tap still changed the
screen.

### Which nodes get grabbed

Capability match, never a name match — vendor names are useless as a key
(`sec_touchscreen` is both the Samsung *and* the Pixel 6 name; OnePlus calls
it `touchpanel`):

| Class | Rule |
|---|---|
| `multitouch` | `ABS_MT_POSITION_X` **and** `ABS_MT_POSITION_Y` |
| `single-touch` | `BTN_TOUCH` + `ABS_X`/`ABS_Y`, and no `EV_REL` (excludes mice/trackpads) |
| `stylus` | `BTN_TOOL_PEN` + `ABS_X`/`ABS_Y` (Galaxy Note S Pen ships as its own node) |

All matching nodes are grabbed, not just the first one.

Two exclusions exist so the module never shoots the automation it protects:

* nodes whose name contains `scrcpy` / `uhid` / `uinput` / `virtual …`
* nodes with bustype `BUS_VIRTUAL` (override with `allow_virtual=1`)

These matter because `scrcpy --uhid` and `scrcpy --otg` create *new* virtual
touch devices that advertise the same `ABS_MT_*` capabilities. Grabbing one
of those would kill remote input.

Run the classifier yourself at any time:

```
adb shell su -c '/data/adb/modules/touchkiller/touchkillerd -s'
```

```
NODE                 NAME                               CLASS         BUS
/dev/input/event1    touchpanel                         multitouch    0x00
/dev/input/event5    gpio-keys                          none          0x19
```

---

## Install

```bash
# build (needs the Android NDK; r27c was used for the shipped binaries)
NDK=/path/to/android-ndk-r27c ./build.sh
# -> out/touchkiller-v1.0.0.zip

# flash
adb push out/touchkiller-v1.0.0.zip /data/local/tmp/
adb shell su -c 'magisk --install-module /data/local/tmp/touchkiller.zip'
adb reboot
```

Or flash `out/touchkiller-v1.0.0.zip` from the Magisk app (Modules → Install
from storage). The installer prints the detected input devices, so you can
confirm the right node was found before you ever enable it.

Both `arm64-v8a` and `armeabi-v7a` binaries are bundled; `customize.sh` keeps
the one matching `$ARCH` and deletes the other. Requires Android 9 (API 28)
or newer and Magisk 20.4+.

---

## Manual control (the primary interface)

Two flag files in `/data/local/tmp`, mode `0666`, so **`adb shell` can toggle
them with no `su`**:

```bash
# touch OFF (physical panel dead)
adb shell 'echo 1 > /data/local/tmp/touchkill_enable'

# touch ON  (normal)
adb shell 'echo 0 > /data/local/tmp/touchkill_enable'

# read back what the daemon actually did
adb shell cat /data/local/tmp/touchkill_status
```

```json
{"grabbed":1,"nodes":1,"manual":1,"auto":0,"session_detected":0,"pid":2841,
 "devices":[{"node":"/dev/input/event1","name":"touchpanel"}]}
```

`touchkill_status` is written by the daemon after every state change, and
replaced by `rename()`, so a reader never sees a half-written file. Poll it
if your farm controller needs confirmation rather than fire-and-forget.

**Latency: 66–107 ms** measured end to end on a Pixel 6 Pro (flag write →
grab actually held). The daemon watches `/data/local/tmp` with `inotify`; the
1 s poll is only a fallback.

### The blocked state survives reboot

The flags live in `/data/local/tmp`, which is on `/data` — not a tmpfs. A
device left with `touchkill_enable=1` **boots back up with touch still
dead**, because `service.sh` recreates the flags only when they are missing
and never resets them. That is deliberate (a farm unit stays locked through a
crash-reboot) but it means adb is your only way back in. Verified on a Pixel
6 Pro: flag `1` before reboot, `{"grabbed":1,"manual":1,...}` and a fresh
`GRABBED` log line after.

There is a **~22 s window during boot where touch is live** (kernel boot
21:52:10 → grab 21:52:32, measured). `service.sh` is a `late_start` service
that waits on `sys.boot_completed` plus 2 s. Closing that window means moving
the grab to `post-fs-data.sh`, which is a *blocking* boot stage with a 40 s
cap — not worth the boot-hang risk unless you specifically need it.

`uninstall.sh` zeroes both flags, so removing the module always clears the
state.

### Race-free writing

`echo 1 > file` truncates before it writes, so a reader can catch a
zero-length file. The daemon treats an empty or unparseable flag as *"no
information, keep the current state"*, so that window can never flip touch
the wrong way — the worst case is one extra poll of delay.

If you want zero window at all, write-then-rename, which the daemon picks up
via `IN_MOVED_TO`:

```bash
adb shell 'printf 1 > /data/local/tmp/.tk && mv /data/local/tmp/.tk /data/local/tmp/touchkill_enable'
```

The bundled CLI already does this:

```bash
adb shell su -c '/data/adb/modules/touchkiller/touchkill on'       # block
adb shell su -c '/data/adb/modules/touchkiller/touchkill off'      # allow
adb shell su -c '/data/adb/modules/touchkiller/touchkill status'
adb shell su -c '/data/adb/modules/touchkiller/touchkill scan'     # classification table
adb shell su -c '/data/adb/modules/touchkiller/touchkill log 50'
adb shell su -c '/data/adb/modules/touchkiller/touchkill restart'
```

The Magisk app's module **Action** button toggles the same flag.

Accepted flag values: `1/0`, `on/off`, `true/false`, `yes/no`.

---

## Auto-detect (phase 2) — shipped, off by default

```bash
adb shell 'echo 1 > /data/local/tmp/touchkill_auto'   # enable auto mode
adb shell 'echo 0 > /data/local/tmp/touchkill_auto'   # disable
```

Effective state is `manual OR (auto AND session_detected)`, so the manual
flag always wins as a hard override and the two never fight.

### What the signal actually is (observed, not guessed)

Both tools were run against the live test devices and inspected with
`ps -A -o ARGS`, `/proc/<pid>/cmdline` and `/proc/net/unix`:

**Laixi** (`youhu.laixijs`, v1.0.7.7) does **not** have its own protocol. It
ships `/data/local/tmp/laixi.jar`, which is a repackaged **scrcpy server
v2.2**, and launches it as:

```
sh -c CLASSPATH=/data/local/tmp/laixi.jar app_process / \
  com.genymobile.scrcpy.Server v2.2 video_codec=h264 video_bit_rate=2000000 \
  audio=false max_fps=10 max_size=360 ... control=true stay_awake=true cleanup=true
```

socket: `@scrcpy`

**scrcpy 4.1** (upstream) pushes `/data/local/tmp/scrcpy-server.jar` and runs:

```
sh -c CLASSPATH=/data/local/tmp/scrcpy-server.jar app_process / \
  com.genymobile.scrcpy.Server 4.1 scid=53ace3af log_level=info ...
```

socket: `@scrcpy_<scid>`

So there is **one signal covering both**: a live process whose cmdline
contains `com.genymobile.scrcpy.Server`. The abstract socket (`@scrcpy`
prefix, matching both the v2 bare name and the v3+/v4 `_<scid>` form) is
checked as a cheaper secondary. The process check is authoritative — the
server exits when its client disconnects.

Inspect what the detector sees right now:

```bash
adb shell su -c '/data/adb/modules/touchkiller/touchkillerd -d'
```

```
marker: "com.genymobile.scrcpy.Server"
-- matching processes --
  pid 2169    app_process / com.genymobile.scrcpy.Server v2.2 video_codec=h264 ...   <- Laixi
  pid 9397    app_process / com.genymobile.scrcpy.Server 4.1 scid=53ace3af ...       <- scrcpy
-- matching abstract sockets --
  @scrcpy
  @scrcpy_53ace3af
verdict: session_detected=1 scrcpy-server pid 2169
```

### Limits you should know before turning it on

* **Granularity is "attached", not "actively touching".** Laixi starts the
  server when the PC client attaches the device and keeps it alive for the
  whole session, including while the device is only a thumbnail in the grid.
  Observed on the fleet: grid devices get `bit_rate=2000000 max_fps=10
  max_size=360`, the focused device gets `bit_rate=4000000 max_fps=25
  max_size=1080` — same process name either way, so the two cannot be told
  apart without keying on stream parameters, which would be fragile. If
  "Laixi is attached" is close enough to "automation is running" for your
  farm, auto mode is fine; if you need per-action precision, use the manual
  flag.
* **Laixi reconnects instantly.** Killing its server process does not end a
  session; the PC client respawns it within ~1 s. Auto mode tracked that
  correctly in testing (`session ENDED` → `RELEASED` → `session STARTED` →
  `GRABBED` inside 1 s), but it means you cannot use "kill the server" as a
  way to re-enable touch. Use the manual flag.
* **Any scrcpy-derived tool trips it.** That is usually what you want in a
  farm, but a developer casually mirroring a device will also get a dead
  panel while auto mode is on.
* `auto_default=1` in the config makes auto mode the boot default when the
  flag file is missing.

---

## Config

`/data/adb/touchkiller/config` (root-only, re-read on daemon restart):

```ini
poll_ms=1000        # fallback poll + auto-detect scan cadence
auto_default=0      # auto mode when touchkill_auto is absent
rescan_ms=5000      # re-scan for new touch nodes while grabbed
grab_stylus=1       # also grab pen/digitizer nodes
allow_virtual=0     # grab BUS_VIRTUAL nodes (leave off: see exclusions)
verbose=0
```

Apply with `touchkill restart` (the supervisor respawns within ~2 s).

---

## Reliability

* **Single instance.** The daemon holds an `flock` on
  `/data/adb/touchkiller/touchkillerd.pid`; a second copy exits immediately.
* **Respawn.** `service.sh` runs a 2-second supervisor loop that restarts the
  daemon if it is killed (OOM, crash, manual `kill`).
* **Fail-open on install and uninstall.** `customize.sh` writes `0` to both
  flags, so flashing never leaves a device with a dead panel; `uninstall.sh`
  clears the flags and kills the daemon before the module goes away.
* **Fail-open on death.** Grabs are held by open fds, so if the daemon dies
  for any reason the kernel closes them and touch comes back on its own.
* **Boot stage.** `service.sh` is `late_start` (non-blocking). The daemon
  does nothing until a flag is set, so there is no reason to use the blocking
  `post-fs-data` stage and risk a boot hang.
* **Logs.** `/data/adb/touchkiller/touchkillerd.log`, rotated at 512 KB.
  Supervisor output: `/data/adb/touchkiller/supervisor.log`.

---

## SELinux

Magisk runs module scripts, and therefore the daemon, as `u:r:magisk:s0`,
which Magisk's patched policy leaves effectively unconfined — the
`EVIOCGRAB` ioctl on `/dev/input/event*` is permitted with no extra work.
**All three test devices grabbed successfully with SELinux in the state
listed below; no `avc` denials were produced.**

`sepolicy.rule` ships anyway, as belt-and-braces for ROMs or kernels with a
tighter policy:

```
allow magisk input_device chr_file { open read write ioctl getattr }
allow magisk shell_data_file file { open read write create unlink getattr setattr rename }
allow magisk proc_net file { open read getattr }
```

If a grab fails, the daemon logs `EVIOCGRAB … failed: Permission denied
(check SELinux: dmesg | grep avc)`. Then:

```bash
adb shell su -c 'dmesg | grep avc | grep -i input'
adb shell su -c 'getenforce'
```

Per-vendor notes from the test fleet are in the results table below.

---

## Troubleshooting

| Symptom | Check |
|---|---|
| Nothing happens on toggle | `touchkill status` — is the daemon running? `touchkill log 50` |
| `no touchscreen node could be grabbed` | `touchkill scan` — does *any* row classify as `multitouch`/`single-touch`? If not, the ROM/kernel exposes no touchscreen evdev node (see the Note 8 result below) |
| `EVIOCGRAB … Device or resource busy` | Something else already grabs the node — another instance, or another input-blocking module |
| `EVIOCGRAB … Permission denied` | SELinux. `dmesg \| grep avc`. Confirm `sepolicy.rule` was applied (Magisk logs "Installing custom sepolicy rules") |
| Touch stays dead after disabling | Confirm the flag: `cat /data/local/tmp/touchkill_enable`. Then `touchkill restart` — dying releases every grab |
| scrcpy/Laixi input stopped working too | You are probably grabbing a virtual node created by `--uhid`/`--otg`. Check `touchkill scan` for a synthetic row and keep `allow_virtual=0` |
| Flags missing after a `/data/local/tmp` wipe | `service.sh` recreates them on boot; or `touchkill on`/`off` recreates on demand |

**Emergency restore** (touch dead, only adb available):

```bash
adb shell 'echo 0 > /data/local/tmp/touchkill_enable; echo 0 > /data/local/tmp/touchkill_auto'
# or, blunt:
adb shell su -c 'pkill -f touchkillerd'
```

Removing the module in the Magisk app also restores touch (via
`uninstall.sh`) even before the reboot.

---

## Test results

See [TESTING.md](TESTING.md) for the per-device results, the method used to
prove the grab (including a `uinput`-based end-to-end event-delivery test),
and the per-vendor SELinux observations.

---

## Layout

```
build.sh                      cross-compile both ABIs + package the zip
src/touchkillerd.c            the daemon
tests/tkuinput.c              test helper: synthetic touch panel via uinput
tests/tkgrabtest.c            test helper: EVIOCGRAB contention probe
module/
  module.prop
  customize.sh                installer: ABI pick, config seed, fail-open flags
  service.sh                  late_start supervisor
  touchkill                   control CLI
  action.sh                   Magisk app Action button
  uninstall.sh                fail-open cleanup
  sepolicy.rule
  skip_mount                  ships no /system overlay
  bin/{arm64-v8a,armeabi-v7a}/touchkillerd
  META-INF/com/google/android/{update-binary,updater-script}
```

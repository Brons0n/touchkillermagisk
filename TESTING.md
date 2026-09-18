# TouchKiller — test results

All results below were produced on the live fleet, not inferred. Module
version `v1.0.0`, Magisk `30.7` on every device, arm64-v8a binary.

## Fleet

| Device | Model | ROM | Android | SELinux | Touch node found |
|---|---|---|---|---|---|
| Pixel | Pixel 6 Pro (raven) | LineageOS 20 (`lineage_raven-userdebug` TQ3A.230901.001) | 13 / API 33 | **Enforcing** | `event2` `sec_touchscreen` (multitouch, bus 0x1c) |
| OnePlus | OnePlus 8T KB2005 (kebab) | LineageOS 20 (`lineage_kebab-userdebug` TQ3A.230901.001) | 13 / API 33 | **Enforcing** | `event1` `touchpanel` (multitouch, bus 0x00) |
| Samsung | Galaxy Note 8 SM-N950F (exynos8895) | third-party Vietnamese tool ROM | 10 / API 29 | **Permissive** | **none — see below** |

Note the naming trap the capability matcher exists to avoid: the **Pixel**'s
touchscreen is called `sec_touchscreen`, the same name a Samsung panel uses,
while the actual Samsung in this fleet has no node by that name at all.

---

## How the grab was proven

A human finger was not available, so two mechanisms were used, both of which
are stronger evidence than a visual check.

### 1. Exclusivity probe (`tests/tkgrabtest.c`) — run on all three devices

`EVIOCGRAB` is exclusive. A second process attempting it returns `EBUSY`
exactly when, and only when, the kernel has stopped delivering that node's
events to every other reader — including `system_server`'s `InputReader`,
which is what makes the panel dead to the UI.

### 2. End-to-end event-delivery test (`tests/tkuinput.c`) — Pixel 6 Pro

`tkuinput` creates a synthetic multitouch panel via `/dev/uinput` with the
same `ABS_MT_POSITION_X/Y` capabilities and a non-virtual bustype, so it
classifies exactly like a real panel, then injects one tap per second.
`getevent` ran as an independent second reader on the resulting node.

`BTN_TOUCH` down events seen by that reader (`0001 014a 00000001`):

```
[  4022.320916]   tap    <- free
[  4023.321939]   tap
[  4024.322873]   tap
[  4025.325778]   tap
                        <- touchkill_enable = 1 at 4026
        (nothing)       <- 6 s of injected taps, zero delivered
                        <- touchkill_enable = 0 at 4031
[  4031.331039]   tap    <- free again, first tap after release
[  4032.331698]   tap
[  4033.332419]   tap
[  4034.333916]   tap
```

Not one event leaked through the grab, and delivery resumed on the very next
injected tap after release. Matching daemon log:

```
21:44:56 GRABBED  /dev/input/event4 ("tk_probe_panel",  multitouch, bus=0x18)
21:44:56 GRABBED  /dev/input/event2 ("sec_touchscreen", multitouch, bus=0x1c)
21:45:01 RELEASED /dev/input/event4 ("tk_probe_panel")
21:45:01 RELEASED /dev/input/event2 ("sec_touchscreen")
```

### 3. Remote control is unaffected — Pixel 6 Pro

With `sec_touchscreen` grabbed, a framework-level tap (`input tap 540 900`,
the same `InputManager.injectInputEvent` path the scrcpy server uses for
control) was injected and the screen content changed — verified by comparing
`screencap` MD5s before and after:

```
8771feed563df5db6e92590200656fdb  before
4879646c8a63d6a62ef66df0a2e1ed2e  after
```

So grabbing the panel does not cost you remote input.

---

## Per-device results

### Pixel 6 Pro — PASS

```
model : Pixel 6 Pro  android 13  selinux Enforcing
daemon:  3386 touchkillerd -f                       <- started by service.sh after reboot
-- scan --
/dev/input/event2    sec_touchscreen      multitouch    0x1c
-- before enable --
/dev/input/event2: GRAB_OK
-- while enabled --
{"grabbed":1,"nodes":1,"manual":1,"auto":0,"session_detected":0,"pid":3386,
 "devices":[{"node":"/dev/input/event2","name":"sec_touchscreen"}]}
/dev/input/event2: GRAB_DENIED errno=16 (Device or resource busy)
-- after disable --
{"grabbed":0,"nodes":0,...}
/dev/input/event2: GRAB_OK
```

* Touch dead when grabbed: **yes** (both probes, section 1 and 2)
* Fully restored on release: **yes**, immediately
* Survives reboot: **yes** — daemon auto-started, fresh pid, flags default to 0
* Auto-detect follows a session: **yes** (`auto on` → `session_detected:1` →
  grabbed; `auto off` → released)
* SELinux Enforcing: **no denials**. `dmesg | grep avc` shows only Magisk's
  own `init` → `/debug_ramdisk/.magisk/worker/...` `backuptool`/`blkid`
  getattr noise, unrelated to `input_device`.
* **Toggle latency measured here: 81 ms / 66 ms / 107 ms / 99 ms** over four
  alternating toggles, flag write → grab state actually observed.

#### Reboot with the block left on

Re-tested deliberately, because the flags are on `/data` and therefore
survive. Set `touchkill_enable=1`, rebooted, and the device came back with
touch still dead:

```
flag file : 1
status    : {"grabbed":1,"nodes":1,"manual":1,"auto":0,"session_detected":0,"pid":3392,
             "devices":[{"node":"/dev/input/event2","name":"sec_touchscreen"}]}
21:52:32 touchkillerd starting (pid 3392, poll=1000ms, auto_default=0, stylus=1)
21:52:32 GRABBED /dev/input/event2 ("sec_touchscreen", multitouch, bus=0x1c)
```

Kernel boot was `21:52:10`, the grab landed at `21:52:32` — a **~22 s window
after boot where the panel is live**, which is `service.sh` waiting on
`sys.boot_completed` plus its 2 s settle.

### OnePlus 8T KB2005 — PASS

```
model : KB2005  android 13  selinux Enforcing
daemon:  6755 touchkillerd -f
-- scan --
/dev/input/event1    touchpanel           multitouch    0x00
-- while enabled --
/dev/input/event1: GRAB_DENIED errno=16 (Device or resource busy)
-- after disable --
/dev/input/event1: GRAB_OK
```

* Touch dead when grabbed / restored on release: **yes**
* Survives reboot: **yes**
* Auto-detect: **yes**, and this device produced the cleanest live
  transition, by killing Laixi's server process out from under it:

```
22:45:30 auto-detect: session STARTED - scrcpy-server pid 8096
22:45:30 GRABBED  /dev/input/event1 ("touchpanel", multitouch, bus=0x00)
22:45:49 auto-detect: session ENDED
22:45:49 RELEASED /dev/input/event1 ("touchpanel")
22:45:49 auto-detect: session STARTED - abstract socket @scrcpy
22:45:49 GRABBED  /dev/input/event1 ("touchpanel", multitouch, bus=0x00)
```

Release and re-grab both happened inside the same second; the re-grab is
Laixi's PC client respawning its server ~1 s after the kill. Note the second
detection came from the socket check winning the race against the process
scan — both signals work.

* One quirk worth recording: `touchpanel` reports **bustype 0x00**, not a
  real bus id. Anything that filtered on bus type would have to be careful;
  the `BUS_VIRTUAL` (0x06) exclusion is unaffected. The device also exposes a
  second node, `touchpanel_kpd`, which correctly classifies as `none` (keys
  only, no ABS axes) and is left alone.
* SELinux Enforcing: **no denials** (same benign Magisk `init` noise only).

### Samsung Galaxy Note 8 SM-N950F — PASS mechanically, but **this unit has no touchscreen to kill**

```
model : SM-N950F  android 10  selinux Permissive
daemon:  5983 touchkillerd -f
-- scan --
/dev/input/event2    sec_virtual-e-pen    single-touch  0x18
/dev/input/event0    sec_e-pen            single-touch  0x18
-- while enabled --
{"grabbed":1,"nodes":2,...,"devices":[{"node":"/dev/input/event2","name":"sec_virtual-e-pen"},
                                      {"node":"/dev/input/event0","name":"sec_e-pen"}]}
/dev/input/event2: GRAB_DENIED errno=16 (Device or resource busy)
/dev/input/event0: GRAB_DENIED errno=16 (Device or resource busy)
-- after disable --
both: GRAB_OK
```

The module works: it found both digitizer nodes, grabbed both, released both,
and auto-started after reboot. But **there is no touchscreen node on this
device at all.** The S Pen digitizer is the only absolute-positioning input
present. Evidence:

* `/proc/bus/input/devices` lists 10 devices, none of them a touch panel:
  `sec_e-pen`, `sec_e-pen-pad`, `sec_virtual-e-pen`, `grip_sensor`,
  `ssp_context`, `hrm_sensor`, `Headset`, `gpio_keys`, `hall`, `certify_hall`.
* Android agrees — `dumpsys input` lists only
  `sec_e-pen-pad`, `sec_virtual-e-pen`, `Headset`, `gpio_keys`, `sec_e-pen`.
* `/sys/class/sec/` has a `sec_epen` entry but **no `tsp` entry**, which is
  where the Samsung touch driver registers. The driver never probed.

So on this unit the finger digitizer is already non-functional — a dead or
disconnected panel, or a kernel on this third-party ROM without a working TSP
driver — independent of TouchKiller. Enabling TouchKiller here blocks the S
Pen and nothing else. If you have a second Note 8 with working touch, that is
the one to re-test on; the module needs no change for it.

* SELinux is **Permissive** on this ROM, so it proves nothing about policy.
  The two Enforcing devices are the meaningful SELinux evidence.

---

## Per-vendor notes

* **Nothing vendor-specific was needed.** The same binary and the same
  capability rules worked on Qualcomm (OnePlus), Google Tensor (Pixel) and
  Exynos (Samsung), on Android 10 and 13, on two LineageOS builds and one
  third-party ROM.
* **SELinux:** zero `avc` denials touching `input_device` on either Enforcing
  device. Magisk runs the daemon as `u:r:magisk:s0`, which its patched policy
  leaves unconfined, so the `EVIOCGRAB` ioctl needs no extra permission.
  `sepolicy.rule` ships as insurance for stricter policies — notably stock
  Samsung One UI with Knox, which was **not** in this fleet (the Note 8 runs a
  third-party ROM, Permissive) and therefore **remains untested**. If a grab
  fails there, `dmesg | grep avc | grep input` is the first thing to check.
* **Static linking:** the daemon is `-static`, which removed any dependency
  on the ROM's linker across API 29–33. A side note found while building the
  *test* helpers: static binaries from NDK r27c can hit
  `executable's TLS segment is underaligned` on arm64 bionic depending on
  what gets linked in. `touchkillerd` does not trigger it and ran on all
  three devices, but if a future change to the source does, build it
  dynamically (`--target=aarch64-linux-android28` without `-static`).

## Not tested

* **armeabi-v7a.** The binary builds cleanly (`ELF 32-bit LSB executable,
  ARM, EABI5, statically linked`) but every device in the fleet is arm64, so
  it has never been executed.
* **Stock Samsung One UI / Knox**, stock Pixel firmware, and GrapheneOS. Both
  Android 13 devices here run LineageOS.
* **A physical finger.** Every result above is kernel-level (`EVIOCGRAB`
  contention) or event-level (`getevent` on an injected stream). Those are
  the mechanism that makes touch dead, so a manual confirmation is expected
  to agree — but it has not been done. Worth one hands-on check per device
  model before relying on it in production.

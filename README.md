# Fedora 44 on a MacBook Pro 13" 2016 (MacBookPro13,1)

Every fix and workaround needed to get **MacBookPro13,1** — the 13-inch 2016 MacBook
Pro *without* Touch Bar — fully working on Fedora, collected from an actual migration
off macOS.

**Test system**

| | |
|---|---|
| Model | MacBookPro13,1 (13" 2016, no Touch Bar) |
| CPU / GPU | Intel Skylake / Iris Graphics 540 |
| RAM / storage | 8 GB / 256 GB NVMe (soldered) |
| OS | Fedora 44 KDE |
| Kernel | `7.2.5-200.fc44.x86_64` |

Everything below was verified on that exact configuration. Kernel parameters and
module names are stable across recent Fedora releases, but the FaceTime HD driver
in particular tracks kernel internals closely — see the [camera](#camera-facetime-hd)
section.

---

## Status at a glance

| Component | Status | What it takes |
|---|---|---|
| Graphics (Iris 540, i915) | Works out of the box | — |
| Wi-Fi (BCM4350) | Works | [Harmless firmware errors](#wi-fi-bcm4350); one router needed investigation |
| Bluetooth | Works out of the box | — |
| Trackpad + gestures | Works out of the box | [Disable tap-to-click](#trackpad) if you want macOS-like behaviour |
| Keyboard, backlight, screen brightness | Works out of the box | — |
| NVMe, battery, USB-C | Works out of the box | — |
| **Suspend / resume** | **Works, drains ~4.1 W** | [Kernel parameters + boot-time script](#sleep-and-hibernation); S0ix is out of reach on this machine |
| **Hibernation** | **Works** | [Swap file, `resume=`, and a `brcmfmac` sleep hook](#hibernation--the-intended-solution). Resume on LUKS works; the passphrase is asked for at power-on |
| **Audio (Cirrus CS8409)** | **Fixed** | [Out-of-tree DKMS driver](#audio-cirrus-cs8409) |
| **Camera (FaceTime HD)** | **Fixed** | [Firmware extraction + DKMS driver + two source fixes](#camera-facetime-hd): a kernel 7.2 build error and missing buffer timestamps. Firefox needs [one pref](#6-firefox-notfounderror-with-a-camera-that-works) on top |
| Caps Lock as layout switch | Configurable | [keyd](#caps-lock-as-a-layout-switch) |
| Microphone | Partially working | Very low recording level — see [open issues](#open-issues) |

Idle temperature sits around **45 °C**, which is normal for Skylake. macOS runs
cooler because Apple parks cores more aggressively.

---

## Works out of the box

No configuration required: Intel Iris 540 graphics (`i915`), BCM4350 Wi-Fi,
Bluetooth, trackpad including gestures, keyboard, screen brightness and keyboard
backlight, NVMe storage, battery reporting, and USB-C.

---

## Sleep and hibernation

Suspend itself works — wake is instant and everything comes back. The problem is
power: `s2idle` on this machine drains the battery at roughly the rate of a running
system. The arrangement that solves it is **suspend-then-hibernate**: s2idle for short
breaks, hibernation after 15 minutes. Both halves work — the hibernate half took
a while to believe, for reasons kept in [open issues](#open-issues).

The suspend part is based on
[Dunedan/mbp-2016-linux issue #207](https://github.com/Dunedan/mbp-2016-linux/issues/207)
("Suspend working with Linux Mint 22.3 on 2016 Macbook (no touch bar)").

### 1. Kernel parameters

```bash
sudo grubby --update-kernel=ALL --args="button.lid_init_state=open nvme_core.default_ps_max_latency_us=0 nvme.noacpi=1 pci=noaer i915.enable_fbc=0 mem_sleep_default=s2idle"
```

`i915.enable_dc=0` and `i915.enable_psr=0` were part of the original recipe and have
since been dropped: removing them changed nothing about the drain, and suspend works
fine without them.

### 2. Boot-time fix script

`/usr/local/bin/mbp-suspend-fix.sh`:

```bash
#!/usr/bin/env bash
find /sys/devices/ -name d3cold_allowed -exec sh -c 'echo 0 > "$1" 2>/dev/null' _ {} \;
for device in LID0 XHC1 ARPT RP01 RP09 RP10; do
  if grep -q "$device.*enabled" /proc/acpi/wakeup; then
    echo "$device" > /proc/acpi/wakeup
  fi
done
```

```bash
sudo chmod +x /usr/local/bin/mbp-suspend-fix.sh
```

### 3. systemd unit

`/etc/systemd/system/mbp-suspend-fix.service`:

```ini
[Unit]
Description=MacBook Pro suspend fixes

[Service]
Type=oneshot
ExecStart=/usr/local/bin/mbp-suspend-fix.sh

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable mbp-suspend-fix.service
```

### 4. Verify

```bash
cat /sys/power/mem_sleep      # expected: [s2idle] deep
```

**Result:** instant wake, USB and Wi-Fi alive afterwards — but see the drain figure
below before relying on suspend alone.

---

### s2idle costs about 4.1 W

Measured with the lid closed over 10 hours, from full charge down to 9%. Battery
full capacity is 46.35 Wh (54.70 Wh by design, 15% wear, 518 cycles), so the drain
works out to roughly **4.1 W** — the consumption of a running machine with the
screen off. A healthy `s2idle` on Skylake sits in the tenths of a watt.

### Why: S0ix is unreachable

```bash
sudo mount -t debugfs none /sys/kernel/debug 2>/dev/null
sudo cat /sys/kernel/debug/pmc_core/slp_s0_residency_usec
sudo cat /sys/kernel/debug/pmc_core/package_cstate_show
```

```
slp_s0_residency_usec: 0

Package C2 : 189515513
Package C3 : 44193409333
Package C6 : 0
Package C7 : 0
Package C8 : 0
Package C9 : 0
Package C10 : 0
```

The package never goes deeper than C3 — neither while suspended nor while idle. S0ix
residency requires C7 or deeper, hence the zero counter.

The firmware does advertise S0:

```console
$ journalctl -k -b | grep -iE "ACPI.*[Ss]upports"
ACPI: PM: (supports S0 S3 S4 S5)
```

but there is no Low Power S0 Idle ACPI device:

```console
$ ls /sys/bus/acpi/devices/ | grep -i PNP0D80
(empty)
```

Without `PNP0D80` the kernel never enables the S0ix path at all.

**This is not caused by the parameters above.** Both suspects were removed and the
measurement repeated:

```bash
sudo grubby --update-kernel=ALL --remove-args="nvme_core.default_ps_max_latency_us=0"
sudo systemctl disable mbp-suspend-fix.service
sudo reboot
```

C6 and deeper stayed at zero. With no gain to show for it, both were put back:

```bash
sudo grubby --update-kernel=ALL --args="nvme_core.default_ps_max_latency_us=0"
sudo systemctl enable mbp-suspend-fix.service
```

### Why not `deep`

`echo deep | sudo tee /sys/power/mem_sleep` fails in two ways:

- The machine wakes itself about 10 seconds after going to sleep.
- After that wake Wi-Fi is dead, and neither reloading the driver nor a `reboot`
  brings it back — only a full power cycle does.
- The spurious wake cannot be traced: `/sys/power/pm_wakeup_irq` is empty, meaning
  the wakeup arrives over an ACPI GPE rather than an ordinary interrupt.

These are exactly the symptoms that make issue #207 recommend `s2idle`.

---

### Hibernation — the intended solution

> **This works.** A full cycle was confirmed on 2026-09-20: about 894 MB written,
> power cut, the passphrase asked for at the next power-on, and the session
> restored. Earlier revisions of this file said the opposite; how that mistake was
> made, and how to avoid repeating it, is in [open issues](#open-issues).

S4 is advertised by the firmware. The disk here is btrfs on LUKS with 213 GB free,
and swap is zram only, which cannot be hibernated to.

#### 1. Swap file

```bash
btrfs --version          # mkswapfile has been available since 6.1
df -h /

sudo btrfs filesystem mkswapfile --size 10g --uuid clear /swapfile
sudo swapon /swapfile
swapon --show
```

`mkswapfile` sets NOCOW and disables compression by itself — nothing to do by hand.

In `/etc/fstab`:

```
/swapfile none swap defaults 0 0
```

#### 2. Resume parameters

```bash
sudo btrfs inspect-internal map-swapfile -r /swapfile
```

The UUID is the same one that appears in `root=` — the UUID of the *decrypted* btrfs
volume, not of the LUKS container:

```bash
sudo grubby --update-kernel=ALL --args="resume=UUID=d816faf6-073e-4b77-88fb-bde89b123bea resume_offset=<number from map-swapfile>"
sudo dracut -f
sudo reboot
```

Check it:

```bash
cat /proc/cmdline
sudo systemctl hibernate
```

**Telling a real hibernation from a dry run is harder than it looks.** A
successful resume continues the *same* boot id and ends with
`PM: hibernation: hibernation exit`, and so does a `pm_test` or `disk=test_resume`
rehearsal that never cuts power — the restored image prints the same lines either
way, because everything the write and the reload printed is rolled back with the
kernel's ring buffer. Three checks that do work, cheapest first:

- **The passphrase.** With the image in a swap file inside LUKS and no keyfile or
  TPM, a genuine resume *cannot* happen without the initramfs asking for the disk
  password. If it did not ask, the machine never powered off.
- **The SSD's own counters.** `sudo nvme smart-log /dev/nvme0` before and after:
  `power_cycles` +1 means the drive lost power, and `unsafe_shutdowns` unchanged
  means it lost it cleanly. Those live on the controller, so nothing the kernel
  does to its own memory can roll them back.
- **The trackpad.** Force Touch has no mechanical click, so a trackpad that still
  clicks proves the board has power — but only if you try it during the window
  when the machine is supposed to be off, which is easy to get wrong: the write
  itself takes a minute with the screen already dark.

Resume on top of LUKS works: the initramfs decrypts the volume before restoring
the image, so the passphrase is asked for at power-on as usual.

**The offset belongs to that one file.** Recreate the swap file and you have to
recompute `resume_offset`.

#### 3. Wi-Fi after hibernation

The same problem as with `deep` comes back: hibernation cuts power to the BCM4350,
the kernel restores its own state from the image and assumes the chip is still
initialised. The cure is to unload the driver *before* hibernating, so the chip is
shut down properly and brought up from scratch afterwards.

`/usr/lib/systemd/system-sleep/brcmfmac-reload`:

```bash
#!/usr/bin/env bash
# Only the hibernate phase may be touched — see the warning below.
case "$SYSTEMD_SLEEP_ACTION" in
  hibernate) ;;
  *) exit 0 ;;
esac

case "$1" in
  pre)
    systemctl stop NetworkManager
    modprobe -r brcmfmac_wcc 2>/dev/null
    modprobe -r brcmfmac
    ;;
  post)
    modprobe brcmfmac
    systemctl start NetworkManager
    ;;
esac
```

```bash
sudo chmod +x /usr/lib/systemd/system-sleep/brcmfmac-reload
```

> **It has to be `/usr/lib`, not `/etc`.** `systemd-sleep` reads
> `/usr/lib/systemd/system-sleep/` and nothing else — there is no
> `/etc/systemd/system-sleep/`, unlike `/etc/systemd/system/`. A hook put there is
> silently never executed, with no error anywhere:
>
> ```bash
> strings /usr/lib/systemd/systemd-sleep | grep system-sleep   # one path only
> man systemd-sleep
> ```
>
> Nor does a package update wipe it: rpm owns the directory (via `systemd-udev`)
> but no file inside it, so a foreign file there survives.

Verified: with the hook in the right place, `SYSTEMD_SLEEP_ACTION=hibernate`
matches, the module is unloaded before the image stage and reloaded afterwards,
and Wi-Fi reconnects by itself. To confirm it ran at all, have it append to a log
and `sync` — the snippet is in [open issues](#open-issues).

> **Do not match on `"$1/$2"` here.** The obvious version of this hook —
> `case "$1/$2" in pre/*) ... post/*) ...` — hangs the machine on
> `suspend-then-hibernate`, and the failure looks like a resume problem rather
> than a hook problem.
>
> `systemd-sleep` runs the hook around *each* phase, but the second argument
> stays `suspend-then-hibernate` for both of them; the phase is only readable
> from `SYSTEMD_SLEEP_ACTION` (`suspend`, `hibernate`, or
> `suspend-after-failed-hibernate`) — see `man systemd-sleep`. A `pre/*`
> pattern therefore fires four times per cycle instead of two. On the way out of
> s2idle the `post` branch loads `brcmfmac` and starts NetworkManager, and
> 300 ms later the `pre` branch tries to unload the module again, fails with
> `modprobe: FATAL: Module brcmfmac is in use`, and the kernel begins writing the
> hibernation image while the chip is still taking on its firmware:
>
> ```
> 11:35:11.446  usbcore: registered new interface driver brcmfmac
> 11:35:11.628  Starting NetworkManager.service...
> 11:35:11.934  NetworkManager: caught SIGTERM, shutting down normally.
> 11:35:11.976  modprobe: FATAL: Module brcmfmac is in use.
> 11:35:11.980  Performing sleep operation 'hibernate'...
> 11:35:12.070  brcmfmac: brcmf_c_process_clm_blob ...
> 11:35:12.227  Filesystems sync: 0.066 seconds
>               <- journal ends here
> ```
>
> That race is real and worth avoiding. It was also blamed, wrongly, for the
> missing hibernation image: the image is not written on this machine whether the
> hook runs, misfires or is absent entirely, so `PM: Image not found (code -22)`
> on the next boot has a different cause. Either way it is not `resume_offset`,
> where nothing is wrong.

If this ever stops being enough, the next step is to power down the PCIe device
itself via `remove` in sysfs before hibernating and `rescan` after.

#### 4. Automatic sleep → hibernate

Two things have to line up: the lid has to ask for `suspend-then-hibernate`
rather than a plain suspend, and the delay before the second phase has to be set.

**The delay** — `/etc/systemd/sleep.conf`:

```
[Sleep]
HibernateDelaySec=15min
```

Nothing needs restarting: `systemd-sleep` reads its configuration at the moment
of going to sleep. A drop-in in `/etc/systemd/sleep.conf.d/` overrides this file,
which is convenient for testing with a shorter delay — check what actually
applies with `systemd-analyze cat-config systemd/sleep.conf`.

**The lid** — **System Settings → Power Management**, and on Plasma 6 this is
*not* the lid action:

| Setting | Value |
|---|---|
| `When laptop lid closed` | `Sleep` — leave as is |
| `When sleeping, enter` | **`Standby, then hibernate`** |

The list of lid actions has no "sleep then hibernate" entry at all. The lid only
selects *which* action runs; *what sleep means* is a separate dropdown, and that
is the one to change. In `~/.config/powerdevilrc` it lands as `SleepMode=3`.

The setting is **per power profile**, so it has to be set for battery and for AC
separately — `[Battery][SuspendAndShutdown]` and `[AC][SuspendAndShutdown]`.
Battery is the one that matters.

Ignore the description KDE prints under the entry ("Switch to hibernation when
battery runs low"). It is `systemd`'s `suspend-then-hibernate`, and the moment of
transition is `HibernateDelaySec`; a low battery is an additional trigger, not the
only one.

To confirm it took, close the lid and reopen it within the delay — `logind`
names the operation it was asked for as soon as the machine goes down:

```bash
journalctl -b 0 | grep 'will suspend'
```

```
The system will suspend and later hibernate now!   <- correct
The system will suspend now!                       <- still a plain suspend
```

On choosing the interval: at 4.1 W every 15 minutes of waiting costs about 1 Wh,
roughly 2% of the charge; 5 minutes would cost 0.7%. The point of the delay is that
short breaks — step away and come back — do not cost a full resume plus the LUKS
passphrase. Arguing against a very short delay: 7.6 GB of image written to the NVMe
every single time the lid closes.

### Battery data

```bash
upower -i /org/freedesktop/UPower/devices/battery_BAT0
```

| Field | Value |
|---|---|
| energy-full | 46.35 Wh |
| energy-full-design | 54.70 Wh |
| wear | 15% |
| charge cycles | 518 |

Beware of the two different meanings of "capacity": in `upower` output the
`capacity` field is the *wear level* (84.7%), not the charge. And
`/sys/class/power_supply/BAT0/capacity` on this machine reports a percentage of the
**design** capacity, so at a full charge it reads 85 while KDE shows 100 (KDE counts
against the current full capacity). For measurements, watt-hours are the reliable
source:

```bash
cat /sys/class/power_supply/BAT0/energy_now; date
```

---

### Thunderbolt is *not* the cause

Issue #207 recommends disabling Thunderbolt because of 2–3 minute resume delays.
That turned out to be a red herring here: with the sleep-mode fix above in place,
wake is instant whether or not the `thunderbolt` module is loaded. The fix is the
**sleep mode**, not Thunderbolt.

### Disabling Thunderbolt anyway

Two ways, both working. A blacklist file:

```bash
sudo sh -c 'echo "blacklist thunderbolt" > /etc/modprobe.d/disable-thunderbolt.conf'
sudo dracut -f
sudo rmmod thunderbolt        # unload now, without rebooting
lsmod | grep thunderbolt      # verify: no output
```

> **`sudo dracut -f` is the step that matters.** `thunderbolt` is loaded early out
> of the initramfs, so a blacklist file on its own changes nothing until the
> initramfs is rebuilt to include it. Skip `dracut -f` and the blacklist looks
> broken — the module simply loads again on the next boot.

Or a kernel parameter, which the kernel applies before any module loading and so
needs no initramfs rebuild of its own:

```bash
sudo grubby --update-kernel=ALL --args="module_blacklist=thunderbolt"
sudo rmmod thunderbolt
```

Undoing the blacklist file:

```bash
sudo rm /etc/modprobe.d/disable-thunderbolt.conf
sudo dracut -f
sudo modprobe thunderbolt     # bring it back without rebooting
```

Be aware that the `thunderbolt` module drives Thunderbolt devices on the USB-C
ports. Plain flash drives and charging keep working without it; external displays
over Thunderbolt and dock stations do not.

---

## Audio (Cirrus CS8409)

Driver: [davidjo/snd_hda_macbookpro](https://github.com/davidjo/snd_hda_macbookpro)
· [my fork](https://github.com/federal1970/snd_hda_macbookpro)

```bash
sudo dnf install gcc kernel-devel make patch wget git dkms
git clone https://github.com/davidjo/snd_hda_macbookpro.git
cd snd_hda_macbookpro/
sudo ./install.cirrus.driver.sh -i     # -i installs via DKMS
```

**Use the `-i` flag specifically.** There is no `--dkms` or `--force`; the script
silently ignores unknown arguments and falls back to a plain install. Removal is `-r`.

**A reboot is mandatory.** After `modprobe` the devices show up in `aplay -l`, but
there is no sound. After a reboot it works.

The module signing error (`SSL error ... signing_key.pem`) is harmless — released
Fedora kernels ship no signing key, and Secure Boot on this machine does not block
the unsigned module from loading.

Expected result:

```console
$ dkms status
snd_hda_macbookpro/0.1, 7.2.5-200.fc44.x86_64, x86_64: installed (Original modules exist)
```

From the project README:

- The audio profile must be **Analogue Stereo Output**; for the microphone,
  **Analogue Stereo Duplex**.
- The microphone is not fully finished — the recording level is very low (as it is
  under macOS) and needs software amplification.
- Suspend behaviour was not tested by the author; the hardware stays permanently
  powered on. That may be one contributor to the
  [4.1 W idle drain](#s2idle-costs-about-41-w) measured here — unverified.

---

## Camera (FaceTime HD)

The device is present on the bus:

```console
$ lspci -nn | grep 1570
03:00.0 Multimedia controller [0480]: Broadcom Inc. 720p FaceTime HD Camera [14e4:1570]
```

### 1. Firmware

[patjak/facetimehd-firmware](https://github.com/patjak/facetimehd-firmware)
· [my fork](https://github.com/federal1970/facetimehd-firmware)

```bash
git clone https://github.com/patjak/facetimehd-firmware.git
cd facetimehd-firmware
make && sudo make install
```

This installs the firmware and the sensor calibration files into
`/usr/lib/firmware/facetimehd` (12 files: `firmware.bin` plus eleven `*_01XX.dat`
calibration blobs).

### 2. Driver

[juicecultus/facetimehd](https://github.com/juicecultus/facetimehd) — a fork of the
original driver maintained for modern kernels
· [my fork](https://github.com/federal1970/facetimehd)

```bash
cd ~/dev
git clone https://github.com/juicecultus/facetimehd.git
sudo ln -sfn "$HOME/dev/facetimehd" /usr/src/facetimehd-0.6.13
sudo dkms install facetimehd/0.6.13
```

### 3. Kernel 7.2 build fix

On kernel 7.2 the build fails:

```
fthd_v4l2.c:392:9: error: implicit declaration of function 'strncpy'
        strncpy(fmt->description, desc, sizeof(fmt->description));
```

`strncpy()` is **no longer declared** in `include/linux/string.h`; it survives only
in comments on `strtomem`/`memtostr`. Verify on your own kernel:

```bash
grep -n "strncpy\|strscpy" /usr/src/kernels/$(uname -r)/include/linux/string.h
```

The fix is one line in `fthd_v4l2.c`:

```diff
-	strncpy(fmt->description, desc, sizeof(fmt->description));
+	strscpy(fmt->description, desc, sizeof(fmt->description));
```

Available in [my fork](https://github.com/federal1970/facetimehd) as commit
[`53db83c`](https://github.com/federal1970/facetimehd/commit/53db83c), and submitted
upstream independently as
[juicecultus/facetimehd#1](https://github.com/juicecultus/facetimehd/pull/1).

Why this specific change:

- `strscpy()` takes the same three arguments and the return value was unused here,
  so behaviour is unchanged — and NUL-termination is now guaranteed, which
  `strncpy()` never promised. Truncation cannot occur anyway: `description` is
  `char[32]` and the string literals are 4 characters.
- **`strscpy()`, not `strscpy_pad()`** — `strscpy()` has existed since kernel 4.3 and
  keeps the driver's older `LINUX_VERSION_CODE` guards working, whereas
  `strscpy_pad()` only landed in 5.2.
- **No extra `#include <linux/string.h>` is needed** — it already comes in
  transitively via `linux/kernel.h`. Proof: `strcpy()` is still declared
  (`string.h:68`) and the driver's four other `strcpy()` call sites compile fine.
- This was the *only* kernel 7.2 API breakage in the driver; nothing else needed
  touching.

### 4. Cheese freezes on the first frame

With the driver built and loaded, Cheese shows one frame and then stops, while the
hardware keeps streaming. The cause is timestamps, not the capture path.

The vb2 queue advertises `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC` (`fthd_v4l2.c:696`),
promising every buffer a monotonic timestamp. But `fthd_buffer_return_handler()`
hands buffers back through `vb2_buffer_done()` without ever setting `vb->timestamp`,
so every frame carries a zero:

```console
$ ffmpeg -f v4l2 -video_size 1280x720 -i /dev/video0 -frames:v 4 -vf showinfo -f null -
n:  0 pts: 0 pts_time:0
n:  1 pts: 0 pts_time:0
n:  2 pts: 0 pts_time:0
n:  3 pts: 0 pts_time:0
```

That splits consumers in two. `ffmpeg` writing to a null muxer ignores timestamps
and reports a healthy `frame=60 fps=30`, which is why the driver looks fine on the
`-list_formats` check above. GStreamer — and therefore Cheese — schedules display
*by* PTS, so after the first frame nothing is ever due, and the picture stands still.
The same thing shows up in `ffmpeg` as soon as it writes a real file:
`frame=2, drop=571` over 20 seconds.

The fix is one line in `fthd_buffer_return_handler()`:

```diff
 		if (ctx->state == BUF_HW_QUEUED || ctx->state == BUF_DRV_QUEUED) {
 			ctx->state = BUF_ALLOC;
+			ctx->vb->timestamp = ktime_get_ns();
 			vb2_buffer_done(ctx->vb, VB2_BUF_STATE_DONE);
 		}
```

`ktime_get_ns()` is exactly the monotonic clock the queue already promises, and no
extra `#include` is needed — it arrives transitively via the videobuf2 headers.

Available in [my fork](https://github.com/federal1970/facetimehd) as commit
[`95cae61`](https://github.com/federal1970/facetimehd/commit/95cae61), submitted
upstream as [juicecultus/facetimehd#2](https://github.com/juicecultus/facetimehd/pull/2).

Frame `sequence` numbering is still unset; nothing here appeared to care, so it was
left alone.

### 5. Build and verify

```bash
sudo dkms remove facetimehd/0.6.13 --all
sudo dkms install facetimehd/0.6.13
sudo modprobe facetimehd
ls -l /dev/video*
sudo dmesg | grep -i fthd
```

A capture-free check that the driver actually works — this exercises the very
`fthd_v4l2_ioctl_enum_fmt_vid_cap()` function that was patched:

```console
$ ffmpeg -f v4l2 -list_formats all -i /dev/video0
Raw       :     yuyv422 :           YUYV 4:2:2 : {320-1280, 8}x{240-720, 2}
Raw       : Unsupported :           YVYU 4:2:2 : {320-1280, 8}x{240-720, 2}
```

(The `Error opening input file` line that follows is normal for `-list_formats`,
not a failure.) For a live preview, `ffplay -f v4l2 -video_size 1280x720 /dev/video0`.

**Check the timestamps too** — this is what tells a working camera apart from one
that will freeze in Cheese. PTS must advance by about 33 ms per frame:

```console
$ ffmpeg -f v4l2 -video_size 1280x720 -i /dev/video0 -frames:v 4 -vf showinfo -f null -
n:  0 pts:      0 pts_time:0
n:  1 pts:  33144 pts_time:0.033144
n:  2 pts:  66521 pts_time:0.066521
n:  3 pts: 101653 pts_time:0.101653
```

And a GStreamer pipeline with `sync=true`, which honours those timestamps the way
Cheese does, must finish rather than stall:

```bash
gst-launch-1.0 v4l2src device=/dev/video0 num-buffers=60 ! videoconvert ! fakesink sync=true
```

### 6. Firefox: `NotFoundError` with a camera that works

Cheese plays, `ffmpeg` captures, and Firefox still reports no camera at all:

```
Could not find a web camera, however there are other media devices.
NotFoundError: The object can not be found here.; DOMException
```

Nothing is wrong below the browser. Worth confirming before chasing the driver: the
V4L2 ioctls all answer (`VIDIOC_QUERYCAP`, `ENUM_FMT`, `ENUM_FRAMESIZES`,
`ENUM_FRAMEINTERVALS`), `/dev/video0` is reachable through a per-seat ACL rather than
the `video` group, and PipeWire exposes the device properly:

```console
$ wpctl status | grep -i facetime
 │      52. Apple Facetime HD                   [v4l2]
 │  *   61. Apple Facetime HD (V4L2)

$ pw-cli info 61 | grep media.role
*		media.role = "Camera"
```

The cause is that Firefox 156 reaches for the camera through the PipeWire portal
instead of opening the device directly, and that path yields nothing here. Two runs
of the same page on a scratch profile, with only this pref differing:

```
media.webrtc.camera.allow-pipewire=true   ->  no answer in 25 s, no devices
media.webrtc.camera.allow-pipewire=false  ->  Capture Devices: 1
                                              GUM_OK: Apple Facetime HD
```

**The fix:** in `about:config` set **`media.webrtc.camera.allow-pipewire`** to
**`false`** and restart Firefox.

To reproduce the diagnosis on your own machine, the camera engine will say how many
devices it found:

```bash
MOZ_LOG="CamerasChild:5,CamerasParent:5" MOZ_LOG_FILE=/tmp/ff.txt \
  firefox --headless --new-instance --profile /tmp/ffprof about:blank
grep -a "Capture Devices:" /tmp/ff.txt*
```

Why the portal path fails is *not* settled — see [open issues](#open-issues). It is
not a missing permission: the KDE portal backend is alive on the bus, the camera
portal reports `IsCameraPresent = true`, and the permission store already holds
`camera: yes` for `org.mozilla.firefox`. A direct `AccessCamera` call simply never
returns a response.

**A warning carried over from the AUR package** of the same driver: keeping the
module permanently loaded may break suspend. Not reproduced here yet, but worth
watching. To back out: `sudo dkms remove facetimehd/0.6.13 --all`.

---

## Wi-Fi (BCM4350)

Works with no additional drivers.

These `dmesg` errors are **harmless**:

```
brcmfmac4350c2-pcie.txt ... -2
brcmfmac4350c2-pcie.clm_blob ... -2
brcmfmac4350c2-pcie.txcap_blob ... -2
```

Those files are optional; the driver continues with built-in values. The related
`device may have limited channels available` warning means a reduced 5 GHz channel
list.

### "Networks are visible but connecting hangs forever"

Reproduced on one particular router; a separate access point with plain WPA2
connected immediately. The router-side cause was not chased down. Candidates:

| Suspect | Mitigation |
|---|---|
| Mixed WPA2/WPA3 transition mode | `modprobe brcmfmac feature_disable=0x82000` |
| PMF set to *required* | Set to *optional* on the router |
| 5 GHz channel in the DFS range | Pin the router to a non-DFS channel |

Reloading the module requires stopping NetworkManager first — the same sequence the
[hibernation hook](#3-wi-fi-after-hibernation) runs automatically:

```bash
sudo systemctl stop NetworkManager
sudo modprobe -r brcmfmac_wcc
sudo modprobe -r brcmfmac
sudo modprobe brcmfmac feature_disable=0x82000
sudo systemctl start NetworkManager
```

---

## Caps Lock as a layout switch

macOS behaviour: a short tap switches the keyboard layout, holding it gives a real
Caps Lock.

This is done with [`keyd`](https://github.com/rvaiya/keyd), which works at the
uinput level and therefore behaves identically on X11 and Wayland — unlike
`setxkbmap`.

`keyd` is not in the main Fedora repositories, only in COPR:

```bash
sudo dnf copr enable alternateved/keyd
sudo dnf install keyd
```

`/etc/keyd/default.conf`:

```ini
[ids]
*

[main]
capslock = timeout(f13, 200, capslock)
```

```bash
sudo systemctl enable --now keyd
sudo keyd reload
```

Then bind F13 to the layout switch in **System Settings → Shortcuts**. KDE displays
F13 under the name **"Tools"** and warns about a conflict with opening System
Settings — accept the reassignment.

To see what the key actually emits: `sudo keyd monitor`.

---

## Trackpad

To get macOS-like behaviour, turn off tap-to-click:

**System Settings → Input Devices → Touchpad →** uncheck **Tap-to-click**.

---

## My forks

Forked so the patches stay available regardless of upstream merge timing.

| Fork | Upstream | Purpose |
|---|---|---|
| [federal1970/facetimehd](https://github.com/federal1970/facetimehd) | [juicecultus/facetimehd](https://github.com/juicecultus/facetimehd) | FaceTime HD driver, **includes the kernel 7.2 `strscpy` fix and the buffer timestamp fix** |
| [federal1970/facetimehd-firmware](https://github.com/federal1970/facetimehd-firmware) | [patjak/facetimehd-firmware](https://github.com/patjak/facetimehd-firmware) | Camera firmware extraction, used unmodified |
| [federal1970/snd_hda_macbookpro](https://github.com/federal1970/snd_hda_macbookpro) | [davidjo/snd_hda_macbookpro](https://github.com/davidjo/snd_hda_macbookpro) | Cirrus CS8409 audio driver, used unmodified |

---

## Open issues

1. **Package C-states never go below C3** — this is what makes S0ix, and therefore a
   low-power `s2idle`, impossible. No `PNP0D80` ACPI device is exposed by the
   firmware. Unclear whether anything on the OS side can change that.
2. **Microphone** — check the recording level and the Analogue Stereo Duplex profile.
3. **Wi-Fi on the main router** — identify what actually blocks the connection.
4. **Hibernation with the audio driver loaded** — its README warns the hardware stays
   permanently powered on; the 4.1 W idle drain may partly come from there. Worth
   measuring with the module unloaded.
5. **Suspend with `facetimehd` loaded** — the AUR package warns the module breaks
   suspend. Not reproduced for `s2idle`: a 60-second cycle resumed cleanly with the
   module loaded and the camera still streamed afterwards. Long cycles remain
   untested. Hibernation works with the module loaded; it reinitialises the camera
   from scratch on resume, firmware upload included.
6. **Hibernation image size** — the snapshot is about 2.9–3.0 GB and scales with
   the memory in use, so a busy session costs more. It is compressed on the way
   out: 757457 pages (2.9 GB) became **894 MB** on disk, about 3.3:1, measured
   with `nvme smart-log`. The whole cycle — snapshot, write, power-off, firmware,
   GRUB, initramfs, passphrase, read-back, restore — took 114 s. Note that
   `PM: hibernation: Allocated 2991616 kbytes in 17.2 seconds (174.0 MB/s)` is the
   *preallocation of the snapshot in RAM*, not a disk write — it is printed on
   every attempt, successful or not, and an earlier version of this file read it as
   proof of a working write.
7. **The PipeWire camera portal returns nothing** — worked around with
   `media.webrtc.camera.allow-pipewire=false`, but the cause is unknown. An
   `org.freedesktop.portal.Camera.AccessCamera` call hands back a request handle and
   then never sends a `Response`, even though the KDE backend is running and the
   permission is already granted. One thing to look at: an unsandboxed Firefox has an
   empty app ID, while the stored permission is keyed to `org.mozilla.firefox`.
8. **Resolved: hibernation works — and how four hours were spent proving it did
   not.** On 2026-09-20 this entry asserted that no image had ever been written.
   That was wrong, and every piece of evidence for it turned out to be an artefact
   of measuring a hibernation from inside the kernel that is being hibernated. The
   real fault was the `brcmfmac` sleep hook, fixed earlier the same day; the only
   real attempt made before that fix produced `PM: Image not found (code -22)`, and
   every attempt afterwards was a `pm_test` or `disk=test_resume` rehearsal that
   never cut power. The three traps are worth keeping, because each one is
   convincing on its own:

   - **`/proc/diskstats` cannot see the write.** Those counters are ordinary kernel
     memory, so they are captured in the snapshot and rolled back by a successful
     restore. Measuring the image this way always reads a few MB, no matter what
     was written. The same effect wipes the printk ring buffer, which this file
     already knew — it was simply never applied to the disk counters. Use the
     NVMe controller's own `data_units_written`; it lives on the SSD.
   - **Apple's controller reports `data_units_written` in plain 512-byte units**,
     not the NVMe-standard 1000 × 512. Taking the spec at its word overstates every
     figure by a factor of 1000. `tools/hib-smart-test.sh --calibrate` measures the
     unit directly.
   - **`Timekeeping suspended for N seconds` on the way back is not a stall.** The
     restored kernel resumes *inside* the snapshot, so `timekeeping_resume()`
     reports everything that happened since the snapshot was taken — the write, the
     power-off, the boot, the read-back — as if the CPU had been stopped. Two
     minutes there is a normal cycle, not a hang. For the same reason
     `swsusp_save()` never appears to log its completion line `Image created (N
     pages copied)`: that line is printed after the snapshot point, so a restore
     rolls the buffer back past it. And the 80–90 ms between
     `ACPI: PM: Waking up from system sleep state S4` and
     `PM: hibernation: Basic memory bitmaps freed`, once read as proof that no 3 GB
     write could have fitted, is simply the tail of the restore; the write happened
     before that, invisibly.

   One self-inflicted failure is worth recording too: a kprobe on
   `copy_data_pages` hangs the machine hard enough to need a power cycle. That
   function runs once per page — 757k times inside the snapshot with interrupts
   off.

   Finally, `systemctl hibernate` ignores whatever you write to `/sys/power/disk`:
   `systemd-sleep` writes its own `HibernateMode=` there immediately beforehand.
   Choose the mode with a `/etc/systemd/sleep.conf.d/` drop-in. The confirmed run
   used the default, `platform`, i.e. ACPI S4.

9. **Testing hibernation without losing the session.** `/sys/power/pm_test` is a
   ladder — `freezer`, `devices`, `platform`, `processors`, `core` — and each rung
   stops one step deeper, waits 5 seconds and returns without writing anything or
   cutting power. `core` covers everything except the snapshot copy.
   `/sys/power/disk=test_resume` is meant to go one further — write a real image,
   then check and restore it in the same boot with no power-off — which is how
   issue 8 was pinned down to the snapshot. Both leave the journal intact, and that
   matters: kernel messages after `PM: hibernation: hibernation entry` are buffered
   and only reach disk if the machine comes back. A sleep hook that logs and
   `sync`s survives even a run that does not:

   ```bash
   { printf '%s  arg1=%s arg2=%s SYSTEMD_SLEEP_ACTION=%s\n' \
       "$(date '+%F %T.%3N')" "$1" "$2" "${SYSTEMD_SLEEP_ACTION-<unset>}" \
       >> /var/log/sleep-hook.log
     sync /var/log/sleep-hook.log; } 2>/dev/null
   ```

   What those rehearsals cannot tell you is whether a *real* run cut power, since
   a restored image prints the same lines either way. For that, read the SSD's own
   counters around the run — `power_cycles` +1 with `unsafe_shutdowns` unchanged is
   a clean power loss — and note whether the initramfs asked for the LUKS
   passphrase, which it cannot skip. `tools/hib-real-test.sh` does the counter part
   and keeps its baseline on disk, so the answer survives even a run that has to be
   ended with a hard reset.

   On this hardware there is also a free physical check, with one catch: the Force
   Touch trackpad has no mechanical click, so a trackpad that still clicks means
   the board has power. The catch is timing — the write takes about a minute with
   the screen already dark, so a click during *that* proves nothing.

---

## References

- [Dunedan/mbp-2016-linux](https://github.com/Dunedan/mbp-2016-linux) — the summary
  table of what works on 2016/2017 MacBooks
- [Dunedan/mbp-2016-linux#207](https://github.com/Dunedan/mbp-2016-linux/issues/207) —
  the working suspend configuration for MacBookPro13,1
- [davidjo/snd_hda_macbookpro](https://github.com/davidjo/snd_hda_macbookpro) —
  Cirrus CS8409 audio driver
- [patjak/facetimehd-firmware](https://github.com/patjak/facetimehd-firmware) —
  camera firmware extraction
- [juicecultus/facetimehd](https://github.com/juicecultus/facetimehd) — FaceTime HD
  driver fork for modern kernels
- [rvaiya/keyd](https://github.com/rvaiya/keyd) — the key remapping daemon
  ([Fedora COPR](https://copr.fedorainfracloud.org/coprs/alternateved/keyd))

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
| **Hibernation** | **The real fix** | [Swap file on btrfs, `resume=`, sleep-then-hibernate](#hibernation--the-solution) |
| **Audio (Cirrus CS8409)** | **Fixed** | [Out-of-tree DKMS driver](#audio-cirrus-cs8409) |
| **Camera (FaceTime HD)** | **Fixed** | [Firmware extraction + DKMS driver + a kernel 7.2 source fix](#camera-facetime-hd) |
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
system. The working arrangement is **suspend-then-hibernate**: s2idle for short
breaks, hibernation after 15 minutes.

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

### Hibernation — the solution

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

Resume on top of LUKS works: the initramfs decrypts the volume before restoring the
image, so the passphrase is asked for at power-on as usual.

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
case "$1/$2" in
  pre/*)
    systemctl stop NetworkManager
    modprobe -r brcmfmac_wcc 2>/dev/null
    modprobe -r brcmfmac
    ;;
  post/*)
    modprobe brcmfmac
    systemctl start NetworkManager
    ;;
esac
```

```bash
sudo chmod +x /usr/lib/systemd/system-sleep/brcmfmac-reload
```

Verified: Wi-Fi reconnects by itself after hibernation. The hook also runs on
ordinary suspend, where it is unnecessary but harmless — it only adds a short
reconnect delay.

If this ever stops being enough, the next step is to power down the PCIe device
itself via `remove` in sysfs before hibernating and `rescan` after.

#### 4. Automatic sleep → hibernate

`/etc/systemd/sleep.conf`:

```
[Sleep]
HibernateDelaySec=15min
```

```bash
sudo systemctl restart systemd-logind
```

Then set the lid-close action to **"Sleep then hibernate"** in
**System Settings → Power Management**.

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

### 4. Build and verify

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
| [federal1970/facetimehd](https://github.com/federal1970/facetimehd) | [juicecultus/facetimehd](https://github.com/juicecultus/facetimehd) | FaceTime HD driver, **includes the kernel 7.2 `strscpy` fix** |
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
5. **Suspend and hibernation with `facetimehd` loaded** — verify the AUR package's
   warning about the module breaking suspend.
6. **Hibernation image size** — 7.6 GB written to the NVMe on every lid close once
   `HibernateDelaySec` elapses. Compression (`resumeflags`, or shrinking the swap
   file) has not been looked at.

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

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
| Wi-Fi (BCM4350) | Works | [Harmless firmware errors](#wi-fi-bcm4350); [no WPA3 under Linux](#no-wpa3-under-linux--macos-does-it-on-the-same-hardware), so it uses a WPA2 SSID |
| Bluetooth | Works out of the box | — |
| Trackpad + gestures | Works out of the box | [Disable tap-to-click](#trackpad) if you want macOS-like behaviour |
| Keyboard, backlight, screen brightness | Works out of the box | — |
| NVMe, battery, USB-C | Works out of the box | — |
| **Suspend / resume** | **Works, `deep` (S3) drains 1.37 W** | [Kernel parameters + boot-time script](#sleep-and-hibernation); `s2idle` costs 4 W and S0ix is out of reach, so the default sleep is `deep` since 2026-09-21 |
| **Hibernation** | **Works** | [Swap file, `resume=`, and a `brcmfmac` sleep hook](#hibernation--the-intended-solution). Resume on LUKS works; the passphrase is asked for at power-on |
| **Audio (Cirrus CS8409)** | **Fixed** | [Out-of-tree DKMS driver](#audio-cirrus-cs8409) |
| **Camera (FaceTime HD)** | **Fixed** | [Firmware extraction + DKMS driver + two source fixes](#camera-facetime-hd): a kernel 7.2 build error and missing buffer timestamps. Firefox needs [one pref](#6-firefox-notfounderror-with-a-camera-that-works) on top |
| Caps Lock as layout switch | Configurable | [keyd](#caps-lock-as-a-layout-switch) |
| Microphone | Works | Nothing to set; the earlier "very low level" note was wrong — see [open issues](#open-issues) |

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
power: `s2idle` on this machine drains the battery at about half the rate of a
running system, some 4 W. `deep` (ACPI S3) measures **1.37 W** with the right
wakeup sources disarmed — a full battery lasts a day and a half instead of half
a day — and has been the default sleep since 2026-09-21. On battery the lid still
asks for **suspend-then-hibernate**, so a lid shut for longer than 15 minutes
ends in hibernation, which costs nothing to hold. Both halves work — the hibernate
half took a while to believe, and `deep` was written off for a day on a bad
measurement; both stories are in [open issues](#open-issues).

The suspend part is based on
[Dunedan/mbp-2016-linux issue #207](https://github.com/Dunedan/mbp-2016-linux/issues/207)
("Suspend working with Linux Mint 22.3 on 2016 Macbook (no touch bar)").

### 1. Kernel parameters

```bash
sudo grubby --update-kernel=ALL --args="button.lid_init_state=open nvme_core.default_ps_max_latency_us=0 nvme.noacpi=1 pci=noaer i915.enable_fbc=0 mem_sleep_default=deep"
```

`mem_sleep_default=deep` selects S3. Issue #207 and every earlier revision of this
file used `s2idle`, for reasons that turned out to be a broken measurement — see
[`deep` costs 1.37 W](#deep-costs-137-w--three-times-less-than-s2idle-2026-09-21).
Switching an installed system: `--remove-args="mem_sleep_default=s2idle"
--args="mem_sleep_default=deep"` in one `grubby` call, and `echo deep | sudo tee
/sys/power/mem_sleep` takes effect without a reboot.

`i915.enable_dc=0` and `i915.enable_psr=0` were part of the original recipe and have
since been dropped: removing them changed nothing about the drain, and suspend works
fine without them.

### 2. Boot-time fix script

`/usr/local/bin/mbp-suspend-fix.sh`:

```bash
#!/usr/bin/env bash
find /sys/devices/ -name d3cold_allowed -exec sh -c 'echo 0 > "$1" 2>/dev/null' _ {} \;
# RP05 XHC2 SPIT were added on 2026-09-21 for deep (S3): the test that measured
# 1.37 W had them disarmed. SPIT is the keyboard and trackpad, so a key press
# does not wake the machine from deep. LID0 was dropped from the list the same
# day: it has to stay armed for the lid to wake the machine from S3.
for device in XHC1 ARPT RP01 RP09 RP10 RP05 XHC2 SPIT; do
  if grep -q "$device.*enabled" /proc/acpi/wakeup; then
    echo "$device" > /proc/acpi/wakeup
  fi
done
```

```bash
sudo chmod +x /usr/local/bin/mbp-suspend-fix.sh
```

Issue #207's list was `LID0 XHC1 ARPT RP01 RP09 RP10`. `RP05` is the Thunderbolt
root port, `XHC2` the xHCI controller inside the Alpine Ridge chip, `SPIT` the
SPI topcase — the keyboard and trackpad. All three were still armed when `deep`
"woke itself after 10 seconds"; the 1.37 W run had them off and slept its full
30 minutes. Which of the three was the spurious wake has not been isolated. The
price is that a key press no longer wakes the machine from `deep`; the power
button and the lid do.

**`LID0` must stay armed under `deep`.** With it disarmed the lid does not wake
the machine from S3 at all — tested twice on 2026-09-21, three minutes and one
minute shut, power button needed both times. The DSDT says why: `LID0._PSW`
writes the EC's `EWLO` bit ("wake on lid open"), and the kernel evaluates `_PSW`
only for an armed wakeup source, so with `LID0` disabled the EC is never told to
wake anybody. Under `s2idle` that never mattered, because the EC stays alive and
its SCI wakes the kernel regardless. Armed, the lid woke the machine from S3 on
the first try and caused no spurious wake in a two-minute cycle; whether it
stays quiet over a night is the remaining check.

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
cat /sys/power/mem_sleep      # expected: s2idle [deep]
grep -v disabled /proc/acpi/wakeup   # expected: LID0 and nothing else
```

Then the first real cycle: close the lid, wait a minute, open it — the lid
wakes it. Afterwards make sure it really was S3 and not a dry run:

```bash
journalctl -k -b | grep -E 'sleep state S3|suspend debug'
cat /sys/power/pm_test        # must be [none]
```

**Result:** USB and Wi-Fi are back after the wake — Wi-Fi because the [sleep hook](#3-wi-fi-after-deep-and-after-hibernation)
reloads `brcmfmac`, which loses its firmware when the rails go down in S3.

---

### Hibernation costs nothing to hold

Measured 2026-09-20 with the battery gauge, charger unplugged, over a 64 min 28 s
hibernation (`tools/hib-battery-test.sh`):

| state, over that hour | cost |
|---|---|
| **hibernated** | **nothing measurable** — the gauge read 26000 µAh *higher* afterwards |
| `s2idle` | 4.12 Wh, about 8% of a full battery |
| awake and idle | 8.80 Wh |

The gauge reading rises because the "before" sample is taken under an ~8 W load,
with the cell voltage sagging, and the "after" sample follows an hour at rest.
`capacity` agrees: 77% before, 78% after. What matters is that the measurement
noise, ±26000 µAh, is an order of magnitude smaller than the 335000 µAh `s2idle`
would have drawn in the same window.

What hibernation does cost is the transition — writing the image, the firmware
boot and reading it back — measured separately at about **0.32 Wh**. Against
`s2idle`'s 3.83 W that pays for itself after roughly five minutes with the lid
shut, which is why the documented `HibernateDelaySec=15min` is comfortable.

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

### `deep` costs 1.37 W — three times less than s2idle (2026-09-21)

Measured with `tools/deep-test.sh`: charger unplugged, `brcmfmac` unloaded,
`RP05 XHC2 SPIT ARPT` disarmed on top of the boot script's list, `thunderbolt`
blacklisted, an RTC alarm at 30 minutes as the safety net
(`sudo MEM_SLEEP=deep SLEEP_SECS=1800 REPORT=/var/tmp/osi-deep.txt tools/deep-test.sh`).
The s2idle control ran the same way a few minutes earlier.

| state | drain | a full battery (46 Wh) lasts |
|---|---|---|
| awake, idle | ~8.2 W | ~6 h |
| `s2idle` | 3.83 W (Sep 20, 30 min) / 4.01 W (Sep 21, 12.5 min) | ~12 h |
| **`deep` (S3)** | **1.37 W** | **~36 h** |

What makes the number believable, in the order the earlier attempt got wrong:
the kernel log has `ACPI: PM: Preparing to enter system sleep state S3` and
`Waking up from system sleep state S3` for that window, `pm_test` was `[none]`,
the machine slept the full 1801 s and was woken by the RTC (`PNP0B00:00` is the
only wakeup source that fired), and the gauge moved 55 mAh in 1 mAh steps, which
is good to a few percent. The battery cooled from 34.1 to 28.8 °C.

**Everything this file used to say against `deep` was a measurement error.** The
2026-09-20 attempt reported 5.30 W and "wakes itself after 10 seconds"; it left
no report file and nobody checked the log for the S3 line, so it most likely
never entered S3 at all, or woke at once. The two symptoms it described were
real but were configuration, not hardware:

- the wake after 10 seconds came from one of `RP05`, `XHC2`, `SPIT`, which the
  original issue #207 list leaves armed. `/sys/power/pm_wakeup_irq` is empty for
  all three because they wake over ACPI GPEs, not an ordinary interrupt;
- "Wi-Fi dead until a power cycle" is the BCM4350 losing its firmware when the
  rails go down in S3, the same thing that happens in hibernation, and the same
  hook cures it. With the module unloaded before sleep and reloaded after, Wi-Fi
  reconnected on every S3 cycle.

Two side effects of S3 that look alarming in the log and are not: `xhci_hcd
0000:07:00.0: xHC error in resume, USBSTS 0x401, Reinit` (the Alpine Ridge
xHCI lost power and is reinitialised) and one `applespi ... Received corrupted
packet (crc mismatch)` on resume.

One thing that is *not* an S3 problem, noticed during the s2idle control: the
keyboard backlight stays lit through `s2idle`. That is by design in `applespi` —
its suspend handler turns off only the caps-lock LED, and the backlight has no
`LED_CORE_SUSPENDRESUME` flag — so at any nonzero level it burns for the whole
of s2idle. In S3 the rail is cut and it cannot matter.

Still 3-4 times what macOS draws in the same S3 (0.3-0.5 W); where the rest goes
is the subject of the next chapter.

---

### What the ACPI tables say — two sleep paths, Linux walks neither (2026-09-21)

The tables are dumped and disassembled in [`acpi/`](acpi/README.md). The short
version: the firmware classifies the OS once at boot through `_OSI`, and
everything about sleep power branches on the answer.

- **macOS path** (`_OSI("Darwin")`, which Linux answers by default): `_PTS` does
  almost nothing. Power is cut in per-device `_PS3` methods and in Apple-private
  methods that Apple's drivers call first — `NHI0.RTPC(0)` and `XHC2.RTPC(0)` to
  release Thunderbolt, `SXFP`/`TRPE` to cut its power, `RP01._PS3` to cut the SSD's
  two power GPIOs, `RP09._PS3` to power-gate Wi-Fi. Linux drivers call none of the
  private ones, and the kernel never puts the Thunderbolt root port into D3 (it is
  a native-hotplug port on x86), so Alpine Ridge stays powered through every sleep.
- **Boot Camp path** (anything else): written for Windows drivers, so `_PTS(3)`
  itself powers off Bluetooth and the camera and arms the EC, and `_WAK` restarts
  the Thunderbolt firmware. Reachable with `acpi_osi=!Darwin`, which the kernel
  documents as the workaround for the "power regressions on Mac laptops" that
  answering Darwin introduced. **Tried 2026-09-21: unbootable.** On this path the
  tables describe the SPI keyboard and trackpad in a shape Linux does not
  implement (no Apple SPI properties, interrupt moved off the GPE), so the LUKS
  passphrase prompt comes up with a dead keyboard. Details and the table of what
  changes are in [`acpi/README.md`](acpi/README.md). The Boot Camp path is
  closed; what it would have switched off has to be called by hand instead.

So the 4 W of s2idle and the 1.37 W of `deep` above were measured with
Thunderbolt, camera and Bluetooth powered and nobody asking the firmware to
switch them off. That is not
firmware that needs reverse-engineering — the methods exist, are named, and can be
called from the OS. The experiments, cheapest first, are listed at the end of
[`acpi/README.md`](acpi/README.md); the first one (`acpi_osi=!Darwin`) has been
run and is out, the rest have not.

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
- **The case temperature.** A machine drawing 3.8 W in `s2idle` stays warm; one
  that has hibernated goes cold within a few minutes. Crude, but it has never
  misled, unlike the trackpad below.

Do **not** use the trackpad. Earlier revisions of this file claimed that Force
Touch has no mechanical click, so a trackpad that still clicks proves the board
has power. On this machine it clicks anyway, and it did so during two cycles that
the passphrase prompt and the NVMe counters both confirmed as real power-off
hibernations. It cost two test runs before that was noticed.

Resume on top of LUKS works: the initramfs decrypts the volume before restoring
the image, so the passphrase is asked for at power-on as usual.

**The offset belongs to that one file.** Recreate the swap file and you have to
recompute `resume_offset`.

#### 3. Wi-Fi after `deep` and after hibernation

Both cut power to the BCM4350. The kernel restores its own state — from RAM
after S3, from the image after hibernation — and assumes the chip is still
initialised; it is not, and the driver never recovers. The cure is to unload
the driver *before* sleeping, so the chip is shut down properly and brought up
from scratch afterwards. The hook acts on the suspend phase and on the
hibernate phase; with `s2idle` it was hibernate only.

`/usr/lib/systemd/system-sleep/brcmfmac-reload`:

```bash
#!/usr/bin/env bash
# BCM4350 loses power in S3 (deep) and in hibernation and does not come back on
# its own: unload brcmfmac before, load it again after.
#
# MUST live in /usr/lib/systemd/system-sleep/ -- systemd-sleep reads that
# directory and only that one; /etc/systemd/system-sleep/ is never scanned.
#
# The phase is read from SYSTEMD_SLEEP_ACTION, never from "$2": during
# suspend-then-hibernate the second argument stays "suspend-then-hibernate" for
# both phases. Because this hook now acts on the suspend phase too, a timer wake
# runs post/suspend and then pre/hibernate a few hundred ms apart: the module is
# loaded, NetworkManager started, and both have to be undone again while the
# chip is still taking its firmware. That is where "Module brcmfmac is in use"
# used to come from, and why the unload is retried instead of attempted once.

case "$SYSTEMD_SLEEP_ACTION" in
  suspend|hibernate|suspend-after-failed-hibernate) ;;
  *) exit 0 ;;
esac

case "$1" in
  pre)
    systemctl stop NetworkManager
    unloaded=0
    for _ in $(seq 30); do
      modprobe -r brcmfmac_wcc 2>/dev/null
      modprobe -r brcmfmac 2>/dev/null && { unloaded=1; break; }
      sleep 0.5
    done
    [ "$unloaded" = 1 ] || echo "brcmfmac-reload: could not unload brcmfmac, sleeping with it loaded" >&2
    # loading brcmfmac re-arms ARPT as a wakeup source; disarm it again before sleeping
    /usr/local/bin/mbp-suspend-fix.sh
    ;;
  post)
    modprobe brcmfmac
    systemctl start NetworkManager
    /usr/local/bin/mbp-suspend-fix.sh
    ;;
esac
```

The two calls to the boot-time script are not decoration. Loading `brcmfmac`
sets `power/wakeup` on the BCM4350's PCI device to `enabled`, which is the same
thing as `ARPT` showing `*enabled` in `/proc/acpi/wakeup` — the boot script's
disarming is undone by every reload, and the PCI core arms that wake for S3 even
with no driver bound. Seen on the very first `deep` cycle after the switch
(2026-09-21 21:40): `ARPT` was the one source armed afterwards.

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

Verified for hibernation: with the hook in the right place,
`SYSTEMD_SLEEP_ACTION=hibernate` matches, the module is unloaded before the image
stage and reloaded afterwards, and Wi-Fi reconnects by itself. To confirm it ran
at all, have it append to a log and `sync` — the snippet is in
[open issues](#open-issues). The suspend-phase half is what `tools/deep-test.sh`
does by hand around every `deep` run, and Wi-Fi came back after each of them.

> **Do not match on `"$1/$2"` here, and do not drop the retry loop.** The
> obvious version of this hook — `case "$1/$2" in pre/*) ... post/*) ...` —
> hangs the machine on `suspend-then-hibernate`, and the failure looks like a
> resume problem rather than a hook problem. The hook above acts on the suspend
> phase deliberately, so the same sequence of events now happens on every timer
> wake; the retry loop is what turns it from a hang into a one-second delay.
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
> That race is what stood between s2idle and hibernation, and fixing it is what
> made the whole arrangement work. The `PM: Image not found (code -22)` seen on
> 2026-09-20 came from the other form of the same fault — the hook sitting in
> `/etc/systemd/system-sleep/`, where it is silently never executed, so nothing
> unloaded the module at all. It was never `resume_offset`, where nothing is
> wrong.

If the retry loop ever runs out (the journal will show `could not unload
brcmfmac`), the next step is to power down the PCIe device itself via `remove` in
sysfs before sleeping and `rescan` after.

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

**Verified end to end, 2026-09-20.** Lid shut at 17:53:38 and not touched again:
`logind` announced `The system will suspend and later hibernate now!`, s2idle
held for exactly 15 min 1 s, the RTC alarm woke the machine at 18:08:40, it
hibernated at 18:08:42 with the lid still down, and the next power-on asked for
the passphrase and restored the session. Measured against the battery, the seven
minutes it spent powered down cost nothing: 3.92 Wh went in 40 minutes, against
3.80 Wh predicted by the awake and s2idle rates plus one transition.

> **`Lid opened` in the journal at exactly the delay is a lie.** On the way out
> of s2idle `logind` re-reads the lid switch and reports it open even when it is
> not — it appeared at 18:08:40, one second before the hibernation, in a run
> where the lid stayed shut until 18:14. It reads as though someone interrupted
> the test. Both lid tests on 2026-09-20 produced it, exactly at the alarm.

On choosing the interval, with `deep` at 1.37 W: 15 minutes of waiting costs
about 0.34 Wh, under 1% of the charge, and one hibernate-and-return transition
costs about 0.32 Wh — so the delay only starts paying for itself after some
14 minutes with the lid shut, and 15 minutes is now the shortest sensible
setting rather than a comfortable one. (Under `s2idle` at 3.83 W the same
15 minutes cost 0.96 Wh and break-even was five minutes.) A longer delay, an
hour say, would spare the NVMe the 894 MB image on every real lid close and
cost 1.4 Wh of waiting; it has not been changed yet. The point of the delay is
that short breaks — step away and come back — cost neither.

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

**Keeping it blacklisted also saves about half a watt.** Measured 2026-09-20 on
battery, idle, sampling `current_now` over 60–90 s, twice in each state:

```
thunderbolt loaded   : 9.27 W, 9.05 W
thunderbolt unloaded : 8.51 W, 8.70 W
                       -> about 0.55 W, well outside the ~0.2 W spread
```

It was worth checking the other way round, because with the module blacklisted
nothing manages Alpine Ridge at all and four of its seven functions sit in `D0`.
Loading the driver does not change that — still four in `D0` — it only adds its
own activity. So there is no idle-power argument for loading it.

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

The cause was that Firefox 156 reaches for the camera through the PipeWire portal
instead of opening the device directly, and that path yielded nothing. Two runs of
the same page on a scratch profile, with only this pref differing:

```
media.webrtc.camera.allow-pipewire=true   ->  no answer in 25 s, no devices
media.webrtc.camera.allow-pipewire=false  ->  Capture Devices: 1
                                              GUM_OK: Apple Facetime HD
```

> **Still true as of 2026-09-20**, on firefox 156.0-1.fc44,
> xdg-desktop-portal 1.22.1, pipewire 1.6.9, wireplumber 0.5.17. Everything
> under Firefox was checked that day and behaves correctly — see
> [open issues](#open-issues). Note that the pref only takes effect on a
> restart; changing it in `about:config` and testing in the same session
> measures the old value.

**The fix:** set **`media.webrtc.camera.allow-pipewire`** to **`false`** and
restart Firefox. Putting it in `user.js` rather than `about:config` applies it at
every start and survives a `prefs.js` reset:

```bash
echo 'user_pref("media.webrtc.camera.allow-pipewire", false);' \
  >> ~/.config/mozilla/firefox/*.default-release/user.js
```

Note the profile path: Firefox 156 keeps profiles under `~/.config/mozilla`, not
`~/.mozilla`.

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

### No WPA3 under Linux — macOS does it on the same hardware

**This is a driver and firmware limit, not a limit of the silicon.** macOS on this
very machine connects to a WPA2/WPA3 network without trouble. Under Linux the
chip is driven by `brcmfmac` with the blob from `linux-firmware`, and that
combination offers no WPA3 at all. `iw list` prints the whole of what it offers:

```
Supported extended features:
	* [ CQM_RSSI_LIST ]
	* [ DFS_OFFLOAD ]
```

No `SAE_OFFLOAD`, and `Supported commands` has `connect` but neither
`authenticate` nor `external_auth` — and those are the only two routes by which
`brcmfmac` can do SAE. The blob is `brcm/brcmfmac4350c2-pcie`, which reports
itself as `Nov 26 2015 ... version 7.35.180.133`; WPA3 was ratified in 2018.
Protected management frames *are* supported (`CMAC` appears among the ciphers),
so that side is fine.

**Extracting the macOS firmware does not help — checked, 2026-09-20.** The idea
was obvious and wrong: macOS drives this chip with **7.35.180.119**, which is a
revision *older* than the 7.35.180.133 already loaded here. The assumption that
Apple ships something newer comes from T2 Macs, where `linux-firmware` carries
nothing at all; it does not transfer to a chip that is supported upstream.

The gap is in the driver, not the blob. `brcmfmac` does WPA3 through the
firmware's `sae_ext` feature — the firmware advertises SAE, and the driver then
hands authentication to `wpa_supplicant`. That was implemented for BCM4345 and
BCM43455, the Raspberry Pi parts. No firmware available for BCM4350 advertises
it.

And macOS does SAE **in the host**. The same firmware cannot even offload the
WPA2 four-way handshake, yet WPA2 works perfectly here, because `brcmfmac` runs
that handshake itself in `wpa_supplicant`. Apple's driver handles SAE the same
way; that path simply has not been written in `brcmfmac` for this chip. Closing
the gap means a kernel patch, not a file swap.

The arrangement here is a separate WPA2 SSID on the same router, set up
deliberately for this machine. It associates instantly and needs nothing
special.

> **KDE will create a WPA3-only profile for a mixed network.** If the SSID
> advertises both `psk` and `sae`, the applet picks `sae`, and the connection
> then fails with `wpa_supplicant: WPA: Failed to select authenticated key
> management type` — no association is even attempted, and NetworkManager
> eventually reports the misleading `ssid-not-found`. Fix the profile rather
> than the router:
>
> ```bash
> nmcli con modify "<SSID>" 802-11-wireless-security.key-mgmt wpa-psk
> ```

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

1. **Package C-states never go below C3 — measured, and not fixable from the OS
   side.** This is what makes S0ix, and therefore a low-power `s2idle`,
   impossible. `intel_pmc_core` exposes the whole picture in
   `/sys/kernel/debug/pmc_core/` (root only):

   ```
   $ cat package_cstate_show
   Package C2 : 445265455
   Package C3 : 1633589593
   Package C6 : 0        <- and C7 through C10 likewise
   $ cat slp_s0_residency_usec
   0
   ```

   `pch_ip_power_gating_status` names what stays powered:

   | block | what is behind it |
   |---|---|
   | `XHCI` | the USB ports |
   | `SPB` | the Thunderbolt root port (`00:1c.4`) |
   | `SPC` | the Wi-Fi and camera root ports (`00:1d.0`, `00:1d.1`) |
   | `LPSS`, `SPI` | the buses the keyboard and trackpad live on (`applespi`) |

   `ltr_show` has exactly one block declaring a real latency requirement —
   `SOUTHPORT_C`, 61 µs — which is those same Wi-Fi and camera ports.

   Three explanations were tested and none survived:

   - **`facetimehd`**, the out-of-tree camera driver. Unloaded: `SPC` stayed on,
     C6 stayed zero.
   - **`brcmfmac`**. Same.
   - **Runtime PM disabled on the endpoints.** `00:02:00.0` and `00:03:00.0` sit
     at `power/control=on`, so they never leave D0. Setting both to `auto`
     changed nothing — the drivers hold a runtime PM reference and never
     suspend. Unbinding them does not help either: a PCI device with no driver
     stays in D0, so the root ports above it cannot suspend.

   So it is not one misbehaving device. At least four blocks stay awake, and one
   of them, `LPSS`/`SPI`, carries the keyboard and trackpad — it cannot be
   powered down on a machine anyone is using. Fixing runtime PM for Wi-Fi and
   the camera would still leave XHCI, Thunderbolt and SPI holding the package at
   C3.

   The same firmware gap shows up one level down: **every PCI device on this
   machine has `d3cold_allowed=0`.** Nothing is permitted to actually lose power,
   only to idle in `D3hot`, because D3cold needs ACPI power resources (`_PR3`)
   that the firmware does not declare. Alpine Ridge illustrates it — four of its
   seven functions sit in `D0` with the driver blacklisted, and loading the
   driver leaves them in `D0` while costing half a watt (see
   [Disabling Thunderbolt](#disabling-thunderbolt-anyway)).

   Above all that, the firmware gives the OS no way in: no `PNP0D80` device, so
   nothing to call to enter S0ix, and part of the PMC is walled off —
   `pll_status` answers `Access denied: please disable PMC_READ_DISABLE setting
   in BIOS`, a setting no Mac has. S0ix itself is out of reach — and macOS never
   used it either: Intel Macs sleep in S3. What *is* reachable is the S3 and
   s2idle drain, because the devices that stay powered (Thunderbolt, camera,
   Bluetooth, possibly the SSD and Wi-Fi) have firmware power-off methods that
   Linux simply never calls — see
   [the ACPI chapter](#what-the-acpi-tables-say--two-sleep-paths-linux-walks-neither-2026-09-21).
   Until those experiments are run, `deep` at 1.37 W is the measured floor and
   has been the default sleep since 2026-09-21; hibernation stays as the
   backstop on battery.
2. **Resolved: the microphone is fine.** This entry used to say the recording
   level was very low. It is not — a Telegram call came through normally, and the
   mixer needs nothing done to it: `Internal Mic Capture Volume` is already at
   63/63 (+12 dB), `Internal Mic Boost` at 2/2 (+20 dB), the capture switch on,
   `Capture Source` on `Internal Mic`, and PipeWire's own source volume at 100%
   and unmuted under the `analog-stereo` profile. Where the original claim came
   from is not recorded, so it may have predated the CS8409 driver being set up
   properly. Nothing to do.
3. **Resolved: Wi-Fi on the main router.** Not a defect. That SSID is WPA3, and
   `brcmfmac` with the stock firmware has no WPA3 — see
   [above](#no-wpa3-under-linux--macos-does-it-on-the-same-hardware). A separate
   WPA2 SSID on the same router serves this machine. macOS manages WPA3 on the
   same hardware, but not through newer firmware — its blob is a revision older
   than the one Linux loads. It does SAE in the host, and `brcmfmac` has no host
   SAE path for this chip. A driver patch, not a firmware swap.
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
7. **Firefox finds no camera over PipeWire — and everything beneath Firefox is
   innocent.** The workaround stands. What was established on 2026-09-20, by
   running the exact call sequence Firefox uses and a control alongside it:

   - **Firefox calls `OpenPipeWireRemote` but never `AccessCamera`.** With
     `G_MESSAGES_DEBUG=all` on `xdg-desktop-portal.service`, a failing attempt
     logs `Adding registered host app 'org.mozilla.firefox'` and nothing else;
     the `Camera: sending response` line that an answered `AccessCamera` always
     prints never appears. The client it creates is visible in `pw-dump` with
     the properties only the portal sets: `access: portal`,
     `app_id: org.mozilla.firefox`, `media_roles: Camera`.
   - **That skip is legal and harmless.** `handle_open_pipewire_remote` in
     `src/camera.c` checks only the permission store, where
     `org.mozilla.firefox` is already `yes`, so the call succeeds.
   - **And it makes no difference to permissions.** The portal deliberately
     hides every node (`PW_PERMISSION_INIT (PW_ID_ANY, 0)`) and leaves the
     granting to WirePlumber. Running both sequences by hand, with and without
     `AccessCamera`, WirePlumber logs the same thing either way:

     ```
     find-portal-access.lua: Setting portal camera permissions to all
     wp-permission-manager:  Updating permissions on client 68: any=rwxml
     ```

   So the portal answers correctly, the permission store is right, WirePlumber
   grants the camera, and the node is there (`wpctl status` shows
   `Apple Facetime HD (V4L2)`). The fault is in Firefox's own PipeWire camera
   path. The next step, if anyone wants it, is Firefox's side of the story:
   `MOZ_LOG="CamerasChild:5,CamerasParent:5"`, as in
   [section 6](#6-firefox-notfounderror-with-a-camera-that-works).

   **Four diagnoses of this were wrong before the above, and three of them were
   committed.** Worth keeping as a set, because each was convincing:

   - *"An unsandboxed Firefox has an empty app ID."* It is not empty — the
     portal derives it from the process tree and logs
     `Adding registered host app 'org.mozilla.firefox'`. A shell under Konsole
     gets `org.kde.konsole` the same way.
   - *"`AccessCamera` never sends a `Response`."* It does. `gdbus call` exits as
     soon as it has the request handle, leaving nothing to receive the signal.
   - *"`GTask ... finalized without ever returning` — the portal drops the
     task."* Normal here: the thread func has no callback and no return value,
     and emits the response before returning.
   - *"The camera works now, the workaround is obsolete."* Measured without
     restarting Firefox, so the old pref value was still in force. The portal
     log showing no camera activity at all was the tell, and it was ignored.

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

   The free physical check that does work is temperature: `s2idle` holds the case
   warm at 3.8 W, a hibernated machine is cold to the touch within minutes. The
   trackpad is **not** such a check, whatever earlier revisions of this file said
   — see the hibernation chapter.

10. **Resolved: what actually blocked s2idle → hibernation.** The `brcmfmac`
    sleep hook, and nothing else. It is needed because the BCM4350 does not
    survive a loss of power, so the module has to come out before the image is
    written; but the hook failed in two different ways, and both look like a
    resume fault or a bad `resume_offset` rather than a hook fault. That is why a
    whole session went into `resume_offset`, where nothing was ever wrong.

    **Wrong directory — the hook never ran at all.** `systemd-sleep` reads
    `/usr/lib/systemd/system-sleep/` and only that. There is no
    `/etc/systemd/system-sleep/` for it, unlike `/etc/systemd/system/`, and a
    hook placed there is silently never executed, with no error anywhere. The
    2026-09-20 12:06 attempt ran that way: nothing unloaded the module, and the
    next boot reported `PM: Image not found (code -22)`.

    ```bash
    strings /usr/lib/systemd/systemd-sleep | grep system-sleep   # one path only
    ```

    **Wrong condition — the hook fired in the wrong phase.** The obvious
    `case "$1/$2" in pre/*) ... post/*)` breaks `suspend-then-hibernate`, because
    the second argument stays `suspend-then-hibernate` through *both* phases; the
    phase is readable only from `SYSTEMD_SLEEP_ACTION`. The pattern therefore
    fires four times per cycle instead of two, and coming out of s2idle it
    reloads the module exactly as the image write starts:

    ```
    11:35:11.446  usbcore: registered new interface driver brcmfmac   <- post loads it
    11:35:11.628  Starting NetworkManager.service...
    11:35:11.976  modprobe: FATAL: Module brcmfmac is in use.         <- pre cannot unload it
    11:35:11.980  Performing sleep operation 'hibernate'...
    11:35:12.070  brcmfmac: brcmf_c_process_clm_blob ...              <- firmware loading
    11:35:12.227  Filesystems sync: 0.066 seconds
                  <- journal ends here
    ```

    Both cured by commit `e3859cb`: filter on `SYSTEMD_SLEEP_ACTION`, keep the
    file in `/usr/lib`. The listing in
    [Wi-Fi after `deep` and after hibernation](#3-wi-fi-after-deep-and-after-hibernation)
    is the fixed version, extended on 2026-09-21 to the suspend phase (needed for
    `deep`) with a retry around the unload, because acting on the suspend phase
    brings back exactly the post-then-pre sequence above on every timer wake.
    Everything documented in issue 8 was measured *after* this fix and therefore
    described a machine that already worked.

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

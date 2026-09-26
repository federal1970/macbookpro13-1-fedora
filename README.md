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
| Kernel | `7.2.6-200.fc44.x86_64` |

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
| **Suspend / resume** | **Works with the lid shut, `deep` (S3) drains ~0.5 W over a night** | [Kernel parameters + boot-time script](#sleep-and-hibernation); `s2idle` costs 4 W and S0ix is out of reach, so the default sleep is `deep` since 2026-09-21 |
| **Hibernation** | **Works** | [Swap file, `resume=`, and a `brcmfmac` sleep hook](#hibernation--the-intended-solution). Resume on LUKS works; the passphrase is asked for at power-on |
| **Audio (Cirrus CS8409)** | **Fixed** | [Out-of-tree DKMS driver](#audio-cirrus-cs8409) |
| **Camera (FaceTime HD)** | **Fixed** | [Firmware extraction + DKMS driver + two source fixes](#camera-facetime-hd): a kernel 7.2 build error and missing buffer timestamps. Firefox needs [one pref](#6-firefox-notfounderror-with-a-camera-that-works) on top |
| Caps Lock as layout switch | Configurable | [keyd](#caps-lock-as-a-layout-switch) |
| Microphone | Works | Nothing to set; the earlier "very low level" note was wrong — see [open issues](#open-issues) |
| **Battery charge limit** | **Works** | [SMC key `BCLM` through a patched `applesmc`](#battery-charge-limit-smc-bclm): the SMC stops charging at the limit on its own, verified 2026-09-22 |

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
a day — and has been the default sleep since 2026-09-21; a night in it measured
**0.5 W**. Since 2026-09-22 the lid asks for plain sleep on battery as well:
timed hibernation cannot fire under `deep` with the lid shut ([the RTC alarm
does not wake S3](#the-rtc-alarm-does-not-wake-s3-while-the-lid-is-shut-2026-09-22)),
and at 0.5 W it is not needed. Hibernation stays for the idle timeout on
battery with the lid open, and for `systemctl hibernate` by hand. Both halves
work — the hibernate half took a while to believe, and `deep` was written off
for a day on a bad measurement; both stories are in [open issues](#open-issues).

### Why there is no "sleep, then hibernate" here — and why it does not matter

macOS on this machine does what it calls Standby: a while in S3, then a timed
transition to hibernation. Linux has the same thing — `suspend-then-hibernate`
in systemd, "Standby, then hibernate" in KDE — and it was in use here for two
days. It cannot work on this hardware under `deep`. To go from sleep to
hibernation the machine has to wake itself on an RTC alarm, write the image
and power off, and this Mac's SMC does not let an RTC alarm wake S3 while the
lid is shut: the same clamshell rule macOS applies, a closed laptop without
an external display and power wakes for nothing but the lid. macOS gets past
it through its own SMC driver and a firmware path the ACPI tables expose
only to macOS ([two sleep paths, Linux walks neither](#what-the-acpi-tables-say--two-sleep-paths-linux-walks-neither-2026-09-21)).
The night of 2026-09-21 showed it: the alarm set for 23:02 did nothing, and
the hibernation happened at 06:46 when the lid was opened — an image write
and a LUKS prompt as the first thing in the morning.

Under `s2idle` the same alarm fired fine, because there the EC stays alive
and its interrupt reaches the kernel without the SMC's consent — and at
s2idle's 4 W a hibernation after 15 minutes was genuinely needed. Under
`deep` it became impossible and unnecessary in the same step: 0.4–0.5 W over a
night is macOS's own Standby figure, not a level worth fleeing into
hibernation from. So sleep is `deep` and is entered by shutting the lid;
hibernation is kept where the lid cannot interfere — the idle timeout with the
lid open, and by hand.

### Sleep, then hibernate from Linux: what the SMC allows (2026-09-23)

The battery cap worked because the SMC had a key for it and only the write
was missing. Could the same trick give Linux macOS's Standby — a timed wake
from S3 with the lid shut? A day of probing says no, and here is the evidence
so nobody has to repeat it.

**The SMC's clock keys exist and refuse to be set.** A dump of all 798 keys
([`tools/smc-dump-keys.sh`](tools/smc-dump-keys.sh)) has `CLKT` (ui32, a clock
that advances one per second while awake, base unrelated to wall time),
`CLWK` (ui16, reads `ffff`), `CLSD` (ui16, 0), `WKEN`/`WKTP` (ui8, 0) and
`MSWr` (the last wake reason). The SMC flags `CLWK`, `CLKT`, `CLSD` and `WKTP`
as readable *and* writable ([`tools/smc-wake-probe.sh`](tools/smc-wake-probe.sh));
`WKEN` and `MSWr` are read-only. Yet every write to `CLWK` — 120 and 2 as a
relative count, `CLKT + 120` as an absolute time — came straight back as
`ffff`, with no error from the driver: the key's handler (flag `0x10`, "func")
validates and drops it. Two lid-shut sleeps in between held for two hours and
half an hour respectively, so nothing was armed by accident.

**The EC has a wake-enable bank, and it names a wake timer.** The DSDT's
`EmbeddedControl` region (the SMC's ACPI face) at `0x68`–`0x69` is the bank
the `_PSW` methods write: `EWLO` lid-open (bit 0, what `LID0._PSW` sets),
`EWLC`, `EWAI`/`EWAR` ac in/out (`ADP1._PSW`), `EWPB` power button, and at
`0x69` bit 3 **`ENWT`** — the one name in the bank that is not `EW??`, "enable
wake timer", which nothing in the tables ever sets. `0x6c`–`0x6d` is the same
layout as *last wake* reasons (`LWWT` included), `0x64`–`0x65` a third copy
(`SW??`) that reads `cf04` all the time: lid both ways, adapter both ways,
bits 6–7, and `PM` — and no `WT`. Fedora does not build `ec_sys`, so
[`tools/ec_poke/`](tools/ec_poke/) is a hundred-line module that exposes
EC bytes through `/sys/kernel/ec_poke/` via the kernel's own `ec_read()` /
`ec_write()`.

**`ENWT` is accepted and changes nothing.** [`tools/ec-wake-timer-test.sh`](tools/ec-wake-timer-test.sh)
sets it (`0x69: 00 -> 08`), arms a plain RTC alarm three minutes ahead, and
the lid is shut: S3 held for twelve and a half minutes until the lid opened.
The RTC did fire — `ff_rt_clk` in `/sys/firmware/acpi/interrupts/` shows its
`STS` bit latched — the SMC just did not act on it, cleared `ENWT` on the way
out, and reported the lid as the wake reason (`LW = 0100`,
[`tools/ec-wake-banks.sh`](tools/ec-wake-banks.sh)). The `SW` bank looks like
the effective mask the SMC keeps for itself, and a write there is refused
(`0x65: 04 -> 04`).

So the clamshell rule lives inside the SMC, behind keys whose handlers reject
everything an unprivileged write can offer, and macOS gets through with a
protocol the ACPI tables do not describe. Without SMC documentation or a
trace of macOS's SMC traffic on this model there is nowhere left to push, and
at 0.4–0.5 W a night there is no reason to. Closed.

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
# LID0 is the only thing that wakes this machine from deep. It came up disarmed
# after a hard reset on 2026-09-22 (a lid shut in that state cannot be woken),
# so arm it here every time instead of trusting the default.
if grep -q "^LID0.*disabled" /proc/acpi/wakeup; then
  echo LID0 > /proc/acpi/wakeup
fi
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
the first try and caused no spurious wake in any of the six S3 cycles that
evening (two to seven minutes each); whether it stays quiet over a night is
the remaining check.

**The other half, found on 2026-09-22: armed, `LID0` also means the machine
cannot sleep with the lid open.** The EC treats `EWLO` as a level, not an edge:
an already-open lid *is* the wake condition, so an S3 entered with the lid open
ends the moment it begins. Every such case in the journal did exactly that —
the 07:03 and 11:51 suspends, twenty-two KDE idle suspends that afternoon, and
one more after a clean reboot with the Thunderbolt port hidden by the firmware,
which is what ruled the port out as the cause. The alternative is worse:
`LID0` disarmed and the lid open, `systemctl suspend` at 20:00 — the machine
never came back, the power button did nothing, and the journal stops at the
hook's `brcmfmac` unload, so whether it slept and could not be woken or never
reached S3 the log cannot say. It took a hard reset, and after that reset
`LID0` came up *disarmed*, the one state in which a shut lid is a trap. Hence
the two rules now in force: the fix script arms `LID0` unconditionally, and
this machine is put to sleep by shutting the lid and by nothing else — KDE's
battery idle action must not be "sleep" (its default of 15 minutes is what ran
the afternoon's loop). The one run that does not fit is the 2-minute lid-open
run of 2026-09-21 22:15 that held until its alarm; the `LID0` state during it
was not recorded.

What replaces the idle sleep is hibernation, which does not care about the lid
at all: the machine powers off and the power button boots it, LUKS passphrase
and all. A manual `systemctl hibernate` with the lid open on 2026-09-22 20:22
went through cleanly (image written, 92 s of nothing in the journal, passphrase
asked, session back, Wi-Fi reloaded by the hook), so the battery profile now
reads `AutoSuspendAction=2` (hibernate) after 15 minutes idle, mains does
nothing, and the lid does plain `deep`. Verified against powerdevil's enum:
`Hibernate = 2`, `Shutdown = 8` — which is also why the critical-battery
action set earlier is a shutdown, not the hibernation this file used to claim.

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

### Hibernation costs next to nothing to hold — once it really powers off

Measured 2026-09-20 with the battery gauge, charger unplugged, over a 64 min 28 s
hibernation (`tools/hib-battery-test.sh`):

| state, over that hour | cost |
|---|---|
| **hibernated** | **nothing measurable** — the gauge read 26000 µAh *higher* afterwards |
| `s2idle` | 4.12 Wh, about 8% of a full battery |
| awake and idle | 8.80 Wh |

The gauge reading rises because the "before" sample is taken under an ~8 W load,
with the cell voltage sagging, and the "after" sample follows an hour at rest.
`capacity` agrees: 77% before, 78% after. What that measurement proves is a
bound, not a zero: the noise, ±26000 µAh, is about 0.3 Wh, so anything under
roughly 0.3 W hides in it. `s2idle` would have drawn 335000 µAh in the same
window.

**The firmware's S4 is not off.** With the default `HibernateMode=platform`
the kernel writes the image and then asks the firmware for ACPI S4 — and in
Apple's S4 the Force Touch trackpad still clicks. After `systemctl poweroff` it
does not. So the topcase rail (keyboard and trackpad, the same SPI device) stays
up in S4, and whatever else Apple leaves powered with it is unknown; the
one-hour measurement above was taken in that state and could not see it. The
journal is no help here: it records the state that was *requested*. The
trackpad is the instrument.

The fix is to have the kernel power the machine off itself after writing the
image, instead of asking the firmware for S4 — `HibernateMode=shutdown` in
`/etc/systemd/sleep.conf`, listed [below](#4-automatic-sleep--hibernate).
Resume is unchanged: the initramfs finds the image at `resume_offset` on the
next boot exactly as before. Verified 2026-09-21 22:33: image written, machine
off, trackpad dead, power key, passphrase, session restored with Wi-Fi. An
overnight `charge_now` comparison in this mode is still to be done.

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

**Overnight it is 0.5 W, not 1.37 W.** The 30-minute figure above rests on
55 mAh from a gauge whose before/after bias is about 26 mAh, so it was really
anywhere between 0.7 and 2 W. The night of 2026-09-21 settled it: lid shut on
battery from 22:47 to 06:46, one continuous S3 (see the next section for why it
never hibernated), `charge_now` 3364 → 3030 mAh:

| | |
|---|---|
| time in S3 | 7 h 59 min |
| used | 334 mAh ≈ 4.0 Wh, including one hibernate-and-resume transition (~0.32 Wh) and a minute awake |
| **S3 alone** | **≈ 0.46–0.50 W** |
| a full battery (46 Wh) in S3 | ~4 days |

That is inside macOS's 0.3–0.5 W on the same hardware. The firmware power-off
methods of the next chapter would still shave something, but there is no longer
a 3× gap to close.

**Second night, 2026-09-22 → 23, same answer.** Lid shut at 20:31 on battery,
opened at 08:42, one continuous S3 of 12 h 10 min, not a single journal line in
between, woken by the lid, Wi-Fi back, no errors. The before figure this time
is an estimate: `upowerd` stopped writing its history file at the manual
hibernation of 20:22 and only resumed sampling in memory at the wake, so the
last on-disk reading is 60.8% at 20:22, followed by the hibernate-and-resume
cycle and seven minutes awake at 7.5 W (about 2.5% together), giving ~58%
at the lid. After: 46.2% at 08:42, `charge_now` 1788 mAh a minute later.

| | |
|---|---|
| time in S3 | 12 h 10 min |
| used | ~12% ≈ 5.3 Wh, ±1.5% on the estimated start |
| **S3 alone** | **≈ 0.4–0.5 W** |

A 3-hour lid-shut sleep in the afternoon of 2026-09-22 had come out at about
0.8 W (5.2% over 2 h 55 min from upower's own log, both readings clean), so the
honest range for this machine in S3 is 0.4–0.9 W; the two full nights sit at
the low end, and what makes a daytime sleep cost more — a warmer room, the
state of charge, gauge bias on a short interval — has not been separated.

### The RTC alarm does not wake S3 while the lid is shut (2026-09-22)

The same night was meant to be a 15-minute `suspend-then-hibernate`. It was not:
the journal has one `suspend entry (deep)` at 22:47:28 and the next line is the
S3 wake at 06:46:48, when the lid was opened. The RTC alarm systemd had set for
23:02 did nothing. systemd then found the delay long elapsed and hibernated on
the spot, so opening the lid in the morning led straight into an image write
and a LUKS prompt — the worst of both arrangements.

It is the lid, not the alarm. The evening before, a hands-off run with the lid
open and a 2-minute delay woke at exactly 2 minutes; an identical run with the
lid shut woke only when the lid was opened at six minutes (misread at the time
as "the user opened it early"); every `rtcwake` run of `tools/deep-test.sh`,
lid open, woke on time. Under `s2idle` on 2026-09-20 the 15-minute alarm fired
with the lid shut, because there the RTC interrupt is an ordinary wake IRQ and
no firmware is involved; from S3 the wake goes through the SMC, and a closed
MacBook does not wake — the same clamshell rule macOS applies unless an
external display and power are attached. Three observations, no counterexample.
A fourth run the same morning (lid shut 06:55, opened 07:02) adds nothing
either way: it was over before the 15-minute alarm was due.

What follows from it, together with the 0.5 W above: under `deep`, timed
hibernation on battery cannot work with the lid closed, and it is not needed.
Plain `deep` costs about 8% of the battery per night. **Done 2026-09-22:** the
battery power profile went back from `Standby, then hibernate` (set on
2026-09-20 for the 4 W `s2idle`) to plain `Standby` — the `SleepMode=3` line
is gone from `[Battery][SuspendAndShutdown]` in `~/.config/powerdevilrc`, the
critical-battery action (`BatteryCriticalAction=8`) stays — which, checked
against powerdevil's enum on 2026-09-22, is *shut down*, not hibernate
(`Hibernate = 2`, `Shutdown = 8`); earlier revisions of this file had it
wrong — and
`sleep.conf` is untouched so a manual `systemctl hibernate` still powers off.
Chapter [4 below](#4-automatic-sleep--hibernate) is kept as the record of how
the timed variant was set up and verified.

> **Do not chain sleep transitions.** The morning's second test suspended the
> machine again 3 s after the lid wake (S3 wake, `brcmfmac` loaded, unloaded,
> S3, wake — 13 s in all). On that resume the Thunderbolt root port `00:1c.4`
> and everything under it came back `device inaccessible`, pciehp tore the
> tree down with a kernel WARNING in `xhci_pci_remove`, and from that second
> PID 1, `logind` and `polkitd` stopped answering in time: the thaw of
> `user.slice` timed out after 60 s, `sudo` took 93 s to open a session, the
> polkit helper timed out every 10 s. Wi-Fi never came back — the firmware
> loaded but every scan hit `brcmf_escan_timeout` and returned `EBUSY`. A
> `systemctl hibernate` never reached the kernel and two `reboot`s hung on the
> frozen user units; it took the power button. Ordinary lid cycles before and
> after were fine. Whether the dead root port is what starved PID 1 cannot be
> checked after a reset; the timing matches to the second.
>
> **Refined the same day: it is a 5-second S3 that does it, chained or not.**
> At 11:51 a suspend requested from the desktop (no lid involved, previous wake
> 21 minutes earlier) was woken after 5 s and came back with the same dead
> root port, the same pciehp teardown and the same `xhci_pci_remove` WARNING —
> though this time PID 1 stayed responsive and only USB-C died until the next
> reboot. Pairing every `suspend entry` with its S3 wake across all boots
> (`journalctl -k -b all`, `pm_test` dry runs excluded) gives four real S3s of
> 5 s — 09-19 20:07, 09-19 20:18, 09-22 07:03, 09-22 11:51 — and all four ended
> with `pcieport 0000:00:1c.4: ... device inaccessible`; all thirteen S3s of a
> minute or longer, up to eight hours, came back clean. The working theory is a
> wake arriving while the Thunderbolt controller is still powering down. The
> rule that follows: nothing may wake this machine within seconds of entering
> S3 — no `rtcwake -s 5`, no suspend from a script that has just resumed, and
> no plugging in the charger the moment the lid shuts.
>
> **And the evening's answer to what those instant wakes were:** the lid. Every
> one of them was an S3 entered with the lid open and `LID0` armed, and the EC
> reads "wake on lid open" as a level — see
> [`LID0` must stay armed](#2-boot-time-fix-script). The dead port was the
> casualty, not the cause: after a reboot with the port hidden by the firmware
> a lid-open suspend still woke in the same second. The afternoon's "screen
> turns off and comes back by itself" was KDE's 15-minute idle suspend on
> battery doing this twenty-two times in a row.

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

So the 4 W of s2idle and the 0.5–1.4 W of `deep` above were measured with
Thunderbolt, camera and Bluetooth powered and nobody asking the firmware to
switch them off. That is not
firmware that needs reverse-engineering — the methods exist, are named, and can be
called from the OS. The experiments, cheapest first, are listed at the end of
[`acpi/README.md`](acpi/README.md); the first one (`acpi_osi=!Darwin`) has been
run and is out, the rest have not.

### Hibernation — the intended solution

> **This works.** A full cycle was confirmed on 2026-09-20: about 894 MB written,
> the passphrase asked for at the next power-on, and the session restored. What
> "power cut" meant took another day to get right — see
> [above](#hibernation-costs-next-to-nothing-to-hold--once-it-really-powers-off). Earlier revisions of this file said the opposite; how that mistake was
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

> **Retired on 2026-09-22.** Under `deep` the RTC alarm does not wake the
> machine while the lid is shut, so the timed hibernation only ever fired on
> lid open — straight into an image write and a LUKS prompt — and a night of
> `deep` costs 0.5 W anyway. The battery profile is back on plain `Standby`;
> what follows is how the timed variant was configured and verified while it
> was in use, and the `sleep.conf` part still applies to manual hibernation.

Two things have to line up: the lid has to ask for `suspend-then-hibernate`
rather than a plain suspend, and the delay before the second phase has to be set.

**The delay, and the power-off** — `/etc/systemd/sleep.conf`:

```
[Sleep]
HibernateDelaySec=15min
HibernateMode=shutdown
```

`HibernateMode=shutdown` makes the kernel power off after writing the image
rather than request ACPI S4 from the firmware, which on this Mac leaves the
topcase powered. `/sys/power/disk` shows `[shutdown]` after the first
hibernation with it.

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

**Verified again under `deep` with the extended hook, 2026-09-21.** Two runs
with a `HibernateDelaySec=2min` drop-in, on battery. Hands off, lid open:
`suspend-then-hibernate` at 22:15:00, real S3, the RTC alarm woke it at
22:16:59, the hook reloaded `brcmfmac` and started NetworkManager, then stopped
it and unloaded the module again within two seconds (no `could not unload`),
hibernation at 22:17:03, resumed by the power button five minutes later with
Wi-Fi up. The lid-shut run before it did the same, except that the lid was
opened at six minutes: systemd found the delay already elapsed and hibernated
anyway, which is its documented behaviour, not a fault.

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

## Battery charge limit (SMC `BCLM`)

The battery has 521 cycles and 82% of its design capacity after nearly ten years,
so it is not in a hurry, but a laptop that lives on the charger ages its battery
fastest at 100%. ThinkPads expose `charge_control_end_threshold` in sysfs for
that; this machine exposes nothing: `/sys/class/power_supply/BAT0/` has no
threshold attributes, `applesmc` only *reads* SMC keys through `key_at_index`,
and there is no macOS left on the disk to run `bclm`.

The cap itself, however, is not a macOS feature. It is the SMC key **`BCLM`**
("Battery Charge Level Max", one byte, percent), which the macOS tool
[`bclm`](https://github.com/zackelia/bclm) writes. The SMC enforces it on its
own, the value survives reboots and operating systems, and only an SMC reset
puts it back to 100. So the only missing piece under Linux is a way to write
one SMC key.

### 1. Does this SMC have the key?

`applesmc` can walk the key table as root. [`tools/smc-keys.sh`](tools/smc-keys.sh)
selects each of the 798 indices in turn and prints the battery-related keys:

```console
$ sudo tools/smc-keys.sh
B0FC type=ui16 len=2 data=0f82
B0RM type=ui16 len=2 data=080f
BBIF type=ui8  len=1 data=0e
BCLM type=ui8  len=1 data=64
BRSC type=ui16 len=2 data=0034
CH0B type=hex_ len=1 data=00
```

`BCLM` is there and reads `0x64` = 100, no cap. `B0FC` (`0x0f82` = 3970 mAh) is
the full-charge figure and matches `charge_full` in sysfs (3931 mAh), so the
reads are sound. `B0RM` and `BRSC` read higher than the kernel's `charge_now`
and percentage did at that moment; not investigated, they are not needed here.

### 2. The patch

`applesmc` already writes keys — that is how it drives the fans and the keyboard
backlight — it just does not expose a generic write. The first version
(2026-09-22) added a private sysfs attribute, `battery_charge_limit`, on the
`applesmc` platform device. The version installed since 2026-09-23 does it the
way the kernel wants it done: the limit is the standard
`charge_control_end_threshold` property **on the battery itself**,
`/sys/class/power_supply/BAT0/charge_control_end_threshold`, added through a
*power supply extension* (`power_supply_register_extension()`), so it also
turns up in the battery's uevent and in UPower. One detail cost an hour: the
ACPI *battery hook* that ThinkPad and the WMI drivers use for exactly this
never fires here, because an Intel Mac's battery is an ACPI Smart Battery
(`ACPI0002`, driver `sbs`), not a `PNP0C0A` battery — the hook registered and
nothing happened. So the driver walks the registered supplies for a battery
and, if none is there yet, attaches on the battery's first property-change
notification. Writes are validated 1..100 and read back, because the SMC
silently drops values it does not accept (the wake-timer chapter below is a
whole story of that). Two more attributes on the platform device, `key_name`
and `key_data`, are local instruments for probing SMC keys — write a
four-letter key name to the first, and the second reads the key as hex or
writes hex of exactly its length; `key_name` reads back type, length and the
SMC's read/write flags. Root-only, no checks, not part of the upstream patch.
The diff against `drivers/hwmon/applesmc.c` from Linux v7.2, as installed:

```diff
--- drivers/hwmon/applesmc.c (Linux v7.2)
+++ applesmc.c
@@ -23,6 +23,7 @@
 #include <linux/kernel.h>
 #include <linux/slab.h>
 #include <linux/module.h>
+#include <linux/hex.h>
 #include <linux/timer.h>
 #include <linux/dmi.h>
 #include <linux/mutex.h>
@@ -33,6 +34,7 @@
 #include <linux/workqueue.h>
 #include <linux/err.h>
 #include <linux/bits.h>
+#include <linux/power_supply.h>
 
 /* data port used by Apple SMC */
 #define APPLESMC_DATA_PORT	0x300
@@ -64,6 +66,8 @@
 
 #define CLAMSHELL_KEY		"MSLD" /* r-o ui8 (unused) */
 
+#define CHARGE_LIMIT_KEY	"BCLM" /* r/w ui8, percent */
+
 #define MOTION_SENSOR_X_KEY	"MO_X" /* r-o sp78 (2 bytes) */
 #define MOTION_SENSOR_Y_KEY	"MO_Y" /* r-o sp78 (2 bytes) */
 #define MOTION_SENSOR_Z_KEY	"MO_Z" /* r-o sp78 (2 bytes) */
@@ -130,6 +134,7 @@
 	int num_light_sensors;		/* number of light sensors */
 	bool has_accelerometer;		/* has motion sensor */
 	bool has_key_backlight;		/* has keyboard backlight */
+	bool has_charge_limit;		/* has battery charge limit */
 	bool init_complete;		/* true when fully initialized */
 	struct applesmc_entry *cache;	/* cached key entries */
 	const char **index;		/* temperature key index */
@@ -621,6 +626,9 @@
 	ret = applesmc_has_key(BACKLIGHT_KEY, &s->has_key_backlight);
 	if (ret)
 		return ret;
+	ret = applesmc_has_key(CHARGE_LIMIT_KEY, &s->has_charge_limit);
+	if (ret)
+		return ret;
 
 	s->num_light_sensors = left_light_sensor + right_light_sensor;
 	s->init_complete = true;
@@ -669,6 +677,155 @@
 }
 
 /* Device model stuff */
+/*
+ * Battery charge limit
+ *
+ * The SMC key BCLM holds the maximum charge level in percent and the SMC
+ * enforces it by itself: charging stops there and the value is kept across
+ * reboots; 100 means no limit. It is the setting the macOS tool "bclm"
+ * writes. Expose it as charge_control_end_threshold on the battery through
+ * a power supply extension. The battery of an Intel Mac is an ACPI Smart
+ * Battery (sbs), which the ACPI battery hooks do not cover, so the battery
+ * is found by walking the registered supplies; if it is not there yet when
+ * this driver loads, the first property-change notification from it does
+ * the job.
+ */
+static const enum power_supply_property applesmc_battery_props[] = {
+	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
+};
+
+static int applesmc_battery_get_property(struct power_supply *psy,
+					 const struct power_supply_ext *ext,
+					 void *data,
+					 enum power_supply_property psp,
+					 union power_supply_propval *val)
+{
+	u8 limit;
+	int ret;
+
+	if (psp != POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD)
+		return -EINVAL;
+
+	ret = applesmc_read_key(CHARGE_LIMIT_KEY, &limit, 1);
+	if (ret)
+		return ret;
+
+	val->intval = limit;
+	return 0;
+}
+
+static int applesmc_battery_set_property(struct power_supply *psy,
+					 const struct power_supply_ext *ext,
+					 void *data,
+					 enum power_supply_property psp,
+					 const union power_supply_propval *val)
+{
+	u8 limit, readback;
+	int ret;
+
+	if (psp != POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD)
+		return -EINVAL;
+	if (val->intval < 1 || val->intval > 100)
+		return -EINVAL;
+
+	limit = val->intval;
+	ret = applesmc_write_key(CHARGE_LIMIT_KEY, &limit, 1);
+	if (ret)
+		return ret;
+
+	/* The SMC silently ignores values it does not accept. */
+	ret = applesmc_read_key(CHARGE_LIMIT_KEY, &readback, 1);
+	if (ret)
+		return ret;
+	if (readback != limit)
+		return -EINVAL;
+
+	return 0;
+}
+
+static int applesmc_battery_property_is_writeable(struct power_supply *psy,
+						  const struct power_supply_ext *ext,
+						  void *data,
+						  enum power_supply_property psp)
+{
+	return psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD;
+}
+
+static const struct power_supply_ext applesmc_battery_ext = {
+	.name = "applesmc-charge-limit",
+	.properties = applesmc_battery_props,
+	.num_properties = ARRAY_SIZE(applesmc_battery_props),
+	.get_property = applesmc_battery_get_property,
+	.set_property = applesmc_battery_set_property,
+	.property_is_writeable = applesmc_battery_property_is_writeable,
+};
+
+static struct power_supply *applesmc_battery;	/* the extended supply */
+static DEFINE_MUTEX(applesmc_battery_mutex);
+
+static int applesmc_battery_extend(struct power_supply *psy, void *data)
+{
+	int ret;
+
+	if (psy->desc->type != POWER_SUPPLY_TYPE_BATTERY)
+		return 0;
+
+	ret = power_supply_register_extension(psy, &applesmc_battery_ext,
+					      &pdev->dev, NULL);
+	if (ret)
+		return ret;
+
+	get_device(&psy->dev);
+	applesmc_battery = psy;
+	return 1;	/* one battery is enough, stop walking */
+}
+
+static void applesmc_battery_attach(struct work_struct *work)
+{
+	mutex_lock(&applesmc_battery_mutex);
+	if (!applesmc_battery)
+		power_supply_for_each_psy(NULL, applesmc_battery_extend);
+	mutex_unlock(&applesmc_battery_mutex);
+}
+
+static DECLARE_WORK(applesmc_battery_work, applesmc_battery_attach);
+
+static int applesmc_battery_notify(struct notifier_block *nb,
+				   unsigned long event, void *data)
+{
+	if (event == PSY_EVENT_PROP_CHANGED && !applesmc_battery)
+		schedule_work(&applesmc_battery_work);
+	return NOTIFY_OK;
+}
+
+static struct notifier_block applesmc_battery_nb = {
+	.notifier_call = applesmc_battery_notify,
+};
+
+static void applesmc_battery_init(void)
+{
+	if (!smcreg.has_charge_limit)
+		return;
+	power_supply_reg_notifier(&applesmc_battery_nb);
+	applesmc_battery_attach(NULL);
+}
+
+static void applesmc_battery_exit(void)
+{
+	if (!smcreg.has_charge_limit)
+		return;
+	power_supply_unreg_notifier(&applesmc_battery_nb);
+	cancel_work_sync(&applesmc_battery_work);
+	mutex_lock(&applesmc_battery_mutex);
+	if (applesmc_battery) {
+		power_supply_unregister_extension(applesmc_battery,
+						  &applesmc_battery_ext);
+		power_supply_put(applesmc_battery);
+		applesmc_battery = NULL;
+	}
+	mutex_unlock(&applesmc_battery_mutex);
+}
+
 static int applesmc_probe(struct platform_device *dev)
 {
 	int ret;
@@ -1066,6 +1223,84 @@
 	return count;
 }
 
+/*
+ * Generic key access for experiments: write a four-letter key name to
+ * key_name, then read key_data (hex) or write hex of exactly the key's
+ * length to it. Root only for the writes, like every other store here.
+ */
+static char applesmc_sel_key[5];
+
+static ssize_t applesmc_key_name_show(struct device *dev,
+				struct device_attribute *attr, char *sysfsbuf)
+{
+	const struct applesmc_entry *entry;
+
+	if (!applesmc_sel_key[0])
+		return sysfs_emit(sysfsbuf, "\n");
+	entry = applesmc_get_entry_by_key(applesmc_sel_key);
+	if (IS_ERR(entry))
+		return sysfs_emit(sysfsbuf, "%s (no such key)\n", applesmc_sel_key);
+	/* flags: 0x80 read, 0x40 write, 0x10 func */
+	return sysfs_emit(sysfsbuf, "%s type=%s len=%u flags=0x%02x%s%s\n",
+			  entry->key, entry->type, entry->len, entry->flags,
+			  (entry->flags & 0x80) ? " R" : "",
+			  (entry->flags & 0x40) ? " W" : "");
+}
+
+static ssize_t applesmc_key_name_store(struct device *dev,
+	struct device_attribute *attr, const char *sysfsbuf, size_t count)
+{
+	char name[5];
+
+	if (sscanf(sysfsbuf, "%4s", name) != 1 || strlen(name) != 4)
+		return -EINVAL;
+	memcpy(applesmc_sel_key, name, sizeof(applesmc_sel_key));
+	return count;
+}
+
+static ssize_t applesmc_key_data_show(struct device *dev,
+				struct device_attribute *attr, char *sysfsbuf)
+{
+	const struct applesmc_entry *entry;
+	u8 buf[APPLESMC_MAX_DATA_LENGTH];
+	int ret, i, n = 0;
+
+	if (!applesmc_sel_key[0])
+		return -EINVAL;
+	entry = applesmc_get_entry_by_key(applesmc_sel_key);
+	if (IS_ERR(entry))
+		return PTR_ERR(entry);
+	ret = applesmc_read_entry(entry, buf, entry->len);
+	if (ret)
+		return ret;
+	for (i = 0; i < entry->len; i++)
+		n += sysfs_emit_at(sysfsbuf, n, "%02x", buf[i]);
+	n += sysfs_emit_at(sysfsbuf, n, "\n");
+	return n;
+}
+
+static ssize_t applesmc_key_data_store(struct device *dev,
+	struct device_attribute *attr, const char *sysfsbuf, size_t count)
+{
+	const struct applesmc_entry *entry;
+	u8 buf[APPLESMC_MAX_DATA_LENGTH];
+	size_t hexlen;
+	int ret;
+
+	if (!applesmc_sel_key[0])
+		return -EINVAL;
+	entry = applesmc_get_entry_by_key(applesmc_sel_key);
+	if (IS_ERR(entry))
+		return PTR_ERR(entry);
+	hexlen = strcspn(sysfsbuf, "\n ");
+	if (hexlen != 2 * entry->len || hex2bin(buf, sysfsbuf, entry->len))
+		return -EINVAL;
+	ret = applesmc_write_entry(entry, buf, entry->len);
+	if (ret)
+		return ret;
+	return count;
+}
+
 static struct led_classdev applesmc_backlight = {
 	.name			= "smc::kbd_backlight",
 	.default_trigger	= "nand-disk",
@@ -1080,6 +1315,8 @@
 	{ "key_at_index_type", applesmc_key_at_index_type_show },
 	{ "key_at_index_data_length", applesmc_key_at_index_data_length_show },
 	{ "key_at_index_data", applesmc_key_at_index_read_show },
+	{ "key_name", applesmc_key_name_show, applesmc_key_name_store },
+	{ "key_data", applesmc_key_data_show, applesmc_key_data_store },
 	{ }
 };
 
@@ -1369,6 +1606,8 @@
 		goto out_light_ledclass;
 	}
 
+	applesmc_battery_init();
+
 	return 0;
 
 out_light_ledclass:
@@ -1398,6 +1637,7 @@
 
 static void __exit applesmc_exit(void)
 {
+	applesmc_battery_exit();
 	hwmon_device_unregister(hwmon_dev);
 	applesmc_release_key_backlight();
 	applesmc_release_light_sensor();
@@ -1415,5 +1655,5 @@
 module_exit(applesmc_exit);
 
 MODULE_AUTHOR("Nicolas Boichat");
-MODULE_DESCRIPTION("Apple SMC");
+MODULE_DESCRIPTION("Apple SMC (charge_control_end_threshold, key_name/key_data)");
 MODULE_LICENSE("GPL v2");
```

### 3. Build through DKMS

The module keeps its name, `applesmc`, and goes to `/extra`, which `depmod` on
Fedora prefers over the in-tree copy — the same mechanism the
[audio driver](#audio-cirrus-cs8409) relies on. Source directory
`~/dev/applesmc-bclm/` with the patched `applesmc.c` and:

`Makefile`:

```makefile
ifneq ($(KERNELRELEASE),)
obj-m := applesmc.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build
default:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
```

`dkms.conf`:

```
PACKAGE_NAME=applesmc-bclm
PACKAGE_VERSION=1.0
BUILT_MODULE_NAME[0]="applesmc"
BUILT_MODULE_LOCATION[0]="."
DEST_MODULE_LOCATION[0]="/extra"
AUTOINSTALL="yes"
```

```bash
sudo ln -sfn "$HOME/dev/applesmc-bclm" /usr/src/applesmc-bclm-1.0
sudo dkms install applesmc-bclm/1.0
sudo modprobe -r applesmc && sudo modprobe applesmc
modinfo -n applesmc            # must end in extra/applesmc.ko.xz
```

The keyboard backlight goes dark for a second while the module is swapped;
that is `applesmc` releasing and re-registering the LED.

### 4. Use and verify

```bash
cat /sys/class/power_supply/BAT0/charge_control_end_threshold    # 100 = no cap
echo 80 | sudo tee /sys/class/power_supply/BAT0/charge_control_end_threshold
```

Nothing to add at boot: the value lives in the SMC. `echo 100` removes the cap.
The proof is the charger: plugged in above the limit, `BAT0/status` must read
`Not charging` (or `Full`) and the percentage must stop rising. The cap never
discharges the battery down to the limit; it only stops charging there.

**Verified 2026-09-22.** Module from `extra/`, `battery_charge_limit` written
to 80 at 12:13 with the battery at 63%, charger plugged in. Charging ran at a
steady 1.55 A and stopped at 12:40:59:

```
$ cat /sys/class/power_supply/BAT0/status; cat /sys/class/power_supply/BAT0/charge_now
Full
3115000            # 79.2% of charge_full (3931000), charger still connected
```

`current_now` went to 0 and stayed there; the SMC cut the charge by itself, no
software involved after the one write. It stops a fraction below the number
(79.2% for 80), which is the gauge's granularity, not a bug. `upower` kept
saying `charging, 31 minutes to full` for a while after that: it estimates from
its own history and catches up on the next state change, so read sysfs, not
`upower`, when checking the cap.

### Kernel updates

The source is frozen at v7.2. DKMS rebuilds it for every 7.2.x, but the day
`dkms` fails on a newer series, fetch that kernel's `drivers/hwmon/applesmc.c`,
apply the diff above, replace the file and reinstall.

### Upstream

The patch as sent to linux-hwmon — the extension part only, without the probe
attributes — is
[`0001-hwmon-applesmc-Expose-the-SMC-battery-charge-limit.patch`](https://github.com/federal1970/macbookpro13-1-fedora/blob/master/applesmc-bclm/0001-hwmon-applesmc-Expose-the-SMC-battery-charge-limit.patch)
in this repository: 75 lines against v7.2, `checkpatch --strict` clean,
`depends on POWER_SUPPLY` in Kconfig, tested on this machine as described in
its message. Recipients: linux-hwmon@vger.kernel.org, the applesmc maintainer
(Henrik Rydberg, "odd fixes"), the hwmon maintainers (Guenter Roeck, Jean
Delvare), Cc linux-acpi and linux-kernel. Kernel patches go by plain-text
e-mail, not pull requests — `git send-email` with a Gmail app password does
it. Sent on 2026-09-23 16:41 CEST, archived at
<https://lore.kernel.org/linux-hwmon/20260923144117.295450-1-michi.szpakowski@gmail.com/>.

**Review, 2026-09-24:** Lukas Wunner pointed to a series by Jordan Brough
that had been on the list since 2026-09-13 and does the same thing better:
[`[PATCH v2 0/2] hwmon: (applesmc) add charge_control_end_threshold support`](https://lore.kernel.org/r/20260918175052.85461-1-jordan@brough.org)
extends the ACPI battery hooks to SBS batteries (the proper fix for what this
patch worked around by walking the supplies), uses the same power-supply
extension, and also drives `BFCL`, the MagSafe LED threshold, which has to sit
a few percent below `BCLM` or the LED stays amber. Rafael Wysocki had review
comments on the ACPI half; a v3 is due. This patch was withdrawn in favour of
that series, with two findings from this machine handed over: the SMC of a
MacBookPro13,1 has no `BFCL` key at all, so v2's unconditional LED write would
return `-EINVAL` after the limit had already been applied; and the SMC drops
writes it does not accept silently, so reading the key back is the only way
to report failure. The reply is at
<https://lore.kernel.org/linux-hwmon/20260924094544.324119-1-michi.szpakowski@gmail.com/>.
The local DKMS module stays until Jordan's series is in a Fedora kernel.

---

## Building a test kernel for this machine (2026-09-26)

Needed to give a Tested-by on the charge-limit series (its first patch changes
the ACPI core, which DKMS cannot touch), and useful for any future kernel
patch: build the exact Fedora kernel that is running, plus patches, with a
config trimmed to this machine, install it next to the stock one, boot it, keep
the stock entry as the default. Scripts in [`tools/kernel-test/`](tools/kernel-test/).

**Sources and config.** `dnf download --source kernel-<nvr>` and `rpm2cpio`
give the tarball, Fedora's `patch-7.2-redhat.patch` and `Makefile.rhelver`
(the patch `include`s it; without the copy the build dies at once). The
config is the running kernel's `/boot/config-*` cut down with
`make LSMOD=<lsmod snapshot> localmodconfig` to the modules actually loaded —
205 instead of 4846 — with BTF off (no `pahole` on the laptop), signing keys
cleared and `LOCALVERSION` **empty in the file**: with it set there *and* on
the command line the release came out `7.2.7-jordantest-jordantest`. The
trimmed config is [`laptop.config`](tools/kernel-test/laptop.config).

**Where to build.** On this i5-6360U the trimmed build takes 39 min 30 s
(129 CPU-minutes). A Fedora 44 VM with 8 vCPUs of a Ryzen 7 7840U did it in
8 min 12 s, so [`vm-build.sh`](tools/kernel-test/vm-build.sh) does the whole
thing on a build host and packs `boot/` and `lib/modules/<release>/` into a
`.tar.zst`. The VM sits behind libvirt NAT on a host that is on Wi-Fi, where a
bridge is impossible (802.11 refuses a second MAC per client), so the VM keeps
a reverse SSH tunnel to the laptop from a user-level systemd unit
(`ssh -N -R 2222:localhost:22`, `Restart=always`, `loginctl enable-linger`),
and `ssh buildvm` on the laptop is `localhost:2222`. Two things bit on the
way: Fedora's sshd may only bind ports labelled `ssh_port_t`, so the `-R`
port needs `semanage port -a -t ssh_port_t -p tcp 2222` on the laptop or
the forward fails silently; and `sshd` in a Fedora Workstation VM is off by
default.

**Install.** [`laptop-install.sh`](tools/kernel-test/laptop-install.sh)
unpacks the tarball, copies the modules, links a local build tree as
`/lib/modules/<release>/build` so the `kernel-install` DKMS hook can rebuild
the camera, audio and applesmc modules (they need a tree with the same
config and release; the tarball carries none), then `kernel-install add`
builds the initramfs and the boot entry with the parameters from
`/etc/kernel/cmdline`. `grubby --set-default` afterwards keeps the stock
kernel as the default.

**The trap that cost a boot.** The first attempt ended in emergency mode:
LUKS opened, root and swap mounted, then nothing worked. The tarball had been
unpacked under `/tmp` and `cp -a` had faithfully carried the `user_tmp_t`
SELinux label onto the whole module tree; after switch-root every `modprobe`
got `avc: denied { module_load }` — no keyboard, no graphics, 29 denials in
the journal, which `systemd-modules-load` reported as "Failed to find
module". `chown -R root:root` and `restorecon -R` on the tree fixed it and are
now in the script. Neither `kernel-install` nor `dracut` had objected.

**Result.** `7.2.7-jordantest` boots and runs this machine fully: zero
denials, Wi-Fi, sound, camera, the charge limit, `deep` sleep with the lid,
and the tunnel to the VM comes back on its own. The one visible difference is
systemd's `bpf-restrict-fs: Failed to load BPF object` — the price of BTF off,
harmless. Applying a series now means `vm-build.sh ... patch.mbox`, a few
minutes of incremental build, the install script, and a reboot.

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

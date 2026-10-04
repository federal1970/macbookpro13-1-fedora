# In-kernel Apple support for the CS8409: working notes

Target: MacBookPro13,1 (and 14,1), HDA codec Cirrus CS8409 `1013:8409`,
subsystem `106b:3300`, CS42L83 and SSM3515 behind the CS8409's I2C bridge.
Plan: a new board in the in-tree `sound/hda/codecs/cirrus/cs8409*.c`, not a new
driver. Working tree: `~/dev/cs8409-apple` (baseline = unmodified 7.2 driver,
built out of tree). Root steps go through `cs8409-dev` (this directory,
installed in `/usr/local/sbin`), run by hand with sudo.

## 2026-10-03: the loop works, and what the stock driver sees

`sudo cs8409-dev swap` with the unmodified in-tree module, PipeWire stopped:

- The HDA stack unloads and reloads live once `alsa-state.service` is stopped
  (alsactl's root daemon keeps `controlC0` open; that is also what makes
  `hwC0D0/reconfig` return EBUSY). SELinux (Enforcing) lets a module from the
  home directory load.
- The in-tree driver binds (`hdaudio:v10138409`) and finds nothing:
  `line_outs=0 speaker_outs=0 hp_outs=0`, no inputs, only the HDMI PCMs. No
  Apple entry in its quirk table, and every pin default on the codec reads "no
  connection" (`0x501000f0`, `0x402b20f0`, `0x40ab20f0`, `0x509000f0`,
  `0x50a000f0`) both before and after, so the pin configuration has to come
  from the driver.
- **The codec's subsystem id is not stable.** Booted by the firmware it reads
  `0x106b3300`; after the controller is re-probed it reads the factory
  `0x10138409`. The controller's PCI subsystem id is `8086:7270`, which is not
  model-specific. So a quirk keyed on the codec SSID works at cold boot and
  fails after any live reset; the board has to be recognised by DMI as well
  (`Apple Inc.` / `MacBookPro13,1`, board `Mac-473D31EABEB93F9B`).
- **The GPIOs survived that re-probe**: IO0–IO3 enabled, IO1 an output driven
  high (CS42L83 out of reset), only IO0's unsolicited enable dropped. A
  controller re-probe is therefore a lighter reset than S3, where the whole
  mask comes back 0. The driver must cope with both.
- Going back to the DKMS driver after a swap takes a reboot: it keys on the
  codec SSID.

## 2026-10-03: phase 1, speakers — works

Working tree commit `b4c9c6d` (277 lines on top of the unmodified 7.2 driver):
a board `CS8409_MBP131`, picked by codec SSID `106b:3300` or by DMI, with pin
configs for the two speaker pins, AppleHDA's ASP1 clock and slot values as one
static `cs8409_cir_param` table, and the four SSM3515 programmed over the
bridge I2C at init and powered from the PCM hook. 44.1 kHz only. The CS42L83 is
held in reset (GPIO1 low); no headphones, microphones or jack detection yet.

Tested by ear on the machine, module loaded with `cs8409-dev swap`:

| test | result |
|---|---|
| probe | fixup picked by SSID, `line_outs=2 (0x24/0x25) type:speaker`, PCM "CS8409/CS42L83 Analog", PipeWire s32le 2ch 44100 Hz on the speaker port |
| I2C | no error from any write to 0x14–0x17: all four amps answer, non-paged, in AppleHDA's order (the replay driver never ran that order) |
| first playback | sound, clean |
| after a runtime suspend of the codec (init runs again, amps reset and reprogrammed) | sound, no click, left is left and right is right |
| after an S3 lid cycle of one minute | sound, left and right correct, nothing in the kernel log |

So the slot order `{0, 2, 1, 3}` for the amps at 0x14..0x17 is right with the
generic parser copying the front pair to the second converter, the static
table is enough (no per-stream slot enable, sync or pad switching as AppleHDA
does), and resume needs nothing special: the init that runs at boot runs again.

Not yet looked at: the HP and mic paths the generic parser builds from the
firmware's pin defaults (0x2c, 0x3c, 0x44) while nothing drives them; a speaker
volume control (SSM3515 `DAC_VOL`); 48 kHz; what AppleHDA's verb 0x7f0 and
coefficients 0x6b/0x71 are for (not needed for sound).

## 2026-10-03, later: phase 2, CS42L83 — headphones, jack, internal mic

Working tree at `0246a07`; the diff against the unmodified 7.2 driver was kept
here as `cs8409-mbp131-wip.patch` until the series of 2026-10-04 replaced it
([`series-rfc-v1/`](series-rfc-v1)).

The CS42L83 is sub-codec 0 and goes through the driver's existing `cs42l42_*`
code. Its init table is the Dell CS42L42 one with AppleHDA's 44.1 kHz clocking
(PLL, serial-port rates, frame config); ASP2 slots and clocks, PLL2 left on and
DMIC1 enabled on the CS8409 side; pin configs for the headphone jack, the
headset mic and the internal mic; pin sense, the unsolicited handler and the
volume controls use this board's pins; the speakers are switched off by hand
while a headphone is in.

Tested by ear:

| test | result |
|---|---|
| CS42L83 after reset release | answers on I2C, id `42 a8 3a` rev `b0` |
| speakers | unchanged |
| internal mic | clean voice at about −24 dBFS, heard on playback |
| headphones | sound, correct sides, speakers silent, PipeWire on the headphone port, hardware volume control |
| unplug / plug / unplug with the codec awake | three events, three correct reports, PipeWire follows |
| S3 | as in phase 1 |

Three of the decode's open questions are answered:

- **Unsolicited responses** work with the driver's standard
  `cs8409_enable_ur()`. AppleHDA's verb `0x7f0` with payload `0xb7` on the
  function group is the same enable with tag `0x37`: with it the responses
  arrive as `0xdc00000x` instead of `0x0000000x`. Not needed.
- **Dell's jack flow** (tip/ring sense through `0x130f`/`0x1320`) works on this
  board as it is, while the codec is awake.
- The static table is still enough with both serial ports in use.

Open:

- **A plug while the codec idles is not noticed until the next sound.** The
  in-tree suspend puts the companion codec in reset; that is the same on the
  Dell boards. Five attempts to do better, none successful: standby instead of
  reset over runtime suspend (the `PDN_DONE` timeout warning is gone with it);
  unsolicited responses left on, with a resume request from the handler;
  verb `0x7f0`; the CS8409's ports and PLL2 stopped so that the function group
  reports clock-stop OK (the controller `00:1f.3` then does runtime-suspend,
  which it does not while the ports run, but the plug does not wake it); the
  function group kept in D0 through `.set_power_state`. What is established:
  the tip/ring sense source does not work in standby, the detect block on page
  `0x1b` does (`0x1b79` armed, `0x1b7b` read to clear), and with the codec held
  awake by a capture stream the CS42L83 in that standby state does pull GPIO 0
  on a plug. What kills the response once the driver's own suspend has run is
  not found. Fallback: no runtime suspend on this board.
- Headset type detection is not stable: the same headset was reported with a
  mic at init and without one on a live plug. AppleHDA waits 1.8 s after the
  tip sense before detecting the type.
- Headset microphone not tested. Speaker volume control, 48 kHz: not done.

## 2026-10-04: everything works; an RFC series

State (as of the evening, after the changes described at the end):
[`series-rfc-v1/`](series-rfc-v1), three patches against `tiwai/sound.git`
for-next (`12455e2b8`, whose three cs8409 files are those of 7.2). Patch 3 is
the tree that was tested, byte for byte, and the same sources are installed
here through DKMS. Working tree `~/dev/cs8409-apple`: `master` is the clean
code, the branch `diag` an earlier state with the instruments below.
checkpatch `--strict`: nothing on patches 1 and 2, four CamelCase checks on
existing coefficient names on patch 3. The patches carry `Assisted-by` and no
`Signed-off-by`: the kernel's `coding-assistants.rst` leaves that to the human
who sends them.

Every "open" point of the section above is closed, and the five failed attempts
listed there proved nothing: they were all made in a state the first row of
this table explains.

| symptom | cause | fix |
|---|---|---|
| a plug with the codec idle is not noticed | the HDA core drops unsolicited responses until the card is registered (`snd_hdac_bus_process_unsol_events`, `codec->registered`); the jack detection run from the build action raises its interrupt before that, nobody reads the CS42L83 status, GPIO 0 stays low and no later event makes an edge. Every idle test had been made right after a module load, in that state | a delayed work looks at the line once responses get through. Runtime suspend then works in its plainest form: CS42L83 in standby, tip sense interrupt of `0x1b79` armed, unsolicited response left on, function group in D3 |
| after S3 the CS42L83 reads as zeroes at the first init, and answers at the next | `spec->dev_addr` caches the CS8409 I2C address register and is never invalidated; the register is lost with the power. In-tree code | forget the cache on suspend (patch 1) |
| headphones right for one stream, distorted for the next | PLL and `SCLK_PRESENT` set once at init, Dell style; the serial port carries 44.1 kHz in a 48 kHz frame through the SRCs | blocks, PLL and clock switch per stream, at prepare and cleanup, as the ASoC cs42l42 driver and AppleHDA do |
| speakers for a fraction of a second and crackles at every start from idle | each runtime resume reset the CS42L83 and ran the type detection under the starting stream | standby keeps the jack state; the resume only undoes the standby and compares the tip sense. Full init only for the first init, after S3 (GPIO mask gone) and after a system suspend (reset line low) |
| headphones silent for a whole stream, about one S3 under music in three | `ASP_RX_DAI0_EN` was set before the switch from the oscillator to the bit clock; the receive side sometimes stayed deaf | enable the serial port channels after the clock switch, clear them at cleanup. 11 S3 cycles under music without a failure afterwards |
| "Timeout waiting for PDN_DONE" at a system suspend under a stream | the PLL still running when `cs42l42_suspend()` powers the ADC and HP down, which `cs42l42.h` forbids | stop the PLL first |
| an unplug in standby is noticed only at the next resume | an artefact of a mechanism that was not needed: the standby armed a second interrupt (page `0x1b`) and compared its debounced level, which lags the event by about 0.47 s | the mechanism is gone, see the last section: the ordinary tip sense wakes the codec by itself |

Also new: `Speaker Playback Volume` on the SSM3515 digital volume, 0.375 dB a
step, stopping at the −3 dB AppleHDA programs; the headset microphone works
(clean voice); the internal microphone is picked by the generic auto-mic, so
only with no headset microphone plugged.

Checked by ear and by the logs on the final build: speakers and their volume,
headphones from idle (6 of 6), plug and unplug under a stream and in standby,
both microphones, S3 from idle and under a stream, the volume keys.

Known limits: 44.1 kHz only; no headset buttons; the type detection, which is
the driver's existing cs42l42 code, took the headset for headphones 3 times in
about 30 plugs (AppleHDA waits 1.8 s before detecting).

How the last three rows were found, since nothing in the registers differed
between the good and the bad state:

- `/proc/asound/card0/cs42l83`: the CS42L83 registers, the CS8409 coefficients
  and the amps, readable without root (branch `diag`).
- Two debug controls that write one CS42L83 register or one CS8409 coefficient
  from user space (`amixer cset iface=CARD,name='L83 Poke Debug' reg,val`).
- A split by ear: with the ADC mixed into the headphone mixer
  (`MIXER_ADC_VOL` := 0) the user heard himself in the silent state, so the
  amplifier was alive and the music was not arriving.
- Five single writes nine seconds apart, each announced on screen with
  `notify-send`; the music came back at the first, `ASP_RX_DAI0_EN` off and on.
- A pitfall of the development loop itself: stopping and starting PipeWire
  around each module swap leaves KDE's `kded6` with a stale view of the sink,
  and the volume keys then move between two values only.
  `systemctl --user restart plasma-kded6.service` after a swap.

## 2026-10-04, later: what changed before the series was final

The code of the morning was read again against the kernel source, the ASoC
cs42l42 driver and the decode, and every resulting version was run on the
hardware. What that turned up, most important first:

| what was wrong | what was done |
|---|---|
| `MCLK_SRC_SEL` is 0, so the CS42L83 PLL that was started, waited for and stopped is never selected | PLL setup, start, lock wait and stop removed; headphones unchanged. What had cured the distortion was the per-stream order, not the PLL |
| the type detection took a headset for headphones: seated at an init 69 of 69 right, plug under a stream 4 of about 19 wrong. The Dell-derived table has no rise debounce on the tip sense, so the detection ran while the plug was sliding in | `TSENSE_CTL` := 0x85 (1 s, the ASoC default): 33 of 33 right since |
| a plug in standby was typed without the settle time; after an init with a headset in, the detection ran twice | dynamic debug showed the ordinary tip/ring sense wakes the codec from standby by itself (5 of 5 unplugs, 5 of 5 plugs) and that its debounced "plugged" starts the detection a second after the plug. The page `0x1b` interrupt and the polling in the resume were removed; a board flag makes the shared code skip the detection for a jack already in |
| the jack handler was not excluded from a runtime suspend; with the debounced "plugged" arriving 1.05 s after a plug and the autosuspend about 1.2 s after the wake, every plug in standby passed within 150 ms of it | the handler holds `pm_runtime_get_if_active()` for its run, and otherwise asks for a resume |
| the light way out of standby was chosen from two GPIO readbacks, which s2idle or a hibernation restore could satisfy with the rest gone | light way only for a runtime resume (`power_state` ON) that also finds one coefficient of the table; full init after any system sleep |
| the speaker PCM was advertised with four channels; PipeWire offered Surround 2.1 and 4.0 | `multiout.max_channels = 2`; only the stereo profile is offered |
| nothing serialised the jack handler against the PCM hooks | a mutex around the handler, the hooks and the volume put; a system suspend takes it too |
| `DEV_CFG3` was 0x0280 after a full init and 0x0080 after a light resume (the init verbs replayed on every init) | the verbs write 0x0280 |
| the "jack already in" skip made the state a latch | board flag only; the detection result is checked against the tip sense after the unmask; out of standby, any status but "plugged" ends a jack believed in |
| with `CONFIG_PM=n` the sync work never ran | a negative return of `pm_runtime_get_if_active()` means "no runtime PM, go on" |

Structure: the board's suspend and its sync work hang on two hooks in the spec
instead of board tests in common functions; `cs8409_suspend_i2c()` is the
shared tail of both suspend functions; header constants and `read_poll_timeout`
instead of literals and open loops. The series is three patches now: the cached
I2C address, the flag for a debounced tip sense in the shared code, the board.

The third row is the lesson of the first table again: the claim "the tip/ring
sense does not work in standby" dated from the tests made with the interrupt
line stuck, and a whole mechanism had been built on it.

Run on the hardware on the final sources: headphones from idle, plug and
unplug under a stream and in standby (each plug detected once, with the
microphone), volume keys, S3 under music (one detection, no `PDN_DONE`
timeout), both microphones. s2idle and hibernation are not tested.

The driver is in use here through DKMS: `~/dev/cs8409-mbp131-dkms` holds the
three files and the private HDA headers of Linux 7.2, which `kernel-devel`
does not ship; `dkms.conf` therefore refuses any other kernel series.

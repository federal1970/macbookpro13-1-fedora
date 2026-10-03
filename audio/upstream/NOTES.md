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

Working tree at `0246a07`; the diff against the unmodified 7.2 driver is kept
here as [`cs8409-mbp131-wip.patch`](cs8409-mbp131-wip.patch). It is work in
progress: debug prints and two experiments are still in it.

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

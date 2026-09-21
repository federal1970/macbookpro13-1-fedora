# ACPI tables of MacBookPro13,1 (firmware 529.120.1.0.0, 2024-03-14)

Dumped 2026-09-21 from `/sys/firmware/acpi/tables/` on the machine described in the
top-level README. `*.aml` are the raw tables, `*.dsl` the `iasl -e ... -d` output
(acpica 20260408). Reproduce with:

```bash
sudo sh -c 'cd /sys/firmware/acpi/tables && for t in DSDT SSDT*; do cat $t > /var/tmp/$t.aml; done'
iasl -e SSDT*.aml -d DSDT.aml
for f in SSDT*.aml; do iasl -e DSDT.aml -d "$f"; done
```

| table | OEM id | what |
|---|---|---|
| DSDT | `MacBookP` | everything: EC, SMC, root ports, Wi-Fi, camera, NVMe, `_PTS`/`_WAK` |
| SSDT2 | `SsdtS3` | the `_S3` package alone — S3 is advertised on purpose |
| SSDT5 | `TbtOnPCH` | Thunderbolt (Alpine Ridge under `RP05`), 230 KB of it |
| SSDT6 | `Xhci` | the PCH xHCI controller |
| SSDT1/3/4 | `SmcDppt`, `SataAhci`, `Sdxc` | stubs |
| SSDT7–11 | `PmRef`/`CpuRef` | Intel CPU P/C-states |

There is no `PNP0D80` (Low Power S0 Idle) device anywhere, no `_PR3` and no
`_S0W`/`_S3W`. The firmware knows S3, S4 and S5 and nothing about S0ix.

## The two sleep paths

`\_SB.PCI0._INI` classifies the OS once at boot (DSDT.dsl:7955):

| `_OSI` answered | `OSYS` | path |
|---|---|---|
| `Darwin` | 0x2710 | macOS |
| `Linux` | 0x03E8 | (never reached: the kernel does not answer `Linux`) |
| `Windows 2009` / `2012` | 0x07D9 / 0x07DC | Boot Camp |
| none | 0x07DC | Boot Camp |

`OSDW()` returns true for macOS and gates 163 places across the tables. Linux
answers `Darwin` on Apple hardware by default (`ACPI: BIOS _OSI(Darwin) query
honored via DMI`), so it walks the macOS path. The two paths divide the work
between firmware and drivers very differently:

**macOS path.** `_PTS(3)` does one thing: `EC.ECSS = 3`. Everything else is in
per-device `_PS3`/`_PS0` methods and in Apple-private methods that Apple's
drivers call before sleep. Linux drivers call none of the private ones.

**Boot Camp path.** Written for Windows drivers that know nothing about Apple, so
`_PTS(3)` does the work itself: `EC.EWPM = 1`, Bluetooth power off (`BLTH.BTPD`,
i.e. `EC.BTPC = 0`), `EC.EWDK = 1`, camera power off (`RP10.CMRA.CMPE(0)` after
waiting for D3), and for S4+ `EC.EWLO = 0`. `_WAK(3)` undoes it and restarts the
Thunderbolt firmware (`RP05.ICMB()`), runs `PTOP()`, `SPIT.CLRB()`, clears the EC
`LWE*` wake latches and notifies the Wi-Fi port.

Linux therefore gets neither: ACPI behaves as for macOS, the drivers behave as
for Windows. The kernel documents exactly this — answering `Darwin` "caused power
regressions on Mac laptops" and `acpi_osi=!Darwin` exists as the workaround
(`Documentation/firmware-guide/acpi/osi.rst`).

## What each device does on the macOS path

**Thunderbolt (SSDT5, `RP05` → `UPSB` → `DSB0` → `NHI0`).** Three GPIOs:
`0x02060000` is controller power, `0x02060001` force-power, `0x02060004` is used by
`XRST`. Power is cut by `RP05._PS3` → `PCDA()`, but only when `POFF()` is true, and
`POFF()` is `!RTBT && !RUSB` — two flags that start at 1 and are cleared only by
`NHI0.RTPC(0)` and `DSB2.XHC2.RTPC(0)`, which macOS's drivers call to say "not
needed during sleep". Linux never calls them, so the branch is dead. Independent of
that, Linux never puts `RP05` into D3 at all: it is a native-hotplug PCIe port on
x86, which `pci_bridge_d3_possible()` refuses. So `RP05._PS3` never runs.

Two methods cut the power directly and need no flags:

- `NHI0.SXFP(0)` — force-power GPIO off, 100 ms, power GPIO off. Same name and
  semantics as the method `quirk_apple_poweroff_thunderbolt` uses on Cactus Ridge
  Macs, where the kernel calls it in `suspend_late` and firmware restores power on
  resume from S3.
- `NHI0.TRPE(0, 0)` — orderly: root port to D3, link disable, wait for link down,
  power GPIO off. `TRPE(1, delay_ms)` powers back up and retrains the link, which is
  what s2idle would need since no firmware runs on the way back.

**NVMe (`RP01`, DSDT.dsl:6814).** `_PS3` under macOS: L2/L3 entry, D3, then GPIOs
`0x02070001` and `0x02040016` off — the SSD loses power. `_PS0` restores them (no
`OSDW` check). Under Boot Camp the GPIOs stay on. `RP01` is an ordinary root port
with a 2024 BIOS date, so the kernel is allowed to put it in D3; whether it does
is one of the things to trace (below).

**Wi-Fi (`RP09`, DSDT.dsl:6515).** `_PS3` under macOS: `MPPG = 1`, `HCPG = 0`,
`ALPR(1)` → `APPD()` — power-gates the card. Boot Camp: nothing. Same kernel
question as `RP01`.

**Camera (`RP10.CMRA`).** No `_PS3`. `CMPE(0)` is the only power-off and it is called
from the Boot Camp `_PTS` alone. On the macOS path Linux leaves the camera port
powered, which matches `pch_ip_power_gating_status` showing `SPC` on.

**Bluetooth (`URT0.BLTH`).** `BTPD()`/`BTPU()` write `EC.BTPC`. Called from the
Boot Camp `_PTS`/`_WAK` only; `BTPU()` also runs in `_WAK` on both paths.

**xHCI (`XHC1`, SSDT6).** `_PS3` exists on both paths and additionally sets
`D0D3 = 3` and the `MPMC` sequence under macOS. It has an `S0IX` bit it clears on
the macOS path — a leftover, since the platform never reaches S0ix.

**EC/SMC.** `ECSS` (EC "sleep state") is written with the target S-state on both
paths. The EC's SCI is GPE 0x07; that is the `gpe07` that fired 136 times in
30 minutes of s2idle in `tools/deep-test.sh`. The SMC is `APP0001`
(`smc-huronriver`) on LPC, no methods of its own touch sleep.

**Wake GPEs (`_PRW`).** 0x69: the four PCIe root ports, `PEG0`, `ARPT`. 0x6D:
`HDEF`, `XHC1`, `XHC2`. 0x6F: `BLTH`, `ADP1`, `LID0`. 0x17: `SPIT` (keyboard and
trackpad) — the `gpe17` of the same test. 0x07: `EC`.

## What this means for the sleep drain

The 3.83 W of s2idle and the 5.30 W of `deep` were measured on the macOS path with
Linux drivers, i.e. with Thunderbolt, camera and Bluetooth powered throughout and
nobody calling the methods that would cut them. None of that is firmware that
needs reverse-engineering: the methods are here, named, and callable from the OS.
The experiments, in order of cost:

1. `acpi_osi=!Darwin` on the kernel command line — the whole Boot Camp path in one
   reversible switch. Re-measure s2idle and `deep`.
2. `NHI0.SXFP(0)` before suspend (Thunderbolt module unloaded) — via `acpi_call` or
   a five-line extension of `quirk_apple_poweroff_thunderbolt` to Alpine Ridge
   (`8086:1578`). For `deep`, firmware restores power; for s2idle, `TRPE(1, 500)`
   on resume.
3. Trace which `_PS3` the kernel actually evaluates during a normal s2idle:
   `echo 'file drivers/acpi/device_pm.c +p' > /sys/kernel/debug/dynamic_debug/control`
   and `echo 1 > /sys/power/pm_debug_messages`, then read `journalctl -k` after
   resume. Settles the `RP01`/`RP09` question without guessing.
4. Camera and Bluetooth off by hand (`CMPE(0)`, `BTPD()`) if item 1 is rejected
   for other reasons.

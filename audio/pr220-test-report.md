Tested on a **MacBookPro13,1** (A1708, 13" 2016, no Touch Bar; CS8409 + CS42L83 + SSM3515, subsystem `106b:3300`), Fedora 44, kernel `7.2.8-200.fc44`, `mem_sleep=deep`, PipeWire. Applied on top of 89b22ff; builds without warnings.

**Before the patch:** speakers silent after resume, with the software side looking healthy (PCM RUNNING, pointer advancing, nothing muted, codec in D0). The codec dump had every GPIO at 0; healthy on this machine is IO0–IO3 enabled, IO1 an output driven high. Nothing short of a reboot brought it back: not a second suspend, not a runtime-PM cycle of the codec, not `reconfig` (EBUSY); unbinding `snd_hda_intel` only made it worse (Dummy Output, which is what #215 is about).

**With the patch**, one boot, four suspends, all of them lost the setup and all were reprogrammed:

| case | result |
|---|---|
| suspend with the codec runtime-suspended (idle for two minutes) | reprogrammed at resume, speakers work |
| suspend with a stream running (`speaker-test`) | reprogrammed, the stream carried on by itself, PCM stayed RUNNING |
| suspend with a headset plugged in and a stream running | reprogrammed, sound in the headset after resume, unplug moved it back to the speakers |
| function-group reset by hand (see below), then runtime resume | reprogrammed, speakers work |

No `still reads as unprogrammed` and no `programming the codec after resume failed`. The branch does not fire at boot. Jack plug and unplug after a reprogramming are handled normally, and the codec goes back to runtime suspend afterwards.

Two of those cases are the ones you listed as not tested (stream running, headphones plugged in).

**One observation in the headset case.** During the reprogramming the log has

```
cs_8409_boot_setup_real headphone ALREADY PLUGGED IN!!
cs_8409_boot_setup_real boot - button detect - FAILED TO GET INTERRUPT!!
```

The headset was detected right after and playback worked; I did not test the headset buttons or its microphone after that.

**A way to provoke the lost state without suspending**, in case it helps others test:

```
hda-verb /dev/snd/hwC0D0 0x01 0x7ff 0
hda-verb /dev/snd/hwC0D0 0x01 0x7ff 0
```

(a function-group reset, twice) puts the codec back in its power-on state; the next runtime resume reprograms it. Writing only a GPIO mask of 0 on a live codec is *not* equivalent. I tried that first, with a simpler patch of my own (same GPIO-mask gate, in `.resume`, without `block_unsol`): the boot setup then ran on a codec that was up, the CS42L83 answered with an interrupt storm (`UNKNOWN INTERRUPT 0x0000000c`, `max count exceeded`) and the speakers stayed silent — the failure MrHogun describes. So gating on the real reset state, as this patch does, looks right to me.

Thanks for this; I had written the same gate independently the same morning and dropped it in favour of yours.

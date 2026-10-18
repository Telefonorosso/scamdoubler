# Emu68 AV POC3 — UVC + UAC2 + Paula CACHE-FIX2 Hardware-Validated Milestone

**Status:** HARDWARE VALIDATED  
**Base:** Emu68 `9b4379a5c5dbf7f12f5e10cfe81a96b872e2426f`  
**Target:** PiStorm Classic / Raspberry Pi 3A+  
**Date:** 2026-10-18  
**Supersedes:** POC2 synthetic-tone milestone for the combined A/V path

## Milestone result

This archive freezes the exact POC3 source code that was tested successfully on
real hardware and then explicitly promoted to a milestone by the project author.
No source-code change was made after the successful test; only milestone
documentation and a fresh SHA-256 manifest were added for packaging.

The validated integration combines, in one USB High-Speed device:

- Framethrower UVC video: 720x576 I420/YU12 at 25 fps on EP1 IN bulk;
- UAC2 capture: 48 kHz stereo signed 16-bit PCM on EP2 IN isochronous;
- real software-rendered Paula audio, replacing the POC2 synthetic 1 kHz tone;
- PaulaMixer V2 CACHE-FIX2 behavior for cold-cache correctness;
- a single DWC2 owner on CPU3;
- Framethrower conversion retained on CPU1;
- Paula rendering and cooperative PiStorm housekeeper on CPU2;
- physical Chip RAM cache reads retained on CPU0.

This validation establishes that the architectural merge itself is viable:
Framethrower video and software Paula audio can coexist over the same native
High-Speed UVC + UAC2 USB transport on the validated PiStorm Classic / Pi 3A+
platform.

No quantitative long-run loss, latency, cache-counter or A/V-sync figures were
supplied with the milestone promotion, so this README intentionally does not
invent them. Those remain follow-up characterization work rather than part of
the validation claim.

## Validated architecture

```
CPU0  Emu68/JIT
      + Paula register-write producer
      + sole physical Chip RAM reader for asynchronous sample cache

CPU1  Framethrower RGB565 -> I420 NEON worker

CPU2  software Paula renderer @ 48 kHz
      + CACHE-FIX2 transactional rendering
      + cooperative PiStorm IPL/reset housekeeper

CPU3  sole DWC2 owner
      + UVC EP1 IN bulk
      + UAC2 EP2 IN isochronous
```

The division above is intentional. In particular, CPU1 is not shared between
video and Paula, and CPU3 remains the only core programming DWC2.

## CACHE-FIX2 preservation

The Paula core/cache implementation is carried over from the previously
hardware-validated PaulaMixer V2 CACHE-FIX2 branch. The important properties are
preserved:

- 2 MiB asynchronous Chip RAM shadow cache using 256-byte pages;
- CPU0 is the only core that performs physical Chip RAM sample reads;
- bounded lookahead of up to 1024 bytes per active Paula channel;
- one-frame transactional rendering;
- if a valid sample address misses in the cache, Paula core state and gain state
  are rolled back, so the synthetic zero does not advance committed playback;
- output fades toward silence during a cache wait and fades back in after
  recovery;
- existing event/snapshot recovery and PaulaProbe/PaulaMixer ABI are retained.

This cache correction is considered an essential part of the POC3 design, not
an optional optimization.

## UAC2 PCM bridge

CPU2 renders Paula continuously at 48 kHz into a 2048-frame SPSC stereo PCM
ring. CPU3 consumes that ring only while UAC2 AudioStreaming ALT1 is active.

The native High-Speed service interval is 125 us (`bInterval=1`). Nominally six
stereo frames are sent per microframe:

```
48000 / 8000 = 6 stereo frames / microframe
6 frames * 2 channels * 2 bytes = 24 bytes
```

The endpoint permits up to 28 bytes so the bridge can occasionally send 5 or 7
frames (20/28 bytes) to keep the independently-clocked producer and USB
consumer near the target fill without resampling.

EP2 remains lazy: physical endpoint activation occurs only after the
AudioStreaming ALT1 `SET_INTERFACE` control transfer has completed its EP0
status stage. This preserves the UVC probe/commit behavior established during
the POC2 bring-up.

## USB identity

The POC3 build uses:

```
VID:PID    0525:a4b7
bcdDevice  0.12
serial     AVPOC012
product    Framethrower + Paula AV POC3
```

## Build

Apply the archive to a clean checkout at the base commit:

```bash
cd /work/Emu68
unzip -o Emu68-AV-POC3-UVC-UAC2-PAULA-CACHEFIX2-HARDWARE-VALIDATED-MILESTONE.zip
cmake --build build -j$(nproc)
```

If the build directory has not yet been configured, use the same PiStorm
Classic configuration used for the preceding validated POC2.

## Basic host verification

On Linux, video should enumerate as a standard V4L2 UVC source:

```bash
v4l2-ctl -d /dev/video0 --list-formats-ext
```

Expected video mode:

```
YU12 / I420
720x576
25 fps
```

The audio side should enumerate as a UAC2 capture device:

```bash
arecord -l
```

A representative capture command is:

```bash
arecord -D hw:1,0 -f S16_LE -c 2 -r 48000 -d 10 paula.wav
```

Adjust the ALSA card number as needed. There is no synthetic 1 kHz source in
this milestone: captured audio is produced by the software Paula renderer.

## Package contents

The archive is an overlay, not a complete Emu68 source tree. It contains the
modified integration files, the complete Paula core/cache subset required by
POC3, this milestone README, the original POC3 development README, and a fresh
SHA-256 manifest.

`MANIFEST-SHA256.txt` covers every packaged file except itself.

## Rollback

Keep the preceding known-good milestones available independently:

- `Emu68-AV-POC2-UVC-UAC2-HS125-TONE-HARDWARE-VALIDATED-MILESTONE.zip`
  for the validated synthetic-tone UVC+UAC2 transport;
- the PaulaMixer V2 CACHE-FIX2 hardware-validated milestone for the standalone
  software-Paula/cache implementation.

These provide clean isolation points if later work on lifecycle, latency,
A/V synchronization or Paula fidelity introduces a regression.

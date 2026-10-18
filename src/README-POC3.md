# Emu68 AV POC3 — Framethrower UVC + UAC2 + Paula CACHE-FIX2

**Status:** experimental integration overlay; **not hardware validated yet**  
**Base:** Emu68 `9b4379a5c5dbf7f12f5e10cfe81a96b872e2426f`  
**Target:** PiStorm Classic / Raspberry Pi 3A+  
**Known-good rollback:** `Emu68-AV-POC2-UVC-UAC2-HS125-TONE-HARDWARE-VALIDATED-MILESTONE.zip`

## Purpose

POC3 replaces the synthetic 1 kHz source of the hardware-validated POC2 with the
software Paula renderer from the hardware-validated PaulaMixer V2 CACHE-FIX2
branch.  The validated UVC + UAC2 USB topology is deliberately preserved:

- UVC Framethrower: 720x576 I420/YU12 at 25 fps, EP1 IN bulk, High-Speed.
- UAC2 capture: 48 kHz, stereo, signed 16-bit PCM, EP2 IN asynchronous ISO.
- UAC2 native High-Speed service interval: 125 us (`bInterval=1`).
- EP2 remains lazy and is physically opened only after AudioStreaming ALT1 has
  completed its EP0 status stage.
- The UAC2 Clock Source remains fixed at 48 kHz.

The important change is the producer behind EP2: real mirrored Paula PCM instead
of the test tone.

## Core ownership

POC3 deliberately avoids sharing DWC2 or the Framethrower conversion worker:

```
CPU0  Emu68/JIT + Paula write producer + sole physical Chip RAM cache service
CPU1  Framethrower RGB565 -> I420 NEON worker (POC2 path preserved)
CPU2  Paula CACHE-FIX2 renderer + cooperative PiStorm IPL/reset housekeeper
CPU3  sole DWC2 owner: UVC EP1 + UAC2 EP2 (POC2 transport preserved)
```

CPU2 inherits the stock housekeeper timer event stream.  While waiting for the
next 48 kHz Paula render deadline it sleeps with `WFE`; each wake services
IPL/reset before checking the audio clock again.  This avoids moving any DWC2
work away from CPU3 and avoids a permanent CPU2 busy loop.

## Paula CACHE-FIX2 preservation

The CACHE-FIX2 renderer/cache implementation is carried over unchanged in its
core files.  Its behavior remains:

- 2 MiB asynchronous Chip RAM shadow cache, 256-byte pages.
- CPU0 is the only core performing physical Chip RAM sample reads.
- bounded lookahead of up to 1024 bytes per active channel;
- transactional one-frame Paula rendering;
- on a missing cache page the small Paula core state and gain state are rolled
  back, so a synthetic zero cannot advance the committed sample cursor;
- the last valid output fades toward silence while waiting and fades back in
  after recovery;
- event/snapshot recovery and the existing PaulaProbe/PaulaMixer register ABI
  are retained.

The existing 50 ms live-replay latency used by the validated Paula USB path is
also retained for this first combined AV test.

## USB PCM bridge

CPU2 continuously renders Paula at 48 kHz into a 2048-frame SPSC stereo ring.
CPU3 consumes that ring only when UAC2 AudioStreaming ALT1 is active.

Nominally each 125-us USB microframe carries six stereo frames (24 bytes).  The
endpoint already advertises a 28-byte maximum packet, so POC3 may occasionally
send five or seven frames (20/28 bytes) to keep the independently-clocked Paula
producer and USB microframe consumer near a bounded target fill without
resampling.  ALT1 starts from a small recent pre-roll rather than stale history.

## Build

Overlay this ZIP directly on a **clean** Emu68 tree at the base commit:

```bash
cd /work/Emu68
unzip -o Emu68-AV-POC3-NOHUD-REGULARBOOT-OVERLAY-9b4379a.zip
cmake --build build -j$(nproc)
```

If the clean Docker does not yet have a configured build directory, use the
same PiStorm Classic configure command used for the preceding POC2 builds.

This regular-boot overlay removes the bring-up-only framebuffer diagnostics and
the 60-second boot hold. CPU1/CPU2/CPU3 workers are enabled exactly as before,
but CPU0 returns immediately to normal Emu68 startup once USB initialization
completes. USB descriptors, UVC/UAC2 transport, Paula CACHE-FIX2, PCM buffering
and core ownership are otherwise unchanged.

## First hardware test — Linux

1. Confirm that UVC survived the merge:

```bash
v4l2-ctl -d /dev/video0 --list-formats-ext
```

Expected: `YU12`, 720x576, 25 fps.

2. Find the UAC2 capture card:

```bash
arecord -l
```

3. After the Amiga has entered normal execution, start a module/game/demo that
uses Paula and record ten seconds, substituting the actual ALSA card number:

```bash
arecord -D hw:1,0 -f S16_LE -c 2 -r 48000 -d 10 paula.wav
```

4. Play the result:

```bash
aplay paula.wav
```

There should be **no synthetic 1 kHz tone** in POC3.  The captured stream should
contain the software-rendered Paula output.

## CACHE-FIX2 diagnostics

The existing PaulaProbe V2-compatible monitor may be used unchanged.  For the
first cold-cache playback, cache misses and wait frames are allowed; the key
property is that missing-page zero substitutions do not advance committed Paula
state.  Useful counters include cache zero attempts, recovered pages, wait
frames, wait recoveries, prefetch pages, dropped events and resyncs.

Also verify ordinary Workbench stability, interrupts and reset behavior because
CPU2 now combines Paula rendering with the formerly dedicated PiStorm
housekeeper.

## Verification performed before packaging

This package has **not** been tested on the user's hardware yet.  The following
software-side checks were completed:

- the exact CACHE-FIX2 core source files were compared byte-for-byte against the
  previous hardware-validated CACHE-FIX2 milestone;
- all six available host regressions passed: cache miss/recovery, invalidation
  and event concurrency, probe ABI/counters, cold-cache transactional rollback,
  speculative IRQ rollback, and bounded prefetch/one-shot-to-loop behavior;
- the new UVC/UAC2/Paula integration sources compile as freestanding big-endian
  AArch64 objects with Clang using `-Wall -Wextra -Werror` (with only
  compiler-specific suppressions for pre-existing Emu68 register-asm warnings
  outside the new code);
- the new Paula/USB/housekeeper objects partial-link with no unresolved symbols
  belonging to the POC3 integration;
- CMake configuration for `raspi64` + `pistorm-classic` completes with a local
  Clang toolchain check;
- `start_rpi64.c`, `support_rpi.c` and `uvc_libyuv_neon.c` are byte-identical to
  the successful POC2 milestone.

A full GCC Emu68 firmware link must still be performed in the normal
`emu68-build` Docker, followed by hardware validation.  Do not replace the POC2
rollback image until POC3 has passed both UVC and Paula capture tests.

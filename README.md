# scamdoubler - AI CODED

**Turn a PiStorm-equipped Amiga into a standard USB audio/video source for modern computers.**

This Emu68 experiment combines two live Amiga data paths into a single USB High-Speed device:

- **Framethrower Denise video** exposed as standard **USB Video Class (UVC)** capture;
- **software-rendered Paula audio** exposed as standard **USB Audio Class 2.0 (UAC2)** capture.

The host therefore sees the PiStorm system as a normal USB multimedia source: video can be opened by ordinary camera/capture software, while audio can be opened by ordinary USB-audio software.

No project-specific host capture driver is required.

> **Current hardware-validated milestone:** UVC 720×576 I420 @ 25 fps + UAC2 48 kHz stereo S16, with real mirrored Paula audio and the PaulaMixer V2 CACHE-FIX2 path.

---

## What it does

The video path is:

```text
Amiga video
    |
    v
Framethrower Denise
    |
    v
Raspberry Pi Unicam
    |
    v
720x576 RGB565 frame
    |
    v
CPU1: RGB565 -> I420 conversion
    |
    v
CPU3: USB Video Class (UVC)
    |
    v
Host
```

The audio path is:

```text
Amiga / Paula register writes
    |
    v
CPU0: mirror producer
    |
    +------------------------------+
    |                              |
    |                      Chip RAM sample data
    |                              |
    |                   CPU0 cache service only
    |                              |
    +--------------+---------------+
                   |
                   v
          2 MiB shadow sample cache
                   |
                   v
       CPU2: software Paula renderer
                   |
                   v
          48 kHz stereo PCM ring
                   |
                   v
       CPU3: USB Audio Class 2.0
                   |
                   v
                  Host
```

The two streams share the same physical USB connection but remain separate standard USB functions.

---

## Current validated output

### Video

| Property | Value |
| --- | --- |
| Resolution | 720 × 576 |
| Frame rate | 25 fps |
| Source format | RGB565 |
| USB format | I420 / YU12 |
| Color format | YUV 4:2:0 planar |
| USB class | UVC |
| USB endpoint | EP1 IN |
| USB transport | High-Speed Bulk IN |

### Audio

| Property | Value |
| --- | --- |
| Source | Software-rendered Paula |
| Sample rate | 48 kHz |
| Channels | Stereo |
| Sample format | Signed 16-bit PCM |
| USB class | UAC2 |
| USB endpoint | EP2 IN |
| USB transport | High-Speed Isochronous IN |
| Service interval | 125 µs |
| Nominal payload | 24 bytes / microframe |

The audio stream is **not** an analog capture of the Amiga audio output. It is reconstructed digitally from mirrored Paula programming and Chip RAM sample data using the software Paula renderer.

---

## Why this is interesting

A conventional Amiga capture setup normally needs at least an external video capture device and a separate audio connection.

This project moves both functions into the PiStorm system itself.

Framethrower already provides access to the live Amiga video stream through the Raspberry Pi capture path. Emu68 consumes that framebuffer, converts it, and exposes it over UVC.

For audio, Emu68 observes the Paula programming performed by the 68k side, reconstructs the sample stream with a software Paula core, and exports the resulting PCM through UAC2.

The result is a single USB connection that can provide:

- live Amiga video capture;
- Paula audio capture;
- recording on a modern PC;
- OBS integration;
- streaming;
- archival capture;
- screenshots;
- remote viewing;
- host-side post-processing;
- future A/V synchronization work without proprietary capture hardware.

The Amiga application itself does not need to know that USB capture is taking place.

---

## Host-visible USB device

The current hardware-validated POC3 firmware identifies itself as:

```text
Manufacturer: PiStorm
Product:      Framethrower + Paula AV POC3
Serial:       AVPOC012
VID:PID:      0525:a4b7
bcdDevice:    0.12
```

The device is a native USB High-Speed composite multimedia device.

Its host-visible functions are:

```text
UVC
  Interface 0  VideoControl
  Interface 1  VideoStreaming
               EP1 IN BULK

UAC2
  Interface 2  AudioControl
  Interface 3  AudioStreaming
               ALT0 = zero bandwidth
               ALT1 = active stream
               EP2 IN ISO
```

The UVC and UAC2 functions use standard operating-system class drivers.

---

## Requirements

The current hardware-validated implementation targets:

- **Amiga 600**;
- **PiStorm Classic**;
- **Raspberry Pi 3A+**;
- **Emu68**;
- **Framethrower Denise**;
- a USB connection from the Pi USB device port to the host computer.

The implementation is tied to the Framethrower/Unicam live framebuffer path and to the PiStorm Classic bus integration used by the software Paula mirror.

Other Pi models or PiStorm variants may be possible, but they are not part of the current validation claim.

---

# Video path

## Framethrower as the source

This project does not emulate the Amiga display.

The source picture comes from the real Framethrower capture path. Framethrower feeds the Raspberry Pi Unicam block, which provides the live RGB565 framebuffer used by the USB producer.

Conceptually:

```text
Framethrower capture
       |
       +----> normal Framethrower video path
       |
       +----> Emu68 USB capture path
```

The UVC code is therefore a consumer of the already-running Framethrower framebuffer rather than the owner of Framethrower itself.

---

## Video conversion

The captured source frame is:

```text
720 x 576 RGB565
```

Before USB transmission it is converted to:

```text
720 x 576 I420 / YU12
```

The conversion is performed on CPU1 with AArch64 NEON acceleration using a layout derived from the same general approach used by libyuv.

The current USB video stream is uncompressed I420 rather than MJPEG or H.264.

---

## Video timing

The producer follows the real Framethrower/Unicam frame cadence.

For the validated PAL path:

```text
50 Hz source cadence
        |
        v
       /2
        |
        v
25 fps USB video
```

This keeps video production related to the incoming Amiga video cadence instead of generating capture frames from an unrelated software timer.

---

## Video buffering

The implementation uses multiple full-frame buffers so that capture, conversion and USB transmission do not all operate on the same memory simultaneously.

Conceptually:

```text
Frame A
USB is transmitting

Frame B
CPU1 is converting

Frame C
available / next frame
```

A frame being transmitted remains immutable.

If a newer completed frame becomes available before an older unpublished frame is used, stale unpublished video may be discarded in favour of the newest image. For live capture this is preferable to allowing latency to grow indefinitely.

---

# Audio path

## What “Paula audio” means here

The USB audio stream is generated by a **software Paula renderer**.

The physical Paula remains part of the real Amiga system. Emu68 mirrors the relevant CPU-originated audio register writes and reconstructs the corresponding digital Paula output in parallel.

This is therefore different from:

- sampling the analog Amiga audio output;
- digitizing the motherboard audio path;
- taking audio from Framethrower;
- forwarding an existing Linux sound device.

The capture path instead reconstructs Paula from:

```text
Paula register programming
+
Chip RAM sample data
+
software Paula state
```

The software core is based on the MIT-licensed Paula emulator by Akustikrausch / Andreas Wendorf.

---

## Why the sample cache exists

The software renderer must read sample bytes from Amiga Chip RAM.

Allowing the renderer core to perform arbitrary physical PiStorm bus reads would create timing, ownership and concurrency problems. The current architecture therefore uses an asynchronous shadow cache.

The validated CACHE-FIX2 design provides:

```text
2 MiB Chip RAM shadow
256-byte pages
CPU0-only physical reads
CPU2-only software rendering
```

CPU2 never directly performs physical Chip RAM transactions for audio.

---

## CACHE-FIX2

CACHE-FIX2 is an essential correctness mechanism, not merely a performance optimization.

An earlier cache implementation could return a synthetic zero when a requested sample page was missing while still allowing the software Paula source cursor to advance. That meant first playback from a cold cache could consume a different logical sample stream than later playback from a warm cache.

CACHE-FIX2 changes this behaviour.

### Bounded lookahead

For each active Paula channel, the renderer requests a bounded amount of upcoming sample data:

```text
up to 1024 bytes per active channel
```

The next loop region is also prefetched when the current one-shot approaches its end.

This is intentionally not a full-module or full-sample preload.

### Transactional rendering

Each Paula output frame is rendered speculatively.

If a valid sample address hits a missing cache page:

1. the software Paula core state is restored;
2. the master-gain state is restored;
3. speculative virtual IRQ accounting is discarded;
4. the sample cursor does **not** advance;
5. the missing page is requested from CPU0.

The missing synthetic zero therefore never becomes part of the committed source progression.

### Recovery behaviour

USB/audio timing must continue even if sample data is temporarily unavailable.

During a cache wait:

- source progression is paused transactionally;
- the last valid output fades toward silence;
- CPU0 continues servicing cache requests;
- once data becomes available, the source resumes from the preserved state;
- output fades back in.

This makes cold-cache behaviour much safer without blocking the USB audio clock.

---

## Paula register mirroring

CPU0 observes the relevant Paula register writes generated by the emulated 68k side.

The mirror tracks:

- `DMACON`;
- `INTENA`;
- `INTREQ`;
- `ADKCON`;
- the four `AUDx` channel register groups.

A bounded event ring transfers this programming to the software Paula side.

A snapshot/resynchronization mechanism exists so the renderer can recover a coherent register state if the event stream falls behind.

The physical Paula remains the source of real Amiga audio interrupts; virtual IRQ activity inside the software renderer is diagnostic state only.

---

## PCM bridge

CPU2 renders software Paula continuously at:

```text
48000 stereo frames / second
```

The output is converted to signed 16-bit stereo PCM and written into a single-producer/single-consumer ring:

```text
2048 stereo frames
```

CPU3 consumes this ring only while the UAC2 AudioStreaming interface is active.

This separates the software Paula clock from the USB microframe scheduler.

---

## Native High-Speed UAC2 timing

The project originally experimented with UAC1-style Full-Speed timing. The successful combined architecture instead uses **native UAC2 High-Speed timing**.

EP2 uses:

```text
bInterval = 1
```

At USB High-Speed this means one service opportunity every:

```text
125 µs
```

There are therefore:

```text
8000 USB microframes / second
```

At 48 kHz:

```text
48000 / 8000 = 6 stereo frames / microframe
```

Each stereo S16 frame is four bytes:

```text
6 x 4 = 24 bytes nominally
```

So the normal UAC2 packet is:

```text
24 bytes / 125 µs
```

---

## Producer/consumer drift

The Paula renderer clock and the USB microframe clock are independent.

Rather than resampling, the current bridge keeps the PCM ring near a bounded target fill by occasionally changing the number of stereo frames sent in a microframe:

```text
5 frames = 20 bytes
6 frames = 24 bytes   nominal
7 frames = 28 bytes
```

The endpoint advertises enough packet capacity for the 28-byte case.

This is a deliberately small rate-correction mechanism rather than a sample-rate converter.

---

## Lazy EP2 activation

One important lesson from bring-up was that programming the audio endpoint too early could interfere with UVC control traffic.

The validated design therefore keeps EP2 physically inactive until the host really opens the AudioStreaming interface.

The sequence is:

```text
Host SET_INTERFACE ALT1
        |
        v
complete EP0 status stage
        |
        v
activate EP2
        |
        v
start UAC2 ISO traffic
```

This preserves the UVC probe/commit path while still allowing audio to start normally.

---

## Audio latency

The current combined milestone intentionally retains the live Paula replay latency used by the validated standalone Paula path.

The renderer currently includes approximately:

```text
50 ms
```

of software replay latency before considering USB buffering and host buffering.

This has not yet been tuned specifically for final A/V synchronization.

The current milestone therefore proves coexistence and stable transport; it does **not** yet claim measured lip-sync accuracy or minimum achievable latency.

---

# CPU architecture

The combined design works because each expensive or timing-sensitive job has a clear owner.

```text
                         PiStorm / Emu68
                              |
        +---------------------+----------------------+
        |                     |                      |
        |                     |                      |
      CPU0                  CPU1                   CPU2
 Emu68 / JIT          Framethrower worker      Paula renderer
 Paula write mirror    RGB565 -> I420 NEON      @ 48 kHz
 Chip RAM cache        video buffering          CACHE-FIX2
 physical reads                                 housekeeper
        |                     |                      |
        |                     |                      |
        +----------+----------+-----------+----------+
                   |                      |
                   |                      |
                   v                      v
              video frames           stereo PCM ring
                   \                      /
                    \                    /
                     +------------------+
                              |
                            CPU3
                     sole DWC2 owner
                              |
                  +-----------+-----------+
                  |                       |
              EP1 IN BULK             EP2 IN ISO
                  |                       |
                 UVC                     UAC2
                  |                       |
                  +-----------+-----------+
                              |
                         USB High-Speed
                              |
                             Host
```

---

## CPU0

CPU0 runs the normal Emu68/JIT side and also owns the Paula mirror producer.

For the audio cache, CPU0 is the **only** core allowed to perform the physical Chip RAM sample reads required to populate missing cache pages.

This preserves PiStorm bus ownership and prevents the renderer from issuing asynchronous physical bus traffic itself.

---

## CPU1

CPU1 remains dedicated to the video conversion path:

```text
RGB565 -> I420
```

This is intentionally preserved from the known-good Framethrower UVC implementation.

The software Paula renderer is **not** placed on CPU1.

---

## CPU2

CPU2 owns:

- software Paula rendering at 48 kHz;
- CACHE-FIX2 speculative/transactional rendering;
- PCM ring production;
- the cooperative PiStorm IPL/reset housekeeper.

The stock housekeeper previously occupied CPU2 as its own loop. In the combined architecture it is serviced cooperatively from the Paula worker rather than being removed.

CPU2 waits with `WFE` around the audio timing loop rather than spinning permanently at 100%.

---

## CPU3

CPU3 is the **only owner of the DWC2 USB controller**.

It handles:

- EP0 control traffic;
- UVC protocol handling;
- UAC2 protocol handling;
- EP1 video transmission;
- EP2 isochronous audio transmission;
- USB frame/microframe scheduling.

Keeping all DWC2 access on one core is a central design invariant.

---

# USB architecture

The current device is a single USB High-Speed composite multimedia device.

```text
Configuration
|
+-- UVC function
|   |
|   +-- VideoControl
|   +-- VideoStreaming
|       |
|       +-- EP1 IN BULK
|
+-- UAC2 function
    |
    +-- AudioControl
    |   |
    |   +-- Clock Source: fixed 48 kHz
    |
    +-- AudioStreaming
        |
        +-- ALT0: idle
        +-- ALT1: active
            |
            +-- EP2 IN ISO
```

The successful UAC2 implementation uses a proper UAC2 Clock Source entity with a fixed 48 kHz clock.

This replaced the earlier UAC1 experiment whose endpoint-level sample-frequency control caused host compatibility problems in the combined High-Speed device.

---

# Host usage

## Windows

Connect the Pi USB device port to the Windows PC after booting the modified Emu68 firmware.

The system should expose standard video and audio class functions from the same physical device.

No project-specific Windows driver is required.

Typical host applications include:

- OBS Studio;
- VLC;
- capture/streaming applications;
- DAWs or audio recorders that accept standard USB audio input;
- video-conferencing software that accepts UVC sources.

### OBS

For video:

```text
Sources
-> Add
-> Video Capture Device
```

Select the Framethrower/PiStorm camera source.

Expected video mode:

```text
720x576
25 fps
I420
```

For audio, add an audio input/capture source and select the audio function belonging to:

```text
Framethrower + Paula AV POC3
```

The video and audio interfaces belong to the same USB device but are selected independently by normal host applications.

---

## Linux

The video side is handled by the normal `uvcvideo` driver.

A device such as:

```text
/dev/video0
```

should appear.

List the negotiated modes with:

```bash
v4l2-ctl -d /dev/video0 --list-formats-ext
```

The validated mode is:

```text
YU12 / I420
720x576
25 fps
```

The audio side is handled by the normal USB audio driver.

List capture devices with:

```bash
arecord -l
```

A representative entry is:

```text
Framethrower + Paula AV POC3
USB Audio
```

Record ten seconds, substituting the actual card number:

```bash
arecord -D hw:1,0 \
  -f S16_LE \
  -c 2 \
  -r 48000 \
  -d 10 \
  paula.wav
```

Play it back with:

```bash
aplay paula.wav
```

The exact `/dev/videoN` and ALSA card numbers depend on the other devices installed on the host.

---

# Development diagnostics

## Current boot hold

The hardware-validated POC3 still retains the diagnostic boot/HUD hold inherited from bring-up.

For approximately 60 seconds after USB startup, diagnostic/test output may be visible before normal Amiga execution continues.

This was deliberately preserved during validation so that the successful UVC+UAC2 transport was not changed at the same time as the Paula merge.

It is a development artefact, not intended final UX.

---

## PaulaProbe / PaulaMixer ABI

The existing PaulaProbe/PaulaMixer register-bank ABI from the CACHE-FIX2 branch is retained.

Useful diagnostics include:

- captured and consumed register events;
- dropped events;
- resynchronizations;
- cache misses;
- synthetic-zero attempts;
- recovered cache pages;
- cache wait frames;
- cache wait recoveries;
- prefetch-page activity;
- cache queue depth;
- sample reads;
- virtual IRQ activity.

A cold first playback may still cause cache misses and wait frames.

The important CACHE-FIX2 property is that missing-page zero substitutions do not advance committed Paula source state.

---

# Source layout

The current hardware-validated overlay contains the combined video, USB and Paula integration files:

```text
CMakeLists.txt

include/
  paula_probe.h

src/
  ExecutionLoop.c

  aarch64/
    start.c
    vectors.c

  pistorm/
    ps_classic_protocol.c

  raspi/
    start_rpi64.c
    support_rpi.c
    usb_uvc_poc.c
    uvc_libyuv_neon.c

    paula/
      LICENSE
      paula.cpp
      paula.h
      paula_live.cpp
      paula_live.h
      paula_mirror.c
      paula_mirror.h
      paula_probe.c
      paula_replay.h
```

The USB implementation currently remains in `usb_uvc_poc.c` for historical reasons even though it now owns both UVC and UAC2.

A future cleanup may rename or split this transport once the combined architecture is no longer considered experimental.

---

# Building

The hardware-validated POC3 is based on:

```text
Emu68 commit:
9b4379a5c5dbf7f12f5e10cfe81a96b872e2426f
```

Use the normal Emu68 PiStorm Classic build environment.

Typical configuration:

```bash
cmake -S . -B build \
  -DTARGET=raspi64 \
  -DVARIANT=pistorm-classic \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux-gnu.cmake
```

Then build:

```bash
cmake --build build -j$(nproc)
```

If using the milestone ZIP as an overlay, apply it to a clean tree at the matching base commit before building.

As with any low-level Emu68 experiment, keep a known-good firmware image available for rollback.

---

# Hardware-validated status

The current POC3 milestone establishes that the following architecture works together on the validated hardware:

- Framethrower as the live video source;
- read-only use of the running Unicam framebuffer;
- 720×576 PAL capture;
- 25 fps UVC production;
- RGB565 -> I420 conversion;
- AArch64/NEON video conversion on CPU1;
- standard UVC video over EP1 Bulk IN;
- standard UAC2 stereo capture over EP2 Isochronous IN;
- native High-Speed 125 µs UAC2 scheduling;
- software-rendered Paula audio;
- Paula register mirroring;
- asynchronous Chip RAM shadow cache;
- CACHE-FIX2 transactional rendering;
- CPU0-only physical sample-cache reads;
- Paula rendering on CPU2;
- cooperative PiStorm housekeeper on CPU2;
- DWC2 ownership isolated to CPU3;
- simultaneous video and real Paula audio over one USB connection.

The validated milestone proves the architectural merge.

It does **not** yet establish long-run quantitative figures for:

- A/V synchronization error;
- end-to-end audio latency;
- end-to-end video latency;
- multi-hour transport loss;
- every game/demo/tracker;
- every Windows/macOS/Linux application;
- every Pi/PiStorm hardware variant.

Those are characterization and cleanup work, not prerequisites for the proof of concept.

---

# Limitations

The project remains experimental.

Current limitations include:

- one primary video mode;
- PAL-oriented 720×576 / 25 fps operation;
- uncompressed I420 video;
- no hardware video compression;
- fixed 48 kHz UAC2 audio clock;
- software Paula is not a cycle-exact replacement for every undocumented Paula behaviour;
- direct `AUDxDAT` and modulation corner cases remain dependent on the current software Paula core;
- physical DMA/blitter writes not observed by the mirror remain a general limitation of the software reconstruction approach;
- approximately 50 ms of retained Paula replay latency before host buffering;
- final A/V synchronization has not yet been tuned;
- the current build retains a development boot/HUD hold;
- USB reset/reconnect lifecycle still deserves broader testing;
- host application compatibility is not exhaustively tested.

The audio path should therefore be understood as a practical digital Paula reconstruction for capture, not as a claim of analog or cycle-perfect equivalence.

---

# Possible next work

Natural follow-up work now includes:

- remove or shorten the 60-second development hold;
- measure actual audio latency;
- measure actual video latency;
- measure A/V offset under sustained playback;
- add a controlled A/V synchronization policy;
- characterize the 5/6/7-frame USB drift correction over long runs;
- test long cold-cache and tracker/module workloads;
- test more games and demos;
- improve USB reset/reconnect lifecycle;
- investigate selectable PAL/NTSC operation;
- add additional UVC modes;
- evaluate optional lower-latency Paula replay;
- clean up the combined USB transport naming;
- prepare the architecture for eventual upstream discussion.

The major architectural goal originally listed as future work has now been achieved:

```text
single PiStorm USB connection
        |
        +---- Amiga video via UVC
        |
        +---- Paula audio via UAC2
```

---

# Credits

This is an unofficial experimental Emu68 / PiStorm extension.

It is not an official Emu68 release.

The project builds on:

- **Emu68**, for the PiStorm execution environment;
- **PiStorm**, for the hardware platform;
- **Framethrower Denise**, for the live Amiga video capture path;
- Raspberry Pi **Unicam**, for video capture;
- Raspberry Pi **DWC2**, for USB device mode;
- **USB Video Class (UVC)**;
- **USB Audio Class 2.0 (UAC2)**;
- AArch64/NEON video conversion techniques derived from the layout used by **libyuv**;
- the MIT-licensed Paula emulator by **Akustikrausch / Andreas Wendorf**.

Upstream Emu68:

```text
https://github.com/michalsc/Emu68
```

Paula emulator source:

```text
https://github.com/akustikrausch/amiga-paula-8364-emulator
```

---

# License

Source files retain their respective original licenses.

The software Paula core carries its original MIT license in:

```text
src/raspi/paula/LICENSE
```

Check the headers of the supplied sources and the licensing terms of Emu68, PiStorm, Framethrower and any incorporated or adapted third-party code before redistribution.

# Emu68 Framethrower UVC - AI CODED

**Turn a PiStorm-equipped Amiga into a standard USB video source for modern computers.**

This Emu68 experiment takes the live Amiga video captured by **Framethrower Denise**, converts it in real time, and exposes it to a PC as a normal **USB Video Class (UVC)** device.

To the host computer, the Amiga behaves much like a USB capture device or webcam.

No proprietary Windows capture driver is required.

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
RGB565 -> I420 conversion
    |
    v
USB Video Class
    |
    v
Windows / Linux / macOS
```

The current milestone streams:

```text
720 x 576
25 fps
I420 / YUV 4:2:0
USB UVC
```

This makes it possible to capture or display the Amiga video directly in standard host applications.

---

## Why this is interesting

A normal Amiga video-capture setup usually needs an external capture device.

With this experiment, the PiStorm system itself becomes the capture interface.

Framethrower already has access to the Amiga video stream. Instead of sending that picture only to HDMI, Emu68 reuses the captured framebuffer and presents it over USB.

That means the same machine can potentially provide:

- live Amiga video capture;
- recording on a modern PC;
- streaming;
- OBS integration;
- screenshots and archival capture;
- remote viewing;
- future combined USB video and audio experiments.

The Amiga software itself does not need to know that any of this is happening.

---

## Host-visible device

The current firmware identifies itself as:

```text
Manufacturer: PiStorm
Product:      Framethrower UVC POC
```

The device uses the standard **USB Video Class** protocol.

On a compatible operating system it should therefore appear automatically as a camera/video-capture source.

---

## Requirements

The current experiment is intended for:

- PiStorm Classic;
- Raspberry Pi 3A+;
- Emu68;
- Framethrower Denise;
- a USB connection from the Pi to the host computer.

The current source is specifically built around the live video framebuffer produced through the Framethrower/Unicam path.

---

## Video format

The validated format in this milestone is:

| Property | Value |
| --- | --- |
| Resolution | 720 × 576 |
| Frame rate | 25 fps |
| Source format | RGB565 |
| USB format | I420 |
| Color format | YUV 4:2:0 planar |
| USB class | UVC |
| USB transport | Bulk IN |

720×576 at 25 fps is a natural fit for PAL Amiga video.

The USB stream is uncompressed I420 rather than MJPEG or H.264.

---

## How the frame reaches USB

Framethrower feeds the Raspberry Pi video capture hardware.

Emu68 reads the already-running Unicam framebuffer without stopping or reconfiguring the video capture path.

The original frame is:

```text
720 x 576 RGB565
```

Before transmission it is converted to:

```text
720 x 576 I420
```

The converted frame is then sent to the host through the USB UVC interface.

---

## CPU usage

The implementation deliberately divides the work between Raspberry Pi cores.

### CPU1

CPU1 performs the expensive full-frame conversion:

```text
RGB565 -> I420
```

The converter uses AArch64 NEON acceleration and follows the same basic conversion layout used by libyuv.

### CPU3

CPU3 owns:

- the DWC2 USB controller;
- UVC protocol handling;
- frame scheduling;
- USB transmission;
- frame-buffer rotation.

This separation keeps the relatively expensive pixel conversion away from the USB transport path.

---

## Frame timing

The video stream follows the real Framethrower/Unicam frame cadence.

PAL video produces a 50 Hz frame/event rhythm.

The UVC producer deliberately selects every second event:

```text
50 Hz source cadence
        |
        v
       /2
        |
        v
25 fps USB video
```

This keeps USB output synchronized with the incoming Amiga video rather than simply generating frames from an unrelated software timer.

---

## Triple buffering

The implementation uses multiple complete video buffers so that capture, conversion and USB transmission do not all have to operate on the same memory at the same time.

Conceptually:

```text
Frame A
USB is transmitting

Frame B
CPU1 is converting

Frame C
available / next frame
```

When a newer completed frame is available, stale unpublished video may be discarded in favour of the newest image.

For live capture this is preferable to allowing latency to grow indefinitely.

---

## Using it on Windows

Connect the Pi USB device port to the Windows PC after booting the modified Emu68 firmware.

Windows should enumerate a UVC video device named approximately:

```text
Framethrower UVC POC
```

Because UVC is a standard USB class, Windows does not require a project-specific driver.

You can then open any application capable of using a camera or video-capture source.

Typical examples include:

- OBS Studio;
- VLC;
- camera/capture applications;
- video-conferencing or streaming applications that accept UVC sources.

Select:

```text
Framethrower UVC POC
```

as the video source.

---

## OBS example

In OBS Studio:

```text
Sources
→ Add
→ Video Capture Device
```

Create a new source and select:

```text
Framethrower UVC POC
```

The expected native mode is:

```text
720x576
25 fps
I420
```

For archival capture, keeping the native 720×576 frame avoids an unnecessary resize before recording.

---

## Linux

On Linux the device should be handled by the normal `uvcvideo` driver.

Typical applications can access it through V4L2.

For example, after enumeration a device such as:

```text
/dev/video0
```

may appear.

The exact number depends on the other video devices already installed on the machine.

---

## What Framethrower is doing

This experiment does not emulate the Amiga display.

The source picture comes from the real Framethrower video-capture path.

The relevant hardware produces a live RGB565 framebuffer through the Raspberry Pi Unicam block.

The UVC code reads that framebuffer and converts it for USB transmission.

The source remains independent of the UVC transport:

```text
Framethrower capture
       |
       +----> normal video path
       |
       +----> Emu68 UVC capture
```

The UVC code is therefore a consumer of the already-running video capture rather than the owner of Framethrower itself.

---

## No on-screen HUD

This milestone is the `nohud` variant.

Development versions displayed diagnostic text and counters inside the outgoing video frame.

The current version deliberately leaves the captured picture clean.

Diagnostics remain available internally, but they are not painted over the Amiga image.

---

## Current architecture

At a high level:

```text
                 Framethrower
                       |
                       v
                 Unicam RGB565
                       |
                       v
                +-------------+
                |    CPU1     |
                | RGB565->I420|
                |    NEON     |
                +-------------+
                       |
                 converted frame
                       |
                       v
                +-------------+
                |    CPU3     |
                | UVC + DWC2  |
                +-------------+
                       |
                       v
                     USB
                       |
                       v
                     Host
```

The complete frame remains immutable while USB is transmitting it.

This avoids the most obvious class of tearing caused by modifying a buffer while it is being sent.

---

## Source files

The experiment is compact.

The supplied source archive contains:

```text
CMakeLists.txt
ps_classic_protocol.c
start.c
start_rpi64.c
support_rpi.c
usb_uvc_poc-poc43-cpu1-libyuv-i420-25fps-nohud-v3.c
uvc_libyuv_neon.c
```

The two UVC-specific files are:

```text
usb_uvc_poc-poc43-cpu1-libyuv-i420-25fps-nohud-v3.c
uvc_libyuv_neon.c
```

The remaining files integrate the experiment into the Emu68 PiStorm Classic startup and build.

---

## Building

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

The resulting Emu68 image contains the UVC experiment.

As with any low-level experimental Emu68 build, keep a known-good firmware image available for rollback.

---

## Current status

This is an experimental but substantial proof of concept.

The current milestone has established:

- live Framethrower video as the source;
- read-only use of the running Unicam framebuffer;
- native 720×576 capture;
- PAL-synchronized 25 fps production;
- RGB565 to I420 conversion;
- AArch64/NEON conversion on CPU1;
- independent UVC/DWC2 handling on CPU3;
- triple-buffer frame ownership;
- standard USB UVC enumeration;
- frame-oriented UVC transport;
- clean output without the development HUD.

---

## Limitations

This is not intended to be a finished commercial capture-card implementation.

The current version deliberately keeps the format simple.

Notable limitations include:

- one primary video mode;
- uncompressed I420 output;
- no hardware video compression;
- no integrated USB audio in this milestone;
- compatibility has not been tested with every host application;
- USB reset/reconnect handling remains experimental;
- this is still a proof-of-concept Emu68 branch.

Because the video is uncompressed, bandwidth and host USB behaviour matter more than they would with MJPEG or H.264 capture.

---

## Possible future work

Natural extensions include:

- cleaner host compatibility;
- additional UVC formats;
- selectable PAL/NTSC modes;
- alternate resolutions;
- improved USB reconnect handling;
- integration with the Paula USB Audio experiment;
- synchronized USB audio + video;
- lower-latency capture paths;
- optional host-side scaling or deinterlacing;
- further cleanup for eventual public integration.

A particularly interesting long-term goal is a single PiStorm USB connection carrying both:

```text
Amiga video
+
Paula audio
```

as standard host multimedia devices.

---

## Credits

This is an unofficial experimental Emu68 / PiStorm extension.

It is not an official Emu68 release.

The project builds on:

- **Emu68**, for the PiStorm execution environment;
- **PiStorm**, for the hardware platform;
- **Framethrower Denise**, for the live Amiga video capture path;
- the Raspberry Pi **Unicam** capture hardware;
- the Raspberry Pi **DWC2** USB device controller;
- the USB Video Class specification;
- conversion techniques derived from the layout used by **libyuv**.

Upstream Emu68:

```text
https://github.com/michalsc/Emu68
```

---

## License

Source files retain their respective original licenses.

Check the headers of the supplied sources and the licensing terms of Emu68, Framethrower and any incorporated or adapted third-party code before redistribution.

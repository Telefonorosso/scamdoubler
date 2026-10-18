/*
 * PiStorm Classic / Raspberry Pi 3A+ bare-metal UVC + UAC2 + Paula AV POC3.
 *
 * Structural reset after the UAC1/HS experiments:
 *   - keep the hardware-validated POC43 UVC path unchanged;
 *   - use a standards-shaped UAC2 microphone function with its own correct IAD;
 *   - expose a fixed internal 48 kHz Clock Source (read-only CUR/RANGE controls);
 *   - run High-Speed isochronous audio at the native 125 us service interval
 *     (bInterval=1), six stereo S16 frames = 24 bytes per microframe;
 *   - keep EP2 completely lazy until SET_INTERFACE(AS, alt1) has completed on EP0;
 *   - replace the validated 1 kHz source with the hardware-validated Paula
 *     CACHE-FIX2 renderer feeding a bounded SPSC PCM ring on CPU2.
 *
 * CPU3 remains the sole DWC2 owner. CPU1 remains the validated Framethrower
 * RGB565->I420 worker. CPU0 remains the sole physical Chip RAM cache reader;
 * CPU2 runs Paula plus the PiStorm housekeeper cooperatively.
 */
#include <stdint.h>
#include "support.h"
#include "paula_probe.h"
#include "paula/paula_live.h"
#include "paula/paula_mirror.h"
#include "paula/paula_replay.h"

#if defined(PISTORM_CLASSIC)

extern uint32_t set_power_state(uint32_t device_id, uint32_t state);

/*
 * Regular-boot build: USB diagnostics remain available through kprintf/probe
 * state, but nothing is painted into the Emu68 framebuffer.  Keep the existing
 * call sites as no-ops so this cleanup cannot perturb the validated USB paths.
 */

enum {
    UVC_S_INIT       = 1U << 0,
    UVC_S_HW_OK      = 1U << 1,
    UVC_S_USBRST     = 1U << 2,
    UVC_S_ENUMDONE   = 1U << 3,
    UVC_S_GETDEV     = 1U << 4,
    UVC_S_GETCFG     = 1U << 5,
    UVC_S_GETSTR     = 1U << 6,
    UVC_S_SETADDR    = 1U << 7,
    UVC_S_SETCFG     = 1U << 8,
    UVC_S_PROBE      = 1U << 9,
    UVC_S_COMMIT     = 1U << 10,
    UVC_S_STREAM     = 1U << 11,
    UVC_S_HW_FAIL    = 1U << 12,
    UVC_S_TX_START   = 1U << 13,
    UVC_S_TX_DONE    = 1U << 14,
    UVC_S_TX_NOFIFO  = 1U << 15,
    UVC_S_DIAG_DONE   = 1U << 16,
    UVC_S_AUDIO_ALT1  = 1U << 17,
    UVC_S_AUDIO_TX    = 1U << 18,
    UVC_S_AUDIO_RATE  = 1U << 19,
    UVC_S_UVC_IF0     = 1U << 20,
    UVC_S_AUDIO_ALT0  = 1U << 21,
    UVC_S_SETUP_STALL = 1U << 22
};

static void uvc_screen_line(uint32_t bit, const char *s)
{
    (void)bit;
    (void)s;
}


#define USB2_BASE               0xf2980000UL
#define USB_GAHBCFG             0x008
#define USB_GUSBCFG             0x00c
#define USB_GRSTCTL             0x010
#define USB_GINTSTS             0x014
#define USB_GINTMSK             0x018
#define USB_GRXSTSP             0x020
#define USB_GRXFSIZ             0x024
#define USB_GNPTXFSIZ           0x028
#define USB_GSNPSID             0x040
#define USB_DPTXFSIZ(n)         (0x100 + ((n) * 4))
#define USB_DCFG                0x800
#define USB_DCTL                0x804
#define USB_DSTS                0x808
#define USB_DIEPMSK             0x810
#define USB_DOEPMSK             0x814
#define USB_DAINT               0x818
#define USB_DAINTMSK            0x81c
#define USB_DIEPEMPMSK          0x834
#define USB_DIEPCTL(n)          (0x900 + ((n) * 0x20))
#define USB_DIEPINT(n)          (0x908 + ((n) * 0x20))
#define USB_DIEPTSIZ(n)         (0x910 + ((n) * 0x20))
#define USB_DTXFSTS(n)          (0x918 + ((n) * 0x20))
#define USB_DOEPCTL(n)          (0xb00 + ((n) * 0x20))
#define USB_DOEPINT(n)          (0xb08 + ((n) * 0x20))
#define USB_DOEPTSIZ(n)         (0xb10 + ((n) * 0x20))
#define USB_FIFO(n)             (0x1000 + ((n) * 0x1000))

#define USB_GAHBCFG_GLBL_INTR_EN        (1U << 0)
#define USB_GAHBCFG_DMA_EN              (1U << 5)
#define USB_GUSBCFG_FORCEDEVMODE        (1U << 30)
#define USB_GUSBCFG_FORCEHOSTMODE       (1U << 29)
#define USB_GUSBCFG_HNPCAP              (1U << 9)
#define USB_GUSBCFG_SRPCAP              (1U << 8)
#define USB_GUSBCFG_TOUTCAL_MASK        0x7U
#define USB_GRSTCTL_AHBIDLE             (1U << 31)
#define USB_GRSTCTL_TXFNUM_ALL          (0x10U << 6)
#define USB_GRSTCTL_TXFNUM(n)           (((uint32_t)(n) & 0x1fU) << 6)
#define USB_GRSTCTL_TXFFLSH             (1U << 5)
#define USB_GRSTCTL_RXFFLSH             (1U << 4)
#define USB_GRSTCTL_CSFTRST             (1U << 0)
#define USB_GINTSTS_OEPINT              (1U << 19)
#define USB_GINTSTS_IEPINT              (1U << 18)
#define USB_GINTSTS_ENUMDONE            (1U << 13)
#define USB_GINTSTS_USBRST              (1U << 12)
#define USB_GINTSTS_RXFLVL              (1U << 4)
#define USB_GINTSTS_CURMODE_HOST        (1U << 0)
#define USB_DCFG_DEVADDR_MASK           (0x7fU << 4)
#define USB_DCFG_DEVADDR(a)             (((uint32_t)(a) & 0x7fU) << 4)
#define USB_DCFG_DEVSPD_MASK            3U
#define USB_DCFG_DEVSPD_HS              0U
#define USB_DCTL_SFTDISCON              (1U << 1)
#define USB_DSTS_ENUMSPD_MASK           (3U << 1)
#define USB_DSTS_ENUMSPD_HS             (0U << 1)
#define USB_DSTS_SOFFN_SHIFT            8U
#define USB_DAINT_INEP(n)               (1U << (n))
#define USB_DAINT_OUTEP(n)              (1U << ((n) + 16))
#define USB_DXEPCTL_EPENA               (1U << 31)
#define USB_DXEPCTL_EPDIS               (1U << 30)
#define USB_DXEPCTL_SODDFRM             (1U << 29)
#define USB_DXEPCTL_SETD0PID            (1U << 28)
#define USB_DXEPCTL_SEVNFRM             (1U << 28)
#define USB_DXEPCTL_SNAK                (1U << 27)
#define USB_DXEPCTL_CNAK                (1U << 26)
#define USB_DXEPCTL_TXFNUM(n)           (((uint32_t)(n) & 0xfU) << 22)
#define USB_DXEPCTL_STALL               (1U << 21)
#define USB_DXEPCTL_EPTYPE_BULK         (2U << 18)
#define USB_DXEPCTL_EPTYPE_ISOC         (1U << 18)
#define USB_DXEPCTL_USBACTEP            (1U << 15)
#define USB_DXEPCTL_MPS(n)              ((uint32_t)(n) & 0x7ffU)
#define USB_DXEPINT_SETUP               (1U << 3)
#define USB_DXEPINT_EPDISBLD            (1U << 1)
#define USB_DXEPINT_TXFEMP             (1U << 7)
#define USB_DXEPINT_XFERCOMPL           (1U << 0)
#define USB_DXEPTSIZ_PKTCNT(n)          (((uint32_t)(n) & 0x3ffU) << 19)
#define USB_DXEPTSIZ_XFERSIZE(n)        ((uint32_t)(n) & 0x7ffffU)
#define USB_DXEPTSIZ_MC(n)              (((uint32_t)(n) & 3U) << 29)
#define USB_DIEPTSIZ0_PKTCNT(n)         (((uint32_t)(n) & 3U) << 19)
#define USB_DIEPTSIZ0_XFERSIZE(n)       ((uint32_t)(n) & 0x7fU)
#define USB_DOEPTSIZ0_SUPCNT(n)         (((uint32_t)(n) & 3U) << 29)
#define USB_DOEPTSIZ0_PKTCNT            (1U << 19)
#define USB_GRXSTS_PKTSTS(v)            (((v) >> 17) & 0xfU)
#define USB_GRXSTS_BYTECNT(v)           (((v) >> 4) & 0x7ffU)
#define USB_GRXSTS_EPNUM(v)             ((v) & 0xfU)
#define USB_PKTSTS_OUTRX                 2U
#define USB_PKTSTS_SETUPRX               6U

#define USB_REQ_GET_STATUS               0x00
#define USB_REQ_CLEAR_FEATURE            0x01
#define USB_REQ_SET_ADDRESS              0x05
#define USB_REQ_GET_DESCRIPTOR           0x06
#define USB_REQ_GET_CONFIGURATION        0x08
#define USB_REQ_SET_CONFIGURATION        0x09
#define USB_REQ_GET_INTERFACE            0x0a
#define USB_REQ_SET_INTERFACE            0x0b
#define USB_DT_DEVICE                    1
#define USB_DT_CONFIG                    2
#define USB_DT_STRING                    3
#define USB_DT_DEVICE_QUALIFIER          6

#define UVC_SET_CUR                       0x01
#define UVC_GET_CUR                       0x81
#define UVC_GET_MIN                       0x82
#define UVC_GET_MAX                       0x83
#define UVC_GET_RES                       0x84
#define UVC_GET_LEN                       0x85
#define UVC_GET_INFO                      0x86
#define UVC_GET_DEF                       0x87
#define UVC_VS_PROBE_CONTROL              0x01
#define UVC_VS_COMMIT_CONTROL             0x02

#define UVC_EP_VIDEO_IN                   1U

/* AV POC3 composite layout: interfaces 0/1 = UVC, 2/3 = UAC2. */
#define UAC_IF_CONTROL                    2U
#define UAC_IF_STREAMING                  3U
#define USB_AUDIO_EP_IN                   2U
#define USB_AUDIO_RATE                    48000U
#define USB_AUDIO_CHANNELS                2U
#define USB_AUDIO_BYTES_PER_SAMPLE        2U
/* High-Speed bInterval=1 => one service every 125 us.  48 kHz is exactly
 * six audio frames per microframe.  Advertise room for +/- one audio slot. */
#define USB_AUDIO_FRAMES_PER_UFRAME       6U
#define USB_AUDIO_MAX_FRAMES_UFRAME       7U
#define USB_AUDIO_PACKET_BYTES            (USB_AUDIO_FRAMES_PER_UFRAME * USB_AUDIO_CHANNELS * USB_AUDIO_BYTES_PER_SAMPLE)
#define USB_AUDIO_MAX_PACKET              (USB_AUDIO_MAX_FRAMES_UFRAME * USB_AUDIO_CHANNELS * USB_AUDIO_BYTES_PER_SAMPLE)
#define USB_AUDIO_MIN_FRAMES_UFRAME       5U
#define USB_AUDIO_RING_FRAMES             2048U
#define USB_AUDIO_RING_MASK               (USB_AUDIO_RING_FRAMES - 1U)
#define USB_AUDIO_TARGET_FILL             384U

/* UAC2 fixed-clock control plane. */
#define UAC2_REQ_CUR                       0x01U
#define UAC2_REQ_RANGE                     0x02U
#define UAC2_CLOCK_ID                      4U
#define UAC2_CS_SAM_FREQ                   0x01U
#define UVC_WIDTH                         720U
#define UVC_HEIGHT                        576U
#define UVC_Y_BYTES                       (UVC_WIDTH * UVC_HEIGHT)
#define UVC_UV_BYTES                      ((UVC_WIDTH / 2U) * (UVC_HEIGHT / 2U))
#define UVC_FRAME_BYTES                   (UVC_Y_BYTES + 2U * UVC_UV_BYTES)
#define UVC_FPS                           25U
#define UVC_INTERVAL_100NS                400000U
#define UVC_INTERVAL_MAX_100NS            10000000U
#define UVC_INTERVAL_STEP_100NS           1000000U
#define UVC_CLOCK_FREQUENCY               27000000U
#define UVC_TINYUSB_PAYLOAD_MAX          32768U
#define UVC_BITRATE                       (UVC_WIDTH * UVC_HEIGHT * 12U * UVC_FPS)

#define WBVAL(x) ((uint8_t)((x) & 0xff)), ((uint8_t)(((x) >> 8) & 0xff))
#define DBVAL(x) WBVAL((uint32_t)(x)), WBVAL(((uint32_t)(x)) >> 16)

struct usb_setup_packet {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
};

static void uvc_screen_setup_stall(const struct usb_setup_packet *r)
{
    (void)r;
}


#define UVC_STREAM_CTRL_LEN 48U

/* UVC VideoStreaming Probe/Commit byte offsets (UVC 1.5 / Linux uvcvideo). */
#define UVC_SC_BMHINT             0U
#define UVC_SC_FORMAT             2U
#define UVC_SC_FRAME              3U
#define UVC_SC_INTERVAL           4U
#define UVC_SC_KEYFRAME           8U
#define UVC_SC_PFRAME            10U
#define UVC_SC_QUALITY           12U
#define UVC_SC_WINDOW            14U
#define UVC_SC_DELAY             16U
#define UVC_SC_FRAME_SIZE        18U
#define UVC_SC_PAYLOAD_SIZE      22U
#define UVC_SC_CLOCK             26U
#define UVC_SC_FRAMING           30U
#define UVC_SC_PREF_VERSION      31U
#define UVC_SC_MIN_VERSION       32U
#define UVC_SC_MAX_VERSION       33U

static uint32_t uvc_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void uvc_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void uvc_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static volatile uint8_t usb_hw_up;
static volatile uint8_t usb_housekeeper_enabled;
static volatile uint8_t usb_configured;
static volatile uint8_t usb_stream_committed;
static volatile uint8_t usb_ep1_busy;
static volatile uint8_t usb_audio_alt;
static volatile uint8_t usb_audio_hw_active;
static volatile uint8_t usb_audio_ep_action_pending; /* 1=start after EP0 status, 2=stop */
static volatile uint8_t usb_ep2_busy;
static volatile uint32_t usb_audio_packets;
static volatile uint32_t usb_audio_xfr_errors;
static volatile uint32_t usb_audio_underruns;
static volatile uint32_t usb_audio_overruns;
static volatile uint8_t av_cpu2_enabled;
static volatile uint8_t usb_ep0_out_kind;

typedef struct {
    int16_t left;
    int16_t right;
} usb_audio_frame;

static usb_audio_frame audio_ring[USB_AUDIO_RING_FRAMES];
static volatile uint32_t audio_wr;
static volatile uint32_t audio_rd;
static uint8_t audio_tx[USB_AUDIO_MAX_PACKET];
static uint16_t usb_bulk_mps = 64;
static uint16_t uvc_payload_size = 64;
static uint64_t next_frame_tick;
static uint64_t timer_freq;
static uint8_t frame[UVC_FRAME_BYTES];
static uint8_t frame_next[UVC_FRAME_BYTES];
static uint8_t frame_third[UVC_FRAME_BYTES];

static uint8_t *video_frame_buf = frame;
static uint8_t *producer_frame_buf = frame_next;
static uint8_t *ready_frame_buf;
static uint8_t *spare_frame_buf = frame_third;
static uint8_t tx_stage[UVC_TINYUSB_PAYLOAD_MAX];

/*
 * POC41 triple-buffer producer state.
 *
 * video_frame_buf: immutable while USB transmits it.
 * ready_frame_buf: newest completed frame waiting for publication.
 * producer_frame_buf: free buffer or buffer currently being filled.
 * spare_frame_buf: third free buffer when available.
 *
 * Latest-frame-wins: if a newer frame completes while an older ready frame is
 * still waiting, the stale ready frame is recycled instead of blocking capture.
 */
static volatile uint8_t producer_busy;
static volatile uint8_t producer_ready;
static uintptr_t producer_src_start;

/*
 * POC43 CPU1 producer mailbox.
 * CPU3 is the sole owner of UVC/DWC2 and of the triple-buffer pointer rotation.
 * CPU1 receives only one immutable source pointer and one destination pointer,
 * converts the complete RGB565 frame, then publishes completion.
 */
static volatile uintptr_t cpu1_job_src;
static volatile uintptr_t cpu1_job_dst;
static volatile uint8_t cpu1_job_pending;
static volatile uint8_t cpu1_job_done;
static volatile uint8_t cpu1_worker_online;
static volatile uint32_t cpu1_jobs_started;
static volatile uint32_t cpu1_jobs_done;


/*
 * TinyUSB DWC2 slave-mode style IN transfer state.
 * One UVC payload may span several 512-byte USB packets.  We arm DIEPTSIZ
 * for the whole logical transfer, push only whole packets that currently fit
 * in the TX FIFO, then continue from TXFE until every byte has been queued.
 */
static const uint8_t *video_tx_buf;
static uint32_t video_tx_total;
static uint32_t video_tx_written;

/*
 * POC14 frame-oriented producer state, modeled after rp2040-uvc's
 * tud_video_n_frame_xfer() / frame-complete callback flow.
 *
 * The application owns one whole video frame at a time.  Individual UVC
 * payloads are chained immediately from EP1 XFRC, never by a later
 * housekeeper turn.
 */
static volatile uint8_t video_frame_busy;
static uint32_t video_frame_pos;
static uint8_t video_frame_fid;

/* POC25 transport diagnostics, rendered into the frozen YUY2 frame. */
static uint64_t diag_stream_start_tick;
static uint64_t diag_frame_start_tick;
static uint64_t diag_last_hud_tick;
static uint64_t diag_last_txfe_tick;
static uint64_t diag_max_txfe_gap_ticks;
static uint64_t diag_last_frame_ticks;
static uint32_t diag_frames_done;
static uint32_t diag_xfrc_total;
static uint32_t diag_txfe_total;
static uint32_t diag_fifo_wait_total;
static uint32_t diag_xfrc_frame;
static uint32_t diag_txfe_frame;
static uint32_t diag_fifo_wait_frame;
static uint32_t diag_last_xfrc_frame;
static uint32_t diag_last_txfe_frame;
static uint32_t diag_last_fifo_wait_frame;
static uint64_t diag_last_max_txfe_gap_ticks;

/*
 * DWC2 EP0 DIEPTSIZ0.XFERSIZE is only 7 bits wide.  The UVC configuration
 * descriptor is 159 bytes, so it must be emitted as consecutive full-size
 * 64-byte EP0 transactions (64 + 64 + 31).  Keeping a persistent staging
 * buffer is also required because the next chunk is sent after the previous
 * IN transfer-complete interrupt/poll event.
 */
static uint8_t ep0_tx_buf[384];
static uint16_t ep0_tx_len;
static uint16_t ep0_tx_pos;

static uint8_t probe_ctrl[UVC_STREAM_CTRL_LEN];
static uint8_t commit_ctrl[UVC_STREAM_CTRL_LEN];

enum {
    EP0_IDLE = 0,
    EP0_IN_DATA,
    EP0_OUT_STATUS,
    EP0_OUT_DATA,
    EP0_IN_STATUS
};
static volatile uint8_t ep0_state;

static const uint8_t device_desc[] = {
    18, USB_DT_DEVICE,
    0x00, 0x02,
    0xef, 0x02, 0x01,       /* Misc/IAD composite device */
    64,
    0x25, 0x05,
    0xb7, 0xa4,             /* AV POC3: UVC + UAC2 HS + Paula */
    0x12, 0x00,
    1, 2, 3,
    1
};

static const uint8_t qualifier_desc[] = {
    10, USB_DT_DEVICE_QUALIFIER,
    0x00, 0x02,
    0xef, 0x02, 0x01,
    64, 1, 0
};

/*
 * Composite High-Speed configuration, 277 bytes / four interfaces:
 *   0/1  UVC 1.5 Framethrower, EP1 IN bulk (POC43 layout)
 *   2/3  UAC2 capture function, EP2 IN asynchronous ISO
 *
 * The UAC2 block follows the USB Audio 2.0 microphone shape used by modern
 * stacks: a dedicated Audio IAD (class 0x01, function subclass 0, protocol
 * 0x20), one AC interface, one fixed internal Clock Source, one physical
 * Input Terminal and one USB-streaming Output Terminal.  There are no Feature
 * Unit controls in this POC.  Sample rate is reported through the Clock Source
 * CUR/RANGE requests, not through endpoint SET_CUR as in UAC1.
 *
 * HS audio uses bInterval=1: 125 us.  At 48 kHz that is exactly 6 stereo
 * S16 frames = 24 bytes per service.  wMaxPacketSize=28 allows +/- one stereo
 * audio slot; the Paula ring normally sends 24 bytes and may send 20/28
 * bytes to absorb small producer/USB clock drift without resampling.
 */
static const uint8_t config_desc[] = {
    9, USB_DT_CONFIG, WBVAL(277), 4, 1, 0, 0x80, 50,

    /* IAD: Video function, interfaces 0..1 */
    8, 0x0b, 0, 2, 0x0e, 0x03, 0x00, 2,

    /* Interface 0: VideoControl */
    9, 4, 0, 0, 0, 0x0e, 0x01, 0x01, 0,

    /* VC header, total class-specific VC length = 40 */
    13, 0x24, 0x01, 0x50, 0x01, WBVAL(40), DBVAL(UVC_CLOCK_FREQUENCY), 1, 1,

    /* Camera Input Terminal ID 1, no camera controls */
    18, 0x24, 0x02, 1, 0x01, 0x02, 0, 0,
    WBVAL(0), WBVAL(0), WBVAL(0), 3, 0, 0, 0,

    /* Streaming Output Terminal ID 2, source = terminal 1 */
    9, 0x24, 0x03, 2, 0x01, 0x01, 0, 1, 0,

    /* Interface 1: VideoStreaming; bulk endpoint is present on alt 0 */
    9, 4, 1, 0, 1, 0x0e, 0x02, 0x01, 0,

    /* VS input header, one format, linked to terminal 2 */
    14, 0x24, 0x01, 1, WBVAL(77), 0x81, 0, 2, 0, 0, 0, 1, 0,

    /* Uncompressed I420 */
    27, 0x24, 0x04, 1, 1,
    'I','4','2','0', 0x00,0x00,0x10,0x00,0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71,
    12, 1, 0, 0, 0, 0,

    /* Native 720x576 @ 25 fps, one discrete frame interval. */
    30, 0x24, 0x05, 1, 0,
    WBVAL(UVC_WIDTH), WBVAL(UVC_HEIGHT),
    DBVAL(UVC_BITRATE),
    DBVAL(UVC_BITRATE),
    DBVAL(UVC_FRAME_BYTES),
    DBVAL(UVC_INTERVAL_100NS),
    1,
    DBVAL(UVC_INTERVAL_100NS),

    /* Color matching */
    6, 0x24, 0x0d, 1, 1, 4,

    /* EP1 IN bulk; wMaxPacketSize patched to 64/512 at GET_DESCRIPTOR. */
    7, 5, 0x81, 0x02, WBVAL(512), 0,

    /* UAC2 IAD: Audio function, interfaces 2..3.
     * Function subclass is UNDEFINED (0), protocol 0x20 as required by UAC2. */
    8, 0x0b, UAC_IF_CONTROL, 2, 0x01, 0x00, 0x20, 0,

    /* Interface 2: AudioControl, UAC2 */
    9, 4, UAC_IF_CONTROL, 0, 0, 0x01, 0x01, 0x20, 0,

    /* UAC2 AC header: header + Clock Source + Input Terminal + Output Terminal
     * = 9 + 8 + 17 + 12 = 46 bytes. Category = Microphone. */
    9, 0x24, 0x01, 0x00, 0x02, 0x03, WBVAL(46), 0x00,

    /* Clock Source ID 4: internal fixed clock; sample frequency readable only. */
    8, 0x24, 0x0a, UAC2_CLOCK_ID, 0x01, 0x01, 0x00, 0,

    /* Input Terminal ID 1: generic microphone, stereo FL/FR, clock source 4. */
    17, 0x24, 0x02, 1, 0x01, 0x02, 0, UAC2_CLOCK_ID,
    2, DBVAL(0x00000003), 0, WBVAL(0), 0,

    /* Output Terminal ID 3: USB Streaming, source = terminal 1, clock source 4. */
    12, 0x24, 0x03, 3, 0x01, 0x01, 0, 1, UAC2_CLOCK_ID, WBVAL(0), 0,

    /* Interface 3 alt 0: zero bandwidth, UAC2 protocol. */
    9, 4, UAC_IF_STREAMING, 0, 0, 0x01, 0x02, 0x20, 0,

    /* Interface 3 alt 1: active AudioStreaming. */
    9, 4, UAC_IF_STREAMING, 1, 1, 0x01, 0x02, 0x20, 0,

    /* UAC2 AS General: terminal 3, Type I PCM, 2 channels FL/FR. */
    16, 0x24, 0x01, 3, 0x00, 0x01,
    DBVAL(0x00000001), 2, DBVAL(0x00000003), 0,

    /* Type I PCM: 2-byte subslot, 16 valid bits. Rate comes from Clock Source. */
    6, 0x24, 0x02, 0x01, 2, 16,

    /* EP2 IN ISO asynchronous data endpoint; HS bInterval=1 => 125 us. */
    7, 5, 0x82, 0x05, WBVAL(USB_AUDIO_MAX_PACKET), 1,

    /* UAC2 class-specific ISO endpoint: no endpoint controls, variable packet OK. */
    8, 0x25, 0x01, 0x00, 0x00, 0x00, WBVAL(0)
};

_Static_assert(sizeof(config_desc) == 277U, "AV POC3 config descriptor size");

static const uint8_t str0[] = { 4, USB_DT_STRING, 0x09, 0x04 };
static const uint8_t str1[] = {
    16, USB_DT_STRING, 'P',0,'i',0,'S',0,'t',0,'o',0,'r',0,'m',0
};
static const uint8_t str2[] = {
    58, USB_DT_STRING,
    'F',0,'r',0,'a',0,'m',0,'e',0,'t',0,'h',0,'r',0,'o',0,'w',0,'e',0,'r',0,
    ' ',0,'+',0,' ',0,'P',0,'a',0,'u',0,'l',0,'a',0,' ',0,'A',0,'V',0,' ',0,
    'P',0,'O',0,'C',0,'3',0
};
static const uint8_t str3[] = {
    18, USB_DT_STRING, 'A',0,'V',0,'P',0,'O',0,'C',0,'0',0,'1',0,'2',0
};

static inline uint32_t rd(uint32_t off)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(USB2_BASE + off);
    uint32_t v = *p;
    dsb();
    return LE32(v);
}

static inline void wr(uint32_t off, uint32_t v)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(USB2_BASE + off);
    *p = LE32(v);
    dsb();
}

static void delay_ms(uint32_t ms)
{
    uint64_t start, now, freq, ticks;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(start));
    ticks = (freq * ms) / 1000U;
    do {
        asm volatile("mrs %0, CNTPCT_EL0" : "=r"(now));
    } while ((now - start) < ticks);
}

static int wait_mask(uint32_t off, uint32_t mask, uint32_t wanted, uint32_t loops)
{
    while (loops--)
        if ((rd(off) & mask) == wanted)
            return 1;
    return 0;
}

static void fifo_write(unsigned ep, const uint8_t *buf, uint32_t len)
{
    volatile uint32_t *fifo =
        (volatile uint32_t *)(uintptr_t)(USB2_BASE + USB_FIFO(ep));
    while (len) {
        uint32_t w = 0;
        uint32_t n = len > 4U ? 4U : len;
        for (uint32_t i = 0; i < n; ++i)
            w |= ((uint32_t)buf[i]) << (8U * i);
        *fifo = LE32(w);
        dsb();
        buf += n;
        len -= n;
    }
}

static void fifo_read(uint8_t *buf, uint32_t len)
{
    volatile uint32_t *fifo =
        (volatile uint32_t *)(uintptr_t)(USB2_BASE + USB_FIFO(0));
    while (len) {
        uint32_t w = LE32(*fifo);
        uint32_t n = len > 4U ? 4U : len;
        dsb();
        for (uint32_t i = 0; i < n; ++i)
            *buf++ = (uint8_t)(w >> (8U * i));
        len -= n;
    }
}

static void drain_fifo(uint32_t len)
{
    uint8_t sink[64];
    while (len) {
        uint32_t n = len > sizeof(sink) ? sizeof(sink) : len;
        fifo_read(sink, n);
        len -= n;
    }
}

static void flush_fifos(void)
{
    wr(USB_GRSTCTL, USB_GRSTCTL_TXFNUM_ALL |
                     USB_GRSTCTL_TXFFLSH |
                     USB_GRSTCTL_RXFFLSH);
    (void)wait_mask(USB_GRSTCTL,
                    USB_GRSTCTL_TXFFLSH | USB_GRSTCTL_RXFFLSH,
                    0, 1000000U);
}

static void flush_audio_tx_fifo(void)
{
    wr(USB_GRSTCTL, USB_GRSTCTL_TXFNUM(USB_AUDIO_EP_IN) |
                     USB_GRSTCTL_TXFFLSH);
    (void)wait_mask(USB_GRSTCTL, USB_GRSTCTL_TXFFLSH, 0, 1000000U);
}

static void ep0_arm_setup(void)
{
    wr(USB_DOEPTSIZ(0), USB_DOEPTSIZ0_SUPCNT(3) |
                         USB_DOEPTSIZ0_PKTCNT | 24U);
    wr(USB_DOEPCTL(0), USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
}

static void ep0_arm_out(uint16_t len)
{
    wr(USB_DOEPTSIZ(0), USB_DOEPTSIZ0_PKTCNT | (uint32_t)len);
    wr(USB_DOEPCTL(0), USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
}

static void ep0_send_next_chunk(void)
{
    uint16_t remain;
    uint16_t chunk;

    if (ep0_tx_pos >= ep0_tx_len)
        return;

    remain = (uint16_t)(ep0_tx_len - ep0_tx_pos);

    /*
     * Never use a non-final short packet: USB hosts treat it as end-of-data.
     * 64 bytes is EP0's max packet size and safely fits the 7-bit XFERSIZE.
     */
    chunk = remain > 64U ? 64U : remain;

    wr(USB_DIEPTSIZ(0), USB_DIEPTSIZ0_PKTCNT(1) |
                         USB_DIEPTSIZ0_XFERSIZE(chunk));
    wr(USB_DIEPCTL(0), USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
    fifo_write(0, &ep0_tx_buf[ep0_tx_pos], chunk);
    ep0_tx_pos = (uint16_t)(ep0_tx_pos + chunk);
}

static void ep0_send(const uint8_t *buf, uint16_t len)
{
    if (len > sizeof(ep0_tx_buf))
        len = sizeof(ep0_tx_buf);

    for (uint16_t i = 0; i < len; ++i)
        ep0_tx_buf[i] = buf[i];

    ep0_tx_len = len;
    ep0_tx_pos = 0;

    if (len)
        ep0_send_next_chunk();
}

static void ep0_zlp(void)
{
    wr(USB_DIEPTSIZ(0), USB_DIEPTSIZ0_PKTCNT(1));
    wr(USB_DIEPCTL(0), USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
}

static void ep0_stall(void)
{
    wr(USB_DIEPCTL(0), rd(USB_DIEPCTL(0)) | USB_DXEPCTL_STALL);
    wr(USB_DOEPCTL(0), rd(USB_DOEPCTL(0)) | USB_DXEPCTL_STALL);
}

static void set_address_now(uint8_t addr)
{
    uint32_t dcfg = rd(USB_DCFG);
    dcfg &= ~USB_DCFG_DEVADDR_MASK;
    dcfg |= USB_DCFG_DEVADDR(addr);
    wr(USB_DCFG, dcfg);
}

static uint32_t clamp_interval(uint32_t interval)
{
    (void)interval;
    /* Exactly one discrete frame interval is advertised: 10 fps. */
    return UVC_INTERVAL_100NS;
}

static uint32_t tinyusb_payload_for_interval(uint32_t interval)
{
    uint32_t interval_ms = interval / 10000U;
    uint32_t payload;

    if (!interval_ms)
        interval_ms = 1;

    /*
     * Same policy as TinyUSB video_device.c:
     * ceil(frame_size / interval_ms) + 2-byte UVC header,
     * capped by CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE (256 in the example).
     */
    payload = (UVC_FRAME_BYTES + interval_ms - 1U) / interval_ms + 2U;
    if (payload > UVC_TINYUSB_PAYLOAD_MAX)
        payload = UVC_TINYUSB_PAYLOAD_MAX;
    /*
     * TinyUSB UVC BULK allows dwMaxPayloadTransferSize to exceed the endpoint
     * wMaxPacketSize.  The DWC2 driver packetizes the logical transfer.
     */
    if (payload < 3U)
        payload = 3U;
    return payload;
}

static void stream_ctrl_defaults(uint8_t c[UVC_STREAM_CTRL_LEN])
{
    for (uint32_t i = 0; i < UVC_STREAM_CTRL_LEN; ++i)
        c[i] = 0;

    /*
     * Exact byte representation expected by Linux uvcvideo:
     *  0 bmHint (LE16)
     *  2 bFormatIndex
     *  3 bFrameIndex
     *  4 dwFrameInterval (LE32, units of 100 ns)
     * 18 dwMaxVideoFrameSize (LE32)
     * 22 dwMaxPayloadTransferSize (LE32)
     * 26 dwClockFrequency (LE32)
     * 30..33 framing/version bytes
     * 34..47 UVC 1.5 extension bytes: zero, matching Linux SET_CUR.
     */
    uvc_put_le16(&c[UVC_SC_BMHINT], 1U);
    c[UVC_SC_FORMAT] = 1U;
    c[UVC_SC_FRAME] = 1U;
    uvc_put_le32(&c[UVC_SC_INTERVAL], UVC_INTERVAL_100NS);
    uvc_put_le16(&c[UVC_SC_KEYFRAME], 1U);
    uvc_put_le16(&c[UVC_SC_PFRAME], 0U);
    uvc_put_le16(&c[UVC_SC_QUALITY], 1U);
    uvc_put_le16(&c[UVC_SC_WINDOW], 1U);
    uvc_put_le16(&c[UVC_SC_DELAY], 0U);
    uvc_put_le32(&c[UVC_SC_FRAME_SIZE], UVC_FRAME_BYTES);
    uvc_put_le32(&c[UVC_SC_PAYLOAD_SIZE],
                 tinyusb_payload_for_interval(UVC_INTERVAL_100NS));
    uvc_put_le32(&c[UVC_SC_CLOCK], UVC_CLOCK_FREQUENCY);
    c[UVC_SC_FRAMING] = 0x03U;
    c[UVC_SC_PREF_VERSION] = 1U;
    c[UVC_SC_MIN_VERSION] = 1U;
    c[UVC_SC_MAX_VERSION] = 1U;
}

static void stream_ctrl_normalize(uint8_t c[UVC_STREAM_CTRL_LEN])
{
    uint32_t interval = uvc_get_le32(&c[UVC_SC_INTERVAL]);

    /* Exactly one format, frame and discrete interval are exposed. */
    c[UVC_SC_FORMAT] = 1U;
    c[UVC_SC_FRAME] = 1U;
    if (!interval)
        interval = UVC_INTERVAL_100NS;
    interval = clamp_interval(interval);

    /*
     * Rebuild every field we own explicitly.  This deliberately avoids
     * dependence on CPU endianness, C structure packing, or alignment.
     */
    uvc_put_le16(&c[UVC_SC_BMHINT], 1U);
    uvc_put_le32(&c[UVC_SC_INTERVAL], interval);
    uvc_put_le16(&c[UVC_SC_KEYFRAME], 1U);
    uvc_put_le16(&c[UVC_SC_PFRAME], 0U);
    uvc_put_le16(&c[UVC_SC_QUALITY], 1U);
    uvc_put_le16(&c[UVC_SC_WINDOW], 1U);
    uvc_put_le16(&c[UVC_SC_DELAY], 0U);
    uvc_put_le32(&c[UVC_SC_FRAME_SIZE], UVC_FRAME_BYTES);
    uvc_put_le32(&c[UVC_SC_PAYLOAD_SIZE],
                 tinyusb_payload_for_interval(interval));
    uvc_put_le32(&c[UVC_SC_CLOCK], UVC_CLOCK_FREQUENCY);
    c[UVC_SC_FRAMING] = 0x03U;
    c[UVC_SC_PREF_VERSION] = 1U;
    c[UVC_SC_MIN_VERSION] = 1U;
    c[UVC_SC_MAX_VERSION] = 1U;

    /* Linux sends zero for the UVC 1.5 extension bytes 34..47. */
    for (uint32_t i = 34U; i < UVC_STREAM_CTRL_LEN; ++i)
        c[i] = 0;
}

static void stream_ctrl_get_variant(uint8_t request,
                                    uint8_t c[UVC_STREAM_CTRL_LEN])
{
    stream_ctrl_defaults(c);

    if (request == UVC_GET_RES) {
        /*
         * One discrete frame interval: GET_RES interval is zero.
         * Keep the rest of the 48-byte control deterministic.
         */
        uvc_put_le32(&c[UVC_SC_INTERVAL], 0U);
        uvc_put_le32(&c[UVC_SC_PAYLOAD_SIZE], 0U);
        return;
    }

    uvc_put_le32(&c[UVC_SC_INTERVAL], UVC_INTERVAL_100NS);
    uvc_put_le32(&c[UVC_SC_PAYLOAD_SIZE],
                 tinyusb_payload_for_interval(UVC_INTERVAL_100NS));
}

static void build_test_pattern_into(uint8_t *dst)
{
    static const uint8_t yv[8] = { 235, 210, 170, 145, 106, 81, 41, 16 };
    uint8_t *dst_y = dst;
    uint8_t *dst_u = dst + UVC_Y_BYTES;
    uint8_t *dst_v = dst_u + UVC_UV_BYTES;

    for (uint32_t y = 0; y < UVC_HEIGHT; ++y)
        for (uint32_t x = 0; x < UVC_WIDTH; ++x)
            dst_y[(uintptr_t)y * UVC_WIDTH + x] = yv[(x * 8U) / UVC_WIDTH];

    for (uint32_t i = 0; i < UVC_UV_BYTES; ++i) {
        dst_u[i] = 128;
        dst_v[i] = 128;
    }
}

static void __attribute__((unused)) build_test_pattern(void)
{
    build_test_pattern_into(frame);
}


/* -------------------------------------------------------------------------
 * Framethrower / Unicam live source -- POC9 minimal read-only gate
 * -------------------------------------------------------------------------
 *
 * POC9 deliberately uses the minimum gate validated on the real hardware:
 *   CPE=1, CSI-2 datatype 0x22, stride 1440, IBSA0 non-zero.
 * It does NOT require IBEA0 span checks, BUF0_RDY, FSI/FEI or IBWP activity.
 * The CPU source address is IBSA0 with the VideoCore bus-alias bits removed.
 * If FT has not delivered a picture yet the result may simply be black; later
 * USB frames sample the same live DMA buffer again.
 *
 * Safety properties:
 *   - all Unicam accesses are reads;
 *   - no stop/start and no pointer reload;
 *   - no cache clean/invalidate on the DMA framebuffer;
 *   - reads the full native 720x576 RGB565 framebuffer, one-to-one.
 */
#define FT_UNICAM_BASE          0xf2801000UL
#define FT_UNICAM_CTRL          0x000U
#define FT_UNICAM_STA           0x004U
#define FT_UNICAM_ISTA          0x104U
#define FT_UNICAM_IDI0          0x108U
#define FT_UNICAM_IBSA0         0x110U
#define FT_UNICAM_IBEA0         0x114U
#define FT_UNICAM_IBLS          0x118U
#define FT_UNICAM_IBWP          0x11cU

#define FT_UNICAM_CPE           (1U << 0)
#define FT_CSI2_RGB565          0x22U
#define FT_WIDTH                720U
#define FT_HEIGHT               576U
#define FT_STRIDE               (FT_WIDTH * 2U)
#define FT_FRAME_BYTES          (FT_STRIDE * FT_HEIGHT)

struct ft_diag_regs {
    uint32_t ctrl, sta, ista, idi0;
    uint32_t ibsa, ibea, ibls, ibwp;
    uintptr_t start, end;
    uint8_t gate;
};

static uint8_t ft_live_logged;
static uint8_t ft_frame_valid;

/*
 * POC32: completely read-only Unicam frame-clock probe.
 *
 * Do NOT acknowledge ISTA and do NOT reconfigure Unicam here.  The purpose of
 * this first experiment is only to discover which already-running hardware
 * signal follows the Framethrower/PAL frame cadence without perturbing Emu68.
 *
 * IBWP is expected to advance while DMA writes a frame and return to the start
 * of the buffer for the next frame.  A backwards move is therefore counted as
 * an IBWP wrap candidate.  STA/ISTA are sampled too, but only transitions are
 * counted because ISTA may be latched until explicitly acknowledged.
 */
#define FT_UNICAM_STA_FSI_S      (1U << 17)
#define FT_UNICAM_STA_FEI_S      (1U << 18)
#define FT_UNICAM_STA_PI0        (1U << 15)
#define FT_UNICAM_STA_BUF0_RDY   (1U << 20)
#define FT_UNICAM_ISTA_FSI       (1U << 0)
#define FT_UNICAM_ISTA_FEI       (1U << 1)

static uint8_t  ftclk_probe_started;
static uint32_t ftclk_last_wp;
static uint32_t ftclk_wp_min;
static uint32_t ftclk_wp_max;
static uint32_t ftclk_last_sta;
static uint32_t ftclk_last_ista;
static uint64_t ftclk_last_report_tick;

static uint32_t ftclk_fsi_total;
static uint32_t ftclk_fei_total;
static uint32_t ftclk_fsi_report_base;
static uint32_t ftclk_fei_report_base;
static uint32_t ftclk_fsi_last_1s;
static uint32_t ftclk_fei_last_1s;
static uint32_t ftclk_frame_seq;
static uint32_t ftclk_pi0_total;
static uint32_t ftclk_pi0_report_base;
static uint32_t ftclk_pi0_last_1s;
static uint32_t ftclk_frame_total;
static uint32_t ftclk_frame_report_base;
static uint32_t ftclk_frame_last_1s;

/* POC41 pipeline-rate diagnostics. */
static uint32_t diag_pi0acc_total;
static uint32_t diag_begin_total;
static uint32_t diag_done_total;
static uint32_t diag_pub_total;
static uint32_t ftclk_last_started_seq;
static volatile uint8_t ftclk_capture_pending;

static inline uint32_t ft_reg_read(uint32_t off)
{
    return LE32(*(volatile uint32_t *)(FT_UNICAM_BASE + off));
}

static void ft_read_diag(struct ft_diag_regs *d)
{
    d->ctrl = ft_reg_read(FT_UNICAM_CTRL);
    d->sta  = ft_reg_read(FT_UNICAM_STA);
    d->ista = ft_reg_read(FT_UNICAM_ISTA);
    d->idi0 = ft_reg_read(FT_UNICAM_IDI0);
    d->ibsa = ft_reg_read(FT_UNICAM_IBSA0);
    d->ibea = ft_reg_read(FT_UNICAM_IBEA0);
    d->ibls = ft_reg_read(FT_UNICAM_IBLS);
    d->ibwp = ft_reg_read(FT_UNICAM_IBWP);
    d->start = (uintptr_t)(d->ibsa & 0x3fffffffU);
    d->end   = (uintptr_t)(d->ibea & 0x3fffffffU);

    d->gate = ((d->ctrl & FT_UNICAM_CPE) != 0 &&
               (d->idi0 & 0xffU) == FT_CSI2_RGB565 &&
               d->ibls == FT_STRIDE &&
               d->start != 0) ? 1U : 0U;
}

static void ft_clock_probe_poll(void)
{
    uint32_t sta, ista, wp;
    uint8_t frame_end;
    uint64_t now;

    sta = ft_reg_read(FT_UNICAM_STA);
    ista = ft_reg_read(FT_UNICAM_ISTA);
    wp = ft_reg_read(FT_UNICAM_IBWP);
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(now));

    if (!ftclk_probe_started) {
        ftclk_probe_started = 1;
        ftclk_last_wp = wp;
        ftclk_wp_min = wp;
        ftclk_wp_max = wp;
        ftclk_last_sta = sta;
        ftclk_last_ista = ista;
        ftclk_last_report_tick = now;
        kprintf("[FTCLK] POC41 PI0-synced producer: STA=%08x ISTA=%08x IBWP=%08x\n",
                sta, ista, wp);
    }

    /*
     * Linux bcm2835-unicam considers frame end to be either:
     *     ISTA.FE  OR  STA.PI0
     *
     * PI0 is the packet-capture status generated by CMP0.  Emu68 configures
     * CMP0 for CSI-2 datatype 1 (Frame End), explicitly to avoid missing
     * frame ends.
     *
     * Acknowledge only the event bits consumed here.  LCI and unrelated
     * status/error bits are deliberately untouched.
     */
    frame_end = ((ista & FT_UNICAM_ISTA_FEI) ||
                 (sta & FT_UNICAM_STA_PI0)) ? 1U : 0U;

    if (ista & FT_UNICAM_ISTA_FSI) {
        ftclk_fsi_total++;
        *(volatile uint32_t *)(uintptr_t)(FT_UNICAM_BASE + FT_UNICAM_ISTA) =
            LE32(FT_UNICAM_ISTA_FSI);
        dsb();
    }

    if (ista & FT_UNICAM_ISTA_FEI) {
        ftclk_fei_total++;
        *(volatile uint32_t *)(uintptr_t)(FT_UNICAM_BASE + FT_UNICAM_ISTA) =
            LE32(FT_UNICAM_ISTA_FEI);
        dsb();
    }

    if (sta & FT_UNICAM_STA_PI0) {
        ftclk_pi0_total++;
        *(volatile uint32_t *)(uintptr_t)(FT_UNICAM_BASE + FT_UNICAM_STA) =
            LE32(FT_UNICAM_STA_PI0);
        dsb();
    }

    if (frame_end) {
        ftclk_frame_total++;
        ftclk_frame_seq++;

        /*
         * Exact PAL 50 -> producer 25 decimation.
         * Latch the request so a PI0 event is not lost while conversion is busy.
         */
        if ((ftclk_frame_seq & 1U) == 0) {
            ftclk_capture_pending = 1;
            diag_pi0acc_total++;
        }
    }

    ftclk_last_wp = wp;
    ftclk_last_sta = sta;
    ftclk_last_ista = ista;

    if (timer_freq && (now - ftclk_last_report_tick) >= timer_freq) {
        ftclk_fsi_last_1s = ftclk_fsi_total - ftclk_fsi_report_base;
        ftclk_fei_last_1s = ftclk_fei_total - ftclk_fei_report_base;
        ftclk_pi0_last_1s = ftclk_pi0_total - ftclk_pi0_report_base;
        ftclk_frame_last_1s = ftclk_frame_total - ftclk_frame_report_base;

        kprintf("[FTCLK] FSI/s=%u FEI/s=%u PI0/s=%u FRAME/s=%u SEQ=%u STA=%08x ISTA=%08x\n",
                ftclk_fsi_last_1s, ftclk_fei_last_1s, ftclk_pi0_last_1s,
                ftclk_frame_last_1s, ftclk_frame_seq, sta, ista);

        ftclk_fsi_report_base = ftclk_fsi_total;
        ftclk_fei_report_base = ftclk_fei_total;
        ftclk_pi0_report_base = ftclk_pi0_total;
        ftclk_frame_report_base = ftclk_frame_total;
        ftclk_last_report_tick = now;
    }
}


/* POC43 libyuv NEON converter lives in a separate translation unit. */
extern void poc43_libyuv_rgb565_to_i420_neon(const uint8_t *src_rgb565,
                                               int src_stride_rgb565,
                                               uint8_t *dst_i420,
                                               int width, int height);

static int video_producer_begin(void)
{
    struct ft_diag_regs d;

    if (producer_busy || producer_frame_buf == 0 || !cpu1_worker_online)
        return 0;

    /*
     * POC43: capture is still clocked by the real Framethrower/Unicam
     * frame-complete cadence.  CPU3 only posts a whole-frame conversion job;
     * it never walks the RGB565 pixels itself.
     */
    if (!ftclk_capture_pending ||
        ftclk_frame_seq == ftclk_last_started_seq)
        return 0;

    /* Never overwrite a mailbox entry that CPU1 has not consumed yet. */
    if (cpu1_job_pending || cpu1_job_done)
        return 0;

    ft_read_diag(&d);
    if (!d.gate)
        return 0;

    producer_src_start = d.start;
    producer_busy = 1;
    ftclk_last_started_seq = ftclk_frame_seq;
    ftclk_capture_pending = 0;

    cpu1_job_src = producer_src_start;
    cpu1_job_dst = (uintptr_t)producer_frame_buf;
    asm volatile("dmb sy" ::: "memory");
    cpu1_job_pending = 1;
    cpu1_jobs_started++;
    diag_begin_total++;
    asm volatile("sev" ::: "memory");
    return 1;
}

/*
 * CPU1-only full-frame RGB565 -> I420 converter.
 * This is the same row decomposition used by libyuv RGB565ToI420(): one UV
 * row for each pair of source lines and one Y row for every source line.
 */
static void cpu1_convert_frame(uintptr_t src_start, uint8_t *dst)
{
    poc43_libyuv_rgb565_to_i420_neon(
        (const uint8_t *)(uintptr_t)src_start, FT_STRIDE, dst, FT_WIDTH, FT_HEIGHT);
}

void emu68_uvc_cpu1_worker(void)
{
    cpu1_worker_online = 1;
    asm volatile("dmb sy; sev" ::: "memory");
    kprintf("[USB-UVC] POC43 CPU1 libyuv-I420 NEON worker online\n");

    for (;;) {
        uintptr_t src;
        uint8_t *dst;

        if (!cpu1_job_pending) {
            asm volatile("wfe");
            continue;
        }

        /* Acquire the src/dst pair published by CPU3 before pending=1. */
        asm volatile("dmb sy" ::: "memory");
        src = cpu1_job_src;
        dst = (uint8_t *)(uintptr_t)cpu1_job_dst;

        cpu1_convert_frame(src, dst);

        /* Publish every destination store before DONE becomes observable. */
        asm volatile("dmb sy" ::: "memory");
        cpu1_job_pending = 0;
        cpu1_jobs_done++;
        cpu1_job_done = 1;
        asm volatile("sev" ::: "memory");
    }
}

/*
 * Reset/re-COMMIT barrier.  Do not recycle any triple-buffer pointer while
 * CPU1 may still be writing to its destination.  CPU1 signals completion with
 * SEV, so the wait normally sleeps rather than burning CPU3.
 */
static void cpu1_producer_quiesce(void)
{
    while (cpu1_job_pending)
        asm volatile("wfe");

    asm volatile("dmb sy" ::: "memory");
    cpu1_job_done = 0;
    producer_busy = 0;
}

/*
 * CPU3 completion harvester.  Pointer rotation remains byte-for-byte equivalent
 * to POC41; the only difference is that conversion itself happened on CPU1.
 */
static void video_producer_poll(void)
{
    uint8_t *finished;

    if (!cpu1_job_done)
        return;

    asm volatile("dmb sy" ::: "memory");
    finished = (uint8_t *)(uintptr_t)cpu1_job_dst;
    cpu1_job_done = 0;
    producer_busy = 0;
    producer_frame_buf = 0;

    if (ready_frame_buf != 0) {
        /* Drop stale ready frame; newest completed frame wins. */
        producer_frame_buf = ready_frame_buf;
        ready_frame_buf = finished;
    } else {
        ready_frame_buf = finished;
        if (spare_frame_buf != 0) {
            producer_frame_buf = spare_frame_buf;
            spare_frame_buf = 0;
        }
    }

    producer_ready = (ready_frame_buf != 0);
    diag_done_total++;

    if (!ft_live_logged) {
        ft_live_logged = 1;
        kprintf("[USB-UVC] POC43 CPU1 I420 frame ready\n");
    }
}



static void diag_rect(uint8_t *dst, uint32_t x0, uint32_t y0,
                      uint32_t w, uint32_t h)
{
    if (x0 + w > UVC_WIDTH) w = UVC_WIDTH - x0;
    if (y0 + h > UVC_HEIGHT) h = UVC_HEIGHT - y0;
    uint8_t *py = dst;
    uint8_t *pu = dst + UVC_Y_BYTES;
    uint8_t *pv = pu + UVC_UV_BYTES;

    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            py[(uintptr_t)(y0 + y) * UVC_WIDTH + (x0 + x)] = 16;

    uint32_t cx0 = x0 >> 1, cy0 = y0 >> 1;
    uint32_t cw = (w + 1U) >> 1, ch = (h + 1U) >> 1;
    for (uint32_t y = 0; y < ch; ++y)
        for (uint32_t x = 0; x < cw; ++x) {
            uintptr_t p = (uintptr_t)(cy0 + y) * (UVC_WIDTH / 2U) + cx0 + x;
            pu[p] = 128;
            pv[p] = 128;
        }
}

/* 5x7 font: digits plus letters used by the compact HUD. */
static const uint8_t *hud_glyph(char c)
{
    static const uint8_t sp[7]={0,0,0,0,0,0,0};
    static const uint8_t d0[7]={14,17,19,21,25,17,14}, d1[7]={4,12,4,4,4,4,14};
    static const uint8_t d2[7]={14,17,1,2,4,8,31}, d3[7]={30,1,1,14,1,1,30};
    static const uint8_t d4[7]={2,6,10,18,31,2,2}, d5[7]={31,16,16,30,1,1,30};
    static const uint8_t d6[7]={14,16,16,30,17,17,14}, d7[7]={31,1,2,4,8,8,8};
    static const uint8_t d8[7]={14,17,17,14,17,17,14}, d9[7]={14,17,17,15,1,1,14};
    static const uint8_t A[7]={14,17,17,31,17,17,17}, D[7]={30,17,17,17,17,17,30};
    static const uint8_t E[7]={31,16,16,30,16,16,31}, F[7]={31,16,16,30,16,16,16};
    static const uint8_t G[7]={14,17,16,23,17,17,15}, I[7]={14,4,4,4,4,4,14};
    static const uint8_t L[7]={16,16,16,16,16,16,31}, M[7]={17,27,21,21,17,17,17};
    static const uint8_t O[7]={14,17,17,17,17,17,14}, P[7]={30,17,17,30,16,16,16};
    static const uint8_t R[7]={30,17,17,30,20,18,17}, S[7]={15,16,16,14,1,1,30};
    static const uint8_t T[7]={31,4,4,4,4,4,4}, W[7]={17,17,17,21,21,21,10};
    static const uint8_t X[7]={17,17,10,4,10,17,17}, C[7]={14,17,16,16,16,17,14};
    static const uint8_t V[7]={17,17,17,17,17,10,4}, H[7]={17,17,17,31,17,17,17};
    static const uint8_t N[7]={17,25,21,19,17,17,17}, K[7]={17,18,20,24,20,18,17};
    static const uint8_t B[7]={30,17,17,30,17,17,30}, U[7]={17,17,17,17,17,17,14};
    static const uint8_t Z[7]={31,1,2,4,8,16,31};
    static const uint8_t pct[7]={17,2,4,8,16,0,17};
    static const uint8_t dot[7]={0,0,0,0,0,12,12};
    static const uint8_t colon[7]={0,12,12,0,12,12,0};
    static const uint8_t dash[7]={0,0,0,31,0,0,0};
    switch(c) {
        case '0': return d0; case '1': return d1; case '2': return d2; case '3': return d3;
        case '4': return d4; case '5': return d5; case '6': return d6; case '7': return d7;
        case '8': return d8; case '9': return d9; case 'A': return A; case 'B': return B;
        case 'C': return C; case 'D': return D; case 'E': return E; case 'F': return F;
        case 'G': return G; case 'H': return H; case 'I': return I; case 'K': return K;
        case 'L': return L; case 'M': return M; case 'N': return N; case 'O': return O;
        case 'P': return P; case 'R': return R; case 'S': return S; case 'T': return T;
        case 'U': return U; case 'V': return V; case 'W': return W; case 'X': return X; case 'Z': return Z; case '%': return pct;
        case '.': return dot; case ':': return colon; case '-': return dash; default: return sp;
    }
}

static void hud_rect_black(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
    diag_rect(frame, x0, y0, w, h);
}

static void hud_pixel(uint32_t x, uint32_t y, uint8_t yy)
{
    if (x >= UVC_WIDTH || y >= UVC_HEIGHT) return;
    frame[(uintptr_t)y * UVC_WIDTH + x] = yy;
}

static void hud_text(uint32_t x, uint32_t y, const char *t)
{
    while (*t) {
        const uint8_t *g=hud_glyph(*t++);
        for (uint32_t gy=0; gy<7; ++gy)
            for (uint32_t gx=0; gx<5; ++gx)
                if (g[gy] & (1U << (4U-gx))) {
                    hud_pixel(x+gx*2U+0U,y+gy*2U+0U,235); hud_pixel(x+gx*2U+1U,y+gy*2U+0U,235);
                    hud_pixel(x+gx*2U+0U,y+gy*2U+1U,235); hud_pixel(x+gx*2U+1U,y+gy*2U+1U,235);
                }
        x += 12U;
    }
}

static char *hud_u32(char *p, uint32_t v)
{
    char tmp[10]; uint32_t n=0;
    do { tmp[n++]=(char)('0'+(v%10U)); v/=10U; } while(v && n<sizeof(tmp));
    while(n) *p++=tmp[--n];
    return p;
}


static void hud_line_u32(uint32_t x, uint32_t y, const char *label, uint32_t v, const char *suffix)
{
    char b[48], *p=b;
    while(*label) *p++=*label++;
    p=hud_u32(p,v);
    while(*suffix) *p++=*suffix++;
    *p=0; hud_text(x,y,b);
}


/*
 * POC41 FTCLK HUD.
 * Draw ONLY into a completed YUY2 producer buffer after it becomes the USB
 * buffer.  This never touches the live Unicam RGB565 DMA framebuffer.
 */
static void __attribute__((unused)) hud_render(uint64_t now)
{
    uint64_t elapsed = (diag_stream_start_tick && timer_freq) ? now-diag_stream_start_tick : 0;
    uint32_t expected = timer_freq ? (uint32_t)((elapsed*UVC_FPS)/timer_freq)+1U : diag_frames_done;
    uint32_t lost = expected>diag_frames_done ? expected-diag_frames_done : 0U;
    uint32_t drop10 = expected ? (lost*1000U)/expected : 0U;
    uint32_t fps10 = elapsed ? (uint32_t)(((uint64_t)diag_frames_done*10U*timer_freq)/elapsed) : 0U;
    uint32_t frame_us = timer_freq ? (uint32_t)((diag_last_frame_ticks*1000000ULL)/timer_freq) : 0U;
    uint32_t gap_us = timer_freq ? (uint32_t)((diag_last_max_txfe_gap_ticks*1000000ULL)/timer_freq) : 0U;
    char b[64], *p;

    hud_rect_black(0,0,470,112);
    hud_text(8,6,"UVC POC28 32K LIVE");

    p=b; *p++='F';*p++='P';*p++='S';*p++=' '; p=hud_u32(p,fps10/10U); *p++='.'; *p++=(char)('0'+fps10%10U); *p=0; hud_text(8,24,b);
    p=b; *p++='D';*p++='R';*p++='O';*p++='P';*p++=' '; p=hud_u32(p,drop10/10U); *p++='.'; *p++=(char)('0'+drop10%10U); *p++='%'; *p=0; hud_text(8,42,b);
    hud_line_u32(8,60,"XFRC/F ",diag_last_xfrc_frame,"");
    hud_line_u32(8,78,"TXFE/F ",diag_last_txfe_frame,"");
    p=b; *p++='F';*p++='R';*p++='A';*p++='M';*p++='E';*p++=' '; p=hud_u32(p,frame_us/1000U); *p++='.'; p=hud_u32(p,(frame_us%1000U)/100U); *p++='M';*p++='S';*p=0; hud_text(220,24,b);
    hud_line_u32(220,42,"WAIT ",diag_last_fifo_wait_frame,"");
    p=b; *p++='G';*p++='A';*p++='P';*p++=' '; p=hud_u32(p,gap_us); *p++='U';*p++='S';*p=0; hud_text(220,60,b);
    hud_line_u32(220,78,"DONE ",diag_frames_done,"");
}

static void configure_video_endpoint(void)
{
    uint32_t ctl = USB_DXEPCTL_MPS(usb_bulk_mps) |
                   USB_DXEPCTL_USBACTEP |
                   USB_DXEPCTL_EPTYPE_BULK |
                   USB_DXEPCTL_TXFNUM(UVC_EP_VIDEO_IN) |
                   USB_DXEPCTL_SETD0PID;
    wr(USB_DIEPCTL(UVC_EP_VIDEO_IN), ctl);
    usb_ep1_busy = 0;
}


/* ------------------------------------------------------------------------- */
/* AV POC3: CACHE-FIX2 Paula producer + UAC2 High-Speed isochronous EP2.    */
/* ------------------------------------------------------------------------- */

static inline uint64_t read_cntvct(void)
{
    uint64_t v;
    asm volatile("mrs %0, CNTVCT_EL0" : "=r"(v));
    return v;
}

static inline uint64_t read_cntfrq(void)
{
    uint64_t v;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(v));
    return v;
}

static int16_t float_to_s16(float sample)
{
    if (sample > 1.0f) sample = 1.0f;
    if (sample < -1.0f) sample = -1.0f;
    if (sample >= 0.0f)
        return (int16_t)(sample * 32767.0f);
    return (int16_t)(sample * 32768.0f);
}

static uint32_t audio_ring_count(void)
{
    const uint32_t wrp = __atomic_load_n(&audio_wr, __ATOMIC_ACQUIRE);
    const uint32_t rdp = __atomic_load_n(&audio_rd, __ATOMIC_ACQUIRE);
    const uint32_t n = wrp - rdp;
    return n > USB_AUDIO_RING_FRAMES ? USB_AUDIO_RING_FRAMES : n;
}

static void audio_ring_push(int16_t left, int16_t right)
{
    /* Paula runs continuously even while the host has AS alt0 selected.
     * Old history may be overwritten; ALT1 always re-aligns the reader to a
     * bounded recent pre-roll instead of replaying stale audio. */
    const uint32_t wrp = __atomic_load_n(&audio_wr, __ATOMIC_RELAXED);
    audio_ring[wrp & USB_AUDIO_RING_MASK].left = left;
    audio_ring[wrp & USB_AUDIO_RING_MASK].right = right;
    __atomic_store_n(&audio_wr, wrp + 1U, __ATOMIC_RELEASE);
}

static unsigned audio_ring_pop_packet(uint8_t *dst)
{
    const uint32_t fill = audio_ring_count();
    unsigned wanted = USB_AUDIO_FRAMES_PER_UFRAME;

    /* CPU2 and the USB HS microframe clock are independent.  UAC2 EP2 is
     * asynchronous and advertises room for seven stereo frames, so occasional
     * 5/7-frame packets keep the ring close to its target without resampling. */
    if (fill > USB_AUDIO_TARGET_FILL + 96U)
        wanted = USB_AUDIO_MAX_FRAMES_UFRAME;
    else if (fill < USB_AUDIO_TARGET_FILL - 96U &&
             fill >= USB_AUDIO_MIN_FRAMES_UFRAME)
        wanted = USB_AUDIO_MIN_FRAMES_UFRAME;

    uint32_t rdp = __atomic_load_n(&audio_rd, __ATOMIC_RELAXED);
    const uint32_t wrp = __atomic_load_n(&audio_wr, __ATOMIC_ACQUIRE);
    uint32_t available = wrp - rdp;

    if (available > USB_AUDIO_RING_FRAMES) {
        rdp = wrp - USB_AUDIO_RING_FRAMES;
        available = USB_AUDIO_RING_FRAMES;
        __atomic_add_fetch(&usb_audio_overruns, 1U, __ATOMIC_RELAXED);
    }

    const unsigned copied = available < wanted ? (unsigned)available : wanted;
    for (unsigned i = 0; i < wanted; ++i) {
        int16_t l = 0, r = 0;
        if (i < copied) {
            const usb_audio_frame f = audio_ring[(rdp + i) & USB_AUDIO_RING_MASK];
            l = f.left;
            r = f.right;
        }
        const unsigned o = i * 4U;
        dst[o + 0U] = (uint8_t)l;
        dst[o + 1U] = (uint8_t)((uint16_t)l >> 8);
        dst[o + 2U] = (uint8_t)r;
        dst[o + 3U] = (uint8_t)((uint16_t)r >> 8);
    }

    if (copied)
        __atomic_store_n(&audio_rd, rdp + copied, __ATOMIC_RELEASE);
    if (copied < wanted)
        __atomic_add_fetch(&usb_audio_underruns, 1U, __ATOMIC_RELAXED);

    return wanted * USB_AUDIO_CHANNELS * USB_AUDIO_BYTES_PER_SAMPLE;
}

static void audio_stream_stop(void)
{
    /* Keep EP2 completely dormant until AS alt1 is actually opened. */
    if (!usb_audio_hw_active) {
        usb_ep2_busy = 0;
        return;
    }

    uint32_t ctl = rd(USB_DIEPCTL(USB_AUDIO_EP_IN));

    if (ctl & USB_DXEPCTL_EPENA) {
        wr(USB_DIEPINT(USB_AUDIO_EP_IN), USB_DXEPINT_EPDISBLD);
        wr(USB_DIEPCTL(USB_AUDIO_EP_IN),
           ctl | USB_DXEPCTL_SNAK | USB_DXEPCTL_EPDIS);
        (void)wait_mask(USB_DIEPINT(USB_AUDIO_EP_IN),
                        USB_DXEPINT_EPDISBLD,
                        USB_DXEPINT_EPDISBLD, 1000000U);
    } else if (ctl & USB_DXEPCTL_USBACTEP) {
        wr(USB_DIEPCTL(USB_AUDIO_EP_IN), ctl | USB_DXEPCTL_SNAK);
    }

    wr(USB_DIEPEMPMSK,
       rd(USB_DIEPEMPMSK) & ~(1U << USB_AUDIO_EP_IN));
    wr(USB_DIEPINT(USB_AUDIO_EP_IN), 0xffffffffU);
    wr(USB_DIEPTSIZ(USB_AUDIO_EP_IN), 0U);
    flush_audio_tx_fifo();
    wr(USB_DAINTMSK, rd(USB_DAINTMSK) & ~USB_DAINT_INEP(USB_AUDIO_EP_IN));
    usb_ep2_busy = 0;
    usb_audio_hw_active = 0;
}

static void audio_stream_start(void)
{
    audio_stream_stop();

    /* Capture starts from a small recent history.  This preserves the
     * continuous Paula timeline while avoiding seconds of stale ring data. */
    const uint32_t wrp = __atomic_load_n(&audio_wr, __ATOMIC_ACQUIRE);
    const uint32_t preroll = wrp < USB_AUDIO_TARGET_FILL ? wrp : USB_AUDIO_TARGET_FILL;
    __atomic_store_n(&audio_rd, wrp - preroll, __ATOMIC_RELEASE);

    wr(USB_DIEPINT(USB_AUDIO_EP_IN), 0xffffffffU);
    wr(USB_DIEPTSIZ(USB_AUDIO_EP_IN), 0U);

    const uint32_t ctl = USB_DXEPCTL_MPS(USB_AUDIO_MAX_PACKET) |
                         USB_DXEPCTL_USBACTEP |
                         USB_DXEPCTL_EPTYPE_ISOC |
                         USB_DXEPCTL_TXFNUM(USB_AUDIO_EP_IN) |
                         USB_DXEPCTL_SEVNFRM;
    wr(USB_DIEPCTL(USB_AUDIO_EP_IN), ctl);
    usb_ep2_busy = 0;
    usb_audio_hw_active = 1U;
    wr(USB_DAINTMSK, rd(USB_DAINTMSK) | USB_DAINT_INEP(USB_AUDIO_EP_IN));
}

static int start_audio_in(void)
{
    if (!usb_configured || usb_audio_alt != 1U || usb_ep2_busy)
        return 0;

    const unsigned len = audio_ring_pop_packet(audio_tx);
    uint32_t ctl = rd(USB_DIEPCTL(USB_AUDIO_EP_IN));
    const uint32_t current_frame =
        (rd(USB_DSTS) >> USB_DSTS_SOFFN_SHIFT) & 1U;

    usb_ep2_busy = 1;
    wr(USB_DIEPINT(USB_AUDIO_EP_IN), 0xffffffffU);
    wr(USB_DIEPTSIZ(USB_AUDIO_EP_IN),
       USB_DXEPTSIZ_MC(1) |
       USB_DXEPTSIZ_PKTCNT(1) |
       USB_DXEPTSIZ_XFERSIZE(len));

    ctl &= ~(USB_DXEPCTL_SEVNFRM | USB_DXEPCTL_SODDFRM);
    ctl |= current_frame ? USB_DXEPCTL_SEVNFRM : USB_DXEPCTL_SODDFRM;
    wr(USB_DIEPCTL(USB_AUDIO_EP_IN),
       ctl | USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
    fifo_write(USB_AUDIO_EP_IN, audio_tx, len);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Hardware-validated Paula CACHE-FIX2 producer, moved from CPU1 to CPU2.    */
/* CPU0 remains the sole physical Chip RAM cache reader.                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t loc[4];
    uint16_t len[4];
    uint16_t dma;
} poc_live_state;

static void poc_prefetch_channel(const poc_live_state *state, unsigned ch)
{
    const uint32_t bytes = (state->len[ch] ? state->len[ch] : 65536u) * 2u;
    paula_mirror_prefetch(state->loc[ch], bytes < 1024u ? bytes : 1024u);
}

static void poc_apply_live_event(poc_live_state *state,
                                 const paula_mirror_event *event)
{
    if (event->width != 1u && event->width != 2u) {
        paula_probe_add(PP_IGNORED,1);
        return;
    }
    if (event->width == 2u && (event->address & 1u)) {
        paula_probe_add(PP_IGNORED,1);
        return;
    }

    const uint32_t r = event->address & 0xffeu;
    const uint16_t value = event->width == 1u
        ? (uint16_t)((event->value & 255u) * 0x0101u)
        : event->value;

    if (r >= 0xa0u && r <= 0xdau) {
        const unsigned ch = (r - 0xa0u) >> 4;
        const unsigned reg = (r - 0xa0u) & 0x0fu;
        if (ch < 4u) {
            if (reg == 0u)
                state->loc[ch] = (state->loc[ch] & 0xffffu) |
                                 ((uint32_t)value << 16);
            else if (reg == 2u) {
                state->loc[ch] = (state->loc[ch] & 0xffff0000u) | value;
                if (state->len[ch])
                    poc_prefetch_channel(state, ch);
            } else if (reg == 4u) {
                state->len[ch] = value;
                poc_prefetch_channel(state, ch);
            }
        }
    } else if (r == 0x096u) {
        const uint16_t before = state->dma;
        if (value & 0x8000u)
            state->dma |= value & 0x7fffu;
        else
            state->dma &= (uint16_t)~(value & 0x7fffu);
        if (state->dma & 0x0200u) {
            for (unsigned ch = 0; ch < 4u; ++ch) {
                const uint16_t mask = (uint16_t)(0x0200u | (1u << ch));
                if ((state->dma & mask) == mask && (before & mask) != mask)
                    poc_prefetch_channel(state, ch);
            }
        }
    }

    paula_live_write(0xdff000u | r, value);
    if (r == 0x096u || (r >= 0x0a0u && r <= 0x0dau &&
        ((r - 0x0a0u) & 15u) <= 4u))
        paula_live_prefetch();
}

static int poc_restore_live_state(poc_live_state *state,
                                  paula_mirror_snapshot *snapshot)
{
    if (!paula_mirror_snapshot_take(snapshot))
        return 0;

    paula_probe_flag(PP_FLAG_CAPTURE_FAILED,1);
    paula_live_reset();
    for (unsigned ch = 0; ch < 4u; ++ch) {
        state->loc[ch] = 0;
        state->len[ch] = 0;
    }
    state->dma = 0;

    for (unsigned ch = 0; ch < 4u; ++ch) {
        for (unsigned reg = 0; reg < 5u; ++reg) {
            if (!(snapshot->seen[ch] & (1u << reg)))
                continue;
            paula_mirror_event event;
            event.address = 0xdff0a0u + ch * 16u + reg * 2u;
            event.value = snapshot->reg[ch][reg];
            event.width = 2u;
            event.reserved = 0;
            event.ticks = snapshot->ticks;
            poc_apply_live_event(state, &event);
        }
    }
    if (snapshot->seen_global & 2u)
        paula_live_write(0xdff09au, (uint16_t)(0x8000u | snapshot->intena));
    if (snapshot->seen_global & 4u)
        paula_live_write(0xdff09cu, (uint16_t)(0x8000u | snapshot->intreq));
    if (snapshot->seen_global & 8u)
        paula_live_write(0xdff09eu, (uint16_t)(0x8000u | snapshot->adkcon));
    if (snapshot->seen_global & 1u) {
        paula_mirror_event event;
        event.address = 0xdff096u;
        event.value = (uint16_t)(0x8000u | snapshot->dma);
        event.width = 2u;
        event.reserved = 0;
        event.ticks = snapshot->ticks;
        poc_apply_live_event(state, &event);
    }

    paula_probe_add(PP_RESYNCS,1u);
    paula_probe_add(PP_RESYNC_SKIPPED,snapshot->skipped);
    paula_probe_flag(PP_FLAG_CAPTURE_FAILED,0);
    return 1;
}

extern void ps_housekeeper_cooperative_init(void);
extern void ps_housekeeper_cooperative_poll(void);

void emu68_av_cpu2_worker(void)
{
    while (!__atomic_load_n(&av_cpu2_enabled, __ATOMIC_ACQUIRE))
        asm volatile("wfe");

    ps_housekeeper_cooperative_init();
    paula_probe_init();
    paula_probe_set(PP_SAMPLE_RATE, USB_AUDIO_RATE);
    paula_probe_set(PP_COUNTER_FREQ, (uint32_t)read_cntfrq());
    paula_live_init();
    paula_mirror_enable();
    paula_probe_flag(PP_FLAG_RENDERING, 1);
    kprintf("[USB-AV] CPU2 Paula CACHE-FIX2 producer active, 48 kHz stereo\n");

    poc_live_state state;
    for (unsigned ch = 0; ch < 4u; ++ch) {
        state.loc[ch] = 0;
        state.len[ch] = 0;
    }
    state.dma = 0;

    paula_mirror_event event;
    paula_mirror_snapshot snapshot;
    uint32_t frames_written = 0;
    uint32_t last_drop = paula_mirror_dropped();
    unsigned recovery_pending = 0;
    uint64_t last_report = read_cntvct();
    const uint64_t frequency = read_cntfrq();
    const uint64_t latency = frequency / 20u; /* preserve validated 50 ms replay latency */
    const uint64_t max_lag = frequency / 4u;
    paula_replay_clock clock;
    paula_replay_start(&clock, last_report, (uint32_t)frequency, latency);

    for (;;) {
        ps_housekeeper_cooperative_poll();
        paula_live_mixer_apply();
        uint64_t now = read_cntvct();
        const uint32_t drops = paula_mirror_dropped();

        if (drops != last_drop ||
            paula_mirror_pending() >= PAULA_MIRROR_EVENT_CAPACITY * 3u / 4u ||
            paula_replay_late(&clock, now, latency, max_lag))
            recovery_pending = 1;

        if (recovery_pending) {
            paula_probe_flag(PP_FLAG_CAPTURE_FAILED,1);
            if (poc_restore_live_state(&state, &snapshot)) {
                last_drop = snapshot.dropped;
                paula_replay_start(&clock, read_cntvct(),
                                   (uint32_t)frequency, latency);
                recovery_pending = 0;
            } else {
                paula_probe_add(PP_SNAPSHOT_RETRIES,1u);
            }
        }
        const unsigned recovering = recovery_pending;

        while (!recovering &&
               !paula_replay_due(read_cntvct(), clock.tick + latency)) {
            /* CPU2 inherited the stock housekeeper timer event stream.  Sleep
             * until the next event instead of burning the core; every wake also
             * services IPL/reset.  The event cadence is far finer than one
             * 48 kHz audio frame, so Paula timing remains bounded. */
            asm volatile("wfe");
            ps_housekeeper_cooperative_poll();
        }

        unsigned processed = 0;
        while (!recovering && processed < 128u && paula_mirror_peek(&event)) {
            if (!paula_replay_due(clock.tick, event.ticks))
                break;
            poc_apply_live_event(&state, &event);
            paula_mirror_pop();
            ++processed;
        }

        unsigned backlog = 0;
        if (!recovering && paula_mirror_peek(&event) &&
            paula_replay_due(clock.tick, event.ticks))
            backlog = 1;

        float left = 0.0f, right = 0.0f;
        if (!recovering && !backlog)
            paula_live_render(&left, &right, 1u);
        else
            paula_probe_add(PP_REPLAY_SILENT_FRAMES,1u);

        const int16_t sl = float_to_s16(left);
        const int16_t sr = float_to_s16(right);
        audio_ring_push(sl, sr);

        if (sl || sr)
            paula_probe_add(PP_NONZERO_FRAMES,1u);
        const uint32_t abs_l = (uint32_t)(sl < 0 ? -(int32_t)sl : sl);
        const uint32_t abs_r = (uint32_t)(sr < 0 ? -(int32_t)sr : sr);
        if (abs_l > paula_probe_get(PP_PEAK_LEFT))
            paula_probe_set(PP_PEAK_LEFT, abs_l);
        if (abs_r > paula_probe_get(PP_PEAK_RIGHT))
            paula_probe_set(PP_PEAK_RIGHT, abs_r);

        ++frames_written;
        paula_probe_set(PP_PCM_FRAMES, frames_written);
        paula_replay_advance(&clock);

        now = read_cntvct();
        if (now - last_report >= frequency) {
            const uint64_t target = clock.tick + latency;
            const uint64_t lag = paula_replay_due(now, target) ? now - target : 0;
            paula_probe_set(PP_REPLAY_LAG_US,
                (uint32_t)((lag * 1000000u) / frequency));
            kprintf("[USB-AV] Paula PCM=%u ring=%u pkt=%u under=%u over=%u miss=%u drop=%u\n",
                    frames_written, audio_ring_count(),
                    __atomic_load_n(&usb_audio_packets, __ATOMIC_RELAXED),
                    __atomic_load_n(&usb_audio_underruns, __ATOMIC_RELAXED),
                    __atomic_load_n(&usb_audio_overruns, __ATOMIC_RELAXED),
                    paula_mirror_misses(), paula_mirror_dropped());
            last_report = now;
        }
    }
}


static void uac2_put_clock_cur(uint8_t out[4])
{
    uvc_put_le32(out, USB_AUDIO_RATE);
}

static void uac2_put_clock_range(uint8_t out[14])
{
    /* One discrete subrange: MIN=MAX=48000, RES=0. */
    uvc_put_le16(out + 0, 1U);
    uvc_put_le32(out + 2, USB_AUDIO_RATE);
    uvc_put_le32(out + 6, USB_AUDIO_RATE);
    uvc_put_le32(out + 10, 0U);
}

static void video_tx_fill_fifo(void)
{
    while (video_tx_written < video_tx_total) {
        uint32_t remain = video_tx_total - video_tx_written;
        uint32_t packet = remain < usb_bulk_mps ? remain : usb_bulk_mps;
        uint32_t avail_bytes =
            (rd(USB_DTXFSTS(UVC_EP_VIDEO_IN)) & 0xffffU) << 2;

        /*
         * TinyUSB DWC2 slave mode only writes complete USB packets into the
         * hardware FIFO.  If the next complete packet does not fit, wait for
         * TXFE and continue later.
         */
        if (packet > avail_bytes) {
            diag_fifo_wait_total++;
            diag_fifo_wait_frame++;
            break;
        }

        fifo_write(UVC_EP_VIDEO_IN, video_tx_buf + video_tx_written, packet);
        video_tx_written += packet;
    }

    if (video_tx_written < video_tx_total) {
        /* Equivalent to TinyUSB's dwc2->diepempmsk |= (1 << epnum). */
        wr(USB_DIEPEMPMSK,
           rd(USB_DIEPEMPMSK) | (1U << UVC_EP_VIDEO_IN));
    } else {
        wr(USB_DIEPEMPMSK,
           rd(USB_DIEPEMPMSK) & ~(1U << UVC_EP_VIDEO_IN));
    }
}

static int start_video_in(const uint8_t *buf, uint32_t len)
{
    uint32_t packets;

    if (!usb_configured || usb_ep1_busy || !len)
        return 0;
    if (len > UVC_TINYUSB_PAYLOAD_MAX)
        len = UVC_TINYUSB_PAYLOAD_MAX;

    packets = (len + usb_bulk_mps - 1U) / usb_bulk_mps;
    if (!packets)
        packets = 1;

    uvc_screen_line(UVC_S_TX_START, "UVC: TX START");
    usb_ep1_busy = 1;
    video_tx_buf = buf;
    video_tx_total = len;
    video_tx_written = 0;

    wr(USB_DIEPINT(UVC_EP_VIDEO_IN), 0xffffffffU);

    /*
     * TinyUSB dcd_dwc2.c / edpt_schedule_packets():
     * program the complete logical transfer, not one packet at a time.
     */
    wr(USB_DIEPTSIZ(UVC_EP_VIDEO_IN),
       USB_DXEPTSIZ_PKTCNT(packets) | USB_DXEPTSIZ_XFERSIZE(len));

    wr(USB_DIEPCTL(UVC_EP_VIDEO_IN),
       rd(USB_DIEPCTL(UVC_EP_VIDEO_IN)) |
       USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);

    /*
     * Current TinyUSB DWC2 slave mode writes the initial packet(s) directly
     * to the FIFO and enables TXFE only if more data remains.
     */
    video_tx_fill_fifo();
    return 1;
}

static int video_start_next_payload(void)
{
    uint32_t payload_max, remain, payload;
    uint8_t eof;

    if (!video_frame_busy || usb_ep1_busy || !usb_configured ||
        !usb_stream_committed)
        return 0;

    payload_max = uvc_payload_size > 2U ? uvc_payload_size - 2U : 0U;
    if (!payload_max)
        return 0;

    remain = UVC_FRAME_BYTES - video_frame_pos;
    payload = remain < payload_max ? remain : payload_max;
    eof = (payload == remain) ? 1U : 0U;

    tx_stage[0] = 2;
    tx_stage[1] = 0x80U |
                  (video_frame_fid & 1U) |
                  (eof ? 0x02U : 0U); /* EOH/FID/EOF */

    for (uint32_t i = 0; i < payload; ++i)
        tx_stage[2U + i] = video_frame_buf[video_frame_pos + i];

    if (!start_video_in(tx_stage, payload + 2U))
        return 0;

    video_frame_pos += payload;
    return 1;
}

static void video_frame_complete(void)
{
    uint64_t now;
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(now));
    diag_frames_done++;
    if (diag_frame_start_tick)
        diag_last_frame_ticks = now - diag_frame_start_tick;
    diag_last_xfrc_frame = diag_xfrc_frame;
    diag_last_txfe_frame = diag_txfe_frame;
    diag_last_fifo_wait_frame = diag_fifo_wait_frame;
    diag_last_max_txfe_gap_ticks = diag_max_txfe_gap_ticks;

    /*
     * POC19: absolute cadence.
     * Advance the existing deadline by exactly one frame period instead of
     * scheduling relative to completion time.  This prevents USB transfer
     * duration from accumulating into the video cadence.
     */
    video_frame_busy = 0;
    video_frame_pos = 0;
    video_frame_fid ^= 1U;

    next_frame_tick += (timer_freq / UVC_FPS);
}

static void kick_video(void)
{
    uint64_t now;

    if (!usb_stream_committed || !usb_configured)
        return;

    /* A waiting ready frame no longer blocks capture into the third buffer. */
    if (!producer_busy && producer_frame_buf != 0)
        (void)video_producer_begin();

    if (video_frame_busy)
        return;

    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(now));

    /*
     * POC41C: PI0 clocks capture/production, but USB publication remains paced
     * at the negotiated 25 fps.  These are separate responsibilities.
     */
    if (next_frame_tick != 0 && (int64_t)(now - next_frame_tick) < 0)
        return;

    /*
     * Publish a newly completed producer buffer only at a frame boundary.
     * The old USB buffer immediately becomes the next producer buffer.
     * If the producer missed the deadline (or FT was transiently unavailable),
     * simply resend the last complete frame; never expose a half-written frame.
     */
    if (ready_frame_buf != 0) {
        uint8_t *old_video = video_frame_buf;

        video_frame_buf = ready_frame_buf;
        ready_frame_buf = 0;
        producer_ready = 0;
        ft_frame_valid = 1;
        diag_pub_total++;

        if (producer_frame_buf == 0)
            producer_frame_buf = old_video;
        else
            spare_frame_buf = old_video;

        asm volatile("dmb sy" ::: "memory");

        (void)video_producer_begin();
    }

    if (next_frame_tick == 0)
        next_frame_tick = now;

    if (!diag_stream_start_tick)
        diag_stream_start_tick = now;
    diag_frame_start_tick = now;
    diag_xfrc_frame = 0;
    diag_txfe_frame = 0;
    diag_fifo_wait_frame = 0;
    diag_max_txfe_gap_ticks = 0;
    diag_last_txfe_tick = 0;

    video_frame_busy = 1;
    video_frame_pos = 0;
    (void)video_start_next_payload();
}

static void on_reset(void)
{
    uvc_screen_line(UVC_S_USBRST, "UVC: USBRST");
    usb_configured = 0;
    usb_stream_committed = 0;
    usb_ep1_busy = 0;
    usb_audio_alt = 0;
    usb_audio_hw_active = 0;
    usb_audio_ep_action_pending = 0;
    usb_ep2_busy = 0;
    usb_audio_packets = 0;
    usb_audio_xfr_errors = 0;
    usb_audio_underruns = 0;
    usb_audio_overruns = 0;
    __atomic_store_n(&audio_rd, __atomic_load_n(&audio_wr, __ATOMIC_ACQUIRE),
                     __ATOMIC_RELEASE);
    video_tx_buf = 0;
    video_tx_total = 0;
    video_tx_written = 0;
    wr(USB_DIEPEMPMSK, 0);
    usb_ep0_out_kind = 0;
    uvc_payload_size = usb_bulk_mps;
    ep0_state = EP0_IDLE;
    ep0_tx_len = 0;
    ep0_tx_pos = 0;
    video_frame_busy = 0;
    video_frame_pos = 0;
    video_frame_fid = 0;
    cpu1_producer_quiesce();
    producer_busy = 0;
    producer_ready = 0;
    producer_src_start = 0;
    video_frame_buf = frame;
    producer_frame_buf = frame_next;
    ready_frame_buf = 0;
    spare_frame_buf = frame_third;
    diag_stream_start_tick = 0;
    diag_frame_start_tick = 0;
    diag_last_hud_tick = 0;
    diag_last_txfe_tick = 0;
    diag_max_txfe_gap_ticks = 0;
    diag_last_frame_ticks = 0;
    diag_frames_done = 0;
    diag_xfrc_total = 0;
    diag_txfe_total = 0;
    diag_fifo_wait_total = 0;
    diag_xfrc_frame = 0;
    diag_txfe_frame = 0;
    diag_fifo_wait_frame = 0;
    diag_last_xfrc_frame = 0;
    diag_last_txfe_frame = 0;
    diag_last_fifo_wait_frame = 0;
    diag_last_max_txfe_gap_ticks = 0;
    set_address_now(0);
    flush_fifos();
    wr(USB_DIEPINT(0), 0xffffffffU);
    wr(USB_DOEPINT(0), 0xffffffffU);
    wr(USB_DIEPINT(UVC_EP_VIDEO_IN), 0xffffffffU);
    wr(USB_DAINTMSK,
       USB_DAINT_INEP(0) |
       USB_DAINT_INEP(UVC_EP_VIDEO_IN) |
       USB_DAINT_OUTEP(0));
    stream_ctrl_defaults(probe_ctrl);
    stream_ctrl_defaults(commit_ctrl);
    ep0_arm_setup();
}

static void on_enum_done(void)
{
    uvc_screen_line(UVC_S_ENUMDONE, "UVC: ENUMDONE");
    uint32_t spd = rd(USB_DSTS) & USB_DSTS_ENUMSPD_MASK;
    usb_bulk_mps = (spd == USB_DSTS_ENUMSPD_HS) ? 512U : 64U;
    uvc_payload_size = (uint16_t)tinyusb_payload_for_interval(UVC_INTERVAL_100NS);
    stream_ctrl_defaults(probe_ctrl);
    stream_ctrl_defaults(commit_ctrl);
    ep0_arm_setup();
}

static void handle_setup(const struct usb_setup_packet *r)
{
    const uint8_t *data = 0;
    uint16_t len = 0;
    uint16_t wValue = r->wValue;
    uint16_t wLength = r->wLength;
    uint8_t tmp[384];
    static const uint8_t zero2[2] = {0,0};
    static const uint8_t zero1[1] = {0};
    static const uint8_t info1[1] = {3}; /* GET/SET supported */
    static const uint8_t len48[2] = {48,0};

    if ((r->bmRequestType & 0x80U) && r->bRequest == USB_REQ_GET_DESCRIPTOR) {
        uint8_t type = (uint8_t)(wValue >> 8);
        uint8_t idx = (uint8_t)wValue;
        if (type == USB_DT_DEVICE) {
            uvc_screen_line(UVC_S_GETDEV, "UVC: GET DEVICE");
            data = device_desc; len = sizeof(device_desc);
        } else if (type == USB_DT_DEVICE_QUALIFIER) {
            data = qualifier_desc; len = sizeof(qualifier_desc);
        } else if (type == USB_DT_CONFIG) {
            uvc_screen_line(UVC_S_GETCFG, "UVC: GET CONFIG");
            for (uint32_t i = 0; i < sizeof(config_desc); ++i)
                tmp[i] = config_desc[i];
            /* EP1 is no longer the final descriptor; locate it explicitly. */
            for (uint32_t o = 9U; o + 6U < sizeof(config_desc); ) {
                const uint8_t dlen = tmp[o];
                if (dlen < 2U || o + dlen > sizeof(config_desc))
                    break;
                if (tmp[o + 1U] == 5U && tmp[o + 2U] == 0x81U) {
                    tmp[o + 4U] = (uint8_t)usb_bulk_mps;
                    tmp[o + 5U] = (uint8_t)(usb_bulk_mps >> 8);
                    break;
                }
                o += dlen;
            }
            data = tmp; len = sizeof(config_desc);
        } else if (type == USB_DT_STRING) {
            uvc_screen_line(UVC_S_GETSTR, "UVC: GET STRING");
            if (idx == 0) { data = str0; len = sizeof(str0); }
            else if (idx == 1) { data = str1; len = sizeof(str1); }
            else if (idx == 2) { data = str2; len = sizeof(str2); }
            else if (idx == 3) { data = str3; len = sizeof(str3); }
        }
        if (!data) { ep0_stall(); return; }
        if (len > wLength) len = wLength;
        ep0_state = EP0_IN_DATA;
        ep0_send(data, len);
        return;
    }

    if (r->bmRequestType == 0x00 && r->bRequest == USB_REQ_SET_ADDRESS) {
        uvc_screen_line(UVC_S_SETADDR, "UVC: SET ADDRESS");
        set_address_now((uint8_t)(wValue & 0x7fU));
        ep0_state = EP0_IN_STATUS;
        ep0_zlp();
        return;
    }

    if (r->bmRequestType == 0x00 && r->bRequest == USB_REQ_SET_CONFIGURATION) {
        if (wValue)
            uvc_screen_line(UVC_S_SETCFG, "UVC: SET CONFIG");
        usb_audio_alt = 0;
        if (usb_audio_hw_active)
            audio_stream_stop();
        usb_configured = (uint8_t)(wValue ? 1U : 0U);
        usb_stream_committed = 0;
        if (usb_configured)
            configure_video_endpoint();
        ep0_state = EP0_IN_STATUS;
        ep0_zlp();
        return;
    }

    if (r->bmRequestType == 0x80 && r->bRequest == USB_REQ_GET_CONFIGURATION) {
        tmp[0] = usb_configured ? 1U : 0U;
        ep0_state = EP0_IN_DATA;
        ep0_send(tmp, wLength < 1U ? wLength : 1U);
        return;
    }

    if ((r->bmRequestType & 0x7fU) == 0x01 &&
        r->bRequest == USB_REQ_SET_INTERFACE) {
        const uint8_t intf = (uint8_t)r->wIndex;
        const uint8_t alt = (uint8_t)wValue;

        /* UVC VC/VS and UAC AudioControl expose only alt setting zero. */
        if ((intf == 0U || intf == 1U || intf == UAC_IF_CONTROL) &&
            alt == 0U) {
            if (intf == 1U)
                uvc_screen_line(UVC_S_UVC_IF0, "UVC: SET IF ALT0");
            ep0_state = EP0_IN_STATUS;
            ep0_zlp();
            return;
        }

        /*
         * UAC2 AudioStreaming: ACK SET_INTERFACE on EP0 first.  Only after
         * the status ZLP completes do we touch periodic EP2.  Starting EP2
         * before the control transfer has completed can perturb the shared
         * DWC2 and makes Linux snd-usb-audio fail snd_pcm_hw_params.
         */
        if (intf == UAC_IF_STREAMING && (alt == 0U || alt == 1U) &&
            usb_configured) {
            usb_audio_alt = alt;
            usb_audio_ep_action_pending = (alt == 1U) ? 1U : 2U;
            if (alt == 1U)
                uvc_screen_line(UVC_S_AUDIO_ALT1, "UAC2: ALT1 ACK");
            else
                uvc_screen_line(UVC_S_AUDIO_ALT0, "UAC2: ALT0 ACK");
            ep0_state = EP0_IN_STATUS;
            ep0_zlp();
            return;
        }
    }

    if ((r->bmRequestType & 0x7fU) == 0x01 &&
        r->bRequest == USB_REQ_GET_INTERFACE) {
        const uint8_t intf = (uint8_t)r->wIndex;
        if (intf <= UAC_IF_STREAMING) {
            tmp[0] = (intf == UAC_IF_STREAMING) ? usb_audio_alt : 0U;
            ep0_state = EP0_IN_DATA;
            ep0_send(tmp, wLength < 1U ? wLength : 1U);
            return;
        }
    }

    if ((r->bmRequestType & 0x7fU) == 0x00 && r->bRequest == USB_REQ_GET_STATUS) {
        ep0_state = EP0_IN_DATA;
        ep0_send(zero2, wLength < 2U ? wLength : 2U);
        return;
    }

    if ((r->bmRequestType & 0x7fU) == 0x02 &&
        r->bRequest == USB_REQ_CLEAR_FEATURE && wValue == 0) {
        ep0_state = EP0_IN_STATUS;
        ep0_zlp();
        return;
    }

    /* UAC2 Clock Source ID 4, fixed internal 48 kHz.
     * wIndex: high byte Entity ID, low byte AudioControl interface.
     * wValue: high byte Control Selector (SAM_FREQ), low byte channel 0.
     * The clock is read-only, so there is deliberately no SET_CUR path. */
    if (r->bmRequestType == 0xa1 &&
        (r->wIndex & 0xffU) == UAC_IF_CONTROL &&
        ((r->wIndex >> 8) & 0xffU) == UAC2_CLOCK_ID &&
        ((wValue >> 8) & 0xffU) == UAC2_CS_SAM_FREQ &&
        (wValue & 0xffU) == 0U) {
        if (r->bRequest == UAC2_REQ_CUR) {
            uac2_put_clock_cur(tmp);
            uvc_screen_line(UVC_S_AUDIO_RATE, "UAC2: CLK CUR 48K");
            ep0_state = EP0_IN_DATA;
            ep0_send(tmp, wLength < 4U ? wLength : 4U);
            return;
        }
        if (r->bRequest == UAC2_REQ_RANGE) {
            uac2_put_clock_range(tmp);
            uvc_screen_line(UVC_S_AUDIO_RATE, "UAC2: CLK RANGE 48K");
            ep0_state = EP0_IN_DATA;
            ep0_send(tmp, wLength < 14U ? wLength : 14U);
            return;
        }
    }

    /*
     * UVC VideoStreaming Probe/Commit controls on interface 1.
     *
     * Modelled on TinyUSB src/class/video/video_device.c:
     * SET_CUR receives a persistent Probe/Commit object; after the OUT data
     * stage we normalize it against our single advertised format/frame.
     * GET_MIN/MAX/RES/DEF are independently negotiated values.
     */
    if ((r->wIndex & 0xffU) == 1U &&
        ((wValue >> 8) == UVC_VS_PROBE_CONTROL ||
         (wValue >> 8) == UVC_VS_COMMIT_CONTROL)) {
        uint8_t selector = (uint8_t)(wValue >> 8);
        uint8_t *c =
            (selector == UVC_VS_PROBE_CONTROL) ? probe_ctrl : commit_ctrl;

        if (r->bmRequestType == 0x21 &&
            r->bRequest == UVC_SET_CUR &&
            wLength == UVC_STREAM_CTRL_LEN) {
            if (selector == UVC_VS_PROBE_CONTROL)
                uvc_screen_line(UVC_S_PROBE, "UVC: PROBE SETUP");
            else
                uvc_screen_line(UVC_S_COMMIT, "UVC: COMMIT SETUP");

            usb_ep0_out_kind = selector;
            ep0_state = EP0_OUT_DATA;
            ep0_arm_out((uint16_t)UVC_STREAM_CTRL_LEN);
            return;
        }

        if (r->bmRequestType == 0xa1) {
            if (r->bRequest == UVC_GET_LEN) {
                data = len48;
                len = 2;
            } else if (r->bRequest == UVC_GET_INFO) {
                data = info1;
                len = 1;
            } else if (r->bRequest == UVC_GET_CUR) {
                stream_ctrl_normalize(c);
                data = c;
                len = UVC_STREAM_CTRL_LEN;
            } else if (r->bRequest == UVC_GET_MIN ||
                       r->bRequest == UVC_GET_MAX ||
                       r->bRequest == UVC_GET_RES ||
                       r->bRequest == UVC_GET_DEF) {
                static uint8_t variant[UVC_STREAM_CTRL_LEN];
                stream_ctrl_get_variant(r->bRequest, variant);
                data = variant;
                len = UVC_STREAM_CTRL_LEN;
            }

            if (data) {
                if (len > wLength)
                    len = wLength;
                ep0_state = EP0_IN_DATA;
                ep0_send(data, len);
                return;
            }
        }
    }

    /* UVC VC request-error-code control: report NO_ERROR. */
    if (r->bmRequestType == 0xa1 && (r->wIndex & 0xffU) == 0U &&
        (wValue >> 8) == 0x02U && r->bRequest == UVC_GET_CUR) {
        ep0_state = EP0_IN_DATA;
        ep0_send(zero1, wLength < 1U ? wLength : 1U);
        return;
    }

    uvc_screen_setup_stall(r);
    ep0_stall();
}

static void poll_rx(void)
{
    while (rd(USB_GINTSTS) & USB_GINTSTS_RXFLVL) {
        uint32_t st = rd(USB_GRXSTSP);
        uint32_t pkt = USB_GRXSTS_PKTSTS(st);
        uint32_t len = USB_GRXSTS_BYTECNT(st);
        uint32_t ep = USB_GRXSTS_EPNUM(st);

        if (pkt == USB_PKTSTS_SETUPRX && ep == 0 && len == 8U) {
            uint8_t raw[8];
            struct usb_setup_packet r;
            fifo_read(raw, 8);
            r.bmRequestType = raw[0];
            r.bRequest = raw[1];
            r.wValue = (uint16_t)(raw[2] | ((uint16_t)raw[3] << 8));
            r.wIndex = (uint16_t)(raw[4] | ((uint16_t)raw[5] << 8));
            r.wLength = (uint16_t)(raw[6] | ((uint16_t)raw[7] << 8));
            handle_setup(&r);
            continue;
        }

        if (pkt == USB_PKTSTS_OUTRX) {
            if (len == 0U) {
                if (ep == 0 && ep0_state == EP0_OUT_STATUS)
                    ep0_state = EP0_IDLE;
                continue;
            }
            if (ep == 0 && usb_ep0_out_kind && len <= 64U) {
                uint8_t buf[64];
                uint32_t n = len;
                uint8_t selector = usb_ep0_out_kind;
                fifo_read(buf, n);

                if (n == UVC_STREAM_CTRL_LEN) {
                    uint8_t *dst =
                        (selector == UVC_VS_PROBE_CONTROL) ?
                        probe_ctrl : commit_ctrl;

                    for (uint32_t i = 0; i < UVC_STREAM_CTRL_LEN; ++i)
                        dst[i] = buf[i];

                    /*
                     * TinyUSB performs this after CONTROL_STAGE_DATA:
                     * validate/normalize the host proposal, then commit it.
                     */
                    stream_ctrl_normalize(dst);

                    if (selector == UVC_VS_PROBE_CONTROL) {
                        uvc_screen_line(UVC_S_PROBE, "UVC: PROBE DATA OK");
                    } else {
                        uvc_payload_size =
                            (uint16_t)uvc_get_le32(&dst[UVC_SC_PAYLOAD_SIZE]);
                        if (uvc_payload_size > UVC_TINYUSB_PAYLOAD_MAX)
                            uvc_payload_size = UVC_TINYUSB_PAYLOAD_MAX;
                        if (uvc_payload_size < 3U)
                            uvc_payload_size = 3U;

                        usb_stream_committed = 1;
                        uvc_screen_line(UVC_S_COMMIT, "UVC: COMMIT DATA OK");
                        uvc_screen_line(UVC_S_STREAM, "UVC: STREAM READY");
                                                                video_frame_busy = 0;
                        video_frame_pos = 0;
                        video_frame_fid = 0;
                        cpu1_producer_quiesce();
                        producer_busy = 0;
                        producer_ready = 0;
                        producer_src_start = 0;
                        video_frame_buf = frame;
    producer_frame_buf = frame_next;
    ready_frame_buf = 0;
    spare_frame_buf = frame_third;
                                            next_frame_tick = 0;
                                            ft_frame_valid = 0;
                    }
                }

                usb_ep0_out_kind = 0;
                ep0_state = EP0_IN_STATUS;
                ep0_zlp();
            } else {
                drain_fifo(len);
            }
        }
    }
}

static void poll_epints(void)
{
    uint32_t daint = rd(USB_DAINT);
    if (daint & USB_DAINT_INEP(0)) {
        uint32_t i = rd(USB_DIEPINT(0));
        if (i) wr(USB_DIEPINT(0), i);
        if (i & USB_DXEPINT_XFERCOMPL) {
            if (ep0_state == EP0_IN_STATUS) {
                const uint8_t audio_action = usb_audio_ep_action_pending;
                usb_audio_ep_action_pending = 0;
                ep0_state = EP0_IDLE;
                ep0_arm_setup();

                /* EP0 transaction is now complete: it is safe to reprogram EP2. */
                if (audio_action == 1U) {
                    audio_stream_start();
                    uvc_screen_line(UVC_S_AUDIO_ALT1, "UAC2: ALT1 OPEN");
                } else if (audio_action == 2U) {
                    audio_stream_stop();
                    uvc_screen_line(UVC_S_AUDIO_ALT0, "UAC2: ALT0 IDLE");
                }
            } else if (ep0_state == EP0_IN_DATA) {
                if (ep0_tx_pos < ep0_tx_len) {
                    /* Continue a long control-IN data stage (e.g. UVC config). */
                    ep0_send_next_chunk();
                } else {
                    ep0_state = EP0_OUT_STATUS;
                    ep0_arm_out(0);
                }
            }
        }
    }
    if (daint & USB_DAINT_OUTEP(0)) {
        uint32_t i = rd(USB_DOEPINT(0));
        if (i) wr(USB_DOEPINT(0), i);
        if ((i & USB_DXEPINT_XFERCOMPL) && ep0_state == EP0_OUT_STATUS) {
            ep0_state = EP0_IDLE;
            ep0_arm_setup();
        }
    }
    if (usb_audio_hw_active && (daint & USB_DAINT_INEP(USB_AUDIO_EP_IN))) {
        uint32_t i = rd(USB_DIEPINT(USB_AUDIO_EP_IN));
        if (i) wr(USB_DIEPINT(USB_AUDIO_EP_IN), i);
        if (i & USB_DXEPINT_XFERCOMPL) {
            usb_ep2_busy = 0;
            usb_audio_packets++;
            uvc_screen_line(UVC_S_AUDIO_TX, "UAC2: ISO TX DONE");
            (void)start_audio_in();
        }
        if (i & USB_DXEPINT_EPDISBLD)
            usb_ep2_busy = 0;
    }

    if (daint & USB_DAINT_INEP(UVC_EP_VIDEO_IN)) {
        uint32_t i = rd(USB_DIEPINT(UVC_EP_VIDEO_IN));
        if (i) wr(USB_DIEPINT(UVC_EP_VIDEO_IN), i);
        /*
         * TinyUSB DWC2 slave mode: TXFE means FIFO space is available for
         * one or more complete packets from the already-armed transfer.
         */
        if ((i & USB_DXEPINT_TXFEMP) &&
            (rd(USB_DIEPEMPMSK) & (1U << UVC_EP_VIDEO_IN))) {
            uint64_t t;
            asm volatile("mrs %0, CNTPCT_EL0" : "=r"(t));
            diag_txfe_total++;
            diag_txfe_frame++;
            if (diag_last_txfe_tick) {
                uint64_t gap = t - diag_last_txfe_tick;
                if (gap > diag_max_txfe_gap_ticks)
                    diag_max_txfe_gap_ticks = gap;
            }
            diag_last_txfe_tick = t;
            video_tx_fill_fifo();
        }

        if (i & USB_DXEPINT_XFERCOMPL) {
            diag_xfrc_total++;
            diag_xfrc_frame++;
            uvc_screen_line(UVC_S_TX_DONE, "UVC: TX DONE");
            wr(USB_DIEPEMPMSK,
               rd(USB_DIEPEMPMSK) & ~(1U << UVC_EP_VIDEO_IN));
            usb_ep1_busy = 0;
            video_tx_buf = 0;
            video_tx_total = 0;
            video_tx_written = 0;

            /*
             * POC14: chain the next payload of the SAME frame immediately,
             * equivalent in spirit to TinyUSB continuing an application-level
             * frame transfer until its frame-complete callback.
             */
            if (video_frame_busy) {
                if (video_frame_pos < UVC_FRAME_BYTES) {
                    (void)video_start_next_payload();
                } else {
                    video_frame_complete();
                }
            }
        }
    }
}

static void usb_poll(void)
{
    uint32_t g;
    if (!usb_hw_up)
        return;
    g = rd(USB_GINTSTS);
    if (g & USB_GINTSTS_USBRST) {
        wr(USB_GINTSTS, USB_GINTSTS_USBRST);
        on_reset();
    }
    if (g & USB_GINTSTS_ENUMDONE) {
        wr(USB_GINTSTS, USB_GINTSTS_ENUMDONE);
        on_enum_done();
    }
    if (g & USB_GINTSTS_RXFLVL)
        poll_rx();
    if (g & (USB_GINTSTS_IEPINT | USB_GINTSTS_OEPINT))
        poll_epints();

    /* ISO has the tighter deadline: keep EP2 armed before servicing video work. */
    if (usb_configured && usb_audio_alt == 1U && usb_audio_hw_active &&
        !usb_ep2_busy) {
        if (!start_audio_in())
            usb_audio_xfr_errors++;
    }

    /* POC32: observe the already-running Unicam clock without touching it. */
    ft_clock_probe_poll();

    /* POC43: DWC2 first, then harvest a completed CPU1 producer job. */
    video_producer_poll();
    kick_video();
}

/*
 * POC15: CPU2 housekeeper must no longer touch DWC2.  Keep the symbol so the
 * existing PiStorm hook can remain in place, but make it intentionally inert.
 */
void emu68_uvc_housekeeper_poll(void)
{
}

/*
 * Dedicated runtime owner for CPU3.  secondary_boot() enters this function
 * only when PPC is disabled.  Before emu68_uvc_init() completes, sleep in WFE;
 * init sets usb_housekeeper_enabled and executes SEV.  After that, CPU3 polls
 * DWC2 continuously and independently of Amiga/housekeeper activity.
 */
void emu68_uvc_cpu3_worker(void)
{
    for (;;) {
        if (!usb_housekeeper_enabled) {
            asm volatile("wfe");
            continue;
        }

        /* POC16: deliberately consume CPU3 completely for the saturation test. */
        usb_poll();
    }
}

static int hw_init(void)
{
    uint32_t v;
    usb_housekeeper_enabled = 0;
    (void)set_power_state(3, 3);
    delay_ms(20);

    v = rd(USB_GSNPSID);
    kprintf("[USB-UVC] GSNPSID=%08x\n", v);
    if ((v & 0xffff0000U) != 0x4f540000U) {
        kprintf("[USB-UVC] no Synopsys OTG core\n");
        uvc_screen_line(UVC_S_HW_FAIL, "UVC: FAIL GSNPSID");
        return 0;
    }

    wr(USB_DCTL, rd(USB_DCTL) | USB_DCTL_SFTDISCON);
    v = rd(USB_GUSBCFG);
    v &= ~(USB_GUSBCFG_FORCEHOSTMODE | USB_GUSBCFG_HNPCAP |
           USB_GUSBCFG_SRPCAP | USB_GUSBCFG_TOUTCAL_MASK);
    v |= USB_GUSBCFG_FORCEDEVMODE | 7U;
    wr(USB_GUSBCFG, v);
    delay_ms(25);

    if (!wait_mask(USB_GRSTCTL, USB_GRSTCTL_AHBIDLE,
                   USB_GRSTCTL_AHBIDLE, 5000000U)) {
        uvc_screen_line(UVC_S_HW_FAIL, "UVC: FAIL AHB IDLE");
        return 0;
    }
    wr(USB_GRSTCTL, USB_GRSTCTL_CSFTRST);
    if (!wait_mask(USB_GRSTCTL, USB_GRSTCTL_CSFTRST, 0, 5000000U)) {
        uvc_screen_line(UVC_S_HW_FAIL, "UVC: FAIL CORE RESET");
        return 0;
    }
    delay_ms(10);

    v = rd(USB_GUSBCFG);
    v &= ~USB_GUSBCFG_FORCEHOSTMODE;
    v |= USB_GUSBCFG_FORCEDEVMODE;
    wr(USB_GUSBCFG, v);
    delay_ms(25);
    if (rd(USB_GINTSTS) & USB_GINTSTS_CURMODE_HOST) {
        uvc_screen_line(UVC_S_HW_FAIL, "UVC: FAIL HOST MODE");
        return 0;
    }

    v = rd(USB_GAHBCFG);
    v &= ~(USB_GAHBCFG_DMA_EN | USB_GAHBCFG_GLBL_INTR_EN);
    wr(USB_GAHBCFG, v);
    wr(USB_GINTMSK, 0);

    wr(USB_GRXFSIZ, 256U);
    wr(USB_GNPTXFSIZ, (128U << 16) | 256U);
    /* Preserve POC43 EP1 video FIFO: words 384..639. */
    wr(USB_DPTXFSIZ(UVC_EP_VIDEO_IN), (256U << 16) | 384U);
    /* AV POC3 EP2 audio FIFO: 64 words = 256 bytes, starts at word 640. */
    wr(USB_DPTXFSIZ(USB_AUDIO_EP_IN), (64U << 16) | 640U);
    flush_fifos();

    v = rd(USB_DCFG);
    v &= ~(USB_DCFG_DEVADDR_MASK | USB_DCFG_DEVSPD_MASK);
    v |= USB_DCFG_DEVSPD_HS;
    wr(USB_DCFG, v);
    wr(USB_DIEPMSK, USB_DXEPINT_XFERCOMPL);
    wr(USB_DOEPMSK, USB_DXEPINT_XFERCOMPL | USB_DXEPINT_SETUP);

    on_reset();
    usb_hw_up = 1;
    wr(USB_DCTL, rd(USB_DCTL) & ~USB_DCTL_SFTDISCON);
    uvc_screen_line(UVC_S_HW_OK, "UVC: CONNECTED");
    return 1;
}

void emu68_uvc_init(void)
{

    build_test_pattern_into(frame);
    build_test_pattern_into(frame_next);
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(timer_freq));


    cpu1_job_src = 0;
    cpu1_job_dst = 0;
    cpu1_job_pending = 0;
    cpu1_job_done = 0;
    /* cpu1_worker_online is owned by CPU1 and may already be 1 here. */
    cpu1_jobs_started = 0;
    cpu1_jobs_done = 0;
    asm volatile("dmb sy" ::: "memory");

    kprintf("[USB-AV] POC3: UVC + UAC2 HS125 + Paula CACHE-FIX2\n");
    if (!hw_init()) {
        kprintf("[USB-UVC] init failed\n");
        return;
    }

    /* Wake CPU2 Paula and CPU3 USB together.  CPU1 is already the validated
     * Framethrower converter; CPU3 remains the sole DWC2 owner. */
    kprintf("[USB-AV] POC3 core map: CPU1 video, CPU2 Paula+housekeeper, CPU3 DWC2\n");
    __atomic_store_n(&audio_wr, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&audio_rd, 0U, __ATOMIC_RELAXED);
    asm volatile("dmb sy" ::: "memory");
    av_cpu2_enabled = 1;
    usb_housekeeper_enabled = 1;
    asm volatile("sev" ::: "memory");

    /* Regular boot: do not hold CPU0 after the workers are live. */
}

#else
void emu68_uvc_init(void) {}
void emu68_uvc_housekeeper_poll(void) {}
void emu68_uvc_cpu3_worker(void) { for (;;) asm volatile("wfe"); }
void emu68_uvc_cpu1_worker(void) { for (;;) asm volatile("wfe"); }
void emu68_av_cpu2_worker(void) { for (;;) asm volatile("wfe"); }
#endif

/**
 * @file anedya_cam_webrtc.c
 * @brief WebRTC integration for BK7258 via libpeer + Anedya cloud signaling.
 *
 * This module:
 *  1. Creates and manages a libpeer PeerConnection (H.264 video, PCMA audio).
 *  2. Processes incoming WebRTC offers received via Anedya MQTT commands.
 *  3. Bridges the Beken AVDK camera (UVC) and voice pipeline into the
 *     PeerConnection's RTP send path.
 *  4. Manages the ICE connection lifecycle (connect, media start, teardown).
 *
 * Signaling flow:
 *   Browser → Anedya MQTT → this module (offer) → libpeer → this module (answer) → Anedya MQTT → Browser
 *
 * Media flow:
 *   UVC Camera → H.264 frames → peer_connection_send_video()
 *   Onboard Mic → G.711a → peer_connection_send_audio()
 */

#include <os/os.h>
#include <os/mem.h>
#include <os/str.h>
#include <components/log.h>
#include <string.h>
#include <stdio.h>

#include "anedya_cam_webrtc.h"
#include "anedya_cam_cmd.h"
#include "anedya_cam_devices.h"
#include "anedya_cam_mqtt.h"
#include "anedya_cam_comm.h"
#include "anedya_config.h"

/* libpeer */
#include "peer_connection.h"

/* cJSON is available from the webrtc component's third_party */
#include "cJSON.h"

/* Offers/answers over Anedya Commands are base64(deflate(...)) to fit the
 * command payload budget (see anedya-dev-sdk-esp-idf's anedya_signaling.c,
 * the reference this decode/encode path is ported from). mbedTLS's base64
 * is used (not libpeer's own src/base64.c) because libpeer's base64_encode/
 * base64_decode collide at link time with Beken's own base64 component,
 * which is also pulled into this project. */
#include "miniz.h"
#include "mbedtls/base64.h"

/* Beken media APIs */
#include "wifi_transfer.h"
#include "media_app.h"
#include <modules/wifi.h>   /* bk_wifi_set_wifi_media_mode / _set_video_quality */

#define TAG "anedya-webrtc"

#define LOGI(...) BK_LOGI(TAG, ##__VA_ARGS__)
#define LOGW(...) BK_LOGW(TAG, ##__VA_ARGS__)
#define LOGE(...) BK_LOGE(TAG, ##__VA_ARGS__)
#define LOGD(...) BK_LOGD(TAG, ##__VA_ARGS__)

/* ── Configuration ────────────────────────────────────────────────────── */

/* STUN/TURN and the credential pair all come from ap/include/anedya_config.h,
 * which is the single file a user edits. The aliases below keep the local
 * names short; the reasoning for each value lives in that header. */
#define ANEDYA_TURN_USER ANEDYA_TURN_USERNAME
#define ANEDYA_TURN_PASS ANEDYA_TURN_CREDENTIAL

/* Peer loop task parameters. Bumped preemptively alongside webrtc_offer_task
 * (which stack-overflowed at 8192 in testing) — this task runs the DTLS-SRTP
 * handshake (peer_connection_loop -> dtls_srtp_handshake), and mbedTLS is
 * known to be stack-hungry during the handshake path. */
#define PEER_LOOP_TASK_STACK   (1024 * 16)
/* BEKEN_APPLICATION_PRIORITY (7), not the plain worker priority: this loop
 * now also decodes and plays inbound audio (peer_connection_loop -> agent_recv
 * -> onaudiotrack -> bk_voice_write_frame_data), which needs a steady ~20ms
 * cadence to avoid speaker underruns. It was previously priority 5 — BELOW
 * trs_app_task's BEKEN_DEFAULT_WORKER_PRIORITY (6), the task that drives our
 * own video-send pipeline (media_app_register_read_frame_callback() spins
 * trs_app_task up unconditionally to call back into webrtc_video_frame_cb on
 * every encoded frame). FreeRTOS is strictly priority-preemptive, so any time
 * trs_app_task woke up with a frame to send, it preempted this loop outright —
 * mid-audio-write, mid-DTLS, mid-RTCP, whatever it was doing — which is a
 * plausible full explanation for both the choppy "old radio" audio and the
 * PLI-storm/framerate symptoms: this loop simply couldn't run on schedule. */
#define PEER_LOOP_TASK_PRIO    BEKEN_APPLICATION_PRIORITY

/* Offer/answer working-buffer sizes (mirrors anedya-dev-sdk-esp-idf's
 * anedya_signaling.c OFFER_DEFLATE_MAX_BYTES/OFFER_JSON_MAX_BYTES/
 * ANSWER_DEFLATE_MAX_BYTES). */
/* A real browser offer (2 ICE servers, full ICE gathering done before
 * sending, no trickle) inflated to 2709 bytes in testing, which blew past
 * the original 2048-byte OFFER_JSON_MAX_BYTES — tinfl ran out of output
 * space, returned HAS_MORE_OUTPUT (not caught by an "if (status < 0)"
 * check), and we silently fed cJSON a truncated buffer. Sized with
 * headroom for more ICE candidates/TURN servers. */
#define OFFER_DEFLATE_MAX_BYTES   1536  /* raw deflate bytes after base64 decode */
#define OFFER_JSON_MAX_BYTES      4096  /* inflated offer JSON, incl. NUL */
#define ANSWER_DEFLATE_MAX_BYTES  1536  /* raw deflate bytes of the compressed answer */

/* Verbose bring-up instrumentation: per-frame H.264 headers and a dump of the
 * offer's codec lines. Chatty (fires per frame / per offer) — off by default,
 * set to 1 when diagnosing media or negotiation problems. Mirrors
 * ANEDYA_WEBRTC_DEBUG in ap/components/webrtc/src/config.h. */
#ifndef ANEDYA_WEBRTC_DEBUG
#define ANEDYA_WEBRTC_DEBUG 1
#endif

/* Capture resolution — 1280x720, the UVC camera's native mode.
 *
 * This was briefly dropped to 640x480 while the PLI storm was mistakenly
 * attributed to bandwidth. It was not: 720p streamed cleanly for hours until
 * audio was enabled, and the actual cause was an unsynchronised srtp_protect()
 * between the video and audio sender tasks corrupting packets (see
 * PeerConnection.send_mutex in peer_connection.c). Lowering the resolution
 * only reduced how often the race was hit, so it is back at native.
 *
 * Worth knowing if bandwidth ever does become the limit: bitrate scales with
 * macroblock count (720p = 3600 MBs) times the driver's per-macroblock bit
 * budget, which CONFIG_H264_QUALITY_LEVEL selects — level 2 (MIDDLE, the
 * default) spends imb_bits=160 / pmb_bits=60 and measured ~3.9 Mbps here. */
/* ANEDYA_VIDEO_WIDTH / _HEIGHT come from anedya_config.h. */

/* UVC camera device ID used by the Beken AVDK */
#define UVC_DEVICE_ID          (0xFDF6)

/* ── State ────────────────────────────────────────────────────────────── */

static PeerConnection *s_pc = NULL;
static beken_thread_t s_peer_loop_thd = NULL;
static volatile bool s_peer_loop_running = false;
static volatile bool s_media_started = false;

/* ── Forward declarations ─────────────────────────────────────────────── */

static void peer_loop_task(void *arg);
static void on_ice_state_change(PeerConnectionState state, void *userdata);
static void on_ice_candidate(char *sdp_text, void *userdata);
static void start_media(void);
static void stop_media(void);

/* ── Video transfer callback (UVC H.264 → WebRTC) ────────────────────── */

/**
 * This callback is invoked by the Beken wifi_transfer subsystem for each
 * video frame captured from the UVC camera. We forward the H.264 NAL units
 * directly into libpeer's RTP packetizer.
 */
/**
 * Whole-frame H.264 callback, registered directly with the media pipeline
 * via media_app_register_read_frame_callback() (NOT through wifi_transfer —
 * see the rationale in start_media()). frame->frame / frame->length is one
 * complete encoded frame; libpeer handles the RTP fragmentation itself.
 */
static void webrtc_video_frame_cb(frame_buffer_t *frame)
{
    if (!frame || !frame->frame || frame->length == 0)
    {
        return;
    }

    if (s_pc && s_media_started)
    {
        int ret = peer_connection_send_video(s_pc, frame->frame, frame->length);

#if ANEDYA_WEBRTC_DEBUG
        /* Rate-limited so it can't flood at ~17fps. Confirms frames actually
         * reach this callback at all and whether libpeer accepts them —
         * peer_connection_send_video() returns -1 unless the connection is
         * in COMPLETED state. */
        static uint32_t s_frame_count = 0;
        if ((s_frame_count++ % 100) == 0)
        {
            /* First bytes matter: libpeer's h264_find_nalu() scans for
             * Annex-B start codes (00 00 01). If this encoder emits AVCC
             * (4-byte big-endian length prefix) or anything else, that scan
             * finds nothing, rtp_encoder_encode_h264()'s loop body never
             * runs, and ZERO RTP packets are emitted — while
             * peer_connection_send_video() still returns 0, since
             * rtp_encoder_encode_h264() unconditionally returns 0. So
             * "send ret=0" alone cannot distinguish "sent" from "silently
             * dropped"; these bytes can. */
            const uint8_t *d = frame->frame;
            LOGI("Video frame %u: len=%u seq=%u h264_type=0x%x ret=%d hdr=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                 (unsigned)s_frame_count, (unsigned)frame->length,
                 (unsigned)frame->sequence, (unsigned)frame->h264_type, ret,
                 d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
        }
#else
        (void)ret;
#endif
    }
}

/**
 * Called by libpeer when the browser sends an RTCP PLI/FIR — i.e. it needs a
 * fresh keyframe to (re)start decoding. Without this the browser can never
 * recover from a missed IDR and just shows nothing indefinitely.
 */
static void on_request_keyframe(void *userdata)
{
    LOGI("Browser requested keyframe (PLI) — regenerating IDR\n");
    media_app_h264_regenerate_idr(UVC_CAMERA);
}

/* ── Audio transfer callback (Mic G.711a → WebRTC) ───────────────────── */

/**
 * Voice read callback — invoked by bk_voice_read for each audio frame.
 * The data is already G.711a encoded (PCMA) by the Beken voice pipeline.
 */
/* The media_transfer_cb_t contract, as implemented by the original doorbell
 * transports (see anedya_cam_udp_aud_send_prepare() -> transmission_pack()):
 *
 *   1. caller obtains the destination buffer via get_tx_buf()
 *   2. caller invokes prepare(src, len)  -- THIS must copy src into that buffer
 *   3. caller invokes send(tx_buf, len)  -- transmits the buffer from step 1
 *
 * See anedya_cam_udp_voice_send_callback() in anedya_cam_devices.c: it passes
 * get_tx_buf() to send(), NOT the incoming `data` pointer. prepare() used to
 * be an empty `return 0` stub here, so nothing ever wrote the buffer and every
 * audio packet transmitted was 1280 bytes of an untouched static array — a
 * constant, silent-but-not-silence G.711A pattern. That is the "bursts of
 * random noise"; the microphone samples were never actually leaving the
 * device. The buffer must live at file scope (not function-static inside
 * get_tx_buf) so prepare() can write the same memory send() will read. */
static uint8_t s_audio_tx_buf[1280];

/* One RTP packet's worth of G.711A: CONFIG_AUDIO_DURATION (20 ms) at 8 kHz,
 * one byte per sample. Must equal the timestamp step rtp_encoder_init()
 * applies to PCMA, and voice_read_cfg.max_read_size is set to match. */
#define ANEDYA_PCMA_FRAME_BYTES 160

/* One RTP packet per 20 ms of audio.
 *
 * rtp_encoder_encode_generic() advances the PCMA timestamp by exactly one
 * CONFIG_AUDIO_DURATION step per call, whatever payload size it is handed, so
 * the packet size and the timestamp step have to agree or the RTP clock drifts
 * away from real time. Splitting here means that stays true even if the voice
 * pipeline's chunk size changes again.
 * */
static int webrtc_audio_send(uint8_t *data, uint32_t len)
{
    uint32_t offset = 0;

    if (!s_pc || !s_media_started)
    {
        return -1;
    }

    while (offset + ANEDYA_PCMA_FRAME_BYTES <= len)
    {
        int ret = peer_connection_send_audio(s_pc, data + offset, ANEDYA_PCMA_FRAME_BYTES);
        if (ret < 0)
        {
            return ret;
        }
        offset += ANEDYA_PCMA_FRAME_BYTES;
    }

    if (offset < len)
    {
        return peer_connection_send_audio(s_pc, data + offset, len - offset);
    }

    return 0;
}

static int webrtc_audio_prepare(uint8_t *data, uint32_t len)
{
    if (data == NULL || len > sizeof(s_audio_tx_buf))
    {
        LOGE("audio prepare: bad frame (data=%p len=%u max=%u)\n",
             data, (unsigned)len, (unsigned)sizeof(s_audio_tx_buf));
        return -1;
    }

    memcpy(s_audio_tx_buf, data, len);
    return 0;
}

static void *webrtc_audio_get_tx_buf(void)
{
    return s_audio_tx_buf;
}

static int webrtc_audio_get_tx_size(void)
{
    return sizeof(s_audio_tx_buf);
}

static const media_transfer_cb_t s_webrtc_audio_transfer_cb = {
    .send        = webrtc_audio_send,
    .prepare     = webrtc_audio_prepare,
    .drop_check  = NULL,
    .get_tx_buf  = webrtc_audio_get_tx_buf,
    .get_tx_size = webrtc_audio_get_tx_size,
};

/* ── Inbound audio (WebRTC → onboard speaker) ────────────────────────── */

/**
 * libpeer calls this from peer_connection_loop() (PEER_CONNECTION_COMPLETED)
 * for every inbound audio RTP packet, already SRTP-decrypted and stripped
 * down to the raw payload — see rtp_decoder_decode()/rtp_decode_generic() in
 * ap/components/webrtc/src/rtp.c. libpeer does not decode PCMA itself (only
 * H.264 gets special handling there); what arrives here is exactly the
 * encoder's G.711A bytes, which is precisely what
 * anedya_cam_audio_data_callback() → bk_voice_write_frame_data() expects —
 * it was already written for this shape of data, just never called, because
 * nothing fed it outside the doorbell's now-dead UDP/TCP/CS2 transports.
 */
static void on_audio_track(uint8_t *data, size_t size, void *userdata)
{
#if ANEDYA_WEBRTC_DEBUG
    /* Distinguishes "the browser never sent us audio" from "audio arrived but
     * the speaker path swallowed it" — the two look identical from the sofa. */
    static uint32_t s_rx_audio = 0;
    if ((s_rx_audio++ % 250) == 0)
    {
        LOGI("Inbound audio frames: %u (this one %u bytes)\n",
             (unsigned)s_rx_audio, (unsigned)size);
    }
#endif

    anedya_cam_audio_data_callback(data, (uint32_t)size);
}

/* ── PeerConnection lifecycle ─────────────────────────────────────────── */

static PeerConnection *create_peer_connection(void)
{
    PeerConfiguration config = {
        .ice_servers = {
            { .urls = ANEDYA_STUN_URL, .username = NULL, .credential = NULL },
            { .urls = ANEDYA_TURN_URL, .username = ANEDYA_TURN_USER, .credential = ANEDYA_TURN_PASS },
            { .urls = NULL }
        },
        .audio_codec = CODEC_PCMA,
        .video_codec = CODEC_H264,
        .datachannel = DATA_CHANNEL_NONE,
        .onaudiotrack = on_audio_track,  /* mic (browser) -> onboard speaker; see sdp.c's a=sendrecv on the audio m-line */
        .onvideotrack = NULL,
        .on_request_keyframe = on_request_keyframe,
        .user_data = NULL,
    };

    PeerConnection *pc = peer_connection_create(&config);
    if (!pc)
    {
        LOGE("Failed to create PeerConnection\n");
        return NULL;
    }

    peer_connection_oniceconnectionstatechange(pc, on_ice_state_change);
    peer_connection_onicecandidate(pc, on_ice_candidate);

    return pc;
}

static void on_ice_candidate(char *sdp_text, void *userdata)
{
    LOGD("ICE candidate gathered\n");
    /* Trickle ICE is not used — full SDP with all candidates is sent in the answer. */
}

static void on_ice_state_change(PeerConnectionState state, void *userdata)
{
    LOGI("ICE state: %s\n", peer_connection_state_to_string(state));

    switch (state)
    {
        case PEER_CONNECTION_COMPLETED:
            LOGI("WebRTC connected! Starting media...\n");
            start_media();
            /* Report success back to Anedya */
            anedya_cam_mqtt_publish_command_status("success", NULL);
            break;

        case PEER_CONNECTION_FAILED:
            LOGE("WebRTC connection failed\n");
            stop_media();
            anedya_cam_mqtt_publish_command_status("failure", "ICE failed");
            break;

        case PEER_CONNECTION_DISCONNECTED:
        case PEER_CONNECTION_CLOSED:
            LOGW("WebRTC disconnected\n");
            stop_media();
            break;

        default:
            break;
    }
}

/* ── Peer loop task ───────────────────────────────────────────────────── */

/* NOTE: do not call rtos_dump_task_runtime_stats() from here (a temporary
 * per-task CPU% dump used to live in this loop). That SDK function disables
 * interrupts, then mallocs a buffer sized from the task count — and its
 * out-of-memory path returns WITHOUT re-enabling them, so under the memory
 * pressure of live streaming it eventually hung the scheduler and watchdogged
 * the device. See docs/PATCHES.md; the leak itself is now fixed in
 * rtos_debug.c, but this loop has no business dumping diagnostics anyway. */
static void peer_loop_task(void *arg)
{
    LOGI("Peer loop task started\n");
    s_peer_loop_running = true;

    while (s_peer_loop_running && s_pc)
    {
        peer_connection_loop(s_pc);
    }

    LOGI("Peer loop task exiting\n");
    rtos_delete_thread(NULL);
}

/* ── Media start/stop ─────────────────────────────────────────────────── */

static void start_media(void)
{
    if (s_media_started)
    {
        LOGW("Media already started\n");
        return;
    }

    LOGI("Starting UVC camera + audio pipeline\n");

    /* Audio still goes through the device subsystem's transfer callback. */
    anedya_cam_devices_set_audio_transfer_callback((void *)&s_webrtc_audio_transfer_cb);

    /* bk_wifi_transfer_frame_open() used to do this before we bypassed
     * wifi_transfer — it tunes the WiFi driver for sustained video streaming
     * (media mode + SD video quality). Keep doing it explicitly so dropping
     * that path doesn't silently regress throughput. */
    bk_wifi_set_wifi_media_mode(true);
    bk_wifi_set_video_quality(WIFI_VIDEO_QUALITY_SD);

    camera_parameters_t cam_params = {0};
    cam_params.id = UVC_DEVICE_ID;
    cam_params.width = ANEDYA_VIDEO_WIDTH;
    cam_params.height = ANEDYA_VIDEO_HEIGHT;
    cam_params.format = 1;     /* 1 = H.264 */
    cam_params.protocol = 0;
    cam_params.rotate = 0;

    int ret = anedya_cam_camera_turn_on(&cam_params);
    if (ret != BK_OK && ret != 2)
    {
        LOGE("Camera open failed: %d\n", ret);
    }

    /* Take whole H.264 frames straight from the media pipeline instead of
     * going through anedya_cam_video_transfer_turn_on() / wifi_transfer.
     *
     * wifi_transfer implements Beken's own UDP streaming protocol (see
     * wifi_transfer_send_frame): it slices each frame into pkt_size chunks
     * and prepends a transfer_data_t header (id/eof/cnt/size) to every one
     * before invoking the media_transfer_cb_t .send callback. That's the
     * right shape for the doorbell demo's custom receiver app, but wrong
     * for WebRTC — libpeer does its own RTP fragmentation and needs intact
     * H.264 NAL units, so it was being handed header-prefixed fragments it
     * could not parse. Registering here delivers the complete encoded
     * frame (frame->frame / frame->length) with no added framing. */
    ret = media_app_register_read_frame_callback(IMAGE_H264, webrtc_video_frame_cb);
    if (ret != BK_OK)
    {
        LOGE("Video frame callback registration failed: %d\n", ret);
    }

    /* Force a keyframe now that the camera + encode pipeline are actually
     * up. A decoder can't start on P-frames — without an IDR (plus its
     * SPS/PPS) the browser has nothing to begin decoding and just shows
     * nothing, which matches the symptom exactly. anedya_cam_camera_turn_on()
     * does attempt this internally, but it runs the request *before* opening
     * the camera, so it always fails there ("media_app_h264_regenerate_idr
     * camera not open" in the log) and no keyframe is ever produced. */
    if (media_app_h264_regenerate_idr(UVC_CAMERA) != BK_OK)
    {
        LOGW("Failed to force initial IDR\n");
    }
    else
    {
        LOGI("Forced initial IDR keyframe\n");
    }

    /* Start audio (onboard mic → G.711a encoder → voice_read callback) */
    audio_parameters_t aud_params = {0};
    aud_params.aec = 1;       /* Echo cancellation on */
    aud_params.uac = 0;       /* Onboard mic, not USB audio */
    aud_params.rmt_recorder_sample_rate = ANEDYA_AUDIO_RATE;
    aud_params.rmt_player_sample_rate = ANEDYA_AUDIO_RATE;
    /* codec_format_t: 0 is CODEC_FORMAT_UNKNOW, not G.711A (that's 1) — this
     * literal 0 hit the switch's default case in anedya_cam_audio_turn_on(),
     * logging "not support encoder format" and failing the whole call with
     * -1, which is exactly the "Audio start failed: -1 (non-fatal)" seen in
     * testing. Both directions use G.711A (matches .audio_codec = CODEC_PCMA
     * on the PeerConnection config below). */
    aud_params.rmt_recoder_fmt = CODEC_FORMAT_G711A;
    aud_params.rmt_player_fmt = CODEC_FORMAT_G711A;

    ret = anedya_cam_audio_turn_on(&aud_params);
    if (ret != BK_OK)
    {
        LOGW("Audio start failed: %d (non-fatal)\n", ret);
    }


    s_media_started = true;
    LOGI("Media pipeline started\n");
}

static void stop_media(void)
{
    if (!s_media_started)
    {
        return;
    }

    LOGI("Stopping media pipeline\n");

    /* Matches the direct media_app_register_read_frame_callback() in
     * start_media() — video never goes through wifi_transfer, so
     * anedya_cam_video_transfer_turn_off() is not the right teardown here. */
    media_app_unregister_read_frame_callback();
    anedya_cam_audio_turn_off();
    anedya_cam_camera_turn_off();

    /* Mirror of the media-mode tuning applied in start_media(). */
    bk_wifi_set_wifi_media_mode(false);
    bk_wifi_set_video_quality(WIFI_VIDEO_QUALITY_HD);

    s_media_started = false;
    LOGI("Media pipeline stopped\n");
}

/* ── Public API ───────────────────────────────────────────────────────── */

void anedya_cam_webrtc_init(void)
{
    LOGI("Initializing WebRTC subsystem\n");

    if (s_pc)
    {
        LOGW("WebRTC already initialized\n");
        return;
    }

    s_pc = create_peer_connection();
    if (!s_pc)
    {
        LOGE("PeerConnection creation failed\n");
        return;
    }

    /* Start the peer loop in its own task */
    bk_err_t ret = rtos_create_thread(
        &s_peer_loop_thd,
        PEER_LOOP_TASK_PRIO,
        "peer_loop",
        (beken_thread_function_t)peer_loop_task,
        PEER_LOOP_TASK_STACK,
        NULL);

    if (ret != BK_OK)
    {
        LOGE("Failed to create peer loop task\n");
        peer_connection_destroy(s_pc);
        s_pc = NULL;
        return;
    }

    LOGI("WebRTC subsystem ready, waiting for offer...\n");
}

void anedya_cam_webrtc_deinit(void)
{
    LOGI("Deinitializing WebRTC subsystem\n");

    stop_media();
    s_peer_loop_running = false;

    if (s_pc)
    {
        peer_connection_close(s_pc);
        /* Give the peer loop task time to exit */
        rtos_delay_milliseconds(200);
        peer_connection_destroy(s_pc);
        s_pc = NULL;
    }

    s_peer_loop_thd = NULL;
    LOGI("WebRTC subsystem deinitialized\n");
}

/* Decode the browser's offer payload (base64 -> raw inflate) into a
 * heap-allocated, NUL-terminated JSON string. Caller frees. Returns NULL on
 * failure. Ported from anedya-dev-sdk-esp-idf's anedya_signaling.c
 * decode_offer(). */
static char *decode_offer_sdp(const char *b64_input, size_t b64_len)
{
    /* base64_decode() has no output-size check, so verify it fits first. */
    size_t decoded_length_estimate = b64_len / 4 * 3;
    if (decoded_length_estimate > OFFER_DEFLATE_MAX_BYTES)
    {
        LOGE("Offer too large: %u encoded bytes exceed %d-byte decode buffer\n",
             (unsigned)b64_len, OFFER_DEFLATE_MAX_BYTES);
        return NULL;
    }

    /* PSRAM, not the small internal-SRAM heap — this device already runs
     * WiFi/TLS/camera/audio concurrently, and tdefl_compressor below alone
     * is ~300KB; internal SRAM allocation for these scratch buffers failed
     * with "Out of memory" in testing. psram_free() is just os_free(). */
    char *deflate_buffer = psram_malloc(OFFER_DEFLATE_MAX_BYTES);
    if (!deflate_buffer)
    {
        LOGE("Out of memory allocating deflate buffer\n");
        return NULL;
    }

    size_t deflate_length = 0;
    int b64_ret = mbedtls_base64_decode((unsigned char *)deflate_buffer, OFFER_DEFLATE_MAX_BYTES,
                                        &deflate_length, (const unsigned char *)b64_input, b64_len);
    if (b64_ret != 0)
    {
        LOGE("base64 decode failed: -0x%x\n", (unsigned)-b64_ret);
        os_free(deflate_buffer);
        return NULL;
    }
    LOGD("Offer base64 decoded: %u bytes\n", (unsigned)deflate_length);

    /* tinfl_decompressor is ~8KB; PSRAM rather than risk the caller's stack
     * (the MQTT rx path) or internal-SRAM pressure. */
    tinfl_decompressor *decompressor = psram_malloc(sizeof(tinfl_decompressor));
    char *offer_buffer = psram_malloc(OFFER_JSON_MAX_BYTES);
    if (!decompressor || !offer_buffer)
    {
        LOGE("Out of memory allocating decompressor/offer buffer\n");
        os_free(decompressor);
        os_free(offer_buffer);
        os_free(deflate_buffer);
        return NULL;
    }

    tinfl_init(decompressor);
    size_t input_bytes = (size_t)deflate_length;
    size_t output_bytes = OFFER_JSON_MAX_BYTES - 1;  /* leave room for NUL */
    tinfl_status status = tinfl_decompress(
        decompressor,
        (const mz_uint8 *)deflate_buffer, &input_bytes,
        (mz_uint8 *)offer_buffer, (mz_uint8 *)offer_buffer, &output_bytes,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    os_free(decompressor);
    os_free(deflate_buffer);

    /* TINFL_STATUS_DONE (0) is the only status meaning "fully decompressed".
     * Anything else (including the non-negative HAS_MORE_OUTPUT/
     * NEEDS_MORE_INPUT) means the buffers were sized wrong and offer_buffer
     * only holds a truncated prefix — must not be treated as success. */
    if (status != TINFL_STATUS_DONE)
    {
        LOGE("tinfl decompression incomplete: status=%d (in=%u out=%u/%d)\n",
             (int)status, (unsigned)input_bytes, (unsigned)output_bytes,
             OFFER_JSON_MAX_BYTES - 1);
        os_free(offer_buffer);
        return NULL;
    }
    offer_buffer[output_bytes] = '\0';
    LOGD("Offer inflated to JSON: %d bytes\n", (int)output_bytes);
    return offer_buffer;
}

/* Compress (raw deflate) + base64-encode an SDP answer for the command
 * ack data budget. Caller frees the returned string. Returns NULL on
 * failure. Ported from anedya_signaling_write_answer() in
 * anedya-dev-sdk-esp-idf's anedya_signaling.c. */
static char *encode_answer_sdp(const char *sdp, size_t sdp_len)
{
    /* tdefl_compressor is ~300KB in this build (default TDEFL_LZ_CODE_BUF_SIZE/
     * TDEFL_OUT_BUF_SIZE/hash table sizes) — internal SRAM allocation for it
     * failed with "Out of memory" in testing alongside WiFi/TLS/camera/audio;
     * PSRAM has the headroom. psram_free() is just os_free(). */
    tdefl_compressor *compressor = psram_malloc(sizeof(tdefl_compressor));
    unsigned char *deflate_buffer = psram_malloc(ANSWER_DEFLATE_MAX_BYTES);
    if (!compressor || !deflate_buffer)
    {
        LOGE("Out of memory allocating compressor/deflate buffer\n");
        os_free(compressor);
        os_free(deflate_buffer);
        return NULL;
    }

    /* raw deflate (no zlib header) — matches the browser's "deflate-raw"
     * and the raw tinfl inflate used for the offer. */
    tdefl_init(compressor, NULL, NULL, TDEFL_DEFAULT_MAX_PROBES);
    size_t input_bytes = sdp_len;
    size_t output_bytes = ANSWER_DEFLATE_MAX_BYTES;
    tdefl_status deflate_status = tdefl_compress(compressor, sdp, &input_bytes,
                                                 deflate_buffer, &output_bytes,
                                                 TDEFL_FINISH);
    os_free(compressor);
    if (deflate_status != TDEFL_STATUS_DONE)
    {
        if (deflate_status == TDEFL_STATUS_OKAY)
        {
            LOGE("Answer too large: %d-byte SDP does not fit %d-byte deflate buffer\n",
                 (int)sdp_len, ANSWER_DEFLATE_MAX_BYTES);
        }
        else
        {
            LOGE("deflate failed: status=%d\n", (int)deflate_status);
        }
        os_free(deflate_buffer);
        return NULL;
    }
    size_t deflate_length = output_bytes;

    size_t base64_capacity = ((deflate_length + 2) / 3) * 4 + 1;
    char *base64_str = os_malloc(base64_capacity);
    if (!base64_str)
    {
        LOGE("Out of memory allocating base64 buffer\n");
        os_free(deflate_buffer);
        return NULL;
    }
    size_t base64_length = 0;
    int b64_ret = mbedtls_base64_encode((unsigned char *)base64_str, base64_capacity,
                                        &base64_length, deflate_buffer, deflate_length);
    os_free(deflate_buffer);
    if (b64_ret != 0)
    {
        LOGE("base64 encode failed: -0x%x\n", (unsigned)-b64_ret);
        os_free(base64_str);
        return NULL;
    }
    base64_str[base64_length] = '\0';

    LOGD("Answer sizes: raw=%dB -> deflate=%dB -> base64=%dB\n",
         (int)sdp_len, (int)deflate_length, (int)strlen(base64_str));
    return base64_str;
}

void anedya_cam_webrtc_on_offer(const char *offer_payload, size_t len)
{
    LOGI("Processing WebRTC offer (%u bytes)\n", (unsigned)len);

    if (!s_pc)
    {
        LOGE("PeerConnection not initialized\n");
        return;
    }

    /* The command's 'data' field is base64(deflate(offer JSON)), not raw
     * JSON — decode+inflate it first. */
    char *offer_json = decode_offer_sdp(offer_payload, len);
    if (!offer_json)
    {
        LOGE("Failed to decode offer payload\n");
        return;
    }

    /* Parse the offer JSON: {"sdp": "v=0\r\n...", "turn": {...}} */
    cJSON *root = cJSON_Parse(offer_json);
    os_free(offer_json);
    if (!root)
    {
        LOGE("Failed to parse offer JSON\n");
        return;
    }

    cJSON *sdp_item = cJSON_GetObjectItemCaseSensitive(root, "sdp");
    if (!cJSON_IsString(sdp_item) || sdp_item->valuestring == NULL)
    {
        LOGE("Offer JSON missing 'sdp' field\n");
        cJSON_Delete(root);
        return;
    }

    const char *remote_sdp = sdp_item->valuestring;
    LOGI("Remote SDP received (%u bytes)\n", (unsigned)strlen(remote_sdp));

#if ANEDYA_WEBRTC_DEBUG
    /* Dump the offer's codec lines (m= / a=rtpmap: / a=fmtp:). Invaluable when
     * the browser and device disagree about payload types or packetization
     * mode — that mismatch is silent otherwise. */
    {
        const char *p = remote_sdp;
        char linebuf[200];
        while (*p)
        {
            const char *nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);
            if (n > 0 && p[n - 1] == '\r') n--;
            if (n >= sizeof(linebuf)) n = sizeof(linebuf) - 1;

            if (n > 0 &&
                (p[0] == 'm' ||
                 strncmp(p, "a=rtpmap:", 9) == 0 ||
                 strncmp(p, "a=fmtp:", 7) == 0))
            {
                memcpy(linebuf, p, n);
                linebuf[n] = '\0';
                LOGI("OFFER| %s\n", linebuf);
            }

            if (!nl) break;
            p = nl + 1;
        }
    }
#endif

    /* If there's an existing session, tear it down first */
    if (s_media_started)
    {
        LOGW("Tearing down previous session for new offer\n");
        stop_media();
    }

    /* Close and recreate PeerConnection for clean state */
    peer_connection_close(s_pc);
    peer_connection_destroy(s_pc);
    s_pc = create_peer_connection();
    if (!s_pc)
    {
        LOGE("Failed to recreate PeerConnection\n");
        cJSON_Delete(root);
        return;
    }

    /* Set the remote offer */
    peer_connection_set_remote_description(s_pc, remote_sdp, SDP_TYPE_OFFER);

    /* Generate our answer */
    const char *answer_sdp = peer_connection_create_answer(s_pc);
    if (!answer_sdp || strlen(answer_sdp) == 0)
    {
        LOGE("Failed to create SDP answer\n");
        cJSON_Delete(root);
        return;
    }

    LOGI("SDP answer generated (%u bytes)\n", (unsigned)strlen(answer_sdp));

    /* Candidate gathering (STUN srflx / TURN relay) happened synchronously
     * inside create_answer() above and its candidates are embedded directly
     * in the SDP (no trickle) — PeerConnection/Agent internals are opaque
     * from here, so count them straight from the SDP text. If this is 1
     * (host only), STUN/TURN gathering failed and only a same-LAN peer will
     * ever be able to connect. */
    {
        int host_n = 0, srflx_n = 0, relay_n = 0, other_n = 0;
        const char *p = answer_sdp;
        while ((p = strstr(p, "a=candidate:")) != NULL)
        {
            if (strstr(p, "typ host")) host_n++;
            else if (strstr(p, "typ srflx")) srflx_n++;
            else if (strstr(p, "typ relay")) relay_n++;
            else other_n++;
            p += 12; /* past "a=candidate:" so strstr makes progress */
        }
        LOGI("Local ICE candidates: host=%d srflx=%d relay=%d other=%d\n",
             host_n, srflx_n, relay_n, other_n);
        if (srflx_n == 0 && relay_n == 0)
        {
            LOGW("No STUN/TURN candidates gathered — only same-LAN peers can connect\n");
        }
    }

    /* Compress + base64 the raw SDP answer (as ackdata) and publish via
     * MQTT — mirrors anedya_signaling_write_answer(); no JSON wrapper. */
    char *answer_b64 = encode_answer_sdp(answer_sdp, strlen(answer_sdp));
    if (answer_b64)
    {
        anedya_cam_mqtt_publish_answer(answer_b64);
        os_free(answer_b64);
    }
    else
    {
        LOGE("Failed to encode answer\n");
    }

    cJSON_Delete(root);

    LOGI("Answer published, waiting for ICE to connect...\n");
}

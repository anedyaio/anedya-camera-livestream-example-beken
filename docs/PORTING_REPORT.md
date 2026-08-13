# Porting libpeer (WebRTC) to the Beken BK7258

**Result:** live 720p H.264 video from a UVC camera on a BK7258 to a browser,
plus **two-way G.711A audio** (onboard mic to the browser, browser mic to the
onboard speaker), over WebRTC, signalled through Anedya's MQTT command channel.
Peer-to-peer or through a TURN relay.

**Status:** video and both audio directions work and are stable, including on
high-detail scenes that previously froze the stream (§3.11). Lost video packets
are recovered by NACK retransmission (§3.12). The remaining gaps are robustness
rather than function — no congestion control, video-only NACK, and open-loop
RTP pacing — see [Known limitations](#known-limitations).

---

## 1. Why libpeer

The original approach used the vendor-supplied closed-source `metaRTC`
(`libyangipc.a`). It consistently failed ICE gathering
(`gather stun candidate fail(4101)`, `turn request timeout`). Instrumentation
ruled out the two plausible local causes — memory exhaustion and UDP socket
pool exhaustion — which meant any further progress needed either vendor source
or vendor support. Neither was available.

[libpeer](https://github.com/sepfy/libpeer) (MIT) was chosen instead: small, C,
BSD-socket based, and designed for embedded targets. Everything below is what
it took to make it work on this SoC.

---

## 2. Architecture

```
Browser (web/index.html)                   BK7258
    |                                         |
    |-- webrtc_offer command --> Anedya MQTT --> anedya_cam_mqtt.c
    |     base64(deflate(SDP))                  |  decode -> anedya_cam_webrtc.c
    |                                           |  libpeer creates answer
    |<-- command ackdata ------- Anedya MQTT <--|  base64(deflate(SDP))
    |                                           |
    |<========== SRTP media (H.264 / RTP) ======|  UVC -> H.264 pipeline
                  direct, peer-to-peer             -> peer_connection_send_video()
```

Key pieces:

| File | Role |
|---|---|
| `app/ap/src/anedya_cam_mqtt.c` | Anedya MQTT: commands in, answer/status out |
| `app/ap/src/anedya_cam_webrtc.c` | libpeer lifecycle, offer/answer codec, media wiring |
| `libpeer/` | libpeer + libsrtp + cJSON + miniz (submodule) |

Signalling rides Anedya's **Commands** feature rather than a dedicated
signalling server. Because a full SDP exceeds the ~1 KB command payload budget,
offers and answers are **raw-deflate compressed and base64-encoded** in both
directions (`miniz` on device, `CompressionStream("deflate-raw")` in browser).

---

## 3. The bugs, in the order they had to be solved

Roughly grouped; each was blocking.

### 3.1 Build / link

Beken's `psa_mbedtls` uses an **explicit, non-glob source list**, so several
mbedTLS files were present on disk but never compiled — their symbols simply
did not exist regardless of config flags (`rsa.c`, later `ecp.c`/`ecdh.c`/
`ecdsa.c`). Separately, `CONFIG_FULL_MBEDTLS` defaults to `n` on this board and
gates an entire config branch containing every DTLS-SRTP symbol needed.

The lesson that recurred all session: **a config flag being set does not mean
the code is compiled.** Ground truth came from `nm` on the `.obj`, from
`compile_commands.json`, and once from `gcc -E -dM` to prove a macro really was
undefined.

### 3.2 Silent self-inflicted stalls

Three separate busy-wait loops with **zero backoff**, each causing resource
starvation that looked like a memory bug:

1. `dtls_srtp_do_handshake()` retried `WANT_READ`/`WANT_WRITE` instantly —
   ~30 `sendto()` failures per millisecond, starving the WiFi driver of the CPU
   it needed to free the very buffers being waited on.
2. `peer_connection_loop()`'s `CONNECTED` case retried the whole handshake with
   no delay on failure — eventually crashed the device outright.
3. `udp_socket_sendto()` had no retry at all, so a transient `ENOMEM` from a
   full TX queue dropped the packet immediately.

### 3.3 Threading

`mqtt_connection_cb` and `mqtt_incoming_data_cb` are raw lwIP callbacks and run
**on the tcpip thread**. Both called into libpeer, which opens BSD sockets —
and lwIP's socket layer blocks waiting for the tcpip thread to service the
request. The tcpip thread deadlocked against itself, permanently, with no log
output. Both paths now dispatch to their own task.

Two more threading bugs surfaced later, once the system was actually under
sustained load:

- **lwIP core lock.** `mqtt_publish()` and `mqtt_client_is_connected()` both
  open with `LWIP_ASSERT_CORE_LOCKED()` — they may only be called while holding
  the tcpip core lock. The heartbeat task and the SDP-answer publish called them
  from ordinary FreeRTOS tasks with no lock. This port enables
  `LWIP_TCPIP_CORE_LOCKING` but leaves `LWIP_ASSERT_CORE_LOCKED` at its default
  no-op, so the violation was completely silent. `mqtt_close()` runs on the
  tcpip thread and nulls `client->conn` before invoking the disconnect callback;
  if that landed near a heartbeat tick, the heartbeat task dereferenced a
  pointer that had just been freed. That is the `mqtt_hb` NULL-deref crash that
  only ever appeared after ~an hour — it needed the two to coincide. All such
  calls are now wrapped in `LOCK_TCPIP_CORE()`/`UNLOCK_TCPIP_CORE()`.

- **`srtp_protect()` from two tasks.** See §3.8 — the worst bug of the audio
  bring-up, and the reason video appeared to "break" the moment audio was
  enabled.

### 3.4 Memory

Two distinct ceilings, both non-obvious:

- `tdefl_compressor` is ~300 KB; allocating it from internal SRAM failed.
  Moved to PSRAM.
- **`MEM_MAX_TX_SIZE`** — a *TX-only* cap enforced inside lwIP's `mem.c`,
  separate from total heap and from the pbuf pool. At its 42666 default, one
  H.264 keyframe (~52 KB of RTP) exceeded it and `sendto()` failed
  continuously. No amount of `PBUF_POOL_SIZE` tuning could have fixed it. This
  was the single highest-impact fix for streaming stability.

### 3.5 Memory-safety bugs in libpeer/libsrtp

Real defects, several of which crashed the device:

- `peer_connection_set_remote_description()` copied SDP lines into a
  `char buf[256]` with an **unbounded** `strncpy` — any longer line smashed the
  stack canary.
- `ice_candidate_from_description()` used `sscanf` with bare `%s` conversions
  (no field widths) into 33/16-byte buffers — the classic `gets()` hazard.
- `agent_create_turn_addr()` **discarded** the second `recv` result and then
  tested a stale `ret`, so a timed-out TURN response was never detected.
- `dtls_srtp_reset_session()` freed the SRTP sessions without nulling the
  pointers, leaving them dangling.
- `ports_get_epoch_time()` packed **millisecond Unix epoch time into a
  `uint32_t`** — which overflows every ~49.7 days of range, i.e. on every call.
  The unsigned keepalive subtraction then underflowed, closing every connection
  within milliseconds of reaching `COMPLETED`. Replaced with lwIP's monotonic
  `sys_now()`.

### 3.6 SDP / negotiation correctness

Each of these caused the browser to reject or silently ignore the stream:

| Symptom | Cause |
|---|---|
| Every `a=` line ignored | Both SDP parsers split lines on literal `"\r\n"`; the browser strips CR to save payload bytes, so the parse loop body never ran once. |
| "order of m-lines doesn't match" | Answer hardcoded `a=mid:video`/`audio`; JSEP requires echoing the offer's actual mid (Chrome uses `0`/`1`). |
| "BUNDLE group contains a MID matching no m= section" | `a=group:BUNDLE` still listed the old hardcoded mids. |
| "Incompatible send direction" | Answer said `a=sendrecv` to a `recvonly` offer; must be `sendonly`. |
| Connected, `ontrack` fired, **nothing rendered** | Answer used hardcoded payload type **96**, which the browser never offered (it proposes 102/104/108/…). H.264 is a *dynamic* PT and RFC 3264 requires reusing the offer's numbering, so Chrome had no decoder bound to it and dropped every packet. Now negotiated from the offer, preferring `packetization-mode=1` (required for the FU-A fragments libpeer emits). |

Audio was unaffected by this class of bug because PCMA is payload type 8 — a
*static* assignment from RFC 3551.

### 3.7 The last one: SRTP was never initialised

With ICE connected, DTLS complete, the correct payload type negotiated, and
100k+ RTP packets emitted — the browser still showed a spinner.

`peer_init()`, which calls libsrtp's mandatory global `srtp_init()`, lives in
`src/peer.c` — **a file that was never added to the build**. Without it every
`srtp_create()` failed, `srtp_in`/`srtp_out` stayed NULL, and the device
transmitted **plaintext RTP**, which the browser silently discards on a
DTLS-SRTP session.

Two things kept this hidden for a long time:

- The `srtp_create()` failure logged at `LOGD`, compiled out at the build's
  INFO level — a silent failure by construction.
- A NULL guard added earlier (to fix a genuine crash) turned the loud failure
  into a silent skip.

Calling `srtp_init()` then exposed a *second* layer: it runs a known-answer
self-test on **every** registered algorithm and aborts wholesale if any fails.
`aes_gcm_mbedtls.c` fails its KAT — so a broken implementation of a cipher
suite **this session never uses** was single-handedly disabling all SRTP.
Disabling GCM (in both the compile flags *and* `srtp_config.h`) resolved it.

---

## 3.8 Remote connectivity: STUN/TURN and device-side relay

The "same-LAN only" limitation was not a firewall. `stun.ap-in-1` /
`turn.ap-in-1` resolve to a host that answers nothing; `turn1.ap-in-1` answers
immediately. A dead endpoint, not the network, is why gathering produced
`srflx=0 relay=0` for the entire bring-up.

With that corrected, a set of genuine protocol bugs surfaced:

- **Constant STUN transaction IDs.** `stun_msg_create()` built every message
  with three fixed `CRC32_TABLE` entries, so every STUN/TURN request the device
  ever sent carried an identical transaction ID. Servers cache responses by
  transaction ID and replay them for apparent retransmissions — so the
  authenticated Allocate could be answered with the 401 cached from the
  unauthenticated one that preceded it. Now randomised per message.

- **ICE candidates on the wrong m-section.** Candidate lines were appended
  after the whole SDP was built, landing them on the *last* m-section (audio).
  With `a=group:BUNDLE 0 1`, Chrome bundles the non-tag sections onto the tag
  and discards their transport parameters — so it accepted the answer and then
  silently threw every candidate away, never forming a pair and never entering
  "checking". The device saw `rx_total=0` and looked like the broken end. This
  affected *every* session, not just relay.

- **TURN relay (RFC 5766) implemented device-side** behind `CONFIG_TURN_RELAY`:
  Allocate with 401/nonce and 438 stale-nonce handling, CreatePermission,
  ChannelBind, ChannelData framing, and — critically — periodic **Allocate
  Refresh**. The allocation lifetime is a separate server-side timer that only
  an explicit Refresh extends; ongoing ChannelData traffic refreshes the
  permission and channel but *not* the allocation. Without it a long relay
  session dies silently mid-stream when the server tears the allocation down.

- Also fixed: a receive helper that returned the first datagram of any kind
  with no transaction/method matching (so an ICE check could be mistaken for a
  TURN response), ICE giving up after a single pass over the candidate list
  with no wall-clock budget, and relay-local pairs sending Binding requests
  straight to the peer's relay address instead of through the TURN server.

Note that `remote candidate type=3` (relay) on the browser side does **not**
imply a double relay hop. The device tries its pairs in gather order and takes
the first that works, so a device-host ↔ browser-relay pair is single-hop. The
selected pair is now logged explicitly at connect time rather than inferred
from data volume.

---

## 3.9 Audio bring-up: three ports of a broken assumption

Audio was the last subsystem, and every bug in it had the same shape — doorbell
code that was **correct under Beken's own UDP transport** and silently wrong
under RTP. None failed loudly; each produced plausible-sounding audio garbage.

1. **`not support encoder format`.** `aud_params.rmt_recoder_fmt = 0` with a
   comment claiming `0 == CODEC_FORMAT_G711A`. It does not — `0` is
   `CODEC_FORMAT_UNKNOW`, G.711A is `1`. This hit the driver's `default:` case
   and failed the whole call with `-1`. Because one call sets up *both*
   directions, this single wrong literal blocked mic **and** speaker.

2. **`prepare()` never copied.** The `media_transfer_cb_t` contract is:
   `get_tx_buf()` → `prepare(src,len)` *copies src into that buffer* →
   `send(tx_buf,len)` transmits it. The original doorbell `prepare()` calls
   `anedya_cam_transmission_pack()`, which does the copy. Ours was an empty
   `return 0` stub, so `send()` transmitted a static array **nothing ever
   wrote** — a constant G.711A pattern. The microphone samples never left the
   device. Symptom: "bursts of random noise", never recognisable as audio.

3. **RTP clock ran 8× slow.** `rtp_encoder_encode_generic()` advances the PCMA
   timestamp by exactly one `CONFIG_AUDIO_DURATION` (20 ms) step **per call**,
   whatever payload size it is handed. `voice_read_cfg.max_read_size` was 1280
   — inherited from the doorbell, whose transport carried no timestamps and so
   did not care how much audio a datagram held. At G.711A/8 kHz that is 160 ms
   of audio per packet stamped as 20 ms. The browser's jitter buffer backlogged
   without bound (audio arriving progressively later) and played at the wrong
   rate (distortion). Fixed by setting `max_read_size = 160` and, defensively,
   splitting oversized payloads in `webrtc_audio_send()` so the packet size and
   timestamp step cannot silently diverge again.

Also required: `onaudiotrack` was `NULL` (libpeer's receive path is generic and
complete, it simply had nothing wired to it), and the SDP answer hardcoded
`a=sendonly` on the audio m-line, so the browser would never negotiate sending
its microphone at all.

---

## 3.10 The audio/video interaction: one shared SRTP context

Video streamed cleanly at 720p for hours. It fell apart the moment audio was
enabled — and *only* then. The temptation was to blame bandwidth; measurements
said otherwise (94 % idle across both cores, and only two lwIP ENOMEM events in
a log full of constant PLIs, so the loss was on-air, not in the TX queue).

`peer_connection_outgoing_rtp_packet()` is the single funnel for both audio and
video RTP, and it calls `srtp_protect()` on the **one shared `srtp_t`**. In this
port the two streams are produced by **different tasks** — video from
`trs_app_task`, audio from the voice service's `voc_rd` task. libSRTP is
explicitly *not* thread-safe: concurrent calls corrupt per-stream sequence/ROC
and cipher state, the receiver's auth check fails, and it **silently discards
the packet**.

That loss is indistinguishable from network loss. And because libpeer handles
only `RTCP_PSFB` (PLI/FIR) and ignores `RTCP_RTPFB` — where NACK lives — there
is no retransmission, so every corrupted video packet costs a whole frame and
provokes another PLI, which is answered with a multi-packet IDR burst. A
self-sustaining storm, produced entirely by a missing lock.

Fixed with `PeerConnection.send_mutex`, held across both the encrypt and the
send. **Lesson: adding a second media stream is a concurrency change**, not
just a bandwidth one.

---

## 3.11 Freezes: two ceilings, one symptom

Long after everything "worked", the stream would freeze for a few seconds and
recover. Two independent causes, and the first one masked the second.

### Unpaced fragment bursts

`rtp_encoder_encode_h264_fu_a()` fragments one NAL unit and emits every
fragment back-to-back with zero delay — a ~75 KB 720p IDR is ~58 packets in a
burst, on top of a ~4 Mbps steady feed. lwIP's TX pool cannot absorb that:
`sendto()` returns ENOMEM, `udp_socket_sendto()` exhausts its retry budget, and
the packet is dropped **locally**.

With no NACK, one lost fragment destroys its whole frame → the browser sends a
PLI → the device answers with another IDR → another 58-packet burst into the
same full queue. The log shape is unmistakable: a drop cluster, a 75 KB IDR,
then a *second* drop cluster caused by that IDR. The gap from first drop to
recovery keyframe was ~2.5 s — precisely the visible freeze.

Fixed by yielding 1 ms every 8 fragments (`RTP_FU_A_PACING_FRAGMENTS`). ~7 ms
added to an IDR against a ~72 ms frame interval. The yield lands with
`send_mutex` released, so it doubles as the only window the audio sender gets
during a long IDR.

Measured after: throughput unchanged (396 → 400 pkt/s), drop bursts of ~60
packets replaced by isolated single packets.

### Capture-side frame buffer truncation

With the network path fixed, a second freeze remained — reproducible by
pointing the camera at a laptop screen. MJPEG is intra-only, so frame size
tracks scene entropy directly, and fine text with high contrast is the worst
case. The stock `CONFIG_JPEG_FRAME_SIZE` (102400) is the capacity of the buffer
`bk_uvc.c` assembles each UVC frame into:

```
uvc_stre: Frame buffer overflow: current=102320, need=640
jpeg hw_: decoder_error, 102392, ...
trs_app:  read frame NULL timeout
→ Browser requested keyframe (PLI)
```

Overflow sets `packet_error`, so the frame is abandoned mid-assembly; the
decoder then chokes on a truncated JPEG and no H.264 frame is produced. **No
packets were lost — the frame never existed.** `CONFIG_H264_FRAME_SIZE` hit the
same wall one stage later (`h264_encode_finish_handle, 103024-102400`).

The trap: both pools draw on the **same** PSRAM slab —
`frame_buffer_fb_malloc()` routes `IMAGE_MJPEG` *and* `IMAGE_H264` to
`frame_buffer_encode_malloc()`; only `IMAGE_YUV` goes to DISPLAY. So the two
sizes must be costed together, against `PSRAM_MEM_SLAB_ENCODE`.

A first attempt raised MJPEG to 128 KB, filling the stock slab to 98.7% on the
byte arithmetic alone. That failed twice over: 128 KB was still overflowed
(`current=130960`), *and* the allocator ran out —

```
frame_bu: frame_buffer_fb_malloc 599 malloc fail, img_format:4   (MJPEG)
frame_bu: frame_buffer_fb_malloc 599 malloc fail, img_format:8   (H.264)
```

— because each buffer also carries a header plus 32-byte alignment, and the
heap has its own block overhead. Worse, that failure *looked like a network
problem*: TX drops jumped from isolated packets to 30–65/second. The mechanism
is that `frame_buffer_fb_malloc()` walks its free list inside
`fb_enter_critical()` (`rtos_disable_int()` + spinlock), so hundreds of failed
allocations per second starve the WiFi driver of the time it needs to drain TX.

Resolved by moving 0x70000 (448 KB) of PSRAM from `PSRAM_MEM_SLAB_DISPLAY` to
`PSRAM_MEM_SLAB_ENCODE` in `partitions/bk7258/ram_regions.csv` — DISPLAY only
ever holds one 720p YUV422 frame (1843200 B) and cannot hold two, so the
remainder was slack — then setting MJPEG to 204800 (the Kconfig cap) and H.264
to 122880. See `docs/PATCHES.md`.

**Two lessons.** A frozen stream is not necessarily a network fault; check
whether the frame was ever produced. And when sizing pooled buffers, leave the
allocator real margin — do not size to the raw byte total.

---

## 3.12 NACK: the recovery path that was negotiated but never implemented

After §3.11 the loss rate was low — single packets, not bursts — yet the stream
still froze for a second or more whenever one was lost. The reason is that a
40 KB frame is ~31 RTP fragments and losing any one of them makes the frame
unreassemblable, so recovery required a PLI, which browsers rate-limit.

The discovery: **the answer had always advertised `a=rtcp-fb:<pt> nack`**
(`sdp.c`), and the browser's own offer carried it, so NACK was negotiated from
the very first session. Chrome had been sending Generic NACK reports the whole
time and `peer_connection_incoming_rtcp()` was discarding them in its
`default:` case. The recovery mechanism was negotiated, requested, and ignored.

Implemented as a ring of sent video packets, indexed by `seq % N` for O(1)
lookup. Three things that matter:

- **The ring stores post-`srtp_protect()` bytes.** A retransmission must be
  byte-identical to the original, and re-encrypting is not an option:
  `srtp_protect()` advances per-stream cipher state, so running it twice over
  one packet corrupts everything after it.
- **The stored sequence number is re-checked on read.** A slot may have been
  overwritten by a newer packet before the NACK arrived; resending the wrong
  bytes under the requested sequence number is worse than not answering.
- **Video only.** Audio is 160 B/packet where a loss costs 20 ms and cannot
  cascade, so buffering it would spend memory to fix nothing.

Sizing is 128 slots (~169 KB, PSRAM) ≈ 320 ms at the ~400 pkt/s this device
sustains, which covers a TURN-relayed round trip. Allocation failure is
deliberately non-fatal — retransmission disables itself and behaviour reverts
to the previous "a lost packet costs a frame", rather than losing video
entirely because a buffer would not fit.

Measured on first run: **51 NACK reports, 56 packets requested, 54 resent, 2
aged out** — a 96% hit rate, confirming the ring is correctly sized. The
functional proof is what stopped appearing: over 28 seconds of streaming with
NACKs actively arriving, **zero PLIs and zero IDRs**. Previously every loss
cluster produced both. Frames are now being completed from retransmissions
instead of rebuilt from keyframes.

Note this also settles a design question that could not be answered from the
code: retransmitting on the original SSRC with the original sequence number is
accepted here, so RFC 4588 RTX (separate SSRC, fresh sequence numbers) was not
needed. See limitation 1 for the caveat.

---

## 4. Diagnostic technique that actually worked

Most of the hard bugs were silent. What broke them open:

- **`LOG_REDIRECT`.** libpeer's internal logging defaults to
  `fprintf(stdout, …)`, which reaches nothing on this target. Every internal
  log — including STUN/TURN failures — was being discarded. Routing `peer_log()`
  to `BK_LOG*` was the single highest-value diagnostic change of the port.
- **Prove, don't infer.** `nm` on object files, `gcc -E -dM` for macro state,
  `addr2line` on the crash PC (which pinned a fault to `srtp_get_stream`
  immediately), and `compile_commands.json` for the real compiler invocation.
- **Distrust success returns.** `peer_connection_send_video()` returns 0 even
  when the packetizer emits nothing, because `rtp_encoder_encode_h264()`
  unconditionally returns 0. A counter inside the emit path was needed to tell
  "streaming" from "silently doing nothing".
- **Check both ends.** Browser-side `getStats()` (`inbound-rtp`:
  `packetsReceived` / `framesDecoded` / `pliCount`) distinguishes "never
  arrived" from "arrived but undecodable" — a distinction the device cannot
  make alone.

Instrumentation is retained behind `ANEDYA_WEBRTC_DEBUG` (default off) in
`ap/components/webrtc/src/config.h` and `anedya_cam_webrtc.c`.

---

## 5. Media pipeline note

The doorbell example this project derives from sends video via `wifi_transfer`,
which implements **Beken's own UDP streaming protocol**: it fragments each frame
and prepends a `transfer_data_t` header to every chunk. That is correct for its
matching receiver app but wrong for WebRTC — libpeer does its own RTP
fragmentation and needs intact H.264 NAL units.

The fix was to tap the pipeline one level earlier via
`media_app_register_read_frame_callback(IMAGE_H264, cb)`, which delivers whole
encoded frames with no added framing.

Also required: forcing an IDR **after** the pipeline is up (the SDK's own
attempt runs before the camera opens and always fails), and wiring
`on_request_keyframe` so the browser's RTCP PLI can trigger a fresh keyframe.

For UVC (unlike DVP) the camera emits MJPEG and a transcode pipeline produces
the H.264 — hence `pipeline_enable = true` on this path.

---

## Known limitations

*(Resolved since the first draft: same-LAN-only — see §3.8, the STUN/TURN
hostname was dead and candidates were being placed on the wrong m-section;
audio not starting — see §3.9; multi-second freezes on scene changes — see
§3.11, unpaced IDR bursts plus a 100 KB capture-side frame buffer ceiling; and
one-second freezes from single-packet loss — see §3.12, NACK was negotiated all
along but never handled.)*

1. **NACK covers video only, and without RTX.** Retransmission is implemented
   (§3.12), but audio is not buffered — a lost PCMA packet is still gone, which
   is the right trade at 160 B and 20 ms per packet. Retransmissions also reuse
   the original SSRC and sequence number rather than RFC 4588 RTX; that works
   against Chrome here, but a receiver enforcing strict SRTP replay protection
   could reject them. The retransmit ring is finite (128 packets, ~320 ms), so
   a NACK arriving later than that still costs a frame.
2. **No congestion control.** RTCP RR is parsed but the receiver-report /
   packet-loss path is `#if 0`'d out, and the encoder bitrate is fixed
   (`CONFIG_H264_QUALITY_LEVEL`, ~3.9 Mbps at 720p). The device cannot adapt to
   a degrading link.
3. **RTP pacing is open-loop.** `rtp_encoder_encode_h264_fu_a()` now yields
   1 ms every 8 fragments (§3.11), which bounds burst rate and was enough to
   stop IDR bursts from congesting the local TX pool. It does not measure
   anything: the interval is fixed, so it cannot speed up on a fast link or
   back off on a slow one. Real pacing would derive the gap from a target
   bitrate, which requires (2) first.
4. **`aes_gcm_mbedtls.c` is broken**, not merely disabled. Fix it before
   enabling GCM for any reason.
5. **Credentials are compile-time placeholders** (`YOUR_*`). TURN credentials
   in particular are short-lived (the username embeds a unix expiry timestamp),
   so a production build should fetch them at runtime as the browser already
   does. An expired pair presents as a 401 on the authenticated Allocate, which
   looks exactly like a credential-plumbing bug — decode the timestamp before
   suspecting the code.
6. **No reconnect logic** — Wi-Fi/MQTT disconnect is logged with a TODO, not
   handled.
7. **`ports_get_epoch_time()` is latently broken on this build.** §3.5 records
   replacing its 32-bit millisecond-epoch truncation with lwIP's monotonic
   `sys_now()` — but that replacement sits behind `#if CONFIG_USE_LWIP`, and
   `config.h` sets `CONFIG_USE_LWIP 0`. This build therefore still compiles the
   `gettimeofday()` branch:

   ```c
   return (uint32_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
   ```

   It works **only because the device has no RTC or NTP**, so `gettimeofday()`
   returns time since boot and stays small. If a wall clock is ever set, that
   expression truncates to a meaningless 32-bit value and the unsigned
   subtraction in `peer_connection_loop()`'s keepalive check
   (`ports_get_epoch_time() - pc->agent.binding_request_time >
   CONFIG_KEEPALIVE_TIMEOUT`) can underflow — closing every connection within
   milliseconds, which is precisely the symptom §3.5 claims to have cured. The
   ICE deadline (`AGENT_ICE_TIMEOUT_MS`) and the TURN allocation expiry read the
   same clock.

   The fix is to make the `sys_now()` path unconditional. It is left undone
   deliberately: it changes the timebase on a system that currently works, so
   it wants a deliberate retest rather than a drive-by edit. **Do this before
   adding any time synchronisation.**

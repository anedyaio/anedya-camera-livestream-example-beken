[<img src="https://img.shields.io/badge/Anedya-Documentation-blue?style=for-the-badge">](https://docs.anedya.io?utm_source=github&utm_medium=link&utm_campaign=github-examples&utm_content=beken-cam)
[<img src="https://img.shields.io/badge/Peer-Live-blue?style=for-the-badge">](https://anedyaio.github.io/anedya-camera-livestream-example-beken/)

<p align="center">
    <img src="https://cdn.anedya.io/anedya_black_banner.png" alt="Logo">
</p>

# Beken BK7258 — WebRTC Camera Livestream with Anedya

Turn a Beken BK7258 with a UVC camera into a real-time, two-way audio and video
device with Anedya (Commands signalling and TURN relay).

## ✨ Features

- **Signalling :** SDP offer/answer exchanged via Anedya Commands over MQTT, no custom signalling server needed
- **Peer to peer with TURN relay fallback :** Direct WebRTC connection, falling back to Anedya's TURN relay when a firewall blocks it
- **Real H.264 video over RTP :** 720p hardware-encoded video on a proper WebRTC media track, not JPEG over a DataChannel. [View Here](https://anedyaio.github.io/anedya-camera-livestream-example-beken/)
- **Two-way audio :** Onboard microphone to the browser and browser microphone to the onboard speaker, G.711A, with echo cancellation
- **Packet-loss recovery :** NACK retransmission (RFC 4585), so a dropped packet costs a round trip instead of a whole frame
- **Reconnects on its own :** Wi-Fi and MQTT both retry with exponential backoff, so an unattended camera survives an AP reboot

---

## 📋 Supported Development Environments

| Framework / Platform | Status |
|---|---|
| Beken Armino AVDK (Docker) | Available |

Builds on Linux, macOS and Windows — the toolchain runs inside Beken's official
Docker image, so nothing is installed on your machine but Docker itself.

---

## 📷 Anedya — Board Support

| Board | Support Status | Notes |
|---|---|---|
| Beken BK7258 development board | Supported | 2.4 GHz Wi-Fi only |

| Camera | Support Status | Notes |
|---|---|---|
| USB UVC camera (720p, MJPEG) | Supported | Transcoded to H.264 on-chip |

---

## 📁 Repository Layout

```
  ├── app/                            — the firmware
  │   ├── ap/include/
  │   │   └── anedya_config.h         — ★ Wi-Fi + Anedya credentials + all tuning
  │   ├── ap/src/
  │   │   ├── anedya_cam_core.c       — boot, event loop, Wi-Fi reconnection
  │   │   ├── anedya_cam_mqtt.c       — Anedya MQTT, Commands signalling, reconnection
  │   │   ├── anedya_cam_webrtc.c     — libpeer lifecycle, offer/answer, media wiring
  │   │   ├── anedya_cam_devices.c    — UVC camera and voice service
  │   │   └── anedya_cam_network.c    — Wi-Fi station
  │   ├── ap/config/bk7258_ap/config  — SDK configuration, documented inline
  │   └── partitions/                 — flash and PSRAM layout
  │
  ├── web/
  │   ├── index.html                  — browser viewer, single self-contained file
  │   └── README.md                   — serving it, and what its diagnostics mean
  │
  ├── docs/
  │   ├── PORTING_REPORT.md           — architecture, and every bug that had to be solved
  │   └── PATCHES.md                  — the six SDK changes, with reasoning for each
  │
  ├── tools/                          — build wrappers (bash + PowerShell)
  ├── sdk/                            — Beken Armino AVDK          (submodule)
  └── libpeer/                        — WebRTC implementation      (submodule)
```

---

## 🚀 Getting Started

### 1. Clone with submodules

```bash
git clone --recursive https://github.com/anedyaio/anedya-camera-livestream-example-beken.git
cd anedya-camera-livestream-example-beken
```

Forgot `--recursive`? Run `git submodule update --init --recursive`.

### 2. Create your Node

The Device UUID is **yours to choose**, not something the console issues.
Generate a UUID (v4) from any generator, then create the Node in the Anedya
console using it — supplying your own avoids colliding with an existing device.
The format is 8-4-4-4-12 hex digits:

```
xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
```

> **Preauthorize the Node.** Without it the device is refused at connect time
> even though the UUID and key are correct, and it looks exactly like a wrong
> credential. Check this first if MQTT will not connect.

### 3. Fill in your credentials

**One file. Nothing else needs editing.**

```
app/ap/include/anedya_config.h
```

| Setting | Where it comes from |
|---|---|
| `ANEDYA_WIFI_SSID` / `_PASSWORD` | Your 2.4 GHz network |
| `ANEDYA_DEVICE_UUID` | The UUID you generated in step 2 |
| `ANEDYA_CONNECTION_KEY` | Issued by the console alongside the Node |
| `ANEDYA_TURN_USERNAME` / `_CREDENTIAL` | `POST /v1/relay/create` on the Anedya API |

The file explains each one inline, including how each fails when it is wrong.

> **TURN credentials expire.** The trailing number in the username is a Unix
> timestamp — the moment they stop working. An expired pair makes the relay
> answer `401`, which looks exactly like a code bug and is not one.

Note the **Connection Key** (the device's, used by the firmware) is not the
**Platform API Key** (your account's, used by the browser page). You need both,
in different places.

### 4. Build

```bash
./tools/build.sh          # Linux / macOS
.\tools\build.ps1         # Windows (PowerShell)
```

Output: `sdk/build/bk7258/app/package/all-app.bin`

> Do **not** build with `sudo`. The container runs as your own user; under
> `sudo` every generated file becomes root-owned and the next normal build
> fails with a permission error.

### 5. Flash

Use **Beken's official flashing tool** (Windows). Select the chip, pick
`all-app.bin`, choose your serial port, and flash. Flashing needs a **hardware
reset** — hold RST, start the flash, release when it reports connecting.

> **On Linux/macOS** Beken's tool is not available. This example was developed
> using [`ltchiptool`](https://github.com/libretiny-eu/ltchiptool) to flash and
> [`picocom`](https://github.com/npat-efault/picocom) for the serial console.
> Neither is officially supported by Beken; if flashing fails, lower the baud
> rate.

### 6. Watch it boot

Serial at **115200 baud**:

```
Wi-Fi connected
IP: 192.168.1.42
MQTT connected to Anedya!
WebRTC subsystem ready, waiting for offer...
```

### 7. Open the viewer

```bash
cd web && python3 -m http.server 8000
```

Open <http://localhost:8000>, enter your **Node ID** and **Platform API Key** in
Settings, and press **Start Handshake**. See [`web/README.md`](web/README.md)
for what its diagnostics mean.

---

## 🏗 How It Works

### Signalling via Anedya Commands + MQTT

WebRTC requires both peers to exchange SDP offers and answers before media can
flow. This example uses Anedya Commands as the signalling channel and Anedya
MQTT as the notification mechanism.

```
Browser Viewer
  │  1. Fetch TURN credentials (Anedya REST API)
  │  2. Create WebRTC offer → Commands (base64(deflate(SDP)))
  ▼
Anedya Cloud  (Commands + MQTT broker + TURN relay)
  │  3. Notify BK7258 over MQTT subscription
  ▼
BK7258
  │  4. Decode offer, parse SDP, learn the peer's payload types and mids
  │  5. Gather ICE candidates, create answer → Commands
  ▼
Browser Viewer
  │  6. Poll Commands status → read answer → apply remote description
  │  7. ICE negotiation completes, DTLS handshake derives the SRTP keys
  │  8. H.264 video and G.711A audio flow over SRTP, both directions
```

An SDP is far larger than the ~1 KB Commands payload budget, so offers and
answers are raw-deflate compressed and base64 encoded in both directions —
`miniz` on the device, `CompressionStream("deflate-raw")` in the browser.

### Media over RTP, not DataChannel

Unlike simpler examples that push JPEG frames over a DataChannel, this one uses
**real WebRTC media tracks**. Video is H.264 from the chip's hardware encoder,
packetised into RTP with FU-A fragmentation; audio is G.711A in both directions.
The browser decodes it with its native pipeline, and standard WebRTC machinery —
NACK, PLI, jitter buffering — works as intended.

The trade is complexity: codec negotiation, SRTP keying and packetisation all
have to be right. [`docs/PORTING_REPORT.md`](docs/PORTING_REPORT.md) records
what that took.

### Connectivity

When both peers can reach each other, ICE resolves a direct path using STUN
address discovery. When a firewall blocks direct traffic, Anedya's managed TURN
relay is used automatically — this example implements the **device side of the
relay**: Allocate, CreatePermission, ChannelBind, and the periodic Allocate
Refresh that keeps a long session from being torn down mid-stream.

---

## 🩺 Troubleshooting

**Nothing on serial** — wrong baud (115200) or port. On Linux, add yourself to
the `dialout` group and log back in.

**Wi-Fi never connects** — 5 GHz network, or a typo. The board is 2.4 GHz only.

**MQTT never connects** — check the Node is **preauthorized** first, then the
UUID and Connection Key. All three look identical from outside: the broker
closes the connection right after CONNECT.

**Connects, but no video** — usually SDP negotiation or SRTP. Look for
`srtp_out is NULL — sending UNENCRYPTED RTP` in the log.

**Works on the same Wi-Fi, fails across networks** — TURN credentials. Check
the expiry timestamp first.

**Video freezes on busy scenes** — look for `uvc_stre: Frame buffer overflow`
*before* blaming the network. That is the camera's MJPEG frame exceeding its
buffer, so the frame never reaches the encoder at all.

---

## 🔧 A note on the SDK

This example needs **six changed files, 93 lines** in the Beken SDK — mostly to
compile sources that ship on disk but were left out of the build, and to enable
DTLS-SRTP. The SDK is consumed as a submodule of a fork, so you can see exactly
what differs from upstream:

```bash
git -C sdk log release/v3.0.1..HEAD
```

Each change is a separate commit with its reasoning, and
[`docs/PATCHES.md`](docs/PATCHES.md) summarises all six with a defence for each.

---

## 📚 References

**Anedya**
- [Anedya Overview](https://docs.anedya.io/anedya-overview/)
- [Anedya Concepts](https://docs.anedya.io/essentials/concepts/)
- [Anedya Project Setup](https://docs.anedya.io/getting-started/project-setup/)
- [Anedya MQTT Endpoints](https://docs.anedya.io/device/mqtt-endpoints/)
- [Anedya Commands](https://docs.anedya.io/features/commands/commands-intro/)
- [Anedya Platform API](https://docs.anedya.io/platform-api/)

**WebRTC & Beken**
- [WebRTC Overview](https://webrtc.org/getting-started/overview)
- [WebRTC Peer Connections](https://webrtc.org/getting-started/peer-connections)
- [libpeer](https://github.com/sepfy/libpeer) — the WebRTC implementation this port builds on
- [Beken Armino AVDK](https://github.com/bekencorp/bk_avdk_smp)
- [RFC 5766 — TURN](https://datatracker.ietf.org/doc/html/rfc5766)
- [RFC 4585 — RTP/AVPF, Generic NACK](https://datatracker.ietf.org/doc/html/rfc4585)

---

## 📑 Looking for other examples

- [Anedya Camera Livestream with ESP32](https://github.com/anedyaio/anedya-camera-livestream-example-esp32)
- [Anedya Camera Livestream with Raspberry Pi](https://github.com/anedyaio/anedya-camera-livestream-example)

---

## 📄 License

Apache-2.0. Builds on the Beken Armino AVDK (Apache-2.0) and
[libpeer](https://github.com/sepfy/libpeer) (MIT), which uses libsrtp
(BSD-3-Clause), cJSON (MIT) and miniz (MIT). Each component keeps its own
licence file.

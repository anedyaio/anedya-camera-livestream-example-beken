# Anedya Camera Livestream Example — Beken BK7258

Live **720p H.264 video** and **two-way voice** from a BK7258 to a web browser,
over WebRTC. Signalling goes through Anedya's cloud; the video and audio go
directly between the camera and the browser, including across different
networks.

This is a complete, working example — not a snippet. It handles NAT traversal,
TURN relay, packet-loss recovery, and Wi-Fi/MQTT reconnection.

```
   BK7258 + UVC camera                                    Your browser
   ├── H.264 video  ─┐                                  ┌─ <video>
   ├── mic  (G.711A) ─┼── SRTP, peer-to-peer ───────────┼─ speakers
   └── speaker       ─┘                                  └─ your mic
              │                                                │
              └──────── SDP offer/answer via Anedya MQTT ──────┘
```

---

## What you need

**Hardware**

| | |
|---|---|
| Board | Beken BK7258 development board |
| Camera | USB UVC camera, 720p, MJPEG output |
| Cable | USB for flashing and serial |

The BK7258 is **2.4 GHz Wi-Fi only**. A 5 GHz-only network will never
associate, and it looks exactly like a wrong password.

**Software**

| | |
|---|---|
| Docker | Builds the firmware. Works on Linux, macOS and Windows |
| Python 3 | Serves the browser page |
| Git | With submodule support |

**An Anedya account.** You will create one Node (using a UUID you generate
yourself) and preauthorize it. From that you get a **Connection Key** for the
firmware, plus a **Platform API Key** for the browser page. Step 2 walks
through it.

---

## Setup

### 1. Clone with submodules

```bash
git clone --recursive https://github.com/<org>/anedya-camera-livestream-example-beken.git
cd anedya-camera-livestream-example-beken
```

Already cloned without `--recursive`? Run:

```bash
git submodule update --init --recursive
```

This pulls the Beken SDK and libpeer. It is a large download — the SDK alone is
several hundred megabytes.

### 2. Fill in your credentials

**One file. Nothing else needs editing.**

```
app/ap/include/anedya_config.h
```

Open it and replace every `YOUR_*` placeholder:

| Setting | Where it comes from |
|---|---|
| `ANEDYA_WIFI_SSID` / `_PASSWORD` | Your 2.4 GHz network |
| `ANEDYA_DEVICE_UUID` | **You generate this**, then create the Node with it — see below |
| `ANEDYA_CONNECTION_KEY` | Issued by the console alongside the Node |
| `ANEDYA_TURN_USERNAME` / `_CREDENTIAL` | `POST /v1/relay/create` on Anedya's API |

The file explains each one inline, including how each fails when it is wrong.

**Creating the Node.** The Device UUID is yours to choose, not something the
console hands you. Generate a UUID (v4) from any generator, then create the
Node in the Anedya console using it — supplying your own avoids colliding with
an existing device. The format is 8-4-4-4-12 hex digits:

```
xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
```

> **Preauthorize the Node** in the console. Without it the device is refused at
> connect time even though the UUID and key are correct — and it looks exactly
> like a wrong credential. Check this first if MQTT will not connect.

Note the **Connection Key** (the device's, used by the firmware) is not the
**Platform API Key** (your account's, used by the browser page). You need both,
in different places.

> **TURN credentials expire.** The trailing number in the username is a Unix
> timestamp — the moment they stop working. An expired pair makes the relay
> answer `401`, which looks exactly like a code bug and is not one. Check the
> timestamp before debugging anything else.

### 3. Build

```bash
./tools/build.sh          # Linux / macOS
.\tools\build.ps1         # Windows (PowerShell)
```

First build takes several minutes; later builds are much faster.

Output: `build/bk7258/.../package/all-app.bin`

> Do **not** run the build with `sudo`. It runs the container as your own user,
> and `sudo` makes every generated file root-owned — the next non-sudo build
> then fails with a permission error and needs `sudo rm -rf build/` to recover.

### 4. Flash

Use **Beken's official flashing tool**, `BKFIL`, which ships with the Armino
SDK documentation. It is a Windows GUI tool: select the chip, pick
`all-app.bin`, choose your serial port, and flash.

Flashing needs a **hardware reset** — hold the RST button, start the flash,
release when the tool reports it is connecting.

> **On Linux/macOS**, Beken's tool is not available. The engineer who developed
> this example used [`ltchiptool`](https://github.com/libretiny-eu/ltchiptool)
> to flash and [`picocom`](https://github.com/npat-efault/picocom) for the
> serial console. Neither is officially supported by Beken — if flashing fails
> at high baud rates, drop the rate.

### 5. Watch it boot

Open the serial port at **115200 baud**. A healthy boot:

```
Wi-Fi connected
IP: 192.168.1.42
MQTT connected to Anedya!
WebRTC subsystem ready, waiting for offer...
```

If you get that far, the camera is online and waiting.

### 6. Open the viewer

```bash
cd web
python3 -m http.server 8000
```

Open <http://localhost:8000>, put your **Node ID** and **Platform API Key** into
Settings, and press **Start Handshake**.

See [`web/README.md`](web/README.md) for details on the page and what its
diagnostics mean.

---

## Troubleshooting

**Nothing on serial** — wrong baud (115200) or wrong port. On Linux you may
need to add yourself to the `dialout` group and log back in.

**Wi-Fi never connects** — 5 GHz network, or a typo in `anedya_config.h`. The
board is 2.4 GHz only.

**MQTT never connects** — check that the Node is **preauthorized** in the
console first; an unauthorized device is refused even with correct credentials.
Then check the Device UUID and Connection Key. Either way it looks the same:
the broker closes the connection right after CONNECT.

**Connects, but no video** — usually SDP negotiation or SRTP. Look for
`srtp_out is NULL — sending UNENCRYPTED RTP` in the log.

**Works on the same Wi-Fi, fails across networks** — TURN credentials. Check
the expiry timestamp first.

**Video freezes for a second on busy scenes** — look for
`uvc_stre: Frame buffer overflow` *before* blaming the network. That is the
camera's MJPEG frame exceeding its buffer, so the frame never reaches the
encoder. Nothing was lost in transit.

For deeper diagnosis, verbose instrumentation is controlled by
`ANEDYA_WEBRTC_DEBUG` in `app/ap/src/anedya_cam_webrtc.c` and
`libpeer/src/config.h`.

---

## How it works

| Document | Contents |
|---|---|
| [`docs/PORTING_REPORT.md`](docs/PORTING_REPORT.md) | Architecture, every significant bug and how it was found |
| [`docs/PATCHES.md`](docs/PATCHES.md) | Every change to the Beken SDK, with reasoning |
| [`web/README.md`](web/README.md) | The browser client |

**On the SDK patches:** this example needs a small number of changes to
Beken's SDK — mostly to compile files that ship on disk but were left out of
the build, and to enable DTLS-SRTP. They are documented line by line in
`docs/PATCHES.md`, and carried in a fork so you can see exactly what differs
from upstream.

---

## Repository layout

```
app/                  the firmware application
├── ap/include/anedya_config.h    ← the only file you edit
├── ap/src/                       application source
└── ap/config/                    board and SDK configuration
web/                  browser client
docs/                 architecture and patch documentation
tools/                build scripts
sdk/                  Beken Armino AVDK          (submodule)
libpeer/              WebRTC implementation      (submodule)
```

## Licence

This example is Apache-2.0. It builds on the Beken Armino AVDK (Apache-2.0) and
[libpeer](https://github.com/sepfy/libpeer) (MIT), which in turn uses libsrtp
(BSD-3-Clause), cJSON (MIT) and miniz (MIT). Each component keeps its own
licence file.

# Browser client

`index.html` is the viewer. It creates the WebRTC offer, sends it to the camera
through Anedya's MQTT command channel, and renders the incoming video and audio.

It is a **single self-contained file** — no build step, no `npm install`, no
CDN. Everything it needs is inlined, which also means it works on an air-gapped
network once loaded.

## Running it

It must be served over **HTTP**, not opened as a `file://` URL. Browsers only
grant microphone access and `getUserMedia` to secure contexts, and `file://` is
not one — the page will load but two-way audio will silently not work.

Any static server will do. From this directory:

```bash
python3 -m http.server 8000
```

Then open <http://localhost:8000>.

`localhost` counts as a secure context, so this works without setting up TLS.
If you serve it from another machine by IP, the browser will block microphone
access until you put it behind HTTPS.

## First run

1. Open the page and expand **Settings**
2. Fill in:
   - **Node ID** — the same device UUID you put in `anedya_config.h`
   - **Platform API Key** — from the Anedya console (this is an *account* key,
     not the device connection key)
3. Press **Start Handshake**

Settings are kept in `localStorage`, so you only enter them once per browser.

## What you should see

```
MQTT connected to Anedya!
ICE connectionState: checking
ICE connectionState: connected
inbound-rtp video: packets=… framesDecoded=… frameW=1280 frameH=720
```

Video appears within a couple of seconds of `connected`. Audio is two-way: the
camera's microphone plays through your speakers, and your microphone plays out
of the camera's speaker. The mute buttons control each direction independently.

## Diagnostics built into the page

The log pane prints `inbound-rtp` statistics every 3 seconds. These answer the
question you actually have when something looks wrong:

| Reading | Meaning |
|---|---|
| `packets=0` | Nothing is arriving — network, ICE or SRTP |
| `packets` rising, `framesDecoded=0` | Arriving but undecodable — codec negotiation |
| `pli` climbing | The browser keeps asking for a keyframe it never gets |
| `nack` rising with `retransPackets` rising | Packet loss, being recovered — working as intended |

That distinction between "never arrived" and "arrived but unusable" is not
visible from the device side, which is why it is printed here.

## Notes

- **The browser fetches its own TURN credentials** from Anedya's relay API
  using your API key. It does not use the ones compiled into the firmware —
  those are for the camera. Both ends need working credentials for a call
  across different networks.
- **Chrome and Edge are the tested browsers.** Firefox and Safari negotiate
  H.264 differently and have not been verified.
- The offer is compressed (`deflate-raw` + base64) before being sent, because
  an Anedya command payload is limited to roughly 1 KB and a full SDP is
  several times that.

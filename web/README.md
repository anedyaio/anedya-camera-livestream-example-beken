# Browser client

`index.html` is the viewer. It creates the WebRTC offer, sends it to the camera
through Anedya's MQTT command channel, and renders the incoming video and audio.

It is a **single self-contained file** — no build step, no `npm install`, no
CDN. Everything it needs is inlined, which also means it works on an air-gapped
network once loaded.

## Running it

**Just open `index.html`.** Double-click it, or drag it into a browser window.
No web server, no build step.

`file://` counts as a potentially-trustworthy origin, so microphone access
works and two-way audio is available without any hosting.

> ### ⚠️ Chrome or Edge only
>
> Other browsers have known WebRTC issues with this stream that are outside our
> control — Firefox and Safari negotiate H.264 differently. Use Chrome or Edge.

If you would rather serve it (for example to open it from another machine on
your network), any static server works:

```bash
python3 -m http.server 8000
```

Note that serving it by IP rather than `localhost` puts it on an insecure
origin, and the browser will then block microphone access until it is behind
HTTPS. Opening the file directly avoids that entirely.

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
- **Chrome and Edge only.** Firefox and Safari negotiate H.264 differently and
  have known issues with this stream that are not fixable from our side.
- The offer is compressed (`deflate-raw` + base64) before being sent, because
  an Anedya command payload is limited to roughly 1 KB and a full SDP is
  several times that.

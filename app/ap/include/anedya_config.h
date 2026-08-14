/*
 * ============================================================================
 *
 *   >>>  EDIT THIS FILE BEFORE YOU BUILD.  <<<
 *
 *   Everything you need to change to get this example running on your own
 *   hardware and your own Anedya account is in this one file. Nothing else
 *   needs touching.
 *
 * ============================================================================
 *
 * There are three groups of settings, and you need all three:
 *
 *   1. Wi-Fi        the network the camera joins
 *   2. Anedya       which device this is, and its key
 *   3. TURN         relay credentials, so the stream works across networks
 *
 * Each is explained below. Nothing here is secret to Anedya — these are YOUR
 * values, from YOUR account.
 *
 * ---------------------------------------------------------------------------
 * A NOTE ON COMMITTING THIS FILE
 * ---------------------------------------------------------------------------
 * The values shipped here are placeholders. Once you fill them in you are
 * holding real credentials in a tracked file, so either keep the change local
 * or tell git to ignore it:
 *
 *     git update-index --skip-worktree projects/.../ap/include/anedya_config.h
 *
 * A production device should not compile credentials in at all — it should be
 * provisioned at first boot. This file exists to keep the example to a single
 * step, not as a pattern to copy into a product.
 */

#ifndef ANEDYA_CONFIG_H
#define ANEDYA_CONFIG_H

/* ===========================================================================
 * 1. Wi-Fi
 * ===========================================================================
 * The 2.4 GHz network the camera joins. The BK7258 is 2.4 GHz only — a
 * 5 GHz-only SSID will never associate, which looks like a wrong password.
 */
#define ANEDYA_WIFI_SSID        "YOUR_WIFI_SSID"
#define ANEDYA_WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"

/* ===========================================================================
 * 2. Anedya device
 * ===========================================================================
 *
 * DEVICE UUID  —  this is the PHYSICAL DEVICE ID, not the Node ID
 *
 * A Node has two identifiers and they are not interchangeable:
 *
 *     Physical Device ID   you generate it    -> goes here, in the firmware
 *     Node ID              the console does   -> goes in the browser page
 *
 * The Node ID is issued by the console after the Node exists. Putting it here
 * will not work, and the failure looks like a rejected credential rather than
 * a mixed-up value.
 *
 * You choose the Physical Device ID rather than receiving it. Generate a UUID
 * (v4) first — any online UUID generator will do — then create the Node in the
 * Anedya console using it. Supplying your own avoids colliding with an
 * existing device.
 *
 * A UUID is 32 hexadecimal digits in five hyphen-separated groups of
 * 8-4-4-4-12, so 36 characters in total:
 *
 *     xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
 *     550e8400-e29b-41d4-a716-446655440000    <- shape only, do not use this
 *
 * PREAUTHORIZE THE NODE
 *
 * For this example, mark the Node as preauthorized in the Anedya console.
 * Without it the device is not permitted to connect and the broker refuses the
 * session — even though the UUID and key are both correct. That reads exactly
 * like a wrong credential, so check this before re-checking the values below.
 *
 * CONNECTION KEY
 *
 * Issued by the console alongside the Node. This is the DEVICE's key. It is
 * not the Platform API Key — that one is separate, and goes into the browser
 * page, not here.
 *
 * Both values are verified at MQTT CONNECT. Anything wrong shows up as the
 * broker closing the connection immediately after connecting.
 */
#define ANEDYA_DEVICE_UUID      "YOUR_ANEDYA_DEVICE_UUID"
#define ANEDYA_CONNECTION_KEY   "YOUR_ANEDYA_CONNECTION_KEY"

/* Anedya regional endpoints. Change these only if your account is in a
 * different region than ap-in-1. */
#define ANEDYA_MQTT_HOST        "mqtt.ap-in-1.anedya.io"
#define ANEDYA_MQTT_PORT        8883

/* ===========================================================================
 * 3. TURN relay
 * ===========================================================================
 * WebRTC needs a relay when the two ends cannot reach each other directly
 * (different networks, mobile data, restrictive NAT). Without valid
 * credentials here the stream only works when browser and camera are on the
 * same LAN.
 *
 * Generate a pair in the Anedya console, under RELAYS. That is the intended
 * route. The platform API does the same thing if you would rather script it:
 *
 *     POST https://api.ap-in-1.anedya.io/v1/relay/create
 *
 * >>> THESE EXPIRE. <<<
 *
 * The username's trailing number is a Unix timestamp — the moment the pair
 * stops working. Decode it before you debug anything else: an expired pair
 * makes the TURN server answer with 401, which looks exactly like a broken
 * credential-plumbing bug and is not one. This cost several days during
 * development.
 *
 *     username "aBcDeFgH1234:1786275882"
 *                            ^^^^^^^^^^ expiry, seconds since 1970
 *
 * A real product fetches these at runtime, as the browser page already does.
 * They are compiled in here to keep the example to one step.
 */
#define ANEDYA_TURN_USERNAME    "YOUR_ANEDYA_TURN_USERNAME"
#define ANEDYA_TURN_CREDENTIAL  "YOUR_ANEDYA_TURN_CREDENTIAL"

/* STUN/TURN endpoints.
 *
 * These MUST be the turn1.* hostnames. The older stun.ap-in-1 / turn.ap-in-1
 * names resolve to a host that answers nothing: a Binding Request there simply
 * times out, the device gathers host candidates only, and the connection fails
 * with no useful error. That dead endpoint — not the network, not the code —
 * is why remote connections failed for the entire early bring-up. */
#define ANEDYA_STUN_URL         "stun:turn1.ap-in-1.anedya.io:3478"
#define ANEDYA_TURN_URL         "turn:turn1.ap-in-1.anedya.io:3478"

/* ===========================================================================
 * 4. Video (optional)
 * ===========================================================================
 * 1280x720 is the UVC camera's native mode. Lowering it does not
 * automatically help a struggling link — see docs/PORTING_REPORT.md, where a
 * drop to 640x480 was tried and turned out to be treating a symptom.
 */
#define ANEDYA_VIDEO_WIDTH      1280
#define ANEDYA_VIDEO_HEIGHT     720

#endif /* ANEDYA_CONFIG_H */

#ifndef __ANEDYA_CAM_WEBRTC_H__
#define __ANEDYA_CAM_WEBRTC_H__

/**
 * Initialize the WebRTC subsystem.
 * Creates a PeerConnection, starts the peer loop task,
 * and registers A/V pipeline callbacks.
 * Called after MQTT is connected.
 */
void anedya_cam_webrtc_init(void);

/**
 * Tear down the WebRTC subsystem.
 * Stops media, closes the PeerConnection, and cleans up.
 */
void anedya_cam_webrtc_deinit(void);

/**
 * Handle an incoming WebRTC offer from Anedya MQTT.
 * Parses the SDP, sets remote description, generates the answer,
 * and publishes it back via MQTT.
 *
 * @param offer_json  The raw JSON string: {"sdp": "v=0...", ...}
 * @param len         Length of the JSON string.
 */
void anedya_cam_webrtc_on_offer(const char *offer_json, size_t len);

#endif

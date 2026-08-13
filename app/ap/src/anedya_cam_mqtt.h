#ifndef __ANEDYA_CAM_MQTT_H__
#define __ANEDYA_CAM_MQTT_H__

/**
 * Initialize the MQTT client. Connects to Anedya's MQTT broker
 * with TLS, subscribes to the command topic for WebRTC signaling,
 * and starts the heartbeat task.
 *
 * Should be called after Wi-Fi is connected.
 */
void anedya_cam_mqtt_init(void);

/**
 * Publish a generic message to a topic.
 */
void anedya_cam_mqtt_publish(const char *topic, const char *payload);

/**
 * Publish the WebRTC SDP answer back to Anedya so the browser can
 * retrieve it. Uses the command status update topic.
 *
 * @param answer_json  JSON string: {"sdp": "...", "type": "answer"}
 */
void anedya_cam_mqtt_publish_answer(const char *answer_json);

/**
 * Publish a command status update (success/failure) to Anedya.
 *
 * @param status  "success", "failure", or "processing"
 * @param reason  Optional reason string (for failure), or NULL.
 */
void anedya_cam_mqtt_publish_command_status(const char *status, const char *reason);

#endif

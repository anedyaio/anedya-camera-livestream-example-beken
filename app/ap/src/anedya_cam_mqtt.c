/**
 * @file anedya_cam_mqtt.c
 * @brief Anedya MQTT client for WebRTC signaling and heartbeat.
 *
 * Uses lwIP's built-in MQTT client with altcp TLS (mbedTLS).
 * On connect: subscribes to the Anedya command topic for WebRTC offers.
 * On incoming command: routes to anedya_cam_webrtc_on_offer().
 * Provides APIs to publish the SDP answer and command status updates.
 */

#include "anedya_cam_mqtt.h"
#include "anedya_config.h"
#include <os/os.h>
#include <os/mem.h>
#include <components/log.h>
#include <string.h>
#include <stdio.h>

#include "lwip/apps/mqtt.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/altcp_tls.h"
#include "lwip/tcpip.h"
#include <time/ntp.h>
#include <sys/time.h>

#include "anedya_cam_webrtc.h"
#include "anedya_cam_network.h"   /* anedya_cam_wifi_is_connected() */

/* cJSON for command parsing — available from the webrtc component's third_party */
#include "cJSON.h"

/* ── Anedya Root CA (ECC-256) ─────────────────────────────────────────── */

static const char ANEDYA_ROOT_CA[] =
"-----BEGIN CERTIFICATE-----\n"
"MIICDDCCAbOgAwIBAgITQxd3Dqj4u/74GrImxc0M4EbUvDAKBggqhkjOPQQDAjBL\n"
"MQswCQYDVQQGEwJJTjEQMA4GA1UECBMHR3VqYXJhdDEPMA0GA1UEChMGQW5lZHlh\n"
"MRkwFwYDVQQDExBBbmVkeWEgUm9vdCBDQSAzMB4XDTI0MDEwMTAwMDAwMFoXDTQz\n"
"MTIzMTIzNTk1OVowSzELMAkGA1UEBhMCSU4xEDAOBgNVBAgTB0d1amFyYXQxDzAN\n"
"BgNVBAoTBkFuZWR5YTEZMBcGA1UEAxMQQW5lZHlhIFJvb3QgQ0EgMzBZMBMGByqG\n"
"SM49AgEGCCqGSM49AwEHA0IABKsxf0vpbjShIOIGweak0/meIYS0AmXaujinCjFk\n"
"BFShcaf2MdMeYBPPFwz4p5I8KOCopgshSTUFRCXiiKwgYPKjdjB0MA8GA1UdEwEB\n"
"/wQFMAMBAf8wHQYDVR0OBBYEFNz1PBRXdRsYQNVsd3eYVNdRDcH4MB8GA1UdIwQY\n"
"MBaAFNz1PBRXdRsYQNVsd3eYVNdRDcH4MA4GA1UdDwEB/wQEAwIBhjARBgNVHSAE\n"
"CjAIMAYGBFUdIAAwCgYIKoZIzj0EAwIDRwAwRAIgR/rWSG8+L4XtFLces0JYS7bY\n"
"5NH1diiFk54/E5xmSaICIEYYbhvjrdR0GVLjoay6gFspiRZ7GtDDr9xF91WbsK0P\n"
"-----END CERTIFICATE-----\n";

/* ── Configuration ────────────────────────────────────────────────────── */

#define TAG "anedya-mqtt"
#define LOGI(...) BK_LOGI(TAG, ##__VA_ARGS__)
#define LOGW(...) BK_LOGW(TAG, ##__VA_ARGS__)
#define LOGE(...) BK_LOGE(TAG, ##__VA_ARGS__)
#define LOGD(...) BK_LOGD(TAG, ##__VA_ARGS__)

#define MQTT_BROKER_HOSTNAME ANEDYA_MQTT_HOST
#define MQTT_BROKER_PORT     ANEDYA_MQTT_PORT

/* Device credentials — hardcoded for development */
/* Device identity comes from ap/include/anedya_config.h. */
#define ANEDYA_CONN_KEY      ANEDYA_CONNECTION_KEY

/* Heartbeat interval */
#define HEARTBEAT_INTERVAL_MS  30000

/* Max incoming MQTT payload buffer */
#define MQTT_RX_BUFFER_SIZE    4096

/* ── State ────────────────────────────────────────────────────────────── */

static mqtt_client_t *s_mqtt_client = NULL;

static void mqtt_schedule_reconnect(void);
static void mqtt_start_connect(void);

/* Reconnect backoff bounds — see the reconnection section further down. */
#define MQTT_RECONNECT_MIN_MS 5000
#define MQTT_RECONNECT_MAX_MS 60000
static volatile bool s_reconnect_running = false;
static uint32_t s_reconnect_delay_ms = MQTT_RECONNECT_MIN_MS;
static struct altcp_tls_config *s_tls_config = NULL;
static ip_addr_t s_broker_ip;
static volatile bool s_connected = false;

/* Buffer to accumulate incoming MQTT data across fragments */
static char s_rx_buffer[MQTT_RX_BUFFER_SIZE];
static size_t s_rx_offset = 0;
static char s_current_topic[128];

/* Tracks the active command ID for status updates */
static char s_active_command_id[64] = {0};

/* ── MQTT topic strings ───────────────────────────────────────────────── */

static char s_topic_commands[128];
static char s_topic_heartbeat[128];
static char s_topic_cmd_status[128];

static void build_topics(void)
{
    snprintf(s_topic_commands, sizeof(s_topic_commands),
             "$anedya/device/%s/commands", ANEDYA_DEVICE_UUID);
    snprintf(s_topic_heartbeat, sizeof(s_topic_heartbeat),
             "$anedya/device/%s/heartbeat/json", ANEDYA_DEVICE_UUID);
    snprintf(s_topic_cmd_status, sizeof(s_topic_cmd_status),
             "$anedya/device/%s/commands/updateStatus/json", ANEDYA_DEVICE_UUID);
}

/* ── MQTT callbacks ───────────────────────────────────────────────────── */

static void mqtt_sub_request_cb(void *arg, err_t err)
{
    if (err == ERR_OK)
    {
        LOGI("MQTT subscribe OK\n");
    }
    else
    {
        LOGE("MQTT subscribe failed: %d\n", err);
    }
}

static void mqtt_pub_request_cb(void *arg, err_t err)
{
    if (err != ERR_OK)
    {
        LOGE("MQTT publish failed: %d\n", err);
    }
}

/* anedya_cam_webrtc_on_offer() recreates the PeerConnection (create_peer_connection()
 * -> agent_create() -> udp_socket_open()), which opens a BSD UDP socket — and, like
 * mqtt_connection_cb before it, mqtt_incoming_data_cb runs on the tcpip thread (it's
 * a raw lwIP MQTT client callback), so calling that directly here deadlocks the
 * tcpip thread against itself exactly as it did for anedya_cam_webrtc_init(). Copy
 * the offer data out and hand it to its own thread instead. */
typedef struct
{
    char *offer_data;
    size_t offer_data_len;
} webrtc_offer_task_arg_t;

static void webrtc_offer_task(void *arg)
{
    webrtc_offer_task_arg_t *task_arg = (webrtc_offer_task_arg_t *)arg;

    anedya_cam_webrtc_on_offer(task_arg->offer_data, task_arg->offer_data_len);

    os_free(task_arg->offer_data);
    os_free(task_arg);
    rtos_delete_thread(NULL);
}

/**
 * Called when a new MQTT message begins arriving.
 * We record the topic name so we know how to route the data.
 */
static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len)
{
    LOGD("MQTT incoming: topic=%s len=%lu\n", topic, (unsigned long)tot_len);

    strncpy(s_current_topic, topic, sizeof(s_current_topic) - 1);
    s_current_topic[sizeof(s_current_topic) - 1] = '\0';
    s_rx_offset = 0;  /* reset accumulator for new message */
}

/**
 * Called for each chunk of incoming MQTT data. The lwIP MQTT client may
 * fragment large payloads across multiple calls. We accumulate into
 * s_rx_buffer and process when flags indicate the message is complete.
 */
static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    /* Accumulate data */
    size_t copy_len = len;
    if (s_rx_offset + copy_len >= MQTT_RX_BUFFER_SIZE - 1)
    {
        LOGE("MQTT RX buffer overflow, dropping message\n");
        s_rx_offset = 0;
        return;
    }
    memcpy(s_rx_buffer + s_rx_offset, data, copy_len);
    s_rx_offset += copy_len;

    /* flags bit 0 set = last chunk of this message */
    if (flags & MQTT_DATA_FLAG_LAST)
    {
        s_rx_buffer[s_rx_offset] = '\0';
        LOGI("MQTT message complete (%u bytes) on %s\n",
             (unsigned)s_rx_offset, s_current_topic);

        /* Route based on topic */
        if (strcmp(s_current_topic, s_topic_commands) == 0)
        {
            /* This is an Anedya command — check if it's a WebRTC offer.
             * Anedya command format (see anedya-dev-sdk-esp-idf's
             * _anedya_parse_inbound_command):
             * {"commandId": "...", "command": "webrtc_offer", "datatype": "string", "data": "...", "exp": ...}
             */
            LOGI("Received command, routing to WebRTC handler\n");

            /* Quick parse to extract command name and data */
            cJSON *cmd_root = cJSON_ParseWithLength(s_rx_buffer, s_rx_offset);
            if (cmd_root)
            {
                cJSON *cmd_name = cJSON_GetObjectItemCaseSensitive(cmd_root, "command");
                cJSON *cmd_data = cJSON_GetObjectItemCaseSensitive(cmd_root, "data");
                cJSON *cmd_id   = cJSON_GetObjectItemCaseSensitive(cmd_root, "commandId");

                if (cJSON_IsString(cmd_name) &&
                    strcmp(cmd_name->valuestring, "webrtc_offer") == 0 &&
                    cJSON_IsString(cmd_data))
                {
                    /* Save the command ID for status updates */
                    if (cJSON_IsString(cmd_id))
                    {
                        strncpy(s_active_command_id, cmd_id->valuestring,
                                sizeof(s_active_command_id) - 1);
                    }

                    LOGI("WebRTC offer command received (cmdId: %s)\n",
                         s_active_command_id);

                    /* Copy the offer data out (cJSON_Delete below frees the
                     * string this points into) and dispatch to its own
                     * thread — see webrtc_offer_task's comment. */
                    size_t offer_len = strlen(cmd_data->valuestring);
                    char *offer_copy = os_malloc(offer_len + 1);
                    if (offer_copy)
                    {
                        memcpy(offer_copy, cmd_data->valuestring, offer_len + 1);

                        webrtc_offer_task_arg_t *task_arg = os_malloc(sizeof(webrtc_offer_task_arg_t));
                        if (task_arg)
                        {
                            task_arg->offer_data = offer_copy;
                            task_arg->offer_data_len = offer_len;
                            /* 8192 stack-overflowed in testing (__stack_chk_fail):
                             * this thread runs the whole offer pipeline — cJSON
                             * parsing, SDP parsing, and STUN/TURN gathering, whose
                             * agent_create_stun_addr()/agent_create_turn_addr() each
                             * carry two ~600-byte StunMessage locals. Sized with
                             * real headroom instead of nudging it crash-by-crash. */
                            if (rtos_create_thread(NULL, 4, "webrtc_offer",
                                                   (beken_thread_function_t)webrtc_offer_task,
                                                   20480, task_arg) != BK_OK)
                            {
                                LOGE("Failed to create webrtc_offer task\n");
                                os_free(offer_copy);
                                os_free(task_arg);
                            }
                        }
                        else
                        {
                            LOGE("Out of memory for offer task arg\n");
                            os_free(offer_copy);
                        }
                    }
                    else
                    {
                        LOGE("Out of memory copying offer data\n");
                    }
                }
                else
                {
                    LOGD("Ignoring non-webrtc command: %s\n",
                         cJSON_IsString(cmd_name) ? cmd_name->valuestring : "(null)");
                }

                cJSON_Delete(cmd_root);
            }
            else
            {
                LOGE("Failed to parse command JSON\n");
            }
        }

        s_rx_offset = 0;
    }
}

/* ── Heartbeat task ───────────────────────────────────────────────────── */

/* mqtt_publish() and mqtt_client_is_connected() are raw lwIP core API — both
 * start with LWIP_ASSERT_CORE_LOCKED() in mqtt.c, meaning lwIP's own contract
 * is that they may only be called while holding the tcpip core lock (i.e.
 * from the tcpip thread itself, or from another thread that has explicitly
 * taken it). This port enables LWIP_TCPIP_CORE_LOCKING but never overrides
 * LWIP_ASSERT_CORE_LOCKED from its default no-op (see lwip/opt.h), so a
 * violation here produces no warning — it silently races the tcpip thread.
 *
 * And it does: mqtt_close() runs ON the tcpip thread on any TCP error/reset/
 * timeout and sets client->conn = NULL right before invoking our disconnect
 * callback. This thread reads that same client state via
 * mqtt_client_is_connected() and then calls mqtt_publish() with no
 * synchronization at all — if a disconnect lands on the tcpip thread in the
 * narrow window around a heartbeat firing, this thread dereferences a pointer
 * mqtt_close() just nulled. That is the mqtt_hb NULL-deref crash: it needs
 * the disconnect and a 30s heartbeat tick to land close together, which is
 * rare on any single attempt but increasingly likely the longer the device
 * runs, matching the "only after ~1h" symptom.
 *
 * LOCK_TCPIP_CORE()/UNLOCK_TCPIP_CORE() is the exact mechanism lwIP's own
 * netconn/socket layer uses internally for this; it is a real mutex here
 * since LWIP_TCPIP_CORE_LOCKING=1. Held across both the connected-check and
 * the publish so nothing can flip conn_state out from under us in between. */
static void mqtt_heartbeat_task(void *arg)
{
    const char *hb_payload = "{\"reqId\":\"hb-bk7258\"}";

    while (1)
    {
        if (s_connected && s_mqtt_client)
        {
            LOCK_TCPIP_CORE();
            if (mqtt_client_is_connected(s_mqtt_client))
            {
                LOGI("Sending heartbeat to Anedya cloud\n");
                mqtt_publish(s_mqtt_client, s_topic_heartbeat, hb_payload,
                             strlen(hb_payload), 1, 0, mqtt_pub_request_cb, NULL);
            }
            UNLOCK_TCPIP_CORE();
        }

        rtos_delay_milliseconds(HEARTBEAT_INTERVAL_MS);
    }
}

/* ── Post-connect task ────────────────────────────────────────────────── */

/* mqtt_connection_cb runs on the tcpip thread (it's invoked straight out of
 * the raw altcp_connect callback chain). create_peer_connection() opens a
 * BSD UDP socket via agent_create()/udp_socket_open(), and lwIP's socket
 * layer blocks the calling thread waiting for the tcpip thread to service
 * it — calling that directly from mqtt_connection_cb deadlocks the tcpip
 * thread against itself. Do the WebRTC init + heartbeat startup from a
 * separate thread instead. */
static void mqtt_post_connect_task(void *arg)
{
    anedya_cam_webrtc_init();

    rtos_create_thread(NULL, 3, "mqtt_hb",
                       (beken_thread_function_t)mqtt_heartbeat_task,
                       4096, NULL);

    rtos_delete_thread(NULL);
}

/* ── Connection callback ──────────────────────────────────────────────── */

static void mqtt_connection_cb(mqtt_client_t *client, void *arg,
                                mqtt_connection_status_t status)
{
    if (status == MQTT_CONNECT_ACCEPTED)
    {
        LOGI("MQTT connected to Anedya!\n");
        s_connected = true;
        /* Successful connect resets the backoff, so the next outage starts
         * retrying promptly again rather than at the previous 60 s ceiling. */
        s_reconnect_delay_ms = MQTT_RECONNECT_MIN_MS;

        /* Set up incoming message callbacks */
        mqtt_set_inpub_callback(client, mqtt_incoming_publish_cb,
                                mqtt_incoming_data_cb, NULL);

        /* Subscribe to the command topic for WebRTC offers */
        LOGI("Subscribing to %s\n", s_topic_commands);
        mqtt_subscribe(client, s_topic_commands, 0, mqtt_sub_request_cb, NULL);

        rtos_create_thread(NULL, 3, "mqtt_postconn",
                           (beken_thread_function_t)mqtt_post_connect_task,
                           4096, NULL);
    }
    else
    {
        /* Covers both "never connected" (bad credentials, broker refused) and
         * "was connected, now dropped" (MQTT_CONNECT_DISCONNECTED). lwIP does
         * not retry on our behalf, so without the schedule below the device
         * stays up with video working and silently never accepts another call. */
        LOGW("MQTT disconnected (status %d), scheduling reconnect\n", status);
        s_connected = false;
        mqtt_schedule_reconnect();
    }
}

/* ── Reconnection ─────────────────────────────────────────────────────────
 *
 * Signalling rides MQTT, so losing the broker means no new calls can be set
 * up — but an already-running WebRTC session keeps streaming, because media
 * flows peer-to-peer and does not touch the broker. So a reconnect must NOT
 * tear down media; it only needs to restore the signalling channel.
 *
 * Exponential backoff from 5 s to 60 s. The broker is a shared service and a
 * device that reconnects in a tight loop during an outage is the thing that
 * keeps it down.
 */
static void mqtt_reconnect_task(void *arg)
{
    (void)arg;

    while (!s_connected)
    {
        LOGI("MQTT reconnect in %u ms\n", (unsigned)s_reconnect_delay_ms);
        rtos_delay_milliseconds(s_reconnect_delay_ms);

        /* Wi-Fi may have gone too. Nothing to do but keep waiting — the
         * Wi-Fi layer has its own retry, and the DNS lookup below would just
         * fail anyway. */
        if (!anedya_cam_wifi_is_connected())
        {
            LOGD("MQTT reconnect: Wi-Fi still down, waiting\n");
            continue;
        }

        if (s_connected)
        {
            break;
        }

        LOGI("MQTT reconnecting...\n");
        /* Re-runs DNS + mqtt_client_connect(). Safe to call repeatedly: the
         * client handle and TLS config are allocated once and reused. */
        mqtt_start_connect();

        /* Give the attempt time to land before deciding it failed; the result
         * arrives asynchronously in mqtt_connection_cb. */
        rtos_delay_milliseconds(5000);

        if (s_reconnect_delay_ms < MQTT_RECONNECT_MAX_MS)
        {
            s_reconnect_delay_ms *= 2;
            if (s_reconnect_delay_ms > MQTT_RECONNECT_MAX_MS)
            {
                s_reconnect_delay_ms = MQTT_RECONNECT_MAX_MS;
            }
        }
    }

    LOGI("MQTT reconnect loop exiting (connected=%d)\n", (int)s_connected);
    s_reconnect_running = false;
    rtos_delete_thread(NULL);
}

static void mqtt_schedule_reconnect(void)
{
    /* One loop at a time. mqtt_connection_cb can fire repeatedly — every
     * failed attempt produces another callback — and each spawning its own
     * task would multiply the retry rate exactly when the broker is least able
     * to cope with it. */
    if (s_reconnect_running)
    {
        return;
    }

    s_reconnect_running = true;

    /* Own task: this is called from mqtt_connection_cb, which runs on the
     * tcpip thread. Sleeping or reconnecting there would block all of lwIP. */
    if (rtos_create_thread(NULL, 3, "mqtt_recon",
                           (beken_thread_function_t)mqtt_reconnect_task,
                           4096, NULL) != kNoErr)
    {
        LOGE("failed to start MQTT reconnect task\n");
        s_reconnect_running = false;
    }
}

/* ── DNS callback ─────────────────────────────────────────────────────── */

static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    if (ipaddr == NULL)
    {
        LOGE("DNS resolution for %s failed\n", name);
        return;
    }

    LOGI("DNS resolved %s -> %s\n", name, ipaddr_ntoa(ipaddr));
    s_broker_ip = *ipaddr;

    /* Connect to MQTT broker */
    static struct mqtt_connect_client_info_t client_info;
    memset(&client_info, 0, sizeof(client_info));
    client_info.client_id = ANEDYA_DEVICE_UUID;
    client_info.client_user = ANEDYA_DEVICE_UUID;
    client_info.client_pass = ANEDYA_CONN_KEY;
    client_info.keep_alive = 60;
#if LWIP_ALTCP && LWIP_ALTCP_TLS
    client_info.tls_config = s_tls_config;
#endif

    LOGI("Connecting MQTT to %s:%d\n", ipaddr_ntoa(&s_broker_ip), MQTT_BROKER_PORT);
    err_t err = mqtt_client_connect(s_mqtt_client, &s_broker_ip, MQTT_BROKER_PORT,
                                     mqtt_connection_cb, NULL, &client_info);
    if (err != ERR_OK)
    {
        LOGE("mqtt_client_connect failed: %d\n", err);
    }
}

/* ── Init task ────────────────────────────────────────────────────────── */

/* DNS lookup + connect. Split out of mqtt_init_task() so the reconnect path
 * can re-run exactly this part without redoing NTP or reallocating the client
 * and TLS config. */
static void mqtt_start_connect(void)
{
    /* Force DNS to Google to avoid issues if DHCP didn't provide one */
    ip_addr_t dns_server;
    if (ipaddr_aton("8.8.8.8", &dns_server))
    {
        dns_setserver(0, &dns_server);
    }

    /* Resolve broker hostname */
    LOGI("Resolving %s...\n", MQTT_BROKER_HOSTNAME);
    err_t err = dns_gethostbyname(MQTT_BROKER_HOSTNAME, &s_broker_ip, dns_found_cb, NULL);

    if (err == ERR_OK)
    {
        /* Already in cache */
        dns_found_cb(MQTT_BROKER_HOSTNAME, &s_broker_ip, NULL);
    }
    else if (err != ERR_INPROGRESS)
    {
        LOGE("dns_gethostbyname failed: %d\n", err);
    }

}

static void mqtt_init_task(void *arg)
{
    /* Sync NTP first for TLS certificate validation */
    LOGI("Syncing NTP...\n");
    ntp_sync_to_rtc();
    LOGI("NTP sync complete\n");

    build_topics();

    /* Create MQTT client. Runs on the "mqtt_init" thread, not tcpip — same
     * core-lock requirement as mqtt_heartbeat_task above; mqtt_client_new()
     * just calloc's the struct, so there's nothing else touching lwIP state
     * concurrently at this point in boot, but lwIP's own contract still
     * requires the lock, and we're not the last thread this file will grow. */
    if (s_mqtt_client == NULL)
    {
        LOCK_TCPIP_CORE();
        s_mqtt_client = mqtt_client_new();
        UNLOCK_TCPIP_CORE();
        if (s_mqtt_client == NULL)
        {
            LOGE("Failed to allocate MQTT client\n");
            rtos_delete_thread(NULL);
            return;
        }
    }

    /* Create TLS config with Anedya root CA */
    if (s_tls_config == NULL)
    {
        s_tls_config = altcp_tls_create_config_client(
            (const u8_t *)ANEDYA_ROOT_CA, sizeof(ANEDYA_ROOT_CA));
        if (s_tls_config == NULL)
        {
            LOGE("Failed to create TLS config\n");
            rtos_delete_thread(NULL);
            return;
        }
    }

    mqtt_start_connect();

    rtos_delete_thread(NULL);
}

/* ── Public API ───────────────────────────────────────────────────────── */

void anedya_cam_mqtt_init(void)
{
    rtos_create_thread(NULL, 4, "mqtt_init",
                       (beken_thread_function_t)mqtt_init_task,
                       6144, NULL);
}

void anedya_cam_mqtt_publish(const char *topic, const char *payload)
{
    /* Called from the webrtc_offer_task thread, not the tcpip thread — the
     * same core-lock requirement applies here as in mqtt_heartbeat_task
     * above (see that comment for why). This is what publishes the SDP
     * answer and command-status updates, so an unsynchronized race here can
     * corrupt state mid-handshake, not just during an idle heartbeat. */
    if (!s_connected || !s_mqtt_client)
    {
        LOGE("Cannot publish: MQTT not connected\n");
        return;
    }

    LOCK_TCPIP_CORE();
    if (!mqtt_client_is_connected(s_mqtt_client))
    {
        UNLOCK_TCPIP_CORE();
        LOGE("Cannot publish: MQTT not connected\n");
        return;
    }
    err_t err = mqtt_publish(s_mqtt_client, topic, payload, strlen(payload),
                              1, 0, mqtt_pub_request_cb, NULL);
    UNLOCK_TCPIP_CORE();

    if (err != ERR_OK)
    {
        LOGE("mqtt_publish failed: %d\n", err);
    }
}

void anedya_cam_mqtt_publish_answer(const char *answer_b64)
{
    if (s_active_command_id[0] == '\0')
    {
        LOGE("No active command to publish answer for\n");
        return;
    }

    /* Build the status update payload (matches anedya-dev-sdk-esp-idf's
     * anedya_op_cmd_status_update):
     * {"reqId":"<uuid>","commandId":"<commandId>","status":"processing","ackdata":"<base64>","ackdatatype":"string"}
     * answer_b64 is base64 text (alphabet has no JSON-special chars), so it
     * can be embedded directly inside quotes with no further escaping. */
    size_t answer_len = strlen(answer_b64);
    size_t buf_size = answer_len + 256;
    char *payload = os_malloc(buf_size);
    if (!payload)
    {
        LOGE("Out of memory for answer payload\n");
        return;
    }

    snprintf(payload, buf_size,
             "{\"reqId\":\"ans-1\",\"commandId\":\"%s\","
             "\"status\":\"processing\","
             "\"ackdata\":\"%s\","
             "\"ackdatatype\":\"string\"}",
             s_active_command_id, answer_b64);

    LOGI("Publishing SDP answer (%u bytes)\n", (unsigned)strlen(payload));
    anedya_cam_mqtt_publish(s_topic_cmd_status, payload);

    os_free(payload);
}

void anedya_cam_mqtt_publish_command_status(const char *status, const char *reason)
{
    if (s_active_command_id[0] == '\0')
    {
        LOGW("No active command for status update\n");
        return;
    }

    char payload[256];
    if (reason)
    {
        snprintf(payload, sizeof(payload),
                 "{\"reqId\":\"st-1\",\"commandId\":\"%s\","
                 "\"status\":\"%s\","
                 "\"ackdata\":\"%s\",\"ackdatatype\":\"string\"}",
                 s_active_command_id, status, reason);
    }
    else
    {
        snprintf(payload, sizeof(payload),
                 "{\"reqId\":\"st-1\",\"commandId\":\"%s\","
                 "\"status\":\"%s\"}",
                 s_active_command_id, status);
    }

    LOGI("Command status: %s (cmdId: %s)\n", status, s_active_command_id);
    anedya_cam_mqtt_publish(s_topic_cmd_status, payload);

    /* Clear active command on terminal statuses */
    if (strcmp(status, "success") == 0 || strcmp(status, "failure") == 0)
    {
        s_active_command_id[0] = '\0';
    }
}

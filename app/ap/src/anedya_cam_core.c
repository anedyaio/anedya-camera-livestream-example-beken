#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <components/shell_task.h>

#include <modules/wifi.h>
#include <components/event.h>
#include <components/netif.h>

#include "anedya_cam_comm.h"
#include "anedya_cam_network.h"
#include "anedya_cam_devices.h"
#include "anedya_cam_boarding.h"
#include "anedya_cam_mqtt.h"
#include "anedya_cam_webrtc.h"
#include "anedya_config.h"

#define LOGI(...) BK_LOGI(TAG, ##__VA_ARGS__)
#define LOGW(...) BK_LOGW(TAG, ##__VA_ARGS__)
#define LOGE(...) BK_LOGE(TAG, ##__VA_ARGS__)
#define LOGD(...) BK_LOGD(TAG, ##__VA_ARGS__)
#define LOGV(...) BK_LOGV(TAG, ##__VA_ARGS__)

#define TAG "anedya-core"

typedef struct
{
    uint32_t enabled : 1;
    uint32_t reserved : 31;

    beken_thread_t thd;
    beken_queue_t queue;
} anedya_cam_info_t;

static anedya_cam_info_t *g_cam_info = NULL;

/* ── Wi-Fi reconnection ───────────────────────────────────────────────────
 *
 * The SDK reports the drop and stops there; nothing retries on our behalf, so
 * without this a camera that loses its AP for a moment stays offline until
 * someone power-cycles it. That is the single most likely way an unattended
 * device silently disappears.
 *
 * Backoff from 5 s to 60 s. A device hammering association attempts at full
 * rate is both pointless and antisocial on a congested 2.4 GHz band.
 */
#define WIFI_RECONNECT_MIN_MS 5000
#define WIFI_RECONNECT_MAX_MS 60000

static volatile bool s_wifi_reconnect_running = false;

static void wifi_reconnect_task(void *arg)
{
    uint32_t delay_ms = WIFI_RECONNECT_MIN_MS;

    while (!anedya_cam_wifi_is_connected())
    {
        LOGI("Wi-Fi reconnect in %u ms\n", (unsigned)delay_ms);
        rtos_delay_milliseconds(delay_ms);

        if (anedya_cam_wifi_is_connected())
        {
            break;
        }

        LOGI("Wi-Fi reconnecting to %s...\n", ANEDYA_WIFI_SSID);
        anedya_cam_wifi_sta_connect(ANEDYA_WIFI_SSID, ANEDYA_WIFI_PASSWORD);

        /* Association plus DHCP takes a few seconds; success arrives as a
         * DBEVT_WIFI_STATION_CONNECTED event, not as a return value here. */
        rtos_delay_milliseconds(10000);

        if (delay_ms < WIFI_RECONNECT_MAX_MS)
        {
            delay_ms *= 2;
            if (delay_ms > WIFI_RECONNECT_MAX_MS)
            {
                delay_ms = WIFI_RECONNECT_MAX_MS;
            }
        }
    }

    LOGI("Wi-Fi reconnect loop exiting (connected)\n");
    s_wifi_reconnect_running = false;
    rtos_delete_thread(NULL);
}

static void wifi_schedule_reconnect(void)
{
    /* One loop at a time — the SDK can emit several disconnect events for a
     * single drop, and each spawning its own task would multiply the retry
     * rate. */
    if (s_wifi_reconnect_running)
    {
        return;
    }

    s_wifi_reconnect_running = true;

    if (rtos_create_thread(NULL, BEKEN_DEFAULT_WORKER_PRIORITY, "wifi_recon",
                           (beken_thread_function_t)wifi_reconnect_task,
                           4096, NULL) != kNoErr)
    {
        LOGE("failed to start Wi-Fi reconnect task\n");
        s_wifi_reconnect_running = false;
    }
}

/* doorbell_core.c (the reference this project was adapted from) defines and
 * assigns doorbell_current_service to switch between service "modes" (e.g.
 * its CS2 chime/relay service). anedya_cam_devices.c's audio_turn_on/off
 * notify whatever service is "current" via this pointer, but this project
 * only uses the WebRTC audio/video path, not that service-switching
 * mechanism, so it's never assigned — left NULL. Its two call sites already
 * null-check it, so this just needs to exist to satisfy the linker. */
const anedya_cam_service_interface_t *anedya_cam_current_service = NULL;

bk_err_t anedya_cam_send_msg(anedya_cam_msg_t *msg)
{
    bk_err_t ret = BK_OK;

    if (g_cam_info->queue)
    {
        ret = rtos_push_to_queue(&g_cam_info->queue, msg, BEKEN_NO_WAIT);

        if (BK_OK != ret)
        {
            LOGE("%s failed\n", __func__);
            return BK_FAIL;
        }

        return ret;
    }

    return ret;
}

static void anedya_cam_message_handle(void)
{
    bk_err_t ret = BK_OK;
    anedya_cam_msg_t msg;

    while (1)
    {
        ret = rtos_pop_from_queue(&g_cam_info->queue, &msg, BEKEN_WAIT_FOREVER);

        if (kNoErr == ret)
        {
            LOGD("event: %d\n", msg.event);
            switch (msg.event)
            {
                case DBEVT_WIFI_STATION_CONNECTED:
                {
                    LOGI("Wi-Fi connected\n");

                    netif_ip4_config_t ip4_config;
                    extern uint32_t uap_ip_is_start(void);

                    os_memset(&ip4_config, 0x0, sizeof(netif_ip4_config_t));

                    if (uap_ip_is_start())
                    {
                        bk_netif_get_ip4_config(NETIF_IF_AP, &ip4_config);
                    }
                    else
                    {
                        bk_netif_get_ip4_config(NETIF_IF_STA, &ip4_config);
                    }

                    LOGI("IP: %s\n", ip4_config.ip);

                    /* Also runs on every RECONNECT, not just first boot.
                     * anedya_cam_mqtt_init() is written to be re-entrant: the
                     * client handle and TLS config are allocated once and
                     * reused, so this re-resolves DNS and reconnects rather
                     * than leaking a second client. */
                    anedya_cam_mqtt_init();
                }
                break;

                case DBEVT_WIFI_STATION_DISCONNECTED:
                {
                    LOGW("Wi-Fi disconnected\n");

                    /* Tear the WebRTC session down rather than leaving it to
                     * time out. Its sockets are bound to an interface that no
                     * longer has an address, so every send fails; without this
                     * the peer loop spins on errors until the ICE keepalive
                     * eventually gives up, and the camera stays powered on the
                     * whole time. Signalling recovers by itself once Wi-Fi and
                     * MQTT are back — the browser simply sends a new offer. */
                    anedya_cam_webrtc_deinit();

                    /* Retry on its own task: this handler runs on the core
                     * message queue, and blocking here would stall every other
                     * event, including the CONNECTED one we are waiting for. */
                    wifi_schedule_reconnect();
                }
                break;

                case DBEVT_EXIT:
                    goto exit;
                    break;

                default:
                    LOGD("Unhandled event: %d\n", msg.event);
                    break;
            }
        }
    }

exit:

    /* delete msg queue */
    ret = rtos_deinit_queue(&g_cam_info->queue);

    if (ret != kNoErr)
    {
        LOGE("delete message queue fail\n");
    }

    g_cam_info->queue = NULL;

    LOGE("delete message queue complete\n");

    /* delete task */
    rtos_delete_thread(NULL);

    g_cam_info->thd = NULL;

    LOGE("task exit complete\n");
}


void anedya_cam_core_init(void)
{
    bk_err_t ret = BK_OK;

    if (g_cam_info == NULL)
    {
        g_cam_info = os_malloc(sizeof(anedya_cam_info_t));

        if (g_cam_info == NULL)
        {
            LOGE("%s, malloc failed\n", __func__);
            goto error;
        }

        os_memset(g_cam_info, 0, sizeof(anedya_cam_info_t));
    }


    if (g_cam_info->queue != NULL)
    {
        ret = BK_FAIL;
        LOGE("%s, queue already init\n", __func__);
        goto error;
    }

    if (g_cam_info->thd != NULL)
    {
        ret = BK_FAIL;
        LOGE("%s, thread already init\n", __func__);
        goto error;
    }

    ret = rtos_init_queue(&g_cam_info->queue,
                          "cam_queue",
                          sizeof(anedya_cam_msg_t),
                          10);

    if (ret != BK_OK)
    {
        LOGE("%s, create message queue failed\n");
        goto error;
    }

    ret = rtos_create_thread(&g_cam_info->thd,
                             BEKEN_DEFAULT_WORKER_PRIORITY,
                             "cam_core",
                             (beken_thread_function_t)anedya_cam_message_handle,
                             1024 * 6,
                             NULL);

    if (ret != BK_OK)
    {
        LOGE("create core thread fail\n");
        goto error;
    }

    /* Initialize media device subsystem (camera, audio) */
    anedya_cam_devices_init();

    /* Wi-Fi credentials live in ap/include/anedya_config.h — that is the only
     * file you need to edit to run this example. */
    anedya_cam_wifi_sta_connect(ANEDYA_WIFI_SSID, ANEDYA_WIFI_PASSWORD);
 
    g_cam_info->enabled = BK_TRUE;

    LOGI("%s success\n", __func__);

    return;

error:

    LOGE("%s fail\n", __func__);
}

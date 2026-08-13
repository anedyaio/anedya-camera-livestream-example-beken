#include <common/bk_include.h>
#include <os/mem.h>
#include <os/str.h>
#include <os/os.h>

#if CONFIG_VOICE_SERVICE
#include <components/bk_voice_service.h>
#include <components/bk_voice_service_types.h>
#include <components/bk_voice_read_service.h>
#include <components/bk_voice_read_service_types.h>
#include <components/bk_voice_write_service.h>
#include <components/bk_voice_write_service_types.h>
#endif

#include <driver/dvp_camera_types.h>
#include <driver/lcd.h>

#include "anedya_cam_comm.h"
#include "anedya_cam_transmission.h"
#include "anedya_cam_cmd.h"
#include "anedya_cam_devices.h"

#include "wifi_transfer.h"
#include "media_app.h"
#include "camera_handle_list.h"
#include "img_service.h"

#include "uvc_pipeline_act.h"
#include "lcd_display_service.h"
#include "media_utils.h"

#define TAG "db-device"

#define LOGI(...) BK_LOGW(TAG, ##__VA_ARGS__)
#define LOGW(...) BK_LOGW(TAG, ##__VA_ARGS__)
#define LOGE(...) BK_LOGE(TAG, ##__VA_ARGS__)
#define LOGD(...) BK_LOGD(TAG, ##__VA_ARGS__)
#define LOGV(...) BK_LOGV(TAG, ##__VA_ARGS__)

#define DB_SAMPLE_RARE_8K (8000)
#define DB_SAMPLE_RARE_16K (16000)

#define CAMERA_DEVICES_REPORT (BK_FALSE)//(BK_TRUE)

typedef enum
{
    LCD_STATUS_CLOSE,
    LCD_STATUS_OPEN,
    LCD_STATUS_UNKNOWN,
} lcd_status_t;

extern const dvp_sensor_config_t **get_sensor_config_devices_list(void);
extern int get_sensor_config_devices_num(void);

extern const anedya_cam_service_interface_t *anedya_cam_current_service;
static camera_type_t curr_cam_type = UNKNOW_CAMERA;


#define DEVICE_RESPONSE_SIZE (ANEDYA_CAM_NETWORK_MAX_SIZE - sizeof(db_evt_head_t))

#define UVC_DEVICE_ID (0xFDF6)

db_device_info_t *db_device_info = NULL;



int anedya_cam_get_ppis(char *ppi, int capability, int size)
{
    int ret = 0;
    strcat(ppi, "[");

    if (capability & PPI_CAP_320X240)
    {
        strcat(ppi, " \"320X240\",");
    }

    if (capability & PPI_CAP_320X480)
    {
        strcat(ppi, " \"320X480\",");
    }

    if (capability & PPI_CAP_480X272)
    {
        strcat(ppi, " \"480X272\",");
    }

    if (capability & PPI_CAP_480X320)
    {
        strcat(ppi, " \"480X320\",");
    }

    if (capability & PPI_CAP_640X480)
    {
        strcat(ppi, " \"640X480\",");
    }

    if (capability & PPI_CAP_480X800)
    {
        strcat(ppi, " \"480X800\",");
    }

    if (capability & PPI_CAP_800X480)
    {
        strcat(ppi, " \"800X480\",");
    }

    if (capability & PPI_CAP_800X600)
    {
        strcat(ppi, " \"800X600\",");
    }

    if (capability & PPI_CAP_864X480)
    {
        strcat(ppi, " \"864X480\",");
    }

    if (capability & PPI_CAP_1024X600)
    {
        strcat(ppi, " \"1024X600\",");
    }

    if (capability & PPI_CAP_1280X720)
    {
        strcat(ppi, " \"1280X720\",");
    }

    if (capability & PPI_CAP_1600X1200)
    {
        strcat(ppi, " \"1600X1200\",");
    }

    if (capability & PPI_CAP_480X480)
    {
        strcat(ppi, " \"480X480\",");
    }

    if (capability & PPI_CAP_720X288)
    {
        strcat(ppi, " \"720X288\",");
    }

    if (capability & PPI_CAP_720X576)
    {
        strcat(ppi, " \"720X576\",");
    }

    if (capability & PPI_CAP_480X854)
    {
        strcat(ppi, " \"480X854\",");
    }

    ret = strlen(ppi);

    ppi[ret - 1] = ']';

    return ret;
}


int anedya_cam_get_supported_camera_devices(int opcode, db_channel_t *channel, anedya_cam_transmission_send_t cb)
{
    db_evt_head_t *evt = os_malloc(sizeof(db_evt_head_t) + DEVICE_RESPONSE_SIZE);
    char *p = (char *)(evt + 1);

    evt->opcode = opcode;
    evt->status = EVT_STATUS_OK;
    evt->flags = EVT_FLAGS_CONTINUE;

    LOGD("DBCMD_GET_CAMERA_SUPPORTED_DEVICES\n");

#if (CAMERA_DEVICES_REPORT == BK_TRUE)

    int ret = 0;
    const dvp_sensor_config_t **sensors = get_sensor_config_devices_list();
    uint32_t i, size = get_sensor_config_devices_num();


    for (i = 0; i < size; i++)
    {
        char ppi[500] = {0};

        ret = anedya_cam_get_ppis(ppi, sensors[i]->ppi_cap, sizeof(ppi));

        if (ret >= sizeof(ppi))
        {
            LOGE("anedya_cam_camera_get_ppis overflow\n");
        }

        os_memset(p, 0, DEVICE_RESPONSE_SIZE);

        LOGV("sensor: %s, ppi: %uX%u\n", sensors[i]->name,
             ppi_to_pixel_x(sensors[i]->def_ppi),
             ppi_to_pixel_y(sensors[i]->def_ppi));
        sprintf(p, "{\"name\": \"%s\", \"id\": \"%d\", \"type\": \"DVP\", \"ppi\": %s}",
                sensors[i]->name,
                sensors[i]->id,
                ppi);

        LOGD("dump: %s\n", p);

        evt->length = CHECK_ENDIAN_UINT16(strlen(p));
        anedya_cam_transmission_pack_send(channel, (uint8_t *)evt, sizeof(db_evt_head_t) + evt->length, cb);
    }

#else
    os_memset(p, 0, DEVICE_RESPONSE_SIZE);

    sprintf(p, "{\"name\": \"%s\", \"id\": \"%d\", \"type\": \"DVP\", \"ppi\":[\"%uX%u\"]}",
            "DVP",
            1,
            ppi_to_pixel_x(0),
            ppi_to_pixel_y(0));
    evt->length = CHECK_ENDIAN_UINT16(strlen(p));
    anedya_cam_transmission_pack_send(channel, (uint8_t *)evt, sizeof(db_evt_head_t) + evt->length, cb);


#endif
    os_memset(p, 0, DEVICE_RESPONSE_SIZE);

    sprintf(p, "{\"name\": \"%s\", \"id\": \"%d\", \"type\": \"UVC\", \"ppi\":[\"%uX%u\"]}",
            "UVC",
            UVC_DEVICE_ID,
            ppi_to_pixel_x(0),
            ppi_to_pixel_y(0));
    evt->length = CHECK_ENDIAN_UINT16(strlen(p));
    evt->flags = EVT_FLAGS_COMPLETE;
    anedya_cam_transmission_pack_send(channel, (uint8_t *)evt, sizeof(db_evt_head_t) + evt->length, cb);

    os_free(evt);

    return 0;
}

int anedya_cam_get_supported_lcd_devices(int opcode, db_channel_t *channel, anedya_cam_transmission_send_t cb)
{
    uint32_t i, size;
    size = get_lcd_devices_num();//media_app_get_lcd_devices_num();
    const lcd_device_t **device = get_lcd_devices_list();//media_app_get_lcd_devices_list();
    db_evt_head_t *evt = os_malloc(sizeof(db_evt_head_t) + DEVICE_RESPONSE_SIZE);
    char *p = (char *)(evt + 1);

    evt->opcode = opcode;
    evt->status = EVT_STATUS_OK;
    evt->flags = EVT_FLAGS_CONTINUE;

    LOGD("DBCMD_GET_LCD_SUPPORTED_DEVICES\n");

    if ((uint32_t)device != kGeneralErr && device != NULL)
    {
        for (i = 0; i < size; i++)
        {
            os_memset(p, 0, DEVICE_RESPONSE_SIZE);

            LOGV("lcd: %s, ppi: %uX%u\n", device[i]->name,
                 ppi_to_pixel_x(device[i]->ppi),
                 ppi_to_pixel_y(device[i]->ppi));
            sprintf(p, "{\"name\": \"%s\", \"id\": \"%d\", \"type\": \"%s\", \"ppi\":\"%uX%u\"}",
                    device[i]->name,
                    device[i]->id,
                    device[i]->type == LCD_TYPE_RGB565 ? "rgb" : "mcu",
                    ppi_to_pixel_x(device[i]->ppi),
                    ppi_to_pixel_y(device[i]->ppi));

            LOGD("dump: %s\n", p);

            evt->length = CHECK_ENDIAN_UINT16(strlen(p));

            if (i == size - 1)
            {
                evt->flags = EVT_FLAGS_COMPLETE;
            }

            anedya_cam_transmission_pack_send(channel, (uint8_t *)evt, sizeof(db_evt_head_t) + evt->length, cb);
        }
    }

    os_free(evt);

    return 0;
}

int anedya_cam_get_lcd_status(int opcode, db_channel_t *channel, anedya_cam_transmission_send_t cb)
{
    uint32_t lcd_status = media_app_get_lcd_status();
    db_evt_head_t *evt = os_malloc(sizeof(db_evt_head_t) + DEVICE_RESPONSE_SIZE);
    char *p = (char *)(evt + 1);

    evt->opcode = opcode;
    evt->status = EVT_STATUS_OK;
    evt->flags = EVT_FLAGS_CONTINUE;

    LOGD("DBCMD_GET_LCD_STATUS\n");
    os_memset(p, 0, DEVICE_RESPONSE_SIZE);

    if (lcd_status != LCD_STATUS_CLOSE && lcd_status != LCD_STATUS_OPEN)
    {
        lcd_status = LCD_STATUS_UNKNOWN;
    }
    sprintf(p, "{\"status\": \"%u\"}", lcd_status);
    LOGD("dump: %s\n", p);
    evt->length = CHECK_ENDIAN_UINT16(strlen(p));

    evt->flags = EVT_FLAGS_COMPLETE;

    anedya_cam_transmission_pack_send(channel, (uint8_t *)evt, sizeof(db_evt_head_t) + evt->length, cb);

    os_free(evt);

    return 0;
}

int anedya_cam_devices_set_camera_transfer_callback(void *cb)
{
    if (db_device_info == NULL)
    {
        LOGE("db_device_info null");
        return  BK_FAIL;
    }

    db_device_info->camera_transfer_cb = (media_transfer_cb_t *)cb;

    return BK_OK;
}

int anedya_cam_devices_set_audio_transfer_callback(const void *cb)
{
    if (db_device_info == NULL)
    {
        LOGE("db_device_info null");
        return  BK_FAIL;
    }

    db_device_info->audio_transfer_cb = (const media_transfer_cb_t *)cb;

    return BK_OK;
}

int anedya_cam_camera_turn_on(camera_parameters_t *parameters)
{
    bk_err_t ret = BK_FAIL;
    uint8_t rot_angle = 0;
    media_camera_device_t device = {0};

    LOGD("%s, id: %d, %d X %d, format: %d, Protocol: %d\n", __func__,
         parameters->id, parameters->width, parameters->height,
         parameters->format, parameters->protocol);

    if (db_device_info->video_handle != NULL)
    {
        LOGD("%s, id: %d already open\n", __func__, parameters->id);
        return EVT_STATUS_ALREADY;
    }

    if (parameters->id == UVC_DEVICE_ID)
    {
        device.type = UVC_CAMERA;
        device.port = 1;
        db_device_info->camera_id = 1;
    }
    else
    {
        device.type = DVP_CAMERA;
        device.port = 0;
        db_device_info->camera_id = 0;
    }

    if (parameters->format == 0) // wifi transfer format 0/1:mjpeg/h264
    {
        device.format = IMAGE_MJPEG;
        if (device.type == DVP_CAMERA)
        {
            device.format = IMAGE_YUV | IMAGE_MJPEG;
        }

        db_device_info->h264_transfer = false;
    }
    else
    {
        if (device.type == DVP_CAMERA)
        {
            device.format = IMAGE_YUV | IMAGE_H264;
            db_device_info->pipeline_enable = false;
        }
        else
        {
            device.format = IMAGE_MJPEG;// uvc output mjpeg(not h264 stream)
            db_device_info->pipeline_enable = true;
        }
        db_device_info->h264_transfer = true;
    }

    LOGD("%s, device:fmt:%d, transfer:%s\n", __func__, device.format, db_device_info->h264_transfer ? "h264" : "mjpeg");
    device.width = parameters->width;
    device.height = parameters->height;
    device.fps = FPS30;

    // if camera already opened and transfer h264, need to regenerate idr
    if (db_device_info->pipeline_enable)
    {
        if (media_app_h264_regenerate_idr(device.type) != BK_OK)
        {
            LOGE("%s h264_regenerate_idr failed\n", __func__);
        }
    }

    ret = media_app_camera_open(&db_device_info->video_handle, &device);
    if (ret != BK_OK)
    {
        LOGE("%s failed\n", __func__);
        return ret;
    }

    // check, output image need rotate or not (for soft jpegdec)
    switch (parameters->rotate)
    {
        case 90:
            rot_angle = ROTATE_90;
            break;
        case 180:
            rot_angle = ROTATE_180;
            break;
        case 270:
            rot_angle = ROTATE_270;
            break;
        case 0:
            rot_angle = ROTATE_NONE;
            break;
        default:
            rot_angle = ROTATE_90;
            break;
    }
    media_app_set_rotate(rot_angle);

    if (db_device_info->pipeline_enable)
    {
        ret = media_app_pipeline_h264_open(NULL);
        if (ret != BK_OK)
        {
            LOGE("%s h264_pipeline_open failed\n", __func__);
            return ret;
        }
    }

    if (check_lcd_task_is_open())
    {
        if (device.type == UVC_CAMERA)
        {
            media_app_jdec_open(JPEGDEC_BY_LINE);
        }
        else if (device.type == DVP_CAMERA)
        {
            media_app_jdec_open(JPEGDEC_BY_FRAME);
        }
    }

    curr_cam_type = device.type;

    return ret;
}

int anedya_cam_camera_turn_off(void)
{
    if (db_device_info->video_handle == NULL)
    {
        LOGD("%s, %d already close\n", __func__);
        return EVT_STATUS_ALREADY;
    }

    //if (db_device_info->pipeline_enable)
    {
        media_app_pipeline_h264_close();
        LOGD("%s h264_pipeline close\n", __func__);
    }

    do
    {
        db_device_info->video_handle = bk_camera_handle_node_pop();
        if (db_device_info->video_handle)
        {
            LOGD("%s, %d, %p\n", __func__, __LINE__, db_device_info->video_handle);
            media_app_camera_close(&db_device_info->video_handle);
        }
        else
        {
            break;
        }
    }
    while (1);

    db_device_info->video_handle = NULL;
    db_device_info->camera_id = CAMERA_MAX_NUM;
    db_device_info->pipeline_enable = false;
    db_device_info->h264_transfer = false;

    return 0;
}

int anedya_cam_video_transfer_turn_on(void)
{
    int ret = -1;

    if (db_device_info->transfer_enable)
    {
        LOGD("%s, id: %d already open\n", __func__, db_device_info->transfer_enable);
        return EVT_STATUS_ALREADY;
    }

    if (db_device_info->camera_transfer_cb)
    {
        if (db_device_info->h264_transfer)
        {
            ret = bk_wifi_transfer_frame_open(db_device_info->camera_transfer_cb, IMAGE_H264);
        }
        else
        {
            ret = bk_wifi_transfer_frame_open(db_device_info->camera_transfer_cb, IMAGE_MJPEG);
        }
    }
    else
    {
        LOGE("media_transfer_cb: NULL\n");
    }

    if (ret == BK_OK)
    {
        db_device_info->transfer_enable = 1;
    }

    return ret;
}

int anedya_cam_video_transfer_turn_off(void)
{
    int ret = -1;

    if (db_device_info->transfer_enable == false)
    {
        LOGD("%s, id: %d already close\n", __func__, db_device_info->transfer_enable);
        return EVT_STATUS_ALREADY;
    }

    ret = bk_wifi_transfer_frame_close();

    /* The doorbell original called anedya_cam_cs2_img_timer_deinit() here under
     * #if (CONFIG_INTEGRATION_ANEDYA_CAM_CS2). That CONFIG is never set in this
     * project and the CS2 service it belonged to has been removed — this port
     * uses WebRTC, not the CS2 P2P transport. */

    db_device_info->transfer_enable = false;

    return ret;
}

int anedya_cam_display_turn_on(uint16_t id, uint16_t rotate, uint16_t fmt)
{
    LOGD("%s, id: %d, rotate: %d fmt: %d\n", __func__, id, rotate, fmt);

    if (db_device_info->lcd_id != 0)
    {
        LOGD("%s, id: %d already open\n", __func__, id);
        return EVT_STATUS_ALREADY;
    }
    const lcd_device_t *device = (const lcd_device_t *)get_lcd_device_by_id(id);
    if ((uint32_t)device == BK_FAIL || device == NULL)
    {
        LOGD("%s, could not find device id: %d\n", __func__, id);
        return EVT_STATUS_ERROR;
    }

    lcd_open_t lcd_open = {0};
    lcd_open.device_ppi = device->ppi;
    lcd_open.device_name = device->name;

    uint8_t rot_angle = 0;
    if (fmt == 0)
    {
        lcd_set_fmt(PIXEL_FMT_RGB565_LE);
    }
    else if (fmt == 1)
    {
        lcd_set_fmt(PIXEL_FMT_RGB888);
    }

    switch (rotate)
    {
        case 90:
            rot_angle = ROTATE_90;
            break;
        case 180:
            rot_angle = ROTATE_180;
            break;
        case 270:
            rot_angle = ROTATE_270;
            break;
        case 0:
        default:
            rot_angle = ROTATE_NONE;
            break;
    }
    media_app_set_rotate(rot_angle);

    if (curr_cam_type == UVC_CAMERA)
    {
        media_app_jdec_open(JPEGDEC_BY_LINE);
    }
    else if (curr_cam_type == DVP_CAMERA)
    {
        media_app_jdec_open(JPEGDEC_BY_FRAME);
    }

    if (media_app_lcd_disp_open(&lcd_open) != BK_OK)
    {
        media_app_jdec_close();
    }

    db_device_info->lcd_id = id;
    return 0;
}

int anedya_cam_display_turn_off(void)
{
    LOGD("%s, id: %d\n", __func__, db_device_info->lcd_id);

    if (db_device_info->lcd_id == 0)
    {
        LOGD("%s, %d already close\n", __func__);
        return EVT_STATUS_ALREADY;
    }

    media_app_jdec_close();
    media_app_lcd_disp_close();
    db_device_info->lcd_id = 0;

    return 0;
}

#if CONFIG_VOICE_SERVICE
int anedya_cam_udp_voice_send_callback(unsigned char *data, unsigned int len, void *args)
{
    if (db_device_info == NULL)
    {
        LOGE("%s, db_device_info NULL\n", __func__);
        return BK_FAIL;
    }

    if (db_device_info->audio_transfer_cb == NULL)
    {
        LOGE("%s, audio_transfer_cb NULL\n", __func__);
        return BK_FAIL;
    }

    if (len > db_device_info->audio_transfer_cb->get_tx_size())
    {
        LOGE("%s, buffer over flow %d %d\n", __func__, len, db_device_info->audio_transfer_cb->get_tx_size());
        return BK_FAIL;
    }

    uint8_t *buffer = db_device_info->audio_transfer_cb->get_tx_buf();

    if (db_device_info->audio_transfer_cb->prepare)
    {
        db_device_info->audio_transfer_cb->prepare(data, len);
    }

    return db_device_info->audio_transfer_cb->send(buffer, len);
}

int anedya_cam_audio_turn_off(void)
{
    if (db_device_info->audio_enable == BK_FALSE)
    {
        LOGD("%s already turn off\n", __func__);

        return BK_FAIL;
    }

    LOGD("%s entry\n", __func__);

    db_device_info->audio_enable = BK_FALSE;

    if (anedya_cam_current_service
        && anedya_cam_current_service->audio_state_changed)
    {
        anedya_cam_current_service->audio_state_changed(DB_TURN_OFF);
    }

    if (db_device_info->voice_read_handle)
    {
        bk_voice_read_stop(db_device_info->voice_read_handle);
    }

    if (db_device_info->voice_write_handle)
    {
        bk_voice_write_stop(db_device_info->voice_write_handle);
    }

    if (db_device_info->voice_handle)
    {
        bk_voice_stop(db_device_info->voice_handle);
    }

    if (db_device_info->voice_read_handle)
    {
        bk_voice_read_deinit(db_device_info->voice_read_handle);
    }

    if (db_device_info->voice_write_handle)
    {
        bk_voice_write_deinit(db_device_info->voice_write_handle);
    }

    if (db_device_info->voice_handle)
    {
        bk_voice_deinit(db_device_info->voice_handle);
    }
    db_device_info->voice_read_handle = NULL;
    db_device_info->voice_write_handle = NULL;
    db_device_info->voice_handle  = NULL;

    LOGD("%s out\n", __func__);
    return BK_OK;
}

bk_err_t anedya_cam_audio_event_handle(vioce_evt_t event, void *param, void *args)
{
    anedya_cam_msg_t msg;

    switch (event)
    {
        case VOC_EVT_MIC_NOT_SUPPORT:
        case VOC_EVT_SPK_NOT_SUPPORT:
        case VOC_EVT_ERROR_UNKNOW:
        case VOC_EVT_STOP:
            LOGD("%s, -->>event: %d\n", __func__, event);
            msg.event = DBEVT_VOICE_EVENT;
            msg.param = event;
            anedya_cam_send_msg(&msg);
            break;

        default:
            break;
    }

    return BK_OK;
}

/* ── Onboard speaker loudness ─────────────────────────────────────────────
 *
 * The SDK's VOICE_BY_ONBOARD_MIC_SPK_CFG_DEFAULT() leaves the speaker at unity
 * with the DRC stage effectively off, which on this EVK's AudioPA is quiet and
 * thin. Ranges below are from the driver headers, not guesswork:
 *
 *   dig_gain  0x00..0x3f  = -45 dB .. +18 dB, 1 dB/step, 0x2d = 0 dB
 *               (onboard_speaker_stream.h; bk_aud_dac_set_gain)
 *   ana_gain  0x00..0x0f  (bk_aud_dac_set_ana_gain; no dB table published)
 *   drc       0x10..0x1f  "the greater the value, the greater the volume"
 *               (aec_v3_algorithm.h)
 *
 * Analog gain is raised before digital: it acts after the DAC, so it lifts the
 * output without risking clipping in the digital path. Digital gain is then
 * added on top with headroom deliberately left over — G.711 decoded from a
 * browser microphone is usually well below full scale, but not always, and
 * clipping sounds far worse than quiet.
 *
 * If it is still too quiet, raise ANEDYA_SPK_DIG_GAIN first (1 dB per step,
 * 0x3f is the ceiling). If it distorts on loud speech, lower that one first —
 * digital gain is the stage that clips. */
#define ANEDYA_SPK_ANA_GAIN   0x08   /* SDK default 0x07; 0x0b was much too hot */
#define ANEDYA_SPK_DIG_GAIN   0x2f   /* SDK default 0x2d (0 dB) — this is +2 dB.
                                      * +9 dB (0x36) overdrove the amp outright;
                                      * +4 dB (0x31) was still uncomfortable in
                                      * the 2-3 kHz region, where both this
                                      * speaker and human hearing are most
                                      * sensitive. */

/* The SDK default is 0x0, which is *below* the documented recommended range of
 * 0x10..0x1f — the DRC/output stage is essentially disabled, and it is the
 * single biggest reason the speaker sounds both quiet and thin. Starting at
 * the bottom of the supported range rather than the top so the effect can be
 * judged before pushing it; DRC raises quiet passages more than loud ones, so
 * too much of it makes background noise audible between words. */
#define ANEDYA_AEC_DRC        0x10   /* was AEC_V3_ALGORITHM_DRC (0x0) */

/* Raising speaker output puts more of it back into the mic, so the echo
 * canceller has more to remove. ec_depth is "the greater the echo, the greater
 * the value setting", range 1..50, SDK default 0xa. Raised in step with the
 * gains above — without this the far end starts hearing itself. */
#define ANEDYA_AEC_EC_DEPTH   0x10   /* was AEC_V3_ALGORITHM_EC_DEPTH (0xa) */

int anedya_cam_audio_turn_on(audio_parameters_t *parameters)
{
    voice_cfg_t *voice_cfg;

    voice_cfg = os_malloc(sizeof(voice_cfg_t));

    if (!voice_cfg)
    {
        LOGD("%s voice_cfg malloc failure!\n", __func__);

        return BK_FAIL;
    }

    if (db_device_info->audio_enable == BK_TRUE)
    {
        LOGD("%s already turn on\n", __func__);

        return BK_FAIL;
    }

    LOGD("%s, AEC: %d, UAC: %d, sample rate: %d, %d, fmt: %d, %d\n", __func__,
         parameters->aec, parameters->uac, parameters->rmt_recorder_sample_rate,
         parameters->rmt_player_sample_rate, parameters->rmt_recoder_fmt, parameters->rmt_player_fmt);

    uint32_t mic_sample_rate = 8000;
    uint32_t spk_sample_rate = 8000;
    switch (parameters->rmt_recorder_sample_rate)
    {
        case DB_SAMPLE_RARE_8K:
            mic_sample_rate = 8000;
            break;

        case DB_SAMPLE_RARE_16K:
            mic_sample_rate = 16000;
            break;

        default:
            mic_sample_rate = 8000;
            break;
    }

    switch (parameters->rmt_player_sample_rate)
    {
        case DB_SAMPLE_RARE_8K:
            spk_sample_rate = 8000;
            break;

        case DB_SAMPLE_RARE_16K:
            spk_sample_rate = 16000;
            break;

        default:
            spk_sample_rate = 8000;
            break;
    }


    if (parameters->uac == 1)
    {
        voice_cfg_t voice_uac = VOICE_BY_UAC_MIC_SPK_CFG_DEFAULT();
        *voice_cfg = voice_uac;
        voice_cfg->mic_cfg.uac_mic_cfg.samp_rate = mic_sample_rate;
        voice_cfg->mic_cfg.uac_mic_cfg.frame_size = mic_sample_rate * 2 * 20 / 1000; //one frame size(20ms)
        voice_cfg->mic_cfg.uac_mic_cfg.out_block_size = voice_cfg->mic_cfg.uac_mic_cfg.frame_size;
        voice_cfg->mic_cfg.uac_mic_cfg.out_block_num = 2;

        voice_cfg->spk_cfg.uac_spk_cfg.samp_rate = spk_sample_rate;
        voice_cfg->spk_cfg.uac_spk_cfg.frame_size = spk_sample_rate * 2 * 20 / 1000; //one frame size(20ms)
    }
    else
    {
        voice_cfg_t voice_onboard = VOICE_BY_ONBOARD_MIC_SPK_CFG_DEFAULT();
        *voice_cfg = voice_onboard;
        voice_cfg->mic_cfg.onboard_mic_cfg.adc_cfg.sample_rate = mic_sample_rate;
        voice_cfg->mic_cfg.onboard_mic_cfg.frame_size = mic_sample_rate * 2 * 20 / 1000; //one frame size(20ms)
        //voice_cfg->mic_cfg.onboard_mic_cfg.out_rb_size = voice_cfg->mic_cfg.onboard_mic_cfg.frame_size;
        voice_cfg->mic_cfg.onboard_mic_cfg.out_block_size = voice_cfg->mic_cfg.onboard_mic_cfg.frame_size;
        voice_cfg->mic_cfg.onboard_mic_cfg.out_block_num = 2;

        voice_cfg->spk_cfg.onboard_spk_cfg.sample_rate = spk_sample_rate;
        voice_cfg->spk_cfg.onboard_spk_cfg.frame_size = spk_sample_rate * 2 * 20 / 1000; //one frame size(20ms)

        /* See the ANEDYA_SPK_* notes above — the SDK defaults leave the
         * onboard speaker at unity and sound quiet on this EVK's AudioPA. */
        voice_cfg->spk_cfg.onboard_spk_cfg.ana_gain = ANEDYA_SPK_ANA_GAIN;
        voice_cfg->spk_cfg.onboard_spk_cfg.dig_gain = ANEDYA_SPK_DIG_GAIN;
    }

    if (parameters->aec == 1)
    {
        voice_cfg->aec_en = true;
        voice_cfg->aec_cfg.aec_alg_cfg.aec_cfg.fs = mic_sample_rate;

        /* The AEC emits one 20 ms frame of 16-bit mono per pass, so its output
         * block and VAD frame must be sized from the sample rate like every
         * other stage. Left at a template's fixed value while the rate changes,
         * the algorithm overruns the block and the device crashes — see the
         * note on the config template selection above. Set explicitly rather
         * than inherited so this cannot silently disagree with mic_sample_rate. */
        voice_cfg->aec_cfg.aec_alg_cfg.out_block_size = mic_sample_rate * 2 * 20 / 1000;
        voice_cfg->aec_cfg.aec_alg_cfg.vad_cfg.vad_frame_size = mic_sample_rate * 2 * 20 / 1000;

        /* DRC ships at 0x0, below its own documented 0x10..0x1f range; and
         * ec_depth has to rise with the speaker gain or the far end hears its
         * own echo. See the ANEDYA_AEC_* notes above. */
        voice_cfg->aec_cfg.aec_alg_cfg.aec_cfg.drc = ANEDYA_AEC_DRC;
        voice_cfg->aec_cfg.aec_alg_cfg.aec_cfg.ec_depth = ANEDYA_AEC_EC_DEPTH;
    }
    else
    {
        voice_cfg->aec_en = false;
    }

    switch (parameters->rmt_recoder_fmt)
    {
        case CODEC_FORMAT_G711A:
        case CODEC_FORMAT_G711U:
        {
            /* g711 encoder config */
            g711_encoder_cfg_t g711_encoder_cfg = DEFAULT_G711_ENCODER_CONFIG();
            voice_cfg->enc_cfg.g711_enc_cfg = g711_encoder_cfg;
            if (parameters->rmt_recoder_fmt == CODEC_FORMAT_G711A)
            {
                voice_cfg->enc_type = AUDIO_ENC_TYPE_G711A;
                voice_cfg->enc_cfg.g711_enc_cfg.enc_mode = G711_ENC_MODE_A_LOW;
            }
            else
            {
                voice_cfg->enc_type = AUDIO_ENC_TYPE_G711U;
                voice_cfg->enc_cfg.g711_enc_cfg.enc_mode = G711_ENC_MODE_U_LOW;
            }
            voice_cfg->enc_cfg.g711_enc_cfg.buf_sz = mic_sample_rate * 2 * 20 / 1000; //one frame size(20ms)
            voice_cfg->enc_cfg.g711_enc_cfg.out_block_size = voice_cfg->enc_cfg.g711_enc_cfg.buf_sz >> 1;
            /* config raw_read input buffer */
            voice_cfg->read_pool_size = voice_cfg->enc_cfg.g711_enc_cfg.out_block_size;

            /* g711 decoder config */
            g711_decoder_cfg_t g711_decoder_cfg = DEFAULT_G711_DECODER_CONFIG();
            voice_cfg->dec_cfg.g711_dec_cfg = g711_decoder_cfg;
            if (parameters->rmt_recoder_fmt == CODEC_FORMAT_G711A)
            {
                voice_cfg->dec_type = AUDIO_DEC_TYPE_G711A;
                voice_cfg->dec_cfg.g711_dec_cfg.dec_mode = G711_DEC_MODE_A_LOW;
            }
            else
            {
                voice_cfg->dec_type = AUDIO_DEC_TYPE_G711U;
                voice_cfg->dec_cfg.g711_dec_cfg.dec_mode = G711_DEC_MODE_U_LOW;
            }
            voice_cfg->dec_cfg.g711_dec_cfg.out_block_size = spk_sample_rate * 2 * 20 / 1000; //one frame size(20ms)
            voice_cfg->dec_cfg.g711_dec_cfg.buf_sz = voice_cfg->dec_cfg.g711_dec_cfg.out_block_size >> 1;
            /* config raw_write output buffer */
            voice_cfg->write_pool_size = voice_cfg->dec_cfg.g711_dec_cfg.buf_sz;
        }
        break;

        case CODEC_FORMAT_PCM:
        {
            /* pcm encoder config */
            voice_cfg->enc_type = AUDIO_ENC_TYPE_PCM;
            voice_cfg->enc_cfg.pcm_enc_cfg = 0;      // not used
            voice_cfg->dec_type = AUDIO_DEC_TYPE_PCM;
            voice_cfg->dec_cfg.pcm_dec_cfg = 0;      //not used

            /* config raw_read input buffer and raw_write output buffer */
            voice_cfg->read_pool_size = mic_sample_rate * 2 * 20 / 1000; //one frame size(20ms)
            voice_cfg->write_pool_size = spk_sample_rate * 2 * 20 / 1000; //one frame size(20ms)
        }
        break;

        default:
        {
            LOGE("not support encoder format\n");
            goto error;
        }
        break;
    }

    //voice_cfg->event_handle = anedya_cam_audio_event_handle; /* close audio event, because sram is not enough */
    voice_cfg->event_handle = NULL;
    voice_cfg->args = NULL;
    db_device_info->voice_handle = bk_voice_init(voice_cfg);
    if (!db_device_info->voice_handle)
    {
        LOGE("voice init fail\n");
        goto error;
    }

    voice_read_cfg_t voice_read_cfg = VOICE_READ_CFG_DEFAULT();
    voice_read_cfg.voice_handle = db_device_info->voice_handle;
    /* Upper bound on ONE read, not a fixed frame size.
     *
     * The read path is a framebuf port (_framebuf_port_read), which returns one
     * whole encoder output frame per call and reports its true length — so this
     * only has to be large enough that a frame is never rejected for exceeding
     * it. That matters for any codec whose frames are variable-length and not
     * self-delimiting: if the transport coalesced them into a byte stream the
     * boundaries would be unrecoverable. G.711A does not care (every byte is a
     * sample), which is why 160 below is an exact 20 ms frame rather than a
     * bound.
     *
     * For G.711A this must instead be EXACTLY 20 ms (160 bytes at 8 kHz):
     * rtp_encoder_encode_generic() advances the timestamp one CONFIG_AUDIO_
     * DURATION step per call regardless of payload size, so a larger read makes
     * the RTP clock run slow. That is the 8x desync described in
     * PORTING_REPORT §3.9 — it was 1280 (160 ms) inherited from the doorbell,
     * whose UDP transport carried no timestamps and did not care. */
    voice_read_cfg.max_read_size = 160;
    voice_read_cfg.voice_read_callback = anedya_cam_udp_voice_send_callback;
    voice_read_cfg.args = NULL;
    voice_read_cfg.task_stack = 1024 * 4;
    voice_read_cfg.mem_type = AUDIO_MEM_TYPE_PSRAM;
    db_device_info->voice_read_handle = bk_voice_read_init(&voice_read_cfg);
    if (!db_device_info->voice_read_handle)
    {
        LOGE("voice read init fail\n");
        goto error;
    }

    voice_write_cfg_t voice_write_cfg = VOICE_WRITE_CFG_DEFAULT();
    voice_write_cfg.voice_handle = db_device_info->voice_handle;
    voice_write_cfg.mem_type = AUDIO_MEM_TYPE_PSRAM;
    db_device_info->voice_write_handle = bk_voice_write_init(&voice_write_cfg);
    if (!db_device_info->voice_write_handle)
    {
        LOGE("voice write init fail\n");
        goto error;
    }

    if (BK_OK != bk_voice_start(db_device_info->voice_handle))
    {
        LOGE("voice start fail\n");
        goto error;
    }

    if (BK_OK != bk_voice_read_start(db_device_info->voice_read_handle))
    {
        LOGE("voice read start fail\n");
        goto error;
    }

    if (BK_OK != bk_voice_write_start(db_device_info->voice_write_handle))
    {
        LOGE("voice write start fail\n");
        goto error;
    }

    db_device_info->audio_enable = BK_TRUE;

    if (anedya_cam_current_service
        && anedya_cam_current_service->audio_state_changed)
    {
        anedya_cam_current_service->audio_state_changed(DB_TURN_ON);
    }

    if (voice_cfg)
    {
        os_free(voice_cfg);
        voice_cfg=NULL;
    }

    return BK_OK;
error:
    if (db_device_info->voice_read_handle)
    {
        bk_voice_read_stop(db_device_info->voice_read_handle);
    }

    if (db_device_info->voice_write_handle)
    {
        bk_voice_write_stop(db_device_info->voice_write_handle);
    }

    if (db_device_info->voice_handle)
    {
        bk_voice_stop(db_device_info->voice_handle);
    }

    if (db_device_info->voice_read_handle)
    {
        bk_voice_read_deinit(db_device_info->voice_read_handle);
    }

    if (db_device_info->voice_write_handle)
    {
        bk_voice_write_deinit(db_device_info->voice_write_handle);
    }

    if (db_device_info->voice_handle)
    {
        bk_voice_deinit(db_device_info->voice_handle);
    }
    db_device_info->voice_read_handle = NULL;
    db_device_info->voice_write_handle  = NULL;
    db_device_info->voice_handle  = NULL;

    if (voice_cfg)
    {
        os_free(voice_cfg);
        voice_cfg=NULL;
    }

    return BK_FAIL;
}

int anedya_cam_audio_acoustics(uint32_t index, uint32_t param)
{
    LOGD("%s, %u, %u\n", __func__, index, param);
#if 0
    bk_err_t ret = BK_FAIL;

    switch (index)
    {
        case AA_ECHO_DEPTH:
            ret = bk_aud_intf_set_aec_para(AUD_INTF_VOC_AEC_EC_DEPTH, param);
            break;
        case AA_MAX_AMPLITUDE:
            ret = bk_aud_intf_set_aec_para(AUD_INTF_VOC_AEC_TXRX_THR, param);
            break;
        case AA_MIN_AMPLITUDE:
            ret = bk_aud_intf_set_aec_para(AUD_INTF_VOC_AEC_TXRX_FLR, param);
            break;
        case AA_NOISE_LEVEL:
            ret = bk_aud_intf_set_aec_para(AUD_INTF_VOC_AEC_NS_LEVEL, param);
            break;
        case AA_NOISE_PARAM:
            ret = bk_aud_intf_set_aec_para(AUD_INTF_VOC_AEC_NS_PARA, param);
            break;
    }

    return ret;
#endif
    return -1;
}

bool anedya_cam_audio_is_on(void)
{
    return (db_device_info != NULL) && (db_device_info->audio_enable == BK_TRUE);
}

void anedya_cam_audio_data_callback(uint8_t *data, uint32_t length)
{
    bk_err_t ret = BK_OK;

    /* Both failure paths below were silent: the disabled case had no else at
     * all, and the short-write case logged at LOGV, which is compiled out at
     * this build's INFO level. Between them, "speaker plays nothing" produced
     * no evidence whatsoever. Rate-limited so a persistent fault is visible
     * without flooding at the 50 frames/sec this is called at. */
    if (!db_device_info->audio_enable)
    {
        static uint32_t s_disabled = 0;
        if ((s_disabled++ % 250) == 0)
        {
            LOGW("speaker: audio_enable is false, discarding inbound frame (count %u)\n",
                 (unsigned)s_disabled);
        }
        return;
    }

    ret = bk_voice_write_frame_data(db_device_info->voice_write_handle, (char *)data, length);
    if (ret != (bk_err_t)length)
    {
        /* The ring-buffer path returns 0 when the pool has less free space
         * than the frame — it drops rather than blocking, so a persistently
         * full pool means the speaker is draining slower than we feed it. */
        static uint32_t s_short_write = 0;
        if ((s_short_write++ % 250) == 0)
        {
            LOGW("speaker: short write, need %u got %d (count %u)\n",
                 (unsigned)length, (int)ret, (unsigned)s_short_write);
        }
    }
}
#endif

int anedya_cam_devices_init(void)
{
    if (db_device_info == NULL)
    {
        db_device_info = os_malloc(sizeof(db_device_info_t));
    }

    if (db_device_info == NULL)
    {
        LOGE("malloc db_device_info failed");
        return  BK_FAIL;
    }

    os_memset(db_device_info, 0, sizeof(db_device_info_t));

    db_device_info->camera_id = CAMERA_MAX_NUM;

    return BK_OK;
}

void anedya_cam_devices_deinit(void)
{
    if (db_device_info)
    {
        os_free(db_device_info);
        db_device_info = NULL;
    }
}

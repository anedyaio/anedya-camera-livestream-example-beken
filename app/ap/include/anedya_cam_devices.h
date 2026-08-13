#ifndef __ANEDYA_CAM_DEVICES_H__
#define __ANEDYA_CAM_DEVICES_H__

#include "anedya_cam_transmission.h"
#include "media_app.h"
#if CONFIG_VOICE_SERVICE
#include <components/bk_voice_service_types.h>
#include <components/bk_voice_read_service_types.h>
#include <components/bk_voice_write_service_types.h>

#if (CONFIG_ASR_SERVICE)
#include <components/bk_asr_service_types.h>
#endif

#endif

typedef enum
{
    CODEC_FORMAT_UNKNOW = 0,
    CODEC_FORMAT_G711A = 1,
    CODEC_FORMAT_PCM = 2,
    CODEC_FORMAT_G711U = 3,
} codec_format_t;

typedef struct
{
	uint16_t id;
	uint16_t width;
	uint16_t height;
	uint16_t format;
	uint16_t protocol;
	uint16_t rotate;

#ifdef CONFIG_STANDARD_DUALSTREAM
	uint16_t dualstream;
	uint16_t d_width;
	uint16_t d_height;
#endif
} camera_parameters_t;

typedef struct
{
	uint8_t aec;
	uint8_t uac;
	uint8_t rmt_recoder_fmt; /* codec_format_t */
	uint8_t rmt_player_fmt; /* codec_format_t */
	uint32_t rmt_recorder_sample_rate;
	uint32_t rmt_player_sample_rate;
	uint8_t asr;
} audio_parameters_t;


typedef enum
{
	AA_UNKNOWN = 0,
    AA_ECHO_DEPTH = 1,
    AA_MAX_AMPLITUDE = 2,
    AA_MIN_AMPLITUDE = 3,
    AA_NOISE_LEVEL = 4,
    AA_NOISE_PARAM = 5,
} audio_acoustics_t;

typedef struct
{
	uint8_t transfer_enable : 1;
	uint8_t pipeline_enable : 1;
	uint8_t audio_enable : 1;
	uint8_t h264_transfer : 1;
	uint16_t lcd_id;
	uint16_t camera_id;
	void *video_handle;
	const media_transfer_cb_t *camera_transfer_cb;
	const media_transfer_cb_t *audio_transfer_cb;
#if CONFIG_VOICE_SERVICE
    voice_handle_t voice_handle;
    voice_read_handle_t voice_read_handle;
    voice_write_handle_t voice_write_handle;
#endif
#if (CONFIG_ASR_SERVICE)
	asr_handle_t asr_handle;
	aud_asr_handle_t aud_asr_handle;
#endif

} db_device_info_t;

int anedya_cam_get_supported_camera_devices(int opcode, db_channel_t *channel, anedya_cam_transmission_send_t cb);
int anedya_cam_get_supported_lcd_devices(int opcode, db_channel_t *channel, anedya_cam_transmission_send_t cb);
int anedya_cam_get_lcd_status(int opcode, db_channel_t *channel, anedya_cam_transmission_send_t cb);
void anedya_cam_devices_deinit(void);
int anedya_cam_devices_init(void);

int anedya_cam_devices_set_camera_transfer_callback(void *cb);
int anedya_cam_devices_set_audio_transfer_callback(const void *cb);

int anedya_cam_camera_turn_on(camera_parameters_t *parameters);
int anedya_cam_camera_turn_off(void);

int anedya_cam_audio_turn_on(audio_parameters_t *parameters);
int anedya_cam_audio_turn_off(void);
int anedya_cam_audio_acoustics(uint32_t index, uint32_t param);
void anedya_cam_audio_data_callback(uint8_t *data, uint32_t length);

/* True once anedya_cam_audio_turn_on() has brought the voice service up.
 * anedya_cam_audio_data_callback() silently discards frames before that, so
 * anything feeding the speaker directly (see anedya_cam_audio_test.c) should
 * check this first rather than appear to succeed while producing no sound. */
bool anedya_cam_audio_is_on(void);

/* Audio sample rate for both directions.
 *
 * Fixed at 8000 because the wire codec is G.711A: RTP payload type 8 (PCMA)
 * is pinned to 8 kHz by RFC 3551. Raising this alone would make the browser's
 * audio play back at the wrong speed — it can only change alongside a codec
 * that supports a higher rate. */
#define ANEDYA_AUDIO_RATE 8000


int anedya_cam_display_turn_on(uint16_t id, uint16_t rotate, uint16_t fmt);
int anedya_cam_display_turn_off(void);

int anedya_cam_video_transfer_turn_on(void);
int anedya_cam_video_transfer_turn_off(void);


#endif

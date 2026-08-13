#ifndef __ANEDYA_CAM_COMM_H__
#define __ANEDYA_CAM_COMM_H__

#include <stdint.h>

#define UDP_SDP_LOCAL_PORT              (10000)
#define UDP_SDP_REMOTE_PORT             (52110)

#define ANEDYA_CAM_CMD_PORT             (7100)

#define ANEDYA_CAM_UDP_IMG_PORT         (7180)
#define ANEDYA_CAM_UDP_AUD_PORT         (7170)

#define ANEDYA_CAM_TCP_IMG_PORT         (7150)
#define ANEDYA_CAM_TCP_AUD_PORT         (7140)

#define ANEDYA_CAM_UDP_NETWORK_MAX_SIZE (1472)
#define ANEDYA_CAM_TCP_NETWORK_MAX_SIZE (1460)
#define ANEDYA_CAM_NETWORK_MAX_SIZE     (1024)

#define TRANSMISSION_BIG_ENDIAN (BK_FALSE)


#if TRANSMISSION_BIG_ENDIAN == BK_TRUE
#define CHECK_ENDIAN_UINT32(var)    htonl(var)
#define CHECK_ENDIAN_UINT16(var)    htons(var)

#define STREAM_TO_UINT16(u16, p) {u16 = (((uint16_t)(*((p) + 1))) + (((uint16_t)(*((p)))) << 8)); (p) += 2;}
#define STREAM_TO_UINT32(u32, p) {u32 = ((((uint32_t)(*((p) + 3)))) + ((((uint32_t)(*((p) + 2)))) << 8) + ((((uint32_t)(*((p) + 1)))) << 16) + ((((uint32_t)(*((p))))) << 24)); (p) += 4;}


#else
#define CHECK_ENDIAN_UINT32
#define CHECK_ENDIAN_UINT16

#define STREAM_TO_UINT16(u16, p) {u16 = ((uint16_t)(*(p)) + (((uint16_t)(*((p) + 1))) << 8)); (p) += 2;}
#define STREAM_TO_UINT32(u32, p) {u32 = (((uint32_t)(*(p))) + ((((uint32_t)(*((p) + 1)))) << 8) + ((((uint32_t)(*((p) + 2)))) << 16) + ((((uint32_t)(*((p) + 3)))) << 24)); (p) += 4;}


#endif

#define STREAM_TO_UINT8(u8, p) {u8 = (uint8_t)(*(p)); (p) += 1;}


typedef enum
{
	DBEVT_WIFI_STATION_CONNECT,
	DBEVT_WIFI_STATION_CONNECTED,
	DBEVT_WIFI_STATION_DISCONNECTED,

	DBEVT_P2P_CS2_SERVICE_START_REQUEST,
	DBEVT_P2P_CS2_SERVICE_START_RESPONSE,

	DBEVT_LAN_UDP_SERVICE_START_REQUEST,
	DBEVT_LAN_UDP_SERVICE_START_RESPONSE,

	DBEVT_LAN_TCP_SERVICE_START_REQUEST,
	DBEVT_LAN_TCP_SERVICE_START_RESPONSE,


	DBEVT_WIFI_SOFT_AP_TURNING_ON,

	DBEVT_REMOTE_DEVICE_CONNECTED,
	DBEVT_REMOTE_DEVICE_DISCONNECTED,

	DBEVT_START_WIFI_STATION,
	DBEVT_START_TCP_SERVICE,
	DBEVT_START_BOARDING_EVENT,
	DBEVT_BLE_DISABLE,
	DBEVT_SDP,
	DBEVT_EXIT,

	DBEVT_IMAGE_TCP_SERVICE_DISCONNECTED,
	DBEVT_VOICE_EVENT
} dbevt_t;

typedef enum
{
	ANEDYA_CAM_SERVICE_NONE = 0,
	ANEDYA_CAM_SERVICE_P2P_CS2 = 1,
	ANEDYA_CAM_SERVICE_LAN_UDP = 2,
	ANEDYA_CAM_SERVICE_LAN_TCP = 3
} anedya_cam_service_t;

/* Keep backward compat aliases for old enum values used in .c files */
#define DOORBELL_SERVICE_NONE    ANEDYA_CAM_SERVICE_NONE
#define DOORBELL_SERVICE_P2P_CS2 ANEDYA_CAM_SERVICE_P2P_CS2
#define DOORBELL_SERVICE_LAN_UDP ANEDYA_CAM_SERVICE_LAN_UDP
#define DOORBELL_SERVICE_LAN_TCP ANEDYA_CAM_SERVICE_LAN_TCP

typedef struct
{
	uint32_t event;
	uint32_t param;
} anedya_cam_msg_t;

/* Backward compat */
typedef anedya_cam_msg_t doorbell_msg_t;

typedef enum
{
	DB_TURN_OFF,
	DB_TURN_ON,
} anedya_cam_state_t;

/* Backward compat */
typedef anedya_cam_state_t doorbell_state_t;


bk_err_t anedya_cam_send_msg(anedya_cam_msg_t *msg);

void anedya_cam_core_init(void);

/* Backward compat */
#define doorbell_send_msg anedya_cam_send_msg
#define doorbell_core_init anedya_cam_core_init

typedef struct
{
	int (*init)(void *param);
	void (*deinit)(void);
	int (*camera_state_changed)(anedya_cam_state_t state);
	int (*audio_state_changed)(anedya_cam_state_t state);
	const void *camera_transfer_cb;
	const void *audio_transfer_cb;
} anedya_cam_service_interface_t;

/* Backward compat */
typedef anedya_cam_service_interface_t doorbell_service_interface_t;


#endif

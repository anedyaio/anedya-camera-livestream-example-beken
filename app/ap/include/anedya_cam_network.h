#ifndef __ANEDYA_CAM_NETWORK_H__
#define __ANEDYA_CAM_NETWORK_H__

#include "lwip/sockets.h"
#include "net.h"

#define IP_QOS_PRIORITY_HIGHEST			(0xD0)
#define IP_QOS_PRIORITY_HIGH			(0xA0)
#define IP_QOS_PRIORITY_LOW				(0x20)
#define IP_QOS_PRIORITY_LOWEST			(0x00)

#define ANEDYA_CAM_SEND_MAX_RETRY (2000)
#define ANEDYA_CAM_SEND_MAX_DELAY (10)

int anedya_cam_wifi_sta_connect(char *ssid, char *key);

/* True between WIFI_LINKSTATE_STA_GOT_IP and WIFI_LINKSTATE_STA_DISCONNECTED. */
bool anedya_cam_wifi_is_connected(void);
int anedya_cam_wifi_soft_ap_start(char *ssid, char *key, uint16_t channel);

int anedya_cam_socket_set_qos(int fd, int qos);
int anedya_cam_socket_sendto(int *fd, const struct sockaddr *dst, uint8_t *data, uint32_t length, int offset);
int anedya_cam_socket_write(int *fd, uint8_t *data, uint32_t length, int offset);

#endif

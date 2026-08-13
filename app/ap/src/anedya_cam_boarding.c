#include "anedya_cam_boarding.h"
#include <os/os.h>

void anedya_cam_boarding_event_notify(uint16_t opcode, int status) {}
void anedya_cam_boarding_event_message(uint16_t opcode, int status) {}
void anedya_cam_boarding_operation_handle(uint16_t opcode, uint16_t length, uint8_t *data) {}
int anedya_cam_boarding_init(void) { return 0; }
void anedya_cam_boarding_event_notify_with_data(uint16_t opcode, int status, char *payload, uint16_t length) {}

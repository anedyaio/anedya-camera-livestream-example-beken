#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <media_service.h>
#include "bk_smart_config.h"
#include <anedya_cam_comm.h>
#include "anedya_cam_core.h"


int main(void)
{
	bk_init();
    media_service_init();

#if CONFIG_INTEGRATION_DOORBELL
    bk_smart_config_init();
    anedya_cam_core_init();
#endif
	return 0;
}

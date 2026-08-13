set(incs include  .)
set(srcs
    ap_main.c
    )

if (CONFIG_INTEGRATION_DOORBELL)
	list(APPEND srcs
		src/anedya_cam_core.c
		src/anedya_cam_boarding.c
		src/anedya_cam_network.c
		src/anedya_cam_devices.c
		src/anedya_cam_transmission.c
		src/anedya_cam_mqtt.c
		src/anedya_cam_webrtc.c
		src/bk_smart_config.c
	)
endif()

set(priv_req  media_service multimedia wifi_transfer avdk_utils lwip_intf_v2_1 bk_wifi_driver libpeer json psa_mbedtls)

armino_component_register(SRCS "${srcs}" INCLUDE_DIRS "${incs}" PRIV_REQUIRES "${priv_req}"
)

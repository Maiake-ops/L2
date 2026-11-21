L2WM_VERSION = 1
L2WM_SITE = $(BR2_EXTERNAL_L2_PATH)/../l2wm
L2WM_SITE_METHOD = local
L2WM_DEPENDENCIES = xlib_libX11

define L2WM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c99 -Wall -Wextra $(TARGET_LDFLAGS) -o $(@D)/l2wm $(L2WM_SITE)/wm.c -lX11
endef

define L2WM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/l2wm $(TARGET_DIR)/usr/bin/l2wm
endef

$(eval $(generic-package))

$(L2WM_TARGET_BUILD): $(L2WM_SITE)/wm.c

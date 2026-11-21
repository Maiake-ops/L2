include $(BR2_EXTERNAL_L2_PATH)/packages/l2wm/l2wm.mk
# /usr/bin belongs to l2pm's active profile. The system XKB compiler lives in /bin.
XSERVER_XORG_SERVER_CONF_OPTS += --with-xkb-bin-directory=/bin
XAPP_XINIT_CONF_OPTS += MCOOKIE=/bin/mcookie --with-xserver=/etc/X11/xinit/xserverrc

$(XAPP_XINIT_TARGET_CONFIGURE) $(XSERVER_XORG_SERVER_TARGET_CONFIGURE): $(BR2_EXTERNAL_L2_PATH)/external.mk

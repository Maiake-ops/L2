include $(BR2_EXTERNAL_L2_PATH)/packages/l2wm/l2wm.mk
# /usr/bin belongs to l2pm's active profile. The system XKB compiler lives in /bin.
XSERVER_XORG_SERVER_CONF_OPTS += --with-xkb-bin-directory=/bin

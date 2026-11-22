include $(BR2_EXTERNAL_L2_PATH)/packages/l2wm/l2wm.mk
# /usr/bin belongs to l2pm's active profile. The system XKB compiler lives in /bin.
XSERVER_XORG_SERVER_CONF_OPTS += --with-xkb-bin-directory=/bin
XAPP_XINIT_CONF_OPTS += MCOOKIE=/bin/mcookie --with-xserver=/etc/X11/xinit/xserverrc
# Core X11 bitmap fonts only (-fn fixed); drop Xft/Fontconfig/Xinerama/legacy emulations to cut RAM.
XSERVER_XORG_SERVER_CONF_OPTS += \
	--disable-record \
	--disable-xvmc \
	--disable-screensaver \
	--disable-xdmcp \
	--disable-xdm-auth-1 \
	--disable-xinerama \
	--disable-xf86vidmode \
	--disable-xres \
	--disable-dbe \
	--disable-config-udev \
	--disable-config-udev-kms \
	--disable-dri \
	--disable-dri2 \
	--disable-dri3 \
	--disable-glx \
	--disable-glamor \
	--disable-libdrm \
	--disable-xshmfence \
	--disable-vgahw \
	--disable-int10-module \
	--disable-linux-acpi \
	--disable-linux-apm \
	--disable-ipv6
XTERM_CONF_OPTS += \
	--disable-freetype \
	--without-xinerama \
	--disable-tek4014 \
	--disable-vt52 \
	--disable-luit \
	--disable-mini-luit \
	--disable-imake \
	--disable-dabbrev \
	--disable-screen-dumps \
	--disable-regis-graphics \
	--disable-sixel-graphics

$(XAPP_XINIT_TARGET_CONFIGURE) $(XSERVER_XORG_SERVER_TARGET_CONFIGURE) $(XTERM_TARGET_CONFIGURE): $(BR2_EXTERNAL_L2_PATH)/external.mk

# NetSurf only needs xxd from host-vim; build src/xxd directly without full vim/ncurses.
define HOST_VIM_CONFIGURE_CMDS
	true
endef
define HOST_VIM_BUILD_CMDS
	$(HOST_MAKE_ENV) $(MAKE) -C $(@D)/src/xxd CC="$(HOSTCC)" CFLAGS="$(HOST_CFLAGS)" LDFLAGS="$(HOST_LDFLAGS)"
endef
define HOST_VIM_INSTALL_CMDS
	$(INSTALL) -D -m 0755 $(@D)/src/xxd/xxd $(HOST_DIR)/bin/xxd
endef

# Use NetSurf's built-in internal font so no external TrueType font files are required.
define NETSURF_FONTLIB_CONFIGURE_CMDS
	echo "override NETSURF_FB_FONTLIB := internal" >> $(@D)/netsurf/Makefile.config
endef

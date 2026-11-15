# l2wifi - build and source tarball.
# Prefer the project's generated musl wrapper (static glibc is absent on some
# hosts, e.g. NixOS); honour an explicit CC from the environment/command line.
ifeq ($(origin CC),default)
CC := $(if $(wildcard ../build/bin/musl-gcc),../build/bin/musl-gcc,cc)
endif
CFLAGS  ?= -std=c11 -Os -Wall -Wextra -Wno-format-truncation
VERSION  = 0.1.0

all: l2wifi

l2wifi: l2wifi.c
	$(CC) $(CFLAGS) -static -o $@ l2wifi.c
	-strip -s $@

# reproducible source tarball: sorted, fixed owner and mtime, xz
dist: l2wifi-$(VERSION).tar.xz

l2wifi-$(VERSION).tar.xz: l2wifi.c Makefile
	tar --sort=name --owner=0 --group=0 --numeric-owner \
	    --mtime='@1767225600' --transform 's,^,l2wifi-$(VERSION)/,' \
	    -cf - l2wifi.c Makefile | xz -9e --check=crc32 > $@

clean:
	rm -f l2wifi l2wifi-$(VERSION).tar.xz
.PHONY: all dist clean

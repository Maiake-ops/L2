# l2pm - small static package manager.
#
# Prefer the project's generated musl wrapper (static glibc is absent on some
# hosts, e.g. NixOS); honour an explicit CC from the environment/command line.
# For the real L2 userland:
#   make CC=/path/to/musl-gcc LDFLAGS=-static
#
PREFIX ?= /usr/local
ifeq ($(origin CC),default)
CC := $(if $(wildcard ../build/bin/musl-gcc),../build/bin/musl-gcc,cc)
endif
CFLAGS ?= -std=c11 -Os -Wall -Wextra -ffile-prefix-map=$(CURDIR)=. -fdebug-prefix-map=$(CURDIR)=.
LDFLAGS ?= -static
BIN := l2pm
LIMIT := 10485760

all: $(BIN)

$(BIN): l2pm.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	@strip -s $@ 2>/dev/null || true
	@echo "== l2pm =="
	@ls -l $@
	@size $@ 2>/dev/null || true

check: $(BIN)
	@./$(BIN) selftest
	@test "$$(stat -c%s $(BIN))" -lt $(LIMIT)

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/l2pm
	install -d $(DESTDIR)$(PREFIX)/share/l2/recipes
	install -m 0644 recipes/*.recipe $(DESTDIR)$(PREFIX)/share/l2/recipes/

clean:
	rm -f $(BIN)

.PHONY: all check install clean
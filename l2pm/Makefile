# l2pm - small static package manager.
#
# Defaults build a static binary with the host cc.  For the real L2 userland:
#   make CC=/path/to/musl-gcc LDFLAGS=-static
#
PREFIX ?= /usr/local
CC ?= cc
CFLAGS ?= -std=c11 -Os -Wall -Wextra -ffile-prefix-map=$(CURDIR)=.
LDFLAGS ?= -static
BIN := l2pm
LIMIT := 10485760

all: $(BIN)

$(BIN): l2pm.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	@strip $@ 2>/dev/null || true
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
CC ?= cc
RUSTC ?= rustc
CFLAGS ?= -Os -Wall -Wextra -Werror
CPPFLAGS += $(shell pkg-config --cflags x11 2>/dev/null)
LDLIBS ?= -lX11
PREFIX ?= /usr
.PHONY: all clean test install
all: build/l2wm
build:
	mkdir -p $@
build/layout.a: src/layout.rs | build
	$(RUSTC) --edition=2021 --crate-type staticlib -C panic=abort -C opt-level=s $(RUSTFLAGS) $< -o $@
build/l2wm: src/wm.c build/layout.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/layout.a $(LDFLAGS) $(LDLIBS) -o $@
test: | build
	$(RUSTC) --edition=2021 --test src/layout.rs -o build/layout-tests
	./build/layout-tests
install: all
	install -Dm755 build/l2wm $(DESTDIR)$(PREFIX)/bin/l2wm
clean:
	rm -rf build

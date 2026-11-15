# rcl2 build - non-GNU toolchain end to end:
#
#   clang   (LLVM)    C core, freestanding, no libc
#   rustc   (rust/LLVM)  Rust core, #![no_std], no crates, staticlib
#   ld.lld  (LLVM)    final link (no GNU ld, no gcc, no binutils, no musl-gcc)
#
# No libc links into rcl2 at all: the kernel ABI is used directly, so the
# result is a single static ELF with no dependencies outside the kernel.

CLANG  ?= clang
RUSTC  ?= rustc
LLD    ?= ld.lld
STRIP  ?= llvm-strip
OBJSIZE ?= llvm-size

BUILD   := build
CFLAGS  := -std=c11 -Os -Wall -Wextra \
           -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-asynchronous-unwind-tables -fomit-frame-pointer \
           -ffunction-sections -fdata-sections \
           -fno-pic -fno-pie -fno-ident -fcf-protection=none
ASFLAGS := -target x86_64-unknown-linux-gnu
RFLAGS  := --edition 2021 --crate-type staticlib --crate-name rcl2rs \
           -O -C panic=abort -C codegen-units=1
LDFLAGS := -static --build-id=none -z max-page-size=4096 -z noseparate-code \
           --gc-sections

.PHONY: all clean selftest test size
all: $(BUILD)/rcl2

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/start.o: src/start.s | $(BUILD)
	$(CLANG) $(ASFLAGS) -c $< -o $@

$(BUILD)/main.o: src/main.c src/syscalls.h src/rcl.h | $(BUILD)
	$(CLANG) $(ASFLAGS) -c $(CFLAGS) $< -o $@

$(BUILD)/mem.o: src/mem.c src/syscalls.h | $(BUILD)
	$(CLANG) $(ASFLAGS) -c $(CFLAGS) $< -o $@

$(BUILD)/librcl2rs.a: src/rs/rcl.rs | $(BUILD)
	$(RUSTC) $(RFLAGS) -o $@ $<

$(BUILD)/rcl2: $(BUILD)/start.o $(BUILD)/main.o $(BUILD)/mem.o $(BUILD)/librcl2rs.a
	$(LLD) $(LDFLAGS) -o $@ $^
	$(STRIP) -s $@
	@echo "== sizes =="
	$(OBJSIZE) $@

size: $(BUILD)/rcl2
	@echo "== file =="
	file $(BUILD)/rcl2
	@echo "== rust core archive =="
	llvm-ar t $(BUILD)/librcl2rs.a

# safe to run as any user: parses the builtin fixture, reports RSS, exits 0
selftest: $(BUILD)/rcl2
	$(BUILD)/rcl2 --selftest

$(BUILD)/test-supervision.o: verification/test-supervision.c src/main.c src/syscalls.h src/rcl.h | $(BUILD)
	$(CLANG) $(ASFLAGS) -c $(CFLAGS) $< -o $@

$(BUILD)/test-supervision: $(BUILD)/start.o $(BUILD)/test-supervision.o $(BUILD)/mem.o
	$(LLD) $(LDFLAGS) -o $@ $^

test: selftest $(BUILD)/test-supervision
	$(BUILD)/test-supervision
	@echo "supervision regression tests passed"

clean:
	rm -rf $(BUILD)

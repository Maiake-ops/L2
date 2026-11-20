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
# -Oz + force-unwind-tables=no (maiake-f6 auditor carry, 04-rcl2.md §6):
# smallest code size; .eh_frame is dropped. Safe here: panic=abort never
# unwinds and the signal return trampoline is hand-written asm (start.s).
# Trade-off: no CFI for gdb backtraces through rcl2 frames.
CFLAGS  := -std=c11 -Oz -Wall -Wextra \
           -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-asynchronous-unwind-tables -fomit-frame-pointer \
           -ffunction-sections -fdata-sections \
           -fno-pic -fno-pie -fno-ident -fcf-protection=none
# -Wno-unused-command-line-argument: the Nix clang wrapper injects C-only
# flags that are unused when assembling .s (noise on clean builds)
ASFLAGS := -target x86_64-unknown-linux-gnu -Wno-unused-command-line-argument
RFLAGS  := --edition 2021 --crate-type staticlib --crate-name rcl2rs \
           -C opt-level=z -C panic=abort -C codegen-units=1 \
           -C force-unwind-tables=no
LDFLAGS := -static --build-id=none -z max-page-size=4096 -z noseparate-code \
           --no-rosegment -z norelro --gc-sections

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

# fake-file regressions call the real Rust parser -> link the archive
$(BUILD)/test-supervision: $(BUILD)/start.o $(BUILD)/test-supervision.o $(BUILD)/mem.o $(BUILD)/librcl2rs.a
	$(LLD) $(LDFLAGS) -o $@ $^

# campaign-9 live scratch caps (ported from maiake-f6, 04-rcl2.md §4.6):
# default mode pins the mount decision, real mode runs the real mounts_setup
$(BUILD)/test-tmpcap.o: verification/test-tmpcap.c src/main.c src/syscalls.h src/rcl.h | $(BUILD)
	$(CLANG) $(ASFLAGS) -c $(CFLAGS) -I src $< -o $@

$(BUILD)/test-tmpcap: $(BUILD)/start.o $(BUILD)/test-tmpcap.o $(BUILD)/mem.o
	$(LLD) $(LDFLAGS) -o $@ $^

$(BUILD)/test-tmpcap-real.o: verification/test-tmpcap.c src/main.c src/syscalls.h src/rcl.h | $(BUILD)
	$(CLANG) $(ASFLAGS) -c $(CFLAGS) -DTEST_REAL_SRC -I src $< -o $@

$(BUILD)/test-tmpcap-real: $(BUILD)/start.o $(BUILD)/test-tmpcap-real.o $(BUILD)/mem.o
	$(LLD) $(LDFLAGS) -o $@ $^

$(BUILD)/test-truncation.o: verification/test-truncation.c src/main.c src/syscalls.h src/rcl.h | $(BUILD)
	$(CLANG) $(ASFLAGS) -c $(CFLAGS) $< -o $@

# calls conf_load/cmdline_flags -> needs the Rust parser archive
$(BUILD)/test-truncation: $(BUILD)/start.o $(BUILD)/test-truncation.o $(BUILD)/mem.o $(BUILD)/librcl2rs.a
	$(LLD) $(LDFLAGS) -o $@ $^

$(BUILD)/test-lifecycle.o: verification/test-lifecycle.c src/main.c src/syscalls.h src/rcl.h | $(BUILD)
	$(CLANG) $(ASFLAGS) -c $(CFLAGS) $< -o $@

$(BUILD)/test-lifecycle: $(BUILD)/start.o $(BUILD)/test-lifecycle.o $(BUILD)/mem.o
	$(LLD) $(LDFLAGS) -o $@ $^

test: selftest $(BUILD)/test-supervision $(BUILD)/test-truncation $(BUILD)/test-lifecycle $(BUILD)/test-tmpcap $(BUILD)/test-tmpcap-real
	$(BUILD)/test-supervision
	$(BUILD)/test-truncation
	$(BUILD)/test-lifecycle
	$(BUILD)/test-tmpcap
	$(BUILD)/test-tmpcap-real
	@echo "supervision regression tests passed"

clean:
	rm -rf $(BUILD)

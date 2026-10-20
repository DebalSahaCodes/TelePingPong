VERSION  := 1.0.0
# Sources are grouped by platform: src/common (all OSes), src/linux, src/windows.
COMMON   := src/common/main.c src/common/server.c src/common/client.c src/common/net_common.c
SRC_LINUX := $(COMMON) src/linux/platform_linux.c
SRC_WIN   := $(COMMON) src/windows/platform_windows.c
INC_LINUX := -Isrc/common -Isrc/linux
INC_WIN   := -Isrc/common -Isrc/windows
HDR      := $(wildcard src/*/*.h)
WARN     := -Wall -Wextra -Wshadow -Wformat=2
CFLAGS   ?= -O2 -std=gnu11
DIST     := dist
BUILD    := build

# Toolchains for the release builds. Override on the command line if yours differ.
#   amd64 : musl-gcc          (apt install musl-tools)
#   arm64 : Zig's bundled clang + musl   (pip install ziglang)
#   win64 : mingw-w64         (apt install gcc-mingw-w64-x86-64)
ZIG        ?= python3 -m ziglang
AMD64_CC   ?= musl-gcc -static
ARM64_CC   ?= $(ZIG) cc -target aarch64-linux-musl
WIN_CC     ?= x86_64-w64-mingw32-gcc
WIN_STRIP  ?= x86_64-w64-mingw32-strip

LINUX_AMD64 := $(BUILD)/linux-amd64/pingpong
LINUX_ARM64 := $(BUILD)/linux-arm64/pingpong
WIN_X64     := $(BUILD)/windows-x64/pingpong.exe

ZIP_WIN     := $(DIST)/pingpong-$(VERSION)-windows-x64.zip
TGZ_AMD64   := $(DIST)/pingpong-$(VERSION)-linux-amd64.tar.gz
TGZ_ARM64   := $(DIST)/pingpong-$(VERSION)-linux-arm64.tar.gz
DEB_AMD64   := $(DIST)/pingpong-net_$(VERSION)_amd64.deb
DEB_ARM64   := $(DIST)/pingpong-net_$(VERSION)_arm64.deb

.PHONY: all linux dist test test-dist clean

# Native build for development (any Linux/macOS/BSD with a C compiler)
all linux: pingpong
pingpong: $(SRC_LINUX) $(HDR)
	$(CC) $(CFLAGS) $(WARN) $(INC_LINUX) -o $@ $(SRC_LINUX) -lm

# ---- release binaries ------------------------------------------------------
# Static musl builds run on every Debian/Ubuntu/Raspberry Pi OS release,
# whatever its glibc version.
$(LINUX_AMD64): $(SRC_LINUX) $(HDR)
	@mkdir -p $(@D)
	$(AMD64_CC) $(CFLAGS) $(WARN) $(INC_LINUX) -s -o $@ $(SRC_LINUX) -lm

$(LINUX_ARM64): $(SRC_LINUX) $(HDR)
	@mkdir -p $(@D)
	$(ARM64_CC) $(CFLAGS) $(WARN) $(INC_LINUX) -s -o $@ $(SRC_LINUX) -lm

$(WIN_X64): $(SRC_WIN) $(HDR)
	@mkdir -p $(@D)
	$(WIN_CC) $(CFLAGS) $(WARN) $(INC_WIN) -o $@ $(SRC_WIN) -static -lws2_32 -lwinmm -lm
	$(WIN_STRIP) $@

# ---- packages --------------------------------------------------------------
$(DEB_AMD64): $(LINUX_AMD64) packaging/mkdeb.sh packaging/control packaging/pingpong-server.service README.md
	@mkdir -p $(DIST)
	sh packaging/mkdeb.sh amd64 $(LINUX_AMD64) $@ $(VERSION)

$(DEB_ARM64): $(LINUX_ARM64) packaging/mkdeb.sh packaging/control packaging/pingpong-server.service README.md
	@mkdir -p $(DIST)
	sh packaging/mkdeb.sh arm64 $(LINUX_ARM64) $@ $(VERSION)

define LINUX_TGZ
$(1): $(2) README.md packaging/pingpong-server.service
	@mkdir -p $(DIST)
	rm -rf $(BUILD)/tgz-$(3) && mkdir -p $(BUILD)/tgz-$(3)/pingpong-$(VERSION)-linux-$(3)
	cp $(2) README.md packaging/pingpong-server.service $(BUILD)/tgz-$(3)/pingpong-$(VERSION)-linux-$(3)/
	tar --owner=0 --group=0 --sort=name -C $(BUILD)/tgz-$(3) -czf $$@ pingpong-$(VERSION)-linux-$(3)
endef
$(eval $(call LINUX_TGZ,$(TGZ_AMD64),$(LINUX_AMD64),amd64))
$(eval $(call LINUX_TGZ,$(TGZ_ARM64),$(LINUX_ARM64),arm64))

# Windows users get double-clickable launchers (CRLF line endings for cmd.exe).
$(ZIP_WIN): $(WIN_X64) README.md packaging/start-client.bat packaging/start-server.bat
	@mkdir -p $(DIST)
	rm -rf $(BUILD)/zip && mkdir -p $(BUILD)/zip/pingpong
	cp $(WIN_X64) $(BUILD)/zip/pingpong/pingpong.exe
	sed 's/$$/\r/' README.md > $(BUILD)/zip/pingpong/README.txt
	sed 's/$$/\r/' packaging/start-client.bat > $(BUILD)/zip/pingpong/start-client.bat
	sed 's/$$/\r/' packaging/start-server.bat > $(BUILD)/zip/pingpong/start-server.bat
	rm -f $@ && cd $(BUILD)/zip && zip -qr $(CURDIR)/$@ pingpong

dist: $(ZIP_WIN) $(TGZ_AMD64) $(TGZ_ARM64) $(DEB_AMD64) $(DEB_ARM64)
	@cd $(DIST) && sha256sum * > SHA256SUMS && cat SHA256SUMS

# ---- tests -----------------------------------------------------------------
# Protocol/server tests, WAN-latency test, and the pseudo-terminal client test
# (needs: pip install pyte).
test: pingpong
	python3 tests/test_server.py ./pingpong
	python3 tests/test_wan.py ./pingpong
	python3 tests/test_client_pty.py ./pingpong

# Same server tests against the static binary that actually ships.
test-dist: $(LINUX_AMD64)
	python3 tests/test_server.py $(LINUX_AMD64)
	python3 tests/test_wan.py $(LINUX_AMD64)

clean:
	rm -rf pingpong $(BUILD) $(DIST)

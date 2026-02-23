# Eloquence V6.1 ARM-to-Any-Arch Bridge
# Builds: eci-bridge (ARM), libeci.so (native shim), eloquence (native CLI),
#         sd_eloquence (speech-dispatcher module)

# Cross compiler for ARM bridge
ARM_CC      = arm-linux-gnueabihf-gcc
ARM_SYSROOT = $(CURDIR)/sysroot/armhf
ARM_CFLAGS  = -Wall -Wextra -O2 -g -Iinclude -Iproto --sysroot=$(ARM_SYSROOT)
ARM_LDFLAGS = --sysroot=$(ARM_SYSROOT) -lpthread -ldl

# Native compiler for host shim + CLI
CC          = gcc
CFLAGS      = -Wall -Wextra -O2 -g -Iinclude -Iproto
LDFLAGS     = -lpthread -ldl

BUILDDIR    = build

# Source files
PROTO_SRC   = proto/rpc_io.c proto/rpc_msg.c
ARM_SRC     = arm/bridge_main.c arm/bridge_dispatch.c arm/bridge_handle.c arm/bridge_callback.c
SHIM_SRC    = host/shim_libeci.c host/shim_connection.c host/shim_callback.c host/shim_launch.c
CLI_SRC     = cli/eloquence.c
SPD_SRC     = spd/sd_eloquence.c

# Object files
PROTO_ARM_OBJ = $(patsubst proto/%.c,$(BUILDDIR)/arm/proto_%.o,$(PROTO_SRC))
ARM_OBJ       = $(patsubst arm/%.c,$(BUILDDIR)/arm/%.o,$(ARM_SRC))
PROTO_HOST_OBJ = $(patsubst proto/%.c,$(BUILDDIR)/host/proto_%.o,$(PROTO_SRC))
SHIM_OBJ      = $(patsubst host/%.c,$(BUILDDIR)/host/%.o,$(SHIM_SRC))
CLI_OBJ        = $(patsubst cli/%.c,$(BUILDDIR)/cli/%.o,$(CLI_SRC))

# Targets
BRIDGE      = $(BUILDDIR)/eci-bridge
SHIM_LIB    = $(BUILDDIR)/libeci.so
CLI_BIN     = $(BUILDDIR)/eloquence
SPD_BIN     = $(BUILDDIR)/sd_eloquence

# Install paths
PREFIX     = /usr
LIBDIR     = $(PREFIX)/lib/x86_64-linux-gnu
DATADIR    = $(PREFIX)/share/eloquence
BINDIR     = $(PREFIX)/bin
MANDIR     = $(PREFIX)/share/man/man1
SPDMODDIR  = $(PREFIX)/lib/speech-dispatcher-modules
SPDCONFDIR = /etc/speech-dispatcher/modules

.PHONY: all clean sysroot sd_eloquence install

all: $(BRIDGE) $(SHIM_LIB) $(CLI_BIN) $(BUILDDIR)/sysroot

# Create build directories
$(BUILDDIR)/arm $(BUILDDIR)/host $(BUILDDIR)/cli:
	mkdir -p $@

# ARM bridge
$(BUILDDIR)/arm/proto_%.o: proto/%.c | $(BUILDDIR)/arm
	$(ARM_CC) $(ARM_CFLAGS) -c -o $@ $<

$(BUILDDIR)/arm/%.o: arm/%.c | $(BUILDDIR)/arm
	$(ARM_CC) $(ARM_CFLAGS) -c -o $@ $<

$(BRIDGE): $(PROTO_ARM_OBJ) $(ARM_OBJ)
	$(ARM_CC) -o $@ $^ $(ARM_LDFLAGS)

# Host shim library (shared .so)
$(BUILDDIR)/host/proto_%.o: proto/%.c | $(BUILDDIR)/host
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

$(BUILDDIR)/host/%.o: host/%.c | $(BUILDDIR)/host
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

$(SHIM_LIB): $(PROTO_HOST_OBJ) $(SHIM_OBJ)
	$(CC) -shared -o $@ $^ $(LDFLAGS) -Wl,-soname,libeci.so

# CLI
$(BUILDDIR)/cli/%.o: cli/%.c | $(BUILDDIR)/cli
	$(CC) $(CFLAGS) -c -o $@ $<

$(CLI_BIN): $(CLI_OBJ) $(SHIM_LIB)
	$(CC) -o $@ $(CLI_OBJ) -L$(BUILDDIR) -leci -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

# Speech-dispatcher module
sd_eloquence: $(SPD_BIN)

$(SPD_BIN): $(SPD_SRC) $(SHIM_LIB)
	$(CC) $(CFLAGS) -Ispd $(shell pkg-config --cflags speech-dispatcher) \
		-o $@ $< -L$(BUILDDIR) -leci -lspeechd_module \
		$(shell pkg-config --libs speech-dispatcher) \
		-Wl,-rpath,'$$ORIGIN'

# Symlink sysroot into build dir so shim can find it relative to libeci.so
$(BUILDDIR)/sysroot:
	ln -sf ../sysroot $(BUILDDIR)/sysroot

# Sysroot setup
sysroot:
	bash sysroot/setup-sysroot.sh

# Install target (used by dpkg-buildpackage via debian/rules)
install:
	install -Dm755 $(BRIDGE)    $(DESTDIR)$(DATADIR)/eci-bridge
	install -Dm755 $(SHIM_LIB)  $(DESTDIR)$(LIBDIR)/libeci.so
	install -Dm755 $(CLI_BIN)   $(DESTDIR)$(BINDIR)/eloquence
	install -Dm755 scripts/eloquence-setup $(DESTDIR)$(BINDIR)/eloquence-setup
	install -Dm644 man/eloquence.1 $(DESTDIR)$(MANDIR)/eloquence.1
	# speech-dispatcher module (only if built)
	test ! -f $(SPD_BIN) || \
		install -Dm755 $(SPD_BIN) $(DESTDIR)$(SPDMODDIR)/sd_eloquence
	test ! -f $(SPD_BIN) || \
		install -Dm644 spd/eloquence.conf $(DESTDIR)$(SPDCONFDIR)/eloquence.conf
	# Support files needed by eloquence-setup
	install -Dm644 sysroot/patch-glibc-versions.py $(DESTDIR)$(DATADIR)/patch-glibc-versions.py
	install -Dm644 arm/sjlj_compat.c  $(DESTDIR)$(DATADIR)/arm/sjlj_compat.c
	install -Dm644 arm/sjlj_compat.map $(DESTDIR)$(DATADIR)/arm/sjlj_compat.map

clean:
	rm -rf $(BUILDDIR)

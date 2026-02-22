# Eloquence V6.1 ARM Bridge

This project lets you use the ETI Eloquence V6.1 text-to-speech engine on any Linux architecture (x86_64, aarch64, etc.). The Eloquence libraries are 32-bit ARM shared objects from a LevelStar Icon PDA rootfs. This bridge runs them under qemu-user emulation and exposes the full ECI API to native programs through a transparent shim library and Unix socket RPC.

## How it works

There are three components:

- **eci-bridge** -- An ARM daemon cross-compiled with `arm-linux-gnueabihf-gcc`. It loads the real `libeci.so` via `dlopen`, listens on a Unix socket, and dispatches incoming RPC calls to the actual ECI functions. Each client gets its own thread. Callbacks (like waveform buffer delivery) are forwarded back to clients over a dedicated callback channel.

- **libeci.so (shim)** -- A native shared library that exports the exact same symbols as the real `libeci.so`. When your program calls `eciNew()`, `eciAddText()`, `eciSynthesize()`, etc., the shim serializes the arguments into a binary message and sends it to the bridge over a Unix socket. It automatically launches the bridge daemon (via `qemu-arm`) on first use if it isn't already running.

- **eloquence** -- A command-line tool that links against the shim. You give it text and it produces a WAV file.

The bridge daemon runs under `qemu-arm -L <sysroot>`, where the sysroot is a minimal Debian bookworm armhf environment containing just `libc6`, `libstdc++6`, `libgcc-s1`, and the Eloquence libraries. The sysroot is about 14MB.

## Prerequisites

You need three packages:

```
sudo apt install gcc-arm-linux-gnueabihf qemu-user python3
```

The Eloquence rootfs must exist at `../rootfs/` relative to this directory, containing at minimum:

- `usr/lib/libeci.so` -- the ECI control library
- `usr/lib/enu.so` -- the English synthesis engine
- `etc/eci.ini` -- the ECI configuration file

## Building

First, set up the ARM sysroot. This downloads three armhf `.deb` packages from Debian bookworm, extracts them, copies the Eloquence libraries in, and applies the necessary ELF patches:

```
make sysroot
```

Then build everything:

```
make all
```

This produces three files in `build/`:

- `eci-bridge` -- ARM ELF binary (run via qemu-arm)
- `libeci.so` -- native shared library (the shim)
- `eloquence` -- native CLI tool

## Usage

```
./build/eloquence -t "Hello world" -o hello.wav
```

Options:

- `-t, --text TEXT` -- text to synthesize
- `-f, --file FILE` -- read text from a file
- `-o, --output FILE` -- output WAV file (default: output.wav)
- `-v, --voice NUM` -- voice preset 0-7
- `-s, --speed NUM` -- speaking speed 0-250 (default: 50)
- `-p, --pitch NUM` -- pitch baseline 0-100 (default: 65)
- `--rate NUM` -- sample rate: 0=8kHz, 1=11kHz, 2=22kHz (default: 1)
- `--list-voices` -- show available voices
- `--version` -- show ECI version

If no `-t` or `-f` is given, it reads from stdin.

You can also link your own programs against `build/libeci.so` and use the ECI API directly. The shim is a drop-in replacement for the real library.

## How the sysroot patches work

The original Eloquence `.so` files were built with an old ARM toolchain and need several binary patches to work with modern glibc:

- **ELF OS/ABI byte**: The old toolchain sets byte 7 of the ELF header to `0x61` (ARM-specific). Modern glibc's dynamic linker rejects this. The setup script patches it to `0x00` (SYSV).

- **GLIBC version symbols**: The libraries reference GLIBC_2.0, GLIBC_2.1, GLIBC_2.1.3, GLIBC_2.3, and GLIBC_2.3.2. ARM glibc has never provided versions below GLIBC_2.4 (ARM Linux support started with glibc 2.4). The `patch-glibc-versions.py` script rewrites these version strings in `.dynstr` and also patches their corresponding hashes in the `.gnu.version_r` ELF section so the dynamic linker accepts them.

- **SJLJ exception handling**: The old libraries use setjmp/longjmp-based C++ exception handling (`__gxx_personality_sj0`), but modern ARM `libstdc++` and `libgcc` only provide DWARF-based unwinding. A small stub library (`libsjlj_compat.so`) provides the required symbols. It is LD_PRELOADed into the bridge's qemu-arm environment.

## Wire protocol

The shim and bridge communicate over a Unix socket at `$XDG_RUNTIME_DIR/eci-bridge.sock` (fallback: `/tmp/eci-bridge.sock`). You can override it with the `ECI_BRIDGE_SOCKET` environment variable.

Each client opens two connections to the bridge:

- **RPC channel** -- synchronous request/response for all ECI function calls. The main thread sends a request and blocks until the response arrives.
- **Callback channel** -- the bridge sends callback events (like waveform buffer data) here, and the shim's background thread receives them, invokes the user's callback function, and sends back the return value.

This two-channel design prevents deadlock: `eciSynchronize()` blocks on the RPC channel while callbacks fire asynchronously on the callback channel.

Messages are length-prefixed binary: a 4-byte payload length, a 1-byte message type, then the payload. Arguments are type-tagged (int32, uint32, handle, string, buffer, null).

## Project structure

- `include/eci.h` -- the ECI API header (symlinked from the parent project)
- `include/eci_proto.h` -- function IDs, message types, protocol constants
- `proto/rpc_io.c`, `proto/rpc_io.h` -- framed socket I/O (length-prefixed read/write)
- `proto/rpc_msg.c`, `proto/rpc_msg.h` -- argument serialization (encode/decode typed values)
- `arm/bridge_main.c` -- daemon: socket listener, accept loop, client threads
- `arm/bridge_dispatch.c` -- RPC dispatch: giant switch on function ID, calls real ECI
- `arm/bridge_handle.c` -- handle map: uint32 IDs on the wire to real ARM pointers
- `arm/bridge_callback.c` -- internal ECI callback that serializes events to clients
- `arm/sjlj_compat.c` -- SJLJ exception handling stubs
- `host/shim_libeci.c` -- exports all ECI symbols, sends RPC to bridge
- `host/shim_connection.c` -- Unix socket connect, handshake, RPC send/receive
- `host/shim_callback.c` -- background thread for receiving callback events
- `host/shim_launch.c` -- auto-spawn bridge daemon via qemu-arm
- `cli/eloquence.c` -- command-line tool
- `sysroot/setup-sysroot.sh` -- downloads and patches the ARM sysroot
- `sysroot/patch-glibc-versions.py` -- ELF patcher for GLIBC version strings and hashes

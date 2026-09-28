# AGENTS.md

## Project

IVMP is an MIT-licensed C99 implementation of a minimal GTA IV
native-multiplayer rendezvous and relay.

Current scope:

- Direct `host:port` connections.
- Native server relay for UDP game traffic over a TCP control channel.
- Stock GTA IV game-mode selection.
- No scripting engine, resources, modules, custom assets, accounts, central
  directory, TLS, passwords, anti-cheat, or executable integrity enforcement.
- `xlive.dll` and the Windows server are 32-bit Win32/XP-compatible binaries; there is
  no separate launcher executable.
- The native server is architecture-dependent and is built for the host OS.

## Hard constraints

- Use C99 or older-compatible C. Do not add C++, Rust, Python, Node, CMake,
  Makefiles, generated dependency files, or third-party runtime libraries.
- Keep Windows code limited to APIs available on Windows XP or earlier.
- Windows artifacts must always be PE32/i386. Do not add x64 Windows targets.
- Do not add TLS or password handling unless explicitly requested.
- Do not add executable hash checks, anti-cheat, or game-file blocking.
- Modified game files and cheating clients must remain possible.
- The build must be driven by `build.sh` and a C compiler only.
- Prefer small, explicit C modules over frameworks or code generation.

## Layout

- `src/common/` — endian helpers, wire protocol, server configuration.
- `src/server/` — CLI and single-threaded TCP/UDP relay.
- `src/windows/xlive.c` — GFWL `xlive.dll` replacement; reads
  `IVMP_SERVER`/`IVMP_NICKNAME` from the environment and drives
  the relay connection.
- `src/windows/relay_client.c` — relay control connection and UDP tunnel
  framing used by `xlive.c`.
- `src/windows/game_patch.c` — mod-check and Social Club patches for GTA IV
  1.0.7.0 and 1.0.4.0.
- `src/windows/asi_loader/` — git submodule (upstream asi_loader, with its
  own `5head` submodule); `xlive.c` compiles only `src/asi_loader.c` and calls
  `load_all_asis()` from its deferred init. Change it upstream, not here.
- `src/windows/crash.c` — XP-compatible vectored crash logging and best-effort
  x86 backtrace generation.
- `src/windows/entry_hook.c` — patches the host EXE's entry point to run
  deferred, post-loader-lock initialization; never call `LoadLibrary` from
  `DllMain` directly.
- `src/windows/gui.c` — `ivmp_gui.exe`, a Win32 launcher that sets the
  `IVMP_*` variables for the game process and starts it.
- `tools/connect.bat` — copies `xlive.dll` into a game folder and persists
  the connection environment variables.
- `tests/` — native protocol tests, smoke test, and Windows fake-client tests.
- `docs/` — protocol, build, connecting, reverse-engineering notes, and
  limitations.
- `build.sh` — the only supported build entry point.

## Build and test

Native server and tests:

```sh
apk add build-base
sh build.sh test
```

Build everything, including the Windows client, using an i686 MinGW compiler:

```sh
apk add i686-mingw-w64-gcc
sh build.sh all
```

Useful individual targets:

```sh
sh build.sh server
sh build.sh windows
sh build.sh clean
```

The Windows crash-handler test is optional and uses Wine:

```sh
sh tests/crash-handler.sh
```

Do not commit `bin/`, `build/`, generated binaries, logs, or packet captures.

## Networking model

- The control channel uses the `IVMPCTL` header defined in
  `src/common/protocol.h`.
- Game traffic is UDP only and uses the `IVMT` tunnel header.
- A zero-length tunnel frame registers a UDP address and is not forwarded.
- Targeted frames go to one player; untargeted frames go to all other players.
- TCP game sockets are plain Winsock and are not relayed.
- The server intentionally treats game payloads as opaque bytes.
- Keep packet length checks in place even though gameplay is not validated.

## Windows client rules

- Do not reject unknown or modified executables.
- Preserve XP-compatible calling conventions and synchronous Winsock behavior.
- Basic Winsock functions and the synchronous WSA variants must remain covered.
- Overlapped I/O is not currently supported; do not silently pass wrapped
  traffic through without documenting the limitation.
- `xlive.dll` connects lazily from its own exported entry points; it must
  never block process startup waiting on the network.
- Never call `LoadLibrary` (or anything else that can touch the loader)
  from `DllMain`; defer it through `entry_hook.c` instead.

## Reverse engineering

- Record relevant findings in `docs/reverse-engineering.md`.
- Keep proprietary decompiler output, extracted binaries, and copyrighted
  strings out of the repository.
- Prefer independently written behavioral notes and original C code.

## Style and validation

- Use bounded copies and explicit length checks around packet data.
- Avoid unbounded allocations based on network input.
- Keep error paths quiet in the relay; useful client diagnostics may go to the
  `ivmp.log` trace.
- Run `sh build.sh test` after native changes.
- Run `sh tests/crash-handler.sh` after crash-handler changes.
- Update `docs/protocol.md` when changing wire formats.
- Update `docs/limitations.md` when adding a known compatibility caveat.

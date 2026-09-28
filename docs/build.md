# Building

The project deliberately uses a single BusyBox-compatible shell script. It does
not require Make, CMake, Rust, C++, Python, Node, or a package manager at build
time.

## Native server

Install a C compiler and libc headers with `apk` when needed:

```sh
apk add build-base
sh build.sh server
```

The resulting executable is `bin/ivmp_server`.

## Windows client

`xlive.dll` embeds the `asi_loader` git submodule. Fetch it (and its own
`5head` submodule) once after cloning:

```sh
git submodule update --init --recursive
```

Windows artifacts are always 32-bit. On Alpine, install the i686 MinGW
cross-compiler from the enabled community/edge repository:

```sh
apk add build-base i686-mingw-w64-gcc
sh build.sh windows
```

The package provides `i686-w64-mingw32-gcc`. An equivalent XP-compatible C
cross-compiler can be supplied with `WINCC`:

```sh
WINCC=/path/to/i686-w64-mingw32-gcc sh build.sh windows
```

The compiler should target the legacy Microsoft C runtime (`msvcrt.dll`) rather
than the Windows API-set/UCRT runtime. The default linker flags request an XP
subsystem version and disable dynamic-base/NX flags for maximum XP
compatibility. Always inspect the produced PE imports on the target toolchain.

The build produces:

- `bin/xlive.dll`
- `bin/ivmp_gui.exe`
- `bin/ivmp_server.exe`

All are PE32/i386. No 64-bit Windows artifact is produced. See
`docs/connecting.md` for how a player uses `xlive.dll` to connect.

## Tests

Native tests require only the native C toolchain:

```sh
sh build.sh test
```

The Windows crash-handler test is optional. On a Linux/Alpine analysis machine
it expects Wine and an i686 Windows compiler in `WINCC`:

```sh
WINCC=i686-w64-mingw32-gcc sh tests/crash-handler.sh
```

It does not require GTA IV files. A real
compatibility test must use legally owned GTA IV 1.0.7.0 and 1.0.8.0 installs.

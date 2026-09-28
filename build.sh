#!/bin/sh
set -eu

CC=${CC:-cc}
WINCC=${WINCC:-i686-w64-mingw32-gcc}

COMMON_FLAGS="-std=c99 -pedantic -Wall -Wextra -Werror -O2"
WINDOWS_CFLAGS="-std=c99 -Wall -Wextra -Werror -O2 -fno-omit-frame-pointer"
NATIVE_FLAGS="-D_POSIX_C_SOURCE=200809L"
WINDOWS_FLAGS="-mcrtdll=msvcrt-os -UWINVER -U_WIN32_WINNT -UNDDDI_VERSION -DWINVER=0x0501 -D_WIN32_WINNT=0x0501 -DNTDDI_VERSION=0x05010000"
WINDOWS_LINK_FLAGS=${WINDOWS_LINK_FLAGS:-"-mwindows -static-libgcc -Wl,--major-subsystem-version=5 -Wl,--minor-subsystem-version=1 -Wl,--disable-dynamicbase -Wl,--disable-nxcompat"}
WINDOWS_SERVER_LINK_FLAGS=${WINDOWS_SERVER_LINK_FLAGS:-"-static-libgcc -Wl,--major-subsystem-version=5 -Wl,--minor-subsystem-version=1 -Wl,--disable-dynamicbase -Wl,--disable-nxcompat"}

PROTOCOL_SOURCES="src/common/endian.c src/common/protocol.c"
SERVER_COMMON_SOURCES="$PROTOCOL_SOURCES src/common/sockcompat.c src/common/config.c"
SERVER_SOURCES="src/server/main.c src/server/relay.c"
WINDOWS_XLIVE_SOURCES="src/windows/asi_loader/src/asi_loader.c src/windows/xlive.c src/windows/relay_client.c src/windows/crash.c src/windows/entry_hook.c src/windows/game_patch.c"

usage()
{
	cat <<'EOF'
Usage: sh build.sh [server|windows|test|all|clean]

Environment:
  CC       Native C compiler (default: cc)
  WINCC    32-bit MinGW compiler (default: i686-w64-mingw32-gcc)

No Make, CMake, Rust, C++, Python, or generated dependency files are used.
EOF
}

require_compiler()
{
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "error: required C compiler not found: $1" >&2
		exit 1
	fi
}

build_server()
{
	require_compiler "$CC"
	mkdir -p bin build
	echo "Building native server with $CC"
	server_libs=""
	server_output="bin/ivmp_server"
	case "$("$CC" -dumpmachine 2>/dev/null)" in
		*mingw*|*windows*|*msvc*)
			server_libs="-lws2_32"
			server_output="bin/ivmp_server.exe"
			;;
	esac
	# shellcheck disable=SC2086
	"$CC" $COMMON_FLAGS $NATIVE_FLAGS -Isrc/common \
		$SERVER_COMMON_SOURCES $SERVER_SOURCES \
		$server_libs \
		-o "$server_output"
}

build_windows_server()
{
	# shellcheck disable=SC2086
	require_compiler "${WINCC%% *}"
	mkdir -p bin build
	echo "Building 32-bit Windows server with $WINCC"
	# shellcheck disable=SC2086
	$WINCC $WINDOWS_CFLAGS $WINDOWS_FLAGS -Isrc/common $WINDOWS_SERVER_LINK_FLAGS \
		$SERVER_COMMON_SOURCES $SERVER_SOURCES \
		-lws2_32 \
		-o bin/ivmp_server.exe
}

build_windows()
{
	# shellcheck disable=SC2086
	require_compiler "${WINCC%% *}"
	mkdir -p bin build
	build_windows_xlive
	build_windows_gui
	build_windows_server
}

build_windows_xlive()
{
	if [ ! -f src/windows/asi_loader/src/5head/5head.h ]; then
		echo "error: asi_loader is missing; run: git submodule update --init --recursive" >&2
		exit 1
	fi
	echo "Building 32-bit Windows xlive shim with $WINCC"
	# shellcheck disable=SC2086
	$WINCC $WINDOWS_CFLAGS $WINDOWS_FLAGS -DASI_LOADER_EXPORT= -Isrc/common -shared $WINDOWS_LINK_FLAGS -Wl,--enable-stdcall-fixup \
		$PROTOCOL_SOURCES $WINDOWS_XLIVE_SOURCES src/windows/xlive.def \
		-lws2_32 -luser32 \
		-o bin/xlive.dll
}

build_windows_gui()
{
	echo "Building 32-bit Windows launcher with $WINCC"
	# shellcheck disable=SC2086
	$WINCC $WINDOWS_CFLAGS $WINDOWS_FLAGS $WINDOWS_LINK_FLAGS \
		src/windows/gui.c \
		-lcomdlg32 -lshell32 -luser32 -lgdi32 \
		-o bin/ivmp_gui.exe
}

build_tests()
{
	require_compiler "$CC"
	mkdir -p bin build
	echo "Building native tests with $CC"
	# shellcheck disable=SC2086
	"$CC" $COMMON_FLAGS $NATIVE_FLAGS -Isrc/common \
		src/common/endian.c src/common/protocol.c tests/test_protocol.c \
		-o build/test-protocol
	# shellcheck disable=SC2086
	"$CC" $COMMON_FLAGS $NATIVE_FLAGS -Isrc/common \
		src/common/endian.c src/common/protocol.c tests/test_client.c \
		-o build/test-client
}

run_tests()
{
	build_tests
	build/test-protocol
	sh tests/smoke.sh
}

mode=${1:-all}
case "$mode" in
	server)
		build_server
		;;
	windows)
		build_windows
		;;
	test)
		build_server
		run_tests
		;;
	all)
		build_server
		build_windows
		run_tests
		;;
	clean)
		rm -rf bin build
		;;
	-h|--help|help)
		usage
		;;
	*)
		echo "error: unknown build mode: $mode" >&2
		usage >&2
		exit 2
		;;
esac

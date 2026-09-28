#!/bin/sh
set -eu

if ! command -v wine >/dev/null 2>&1; then
	echo "wine not installed; skipping crash-handler test" >&2
	exit 0
fi

CC=${WINCC:-i686-w64-mingw32-gcc}
# shellcheck disable=SC2086
if ! $CC --version >/dev/null 2>&1; then
	echo "i686 Windows compiler not available; skipping crash-handler test" >&2
	exit 0
fi

WINEPREFIX=${WINEPREFIX:-/tmp/ivmp-wine}
WINEDEBUG=${WINEDEBUG:--all}
export WINEPREFIX WINEDEBUG
mkdir -p build
rm -f build/ivmp-crash.log
# shellcheck disable=SC2086
$CC -std=c99 -Wall -Wextra -Werror -O2 -fno-omit-frame-pointer \
	-mcrtdll=msvcrt-os -DWINVER=0x0501 -D_WIN32_WINNT=0x0501 \
	-DNTDDI_VERSION=0x05010000 -Isrc/common -Isrc/windows \
	tests/crash_probe.c src/windows/crash.c \
	-static-libgcc -o build/crash-probe.exe

set +e
wine build/crash-probe.exe >/dev/null 2>&1
status=$?
set -e
if [ "$status" -eq 0 ]; then
	echo "crash probe unexpectedly exited successfully" >&2
	exit 1
fi
if [ ! -s build/ivmp-crash.log ] || ! grep -q 'backtrace:' build/ivmp-crash.log; then
	echo "crash handler did not produce a backtrace report" >&2
	cat build/ivmp-crash.log 2>/dev/null || true
	exit 1
fi
echo "crash-handler test passed"

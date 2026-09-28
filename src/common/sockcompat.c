#include "sockcompat.h"

#if defined(_WIN32)

#include <stdio.h>

int ivmp_sockets_init(void)
{
	WSADATA data;

	return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

void ivmp_sockets_cleanup(void)
{
	(void)WSACleanup();
}

int ivmp_socket_errno(void)
{
	return WSAGetLastError();
}

const char *ivmp_socket_strerror(int code)
{
	static char buffer[256];
	DWORD length;

	length = FormatMessageA(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
		(DWORD)code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buffer,
		(DWORD)sizeof(buffer), NULL);
	if (length == 0u) {
		(void)snprintf(buffer, sizeof(buffer), "socket error %d", code);
		return buffer;
	}
	while (length > 0u &&
		(buffer[length - 1u] == '\r' || buffer[length - 1u] == '\n')) {
		buffer[--length] = '\0';
	}
	return buffer;
}

int ivmp_socket_close(int fd)
{
	return closesocket((SOCKET)fd) == 0 ? 0 : -1;
}

int ivmp_socket_set_nonblocking(int fd)
{
	u_long mode = 1ul;

	return ioctlsocket((SOCKET)fd, FIONBIO, &mode) == 0;
}

typedef BOOLEAN (WINAPI *ivmp_rtl_gen_random_fn)(PVOID buffer, ULONG length);

int ivmp_random_bytes(unsigned char *output, size_t length)
{
	static ivmp_rtl_gen_random_fn rtl_gen_random;

	if (rtl_gen_random == NULL) {
		HMODULE advapi = LoadLibraryA("advapi32.dll");
		void (*function)(void);

		if (advapi == NULL) {
			return 0;
		}
		function = (void (*)(void))GetProcAddress(advapi, "SystemFunction036");
		if (function == NULL) {
			return 0;
		}
		rtl_gen_random = (ivmp_rtl_gen_random_fn)function;
	}
	return rtl_gen_random(output, (ULONG)length) != FALSE;
}

uint64_t ivmp_monotonic_milliseconds(void)
{
	static DWORD previous_tick;
	static uint64_t total_ms;
	static int initialized;
	DWORD tick = GetTickCount();

	if (!initialized) {
		previous_tick = tick;
		total_ms = (uint64_t)tick;
		initialized = 1;
	}
	total_ms += (uint64_t)(DWORD)(tick - previous_tick);
	previous_tick = tick;
	return total_ms;
}

#else

#include <stdio.h>
#include <string.h>
#include <time.h>

int ivmp_sockets_init(void)
{
	(void)signal(SIGPIPE, SIG_IGN);
	return 1;
}

void ivmp_sockets_cleanup(void)
{
}

int ivmp_socket_errno(void)
{
	return errno;
}

const char *ivmp_socket_strerror(int code)
{
	return strerror(code);
}

int ivmp_socket_close(int fd)
{
	return close(fd);
}

int ivmp_socket_set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
		return 0;
	}
	return 1;
}

int ivmp_random_bytes(unsigned char *output, size_t length)
{
	FILE *source = fopen("/dev/urandom", "rb");
	size_t read_length;

	if (source == NULL) {
		return 0;
	}
	read_length = fread(output, 1u, length, source);
	(void)fclose(source);
	return read_length == length;
}

uint64_t ivmp_monotonic_milliseconds(void)
{
	static uint64_t last_ms;
	struct timespec value;
	uint64_t now_ms;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
		return last_ms;
	}
	now_ms = (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
	if (now_ms > last_ms) {
		last_ms = now_ms;
	}
	return last_ms;
}

#endif

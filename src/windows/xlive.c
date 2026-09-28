#include <winsock2.h>
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asi_loader/src/asi_loader.h"
#include "crash.h"
#include "endian.h"
#include "protocol.h"
#include "entry_hook.h"
#include "game_patch.h"
#include "relay_client.h"
#include "xlive_types.h"

static HINSTANCE module_instance;

static int xlive_log_path(char *path, size_t size)
{
	char module_path[MAX_PATH];
	char *separator;
	DWORD length;

	length = GetModuleFileNameA(module_instance, module_path,
		(DWORD)sizeof(module_path));
	if (length == 0u || length >= sizeof(module_path)) {
		return 0;
	}
	separator = strrchr(module_path, '\\');
	if (separator != NULL) {
		module_path[separator - module_path] = '\0';
	} else {
		module_path[0] = '\0';
	}
	(void)snprintf(path, size, "%s\\ivmp.log", module_path);
	return 1;
}

static void clear_xlive_log(void)
{
	char path[MAX_PATH + 32];
	HANDLE file;

	if (!xlive_log_path(path, sizeof(path))) {
		return;
	}
	file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file != INVALID_HANDLE_VALUE) {
		(void)CloseHandle(file);
	}
}

static void xlive_log(const char *message)
{
	char line[1024];
	HANDLE file;
	DWORD written;

	OutputDebugStringA(message);
	OutputDebugStringA("\r\n");
	if (!xlive_log_path(line, sizeof(line))) {
		return;
	}
	file = CreateFileA(line, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		return;
	}
	(void)wsprintfA(line, "%s\r\n", message);
	(void)WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
	(void)CloseHandle(file);
}

static int trace_enabled = -1;
static uint32_t random_state;

static void xlive_trace(const char *format, ...)
{
	char line[1024];
	va_list arguments;

	if (trace_enabled < 0) {
		char value[8];

		trace_enabled = GetEnvironmentVariableA("IVMP_TRACE", value,
					(DWORD)sizeof(value)) != 0u &&
			lstrcmpA(value, "0") != 0;
	}
	if (!trace_enabled) {
		return;
	}
	va_start(arguments, format);
	(void)wvsprintfA(line, format, arguments);
	va_end(arguments);
	xlive_log(line);
}

static void xlive_trace_entry(const char *name, unsigned int *calls)
{
	unsigned int count;

	if (trace_enabled == 0) {
		return;
	}
	count = ++*calls;
	if (count <= 5u || count % 1000u == 0u) {
		xlive_trace("%s #%u", name, count);
	}
}

#define XLIVE_TRACE_ENTRY() \
	do { \
		static unsigned int trace_calls; \
		xlive_trace_entry(__func__, &trace_calls); \
	} while (0)

static void xlive_unused(int count, ...)
{
	(void)count;
}

#define XLIVE_UNUSED(...) xlive_unused(0, __VA_ARGS__)

#define XLIVE_STUB(type, name, params, args, value) \
	type WINAPI name params \
	{ \
		XLIVE_TRACE_ENTRY(); \
		XLIVE_UNUSED args; \
		return value; \
	}

#define XLIVE_CONSTANT(type, name, value) \
	type WINAPI name(void) \
	{ \
		XLIVE_TRACE_ENTRY(); \
		return value; \
	}

#define XLIVE_FORWARD(type, name, params, call) \
	type WINAPI name params \
	{ \
		XLIVE_TRACE_ENTRY(); \
		return call; \
	}

static void trace_packet(const char *direction, const struct sockaddr *peer, int result)
{
	static LONG packets;
	LONG count;
	const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)(const void *)peer;
	int has_address = peer != NULL && peer->sa_family == AF_INET;

	if (trace_enabled == 0 || result == SOCKET_ERROR) {
		return;
	}
	count = InterlockedIncrement(&packets);
	if (count > 60 && count % 1000 != 0) {
		return;
	}
	xlive_trace("packet #%ld %s 0x%08lx:%u length=%d", (long)count, direction, has_address ? (unsigned long)ipv4->sin_addr.s_addr : 0ul, has_address ? (unsigned int)ntohs(ipv4->sin_port) : 0u, result);
}

static void fill_random(BYTE *output, size_t length)
{
	size_t index;

	if (random_state == 0u) {
		LARGE_INTEGER counter;

		(void)QueryPerformanceCounter(&counter);
		random_state = (uint32_t)counter.LowPart ^ (uint32_t)GetTickCount() ^
			((uint32_t)GetCurrentProcessId() << 16) ^ 0x9e3779b9u;
		if (random_state == 0u) {
			random_state = 1u;
		}
	}
	for (index = 0u; index < length; ++index) {
		random_state ^= random_state << 13;
		random_state ^= random_state >> 17;
		random_state ^= random_state << 5;
		output[index] = (BYTE)random_state;
	}
}

static const XUID offline_xuid = 0x0009000100000001ULL;

static void local_nickname(char *output, size_t size)
{
	DWORD length;

	if (size == 0u) {
		return;
	}
	length = GetEnvironmentVariableA("IVMP_NICKNAME", output,
		(DWORD)size);
	if (length == 0u || length >= size) {
		char full[128];

		length = GetEnvironmentVariableA("IVMP_NICKNAME", full,
			(DWORD)sizeof(full));
		if (length == 0u || length >= sizeof(full)) {
			(void)lstrcpynA(output, "Player", (int)size);
			return;
		}
		(void)lstrcpynA(output, full, (int)size);
	}
}

static XUID online_xuid(void)
{
	char nickname[XUSER_NAME_SIZE];
	const unsigned char *cursor;
	uint32_t hash = 2166136261u;

	local_nickname(nickname, sizeof(nickname));
	for (cursor = (const unsigned char *)nickname; *cursor != '\0';
		++cursor) {
		hash ^= (uint32_t)*cursor;
		hash *= 16777619u;
	}
	return 0x0009000000000000ULL | (XUID)hash;
}

static volatile LONG relay_init_state;
static volatile LONG relay_connect_state;
static volatile LONG relay_last_attempt;
static volatile LONG last_multiplayer_activity;
static volatile LONG session_active;
static volatile LONG single_player;

#define IVMP_CONTEXT_PRESENCE 0x00008001u
#define IVMP_PRESENCE_SINGLE_PLAYER 10u

static void request_relay(DWORD wait_ms);

static void relay_log(const char *message)
{
	static char previous[256];

	if (lstrcmpA(previous, message) == 0) {
		return;
	}
	(void)lstrcpynA(previous, message, (int)sizeof(previous));
	xlive_log(message);
}

static int relay_idle(void)
{
	DWORD now = GetTickCount();
	DWORD activity = (DWORD)last_multiplayer_activity;
	DWORD traffic = (DWORD)ivmp_last_game_traffic;

	if (session_active) {
		return 0;
	}
	if (single_player) {
		return 1;
	}
	return now - activity >= 60000u && now - traffic >= 60000u;
}

static void init_relay_state(void)
{
	char server_text[8];

	if (relay_init_state == 2) {
		return;
	}
	if (InterlockedCompareExchange(&relay_init_state, 1, 0) != 0) {
		while (relay_init_state != 2) {
			Sleep(1u);
		}
		return;
	}
	ivmp_relay_client_init(relay_log, request_relay, relay_idle);
	if (GetEnvironmentVariableA("IVMP_SERVER", server_text, (DWORD)sizeof(server_text)) == 0u) {
		(void)InterlockedExchange(&ivmp_relay_lost, 1);
	}
	if (ivmp_connection_id == 0u) {
		BYTE id[3];

		fill_random(id, sizeof(id));
		ivmp_connection_id = ((uint32_t)id[0] << 16) |
			((uint32_t)id[1] << 8) | (uint32_t)id[2];
		if (ivmp_connection_id == 0u || ivmp_connection_id == 0x00ffffffu) {
			ivmp_connection_id = 1u;
		}
	}
	(void)InterlockedExchange(&relay_init_state, 2);
}

static DWORD WINAPI relay_connect_thread(LPVOID parameter)
{
	char server_text[512];
	char nickname[128];

	(void)parameter;
	if (GetEnvironmentVariableA("IVMP_NICKNAME", nickname,
		    (DWORD)sizeof(nickname)) == 0u) {
		(void)strcpy(nickname, "Player");
	}
	if (GetEnvironmentVariableA("IVMP_SERVER", server_text,
		    (DWORD)sizeof(server_text)) != 0u &&
		ivmp_relay_client_connect(server_text, nickname)) {
		HANDLE heartbeat = CreateThread(NULL, 0u, ivmp_relay_heartbeat_thread,
			NULL, 0u, NULL);

		if (heartbeat != NULL) {
			(void)CloseHandle(heartbeat);
		}
	} else {
		(void)InterlockedExchange(&ivmp_relay_lost, 1);
	}
	(void)InterlockedExchange(&relay_connect_state, 0);
	return 0u;
}

static int start_relay_connect(DWORD now)
{
	char server_text[8];
	HANDLE thread;

	init_relay_state();
	if (ivmp_control_ready || GetEnvironmentVariableA("IVMP_SERVER", server_text, (DWORD)sizeof(server_text)) == 0u) {
		return 0;
	}
	if (relay_connect_state == 0 && (relay_last_attempt == 0 || now - (DWORD)relay_last_attempt >= 5000u) && InterlockedCompareExchange(&relay_connect_state, 1, 0) == 0) {
		(void)InterlockedExchange(&relay_last_attempt, (LONG)(now | 1u));
		if (!ivmp_relay_lost) {
			relay_log("multiplayer requested; connecting to the IVMP relay");
		}
		thread = CreateThread(NULL, 0u, relay_connect_thread, NULL, 0u, NULL);
		if (thread == NULL) {
			(void)InterlockedExchange(&relay_connect_state, 0);
		} else {
			(void)CloseHandle(thread);
		}
	}
	return 1;
}

static void request_relay(DWORD wait_ms)
{
	DWORD started = GetTickCount();

	init_relay_state();
	(void)InterlockedExchange(&last_multiplayer_activity, (LONG)started);
	if ((single_player && !session_active) || !start_relay_connect(started)) {
		return;
	}
	while (!ivmp_control_ready && relay_connect_state != 0 && GetTickCount() - started < wait_ms) {
		Sleep(10u);
	}
}

#define PBUFFER_MAGIC 0x58425058u
#define PBUFFER_VALID(buffer) ((buffer) != NULL && (buffer)->magic == PBUFFER_MAGIC)

static DWORD no_enumerator(PDWORD buffer_size, PHANDLE handle)
{
	if (buffer_size != NULL) {
		*buffer_size = 0u;
	}
	if (handle != NULL) {
		*handle = NULL;
	}
	return ERROR_NOT_FOUND;
}

static HRESULT copy_protected_data(const BYTE *in_buffer, DWORD in_size,
	BYTE *out_buffer, DWORD *out_size)
{
	if (out_size == NULL) {
		return E_INVALIDARG;
	}
	if (out_buffer != NULL && in_buffer != NULL) {
		DWORD copy_size = in_size < *out_size ? in_size : *out_size;

		memcpy(out_buffer, in_buffer, copy_size);
		*out_size = copy_size;
	} else {
		*out_size = in_size;
	}
	return S_OK;
}

static void complete_overlapped(PXOVERLAPPED overlapped, DWORD result)
{
	if (overlapped == NULL) {
		return;
	}
	overlapped->InternalHigh = 0u;
	overlapped->dwExtendedError = result;
	(void)InterlockedExchange((LONG *)&overlapped->InternalLow, (LONG)result);
	if (overlapped->hEvent != NULL) {
		(void)SetEvent(overlapped->hEvent);
	}
	if (overlapped->pCompletionRoutine != NULL) {
		overlapped->pCompletionRoutine((DWORD)overlapped->InternalLow,
			(DWORD)overlapped->InternalHigh,
			(DWORD)overlapped->dwCompletionContext);
	}
}

static DWORD finish_async(PXOVERLAPPED overlapped, DWORD result)
{
	if (overlapped == NULL) {
		return result;
	}
	complete_overlapped(overlapped, result);
	return ERROR_IO_PENDING;
}

int WINAPI XWSAStartup(WORD version_requested, LPWSADATA wsa_data)
{
	XLIVE_TRACE_ENTRY();
	init_relay_state();
	return WSAStartup(version_requested, wsa_data);
}

XLIVE_FORWARD(int, XWSACleanup, (void), WSACleanup())

SOCKET WINAPI XCreateSocket(int address_family, int type, int protocol)
{
	XLIVE_TRACE_ENTRY();
	init_relay_state();
	xlive_trace("XCreateSocket af=%d type=%d protocol=%d", address_family, type,
		protocol);
	if (protocol == IVMP_IPPROTO_VDP) {
		protocol = IPPROTO_UDP;
	}
	return socket(address_family, type, protocol);
}

int WINAPI XSocketClose(SOCKET s)
{
	XLIVE_TRACE_ENTRY();
	xlive_trace("XSocketClose %lu", (unsigned long)s);
	return ivmp_hook_closesocket(s);
}

XLIVE_FORWARD(int, XSocketShutdown, (SOCKET s, int how), shutdown(s, how))

XLIVE_FORWARD(int, XSocketIOCTLSocket, (SOCKET s, long cmd, u_long *argp), ioctlsocket(s, cmd, argp))

XLIVE_FORWARD(int, XSocketSetSockOpt, (SOCKET s, int level, int optname, const char *optval, int optlen), setsockopt(s, level, optname, optval, optlen))

int WINAPI XSocketGetSockName(SOCKET s, struct sockaddr *name, int *namelen)
{
	XLIVE_TRACE_ENTRY();
	int result = getsockname(s, name, namelen);
	u_short port;

	if (result != SOCKET_ERROR && name != NULL && name->sa_family == AF_INET &&
		namelen != NULL && *namelen >= (int)sizeof(struct sockaddr_in) &&
		ivmp_relay_virtual_port(s, &port)) {
		((struct sockaddr_in *)name)->sin_port = port;
	}
	return result;
}

SOCKET WINAPI XSocketBind(SOCKET s, const struct sockaddr *name, int namelen)
{
	XLIVE_TRACE_ENTRY();
	struct sockaddr_in any_port;
	const struct sockaddr *target = name;
	int target_length = namelen;
	u_short requested_port = 0u;
	int type = 0;
	int type_length = (int)sizeof(type);
	int result;

	if (ivmp_redirect_all && name != NULL &&
		namelen >= (int)sizeof(struct sockaddr_in) &&
		name->sa_family == AF_INET &&
		((const struct sockaddr_in *)name)->sin_port != 0u &&
		getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &type_length) !=
			SOCKET_ERROR &&
		type == SOCK_DGRAM) {
		requested_port = ((const struct sockaddr_in *)name)->sin_port;
		memcpy(&any_port, name, sizeof(any_port));
		any_port.sin_port = 0u;
		target = (const struct sockaddr *)&any_port;
		target_length = (int)sizeof(any_port);
	}
	result = bind(s, target, target_length);
	xlive_trace("XSocketBind socket=%lu port=%u result=%d error=%d%s",
		(unsigned long)s,
		name != NULL && namelen >= (int)sizeof(struct sockaddr_in)
			? (unsigned int)ntohs(
				  ((const struct sockaddr_in *)name)->sin_port)
			: 0u,
		result, result == SOCKET_ERROR ? WSAGetLastError() : 0,
		requested_port != 0u ? " (bound to a free port instead)" : "");
	if (result != SOCKET_ERROR) {
		if (requested_port != 0u) {
			ivmp_relay_set_virtual_port(s, requested_port);
		}
		init_relay_state();
		ivmp_relay_register_udp_socket(s);
	}
	return result;
}

XLIVE_FORWARD(int, XSocketConnect, (SOCKET s, const struct sockaddr *name, int namelen), connect(s, name, namelen))

XLIVE_FORWARD(int, XSocketListen, (SOCKET s, int backlog), listen(s, backlog))

XLIVE_FORWARD(SOCKET, XSocketAccept, (SOCKET s, struct sockaddr *addr, int *addrlen), accept(s, addr, addrlen))

XLIVE_FORWARD(int, XSocketSelect, (int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timeval *timeout), select(nfds, readfds, writefds, exceptfds, timeout))

XLIVE_FORWARD(int, XSocketRecv, (SOCKET s, char *buf, int len, int flags), ivmp_hook_recvfrom(s, buf, len, flags, NULL, NULL))

int WINAPI XSocketRecvFrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
	XLIVE_TRACE_ENTRY();
	int result = ivmp_hook_recvfrom(s, buf, len, flags, from, fromlen);

	trace_packet("from", from, result);
	return result;
}

XLIVE_FORWARD(int, XSocketSend, (SOCKET s, const char *buf, int len, int flags), send(s, buf, len, flags))

int WINAPI XSocketSendTo(SOCKET s, const char *buf, int len, int flags, struct sockaddr *to, int tolen)
{
	XLIVE_TRACE_ENTRY();
	int result;

	init_relay_state();
	result = ivmp_hook_sendto(s, buf, len, flags, to, tolen);
	trace_packet("to", to, result);
	return result;
}

XLIVE_FORWARD(LONG, XSocketInet_Addr, (const char *cp), inet_addr(cp))

XLIVE_FORWARD(INT, XWSAGetLastError, (void), WSAGetLastError())

XLIVE_FORWARD(WORD, XSocketNTOHS, (WORD netshort), ntohs(netshort))

XLIVE_FORWARD(DWORD, XSocketNTOHL, (DWORD netlong), ntohl(netlong))

int WINAPI XNetStartup(void *params)
{
	XLIVE_TRACE_ENTRY();
	xlive_trace("XNetStartup");
	(void)params;
	init_relay_state();
	return 0;
}

INT WINAPI XNetCleanup(void)
{
	XLIVE_TRACE_ENTRY();
	xlive_trace("XNetCleanup");
	return 0;
}

INT WINAPI XNetXnAddrToInAddr(const XNADDR *xnaddr, const XNKID *xnkid,
	IN_ADDR *out_address)
{
	XLIVE_TRACE_ENTRY();
	(void)xnkid;
	if (xnaddr == NULL || out_address == NULL) {
		return WSAEINVAL;
	}
	*out_address = xnaddr->ina;
	xlive_trace("XNetXnAddrToInAddr -> 0x%08lx",
		(unsigned long)out_address->s_addr);
	return 0;
}

INT WINAPI XNetServerToInAddr(const IN_ADDR server_address, DWORD service_id,
	IN_ADDR *out_address)
{
	XLIVE_TRACE_ENTRY();
	(void)server_address;
	(void)service_id;
	if (out_address != NULL) {
		*out_address = ivmp_server_address.sin_addr;
	}
	return 0;
}

int WINAPI XNetUnregisterInAddr(DWORD address)
{
	XLIVE_TRACE_ENTRY();
	xlive_trace("XNetUnregisterInAddr 0x%08lx", (unsigned long)address);
	(void)address;
	return 0;
}

int WINAPI XNetGetConnectStatus(DWORD address)
{
	XLIVE_TRACE_ENTRY();
	xlive_trace("XNetGetConnectStatus 0x%08lx", (unsigned long)address);
	(void)address;
	return 0;
}

#define IVMP_MAX_CONTEXTS 32u
#define IVMP_MAX_PROPERTIES 64u
#define IVMP_MAX_QOS_DATA 1024u
#define IVMP_MAX_REMOTE_SESSIONS 64u
#define IVMP_SESSION_MAGIC 0x32535649u
#define IVMP_PROPERTY_GAMER_HOSTNAME 0x40008109u
#define IVMP_SEARCH_TIMEOUT_MS 2000u

struct remote_session {
	uint32_t connection_id;
	uint32_t serial;
	char host_name[XUSER_NAME_SIZE];
	XSESSION_INFO info;
	DWORD flags;
	DWORD max_public;
	DWORD max_private;
	DWORD filled_public;
	DWORD filled_private;
	DWORD context_count;
	XUSER_CONTEXT contexts[IVMP_MAX_CONTEXTS];
	DWORD property_count;
	XUSER_PROPERTY properties[IVMP_MAX_PROPERTIES];
	DWORD qos_length;
	BYTE qos[IVMP_MAX_QOS_DATA];
};

struct search_request {
	DWORD procedure;
	DWORD context_count;
	XUSER_CONTEXT contexts[IVMP_MAX_CONTEXTS];
	DWORD num_results;
	DWORD buffer_size;
	PXSESSION_SEARCHRESULT_HEADER results;
	PXOVERLAPPED overlapped;
};

static CRITICAL_SECTION session_state_lock;
static XSESSION_LOCAL_DETAILS session_details;
static int session_hosting;
static DWORD session_members;
static XUSER_CONTEXT user_contexts[IVMP_MAX_CONTEXTS];
static DWORD user_context_count;
static XUSER_PROPERTY user_properties[IVMP_MAX_PROPERTIES];
static DWORD user_property_count;
static BYTE qos_data[IVMP_MAX_QOS_DATA];
static DWORD qos_data_length;
static struct remote_session remote_sessions[IVMP_MAX_REMOTE_SESSIONS];
static DWORD remote_session_count;
static unsigned char session_list_buffer[IVMP_MAX_CONTROL_PAYLOAD];

typedef char ivmp_xuser_property_size_check[sizeof(XUSER_PROPERTY) == 24u ? 1 : -1];
typedef char ivmp_search_result_size_check[sizeof(XSESSION_SEARCHRESULT) == 92u ? 1 : -1];
typedef char ivmp_xnqosinfo_size_check[sizeof(XNQOSINFO) == 24u ? 1 : -1];

static void store_user_context(DWORD context_id, DWORD value)
{
	DWORD index;

	if (context_id == IVMP_CONTEXT_PRESENCE) {
		LONG now_single = value == IVMP_PRESENCE_SINGLE_PLAYER;

		if (InterlockedExchange(&single_player, now_single) != now_single) {
			xlive_trace("presence %lu: %s", (unsigned long)value,
				now_single ? "single player" : "multiplayer");
		}
	}

	EnterCriticalSection(&session_state_lock);
	for (index = 0u; index < user_context_count; ++index) {
		if (user_contexts[index].dwContextId == context_id) {
			break;
		}
	}
	if (index < IVMP_MAX_CONTEXTS) {
		user_contexts[index].dwContextId = context_id;
		user_contexts[index].dwValue = value;
		if (index == user_context_count) {
			++user_context_count;
		}
	}
	LeaveCriticalSection(&session_state_lock);
}

static void store_user_property(DWORD property_id, DWORD size,
	const void *value)
{
	BYTE type = (BYTE)((property_id >> 28) & 0x0fu);
	DWORD index;

	if (value == NULL || size == 0u || size > 8u ||
		(type != 1u && type != 2u && type != 3u && type != 5u &&
			type != 7u)) {
		return;
	}
	EnterCriticalSection(&session_state_lock);
	for (index = 0u; index < user_property_count; ++index) {
		if (user_properties[index].dwPropertyId == property_id) {
			break;
		}
	}
	if (index < IVMP_MAX_PROPERTIES) {
		memset(&user_properties[index], 0, sizeof(user_properties[index]));
		user_properties[index].dwPropertyId = property_id;
		user_properties[index].value.type = type;
		memcpy(&user_properties[index].value.i64Data, value, size);
		if (index == user_property_count) {
			++user_property_count;
		}
	}
	LeaveCriticalSection(&session_state_lock);
}

static void append_bytes(unsigned char *record, size_t *offset,
	const void *data, size_t length)
{
	memcpy(record + *offset, data, length);
	*offset += length;
}

static void append_dword(unsigned char *record, size_t *offset, DWORD value)
{
	append_bytes(record, offset, &value, sizeof(value));
}

static void publish_session_state(void)
{
	static int published;
	unsigned char record[IVMP_MAX_SESSION_RECORD];
	size_t offset = 0u;
	DWORD index;
	DWORD filled_public;
	DWORD filled_private = 0u;

	EnterCriticalSection(&session_state_lock);
	if (!session_hosting) {
		int was_published = published;

		published = 0;
		LeaveCriticalSection(&session_state_lock);
		if (was_published) {
			ivmp_relay_publish_session(NULL, 0u);
			xlive_trace("session unpublished");
		}
		return;
	}
	if (!published) {
		xlive_trace("session published");
	}
	published = 1;
	filled_public = session_members;
	if (filled_public > session_details.dwMaxPublicSlots) {
		filled_private = filled_public - session_details.dwMaxPublicSlots;
		filled_public = session_details.dwMaxPublicSlots;
	}
	append_dword(record, &offset, IVMP_SESSION_MAGIC);
	{
		char host_name[XUSER_NAME_SIZE];

		memset(host_name, 0, sizeof(host_name));
		local_nickname(host_name, sizeof(host_name));
		append_bytes(record, &offset, host_name, sizeof(host_name));
	}
	append_bytes(record, &offset, &session_details.sessionInfo,
		sizeof(session_details.sessionInfo));
	append_dword(record, &offset, session_details.dwFlags);
	append_dword(record, &offset, session_details.dwMaxPublicSlots);
	append_dword(record, &offset, session_details.dwMaxPrivateSlots);
	append_dword(record, &offset, filled_public);
	append_dword(record, &offset, filled_private);
	append_dword(record, &offset, user_context_count);
	for (index = 0u; index < user_context_count; ++index) {
		append_dword(record, &offset, user_contexts[index].dwContextId);
		append_dword(record, &offset, user_contexts[index].dwValue);
	}
	append_dword(record, &offset, user_property_count);
	for (index = 0u; index < user_property_count; ++index) {
		append_dword(record, &offset, user_properties[index].dwPropertyId);
		append_bytes(record, &offset, &user_properties[index].value.type, 1u);
		append_bytes(record, &offset, &user_properties[index].value.i64Data,
			8u);
	}
	append_dword(record, &offset, qos_data_length);
	append_bytes(record, &offset, qos_data, qos_data_length);
	LeaveCriticalSection(&session_state_lock);
	ivmp_relay_publish_session(record, offset);
}

static int read_bytes(const unsigned char *data, size_t length,
	size_t *offset, void *output, size_t size)
{
	if (length - *offset < size || *offset > length) {
		return 0;
	}
	memcpy(output, data + *offset, size);
	*offset += size;
	return 1;
}

static int parse_session_record(const unsigned char *data, size_t length,
	struct remote_session *session)
{
	size_t offset = 0u;
	DWORD magic;
	DWORD index;

	memset(session, 0, sizeof(*session));
	if (!read_bytes(data, length, &offset, &magic, 4u) ||
		magic != IVMP_SESSION_MAGIC ||
		!read_bytes(data, length, &offset, session->host_name,
			sizeof(session->host_name)) ||
		!read_bytes(data, length, &offset, &session->info,
			sizeof(session->info)) ||
		!read_bytes(data, length, &offset, &session->flags, 4u) ||
		!read_bytes(data, length, &offset, &session->max_public, 4u) ||
		!read_bytes(data, length, &offset, &session->max_private, 4u) ||
		!read_bytes(data, length, &offset, &session->filled_public, 4u) ||
		!read_bytes(data, length, &offset, &session->filled_private, 4u) ||
		!read_bytes(data, length, &offset, &session->context_count, 4u) ||
		session->context_count > IVMP_MAX_CONTEXTS) {
		return 0;
	}
	for (index = 0u; index < session->context_count; ++index) {
		if (!read_bytes(data, length, &offset,
			    &session->contexts[index].dwContextId, 4u) ||
			!read_bytes(data, length, &offset,
				&session->contexts[index].dwValue, 4u)) {
			return 0;
		}
	}
	if (!read_bytes(data, length, &offset, &session->property_count, 4u) ||
		session->property_count > IVMP_MAX_PROPERTIES) {
		return 0;
	}
	for (index = 0u; index < session->property_count; ++index) {
		XUSER_PROPERTY *property = &session->properties[index];

		if (!read_bytes(data, length, &offset, &property->dwPropertyId, 4u) ||
			!read_bytes(data, length, &offset, &property->value.type, 1u) ||
			!read_bytes(data, length, &offset, &property->value.i64Data,
				8u)) {
			return 0;
		}
	}
	if (!read_bytes(data, length, &offset, &session->qos_length, 4u) ||
		session->qos_length > IVMP_MAX_QOS_DATA ||
		!read_bytes(data, length, &offset, session->qos,
			session->qos_length)) {
		return 0;
	}
	session->host_name[sizeof(session->host_name) - 1u] = '\0';
	return 1;
}

static void store_remote_session(DWORD *stored, uint32_t connection_id,
	uint32_t serial, const unsigned char *data,
	size_t length)
{
	struct remote_session *slot;
	DWORD index;

	if (*stored >= IVMP_MAX_REMOTE_SESSIONS) {
		return;
	}
	slot = &remote_sessions[*stored];
	if (!parse_session_record(data, length, slot)) {
		return;
	}
	slot->connection_id = connection_id;
	slot->serial = serial;
	for (index = 0u; index < *stored; ++index) {
		if (memcmp(remote_sessions[index].info.sessionID.ab,
			    slot->info.sessionID.ab,
			    sizeof(slot->info.sessionID.ab)) == 0) {
			if (serial > remote_sessions[index].serial) {
				remote_sessions[index] = *slot;
			}
			return;
		}
	}
	++*stored;
}

static int refresh_remote_sessions(void)
{
	size_t length = 0u;
	size_t offset = 4u;
	uint32_t count;
	uint32_t index;
	DWORD stored = 0u;

	if (!ivmp_relay_query_sessions(session_list_buffer,
		    sizeof(session_list_buffer), &length,
		    IVMP_SEARCH_TIMEOUT_MS) ||
		length < 4u) {
		return 0;
	}
	count = ivmp_read_u32_be(session_list_buffer);
	EnterCriticalSection(&session_state_lock);
	for (index = 0u; index < count && offset + 12u <= length; ++index) {
		uint32_t connection_id = ivmp_read_u32_be(session_list_buffer +
			offset);
		uint32_t serial = ivmp_read_u32_be(session_list_buffer + offset +
			4u);
		uint32_t record_length = ivmp_read_u32_be(session_list_buffer +
			offset + 8u);

		offset += 12u;
		if (record_length > length - offset) {
			break;
		}
		if (connection_id != ivmp_connection_id) {
			store_remote_session(&stored, connection_id, serial,
				session_list_buffer + offset, record_length);
		}
		offset += record_length;
	}
	remote_session_count = stored;
	LeaveCriticalSection(&session_state_lock);
	return 1;
}

static DWORD search_result_capacity(DWORD num_results)
{
	if (num_results > IVMP_MAX_REMOTE_SESSIONS) {
		num_results = IVMP_MAX_REMOTE_SESSIONS;
	}
	return (DWORD)(sizeof(XSESSION_SEARCHRESULT_HEADER) + 8u +
		num_results * (sizeof(XSESSION_SEARCHRESULT) + (IVMP_MAX_PROPERTIES + 1u) * sizeof(XUSER_PROPERTY) + IVMP_MAX_CONTEXTS * sizeof(XUSER_CONTEXT) + XUSER_NAME_SIZE * sizeof(WCHAR)));
}

static int session_matches_contexts(const struct remote_session *remote,
	const struct search_request *request)
{
	DWORD wanted;
	DWORD index;

	for (wanted = 0u; wanted < request->context_count; ++wanted) {
		for (index = 0u; index < remote->context_count; ++index) {
			if (remote->contexts[index].dwContextId ==
				request->contexts[wanted].dwContextId) {
				if (remote->contexts[index].dwValue !=
					request->contexts[wanted].dwValue) {
					return 0;
				}
				break;
			}
		}
	}
	return 1;
}

static DWORD fill_search_results(const struct search_request *request,
	DWORD *skipped)
{
	PXSESSION_SEARCHRESULT_HEADER header = request->results;
	BYTE *base = (BYTE *)header;
	BYTE *cursor;
	BYTE *end = base + request->buffer_size;
	DWORD limit = request->num_results;
	DWORD count = 0u;
	DWORD index;

	*skipped = 0u;
	EnterCriticalSection(&session_state_lock);
	if (limit > remote_session_count) {
		limit = remote_session_count;
	}
	header->pResults = (XSESSION_SEARCHRESULT *)(void *)(header + 1);
	cursor = (BYTE *)(header->pResults + limit);
	cursor = base + (((size_t)(cursor - base) + 7u) & ~(size_t)7u);
	for (index = 0u; index < remote_session_count && count < limit; ++index) {
		const struct remote_session *remote = &remote_sessions[index];
		XSESSION_SEARCHRESULT *result = &header->pResults[count];
		size_t properties_size = (remote->property_count + 1u) *
			sizeof(XUSER_PROPERTY);
		size_t contexts_size = remote->context_count * sizeof(XUSER_CONTEXT);
		size_t name_size = (strlen(remote->host_name) + 1u) * sizeof(WCHAR);
		XUSER_PROPERTY *name_property;
		WCHAR *name;
		size_t character;

		if (!session_matches_contexts(remote, request)) {
			++*skipped;
			continue;
		}
		if ((size_t)(end - cursor) <
			properties_size + contexts_size + name_size) {
			break;
		}
		memset(result, 0, sizeof(*result));
		result->info = remote->info;
		result->dwFilledPublicSlots = remote->filled_public;
		result->dwFilledPrivateSlots = remote->filled_private;
		result->dwOpenPublicSlots = remote->max_public > remote->filled_public
			? remote->max_public -
				remote->filled_public
			: 0u;
		result->dwOpenPrivateSlots =
			remote->max_private > remote->filled_private
			? remote->max_private - remote->filled_private
			: 0u;
		result->cProperties = remote->property_count + 1u;
		result->pProperties = (PXUSER_PROPERTY)(void *)cursor;
		memcpy(cursor, remote->properties,
			remote->property_count * sizeof(XUSER_PROPERTY));
		name_property = result->pProperties + remote->property_count;
		cursor += properties_size;
		result->cContexts = remote->context_count;
		result->pContexts = (PXUSER_CONTEXT)(void *)cursor;
		memcpy(cursor, remote->contexts, contexts_size);
		cursor += contexts_size;
		name = (WCHAR *)(void *)cursor;
		for (character = 0u; character * sizeof(WCHAR) < name_size;
			++character) {
			name[character] = (WCHAR)(unsigned char)remote->host_name[character];
		}
		cursor += name_size;
		memset(name_property, 0, sizeof(*name_property));
		name_property->dwPropertyId = IVMP_PROPERTY_GAMER_HOSTNAME;
		name_property->value.type = 4u;
		name_property->value.string.cbData = (DWORD)name_size;
		name_property->value.string.pwszData = name;
		++count;
	}
	header->dwSearchResults = count;
	LeaveCriticalSection(&session_state_lock);
	return count;
}

static void run_search(struct search_request *request)
{
	int refreshed;
	DWORD skipped;
	DWORD count;

	request_relay(10000u);
	refreshed = refresh_remote_sessions();
	count = fill_search_results(request, &skipped);

	xlive_trace("XSessionSearchEx procedure=%lu max=%lu relay=%s -> %lu sessions, %lu with other contexts",
		(unsigned long)request->procedure,
		(unsigned long)request->num_results,
		refreshed ? "ok" : "no reply", (unsigned long)count,
		(unsigned long)skipped);
}

static void change_session_members(LONG delta)
{
	EnterCriticalSection(&session_state_lock);
	if (delta < 0 && (DWORD)(-delta) > session_members) {
		session_members = 0u;
	} else {
		session_members = (DWORD)((LONG)session_members + delta);
	}
	LeaveCriticalSection(&session_state_lock);
	publish_session_state();
}

static DWORD WINAPI search_thread(LPVOID parameter)
{
	struct search_request *request = (struct search_request *)parameter;

	run_search(request);
	complete_overlapped(request->overlapped, ERROR_SUCCESS);
	free(request);
	return 0u;
}

static const struct remote_session *find_remote_session(const XNKID *id,
	const XNADDR *address)
{
	DWORD index;

	for (index = 0u; id != NULL && index < remote_session_count; ++index) {
		if (memcmp(remote_sessions[index].info.sessionID.ab, id->ab,
			    sizeof(id->ab)) == 0) {
			return &remote_sessions[index];
		}
	}
	for (index = 0u; address != NULL && index < remote_session_count;
		++index) {
		if (remote_sessions[index].info.hostAddress.ina.s_addr ==
			address->ina.s_addr) {
			return &remote_sessions[index];
		}
	}
	return NULL;
}

static const struct remote_session *lookup_target(const XNKID **session_ids,
	const XNADDR **addresses,
	UINT index)
{
	return find_remote_session(session_ids != NULL ? session_ids[index] : NULL,
		addresses != NULL ? addresses[index] : NULL);
}

DWORD WINAPI XNetQosListen(XNKID *session_id, PBYTE data, UINT data_length,
	DWORD bits_per_second, DWORD flags)
{
	XLIVE_TRACE_ENTRY();
	request_relay(0u);
	xlive_trace("XNetQosListen flags=0x%08lx length=%u", (unsigned long)flags,
		data_length);
	(void)session_id;
	(void)bits_per_second;
	EnterCriticalSection(&session_state_lock);
	if ((flags & XNET_QOS_LISTEN_SET_DATA) != 0u) {
		qos_data_length = 0u;
		if (data != NULL && data_length <= IVMP_MAX_QOS_DATA) {
			memcpy(qos_data, data, data_length);
			qos_data_length = data_length;
		}
	}
	if ((flags & XNET_QOS_LISTEN_RELEASE) != 0u) {
		qos_data_length = 0u;
	}
	LeaveCriticalSection(&session_state_lock);
	publish_session_state();
	return 0;
}

DWORD WINAPI XNetQosLookup(UINT address_count, const XNADDR **addresses,
	const XNKID **session_ids, const XNKEY **keys,
	UINT server_count, const IN_ADDR *servers,
	const DWORD *service_ids, UINT probes,
	DWORD bits_per_second, DWORD flags, HANDLE event,
	XNQOS **result)
{
	XLIVE_TRACE_ENTRY();
	UINT total = address_count + server_count;
	UINT index;
	UINT found = 0u;
	size_t size;
	XNQOS *qos;
	XNQOSINFO *infos;
	BYTE *data_cursor;
	int refreshed = 0;

	(void)keys;
	(void)servers;
	(void)service_ids;
	(void)bits_per_second;
	(void)flags;
	if (result == NULL || total == 0u || total > 1024u) {
		return (DWORD)WSAEINVAL;
	}
	for (index = 0u; index < address_count && !refreshed; ++index) {
		EnterCriticalSection(&session_state_lock);
		if (lookup_target(session_ids, addresses, index) == NULL) {
			refreshed = 1;
		}
		LeaveCriticalSection(&session_state_lock);
	}
	if (refreshed) {
		request_relay(5000u);
		(void)refresh_remote_sessions();
	}
	EnterCriticalSection(&session_state_lock);
	size = sizeof(XNQOS) + total * sizeof(XNQOSINFO);
	for (index = 0u; index < address_count; ++index) {
		const struct remote_session *remote =
			lookup_target(session_ids, addresses, index);

		if (remote != NULL) {
			size += remote->qos_length;
		}
	}
	qos = (XNQOS *)calloc(1u, size);
	if (qos == NULL) {
		LeaveCriticalSection(&session_state_lock);
		return (DWORD)WSAENOBUFS;
	}
	qos->cxnqos = total;
	qos->cxnqosPending = 0u;
	infos = qos->axnqosinfo;
	data_cursor = (BYTE *)qos + sizeof(XNQOS) + total * sizeof(XNQOSINFO);
	for (index = 0u; index < total; ++index) {
		XNQOSINFO *info = infos + index;
		const struct remote_session *remote = NULL;

		if (index < address_count) {
			remote = lookup_target(session_ids, addresses, index);
		}
		info->cProbesXmit = (WORD)(probes != 0u ? probes : 1u);
		info->wRttMinInMsecs = 20u;
		info->wRttMedInMsecs = 30u;
		info->dwUpBitsPerSec = 1000000u;
		info->dwDnBitsPerSec = 1000000u;
		if (index >= address_count) {
			info->bFlags = XNET_XNQOSINFO_COMPLETE |
				XNET_XNQOSINFO_TARGET_CONTACTED;
			info->cProbesRecv = info->cProbesXmit;
		} else if (remote != NULL) {
			++found;
			info->bFlags = XNET_XNQOSINFO_COMPLETE |
				XNET_XNQOSINFO_TARGET_CONTACTED;
			info->cProbesRecv = info->cProbesXmit;
			if (remote->qos_length != 0u) {
				info->bFlags |= XNET_XNQOSINFO_DATA_RECEIVED;
				info->cbData = (WORD)remote->qos_length;
				info->pbData = data_cursor;
				memcpy(data_cursor, remote->qos, remote->qos_length);
				data_cursor += remote->qos_length;
			}
		} else {
			info->bFlags = XNET_XNQOSINFO_COMPLETE |
				XNET_XNQOSINFO_TARGET_DISABLED;
		}
	}
	LeaveCriticalSection(&session_state_lock);
	*result = qos;
	xlive_trace("XNetQosLookup addresses=%u servers=%u found=%u known=%lu",
		address_count, server_count, found,
		(unsigned long)remote_session_count);
	if (event != NULL) {
		(void)SetEvent(event);
	}
	return 0;
}

DWORD WINAPI XNetQosRelease(XNQOS *qos)
{
	XLIVE_TRACE_ENTRY();
	free(qos);
	return 0;
}

DWORD WINAPI XNetGetTitleXnAddr(XNADDR *out_address)
{
	XLIVE_TRACE_ENTRY();
	uint32_t connection_id;

	init_relay_state();
	if (out_address == NULL) {
		return XNET_GET_XNADDR_NONE;
	}
	connection_id = ivmp_connection_id;
	memset(out_address, 0, sizeof(*out_address));
	out_address->ina.s_addr = ivmp_relay_peer_address(connection_id);
	out_address->inaOnline = out_address->ina;
	out_address->abEnet[0] = 0x02u;
	out_address->abEnet[1] = 0x47u;
	out_address->abEnet[2] = (BYTE)(connection_id >> 24);
	out_address->abEnet[3] = (BYTE)(connection_id >> 16);
	out_address->abEnet[4] = (BYTE)(connection_id >> 8);
	out_address->abEnet[5] = (BYTE)connection_id;
	return XNET_GET_XNADDR_ETHERNET | XNET_GET_XNADDR_STATIC |
		XNET_GET_XNADDR_GATEWAY | XNET_GET_XNADDR_DNS |
		XNET_GET_XNADDR_ONLINE;
}

DWORD WINAPI XNetGetEthernetLinkStatus(void)
{
	XLIVE_TRACE_ENTRY();
	return XNET_ETHERNET_LINK_ACTIVE;
}

DWORD WINAPI XNetSetSystemLinkPort(DWORD port)
{
	XLIVE_TRACE_ENTRY();
	(void)port;
	return 0;
}

BOOL WINAPI XNotifyGetNext(HANDLE notification, DWORD filter, PDWORD id,
	PULONG_PTR param)
{
	XLIVE_TRACE_ENTRY();
	static LONG reported_lost;
	LONG lost;

	(void)notification;
	init_relay_state();
	lost = ivmp_relay_lost;
	if (lost) {
		(void)start_relay_connect(GetTickCount());
	}
	if (lost == reported_lost || (filter != 0u && filter != XN_SYS_SIGNINCHANGED) || id == NULL) {
		return FALSE;
	}
	reported_lost = lost;
	relay_log(lost ? "IVMP relay unavailable; multiplayer is offline" : "IVMP relay reachable; multiplayer is online");
	*id = XN_SYS_SIGNINCHANGED;
	if (param != NULL) {
		*param = 1u;
	}
	return TRUE;
}

DWORD WINAPI XNotifyPositionUI(DWORD position)
{
	XLIVE_TRACE_ENTRY();
	(void)position;
	return 0;
}

DWORD WINAPI XGetOverlappedExtendedError(PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	if (overlapped == NULL) {
		return ERROR_INVALID_PARAMETER;
	}
	return overlapped->dwExtendedError;
}

DWORD WINAPI XGetOverlappedResult(PXOVERLAPPED overlapped, DWORD *result,
	DWORD wait)
{
	XLIVE_TRACE_ENTRY();
	if (overlapped == NULL) {
		return ERROR_INVALID_PARAMETER;
	}
	if (overlapped->InternalLow == ERROR_IO_PENDING) {
		if (!wait || overlapped->hEvent == NULL) {
			return ERROR_IO_INCOMPLETE;
		}
		(void)WaitForSingleObject(overlapped->hEvent, INFINITE);
	}
	if (result != NULL) {
		*result = (DWORD)overlapped->InternalHigh;
	}
	return overlapped->dwExtendedError;
}

int WINAPI XHVCreateEngine(void *init_params, PHANDLE worker_thread,
	void **engine)
{
	XLIVE_TRACE_ENTRY();
	(void)init_params;
	if (worker_thread != NULL) {
		*worker_thread = NULL;
	}
	if (engine != NULL) {
		*engine = NULL;
	}
	return (int)E_FAIL;
}

LONG WINAPI XLivePBufferAllocate(DWORD size, ivmp_xlive_pbuffer **buffer)
{
	XLIVE_TRACE_ENTRY();
	ivmp_xlive_pbuffer *created;

	if (buffer == NULL) {
		return (LONG)E_INVALIDARG;
	}
	created = (ivmp_xlive_pbuffer *)malloc(sizeof(*created));
	if (created == NULL) {
		return (LONG)E_OUTOFMEMORY;
	}
	created->dwSize = size;
	created->magic = PBUFFER_MAGIC;
	created->pbData = (LPBYTE)calloc(1u, size > 0u ? size : 1u);
	created->id = (HANDLE)created;
	if (created->pbData == NULL) {
		free(created);
		return (LONG)E_OUTOFMEMORY;
	}
	*buffer = created;
	return ERROR_SUCCESS;
}

DWORD WINAPI XLivePBufferFree(ivmp_xlive_pbuffer *buffer)
{
	XLIVE_TRACE_ENTRY();
	if (!PBUFFER_VALID(buffer)) {
		return ERROR_INVALID_HANDLE;
	}
	free(buffer->pbData);
	buffer->magic = 0u;
	free(buffer);
	return ERROR_SUCCESS;
}

DWORD WINAPI XLivePBufferGetByte(ivmp_xlive_pbuffer *buffer, DWORD offset,
	BYTE *value)
{
	XLIVE_TRACE_ENTRY();
	if (!PBUFFER_VALID(buffer) || value == NULL ||
		offset >= buffer->dwSize) {
		return ERROR_INVALID_PARAMETER;
	}
	*value = buffer->pbData[offset];
	return ERROR_SUCCESS;
}

DWORD WINAPI XLivePBufferSetByte(ivmp_xlive_pbuffer *buffer, DWORD offset,
	BYTE value)
{
	XLIVE_TRACE_ENTRY();
	if (!PBUFFER_VALID(buffer) ||
		offset >= buffer->dwSize) {
		return ERROR_INVALID_PARAMETER;
	}
	buffer->pbData[offset] = value;
	return ERROR_SUCCESS;
}

DWORD WINAPI XLivePBufferGetDWORD(ivmp_xlive_pbuffer *buffer, DWORD offset,
	DWORD *value)
{
	XLIVE_TRACE_ENTRY();
	if (!PBUFFER_VALID(buffer) || value == NULL ||
		offset + sizeof(DWORD) > buffer->dwSize) {
		return ERROR_INVALID_PARAMETER;
	}
	memcpy(value, buffer->pbData + offset, sizeof(DWORD));
	return ERROR_SUCCESS;
}

DWORD WINAPI XLivePBufferSetDWORD(ivmp_xlive_pbuffer *buffer, DWORD offset,
	DWORD value)
{
	XLIVE_TRACE_ENTRY();
	if (!PBUFFER_VALID(buffer) ||
		offset + sizeof(DWORD) > buffer->dwSize) {
		return ERROR_INVALID_PARAMETER;
	}
	memcpy(buffer->pbData + offset, &value, sizeof(DWORD));
	return ERROR_SUCCESS;
}

DWORD WINAPI XLivePBufferGetByteArray(ivmp_xlive_pbuffer *buffer,
	DWORD offset, BYTE *destination,
	DWORD size)
{
	XLIVE_TRACE_ENTRY();
	if (!PBUFFER_VALID(buffer) ||
		destination == NULL || offset + size > buffer->dwSize) {
		return ERROR_INVALID_PARAMETER;
	}
	memcpy(destination, buffer->pbData + offset, size);
	return ERROR_SUCCESS;
}

DWORD WINAPI XLivePBufferSetByteArray(ivmp_xlive_pbuffer *buffer,
	DWORD offset, BYTE *source, DWORD size)
{
	XLIVE_TRACE_ENTRY();
	if (!PBUFFER_VALID(buffer) || source == NULL ||
		offset + size > buffer->dwSize) {
		return ERROR_INVALID_PARAMETER;
	}
	memcpy(buffer->pbData + offset, source, size);
	return ERROR_SUCCESS;
}

HRESULT WINAPI XLiveUnprotectData(BYTE *in_buffer, DWORD in_size,
	BYTE *out_buffer, DWORD *out_size,
	PHANDLE handle)
{
	XLIVE_TRACE_ENTRY();
	if (handle != NULL) {
		*handle = NULL;
	}
	return copy_protected_data(in_buffer, in_size, out_buffer, out_size);
}

DWORD WINAPI XLiveCreateProtectedDataContext(DWORD *type, PHANDLE handle)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(type);
	if (handle != NULL) {
		*handle = (HANDLE)1;
	}
	return ERROR_SUCCESS;
}

DWORD WINAPI XLiveQueryProtectedDataInformation(HANDLE handle, DWORD *info)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(handle);
	if (info != NULL) {
		*info = 0u;
	}
	return ERROR_SUCCESS;
}

BOOL WINAPI XCloseHandle(HANDLE object)
{
	XLIVE_TRACE_ENTRY();
	if (object == NULL) {
		return TRUE;
	}
	return CloseHandle(object);
}

int WINAPI XCancelOverlapped(DWORD reserved)
{
	XLIVE_TRACE_ENTRY();
	(void)reserved;
	return 0;
}

int WINAPI XEnumerate(HANDLE enumerator, CHAR *buffer, DWORD buffer_size,
	PDWORD items_returned, PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(enumerator, buffer, buffer_size);
	if (items_returned != NULL) {
		*items_returned = 0u;
	}
	return (int)finish_async(overlapped, (DWORD)ERROR_NO_MORE_FILES);
}

XUSER_SIGNIN_STATE WINAPI XUserGetSigninState(DWORD user_index)
{
	XLIVE_TRACE_ENTRY();
	if (user_index != 0u) {
		xlive_trace("XUserGetSigninState user=%lu -> not signed in",
			(unsigned long)user_index);
	}
	if (user_index != 0u) {
		return eXUserSigninState_NotSignedIn;
	}
	init_relay_state();
	if (ivmp_relay_lost) {
		(void)start_relay_connect(GetTickCount());
		return eXUserSigninState_SignedInLocally;
	}
	return eXUserSigninState_SignedInToLive;
}

DWORD WINAPI XUserGetXUID(DWORD user_index, PXUID xuid)
{
	XLIVE_TRACE_ENTRY();
	if (user_index != 0u) {
		return ERROR_NOT_LOGGED_ON;
	}
	if (xuid == NULL) {
		return ERROR_INVALID_PARAMETER;
	}
	*xuid = online_xuid();
	return ERROR_SUCCESS;
}

DWORD WINAPI XUserGetName(DWORD user_index, LPSTR user_name,
	DWORD user_name_length)
{
	XLIVE_TRACE_ENTRY();
	if (user_index != 0u) {
		xlive_trace("XUserGetName user=%lu -> not logged on",
			(unsigned long)user_index);
		return ERROR_NOT_LOGGED_ON;
	}
	if (user_name == NULL || user_name_length == 0u) {
		return ERROR_INSUFFICIENT_BUFFER;
	}
	local_nickname(user_name, user_name_length < XUSER_NAME_SIZE ? (size_t)user_name_length : (size_t)XUSER_NAME_SIZE);
	return ERROR_SUCCESS;
}

int WINAPI XUserAreUsersFriends(DWORD user_index, DWORD *xuids,
	DWORD xuid_count, DWORD *result,
	PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(user_index, xuids, xuid_count);
	if (result != NULL) {
		*result = 0u;
	}
	return (int)finish_async(overlapped, ERROR_SUCCESS);
}

DWORD WINAPI XUserCheckPrivilege(DWORD user_index, DWORD privilege_type,
	PBOOL result)
{
	XLIVE_TRACE_ENTRY();
	request_relay(5000u);
	xlive_trace("XUserCheckPrivilege user=%lu privilege=%lu", (unsigned long)user_index, (unsigned long)privilege_type);
	if (result != NULL) {
		*result = !ivmp_relay_lost;
	}
	return ERROR_SUCCESS;
}

int WINAPI XUserGetSigninInfo(DWORD user_index, DWORD flags,
	PXUSER_SIGNIN_INFO signin_info)
{
	XLIVE_TRACE_ENTRY();
	if (signin_info == NULL) {
		return ERROR_INVALID_PARAMETER;
	}
	memset(signin_info, 0, sizeof(*signin_info));
	if (user_index != 0u) {
		xlive_trace("XUserGetSigninInfo user=%lu flags=%lu -> not logged on",
			(unsigned long)user_index, (unsigned long)flags);
		signin_info->UserSigninState = eXUserSigninState_NotSignedIn;
		return ERROR_NOT_LOGGED_ON;
	}
	signin_info->xuid = (flags & XUSER_GET_SIGNIN_INFO_OFFLINE_XUID_ONLY) != 0u
		? offline_xuid
		: online_xuid();
	signin_info->UserSigninState = eXUserSigninState_SignedInToLive;
	local_nickname(signin_info->szUserName, sizeof(signin_info->szUserName));
	return ERROR_SUCCESS;
}

HANDLE WINAPI XNotifyCreateListener(ULONGLONG areas)
{
	XLIVE_TRACE_ENTRY();
	(void)areas;
	return CreateEventA(NULL, TRUE, FALSE, NULL);
}

void WINAPI XUserSetProperty(DWORD user_index, DWORD property_id,
	DWORD value_size, CONST VOID *value)
{
	XLIVE_TRACE_ENTRY();
	xlive_trace("XUserSetProperty user=%lu id=0x%08lx size=%lu value=%ld", (unsigned long)user_index, (unsigned long)property_id, (unsigned long)value_size, value != NULL && value_size >= 4u ? *(const LONG *)value : 0l);
	store_user_property(property_id, value_size, value);
	publish_session_state();
}

DWORD WINAPI XUserSetContext(DWORD user_index, DWORD context_id,
	DWORD context_value)
{
	XLIVE_TRACE_ENTRY();
	xlive_trace("XUserSetContext user=%lu id=0x%08lx value=%lu", (unsigned long)user_index, (unsigned long)context_id, (unsigned long)context_value);
	store_user_context(context_id, context_value);
	publish_session_state();
	return ERROR_SUCCESS;
}

DWORD WINAPI XUserReadStats(DWORD title_id, DWORD xuid_count,
	CONST XUID *xuids, DWORD spec_count, DWORD *specs,
	DWORD *results_size, DWORD *results,
	PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(title_id, xuids, specs, results_size, results, overlapped);
	xlive_trace("XUserReadStats players=%lu views=%lu -> unavailable",
		(unsigned long)xuid_count, (unsigned long)spec_count);
	return ERROR_FUNCTION_FAILED;
}

int WINAPI XUserSetContextEx(DWORD user_index, DWORD context_id,
	DWORD context_value, PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	xlive_trace("XUserSetContextEx user=%lu id=0x%08lx value=%lu", (unsigned long)user_index, (unsigned long)context_id, (unsigned long)context_value);
	store_user_context(context_id, context_value);
	publish_session_state();
	return (int)finish_async(overlapped, ERROR_SUCCESS);
}

int WINAPI XLiveInitializeEx(void *init_info, DWORD version)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(init_info);
	xlive_trace("XLiveInitializeEx version=0x%08lx", (unsigned long)version);
	init_relay_state();
	return 0;
}

typedef char ivmp_xuser_data_size_check[sizeof(XUSER_DATA) == 16u ? 1 : -1];
typedef char ivmp_xnaddr_size_check[sizeof(XNADDR) == 36u ? 1 : -1];
typedef char ivmp_xuser_profile_setting_size_check[sizeof(XUSER_PROFILE_SETTING) == 40u ? 1 : -1];
typedef char ivmp_xuser_read_profile_result_size_check[sizeof(XUSER_READ_PROFILE_SETTING_RESULT) == 8u ? 1 : -1];

LONG WINAPI XSessionCreate(DWORD flags, DWORD user_index,
	DWORD max_public_slots, DWORD max_private_slots,
	ULONGLONG *session_nonce,
	PXSESSION_INFO session_info,
	PXOVERLAPPED overlapped, HANDLE *handle)
{
	XLIVE_TRACE_ENTRY();
	request_relay(10000u);
	if (!ivmp_control_ready) {
		xlive_log("XSessionCreate refused: not connected to the IVMP relay");
		return (LONG)finish_async(overlapped, ERROR_FUNCTION_FAILED);
	}
	if (handle != NULL) {
		*handle = CreateMutexA(NULL, FALSE, NULL);
	}
	EnterCriticalSection(&session_state_lock);
	memset(&session_details, 0, sizeof(session_details));
	session_details.dwUserIndexHost = user_index;
	session_details.dwFlags = flags;
	session_details.dwMaxPublicSlots = max_public_slots;
	session_details.dwMaxPrivateSlots = max_private_slots;
	session_details.dwAvailablePublicSlots = max_public_slots;
	session_details.dwAvailablePrivateSlots = max_private_slots;
	session_details.eState = XSESSION_STATE_LOBBY;
	if (session_nonce != NULL) {
		if ((flags & XSESSION_CREATE_HOST) != 0u) {
			fill_random((BYTE *)session_nonce, sizeof(*session_nonce));
		}
		session_details.qwNonce = *session_nonce;
	}
	if (session_info != NULL) {
		if ((flags & XSESSION_CREATE_HOST) != 0u) {
			memset(session_info->sessionID.ab, 0,
				sizeof(session_info->sessionID.ab));
			fill_random(session_info->sessionID.ab + 3,
				sizeof(session_info->sessionID.ab) - 3u);
			(void)XNetGetTitleXnAddr(&session_info->hostAddress);
			fill_random(session_info->keyExchangeKey.ab,
				sizeof(session_info->keyExchangeKey.ab));
		}
		session_details.sessionInfo = *session_info;
	}
	session_hosting = (flags & XSESSION_CREATE_HOST) != 0u;
	session_members = 0u;
	(void)InterlockedExchange(&session_active, 1);
	LeaveCriticalSection(&session_state_lock);
	publish_session_state();
	xlive_trace("XSessionCreate flags=0x%08lx user=%lu public=%lu private=%lu host_ina=0x%08lx",
		(unsigned long)flags, (unsigned long)user_index,
		(unsigned long)max_public_slots,
		(unsigned long)max_private_slots,
		session_info != NULL
			? (unsigned long)session_info->hostAddress.ina.s_addr
			: 0ul);
	return (LONG)finish_async(overlapped, ERROR_SUCCESS);
}

DWORD WINAPI XStringVerify(DWORD flags, const CHAR *locale, DWORD num_strings,
	const STRING_DATA *string_data, DWORD results_size,
	STRING_VERIFY_RESPONSE *results,
	PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(flags, locale, string_data, results_size);
	if (results != NULL) {
		results->wNumStrings = (WORD)num_strings;
	}
	return finish_async(overlapped, ERROR_SUCCESS);
}

int WINAPI XOnlineStartup(void)
{
	XLIVE_TRACE_ENTRY();
	xlive_trace("XOnlineStartup");
	init_relay_state();
	return 0;
}

int WINAPI XOnlineCleanup(void)
{
	XLIVE_TRACE_ENTRY();
	xlive_trace("XOnlineCleanup");
	return 0;
}

int WINAPI XSessionStart(HANDLE session, DWORD flags, PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(session);
	xlive_trace("XSessionStart flags=0x%08lx", (unsigned long)flags);
	session_details.eState = XSESSION_STATE_INGAME;
	return (int)finish_async(overlapped, ERROR_SUCCESS);
}

DWORD WINAPI XSessionSearchEx(DWORD procedure, DWORD user_index,
	DWORD num_results, DWORD num_users,
	DWORD num_properties, DWORD num_contexts,
	PXUSER_PROPERTY search_properties,
	PXUSER_CONTEXT search_contexts,
	DWORD *buffer_size,
	PXSESSION_SEARCHRESULT_HEADER results,
	PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(user_index, num_users, num_properties, search_properties);
	struct search_request *request;
	DWORD required;
	HANDLE thread;

	if (buffer_size == NULL) {
		return ERROR_INVALID_PARAMETER;
	}
	required = search_result_capacity(num_results);
	if (results == NULL || *buffer_size < required) {
		xlive_trace("XSessionSearchEx procedure=%lu max=%lu needs %lu bytes",
			(unsigned long)procedure, (unsigned long)num_results,
			(unsigned long)required);
		*buffer_size = required;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	request = (struct search_request *)calloc(1u, sizeof(*request));
	if (request == NULL) {
		return ERROR_OUTOFMEMORY;
	}
	request->procedure = procedure;
	request->num_results = num_results;
	if (search_contexts != NULL) {
		DWORD index;

		for (index = 0u; index < (num_contexts & 0xffffu) && request->context_count < IVMP_MAX_CONTEXTS; ++index) {
			request->contexts[request->context_count++] = search_contexts[index];
		}
	}
	request->buffer_size = *buffer_size;
	request->results = results;
	request->overlapped = overlapped;
	memset(results, 0, sizeof(*results));
	if (overlapped == NULL) {
		run_search(request);
		free(request);
		return ERROR_SUCCESS;
	}
	overlapped->InternalHigh = 0u;
	overlapped->dwExtendedError = ERROR_IO_INCOMPLETE;
	(void)InterlockedExchange((LONG *)&overlapped->InternalLow,
		(LONG)ERROR_IO_PENDING);
	thread = CreateThread(NULL, 0u, search_thread, request, 0u, NULL);
	if (thread == NULL) {
		run_search(request);
		free(request);
		complete_overlapped(overlapped, ERROR_SUCCESS);
		return ERROR_IO_PENDING;
	}
	(void)CloseHandle(thread);
	return ERROR_IO_PENDING;
}

DWORD WINAPI XSessionModify(HANDLE session, DWORD flags, DWORD max_public_slots, DWORD max_private_slots, PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(session, overlapped);
	xlive_trace("XSessionModify flags=0x%08lx", (unsigned long)flags);
	EnterCriticalSection(&session_state_lock);
	session_details.dwMaxPublicSlots = max_public_slots;
	session_details.dwMaxPrivateSlots = max_private_slots;
	LeaveCriticalSection(&session_state_lock);
	publish_session_state();
	return ERROR_SUCCESS;
}

DWORD WINAPI XSessionMigrateHost(HANDLE session, DWORD user_index,
	PXSESSION_INFO session_info,
	PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(session);
	request_relay(0u);
	int becomes_host = user_index != XUSER_INDEX_NONE;

	EnterCriticalSection(&session_state_lock);
	if (becomes_host) {
		(void)XNetGetTitleXnAddr(&session_details.sessionInfo.hostAddress);
		session_details.dwUserIndexHost = user_index;
		session_details.dwFlags |= XSESSION_CREATE_HOST;
		session_hosting = 1;
		if (session_info != NULL) {
			*session_info = session_details.sessionInfo;
		}
	} else {
		if (session_info != NULL) {
			session_details.sessionInfo = *session_info;
		}
		session_details.dwFlags &= ~XSESSION_CREATE_HOST;
		session_hosting = 0;
	}
	LeaveCriticalSection(&session_state_lock);
	xlive_trace("XSessionMigrateHost user=%lu %s host_ina=0x%08lx",
		(unsigned long)user_index,
		becomes_host ? "local player is the new host"
			     : "remote player is the new host",
		(unsigned long)session_details.sessionInfo.hostAddress.ina.s_addr);
	publish_session_state();
	return finish_async(overlapped, ERROR_SUCCESS);
}

DWORD WINAPI XSessionLeaveLocal(HANDLE session, DWORD user_count, const DWORD *user_indexes, PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(session, user_indexes, overlapped);
	xlive_trace("XSessionLeaveLocal count=%lu", (unsigned long)user_count);
	change_session_members(-(LONG)user_count);
	return ERROR_SUCCESS;
}

DWORD WINAPI XSessionJoinRemote(HANDLE session, DWORD xuid_count, const XUID *xuids, const BOOL *private_slots, PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(session, xuids, private_slots, overlapped);
	request_relay(0u);
	xlive_trace("XSessionJoinRemote count=%lu", (unsigned long)xuid_count);
	change_session_members((LONG)xuid_count);
	return ERROR_SUCCESS;
}

DWORD WINAPI XSessionJoinLocal(HANDLE session, DWORD user_count,
	const DWORD *user_indexes,
	const BOOL *private_slots,
	PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(session, user_indexes, private_slots);
	request_relay(0u);
	xlive_trace("XSessionJoinLocal count=%lu", (unsigned long)user_count);
	change_session_members((LONG)user_count);
	return finish_async(overlapped, ERROR_SUCCESS);
}

DWORD WINAPI XSessionGetDetails(HANDLE session, PDWORD results_size,
	PXSESSION_LOCAL_DETAILS details,
	PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(session);
	xlive_trace("XSessionGetDetails");
	if (results_size != NULL) {
		*results_size = (DWORD)sizeof(*details);
	}
	if (details != NULL) {
		*details = session_details;
	}
	return finish_async(overlapped, ERROR_SUCCESS);
}

DWORD WINAPI XSessionDelete(HANDLE session, PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(session, overlapped);
	xlive_trace("XSessionDelete");
	EnterCriticalSection(&session_state_lock);
	session_details.eState = XSESSION_STATE_DELETED;
	session_hosting = 0;
	(void)InterlockedExchange(&session_active, 0);
	session_members = 0u;
	LeaveCriticalSection(&session_state_lock);
	publish_session_state();
	return ERROR_SUCCESS;
}

DWORD WINAPI XUserReadProfileSettings(
	DWORD title_id, DWORD user_index, DWORD num_setting_ids,
	DWORD *setting_ids, DWORD *results_size,
	XUSER_READ_PROFILE_SETTING_RESULT *results, PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(title_id);
	XUSER_PROFILE_SETTING *settings;
	DWORD required;
	DWORD index;

	if (results_size == NULL ||
		(num_setting_ids != 0u && setting_ids == NULL) ||
		num_setting_ids > (0xffffffffu - (DWORD)sizeof(*results)) /
				(DWORD)sizeof(XUSER_PROFILE_SETTING)) {
		return ERROR_INVALID_PARAMETER;
	}
	required = (DWORD)sizeof(*results) +
		num_setting_ids * (DWORD)sizeof(XUSER_PROFILE_SETTING);
	if (results == NULL || *results_size < required) {
		*results_size = required;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	memset(results, 0, required);
	settings = (XUSER_PROFILE_SETTING *)(void *)(results + 1);
	results->dwSettingsLen = num_setting_ids;
	results->pSettings = num_setting_ids != 0u ? settings : NULL;
	for (index = 0u; index < num_setting_ids; ++index) {
		settings[index].source = XSOURCE_DEFAULT;
		settings[index].user.dwUserIndex = user_index;
		settings[index].dwSettingId = setting_ids[index];
		settings[index].data.type =
			(BYTE)((setting_ids[index] >> 28) & 0x0fu);
	}
	return finish_async(overlapped, ERROR_SUCCESS);
}

int WINAPI XSessionEnd(HANDLE session, PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(session, overlapped);
	xlive_trace("XSessionEnd");
	session_details.eState = XSESSION_STATE_REPORTING;
	return 0;
}

DWORD WINAPI XSessionLeaveRemote(HANDLE session, DWORD xuid_count, const XUID *xuids, PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(session, xuids, overlapped);
	xlive_trace("XSessionLeaveRemote count=%lu", (unsigned long)xuid_count);
	change_session_members(-(LONG)xuid_count);
	return ERROR_SUCCESS;
}

DWORD WINAPI XStorageBuildServerPath(DWORD user_index,
	XSTORAGE_FACILITY storage_facility,
	const void *storage_facility_info,
	DWORD storage_facility_info_size,
	LPCWSTR *item_name, WCHAR *server_path,
	DWORD *server_path_length)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(user_index, storage_facility, storage_facility_info, storage_facility_info_size, item_name);
	if (server_path != NULL && server_path_length != NULL &&
		*server_path_length > 0u) {
		server_path[0] = L'\0';
	}
	if (server_path_length != NULL) {
		*server_path_length = 1u;
	}
	return ERROR_SUCCESS;
}

DWORD WINAPI XStorageDownloadToMemory(
	DWORD user_index, const WCHAR *server_path, DWORD buffer_size,
	const BYTE *buffer, DWORD results_size,
	XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS *results, PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(user_index, server_path, buffer_size, buffer, results_size);
	if (results != NULL) {
		memset(results, 0, sizeof(*results));
	}
	return finish_async(overlapped, ERROR_FILE_NOT_FOUND);
}

DWORD WINAPI XLiveContentCreateAccessHandle(DWORD title_id,
	PXCONTENT_DATA content_info,
	DWORD license_info_version,
	ivmp_xlive_pbuffer *buffer,
	DWORD offset, HANDLE *access,
	PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(title_id, content_info, license_info_version, buffer, offset);
	if (access != NULL) {
		*access = NULL;
	}
	return finish_async(overlapped, ERROR_FILE_NOT_FOUND);
}

LONG WINAPI XLiveContentGetPath(DWORD user_index, PXCONTENT_DATA content_data,
	wchar_t *path, DWORD *path_length)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(user_index, content_data);
	if (path != NULL && path_length != NULL && *path_length > 0u) {
		path[0] = L'\0';
	}
	if (path_length != NULL) {
		*path_length = 1u;
	}
	return (LONG)ERROR_FILE_NOT_FOUND;
}

DWORD WINAPI XContentGetMarketplaceCounts(
	DWORD user_index, DWORD content_categories, DWORD results_size,
	XOFFERING_CONTENTAVAILABLE_RESULT *results, PXOVERLAPPED overlapped)
{
	XLIVE_TRACE_ENTRY();
	XLIVE_UNUSED(user_index, content_categories, results_size);
	if (results != NULL) {
		memset(results, 0, sizeof(*results));
	}
	return finish_async(overlapped, ERROR_SUCCESS);
}

static void apply_game_patches(void)
{
	enum ivmp_game_patch_result result;
	char setting[8];

	if (GetEnvironmentVariableA("IVMP_GAME_PATCHES", setting,
		    (DWORD)sizeof(setting)) != 0u &&
		lstrcmpA(setting, "0") == 0) {
		xlive_log("game patches disabled by IVMP_GAME_PATCHES=0");
		return;
	}
	result = ivmp_game_patch_apply();
	if (result == IVMP_GAME_PATCH_APPLIED) {
		xlive_log("multiplayer game patches applied");
	} else if (result == IVMP_GAME_PATCH_PARTIAL) {
		xlive_log("multiplayer game patches only partly applied");
	} else if (result == IVMP_GAME_PATCH_UNKNOWN_EXECUTABLE) {
		xlive_log("multiplayer game patches skipped: unknown GTAIV.exe build");
	} else {
		xlive_log("multiplayer game patches failed");
	}
}

static void deferred_init(void)
{
	xlive_trace("trace enabled");
	apply_game_patches();
	asi_loader_logger = xlive_log;
	load_all_asis();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		module_instance = instance;
		InitializeCriticalSection(&session_state_lock);
		DisableThreadLibraryCalls(instance);
		clear_xlive_log();
		ivmp_crash_install(instance);
		ivmp_entry_hook_install(deferred_init);
	} else if (reason == DLL_PROCESS_DETACH) {
		ivmp_crash_uninstall();
	}
	return TRUE;
}

XLIVE_CONSTANT(int, XLiveRender, 0)

XLIVE_CONSTANT(int, XLiveUninitialize, 0)

XLIVE_STUB(int, XLiveOnCreateDevice, (DWORD device, DWORD swap_chain), (device, swap_chain), 0)

XLIVE_STUB(int, XLiveOnResetDevice, (DWORD present_parameters), (present_parameters), 0)

XLIVE_STUB(int, XLiveGetUpdateInformation, (DWORD reserved), (reserved), 0)

XLIVE_STUB(int, XLiveUpdateSystem, (DWORD reserved), (reserved), 0)

XLIVE_STUB(int, XLivePreTranslateMessage, (DWORD reserved), (reserved), 0)

XLIVE_STUB(HRESULT, XLiveProtectData, (BYTE * in_buffer, DWORD in_size, BYTE *out_buffer, DWORD *out_size, HANDLE handle), (handle), copy_protected_data(in_buffer, in_size, out_buffer, out_size))

XLIVE_STUB(DWORD, XLiveCloseProtectedDataContext, (HANDLE handle), (handle), ERROR_SUCCESS)

XLIVE_STUB(int, XShowPlayerReviewUI, (DWORD user_index, XUID target), (user_index, target), (int)E_FAIL)

XLIVE_STUB(int, XShowGuideUI, (DWORD user_index), (user_index), (int)E_FAIL)

XLIVE_STUB(int, XShowGamerCardUI, (DWORD user_index, XUID player), (user_index, player), (int)E_FAIL)

XLIVE_STUB(int, XShowSigninUI, (DWORD panes, DWORD flags), (panes, flags), 0)

XLIVE_STUB(DWORD, XUserWriteAchievements, (DWORD count, PXUSER_ACHIEVEMENT achievements, LPVOID overlapped), (count, achievements, overlapped), (DWORD)finish_async((PXOVERLAPPED)overlapped, ERROR_SUCCESS))

XLIVE_STUB(DWORD, XUserCreateAchievementEnumerator, (DWORD title_id, DWORD user_index, XUID xuid, DWORD detail_flags, DWORD starting_index, DWORD max_count, PDWORD buffer_size, PHANDLE handle), (title_id, user_index, xuid, detail_flags, starting_index, max_count), no_enumerator(buffer_size, handle))

XLIVE_STUB(DWORD, XUserCreateStatsEnumeratorByRank, (DWORD title_id, DWORD rank_start, DWORD rows, DWORD spec_count, void *specs, DWORD *buffer_size, PHANDLE handle), (title_id, rank_start, rows, spec_count, specs), no_enumerator(buffer_size, handle))

XLIVE_STUB(DWORD, XUserCreateStatsEnumeratorByXuid, (DWORD title_id, XUID pivot, DWORD rows, DWORD spec_count, const void *specs, DWORD *buffer_size, PHANDLE handle), (title_id, pivot, rows, spec_count, specs), no_enumerator(buffer_size, handle))

XLIVE_STUB(DWORD, XStorageUploadFromMemory, (DWORD user_index, const WCHAR *server_path, DWORD buffer_size, const BYTE *buffer, PXOVERLAPPED overlapped), (user_index, server_path, buffer_size, buffer, overlapped), finish_async(overlapped, ERROR_SUCCESS))

XLIVE_STUB(DWORD, XFriendsCreateEnumerator, (DWORD user_index, DWORD starting_index, DWORD friends_to_return, DWORD *buffer_size, HANDLE *handle), (user_index, starting_index, friends_to_return), no_enumerator(buffer_size, handle))

XLIVE_STUB(int, XUserMuteListQuery, (DWORD user_index, XUID remote_talker, BOOL *on_mute_list), (user_index, remote_talker, on_mute_list), 0)

XLIVE_STUB(int, XInviteGetAcceptedInfo, (DWORD user_index, void *info), (user_index, info), (int)E_FAIL)

XLIVE_STUB(int, XInviteSend, (DWORD user_index, DWORD invitee_count, const XUID *invitees, const WCHAR *text, PXOVERLAPPED overlapped), (user_index, invitee_count, invitees, text, overlapped), (int)E_FAIL)

XLIVE_STUB(DWORD, XSessionWriteStats, (HANDLE session, XUID xuid, DWORD view_count, const void *views, PXOVERLAPPED overlapped), (session, xuid, view_count, views), finish_async(overlapped, ERROR_SUCCESS))

XLIVE_CONSTANT(XONLINE_NAT_TYPE, XOnlineGetNatType, XONLINE_NAT_OPEN)

XLIVE_STUB(int, XSessionFlushStats, (HANDLE session, PXOVERLAPPED overlapped), (session, overlapped), 0)

XLIVE_STUB(DWORD, XSessionArbitrationRegister, (HANDLE session, DWORD flags, ULONGLONG nonce, DWORD *results_size, void *results, PXOVERLAPPED overlapped), (session, flags, nonce, results_size, results, overlapped), ERROR_SUCCESS)

XLIVE_STUB(DWORD, XTitleServerCreateEnumerator, (LPCSTR server_info, DWORD count, DWORD *buffer_size, PHANDLE handle), (server_info, count), no_enumerator(buffer_size, handle))

XLIVE_STUB(DWORD, XUserWriteProfileSettings, (DWORD user_index, DWORD num_settings, const PXUSER_PROFILE_SETTING settings, PXOVERLAPPED overlapped), (user_index, num_settings, settings, overlapped), finish_async(overlapped, ERROR_SUCCESS))

XLIVE_STUB(DWORD, XContentCreateEnumerator, (DWORD max_count, PDWORD reserved, PDWORD buffer_size, PHANDLE handle), (max_count, reserved), no_enumerator(buffer_size, handle))

XLIVE_STUB(DWORD, XShowMarketplaceUI, (DWORD user_index, DWORD entry_point, ULONGLONG offer_id, DWORD content_categories), (user_index, entry_point, offer_id, content_categories), (DWORD)E_FAIL)

XLIVE_STUB(DWORD, XMarketplaceCreateOfferEnumerator, (DWORD user_index, DWORD offer_type, DWORD content_categories, DWORD count, PDWORD buffer_size, PHANDLE handle), (user_index, offer_type, content_categories, count), no_enumerator(buffer_size, handle))

#include "relay_client.h"

#include <ws2tcpip.h>

#include <stdlib.h>
#include <string.h>

#include "protocol.h"

#define MAX_UDP_SOCKETS 32u
#define UDP_BUFFER_SIZE (IVMP_TUNNEL_HEADER_SIZE + IVMP_MAX_TUNNEL_PAYLOAD)

struct udp_socket {
	SOCKET socket;
	u_short virtual_port;
	int registered;
};

struct sockaddr_in ivmp_server_address;
uint32_t ivmp_connection_id;
uint64_t ivmp_session_token;
int ivmp_redirect_all = 1;
int ivmp_control_ready;
volatile LONG ivmp_last_game_traffic;
volatile LONG ivmp_relay_lost;

static SOCKET control_socket = INVALID_SOCKET;
static int winsock_started;
static CRITICAL_SECTION socket_lock;
static struct udp_socket udp_sockets[MAX_UDP_SOCKETS];

static void (*log_function)(const char *message);
static void (*demand_function)(DWORD wait_ms);
static int (*idle_function)(void);

static CRITICAL_SECTION session_lock;
static unsigned char session_record[IVMP_MAX_SESSION_RECORD];
static size_t session_record_length;
static int session_record_dirty;
static int session_query_pending;
static HANDLE session_list_event;
static unsigned char *session_list;
static size_t session_list_length;
static unsigned long session_list_serial;

static void relay_log(const char *message)
{
	if (log_function != NULL) {
		log_function(message);
	}
}

void ivmp_relay_client_init(void (*logger)(const char *message), void (*demand)(DWORD wait_ms), int (*idle)(void))
{
	char redirect_text[8];
	unsigned int index;

	log_function = logger;
	demand_function = demand;
	idle_function = idle;
	InitializeCriticalSection(&socket_lock);
	InitializeCriticalSection(&session_lock);
	session_list_event = CreateEventA(NULL, TRUE, FALSE, NULL);
	session_list = (unsigned char *)malloc(IVMP_MAX_CONTROL_PAYLOAD);
	for (index = 0u; index < MAX_UDP_SOCKETS; ++index) {
		udp_sockets[index].socket = INVALID_SOCKET;
	}
	if (GetEnvironmentVariableA("IVMP_REDIRECT_ALL", redirect_text, (DWORD)sizeof(redirect_text)) != 0u) {
		ivmp_redirect_all = lstrcmpA(redirect_text, "0") != 0;
	}
}

static int relay_available(DWORD wait_ms)
{
	if (!ivmp_control_ready && demand_function != NULL) {
		demand_function(wait_ms);
	}
	if (ivmp_control_ready) {
		(void)InterlockedExchange(&ivmp_last_game_traffic, (LONG)GetTickCount());
	}
	return ivmp_control_ready;
}

static int resolve_server_address(const char *input, struct sockaddr_in *output)
{
	char host[256];
	char message[384];
	const char *port_text = strrchr(input, ':');
	struct addrinfo hints;
	struct addrinfo *results = NULL;
	int error;

	if (port_text == NULL || port_text == input || (size_t)(port_text - input) >= sizeof(host) || port_text[1] == '\0' || strspn(port_text + 1, "0123456789") != strlen(port_text + 1) || atol(port_text + 1) < 1 || atol(port_text + 1) > 65535) {
		relay_log("server address must be host:port");
		return 0;
	}
	memcpy(host, input, (size_t)(port_text - input));
	host[port_text - input] = '\0';
	++port_text;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	error = getaddrinfo(host, port_text, &hints, &results);
	if (error != 0 || results == NULL) {
		(void)wsprintfA(message, "could not resolve %s (error %d)", host, error);
		relay_log(message);
		return 0;
	}
	if (memcmp(output, results->ai_addr, sizeof(*output)) != 0) {
		memcpy(output, results->ai_addr, sizeof(*output));
		(void)wsprintfA(message, "resolved %s to %s:%s", host, inet_ntoa(output->sin_addr), port_text);
		relay_log(message);
	}
	freeaddrinfo(results);
	return 1;
}

static int socket_send_all(const unsigned char *data, size_t length)
{
	size_t sent = 0u;

	while (sent < length) {
		int result = send(control_socket, (const char *)data + sent, (int)(length - sent), 0);

		if (result <= 0) {
			return 0;
		}
		sent += (size_t)result;
	}
	return 1;
}

static int socket_receive_all(unsigned char *data, size_t length)
{
	size_t received = 0u;

	while (received < length) {
		int result = recv(control_socket, (char *)data + received, (int)(length - received), 0);

		if (result <= 0) {
			return 0;
		}
		received += (size_t)result;
	}
	return 1;
}

static int receive_control_message(unsigned char *packet, size_t capacity, struct ivmp_control_header *header)
{
	return socket_receive_all(packet, IVMP_CONTROL_HEADER_SIZE) && ivmp_decode_control_header(packet, IVMP_CONTROL_HEADER_SIZE, header) && header->payload_length <= capacity - IVMP_CONTROL_HEADER_SIZE && socket_receive_all(packet + IVMP_CONTROL_HEADER_SIZE, header->payload_length);
}

static int send_control_message(uint16_t type, const unsigned char *payload, size_t length)
{
	unsigned char header[IVMP_CONTROL_HEADER_SIZE];

	return ivmp_encode_control_header(header, sizeof(header) + length, type, (uint32_t)length) != 0 && socket_send_all(header, sizeof(header)) && (length == 0u || socket_send_all(payload, length));
}

static void log_server_error(const unsigned char *packet, size_t length)
{
	struct ivmp_error error;
	char text[128];
	char message[192];
	size_t copy;

	if (!ivmp_decode_error(packet, length, &error)) {
		relay_log("invalid welcome from the IVMP relay");
		return;
	}
	copy = error.message_length < sizeof(text) - 1u ? error.message_length : sizeof(text) - 1u;
	memcpy(text, error.message, copy);
	text[copy] = '\0';
	(void)wsprintfA(message, "IVMP relay error %u: %s", (unsigned int)error.code, text);
	relay_log(message);
}

static void close_control_socket(void)
{
	(void)closesocket(control_socket);
	control_socket = INVALID_SOCKET;
}

static void refresh_udp_registrations(void);

int ivmp_relay_client_connect(const char *server_text, const char *nickname)
{
	WSADATA wsa_data;
	struct ivmp_hello hello;
	struct ivmp_control_header header;
	unsigned char packet[IVMP_CONTROL_HEADER_SIZE + 256u];
	char message[96];
	size_t nickname_length;
	int length;

	if (ivmp_control_ready) {
		return 1;
	}
	if (!winsock_started) {
		if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
			relay_log("WSAStartup failed");
			return 0;
		}
		winsock_started = 1;
	}
	if (server_text == NULL || !resolve_server_address(server_text, &ivmp_server_address)) {
		return 0;
	}
	control_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (control_socket == INVALID_SOCKET) {
		relay_log("control socket creation failed");
		return 0;
	}
	if (connect(control_socket, (const struct sockaddr *)&ivmp_server_address, (int)sizeof(ivmp_server_address)) == SOCKET_ERROR) {
		relay_log("control connect failed");
		close_control_socket();
		return 0;
	}

	if (nickname == NULL || nickname[0] == '\0') {
		nickname = "Player";
	}
	nickname_length = strlen(nickname);
	if (nickname_length > IVMP_MAX_NICKNAME) {
		nickname_length = IVMP_MAX_NICKNAME;
	}
	memset(&hello, 0, sizeof(hello));
	hello.minimum_version = IVMP_PROTOCOL_VERSION;
	hello.maximum_version = IVMP_PROTOCOL_VERSION;
	hello.requested_connection_id = ivmp_connection_id;
	hello.nickname_length = (uint16_t)nickname_length;
	hello.nickname = (const unsigned char *)nickname;
	length = ivmp_encode_hello(packet, sizeof(packet), &hello);
	if (length == 0 || !socket_send_all(packet, (size_t)length)) {
		relay_log("sending the control hello failed");
		close_control_socket();
		return 0;
	}
	if (!receive_control_message(packet, sizeof(packet), &header)) {
		relay_log("the IVMP relay closed the connection during the handshake");
		close_control_socket();
		return 0;
	}
	if (!ivmp_decode_welcome(packet, IVMP_CONTROL_HEADER_SIZE + header.payload_length, &ivmp_connection_id, &ivmp_session_token)) {
		log_server_error(packet, IVMP_CONTROL_HEADER_SIZE + header.payload_length);
		close_control_socket();
		return 0;
	}
	(void)wsprintfA(message, "registered with the IVMP relay as connection %lu (10.%lu.%lu.%lu)", (unsigned long)ivmp_connection_id, (unsigned long)((ivmp_connection_id >> 16) & 0xffu), (unsigned long)((ivmp_connection_id >> 8) & 0xffu), (unsigned long)(ivmp_connection_id & 0xffu));
	relay_log(message);
	ivmp_control_ready = 1;
	(void)InterlockedExchange(&ivmp_relay_lost, 0);
	refresh_udp_registrations();
	return 1;
}

void ivmp_relay_publish_session(const unsigned char *record, size_t length)
{
	if (length > sizeof(session_record)) {
		return;
	}
	EnterCriticalSection(&session_lock);
	if (length != 0u) {
		memcpy(session_record, record, length);
	}
	session_record_length = length;
	session_record_dirty = 1;
	LeaveCriticalSection(&session_lock);
}

int ivmp_relay_query_sessions(unsigned char *output, size_t capacity, size_t *length, DWORD timeout_ms)
{
	unsigned long serial;
	DWORD started = GetTickCount();
	int received = 0;

	*length = 0u;
	if (!ivmp_control_ready || session_list_event == NULL) {
		return 0;
	}
	EnterCriticalSection(&session_lock);
	serial = session_list_serial;
	session_query_pending = 1;
	(void)ResetEvent(session_list_event);
	LeaveCriticalSection(&session_lock);
	for (;;) {
		DWORD elapsed = GetTickCount() - started;

		if (elapsed >= timeout_ms) {
			break;
		}
		(void)WaitForSingleObject(session_list_event, timeout_ms - elapsed);
		EnterCriticalSection(&session_lock);
		if (session_list_serial != serial) {
			if (session_list_length <= capacity) {
				memcpy(output, session_list, session_list_length);
				*length = session_list_length;
				received = 1;
			}
			LeaveCriticalSection(&session_lock);
			break;
		}
		LeaveCriticalSection(&session_lock);
	}
	return received;
}

static int handle_control_message(unsigned char *packet, size_t capacity)
{
	struct ivmp_control_header header;

	if (!receive_control_message(packet, capacity, &header)) {
		return 0;
	}
	if (header.type == IVMP_CONTROL_SERVER_SESSION_LIST) {
		EnterCriticalSection(&session_lock);
		memcpy(session_list, packet + IVMP_CONTROL_HEADER_SIZE, header.payload_length);
		session_list_length = header.payload_length;
		++session_list_serial;
		(void)SetEvent(session_list_event);
		LeaveCriticalSection(&session_lock);
	} else if (header.type == IVMP_CONTROL_SERVER_ERROR) {
		log_server_error(packet, IVMP_CONTROL_HEADER_SIZE + header.payload_length);
	}
	return 1;
}

static struct udp_socket *find_udp_socket(SOCKET socket)
{
	unsigned int index;

	for (index = 0u; index < MAX_UDP_SOCKETS; ++index) {
		if (udp_sockets[index].socket == socket) {
			return &udp_sockets[index];
		}
	}
	return NULL;
}

static struct udp_socket *track_udp_socket(SOCKET socket)
{
	struct udp_socket *entry = find_udp_socket(socket);

	if (entry == NULL) {
		entry = find_udp_socket(INVALID_SOCKET);
		if (entry != NULL) {
			memset(entry, 0, sizeof(*entry));
			entry->socket = socket;
		}
	}
	return entry;
}

static int is_tracked(SOCKET socket)
{
	int tracked;

	EnterCriticalSection(&socket_lock);
	tracked = find_udp_socket(socket) != NULL;
	LeaveCriticalSection(&socket_lock);
	return tracked;
}

static void send_udp_registration(SOCKET socket)
{
	unsigned char packet[IVMP_TUNNEL_HEADER_SIZE];
	int length = ivmp_encode_tunnel(packet, sizeof(packet), ivmp_connection_id, ivmp_session_token, 0u, 0u, NULL, 0u);

	if (length > 0) {
		(void)sendto(socket, (const char *)packet, length, 0, (const struct sockaddr *)&ivmp_server_address, (int)sizeof(ivmp_server_address));
	}
}

void ivmp_relay_register_udp_socket(SOCKET socket)
{
	int type = 0;
	int type_length = (int)sizeof(type);
	struct udp_socket *entry;

	if (!ivmp_redirect_all || getsockopt(socket, SOL_SOCKET, SO_TYPE, (char *)&type, &type_length) == SOCKET_ERROR || type != SOCK_DGRAM) {
		return;
	}
	EnterCriticalSection(&socket_lock);
	entry = track_udp_socket(socket);
	if (entry != NULL) {
		entry->registered = 1;
	}
	LeaveCriticalSection(&socket_lock);
	if (entry != NULL && ivmp_control_ready) {
		send_udp_registration(socket);
	}
}

static void refresh_udp_registrations(void)
{
	SOCKET sockets[MAX_UDP_SOCKETS];
	unsigned int count = 0u;
	unsigned int index;

	EnterCriticalSection(&socket_lock);
	for (index = 0u; index < MAX_UDP_SOCKETS; ++index) {
		if (udp_sockets[index].socket != INVALID_SOCKET && udp_sockets[index].registered) {
			sockets[count++] = udp_sockets[index].socket;
		}
	}
	LeaveCriticalSection(&socket_lock);
	for (index = 0u; index < count; ++index) {
		send_udp_registration(sockets[index]);
	}
}

DWORD WINAPI ivmp_relay_heartbeat_thread(LPVOID parameter)
{
	unsigned char *packet = (unsigned char *)malloc(IVMP_CONTROL_HEADER_SIZE + IVMP_MAX_CONTROL_PAYLOAD);
	unsigned char *record = (unsigned char *)malloc(IVMP_MAX_SESSION_RECORD);
	DWORD next_ping = GetTickCount() + IVMP_HEARTBEAT_MS;
	LONG lost = 1;

	(void)parameter;
	if (packet == NULL || record == NULL) {
		relay_log("relay control buffers could not be allocated");
		goto done;
	}
	for (;;) {
		fd_set read_fds;
		struct timeval timeout;
		DWORD now;
		size_t record_length = 0u;
		int publish = 0;
		int query;

		FD_ZERO(&read_fds);
		FD_SET(control_socket, &read_fds);
		timeout.tv_sec = 0;
		timeout.tv_usec = 50000;
		if (select(0, &read_fds, NULL, NULL, &timeout) > 0 && !handle_control_message(packet, IVMP_CONTROL_HEADER_SIZE + IVMP_MAX_CONTROL_PAYLOAD)) {
			relay_log("relay control channel closed");
			break;
		}
		EnterCriticalSection(&session_lock);
		if (session_record_dirty) {
			publish = 1;
			record_length = session_record_length;
			memcpy(record, session_record, record_length);
			session_record_dirty = 0;
		}
		query = session_query_pending;
		session_query_pending = 0;
		LeaveCriticalSection(&session_lock);
		if (publish && !send_control_message(IVMP_CONTROL_CLIENT_SESSION_PUBLISH, record, record_length)) {
			relay_log("publishing the session failed");
			break;
		}
		if (query && !send_control_message(IVMP_CONTROL_CLIENT_SESSION_QUERY, NULL, 0u)) {
			relay_log("querying sessions failed");
			break;
		}
		now = GetTickCount();
		if ((LONG)(now - next_ping) >= 0) {
			if (!send_control_message(IVMP_CONTROL_CLIENT_PING, NULL, 0u)) {
				relay_log("relay heartbeat failed");
				break;
			}
			refresh_udp_registrations();
			next_ping = now + IVMP_HEARTBEAT_MS;
		}
		if (idle_function != NULL && idle_function()) {
			(void)send_control_message(IVMP_CONTROL_CLIENT_GOODBYE, NULL, 0u);
			relay_log("multiplayer left; disconnected from the IVMP relay");
			lost = 0;
			break;
		}
	}
done:
	close_control_socket();
	ivmp_control_ready = 0;
	(void)InterlockedExchange(&ivmp_relay_lost, lost);
	free(packet);
	free(record);
	return 0u;
}

uint32_t ivmp_relay_peer_address(uint32_t connection_id)
{
	return htonl(0x0a000000u | (connection_id & 0x00ffffffu));
}

void ivmp_relay_set_virtual_port(SOCKET socket, u_short port)
{
	struct udp_socket *entry;

	EnterCriticalSection(&socket_lock);
	entry = track_udp_socket(socket);
	if (entry != NULL) {
		entry->virtual_port = port;
	}
	LeaveCriticalSection(&socket_lock);
}

int ivmp_relay_virtual_port(SOCKET socket, u_short *port)
{
	struct udp_socket *entry;
	int found = 0;

	EnterCriticalSection(&socket_lock);
	entry = find_udp_socket(socket);
	if (entry != NULL && entry->virtual_port != 0u) {
		*port = entry->virtual_port;
		found = 1;
	}
	LeaveCriticalSection(&socket_lock);
	return found;
}

static uint32_t target_from_address(const struct sockaddr *address, int address_length)
{
	uint32_t host_order;

	if (address == NULL || address_length < (int)sizeof(struct sockaddr_in) || address->sa_family != AF_INET) {
		return 0u;
	}
	host_order = ntohl(((const struct sockaddr_in *)(const void *)address)->sin_addr.s_addr);
	if ((host_order & 0xff000000u) != 0x0a000000u || (host_order & 0x00ffffffu) == 0x00ffffffu) {
		return 0u;
	}
	return host_order & 0x00ffffffu;
}

static void rewrite_udp_source(SOCKET socket, struct sockaddr *address, int *address_length, uint32_t sender_id)
{
	struct sockaddr_in local;
	struct sockaddr_in source;
	int local_length = (int)sizeof(local);

	if (address == NULL || address_length == NULL || *address_length < (int)sizeof(source)) {
		return;
	}
	memset(&local, 0, sizeof(local));
	if (!ivmp_relay_virtual_port(socket, &local.sin_port) && getsockname(socket, (struct sockaddr *)&local, &local_length) == SOCKET_ERROR) {
		local.sin_port = 0u;
	}
	memset(&source, 0, sizeof(source));
	source.sin_family = AF_INET;
	source.sin_port = local.sin_port;
	source.sin_addr.s_addr = ivmp_relay_peer_address(sender_id);
	memcpy(address, &source, sizeof(source));
	*address_length = (int)sizeof(source);
}

int WSAAPI ivmp_hook_sendto(SOCKET socket, const char *buffer, int length, int flags, const struct sockaddr *address, int address_length)
{
	unsigned char *packet;
	int packet_length;
	int result;
	int error;

	if (!is_tracked(socket)) {
		if (!ivmp_redirect_all || address == NULL || address->sa_family != AF_INET) {
			return sendto(socket, buffer, length, flags, address, address_length);
		}
		EnterCriticalSection(&socket_lock);
		(void)track_udp_socket(socket);
		LeaveCriticalSection(&socket_lock);
	}
	if (!relay_available(0u)) {
		return length;
	}
	if (length < 0 || (size_t)length > IVMP_MAX_TUNNEL_PAYLOAD) {
		WSASetLastError(WSAEMSGSIZE);
		return SOCKET_ERROR;
	}
	packet = (unsigned char *)malloc((size_t)length + IVMP_TUNNEL_HEADER_SIZE);
	if (packet == NULL) {
		WSASetLastError(WSAENOBUFS);
		return SOCKET_ERROR;
	}
	packet_length = ivmp_encode_tunnel(packet, (size_t)length + IVMP_TUNNEL_HEADER_SIZE, ivmp_connection_id, ivmp_session_token, target_from_address(address, address_length), 0u, (const unsigned char *)buffer, (size_t)length);
	result = sendto(socket, (const char *)packet, packet_length, flags, (const struct sockaddr *)&ivmp_server_address, (int)sizeof(ivmp_server_address));
	error = WSAGetLastError();
	free(packet);
	if (result == SOCKET_ERROR) {
		WSASetLastError(error);
		return SOCKET_ERROR;
	}
	return length;
}

int WSAAPI ivmp_hook_recvfrom(SOCKET socket, char *buffer, int length, int flags, struct sockaddr *address, int *address_length)
{
	struct ivmp_tunnel_header header;
	const unsigned char *payload = NULL;
	unsigned char *packet;
	int packet_length;
	int copied;

	if (!is_tracked(socket)) {
		return recvfrom(socket, buffer, length, flags, address, address_length);
	}
	packet = (unsigned char *)malloc(UDP_BUFFER_SIZE);
	if (packet == NULL) {
		WSASetLastError(WSAENOBUFS);
		return SOCKET_ERROR;
	}
	packet_length = recvfrom(socket, (char *)packet, (int)UDP_BUFFER_SIZE, flags, address, address_length);
	if (packet_length <= 0) {
		free(packet);
		return packet_length;
	}
	if (!ivmp_decode_tunnel(packet, (size_t)packet_length, &header, &payload)) {
		free(packet);
		WSASetLastError(WSAECONNRESET);
		return SOCKET_ERROR;
	}
	if (header.payload_length != 0u) {
		(void)InterlockedExchange(&ivmp_last_game_traffic, (LONG)GetTickCount());
	}
	copied = header.payload_length > (uint32_t)length ? length : (int)header.payload_length;
	memcpy(buffer, payload, (size_t)copied);
	free(packet);
	rewrite_udp_source(socket, address, address_length, header.connection_id);
	if (header.payload_length > (uint32_t)length) {
		WSASetLastError(WSAEMSGSIZE);
	}
	return copied;
}

int WSAAPI ivmp_hook_closesocket(SOCKET socket)
{
	struct udp_socket *entry;

	EnterCriticalSection(&socket_lock);
	entry = find_udp_socket(socket);
	if (entry != NULL) {
		entry->socket = INVALID_SOCKET;
		entry->registered = 0;
		entry->virtual_port = 0u;
	}
	LeaveCriticalSection(&socket_lock);
	return closesocket(socket);
}

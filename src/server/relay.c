#include "relay.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "endian.h"
#include "protocol.h"
#include "sockcompat.h"

#define PENDING_STREAMS 16u
#define READ_CHUNK 8192
#define PLAYER_TIMEOUT_MS 60000u
#define HANDSHAKE_TIMEOUT_MS 10000u
#define UDP_BUFFER_SIZE (IVMP_TUNNEL_HEADER_SIZE + IVMP_MAX_TUNNEL_PAYLOAD)
#define MAX_CONTROL_MESSAGE (IVMP_CONTROL_HEADER_SIZE + IVMP_MAX_CONTROL_PAYLOAD)
#define OUTPUT_LIMIT (2u * MAX_CONTROL_MESSAGE)
#define LOG_NAME_CAPACITY (IVMP_MAX_NICKNAME * 4u + 1u)

static volatile sig_atomic_t stop_requested = 0;

struct player;

struct stream {
	int fd;
	struct player *player;
	uint64_t accepted_ms;
	unsigned char *input;
	size_t input_length;
	size_t input_capacity;
	unsigned char *output;
	size_t output_length;
	size_t output_capacity;
};

struct player {
	int used;
	struct stream *stream;
	uint32_t connection_id;
	uint64_t token;
	char nickname[IVMP_MAX_NICKNAME + 1u];
	char log_name[LOG_NAME_CAPACITY];
	struct sockaddr_storage udp_address;
	socklen_t udp_address_length;
	int has_udp_address;
	uint64_t last_activity_ms;
	unsigned char *session_record;
	size_t session_record_length;
	uint32_t session_serial;
};

struct relay {
	struct ivmp_server_config config;
	int tcp_listener;
	int udp_socket;
	uint32_t next_connection_id;
	uint16_t player_count;
	uint32_t next_session_serial;
	struct player *players;
	struct stream *streams;
	unsigned int stream_count;
};

static uint64_t elapsed_ms(uint64_t now, uint64_t since)
{
	return now > since ? now - since : 0u;
}

static void escape_for_log(char *output, size_t capacity, const char *input, size_t length)
{
	static const char digits[] = "0123456789abcdef";
	size_t offset = 0u;
	size_t index;

	for (index = 0u; index < length && offset + 5u <= capacity; ++index) {
		unsigned char value = (unsigned char)input[index];

		if (value < 0x20u || value == 0x7fu || value == '\\' || value == '\'') {
			output[offset++] = '\\';
			output[offset++] = 'x';
			output[offset++] = digits[value >> 4];
			output[offset++] = digits[value & 0x0fu];
		} else {
			output[offset++] = (char)value;
		}
	}
	output[offset] = '\0';
}

static int is_would_block(int error)
{
	return error == IVMP_EAGAIN || error == IVMP_EWOULDBLOCK;
}

static int flush_output(struct stream *stream)
{
	size_t sent = 0u;

	while (sent < stream->output_length) {
		ivmp_ssize_t result = IVMP_SEND(stream->fd, stream->output + sent, stream->output_length - sent, 0);

		if (result > 0) {
			sent += (size_t)result;
		} else if (result < 0 && ivmp_socket_errno() == IVMP_EINTR) {
			continue;
		} else if (result < 0 && is_would_block(ivmp_socket_errno())) {
			break;
		} else {
			return 0;
		}
	}
	stream->output_length -= sent;
	if (stream->output_length == 0u) {
		free(stream->output);
		stream->output = NULL;
		stream->output_capacity = 0u;
	} else if (sent != 0u) {
		memmove(stream->output, stream->output + sent, stream->output_length);
	}
	return 1;
}

static int queue_output(struct stream *stream, const unsigned char *data, size_t length)
{
	size_t sent = 0u;
	size_t remainder;
	size_t required;

	if (stream->output_length == 0u) {
		while (sent < length) {
			ivmp_ssize_t result = IVMP_SEND(stream->fd, data + sent, length - sent, 0);

			if (result > 0) {
				sent += (size_t)result;
			} else if (result < 0 && ivmp_socket_errno() == IVMP_EINTR) {
				continue;
			} else if (result < 0 && is_would_block(ivmp_socket_errno())) {
				break;
			} else {
				return 0;
			}
		}
	}
	remainder = length - sent;
	if (remainder == 0u) {
		return 1;
	}
	if (remainder > OUTPUT_LIMIT - stream->output_length) {
		return 0;
	}
	required = stream->output_length + remainder;
	if (required > stream->output_capacity) {
		size_t capacity = stream->output_capacity * 2u;
		unsigned char *resized;

		if (capacity < required) {
			capacity = required;
		}
		if (capacity > OUTPUT_LIMIT) {
			capacity = OUTPUT_LIMIT;
		}
		resized = (unsigned char *)realloc(stream->output, capacity);
		if (resized == NULL) {
			return 0;
		}
		stream->output = resized;
		stream->output_capacity = capacity;
	}
	memcpy(stream->output + stream->output_length, data + sent, remainder);
	stream->output_length = required;
	return 1;
}

static struct player *find_player(struct relay *relay, uint32_t id)
{
	unsigned int index;

	for (index = 0u; index < relay->config.maximum_players; ++index) {
		if (relay->players[index].used && relay->players[index].connection_id == id) {
			return &relay->players[index];
		}
	}
	return NULL;
}

static struct player *find_free_player(struct relay *relay)
{
	unsigned int index;

	if (relay->player_count >= relay->config.maximum_players) {
		return NULL;
	}
	for (index = 0u; index < relay->config.maximum_players; ++index) {
		if (!relay->players[index].used) {
			return &relay->players[index];
		}
	}
	return NULL;
}

static void close_stream(struct relay *relay, struct stream *stream)
{
	struct player *player = stream->player;

	if (player != NULL) {
		fprintf(stdout, "disconnect connection=%u player='%s'\n", (unsigned int)player->connection_id, player->log_name);
		fflush(stdout);
		free(player->session_record);
		memset(player, 0, sizeof(*player));
		--relay->player_count;
	}
	(void)ivmp_socket_close(stream->fd);
	free(stream->input);
	free(stream->output);
	memset(stream, 0, sizeof(*stream));
	stream->fd = -1;
}

static int append_input(struct stream *stream, const unsigned char *data, size_t length)
{
	size_t required = stream->input_length + length;

	if (required > MAX_CONTROL_MESSAGE) {
		return 0;
	}
	if (required > stream->input_capacity) {
		size_t capacity = stream->input_capacity == 0u ? 4096u : stream->input_capacity;
		unsigned char *resized;

		while (capacity < required) {
			capacity *= 2u;
		}
		resized = (unsigned char *)realloc(stream->input, capacity);
		if (resized == NULL) {
			return 0;
		}
		stream->input = resized;
		stream->input_capacity = capacity;
	}
	memcpy(stream->input + stream->input_length, data, length);
	stream->input_length = required;
	return 1;
}

static void consume_input(struct stream *stream, size_t length)
{
	stream->input_length -= length;
	memmove(stream->input, stream->input + length, stream->input_length);
}

static int reject(struct stream *stream, uint16_t code, const char *message)
{
	unsigned char packet[IVMP_CONTROL_HEADER_SIZE + 128u];
	int length = ivmp_encode_error(packet, sizeof(packet), code, message);

	if (length != 0) {
		(void)queue_output(stream, packet, (size_t)length);
	}
	return 0;
}

static uint32_t allocate_connection_id(struct relay *relay, uint32_t requested)
{
	if (requested != 0u && requested < 0x00ffffffu && find_player(relay, requested) == NULL) {
		return requested;
	}
	do {
		requested = relay->next_connection_id++;
		if (relay->next_connection_id >= 0x00ffffffu) {
			relay->next_connection_id = 1u;
		}
	} while (find_player(relay, requested) != NULL);
	return requested;
}

static int register_player(struct relay *relay, struct stream *stream, size_t packet_length)
{
	unsigned char packet[IVMP_CONTROL_HEADER_SIZE + IVMP_WELCOME_PAYLOAD_SIZE];
	unsigned char token[8];
	struct ivmp_hello hello;
	struct player *player;
	int length;

	if (!ivmp_decode_hello(stream->input, packet_length, &hello)) {
		return reject(stream, IVMP_ERROR_BAD_PACKET, "invalid hello packet");
	}
	if (hello.minimum_version > IVMP_PROTOCOL_VERSION || hello.maximum_version < IVMP_PROTOCOL_VERSION) {
		return reject(stream, IVMP_ERROR_UNSUPPORTED_VERSION, "unsupported protocol version");
	}
	player = find_free_player(relay);
	if (player == NULL) {
		return reject(stream, IVMP_ERROR_SERVER_FULL, "server is full");
	}
	if (!ivmp_random_bytes(token, sizeof(token))) {
		fprintf(stderr, "cannot generate session token\n");
		return 0;
	}
	memset(player, 0, sizeof(*player));
	player->used = 1;
	player->stream = stream;
	player->connection_id = allocate_connection_id(relay, hello.requested_connection_id);
	player->token = ivmp_read_u64_be(token);
	player->last_activity_ms = ivmp_monotonic_milliseconds();
	memcpy(player->nickname, hello.nickname, hello.nickname_length);
	escape_for_log(player->log_name, sizeof(player->log_name), player->nickname, hello.nickname_length);
	stream->player = player;
	++relay->player_count;

	length = ivmp_encode_welcome(packet, sizeof(packet), player->connection_id, player->token);
	if (length == 0 || !queue_output(stream, packet, (size_t)length)) {
		return 0;
	}
	fprintf(stdout, "connected connection=%u player='%s' players=%u/%u\n", (unsigned int)player->connection_id, player->log_name, (unsigned int)relay->player_count, (unsigned int)relay->config.maximum_players);
	fflush(stdout);
	return 1;
}

static int store_session_record(struct relay *relay, struct player *player, const unsigned char *record, size_t length)
{
	unsigned char *copy = NULL;
	int had_record = player->session_record != NULL;

	if (length > IVMP_MAX_SESSION_RECORD) {
		return 0;
	}
	if (length != 0u) {
		copy = (unsigned char *)malloc(length);
		if (copy == NULL) {
			return 0;
		}
		memcpy(copy, record, length);
	}
	free(player->session_record);
	player->session_record = copy;
	player->session_record_length = length;
	if (!had_record && copy != NULL) {
		player->session_serial = ++relay->next_session_serial;
		fprintf(stdout, "session hosted connection=%u player='%s'\n", (unsigned int)player->connection_id, player->log_name);
	} else if (had_record && copy == NULL) {
		fprintf(stdout, "session closed connection=%u player='%s'\n", (unsigned int)player->connection_id, player->log_name);
	}
	fflush(stdout);
	return 1;
}

static int send_session_list(struct relay *relay, struct stream *stream)
{
	unsigned char *packet = (unsigned char *)malloc(MAX_CONTROL_MESSAGE);
	unsigned char *payload;
	size_t offset = 4u;
	uint32_t count = 0u;
	unsigned int index;
	int result;

	if (packet == NULL) {
		return 0;
	}
	payload = packet + IVMP_CONTROL_HEADER_SIZE;
	for (index = 0u; index < relay->config.maximum_players; ++index) {
		const struct player *player = &relay->players[index];

		if (!player->used || player->session_record == NULL || offset + 12u + player->session_record_length > IVMP_MAX_CONTROL_PAYLOAD) {
			continue;
		}
		ivmp_write_u32_be(payload + offset, player->connection_id);
		ivmp_write_u32_be(payload + offset + 4u, player->session_serial);
		ivmp_write_u32_be(payload + offset + 8u, (uint32_t)player->session_record_length);
		memcpy(payload + offset + 12u, player->session_record, player->session_record_length);
		offset += 12u + player->session_record_length;
		++count;
	}
	ivmp_write_u32_be(payload, count);
	result = ivmp_encode_control_header(packet, IVMP_CONTROL_HEADER_SIZE + offset, IVMP_CONTROL_SERVER_SESSION_LIST, (uint32_t)offset) != 0 && queue_output(stream, packet, IVMP_CONTROL_HEADER_SIZE + offset);
	free(packet);
	return result;
}

static int handle_message(struct relay *relay, struct stream *stream, const struct ivmp_control_header *header, size_t packet_length)
{
	struct player *player = stream->player;
	unsigned char packet[IVMP_CONTROL_HEADER_SIZE];

	if (player == NULL) {
		return header->type == IVMP_CONTROL_CLIENT_HELLO && register_player(relay, stream, packet_length);
	}
	player->last_activity_ms = ivmp_monotonic_milliseconds();
	switch (header->type) {
	case IVMP_CONTROL_CLIENT_PING:
		return ivmp_encode_control_header(packet, sizeof(packet), IVMP_CONTROL_SERVER_PONG, 0u) != 0 && queue_output(stream, packet, sizeof(packet));
	case IVMP_CONTROL_CLIENT_SESSION_PUBLISH:
		return store_session_record(relay, player, stream->input + IVMP_CONTROL_HEADER_SIZE, header->payload_length);
	case IVMP_CONTROL_CLIENT_SESSION_QUERY:
		return send_session_list(relay, stream);
	default:
		return 0;
	}
}

static int process_input(struct relay *relay, struct stream *stream)
{
	while (stream->input_length >= IVMP_CONTROL_HEADER_SIZE) {
		struct ivmp_control_header header;
		size_t packet_length;

		if (!ivmp_decode_control_header(stream->input, stream->input_length, &header)) {
			return 0;
		}
		packet_length = IVMP_CONTROL_HEADER_SIZE + header.payload_length;
		if (stream->input_length < packet_length) {
			break;
		}
		if (!handle_message(relay, stream, &header, packet_length)) {
			return 0;
		}
		consume_input(stream, packet_length);
	}
	return 1;
}

static int read_stream(struct relay *relay, struct stream *stream)
{
	unsigned char buffer[READ_CHUNK];

	for (;;) {
		ivmp_ssize_t received = IVMP_RECV(stream->fd, buffer, sizeof(buffer), 0);

		if (received > 0) {
			if (!append_input(stream, buffer, (size_t)received) || !process_input(relay, stream)) {
				return 0;
			}
		} else if (received == 0) {
			return 0;
		} else if (is_would_block(ivmp_socket_errno())) {
			return 1;
		} else if (ivmp_socket_errno() != IVMP_EINTR) {
			return 0;
		}
	}
}

static void accept_stream(struct relay *relay)
{
	int fd = IVMP_ACCEPT(relay->tcp_listener, NULL, NULL);
	unsigned int index;

	if (fd < 0) {
		return;
	}
	if (IVMP_FD_SELECTABLE(fd) && ivmp_socket_set_nonblocking(fd)) {
		for (index = 0u; index < relay->stream_count; ++index) {
			if (relay->streams[index].fd < 0) {
				relay->streams[index].fd = fd;
				relay->streams[index].accepted_ms = ivmp_monotonic_milliseconds();
				return;
			}
		}
		fprintf(stderr, "connection rejected: stream limit reached\n");
	}
	(void)ivmp_socket_close(fd);
}

static void send_udp(struct relay *relay, const struct player *target, const unsigned char *frame, size_t frame_length)
{
	if (target->has_udp_address) {
		(void)IVMP_SENDTO(relay->udp_socket, frame, frame_length, 0, (const struct sockaddr *)&target->udp_address, target->udp_address_length);
	}
}

static void forward_udp_frame(struct relay *relay, const struct player *source, uint32_t target_id, const unsigned char *frame, size_t frame_length)
{
	unsigned int index;

	if (target_id != 0u) {
		const struct player *target = find_player(relay, target_id);

		if (target != NULL && target != source) {
			send_udp(relay, target, frame, frame_length);
		}
		return;
	}
	for (index = 0u; index < relay->config.maximum_players; ++index) {
		if (relay->players[index].used && &relay->players[index] != source) {
			send_udp(relay, &relay->players[index], frame, frame_length);
		}
	}
}

static void receive_udp(struct relay *relay)
{
	static unsigned char buffer[UDP_BUFFER_SIZE];
	struct sockaddr_storage source_address;
	socklen_t source_length = sizeof(source_address);
	struct ivmp_tunnel_header header;
	const unsigned char *payload = NULL;
	struct player *player;
	ivmp_ssize_t received = IVMP_RECVFROM(relay->udp_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&source_address, &source_length);

	if (received < 0 || !ivmp_decode_tunnel(buffer, (size_t)received, &header, &payload)) {
		return;
	}
	player = find_player(relay, header.connection_id);
	if (player == NULL || header.token != player->token) {
		return;
	}
	memcpy(&player->udp_address, &source_address, source_length);
	player->udp_address_length = source_length;
	player->has_udp_address = 1;
	player->last_activity_ms = ivmp_monotonic_milliseconds();
	if (header.payload_length != 0u) {
		memset(buffer + IVMP_TUNNEL_TOKEN_OFFSET, 0, 8u);
		forward_udp_frame(relay, player, header.target_id, buffer, IVMP_TUNNEL_HEADER_SIZE + (size_t)header.payload_length);
	}
}

static void expire_idle_streams(struct relay *relay)
{
	uint64_t now = ivmp_monotonic_milliseconds();
	unsigned int index;

	for (index = 0u; index < relay->stream_count; ++index) {
		struct stream *stream = &relay->streams[index];

		if (stream->fd < 0) {
			continue;
		}
		if (stream->player == NULL) {
			if (elapsed_ms(now, stream->accepted_ms) > HANDSHAKE_TIMEOUT_MS) {
				close_stream(relay, stream);
			}
		} else if (elapsed_ms(now, stream->player->last_activity_ms) > PLAYER_TIMEOUT_MS) {
			fprintf(stdout, "timeout connection=%u player='%s'\n", (unsigned int)stream->player->connection_id, stream->player->log_name);
			close_stream(relay, stream);
		}
	}
}

static int open_listener(const struct ivmp_server_config *config, int type)
{
	struct addrinfo hints;
	struct addrinfo *results = NULL;
	struct addrinfo *current;
	char port[16];
	int fd = -1;
	int result;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = type;
	hints.ai_flags = AI_PASSIVE;
	(void)snprintf(port, sizeof(port), "%u", (unsigned int)config->bind_port);
	result = getaddrinfo(config->bind_host, port, &hints, &results);
	if (result != 0) {
		fprintf(stderr, "cannot resolve bind address: %s\n", gai_strerror(result));
		return -1;
	}
	for (current = results; current != NULL; current = current->ai_next) {
		int reuse = 1;

		fd = IVMP_SOCKET(current->ai_family, current->ai_socktype, current->ai_protocol);
		if (fd < 0) {
			continue;
		}
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
		if (bind(fd, current->ai_addr, (int)current->ai_addrlen) == 0 && (type != SOCK_STREAM || listen(fd, 16) == 0) && ivmp_socket_set_nonblocking(fd)) {
			break;
		}
		(void)ivmp_socket_close(fd);
		fd = -1;
	}
	freeaddrinfo(results);
	if (fd < 0) {
		fprintf(stderr, "cannot open %s listener: %s\n", type == SOCK_STREAM ? "TCP" : "UDP", ivmp_socket_strerror(ivmp_socket_errno()));
	}
	return fd;
}

static void close_relay(struct relay *relay)
{
	unsigned int index;

	for (index = 0u; index < relay->stream_count; ++index) {
		if (relay->streams[index].fd >= 0) {
			close_stream(relay, &relay->streams[index]);
		}
	}
	if (relay->tcp_listener >= 0) {
		(void)ivmp_socket_close(relay->tcp_listener);
	}
	if (relay->udp_socket >= 0) {
		(void)ivmp_socket_close(relay->udp_socket);
	}
	free(relay->players);
	free(relay->streams);
}

void ivmp_relay_request_stop(void)
{
	stop_requested = 1;
}

int ivmp_relay_run(const struct ivmp_server_config *config)
{
	static struct relay relay;
	unsigned int index;

	memset(&relay, 0, sizeof(relay));
	relay.config = *config;
	relay.next_connection_id = 1u;
	relay.tcp_listener = -1;
	relay.udp_socket = -1;
	relay.stream_count = config->maximum_players + PENDING_STREAMS;
	relay.players = (struct player *)calloc(config->maximum_players, sizeof(*relay.players));
	relay.streams = (struct stream *)calloc(relay.stream_count, sizeof(*relay.streams));
	if (relay.players == NULL || relay.streams == NULL) {
		fprintf(stderr, "cannot allocate %u player slots\n", (unsigned int)config->maximum_players);
		relay.stream_count = 0u;
		close_relay(&relay);
		return 0;
	}
	for (index = 0u; index < relay.stream_count; ++index) {
		relay.streams[index].fd = -1;
	}
	relay.tcp_listener = open_listener(config, SOCK_STREAM);
	relay.udp_socket = relay.tcp_listener < 0 ? -1 : open_listener(config, SOCK_DGRAM);
	if (relay.udp_socket < 0) {
		close_relay(&relay);
		return 0;
	}
	fprintf(stdout, "IVMP relay %u listening on %s:%u (TCP+UDP), max_players=%u\n", (unsigned int)IVMP_PROTOCOL_VERSION, config->bind_host, (unsigned int)config->bind_port, (unsigned int)config->maximum_players);
	fflush(stdout);

	while (!stop_requested) {
		fd_set read_fds;
		fd_set write_fds;
		int max_fd = relay.tcp_listener > relay.udp_socket ? relay.tcp_listener : relay.udp_socket;
		struct timeval timeout;

		FD_ZERO(&read_fds);
		FD_ZERO(&write_fds);
		FD_SET(relay.tcp_listener, &read_fds);
		FD_SET(relay.udp_socket, &read_fds);
		for (index = 0u; index < relay.stream_count; ++index) {
			if (relay.streams[index].fd >= 0) {
				FD_SET(relay.streams[index].fd, &read_fds);
				if (relay.streams[index].output_length != 0u) {
					FD_SET(relay.streams[index].fd, &write_fds);
				}
				if (relay.streams[index].fd > max_fd) {
					max_fd = relay.streams[index].fd;
				}
			}
		}
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;
		if (select(max_fd + 1, &read_fds, &write_fds, NULL, &timeout) < 0) {
			if (ivmp_socket_errno() == IVMP_EINTR) {
				continue;
			}
			fprintf(stderr, "select failed: %s\n", ivmp_socket_strerror(ivmp_socket_errno()));
			break;
		}
		if (FD_ISSET(relay.tcp_listener, &read_fds)) {
			accept_stream(&relay);
		}
		if (FD_ISSET(relay.udp_socket, &read_fds)) {
			receive_udp(&relay);
		}
		for (index = 0u; index < relay.stream_count; ++index) {
			struct stream *stream = &relay.streams[index];

			if (stream->fd >= 0 && stream->output_length != 0u && FD_ISSET(stream->fd, &write_fds) && !flush_output(stream)) {
				close_stream(&relay, stream);
			}
			if (stream->fd >= 0 && FD_ISSET(stream->fd, &read_fds) && !read_stream(&relay, stream)) {
				close_stream(&relay, stream);
			}
		}
		expire_idle_streams(&relay);
	}
	close_relay(&relay);
	return 1;
}

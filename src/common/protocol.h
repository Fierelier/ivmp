#ifndef IVMP_PROTOCOL_H
#define IVMP_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define IVMP_PROTOCOL_VERSION 5u
#define IVMP_CONTROL_HEADER_SIZE 16u
#define IVMP_TUNNEL_HEADER_SIZE 28u
#define IVMP_TUNNEL_TOKEN_OFFSET 20u
#define IVMP_WELCOME_PAYLOAD_SIZE 12u
#define IVMP_MAX_CONTROL_PAYLOAD 65536u
#define IVMP_MAX_SESSION_RECORD 4096u
#define IVMP_MAX_TUNNEL_PAYLOAD 65535u
#define IVMP_MAX_NICKNAME 64u
#define IVMP_DEFAULT_PORT 42123u
#define IVMP_DEFAULT_MAX_PLAYERS 32u
#define IVMP_MAX_PLAYERS 1000u
#define IVMP_HEARTBEAT_MS 5000u

extern const unsigned char ivmp_control_magic[8];
extern const unsigned char ivmp_tunnel_magic[4];

enum ivmp_control_type {
	IVMP_CONTROL_CLIENT_HELLO = 1,
	IVMP_CONTROL_SERVER_WELCOME = 2,
	IVMP_CONTROL_CLIENT_PING = 3,
	IVMP_CONTROL_SERVER_PONG = 4,
	IVMP_CONTROL_CLIENT_GOODBYE = 5,
	IVMP_CONTROL_SERVER_ERROR = 6,
	IVMP_CONTROL_CLIENT_SESSION_PUBLISH = 8,
	IVMP_CONTROL_CLIENT_SESSION_QUERY = 9,
	IVMP_CONTROL_SERVER_SESSION_LIST = 10
};

enum ivmp_error_code {
	IVMP_ERROR_UNSUPPORTED_VERSION = 2,
	IVMP_ERROR_BAD_PACKET = 3,
	IVMP_ERROR_SERVER_FULL = 4
};

struct ivmp_control_header {
	uint16_t version;
	uint16_t type;
	uint32_t payload_length;
};

struct ivmp_tunnel_header {
	uint32_t connection_id;
	uint32_t payload_length;
	uint16_t flags;
	uint32_t target_id;
	uint64_t token;
};

struct ivmp_hello {
	uint16_t minimum_version;
	uint16_t maximum_version;
	uint32_t requested_connection_id;
	uint16_t nickname_length;
	const unsigned char *nickname;
};

struct ivmp_error {
	uint16_t code;
	uint16_t message_length;
	const unsigned char *message;
};

int ivmp_encode_control_header(unsigned char *output, size_t capacity, uint16_t type, uint32_t payload_length);
int ivmp_decode_control_header(const unsigned char *input, size_t length, struct ivmp_control_header *header);
int ivmp_encode_hello(unsigned char *output, size_t capacity, const struct ivmp_hello *hello);
int ivmp_decode_hello(const unsigned char *input, size_t length, struct ivmp_hello *hello);
int ivmp_encode_welcome(unsigned char *output, size_t capacity, uint32_t connection_id, uint64_t token);
int ivmp_decode_welcome(const unsigned char *input, size_t length, uint32_t *connection_id, uint64_t *token);
int ivmp_encode_error(unsigned char *output, size_t capacity, uint16_t code, const char *message);
int ivmp_decode_error(const unsigned char *input, size_t length, struct ivmp_error *error);
int ivmp_encode_tunnel(unsigned char *output, size_t capacity, uint32_t connection_id, uint64_t token, uint32_t target_id, uint16_t flags, const unsigned char *payload, size_t payload_length);
int ivmp_decode_tunnel(const unsigned char *input, size_t length, struct ivmp_tunnel_header *header, const unsigned char **payload);
int ivmp_control_type_is_valid(uint16_t type);

#endif

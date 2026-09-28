#include "protocol.h"

#include <string.h>

#include "endian.h"

#define HELLO_FIXED_SIZE 10u

const unsigned char ivmp_control_magic[8] = {
	'I', 'V', 'M', 'P', 'C', 'T', 'L', 0
};

const unsigned char ivmp_tunnel_magic[4] = {
	'I', 'V', 'M', 'T'
};

static int nickname_is_valid(const unsigned char *nickname, size_t length)
{
	size_t index;

	if (length == 0u || length > IVMP_MAX_NICKNAME) {
		return 0;
	}
	for (index = 0u; index < length; ++index) {
		if (nickname[index] == 0u || nickname[index] == '\n' || nickname[index] == '\r') {
			return 0;
		}
	}
	return 1;
}

static const unsigned char *decode_payload(const unsigned char *input, size_t length, uint16_t type, struct ivmp_control_header *header)
{
	if (!ivmp_decode_control_header(input, length, header) || header->type != type || length < IVMP_CONTROL_HEADER_SIZE + header->payload_length) {
		return NULL;
	}
	return input + IVMP_CONTROL_HEADER_SIZE;
}

int ivmp_encode_control_header(unsigned char *output, size_t capacity, uint16_t type, uint32_t payload_length)
{
	if (output == NULL || capacity < IVMP_CONTROL_HEADER_SIZE + (size_t)payload_length || payload_length > IVMP_MAX_CONTROL_PAYLOAD || !ivmp_control_type_is_valid(type)) {
		return 0;
	}
	memcpy(output, ivmp_control_magic, sizeof(ivmp_control_magic));
	ivmp_write_u16_be(output + 8, IVMP_PROTOCOL_VERSION);
	ivmp_write_u16_be(output + 10, type);
	ivmp_write_u32_be(output + 12, payload_length);
	return (int)(IVMP_CONTROL_HEADER_SIZE + payload_length);
}

int ivmp_decode_control_header(const unsigned char *input, size_t length, struct ivmp_control_header *header)
{
	uint16_t version;
	uint16_t type;
	uint32_t payload_length;

	if (input == NULL || header == NULL || length < IVMP_CONTROL_HEADER_SIZE || memcmp(input, ivmp_control_magic, sizeof(ivmp_control_magic)) != 0) {
		return 0;
	}
	version = ivmp_read_u16_be(input + 8);
	type = ivmp_read_u16_be(input + 10);
	payload_length = ivmp_read_u32_be(input + 12);
	if (version != IVMP_PROTOCOL_VERSION || payload_length > IVMP_MAX_CONTROL_PAYLOAD || !ivmp_control_type_is_valid(type)) {
		return 0;
	}
	header->version = version;
	header->type = type;
	header->payload_length = payload_length;
	return 1;
}

int ivmp_encode_hello(unsigned char *output, size_t capacity, const struct ivmp_hello *hello)
{
	unsigned char *payload;
	int length;

	if (hello == NULL || hello->nickname == NULL || !nickname_is_valid(hello->nickname, hello->nickname_length)) {
		return 0;
	}
	length = ivmp_encode_control_header(output, capacity, IVMP_CONTROL_CLIENT_HELLO, HELLO_FIXED_SIZE + hello->nickname_length);
	if (length == 0) {
		return 0;
	}
	payload = output + IVMP_CONTROL_HEADER_SIZE;
	ivmp_write_u16_be(payload, hello->minimum_version);
	ivmp_write_u16_be(payload + 2, hello->maximum_version);
	ivmp_write_u32_be(payload + 4, hello->requested_connection_id);
	ivmp_write_u16_be(payload + 8, hello->nickname_length);
	memcpy(payload + HELLO_FIXED_SIZE, hello->nickname, hello->nickname_length);
	return length;
}

int ivmp_decode_hello(const unsigned char *input, size_t length, struct ivmp_hello *hello)
{
	struct ivmp_control_header header;
	const unsigned char *payload = decode_payload(input, length, IVMP_CONTROL_CLIENT_HELLO, &header);
	uint16_t nickname_length;

	if (payload == NULL || hello == NULL || header.payload_length < HELLO_FIXED_SIZE) {
		return 0;
	}
	nickname_length = ivmp_read_u16_be(payload + 8);
	if (header.payload_length != HELLO_FIXED_SIZE + (size_t)nickname_length || !nickname_is_valid(payload + HELLO_FIXED_SIZE, nickname_length)) {
		return 0;
	}
	hello->minimum_version = ivmp_read_u16_be(payload);
	hello->maximum_version = ivmp_read_u16_be(payload + 2);
	hello->requested_connection_id = ivmp_read_u32_be(payload + 4);
	hello->nickname_length = nickname_length;
	hello->nickname = payload + HELLO_FIXED_SIZE;
	return 1;
}

int ivmp_encode_welcome(unsigned char *output, size_t capacity, uint32_t connection_id, uint64_t token)
{
	int length = ivmp_encode_control_header(output, capacity, IVMP_CONTROL_SERVER_WELCOME, IVMP_WELCOME_PAYLOAD_SIZE);

	if (length != 0) {
		ivmp_write_u32_be(output + IVMP_CONTROL_HEADER_SIZE, connection_id);
		ivmp_write_u64_be(output + IVMP_CONTROL_HEADER_SIZE + 4u, token);
	}
	return length;
}

int ivmp_decode_welcome(const unsigned char *input, size_t length, uint32_t *connection_id, uint64_t *token)
{
	struct ivmp_control_header header;
	const unsigned char *payload = decode_payload(input, length, IVMP_CONTROL_SERVER_WELCOME, &header);

	if (payload == NULL || connection_id == NULL || token == NULL || header.payload_length != IVMP_WELCOME_PAYLOAD_SIZE) {
		return 0;
	}
	*connection_id = ivmp_read_u32_be(payload);
	*token = ivmp_read_u64_be(payload + 4);
	return 1;
}

int ivmp_encode_error(unsigned char *output, size_t capacity, uint16_t code, const char *message)
{
	size_t message_length;
	int length;

	if (message == NULL) {
		return 0;
	}
	message_length = strlen(message);
	if (message_length > 0xffffu - 2u) {
		return 0;
	}
	length = ivmp_encode_control_header(output, capacity, IVMP_CONTROL_SERVER_ERROR, (uint32_t)(2u + message_length));
	if (length != 0) {
		ivmp_write_u16_be(output + IVMP_CONTROL_HEADER_SIZE, code);
		memcpy(output + IVMP_CONTROL_HEADER_SIZE + 2u, message, message_length);
	}
	return length;
}

int ivmp_decode_error(const unsigned char *input, size_t length, struct ivmp_error *error)
{
	struct ivmp_control_header header;
	const unsigned char *payload = decode_payload(input, length, IVMP_CONTROL_SERVER_ERROR, &header);

	if (payload == NULL || error == NULL || header.payload_length < 2u || header.payload_length > 0xffffu) {
		return 0;
	}
	error->code = ivmp_read_u16_be(payload);
	error->message_length = (uint16_t)(header.payload_length - 2u);
	error->message = payload + 2;
	return 1;
}

int ivmp_encode_tunnel(unsigned char *output, size_t capacity, uint32_t connection_id, uint64_t token, uint32_t target_id, uint16_t flags, const unsigned char *payload, size_t payload_length)
{
	if (output == NULL || payload_length > IVMP_MAX_TUNNEL_PAYLOAD || (payload_length != 0u && payload == NULL) || capacity < IVMP_TUNNEL_HEADER_SIZE + payload_length) {
		return 0;
	}
	memcpy(output, ivmp_tunnel_magic, sizeof(ivmp_tunnel_magic));
	ivmp_write_u16_be(output + 4, IVMP_PROTOCOL_VERSION);
	ivmp_write_u16_be(output + 6, flags);
	ivmp_write_u32_be(output + 8, connection_id);
	ivmp_write_u32_be(output + 12, (uint32_t)payload_length);
	ivmp_write_u32_be(output + 16, target_id);
	ivmp_write_u64_be(output + IVMP_TUNNEL_TOKEN_OFFSET, token);
	if (payload_length != 0u) {
		memmove(output + IVMP_TUNNEL_HEADER_SIZE, payload, payload_length);
	}
	return (int)(IVMP_TUNNEL_HEADER_SIZE + payload_length);
}

int ivmp_decode_tunnel(const unsigned char *input, size_t length, struct ivmp_tunnel_header *header, const unsigned char **payload)
{
	uint32_t payload_length;

	if (input == NULL || header == NULL || payload == NULL || length < IVMP_TUNNEL_HEADER_SIZE || memcmp(input, ivmp_tunnel_magic, sizeof(ivmp_tunnel_magic)) != 0) {
		return 0;
	}
	payload_length = ivmp_read_u32_be(input + 12);
	if (ivmp_read_u16_be(input + 4) != IVMP_PROTOCOL_VERSION || payload_length > IVMP_MAX_TUNNEL_PAYLOAD || length < IVMP_TUNNEL_HEADER_SIZE + (size_t)payload_length) {
		return 0;
	}
	header->connection_id = ivmp_read_u32_be(input + 8);
	header->payload_length = payload_length;
	header->flags = ivmp_read_u16_be(input + 6);
	header->target_id = ivmp_read_u32_be(input + 16);
	header->token = ivmp_read_u64_be(input + IVMP_TUNNEL_TOKEN_OFFSET);
	*payload = input + IVMP_TUNNEL_HEADER_SIZE;
	return 1;
}

int ivmp_control_type_is_valid(uint16_t type)
{
	return type >= IVMP_CONTROL_CLIENT_HELLO && type <= IVMP_CONTROL_SERVER_SESSION_LIST;
}

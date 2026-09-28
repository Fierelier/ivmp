#include <stdio.h>
#include <string.h>

#include "endian.h"
#include "protocol.h"

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			return 0; \
		} \
	} while (0)

static int test_endian(void)
{
	unsigned char buffer[8];

	ivmp_write_u16_be(buffer, 0x1234u);
	CHECK(ivmp_read_u16_be(buffer) == 0x1234u);
	ivmp_write_u32_be(buffer, 0x89abcdefu);
	CHECK(ivmp_read_u32_be(buffer) == 0x89abcdefu);
	CHECK(buffer[0] == 0x89u && buffer[3] == 0xefu);
	ivmp_write_u64_be(buffer, 0x0102030405060708ull);
	CHECK(ivmp_read_u64_be(buffer) == 0x0102030405060708ull);
	CHECK(buffer[0] == 0x01u && buffer[7] == 0x08u);
	return 1;
}

static int test_control_header(void)
{
	unsigned char packet[IVMP_CONTROL_HEADER_SIZE];
	struct ivmp_control_header decoded;

	CHECK(ivmp_encode_control_header(packet, sizeof(packet), IVMP_CONTROL_CLIENT_PING, 0u) == (int)IVMP_CONTROL_HEADER_SIZE);
	CHECK(ivmp_decode_control_header(packet, sizeof(packet), &decoded));
	CHECK(decoded.version == IVMP_PROTOCOL_VERSION);
	CHECK(decoded.type == IVMP_CONTROL_CLIENT_PING);
	CHECK(decoded.payload_length == 0u);
	CHECK(!ivmp_encode_control_header(packet, sizeof(packet), IVMP_CONTROL_CLIENT_PING, 1u));
	packet[8] = 0xffu;
	CHECK(!ivmp_decode_control_header(packet, sizeof(packet), &decoded));
	return 1;
}

static int test_hello(void)
{
	static const unsigned char nickname[] = "Player One";
	unsigned char packet[IVMP_CONTROL_HEADER_SIZE + 64u];
	struct ivmp_hello input;
	struct ivmp_hello output;
	int length;

	memset(&input, 0, sizeof(input));
	input.minimum_version = IVMP_PROTOCOL_VERSION;
	input.maximum_version = IVMP_PROTOCOL_VERSION;
	input.requested_connection_id = 0x123456u;
	input.nickname_length = (uint16_t)(sizeof(nickname) - 1u);
	input.nickname = nickname;
	length = ivmp_encode_hello(packet, sizeof(packet), &input);
	CHECK(length == (int)(IVMP_CONTROL_HEADER_SIZE + 10u + input.nickname_length));
	CHECK(ivmp_decode_hello(packet, (size_t)length, &output));
	CHECK(output.minimum_version == IVMP_PROTOCOL_VERSION);
	CHECK(output.maximum_version == IVMP_PROTOCOL_VERSION);
	CHECK(output.requested_connection_id == 0x123456u);
	CHECK(output.nickname_length == input.nickname_length);
	CHECK(memcmp(output.nickname, nickname, input.nickname_length) == 0);
	CHECK(!ivmp_decode_hello(packet, (size_t)length - 1u, &output));
	return 1;
}

static int test_welcome(void)
{
	unsigned char packet[IVMP_CONTROL_HEADER_SIZE + IVMP_WELCOME_PAYLOAD_SIZE];
	uint32_t connection_id = 0u;
	uint64_t token = 0u;

	CHECK(ivmp_encode_welcome(packet, sizeof(packet), 42u, 0x0123456789abcdefull) == (int)sizeof(packet));
	CHECK(ivmp_decode_welcome(packet, sizeof(packet), &connection_id, &token));
	CHECK(connection_id == 42u);
	CHECK(token == 0x0123456789abcdefull);
	CHECK(!ivmp_decode_welcome(packet, sizeof(packet) - 1u, &connection_id, &token));
	return 1;
}

static int test_error(void)
{
	unsigned char packet[IVMP_CONTROL_HEADER_SIZE + 32u];
	struct ivmp_error error;
	int length = ivmp_encode_error(packet, sizeof(packet), IVMP_ERROR_SERVER_FULL, "server is full");

	CHECK(length == (int)(IVMP_CONTROL_HEADER_SIZE + 16u));
	CHECK(ivmp_decode_error(packet, (size_t)length, &error));
	CHECK(error.code == IVMP_ERROR_SERVER_FULL);
	CHECK(error.message_length == 14u);
	CHECK(memcmp(error.message, "server is full", 14u) == 0);
	return 1;
}

static int test_tunnel(void)
{
	static const unsigned char payload[] = { 1u, 2u, 3u, 4u, 5u };
	unsigned char packet[IVMP_TUNNEL_HEADER_SIZE + sizeof(payload)];
	struct ivmp_tunnel_header header;
	const unsigned char *decoded_payload = NULL;

	CHECK(ivmp_encode_tunnel(packet, sizeof(packet), 99u, 0xfedcba9876543210ull, 7u, 0u, payload, sizeof(payload)));
	CHECK(ivmp_decode_tunnel(packet, sizeof(packet), &header, &decoded_payload));
	CHECK(header.connection_id == 99u);
	CHECK(header.target_id == 7u);
	CHECK(header.token == 0xfedcba9876543210ull);
	CHECK(header.payload_length == sizeof(payload));
	CHECK(memcmp(decoded_payload, payload, sizeof(payload)) == 0);
	return 1;
}

int main(void)
{
	CHECK(test_endian());
	CHECK(test_control_header());
	CHECK(test_hello());
	CHECK(test_welcome());
	CHECK(test_error());
	CHECK(test_tunnel());
	puts("protocol tests passed");
	return 0;
}

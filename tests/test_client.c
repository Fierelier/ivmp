#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"

static int receive_all(int fd, unsigned char *data, size_t length)
{
	size_t received = 0u;

	while (received < length) {
		ssize_t result = recv(fd, data + received, length - received, 0);

		if (result <= 0) {
			return 0;
		}
		received += (size_t)result;
	}
	return 1;
}

static int receive_message(int fd, unsigned char *packet, size_t capacity, struct ivmp_control_header *header)
{
	return receive_all(fd, packet, IVMP_CONTROL_HEADER_SIZE) && ivmp_decode_control_header(packet, IVMP_CONTROL_HEADER_SIZE, header) && header->payload_length <= capacity - IVMP_CONTROL_HEADER_SIZE && receive_all(fd, packet + IVMP_CONTROL_HEADER_SIZE, header->payload_length);
}

int main(int argc, char **argv)
{
	static const unsigned char nickname[] = "Smoke Test";
	struct sockaddr_in address;
	struct ivmp_hello hello;
	struct ivmp_control_header header;
	struct ivmp_error error;
	unsigned char packet[IVMP_CONTROL_HEADER_SIZE + 128u];
	uint32_t connection_id;
	uint64_t token;
	int length;
	int fd;
	int port;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
		return 2;
	}
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return 1;
	}
	port = atoi(argv[2]);
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	((unsigned char *)&address.sin_port)[0] = (unsigned char)(port >> 8);
	((unsigned char *)&address.sin_port)[1] = (unsigned char)port;
	if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1 || connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		(void)close(fd);
		return 1;
	}

	memset(&hello, 0, sizeof(hello));
	hello.minimum_version = IVMP_PROTOCOL_VERSION;
	hello.maximum_version = IVMP_PROTOCOL_VERSION;
	hello.nickname_length = (uint16_t)(sizeof(nickname) - 1u);
	hello.nickname = nickname;
	length = ivmp_encode_hello(packet, sizeof(packet), &hello);
	if (length == 0 || send(fd, packet, (size_t)length, 0) != length || !receive_message(fd, packet, sizeof(packet), &header)) {
		fprintf(stderr, "failed to receive welcome\n");
		(void)close(fd);
		return 1;
	}
	if (ivmp_decode_error(packet, IVMP_CONTROL_HEADER_SIZE + header.payload_length, &error)) {
		printf("rejected code=%u message='%.*s'\n", (unsigned int)error.code, (int)error.message_length, (const char *)error.message);
		(void)close(fd);
		return 3;
	}
	if (!ivmp_decode_welcome(packet, IVMP_CONTROL_HEADER_SIZE + header.payload_length, &connection_id, &token)) {
		fprintf(stderr, "invalid welcome\n");
		(void)close(fd);
		return 1;
	}
	length = ivmp_encode_control_header(packet, sizeof(packet), IVMP_CONTROL_CLIENT_PING, 0u);
	if (send(fd, packet, (size_t)length, 0) != length || !receive_message(fd, packet, sizeof(packet), &header) || header.type != IVMP_CONTROL_SERVER_PONG || header.payload_length != 0u) {
		fprintf(stderr, "invalid pong\n");
		(void)close(fd);
		return 1;
	}
	printf("connected id=%u\n", (unsigned int)connection_id);
	(void)close(fd);
	return 0;
}

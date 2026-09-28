#ifndef IVMP_CONFIG_H
#define IVMP_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define IVMP_HOST_CAPACITY 256

struct ivmp_server_config {
	char bind_host[IVMP_HOST_CAPACITY];
	uint16_t bind_port;
	uint16_t maximum_players;
};

int ivmp_server_config_load(const char *path, struct ivmp_server_config *config, char *error, size_t error_capacity);

#endif

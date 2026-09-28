#include <signal.h>
#include <stdio.h>

#include "config.h"
#include "relay.h"
#include "sockcompat.h"

static void handle_signal(int signal_number)
{
	(void)signal_number;
	ivmp_relay_request_stop();
}

int main(int argc, char **argv)
{
	struct ivmp_server_config config;
	char error[256];
	int result;

	if (argc != 2) {
		fprintf(stderr, "usage: ivmp_server <config file>\n");
		return 2;
	}
	if (!ivmp_server_config_load(argv[1], &config, error, sizeof(error))) {
		fprintf(stderr, "%s: %s\n", argv[1], error);
		return 1;
	}
	(void)signal(SIGINT, handle_signal);
	(void)signal(SIGTERM, handle_signal);
	if (!ivmp_sockets_init()) {
		fprintf(stderr, "cannot initialize sockets\n");
		return 1;
	}
	result = ivmp_relay_run(&config) ? 0 : 1;
	ivmp_sockets_cleanup();
	return result;
}

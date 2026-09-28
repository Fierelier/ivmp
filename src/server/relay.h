#ifndef IVMP_RELAY_H
#define IVMP_RELAY_H

#include "config.h"

void ivmp_relay_request_stop(void);
int ivmp_relay_run(const struct ivmp_server_config *config);

#endif

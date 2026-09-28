#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"

static void copy_bounded(char *destination, size_t capacity, const char *source)
{
	size_t length = strlen(source);

	if (capacity == 0u) {
		return;
	}
	if (length >= capacity) {
		length = capacity - 1u;
	}
	memcpy(destination, source, length);
	destination[length] = '\0';
}

static int fail(char *error, size_t capacity, const char *format, ...)
{
	va_list arguments;

	if (error != NULL && capacity != 0u) {
		va_start(arguments, format);
		(void)vsnprintf(error, capacity, format, arguments);
		va_end(arguments);
	}
	return 0;
}

static char *trim(char *text)
{
	char *end;

	while (*text == ' ' || *text == '\t') {
		++text;
	}
	end = text + strlen(text);
	while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
		--end;
	}
	*end = '\0';
	return text;
}

static int parse_u16(const char *value, uint16_t *output)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed > 65535ul) {
		return 0;
	}
	*output = (uint16_t)parsed;
	return 1;
}

static int parse_file(FILE *file, struct ivmp_server_config *config, char *error, size_t error_capacity)
{
	char line[1024];
	unsigned long line_number = 0ul;

	while (fgets(line, sizeof(line), file) != NULL) {
		char *key = trim(line);
		char *separator;
		char *value;

		++line_number;
		if (*key == '\0' || *key == '#' || *key == ';') {
			continue;
		}
		separator = strchr(key, '=');
		if (separator == NULL) {
			return fail(error, error_capacity, "line %lu: expected key=value", line_number);
		}
		*separator = '\0';
		value = trim(separator + 1);
		key = trim(key);
		if (strcmp(key, "bind_host") == 0) {
			copy_bounded(config->bind_host, sizeof(config->bind_host), value);
		} else if (strcmp(key, "bind_port") == 0) {
			if (!parse_u16(value, &config->bind_port)) {
				return fail(error, error_capacity, "line %lu: invalid bind_port", line_number);
			}
		} else if (strcmp(key, "max_players") == 0) {
			if (!parse_u16(value, &config->maximum_players)) {
				return fail(error, error_capacity, "line %lu: invalid max_players", line_number);
			}
		} else {
			return fail(error, error_capacity, "line %lu: unknown key '%.64s'", line_number, key);
		}
	}
	if (ferror(file) != 0) {
		return fail(error, error_capacity, "cannot read configuration: %s", strerror(errno));
	}
	return 1;
}

int ivmp_server_config_load(const char *path, struct ivmp_server_config *config, char *error, size_t error_capacity)
{
	FILE *file;
	int parsed;

	if (path == NULL || config == NULL) {
		return fail(error, error_capacity, "invalid configuration arguments");
	}
	memset(config, 0, sizeof(*config));
	copy_bounded(config->bind_host, sizeof(config->bind_host), "0.0.0.0");
	config->bind_port = IVMP_DEFAULT_PORT;
	config->maximum_players = IVMP_DEFAULT_MAX_PLAYERS;
	file = fopen(path, "rb");
	if (file == NULL) {
		return fail(error, error_capacity, "cannot open configuration: %s", strerror(errno));
	}
	parsed = parse_file(file, config, error, error_capacity);
	(void)fclose(file);
	if (!parsed) {
		return 0;
	}
	if (config->bind_host[0] == '\0') {
		return fail(error, error_capacity, "bind_host must not be empty");
	}
	if (config->bind_port == 0u) {
		return fail(error, error_capacity, "bind_port must be between 1 and 65535");
	}
	if (config->maximum_players == 0u || config->maximum_players > IVMP_MAX_PLAYERS) {
		return fail(error, error_capacity, "max_players must be between 1 and %u", IVMP_MAX_PLAYERS);
	}
	return 1;
}

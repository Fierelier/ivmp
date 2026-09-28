#ifndef IVMP_ENDIAN_H
#define IVMP_ENDIAN_H

#include <stdint.h>

uint16_t ivmp_read_u16_be(const unsigned char *data);
uint32_t ivmp_read_u32_be(const unsigned char *data);
uint64_t ivmp_read_u64_be(const unsigned char *data);
void ivmp_write_u16_be(unsigned char *data, uint16_t value);
void ivmp_write_u32_be(unsigned char *data, uint32_t value);
void ivmp_write_u64_be(unsigned char *data, uint64_t value);

#endif

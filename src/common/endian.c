#include "endian.h"

uint16_t ivmp_read_u16_be(const unsigned char *data)
{
	return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

uint32_t ivmp_read_u32_be(const unsigned char *data)
{
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

uint64_t ivmp_read_u64_be(const unsigned char *data)
{
	return ((uint64_t)ivmp_read_u32_be(data) << 32) | (uint64_t)ivmp_read_u32_be(data + 4);
}

void ivmp_write_u16_be(unsigned char *data, uint16_t value)
{
	data[0] = (unsigned char)(value >> 8);
	data[1] = (unsigned char)value;
}

void ivmp_write_u32_be(unsigned char *data, uint32_t value)
{
	data[0] = (unsigned char)(value >> 24);
	data[1] = (unsigned char)(value >> 16);
	data[2] = (unsigned char)(value >> 8);
	data[3] = (unsigned char)value;
}

void ivmp_write_u64_be(unsigned char *data, uint64_t value)
{
	ivmp_write_u32_be(data, (uint32_t)(value >> 32));
	ivmp_write_u32_be(data + 4, (uint32_t)value);
}

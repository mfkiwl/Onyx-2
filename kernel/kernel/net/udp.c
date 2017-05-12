/*
* Copyright (c) 2016, 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <kernel/ip.h>
#include <kernel/udp.h>

int send_udp_packet(char *payload, size_t payload_size, int source_port, int dest_port, uint32_t srcip, uint32_t destip)
{
	udp_header_t *udp_header = malloc(sizeof(udp_header_t) + payload_size);
	if(!udp_header)
		return errno = ENOMEM, 1;
	memset(udp_header, 0, sizeof(udp_header_t) + payload_size);
	udp_header->source_port = LITTLE_TO_BIG16(source_port);
	udp_header->dest_port = LITTLE_TO_BIG16(dest_port);
	udp_header->len = LITTLE_TO_BIG16((uint16_t)(sizeof(udp_header_t) + payload_size));
	memcpy(&udp_header->payload, payload, payload_size);

	// TODO: Doesn't work yet, investigate. udp_header->checksum = udpsum(udp_header);
	int ret = send_ipv4_packet(srcip, destip, IPV4_UDP, (char*) udp_header, sizeof(udp_header_t) + payload_size);
	free(udp_header);
	return ret;
}
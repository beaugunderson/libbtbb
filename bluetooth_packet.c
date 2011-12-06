/* -*- c -*- */
/*
 * Copyright 2007 - 2010 Dominic Spill, Michael Ossmann                                                                                            
 * Copyright 2005, 2006 Free Software Foundation, Inc.
 * 
 * This file is part of libbtbb
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with libbtbb; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <inttypes.h>
#include <string.h>

#include "bluetooth_packet.h"

void remove_colons(char *input) {
	char *src, *dest;

	src = dest = input;

	while (*src != '\0')
	{
		if (*src != ':')
		{
			*dest = *src;

			dest++;
		}

		src++;
	}

	*dest = '\0';
}

/*
 * Search a symbol stream to find a packet with arbitrary LAP, return index.
 * The length of the stream must be at least search_length + 72.
 */
int sniff_ac(char *stream, int search_length)
{
	/* Find any/all LAPs (LAP == 0xffffffff) */
	return find_ac(stream, search_length, 0xffffffff);
}

/*
 * Search for known LAP and return the index.  The length of the stream must be
 * at least search_length + 72.
 */
int find_ac(char *stream, int search_length, uint32_t LAP)
{
	/* Looks for an AC in the stream */
	int count;
	uint8_t barker; // barker code at end of sync word (includes MSB of LAP)
	int max_distance = 1; // maximum number of bit errors to tolerate in barker
	uint32_t data_LAP;
	uint64_t syncword;
	char *symbols;

	if (LAP != 0xffffffff) {
		syncword = gen_syncword(LAP);

		//printf("LAP = %06x\n", LAP);
		//printf("syncword = %016" PRIx64 "\n", syncword);
	}

	barker = air_to_host8(&stream[61], 6);

	// The stream length must be 72 symbols longer than search_length.
	for (count = 0; count < search_length; count++)
	{
		symbols = &stream[count];
		barker |= (symbols[67] << 6);

		if (BARKER_DISTANCE[barker] <= max_distance)
		{
			data_LAP = air_to_host32(&symbols[38], 24);

			if (LAP == 0xffffffff) {
				syncword = gen_syncword(data_LAP);

				//printf("data_LAP = %06x\n", data_LAP);
				//printf("syncword = %016" PRIx64 "\n", syncword);
			}

			if (check_syncword(&symbols[4], syncword)) {
				return count;
			}
		}

		barker >>= 1;
	}

	return -1;
}

void init_packet(packet *p, char *syms, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		p->symbols[i] = syms[i]; 
	}

	p->LAP = air_to_host32(&p->symbols[38], 24);

	p->length = len;
	p->whitened = 1;
	p->have_UAP = 0;
	p->have_NAP = 0;
	p->have_clk6 = 0;
	p->have_clk27 = 0;
	p->have_payload = 0;
	p->payload_length = 0;

	uint64_t bd_addr = BD_ADDR_from_env(p->LAP);
	
	if (bd_addr != -1) {
		p->UAP = UAP_from_BD_ADDR(bd_addr);
		p->have_UAP = 1;

		p->NAP = NAP_from_BD_ADDR(bd_addr);
		p->have_NAP = 1;
	}
}

/* Reverse the bits in a byte */
uint8_t reverse(char byte)
{
	return (byte & 0x80) >> 7 |
	 	(byte & 0x40) >> 5 |
		(byte & 0x20) >> 3 |
		(byte & 0x10) >> 1 |
		(byte & 0x08) << 1 |
		(byte & 0x04) << 3 |
		(byte & 0x02) << 5 |
		(byte & 0x01) << 7;
}

/* Generate Sync Word from an LAP */
uint64_t gen_syncword(int LAP)
{
	int i;
	/* default codeword modified for PN sequence and barker code */
	uint64_t codeword = 0xb0000002c7820e7eULL;
	
	/* the sync word generated is in host order, not air order */
	for (i = 0; i < 24; i++) {
		if (LAP & (0x800000 >> i)) {
			codeword ^= sw_matrix[i];
		}
	}

	return codeword;
}

/* Decode 1/3 rate FEC, three like symbols in a row */
int unfec13(char *input, char *output, int length)
{
	int a, b, c, i;
	int be = 0; /* bit errors */

	for (i = 0; i < length; i++) {
		a = 3 * i;
		b = a + 1;
		c = a + 2;

		output[i] = ((input[a] & input[b]) | (input[b] & input[c]) |
				(input[c] & input[a]));

		be += ((input[a] ^ input[b]) | (input[b] ^ input[c]) |
				(input[c] ^ input[a]));
	}

	return (be < (length / 4));
}

/* encode 10 bits with 2/3 rate FEC code, a (15,10) shortened Hamming code */
uint16_t fec23(uint16_t data)
{
	int i;
	uint16_t codeword = 0;

	/* host order, not air order */
	for (i = 0; i < 10; i++) {
		if (data & (1 << i)) {
			codeword ^= fec23_gen_matrix[i];
		}
	}

	return codeword;
}

/* Decode 2/3 rate FEC, a (15,10) shortened Hamming code */
char *unfec23(char *input, int length)
{
	/* input points to the input data
	 * length is length in bits of the data
	 * before it was encoded with fec2/3 */
	int iptr, optr, count;
	char* output;
	uint8_t diff, check;
	uint16_t data, codeword;

	diff = length % 10;
	// padding at end of data
	if (0!=diff)
		length += (10 - diff);

	output = (char *) malloc(length);

	for (iptr = 0, optr = 0; optr<length; iptr += 15, optr += 10) {
		// copy data to output
		for (count=0;count<10;count++) {
			output[optr+count] = input[iptr+count];
		}

		// grab data and error check in host format
		data = air_to_host16(input+iptr, 10);
		check = air_to_host8(input+iptr+10, 5);

		// call fec23 on data to generate the codeword
		codeword = fec23(data);
		diff = check ^ (codeword >> 10);

		/* no errors or single bit errors (errors in the parity bit):
		 * (a strong hint it's a real packet)
		 * Otherwise we need to corret the output*/
		if (diff & (diff - 1)) {
			switch (diff) {
			/* comments are the bit that's wrong and the value
			* of diff in air order, from the BT spec */
				// 1000000000 11010
				case 0x0b: output[optr] ^= 1; break;
				// 0100000000 01101
				case 0x16: output[optr+1] ^= 1; break;
				// 0010000000 11100
				case 0x07: output[optr+2] ^= 1; break;
				// 0001000000 01110
				case 0x0e: output[optr+3] ^= 1; break;
				// 0000100000 00111
				case 0x1c: output[optr+4] ^= 1; break;
				// 0000010000 11001
				case 0x13: output[optr+5] ^= 1; break;
				// 0000001000 10110
				case 0x0d: output[optr+6] ^= 1; break;
				// 0000000100 01011
				case 0x1a: output[optr+7] ^= 1; break;
				// 0000000010 11111
				case 0x1f: output[optr+8] ^= 1; break;
				// 0000000001 10101
				case 0x15: output[optr+9] ^= 1; break;
				/* not one of these errors, probably multiple bit errors
				* or maybe not a real packet, safe to drop it? */
				default: free(output); return 0;
			}
		}
	}

	return output;
}

/* Compare stream with sync word */
int check_syncword(char *stream, uint64_t syncword)
{
	int biterrors;
	uint64_t streamword; /* candidate sync word extracted from rx symbols */
	uint64_t barker;     /* corrected barker code */

	streamword = air_to_host64(stream, 64);

	/* correct the barker code with a simple comparison */
	barker = barker_correct[(uint8_t)(streamword >> 57)];
	streamword = (streamword & 0x01ffffffffffffffULL) | barker;

	//FIXME do error correction instead of detection
	biterrors = count_bits(streamword ^ syncword);

	if (biterrors >= 5) {
		return 0;
	}

	//if (biterrors)
		//printf("POSSIBLE PACKET, LAP = %06x with %d errors\n", LAP, biterrors);

	return 1;
}

/* Convert some number of bits of an air order array to a host order integer */
uint8_t air_to_host8(char *air_order, int bits)
{
	int i;
	uint8_t host_order = 0;

	for (i = 0; i < bits; i++) {
		host_order |= ((uint8_t)air_order[i] << i);
	}

	return host_order;
}

uint16_t air_to_host16(char *air_order, int bits)
{
	int i;
	uint16_t host_order = 0;

	for (i = 0; i < bits; i++) {
		host_order |= ((uint16_t)air_order[i] << i);
	}

	return host_order;
}

uint32_t air_to_host32(char *air_order, int bits)
{
	int i;
	uint32_t host_order = 0;

	for (i = 0; i < bits; i++) {
		host_order |= ((uint32_t)air_order[i] << i);
	}

	return host_order;
}

uint64_t air_to_host64(char *air_order, int bits)
{
	int i;
	uint64_t host_order = 0;

	for (i = 0; i < bits; i++) {
		host_order |= ((uint64_t)air_order[i] << i);
	}

	return host_order;
}

/* Convert some number of bits in a host order integer to an air order array */
void host_to_air(uint8_t host_order, char *air_order, int bits)
{
	int i;

	for (i = 0; i < bits; i++) {
		air_order[i] = (host_order >> i) & 0x01;
	}
}

/* Remove the whitening from an air order array */
void unwhiten(char* input, char* output, int clock, int length, int skip, packet* p)
{
	int count, index;

	index = INDICES[clock & 0x3f];
	index += skip;
	index %= 127;

	for (count = 0; count < length; count++)
	{
		/* unwhiten if whitened, otherwise just copy input to output */
		output[count] = (p->whitened) ? input[count] ^ WHITENING_DATA[index] : input[count];
		index += 1;
		index %= 127;
	}
}

/* Pointer to start of packet, length of packet in bits, UAP */
uint16_t crcgen(char *payload, int length, int UAP)
{
	char byte;
	uint16_t reg, count;

	reg = (reverse(UAP) << 8) & 0xff00;

	for (count = 0; count < length; count++)
	{
		byte = payload[count];

		reg = (reg >> 1) | (((reg & 0x0001) ^ (byte & 0x01))<<15);

		/*Bit 5*/
		reg ^= ((reg & 0x8000)>>5);

		/*Bit 12*/
		reg ^= ((reg & 0x8000)>>12);
	}

	return reg;
}

int validate_BD_ADDR(uint64_t bd_addr) {
	if (bd_addr & 0xffff000000000000ULL) {
		printf("Invalid BD_ADDR: %016" PRIx64 "\n", bd_addr);

		return -1;
	}

	return 0;
}

uint16_t NAP_from_BD_ADDR(uint64_t bd_addr) {
	validate_BD_ADDR(bd_addr);

	return (bd_addr & 0xffff00000000ULL) >> 32;
}

uint16_t UAP_from_BD_ADDR(uint64_t bd_addr) {		
	validate_BD_ADDR(bd_addr);

	return (bd_addr & 0x0000ff000000ULL) >> 24;
}

uint32_t LAP_from_BD_ADDR(uint64_t bd_addr) {		
	validate_BD_ADDR(bd_addr);

	return bd_addr & 0x000000ffffffULL;
}

int BD_ADDR_from_env(uint32_t lap) {
	char *bd_addrs = getenv("BBTB_BD_ADDRS");

	if (bd_addrs == NULL) {
		return -1;
	}

	//printf("UAP_MAPPINGS: %s\n", bd_addrs);

	int length = strlen(bd_addrs);

	char bd_addrs_copy[length + 1];

	strcpy(bd_addrs_copy, bd_addrs);

	char *bd_addr = strtok(bd_addrs_copy, ",");

	while (bd_addr) {
		//printf("\tMapping: %s\n", bd_addr);

		remove_colons(bd_addr);

		//printf("\tRemoved: %s\n", bd_addr);

		uint64_t bd_addr = strtoll(bd_addr, NULL, 16);

		//printf("\tLong: %012" PRIx64 "\n", a);

		uint16_t tnap = NAP_from_BD_ADDR(bd_addr);
		uint16_t tuap = UAP_from_BD_ADDR(bd_addr);
		uint32_t tlap = LAP_from_BD_ADDR(bd_addr); 

		//printf("\tSplit: %04x %02x %06x\n", tnap, tuap, tlap);
		//printf("\tMapping: %06x == %012" PRIx64 "\n", lap, a);

		if (lap == tlap) {
			//printf("Found bd_addr: %06x == %012" PRIx64 "\n", lap, a);

			return tuap;
		}

		bd_addr = strtok(NULL, ",");
	}

	return -1;
}

/* extract UAP by reversing the HEC computation */
int UAP_from_hec(uint16_t data, uint8_t hec)
{
        int i;

        for (i = 9; i >= 0; i--) {
                /* 0x65 is xor'd if MSB is 1, else 0x00 (which does nothing) */
                if (hec & 0x80) {
                        hec ^= 0x65;
		}

                hec = (hec << 1) | (((hec >> 7) ^ (data >> i)) & 0x01);
        }

        return reverse(hec);
}

/* check if the packet's CRC is correct for a given clock (CLK1-6) */
int crc_check(int clock, packet* p)
{
	/*
	 * return value of 1 represents inconclusive result (default)
	 * return value > 1 represents positive result (e.g. CRC match)
	 * return value of 0 represents negative result (e.g. CRC failure without
	 * the possibility that we have assumed the wrong logical transport)
	 */
	int retval = 1;

	switch (p->packet_type)
	{
		case 2:/* FHS */
			retval = fhs(clock, p);
			break;

		case 8:/* DV */
		case 3:/* DM1 */
		case 10:/* DM3 */
		case 14:/* DM5 */
			retval = DM(clock, p);
			break;

		case 4:/* DH1 */
		case 11:/* DH3 */
		case 15:/* DH5 */
			retval = DH(clock, p);
			break;

		case 7:/* EV3 */
			retval = EV3(clock, p);
			break;
		case 12:/* EV4 */
			retval = EV4(clock, p);
			break;
		case 13:/* EV5 */
			retval = EV5(clock, p);
			break;
		
		case 5:/* HV1 */
			retval = HV(clock, p);
			break;

		/* some types can't help us */
		default:
			break;
	}
	/*
	 * never return a zero result unless this is a FHS, DM1, or HV1.  any
	 * other type could have actually been something else (another logical
	 * transport)
	 */
	if (retval == 0 && (p->packet_type != 2 && p->packet_type != 3 &&
			p->packet_type != 5))
		return 1;

	/* EV3 and EV5 have a relatively high false positive rate */
	if (retval > 1 && (p->packet_type == 7 || p->packet_type == 13))
		return 1;

	return retval;
}

/* verify the payload CRC */
int payload_crc(packet* p)
{
	uint16_t crc;   /* CRC calculated from payload data */
	uint16_t check; /* CRC supplied by packet */

	crc = crcgen(p->payload, (p->payload_length - 2) * 8, p->UAP);
	check = air_to_host16(&p->payload[(p->payload_length - 2) * 8], 16);

	return (crc == check);
}

int fhs(int clock, packet* p)
{
	/* skip the access code and packet header */
	char *stream = p->symbols + 126;
	/* number of symbols remaining after access code and packet header */
	int size = p->length - 126;

	p->payload_length = 20;

	if (size < p->payload_length * 12) {
		return 1; //FIXME should throw exception
	}

	char *corrected = unfec23(stream, p->payload_length * 8);

	if (!corrected) {
		return 0;
	}

	/* try to unwhiten with known clock bits */
	unwhiten(corrected, p->payload, clock, p->payload_length * 8, 18, p);

	if (payload_crc(p)) {
		free(corrected);

		return 1000;
	}

	/* try all 32 possible X-input values instead */
	for (clock = 32; clock < 64; clock++) {
		unwhiten(corrected, p->payload, clock, p->payload_length * 8, 18, p);

		if (payload_crc(p)) {
			free(corrected);

			return 1000;
		}
	}

	/* failed to unwhiten */
	free(corrected);

	return 0;
}

/* decode payload header, return value indicates success */
int decode_payload_header(char *stream, int clock, int header_bytes, int size, int fec, packet* p)
{
	if (header_bytes == 2)
	{
		if (size < 16) {
			return 0; //FIXME should throw exception
		}

		if (fec) {
			if (size < 30) {
				return 0; //FIXME should throw exception
			}

			char *corrected = unfec23(stream, 16);

			if (!corrected) {
				return 0;
			}

			unwhiten(corrected, p->payload_header, clock, 16, 18, p);

			free(corrected);
		} else {
			unwhiten(stream, p->payload_header, clock, 16, 18, p);
		}

		/* payload length is payload body length + 2 bytes payload header + 2 bytes CRC */
		p->payload_length = air_to_host16(&p->payload_header[3], 10) + 4;
	} else {
		if (size < 8) {
			return 0; //FIXME should throw exception
		}

		if (fec) {
			if (size < 15) {
				return 0; //FIXME should throw exception
			}

			char *corrected = unfec23(stream, 8);

			if (!corrected) {
				return 0;
			}

			unwhiten(corrected, p->payload_header, clock, 8, 18, p);

			free(corrected);
		} else {
			unwhiten(stream, p->payload_header, clock, 8, 18, p);
		}

		/* payload length is payload body length + 1 byte payload header + 2 bytes CRC */
		p->payload_length = air_to_host8(&p->payload_header[3], 5) + 3;
	}

	p->payload_llid = air_to_host8(&p->payload_header[0], 2);
	p->payload_flow = air_to_host8(&p->payload_header[2], 1);
	p->payload_header_length = header_bytes;

	return 1;
}

/* DM 1/3/5 packet (and DV)*/
int DM(int clock, packet* p)
{
	int bitlength;
	/* number of bytes in the payload header */
	int header_bytes = 2;
	/* maximum payload length */
	int max_length;
	/* skip the access code and packet header */
	char *stream = p->symbols + 126;
	/* number of symbols remaining after access code and packet header */
	int size = p->length - 126;

	switch (p->packet_type)
	{
		case(8): /* DV */
			/* skip 80 voice bits, then treat the rest like a DM1 */
			stream += 80;
			size -= 80;
			header_bytes = 1;
			/* I don't think the length of the voice field ("synchronous data
			 * field") is included in the length indicated by the payload
			 * header in the data field ("asynchronous data field"), but I
			 * could be wrong.
			 */
			max_length = 12;
			break;
		case(3): /* DM1 */
			header_bytes = 1;
			max_length = 20;
			break;
		case(10): /* DM3 */
			max_length = 125;
			break;
		case(14): /* DM5 */
			max_length = 228;
			break;
		default: /* not a DM1/3/5 or DV */
			return 0;
	}

	if (!decode_payload_header(stream, clock, header_bytes, size, 1, p)) {
		return 0;
	}

	/* check that the length indicated in the payload header is within spec */
	if (p->payload_length > max_length) {
		/* could be encrypted */
		return 1;
	}

	bitlength = p->payload_length*8;

	if (bitlength > size) {
		return 1; //FIXME should throw exception
	}

	char *corrected = unfec23(stream, bitlength);

	if (!corrected) {
		return 0;
	}

	unwhiten(corrected, p->payload, clock, bitlength, 18, p);

	free(corrected);

	if (payload_crc(p)) {
		return 10;
	}

	/* could be encrypted */
	return 1;
}

/* DH 1/3/5 packet (and AUX1) */
/* similar to DM 1/3/5 but without FEC */
int DH(int clock, packet* p)
{
	int bitlength;
	/* number of bytes in the payload header */
	int header_bytes = 2;
	/* maximum payload length */
	int max_length;
	/* skip the access code and packet header */
	char *stream = p->symbols + 126;
	/* number of symbols remaining after access code and packet header */
	int size = p->length - 126;
	
	switch (p->packet_type)
	{
		case(9): /* AUX1 */
		case(4): /* DH1 */
			header_bytes = 1;
			max_length = 30;
			break;
		case(11): /* DH3 */
			max_length = 187;
			break;
		case(15): /* DH5 */
			max_length = 343;
			break;
		default: /* not a DH1/3/5 */
			return 0;
	}

	if (!decode_payload_header(stream, clock, header_bytes, size, 0, p))
		return 0;

	/* check that the length indicated in the payload header is within spec */
	if (p->payload_length > max_length)
		/* could be encrypted */
		return 1;

	bitlength = p->payload_length*8;

	if (bitlength > size)
		return 1; //FIXME should throw exception

	unwhiten(stream, p->payload, clock, bitlength, 18, p);
	
	/* AUX1 has no CRC */
	if (p->packet_type == 9)
		return 1;

	if (payload_crc(p))
		return 10;

	/* could be encrypted */
	return 1;
}

int EV3(int clock, packet* p)
{
	/* skip the access code and packet header */
	char *stream = p->symbols + 126;

	/* number of symbols remaining after access code and packet header */
	int size = p->length - 126;

	/* maximum payload length is 30 bytes + 2 bytes CRC */
	int maxlength = 32;

	/* number of bits we have decoded */
	int bits;

	/* check CRC for any integer byte length up to maxlength */
	for (p->payload_length = 0;
			p->payload_length < maxlength; p->payload_length++) {

		bits = p->payload_length * 8;

		/* unwhiten next byte */
		if ((bits + 8) > size)
			return 1; //FIXME should throw exception
		unwhiten(stream, p->payload + bits, clock, 8, 18 + bits, p);

		if ((p->payload_length > 2) && (payload_crc(p)))
				return 10;
	}
	return 1;
}

int EV4(int clock, packet* p)
{
	char *corrected;

	/* skip the access code and packet header */
	char *stream = p->symbols + 126;

	/* number of symbols remaining after access code and packet header */
	int size = p->length - 126;

	/*
	 * maximum payload length is 120 bytes + 2 bytes CRC
	 * after FEC2/3, this results in a maximum of 1470 symbols
	 */
	int maxlength = 1470;

	/*
	 * minumum payload length is 1 bytes + 2 bytes CRC
	 * after FEC2/3, this results in a minimum of 45 symbols
	 */
	int minlength = 45;

	int syms = 0; /* number of symbols we have decoded */
	int bits = 0; /* number of payload bits we have decoded */

	p->payload_length = 1;

	while (syms < maxlength) {

		/* unfec/unwhiten next block (15 symbols -> 10 bits) */
		if (syms + 15 > size)
			return 1; //FIXME should throw exception

		corrected = unfec23(stream + syms, 10);

		if (!corrected) {
			free(corrected);

			if (syms < minlength)
				return 0;
			else
				return 1;
		}

		unwhiten(corrected, p->payload + bits, clock, 10, 18 + bits, p);

		free(corrected);

		/* check CRC one byte at a time */
		while (p->payload_length * 8 <= bits) {
			if (payload_crc(p))
				return 10;

			p->payload_length++;
		}

		syms += 15;
		bits += 10;
	}

	return 1;
}

int EV5(int clock, packet* p)
{
	/* skip the access code and packet header */
	char *stream = p->symbols + 126;

	/* number of symbols remaining after access code and packet header */
	int size = p->length - 126;

	/* maximum payload length is 180 bytes + 2 bytes CRC */
	int maxlength = 182;

	/* number of bits we have decoded */
	int bits;

	/* check CRC for any integer byte length up to maxlength */
	for (p->payload_length = 0;
			p->payload_length < maxlength; p->payload_length++) {

		bits = p->payload_length * 8;

		/* unwhiten next byte */
		if ((bits + 8) > size)
			return 1; //FIXME should throw exception
		unwhiten(stream, p->payload + bits, clock, 8, 18 + bits, p);

		if ((p->payload_length > 2) && (payload_crc(p)))
				return 10;
	}
	return 1;
}

/* HV packet type payload parser */
int HV(int clock, packet* p)
{
	/* skip the access code and packet header */
	char *stream = p->symbols + 126;
	/* number of symbols remaining after access code and packet header */
	int size = p->length - 126;

	if (size < 240) {
		p->payload_length = 0;

		return 1; //FIXME should throw exception
	}

	switch (p->packet_type) {
		case 5:/* HV1 */
			{
				char corrected[80];

				if (!unfec13(stream, corrected, 80)) {
					return 0;
				}

				p->payload_length = 10;

				unwhiten(corrected, p->payload, clock, p->payload_length*8, 18, p);
			}
	
			break;
		case 6:/* HV2 */
			{
				char *corrected = unfec23(stream, 160);

				if (!corrected) {
					return 0;
				}

				p->payload_length = 20;

				unwhiten(corrected, p->payload, clock, p->payload_length*8, 18, p);

				free(corrected);
			}

			break;
		case 7:/* HV3 */
			p->payload_length = 30;

			unwhiten(stream, p->payload, clock, p->payload_length*8, 18, p);

			break;
	}

	return 1;
}

/* try a clock value (CLK1-6) to unwhiten packet header,
 * sets resultant p->packet_type and p->UAP, returns UAP. */
uint8_t try_clock(int clock, packet* p)
{
	/* skip 72 bit access code */
	char *stream = p->symbols + 72;
	/* 18 bit packet header */
	char header[18];
	char unwhitened[18];

	if (!unfec13(stream, header, 18)) {
		return 0;
	}

	unwhiten(header, unwhitened, clock, 18, 0, p);

	uint16_t hdr_data = air_to_host16(unwhitened, 10);
	uint8_t hec = air_to_host8(&unwhitened[10], 8);

	p->UAP = UAP_from_hec(hdr_data, hec);
	p->packet_type = air_to_host8(&unwhitened[3], 4);

	return p->UAP;
}

/* decode the packet header */
int decode_header(packet* p)
{
	/* skip 72 bit access code */
	char *stream = p->symbols + 72;
	/* 18 bit packet header */
	char header[18];
	uint8_t UAP;

	if (p->have_clk6 && unfec13(stream, header, 18)) {
		unwhiten(header, p->packet_header, p->clock, 18, 0, p);

		uint16_t hdr_data = air_to_host16(p->packet_header, 10);
		uint8_t hec = air_to_host8(&p->packet_header[10], 8);

		UAP = UAP_from_hec(hdr_data, hec);

		if (UAP == p->UAP) {
			p->packet_type = air_to_host8(&p->packet_header[3], 4);

			return 1;
		} else {
			printf("Bad HEC: UAP = %02x, p->UAP = %02x\n", UAP, p->UAP);
		}
	}
	
	printf("Failed to decode header.\n");

	return 0;
}

void decode_payload(packet* p)
{
	p->payload_header_length = 0;

	switch (p->packet_type)
	{
		case 0: /* NULL */
			/* no payload to decode */
			p->payload_length = 0;
			break;
		case 1: /* POLL */
			/* no payload to decode */
			p->payload_length = 0;
			break;
		case 2: /* FHS */
			fhs(p->clock, p);
			break;
		case 3: /* DM1 */
			DM(p->clock, p);
			break;
		case 4: /* DH1 */
			/* assuming DH1 but could be 2-DH1 */
			DH(p->clock, p);
			break;
		case 5: /* HV1 */
			HV(p->clock, p);
			break;
		case 6: /* HV2 */
			HV(p->clock, p);
			break;
		case 7: /* HV3/EV3/3-EV3 */
			/* decode as EV3 if CRC checks out */
			if (EV3(p->clock, p) <= 1) {
				/* otherwise assume HV3 */
				HV(p->clock, p);
			}
			/* don't know how to decode 3-EV3 */
			break;
		case 8: /* DV */
			/* assuming DV but could be 3-DH1 */
			DM(p->clock, p);
			break;
		case 9: /* AUX1 */
			DH(p->clock, p);
			break;
		case 10: /* DM3 */
			/* assuming DM3 but could be 2-DH3 */
			DM(p->clock, p);
			break;
		case 11: /* DH3 */
			/* assuming DH3 but could be 3-DH3 */
			DH(p->clock, p);
			break;
		case 12: /* EV4 */
			/* assuming EV4 but could be 2-EV5 */
			EV4(p->clock, p);
			break;
		case 13: /* EV5 */
			/* assuming EV5 but could be 3-EV5 */
			EV5(p->clock, p);
		case 14: /* DM5 */
			/* assuming DM5 but could be 2-DH5 */
			DM(p->clock, p);
			break;
		case 15: /* DH5 */
			/* assuming DH5 but could be 3-DH5 */
			DH(p->clock, p);
			break;
	}

	p->have_payload = 1;
}

/* decode the whole packet */
void decode(packet* p)
{
	p->have_payload = 0;

	if (decode_header(p)) {
		decode_payload(p);
	}
}

/* print packet information */
void print(packet* p)
{
	if (p->have_payload) {
		printf("%s\n", TYPE_NAMES[p->packet_type]);

		if (p->payload_header_length > 0) {
			printf("\tLLID: %d\n", p->payload_llid);
			printf("\tFlow: %d\n", p->payload_flow);
			printf("\tPayload length: %d\n", p->payload_length);
		}
	}
}

char *tun_format(packet* p)
{
	/* include 6 bytes for meta data, 3 bytes for packet header */
	int length = 9 + p->payload_length;
	char *tun_format = (char *) malloc(length);
	int i;

	/* meta data */
	tun_format[0] = p->clock & 0xff;
	tun_format[1] = (p->clock >> 8) & 0xff;
	tun_format[2] = (p->clock >> 16) & 0xff;
	tun_format[3] = (p->clock >> 24) & 0xff;
	tun_format[4] = p->channel;
	tun_format[5] = p->have_clk27 | (p->have_NAP << 1);

	/* packet header modified to fit byte boundaries */
	/* lt_addr and type */
	tun_format[6] = (char) air_to_host8(&p->packet_header[0], 7);
	/* flags */
	tun_format[7] = (char) air_to_host8(&p->packet_header[7], 3);
	/* HEC */
	tun_format[8] = (char) air_to_host8(&p->packet_header[10], 8);

	for (i=0;i<p->payload_length;i++) {
		tun_format[i+9] = (char) air_to_host8(&p->payload[i*8], 8);
	}

	return tun_format;
}

int got_payload(packet* p)
{
	return p->have_payload;
}

int get_payload_length(packet* p)
{
	return p->payload_length;
}

int get_type(packet* p)
{
	return p->packet_type;
}

/* check to see if the packet has a header */
int header_present(packet* p)
{
	/* skip to last bit of sync word */
	char *stream = p->symbols + 67;
	int be = 0; /* bit errors */
	char msb;   /* most significant (last) bit of sync word */
	int a, b, c;

	/* check that we have enough symbols */
	if (p->length < 126) {
		return 0;
	}

	/* check that the AC trailer is correct */
	msb = stream[0];

	be += stream[1] ^ !msb;
	be += stream[2] ^ msb;
	be += stream[3] ^ !msb;
	be += stream[4] ^ msb;

	/*
	 * Each bit of the 18 bit header is repeated three times.  Without
	 * checking the correctness of any particular bit, just count the
	 * number of times three symbols in a row don't all agree.
	 */
	stream += 5;

	for (a = 0; a < 54; a += 3) {
		b = a + 1;
		c = a + 2;

		be += ((stream[a] ^ stream[b]) |
			(stream[b] ^ stream[c]) | (stream[c] ^ stream[a]));
	}

	/*
	 * Few bit errors indicates presence of a header.  Many bit errors
	 * indicates no header is present (i.e. it is an ID packet).
	 */
	return (be < ID_THRESHOLD);
}

/* extract LAP from FHS payload */
uint32_t lap_from_fhs(packet* p)
{
	/* caller should check got_payload() and get_type() */
	return air_to_host32(&p->payload[34], 24);
}

/* extract UAP from FHS payload */
uint8_t uap_from_fhs(packet* p)
{
	/* caller should check got_payload() and get_type() */
	return air_to_host8(&p->payload[64], 8);
}

/* extract NAP from FHS payload */
uint16_t nap_from_fhs(packet* p)
{
	/* caller should check got_payload() and get_type() */
	return air_to_host8(&p->payload[72], 16);
}

/* extract clock from FHS payload */
uint32_t clock_from_fhs(packet* p)
{
	/*
	 * caller should check got_payload() and get_type()
	 *
	 * This is CLK2-27 (units of 1.25 ms).
	 * CLK0 and CLK1 are implicitly zero.
	 */
	return air_to_host32(&p->payload[115], 26);
}

/* count the number of 1 bits in a uint64_t */
int count_bits(uint64_t n)
{
	int i;

	for (i = 0; n != 0; i++) {
		n &= n - 1;
	}

	return i;
}

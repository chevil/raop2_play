/*****************************************************************************
 * audio_stream.h: audio file stream, header file
 *
 * Copyright (C) 2005 Shiro Ninomiya <shiron@snino.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/
#ifndef __AUDIO_STREAM_H_
#define __AUDIO_STREAM_H_

#include <signal.h>

typedef enum data_type_t {
	AUD_TYPE_NONE = 0,
	AUD_TYPE_WAV,
} data_type_t;

typedef struct auds_t {
/* public variables */
	void *stream;
	int sample_rate;
	void (*sigchld_cb)(void *, siginfo_t *);
	int chunk_size;
	data_type_t data_type;
	int channels;
	struct auds_t *auds;
	__u32 dummy;
} auds_t;


typedef enum data_source_type_t {
	DESCRIPTOR=0,
	STREAM,
	MEMORY,
}data_source_type_t;
typedef struct mem_source_t {
	int size;
	__s16 *data;
}mem_source_t;
typedef struct data_source_t {
	data_source_type_t type;
	FILE *inf;
	FILE *inf2;
}data_source_t;

#define DEFAULT_SAMPLE_RATE 44100
#define MAX_SAMPLES_IN_CHUNK 352

/*
 * if I send very small chunk of data, AEX is going to disconnect.
 * To avoid it, apply this size as the minimum size of chunk.
 */
#define MINIMUM_SAMPLE_SIZE 32

auds_t *auds_open(char *fname, data_type_t adt);
int auds_close(auds_t *auds);
int auds_get_top_sample(auds_t *auds, __u8 **data, int *size);
int auds_get_next_sample(auds_t *auds, __u8 **data, int *size);
int auds_get_sample_rate(auds_t *auds);
int auds_write_pcm(auds_t *auds, __u8 *buffer, __u8 **data, int *size,
		   int bsize, data_source_t *ds);
int auds_sigchld(auds_t *auds, siginfo_t *siginfo);
int aud_clac_chunk_size(int sample_rate);


/* write bits filed data, *bpos=0 for msb, *bpos=7 for lsb
   d=data, blen=length of bits field
 */
static inline void bits_write(__u8 **p, __u8 d, int blen, int *bpos)
{
	int lb,rb,bd;
	lb=7-*bpos;
	rb=lb-blen+1;
	if(rb>=0){
		bd=d<<rb;
		if(*bpos)
			**p|=bd;
		else
			**p=bd;
		*bpos+=blen;
	}else{
		bd=d>>-rb;
		**p|=bd;
		*p+=1;
		**p=d<<(8+rb);
		*bpos=-rb;
	}
}

#endif

/*****************************************************************************
 * wav_stream.h: wave file stream, header file
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
#ifndef __WAV_STREAM_H_
#define __WAV_STREAM_H_

typedef struct wave_header_t{
	char charChunkID[4];
	__u32 ChunkSize;
	char Format[4];
	char Subchunk1ID[4];
	__u32 Subchunk1Size;
	__u16 AudioFormat;
	__u16 NumChannels;
	__u32 SampleRate;
	__u32 ByteRate;
	__u16 BlockAlign;
	__u16 BitsPerSample;
	char Subchunk2ID[4];
	__u32 Subchunk2Size;
} __attribute__ ((packed)) wave_header_t;

typedef struct wav_t {
/* public variables */
/* private variables */
#ifdef WAV_STREAM_C
	FILE *inf;
	__u8 *buffer;
	int subchunk2size;
	int playedbytes;
#else
	__u32 dummy;
#endif
} wav_t;

int wav_open(auds_t *auds, char *fname);
int wav_close(auds_t *auds);
int wav_get_top_sample(auds_t *auds, __u8 **data, int *size);
int wav_get_next_sample(auds_t *auds, __u8 **data, int *size);
int wav_get_duration();
int wav_get_position();
int wav_get_length();


#endif

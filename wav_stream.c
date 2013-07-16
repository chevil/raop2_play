/*****************************************************************************
 * wav_stream.c: wave file stream
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
#include <asm/types.h>
#include <sys/stat.h>
#include <stdio.h>
#define WAV_STREAM_C
#include "audio_stream.h"
#include "wav_stream.h"
#include "raop_play.h"
#include "aexcl_lib.h"

extern long startinms;
extern long endinms;

static int read_wave_header(wav_t *wav, int *sample_rate, int *channels);

int fduration=0;
int flength=0;
int fposition=0;

int wav_open(auds_t *auds, char *fname)
{
        struct stat finfo;
	wav_t *wav=malloc(sizeof(wav_t));
	if(!wav) return -1;
	memset(wav,0,sizeof(wav_t));
	auds->stream=(void *)wav;
	wav->inf=fopen(fname,"r");
	if(!wav->inf) goto erexit;
	if(read_wave_header(wav, &auds->sample_rate, &auds->channels)==-1) goto erexit;
	auds->chunk_size=aud_clac_chunk_size(auds->sample_rate);
	wav->buffer=(__u8 *)malloc(MAX_SAMPLES_IN_CHUNK*4+16);
	if(!wav->buffer) goto erexit;

        if(stat(fname,&finfo)<0)
        {
          ERRMSG( "Couldn't get file size\n" );
          fduration=0;
        }
        else
        {
          fduration=(finfo.st_size-sizeof(wave_header_t))/(sizeof(short)*auds->channels*auds->sample_rate);
          flength=(finfo.st_size-sizeof(wave_header_t))/(sizeof(short)*auds->channels);
          DBGMSG( "duration in seconds : %d\n", fduration );
        }

        DBGMSG( "Start time is %ld\n", startinms );
        if ( startinms != -1 )
        {
           long nbbytes = sizeof(short)*auds->channels*auds->sample_rate;
           nbbytes *= startinms/1000;
           DBGMSG( "Seeking to %ld bytes\n", nbbytes );

           if ( fseek( wav->inf, nbbytes, SEEK_CUR ) < 0 )
           {
              ERRMSG( "Couldn't seek specified start time : %s", strerror( errno ) );
              wav_close(auds);
              return -1;
           }
        }
        else
        {
	   fseek(wav->inf,sizeof(wave_header_t),SEEK_SET);
        }

	return 0;
 erexit:
	wav_close(auds);
	return -1;
}

int wav_close(auds_t *auds)
{
	wav_t *wav=(wav_t *)auds->stream;
	if(!wav) return -1;
	if(wav->inf) fclose(wav->inf);
	if(wav->buffer) free(wav->buffer);
	free(wav);
	auds->stream=NULL;
	return 0;
}

int wav_get_top_sample(auds_t *auds, __u8 **data, int *size)
{
	wav_t *wav=(wav_t *)auds->stream;
	wav->playedbytes=0;
	return wav_get_next_sample(auds, data, size);
}

int wav_get_duration()
{
        return fduration;
}

int wav_get_position()
{
        return fposition;
}

int wav_get_length()
{
        return flength;
}

int wav_get_next_sample(auds_t *auds, __u8 **data, int *size)
{
	wav_t *wav=(wav_t *)auds->stream;
	int bsize=MAX_SAMPLES_IN_CHUNK;
	data_source_t ds={.type=STREAM, .u.inf=wav->inf};
	if(!bsize) return -1;
	wav->playedbytes+=bsize;
        int posinbytes=ftell(wav->inf);
        if ( posinbytes < 0 )
        {
           ERRMSG( "Couldn't get file position\n" );
        }
        else
        {
           // in secs
           fposition=(posinbytes-sizeof(wave_header_t))/(sizeof(short)*auds->channels*auds->sample_rate);
        }
        long enbbytes = sizeof(short)*auds->channels*auds->sample_rate;
        if ( endinms != -1 )
        {
          enbbytes *= endinms/1000;
          if ( posinbytes >= enbbytes )
          {
             DBGMSG( "marker end position reached\n" );
             return -1;
          }
        }
	return auds_write_pcm(auds, wav->buffer, data, size, bsize, &ds);
}


static int read_wave_header(wav_t *wav, int *sample_rate, int *channels)
{
	wave_header_t head;
	FILE *infile=wav->inf;
	
	if(fread(&head,1,sizeof(head),infile)<sizeof(head)) return -1;
	if(strncmp(head.charChunkID,"RIFF",4) || strncmp(head.Format,"WAVE",4)){
		ERRMSG("This is not a WAV file\n");
		return -1;
	}
	*channels=head.NumChannels;
	if(head.NumChannels!=2 && head.NumChannels!=1){
		ERRMSG("This is neither stereo nor mono, NumChannels=%d\n",head.NumChannels);
		return -1;
	}
	if(head.BitsPerSample!=16){
		ERRMSG("bits per sample = %d, we need 16 bits data\n", head.BitsPerSample);
		return -1;
	}
	*sample_rate=head.SampleRate;
#if 0	
	if(head.SampleRate!=DEFAULT_SAMPLE_RATE){
		ERRMSG("sample rate = %d, we need %d\n", head.SampleRate,DEFAULT_SAMPLE_RATE);
		return -1;
	}
#endif	
	// if(strncmp(head.Subchunk2ID,"data",4)){
	// 	ERRMSG("sub chunk is not \"data\"\n");
	//      return -1;
	// }
	wav->subchunk2size=head.Subchunk2Size;
	return 0;
}



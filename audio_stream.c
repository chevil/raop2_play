/*****************************************************************************
 * audio_stream.c: audio file stream
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
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#define AUDIO_STREAM_C
#include "audio_stream.h"
#include "wav_stream.h"
#include "aexcl_lib.h"

extern double balance;
extern double csync;

static data_type_t get_data_type(char *fname);

auds_t *auds_open(char *fname, data_type_t adt)
{
    auds_t *auds=malloc(sizeof(auds_t));
    if(!auds) return NULL;
    memset(auds, 0, sizeof(auds_t));
    int rval=-1;

    auds->channels=2; //default is stereo
    if(adt==AUD_TYPE_NONE)
        auds->data_type=get_data_type(fname);
    else
        auds->data_type=adt;
    switch(auds->data_type){
    case AUD_TYPE_WAV:
        rval=wav_open(auds,fname);
        break;
    case AUD_TYPE_NONE:
        ERRMSG("unknown audio data type : %d\n", adt);
        break;
    }
    if(rval) goto erexit;
    return auds;
 erexit:
    ERRMSG("errror: %s\n",__func__);
    auds_close(auds);
    return NULL;
}

int auds_close(auds_t *auds)
{
    if(auds->stream){
        switch(auds->data_type){
        case AUD_TYPE_WAV:
            wav_close(auds);
            break;
        case AUD_TYPE_NONE:
            ERRMSG("### shouldn't come here\n");
            break;
        }
    }
    free(auds);
    return 0;
}

int auds_get_top_sample(auds_t *auds, uint8_t **data, int *size)
{
    switch(auds->data_type){
    case AUD_TYPE_WAV:
        return wav_get_top_sample(auds, data, size);
    case AUD_TYPE_NONE:
        ERRMSG("%s:### shouldn't come here\n",__func__);
        return -1;
    }
    return -1;
}

int auds_get_next_sample(auds_t *auds, uint8_t **data, int *size)
{
    int rval;
    auds_t *lauds=auds;
    if(auds->auds) lauds=auds->auds;
    switch(lauds->data_type){
    case AUD_TYPE_WAV:
        rval=wav_get_next_sample(lauds, data, size);
        break;
    case AUD_TYPE_NONE:
        ERRMSG("%s:### shouldn't come here\n",__func__);
        return -1;
    }
    return rval;
}

int auds_get_sample_rate(auds_t *auds)
{
    if ( !auds ) return DEFAULT_SAMPLE_RATE;
    else
       return auds->sample_rate;
}

int auds_sigchld(auds_t *auds, siginfo_t *siginfo)
{
    if(!auds) return 0;
    if(auds->auds && auds->auds->sigchld_cb){
        auds->auds->sigchld_cb(auds->auds->stream, siginfo);
        return 0;
    }
    if(auds->sigchld_cb){
        auds->sigchld_cb(auds->stream, siginfo);
        return 0;
    }
    return 0;
}


int auds_write_pcm(auds_t *auds, uint8_t *buffer, uint8_t **data, int *size,
           int bsize, data_source_t *ds)
{
    uint8_t one[4];
    uint8_t two[4];
    int count=0;
    int bpos=0;
    uint8_t *bp=buffer;
    int i,nodata=0;
    int channels=2;

    bits_write(&bp,1,3,&bpos); // channel=1, stereo
    bits_write(&bp,0,4,&bpos); // unknown
    bits_write(&bp,0,12,&bpos); // unknown
    // always sets the size
    // if(bsize!=MAX_SAMPLES_IN_CHUNK)
        bits_write(&bp,1,1,&bpos); // hassize
    // else
    //     bits_write(&bp,0,1,&bpos); // hassize
    bits_write(&bp,0,2,&bpos); // unused
    bits_write(&bp,1,1,&bpos); // is-not-compressed

    // if(bsize!=MAX_SAMPLES_IN_CHUNK){
        bits_write(&bp,(bsize>>24)&0xff,8,&bpos); // size of data, integer, big endian
        bits_write(&bp,(bsize>>16)&0xff,8,&bpos);
        bits_write(&bp,(bsize>>8)&0xff,8,&bpos);
        bits_write(&bp,bsize&0xff,8,&bpos);
    // }

    while(1){
        switch(ds->type){
            case STREAM:
                if(channels==1){
                    if(fread(one,1,2,ds->inf)!=2) 
                    {
                       ERRMSG( "fread failed : reason : %s\n", strerror(errno) );
                       nodata=1;
                    }
                    *((int16_t*)one+1)=*((int16_t*)one);
                    memcpy( &two[0], &one[0], 4 );
                }else{
                       if ( csync<0.0 )
                       {
                          if(fread(one,1,4,ds->inf)!=4) 
                          {
                            ERRMSG( "fread failed : reason : %s\n", strerror(errno) );
                            nodata=1;
                          }
                          if(fread(two,1,4,ds->inf2)!=4) 
                          {
                            ERRMSG( "fread failed : reason : %s\n", strerror(errno) );
                            nodata=1;
                          }
                       }
                       if ( csync>0.0 )
                       {
                          if(fread(one,1,4,ds->inf2)!=4) 
                          {
                            ERRMSG( "fread failed : reason : %s\n", strerror(errno) );
                            nodata=1;
                          }
                          if(fread(two,1,4,ds->inf)!=4) 
                          {
                            ERRMSG( "fread failed : reason : %s\n", strerror(errno) );
                            nodata=1;
                          }
                       }
                       if ( csync==0.0 )
                       {
                          if(fread(one,1,4,ds->inf)!=4) 
                          {
                            ERRMSG( "fread failed : reason : %s\n", strerror(errno) );
                            nodata=1;
                          }
                          memcpy( &two[0], &one[0], 4 );
                       }
                }
                break;

           default:
                ERRMSG( "bad audio stream type : %d\n", ds->type );
                // should not happen
                break;
        }
        if(nodata) break;

        // apply balance here
        *((int16_t*)one)=*((int16_t*)one)*((1.0-balance/100.0));
        *((int16_t*)two+1)=*((int16_t*)two+1)*(balance/100.0);

        bits_write(&bp,one[1],8,&bpos);
        bits_write(&bp,one[0],8,&bpos);
        bits_write(&bp,two[3],8,&bpos);
        bits_write(&bp,two[2],8,&bpos);
        if(++count==bsize) break;
    }
    if(!count) 
    {
        ERRMSG( "count is 0, nodata = %d\n", nodata );
        return -1; // when no data at all, it should stop playing
    }
    /* when readable size is less than bsize, fill 0 at the bottom */
    for(i=0;i<(bsize-count)*4;i++){
        bits_write(&bp,0,8,&bpos);
    }
    // frame footer ??
    bits_write(&bp,7,3,&bpos); // should be always 7 ( says wikipedia )

    *size=bp-buffer;
    if(bpos) *size+=1;
    *data=buffer;
    // DBGMSG( "wrote %d frames from %d, size : %d\n", count, bsize, *size );
    // DBGMSG( "ALAC header : %x %x %x\n", buffer[0], buffer[1], buffer[2] );
    return 0;
}

int aud_clac_chunk_size(int sample_rate)
{
    int bsize=MAX_SAMPLES_IN_CHUNK;
    int ratio=DEFAULT_SAMPLE_RATE*100/sample_rate;
    // to make suer the resampled size is <= 4096
    if(ratio>100) bsize=bsize*100/ratio-1;
    return bsize;
}

static data_type_t get_data_type(char *fname)
{
    int i;
    for(i=strlen(fname)-1;i>=0;i--)
        if(fname[i]=='.') break;
    if(i>=strlen(fname)-1) return AUD_TYPE_NONE;
    if(!strcasecmp(fname+i+1,"wav")) return AUD_TYPE_WAV;
    return AUD_TYPE_NONE;
}

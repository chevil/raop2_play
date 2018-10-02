/*****************************************************************************
 * rtsp_client.c: RAOP Client
 *
 * Copyright (C) 2004 Shiro Ninomiya <shiron@snino.com>
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
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/engine.h>

//thread
#include <pthread.h>
#include <semaphore.h>

//time
#include <time.h>
#include <stdlib.h>

#include <limits.h>
#include "aexcl_lib.h"
#include "rtsp_client.h"
#include "raop_client.h"
#include "raop_play.h"
#include "base64.h"
#include "aes.h"
#include "raop_play.h"
#include "audio_stream.h"
#include "wav_stream.h"

#define JACK_STATUS_DISCONNECTED 0
#define JACK_STATUS_CONNECTED 1

#define JACK_TYPE_ANALOG 0
#define JACK_TYPE_DIGITAL 1

#define VOLUME_MIN -30 
#define VOLUME_MAX 0

typedef struct raopcl_data_t {
	rtspcl_t *rtspcl;
	uint8_t iv[16]; // initialization vector for aes-cbc
	uint8_t nv[16]; // next vector for aes-cbc
	uint8_t key[16]; // key for aes-cbc
	char *addr; // target host address
	uint16_t rtsp_port;
	int ajstatus;
	int ajtype;
	int volume;
	int sfd; // stream socket fd
	int cfd; // control socket fd
	int tfd; // time socket fd
	int wblk_wsize;
	int wblk_remsize;
	pause_state_t pause;
	aes_context ctx;
	uint8_t *data;
	uint8_t min_sdata[MINIMUM_SAMPLE_SIZE*4+16];
	int min_sdata_size;
	time_t paused_time;
	int size_in_aex;
	struct timeval last_read_tv;
	unsigned long ssrc;
	unsigned int timestamp;
        unsigned int servertimes; 
        unsigned int servertimef; 
	int encrypt;
} raopcl_data_t;

struct timeval starttime;
struct timeval acttime;
extern struct raopld_t *raopld;
unsigned long long rtimestamp=0;

static unsigned int seq_number=0;
int raopcl_update_progress(raopcl_data_t *p, int rtimestamp);

static int rsa_encrypt(uint8_t *text, int len, uint8_t *res)
{
	RSA *rsa;
	uint8_t modules[256];
	uint8_t exponent[8];
	int size;

        char n[] =
            "59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUtwC"
            "5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDR"
            "KSKv6kDqnw4UwPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuB"
            "OitnZ/bDzPHrTOZz0Dew0uowxf/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJ"
            "Q+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/UAaHqn9JdsBWLUEpVviYnh"
            "imNVvYFZeCXg/IdTQ+x4IRdiXNv5hEew==";
        char e[] = "AQAB";

	rsa=RSA_new();
#if OPENSSL_VERSION_NUMBER < 0x10100000L
	size=base64_decode(n,modules);
	rsa->n=BN_bin2bn(modules,size,NULL);
	size=base64_decode(e,exponent);
	rsa->e=BN_bin2bn(exponent,size,NULL);
#else
	size=base64_decode(n,modules);
	BIGNUM *bn_n=BN_bin2bn(modules,size,NULL);
	size=base64_decode(e,exponent);
	BIGNUM *bn_e=BN_bin2bn(exponent,size,NULL);
	RSA_set0_key(rsa, bn_n, bn_e, NULL);
#endif
	size=RSA_public_encrypt(len, text, res, rsa, RSA_PKCS1_OAEP_PADDING);
	RSA_free(rsa);
	return size;
}

static int encrypt(raopcl_data_t *raopcld, uint8_t *data, int size)
{
	uint8_t *buf;
	//uint8_t tmp[16];
	int i=0,j;
	memcpy(raopcld->nv,raopcld->iv,16);
	while(i+16<=size){
		buf=data+i;
		for(j=0;j<16;j++) buf[j] ^= raopcld->nv[j];
		aes_encrypt(&raopcld->ctx, buf, buf);
		memcpy(raopcld->nv,buf,16);
		i+=16;
	}
	if(i<size){
#if 0		
		INFMSG("%s: a block less than 16 bytes(%d) is not encrypted\n",__func__, size-i);
		memset(tmp,0,16);
		memcpy(tmp,data+i,size-i);
		for(j=0;j<16;j++) tmp[j] ^= raopcld->nv[j];
		aes_encrypt(&raopcld->ctx, tmp, tmp);
		memcpy(raopcld->nv,tmp,16);
		memcpy(data+i,tmp,16);
		i+=16;
#endif		
	}
	return i;
}

/*
 * after I've updated the firmware of my AEX, data from AEX doesn't come
 * when raop_play is not sending data.
 * 'rsize=GET_BIGENDIAN_INT(buf+0x2c)' this size stops with 330750.
 * I think '330750/44100=7.5 sec' is data in the AEX bufeer, so add a timer
 * to check when the AEX buffer data becomes empty.
 *
 * from aexcl_play, it goes down but doesn't become zero.
 * (12/15/2005)
 */
static int fd_event_callback(void *p, int flags)
{
	int i;
	uint8_t buf[256];
	raopcl_data_t *raopcld;
	int rsize;
	if(!p) return -1;
	raopcld=(raopcl_data_t *)p;
	if(flags&RAOP_FD_READ){
		i=read(raopcld->sfd,buf,sizeof(buf));
		if(i>0){
			rsize=GET_BIGENDIAN_INT(buf+0x2c);
			raopcld->size_in_aex=rsize;
			gettimeofday(&raopcld->last_read_tv,NULL);
                        DBGMSG( "size in aex : %d\n", raopcld->size_in_aex );
			return 0;
		}
		if(i<0) ERRMSG("%s: read error: %s\n", __func__, strerror(errno));
		if(i==0) INFMSG("%s: read, disconnected on the other end\n", __func__);
		return -1;
	}
	
	if(!(flags&RAOP_FD_WRITE)){
		ERRMSG("%s: unknow event flags=%d\n", __func__,flags);
		return -1;
	}
	
	if(!raopcld->wblk_remsize) {
		ERRMSG("%s: write is called with remsize=0\n", __func__);
		return -1;
	}
	i=write(raopcld->sfd,raopcld->data+raopcld->wblk_wsize,raopcld->wblk_remsize);
	if(i<0){
		ERRMSG("%s: write error: %s\n", __func__, strerror(errno));
		return -1;
	}
	if(i==0){
		INFMSG("%s: write, disconnected on the other end\n", __func__);
		return -1;
	}
	raopcld->wblk_wsize+=i;
	raopcld->wblk_remsize-=i;
	if(!raopcld->wblk_remsize) {
           set_fd_event(raopcld->sfd, RAOP_FD_READ, fd_event_callback,(void*)raopcld);
	}
		
	// DBGMSG("%d bytes are sent, remaining size=%d, block size=%d\n",i,raopcld->wblk_remsize,raopcld->wblk_wsize);
	return 0;
}

static int raopcl_connectcontrol(raopcl_data_t *raopcld, uint16_t myport)
{
	if((raopcld->cfd=open_udp_socket(NULL, &myport))==-1) return -1;
        DBGMSG( "opened control port : %d\n", myport );
	if(get_tcp_connect_by_host(raopcld->cfd, raopcld->addr,
				   rtspcl_get_control_port(raopcld->rtspcl))) {
		close(raopcld->tfd);
		raopcld->tfd=-1;
		return -1;
	}
        return 0;
}

int raopcl_start_sync(raopcl_t *p)
{
	pthread_t pth;
	raopcl_data_t *raopcld=(raopcl_data_t *)p;
	return pthread_create(&pth, NULL, (void*)raopcl_sync, (void*)raopcld);
}

int raopcl_sync(void *args)
{
	raopcl_data_t* raopcld = (raopcl_data_t*)args;
	struct timeval ctv;
        static int isFirst = 1;
	int i;
	while(1)
	{
		char data[20];  //geen SSRC
                if ( isFirst )
                {
		   data[0] = 144;	//RTP protocol (0x80) + extension ( 0x10 )
                   isFirst=0;
                }
                else
                {
		   data[0] = 128;	//RTP protocol ( 0x80 )
                }
                // calculate real timestamp in frames
                if ( starttime.tv_sec > 0 ) 
                {
                   gettimeofday(&acttime,NULL);
                   unsigned long long etimeinms = (unsigned long)((acttime.tv_sec-starttime.tv_sec)*1000+(acttime.tv_usec-starttime.tv_usec)/1000);
                   rtimestamp = etimeinms*auds_get_sample_rate(raopld->auds)/1000;

                   // INFMSG("sync ts : %llu te : %llu diff : %llu\n", rtimestamp, etimeinms, raopcld->timestamp-rtimestamp );
                }

		data[1] = 212;  //payload type=84 + marker bit set = 212
	        data[2] = (unsigned char)((seq_number & 0xFF00) >> 8); //seq_number
	        data[3] = (unsigned char)(seq_number & 0x00FF);
		// data[2] = 0x00; //sequence number -> always 7??
		// data[3] = 0x07;
		data[4] = (unsigned char)(((rtimestamp-alatency) & 0xFF000000) >> 24); //timestamp
		data[5] = (unsigned char)(((rtimestamp-alatency) & 0x00FF0000) >> 16);
		data[6] = (unsigned char)(((rtimestamp-alatency) & 0x0000FF00) >> 8); 
		data[7] = (unsigned char)((rtimestamp-alatency) & 0x000000FF);
		
		gettimeofday(&ctv,NULL);
		uint64_t timestamp = calculate_timestamp(&ctv);

		data[8] = (unsigned char)((timestamp & 0xFF00000000000000ULL) >> 56); //ntp-timestamp
		data[9] = (unsigned char)((timestamp & 0x00FF000000000000ULL) >> 48);
		data[10] = (unsigned char)((timestamp & 0x0000FF0000000000ULL) >> 40);
		data[11] = (unsigned char)((timestamp & 0x000000FF00000000ULL) >> 32);
		data[12] = (unsigned char)((timestamp & 0x00000000FF000000ULL) >> 24);
		data[13] = (unsigned char)((timestamp & 0x0000000000FF0000ULL) >> 16);
		data[14] = (unsigned char)((timestamp & 0x000000000000FF00ULL) >> 8); 
		data[15] = (unsigned char)(timestamp & 0x00000000000000FFULL);
			
		data[16] = (unsigned char)((rtimestamp & 0xFF000000) >> 24); //timestamp
		data[17] = (unsigned char)((rtimestamp & 0x00FF0000) >> 16);
		data[18] = (unsigned char)((rtimestamp & 0x0000FF00) >> 8); 
		data[19] = (unsigned char)(rtimestamp & 0x000000FF);
		
		i = write(raopcld->cfd,&data,sizeof(data));
		if(i<0) ERRMSG("raop_sync: write error: %s\n", strerror(errno));
		if(i==0) INFMSG("raop_sync: write, disconnected on the other end\n");

                // update progress
                // raopcl_update_progress(raopcld, rtimestamp);

		sleep(1);

	}
	return 0;
}

static int raopcl_connecttime(raopcl_data_t *raopcld, uint16_t myport)
{
	if((raopcld->tfd=open_udp_socket(NULL, &myport))==-1) return -1;
        DBGMSG( "opened timing port : %d\n", myport );
	if(get_tcp_connect_by_host(raopcld->tfd, raopcld->addr,
				   rtspcl_get_timing_port(raopcld->rtspcl))) {
		close(raopcld->tfd);
		raopcld->tfd=-1;
		return -1;
	}
	char c = 255;
	int i = write(raopcld->tfd,&c,1);
	if(i<0){
		ERRMSG("%s: write error: %s\n", __func__, strerror(errno));
		return -1;
	}
	if(i==0){
		INFMSG("%s: write, disconnected on the other end\n", __func__);
		return -1;
	}
	pthread_t pth;
	return pthread_create(&pth, NULL, (void*)raopcl_time_connect, (void*)raopcld);
}

int raopcl_time_connect(void *args)
{
	raopcl_data_t* raopcld = (raopcl_data_t*)args;
	uint8_t buf[256];
	int j, i;
	struct timeval ctv;

	while(1)
	{
		i=read(raopcld->tfd,buf,sizeof(buf));
		if(i>0)
		{
			// DBGMSG("%s: read %d bytes, rsize=%d\n", __func__, i,rsize);
			char data[32];  //geen SSRC
			data[0] = 128;	//RTP protocol
			data[1] = 0xd3;  //payload type=83 + marker bit set = 211
	                // data[2] = (unsigned char)((seq_number & 0xFF00) >> 8); //seq_number
	                // data[3] = (unsigned char)(seq_number & 0x00FF);
			data[2] = 0x00; //sequence number ??
			data[3] = 0x07;
			data[4] = 0x00; //zero padding = 0
			data[5] = 0x00;
			data[6] = 0x00;
			data[7] = 0x00;
                        raopcld->servertimes=0;
                        raopcld->servertimef=0;
			for(j=8;j<16;j++) // reference ntp time : same as sent by server
			{
			    data[j] = buf[j+16]; //previous transmit time
                            if ( j<12 )
                            {
                               raopcld->servertimes+=buf[j+16]<<(8*(11-j));
                            }
                            else
                            {
                               raopcld->servertimef+=buf[j+16]<<(8*(15-j));
                            }
			}

			gettimeofday(&ctv,NULL);
			// DBGMSG("%s: server time : ( %u, %u ) local : %lu\n", __func__, raopcld->servertimes, raopcld->servertimef, ctv.tv_sec );
			uint64_t timestamp = calculate_timestamp(&ctv); // received time

			data[16] = (unsigned char)((timestamp & 0xFF00000000000000ULL) >> 56); //ntp-timestamp
			data[17] = (unsigned char)((timestamp & 0x00FF000000000000ULL) >> 48);
			data[18] = (unsigned char)((timestamp & 0x0000FF0000000000ULL) >> 40);
			data[19] = (unsigned char)((timestamp & 0x000000FF00000000ULL) >> 32);
			data[20] = (unsigned char)((timestamp & 0x00000000FF000000ULL) >> 24);
			data[21] = (unsigned char)((timestamp & 0x0000000000FF0000ULL) >> 16);
			data[22] = (unsigned char)((timestamp & 0x000000000000FF00ULL) >> 8); 
			data[23] = (unsigned char)(timestamp & 0x00000000000000FFULL);
			
			for(j=24;j<32;j++) // sent time = received time
			{
				data[j] = data[j-8];
			}
                        data[31]+=10;

			//DBGMSG("%s: %d bytes of timestamp: %s\n", __func__, sizeof(timestamp), timestamp);
			
			i = write(raopcld->tfd,&data,sizeof(data));
                        if ( i != sizeof(data) )
                        {
                           ERRMSG( "error responding to sync\n" );
                        }
		}
		if (i<0) 
                {
                   ERRMSG("raopcl_time_connect: read error: %s\n", strerror(errno)); 
		   break;
                }
		if (i==0)
                {
                   ERRMSG("raopcl_time_connect: read, disconnected on the other end\n"); 
		   break;
                }
	}
	return 0;
}

uint64_t calculate_timestamp(struct timeval *tv) {
    uint64_t ntpts;

    ntpts = (((uint64_t)tv->tv_sec + 2208988800u) << 32) + ((uint32_t)tv->tv_usec * 4294.967296);
    // ntpts = (((uint64_t)tv->tv_sec + 2208988800u) << 32) + ((uint32_t)( ( tv->tv_usec * ( UINT_MAX + 1 ) / 1000000 ) ) );

    return (ntpts);
}

static int raopcl_stream_connect(raopcl_data_t *raopcld)
{
	raopcl_connecttime(raopcld, rtspcl_get_timing_port(raopcld->rtspcl));
	raopcl_connectcontrol(raopcld, rtspcl_get_control_port(raopcld->rtspcl));
	uint16_t myport=0;

	if((raopcld->sfd=open_udp_socket(NULL, &myport))==-1) return -1;
        DBGMSG( "opened stream port : %d\n", myport );
	if(get_tcp_connect_by_host(raopcld->sfd, raopcld->addr,
				   rtspcl_get_server_port(raopcld->rtspcl))) {
		close(raopcld->sfd);
		raopcld->sfd=-1;
		return -1;
	}

	return 0;
}

int raopcl_small_silent(raopcl_t *p)
{
	raopcl_data_t *raopcld=(raopcl_data_t *)p;
	raopcl_send_sample(p,raopcld->min_sdata,raopcld->min_sdata_size);
	//DBGMSG("sent a small silent data\n");
	return 0;
}

int raopcl_pause_check(raopcl_t *p)
{
	raopcl_data_t *raopcld=(raopcl_data_t *)p;
	// if in puase, keep sending a small silent data every 3 seconds
	switch(raopcld->pause) {
	case NO_PAUSE:
		return 0;
	case OP_PAUSE:
		if(time(NULL)-raopcld->paused_time<3) return 0;
		rtspcl_flush(raopcld->rtspcl);
		raopcld->paused_time=time(NULL);
		return 1;
	case NODATA_PAUSE:
		if(time(NULL)-raopcld->paused_time<3) return 0;
		raopcl_small_silent(p);
		raopcld->paused_time=time(NULL);
		return 1;
	}
	return -1;
}

void msleep (unsigned long long ms) {
    struct timeval tv;
    tv.tv_sec  = ms / 1000000;
    tv.tv_usec = ms % 1000000;
    select (0, NULL, NULL, NULL, &tv);  
}

int raopcl_wait_songdone(raopcl_t *p, int set)
{
   raopcl_data_t *raopcld=(raopcl_data_t *)p;

   while ( raopcld->timestamp > rtimestamp )
   {
      // update timestamp
      gettimeofday(&acttime,NULL);
      unsigned long long etimeinms = (unsigned long)((acttime.tv_sec-starttime.tv_sec)*1000+(acttime.tv_usec-starttime.tv_usec)/1000);
      rtimestamp = etimeinms*auds_get_sample_rate(raopld->auds)/1000;

      msleep( 10*1000 ); // sleep 10 ms
   }

   return 0;
}

int raopcl_sample_remsize(raopcl_t *p)
{
	raopcl_data_t *raopcld=(raopcl_data_t *)p;
	if(!p) return -1;
	return raopcld->wblk_remsize;
}

int raopcl_count_samples(raopcl_data_t *raopcld)
{
	if(raopcld->timestamp>=rtimestamp+BUFFER*MAX_SAMPLES_IN_CHUNK)
	{
           unsigned long long mdelay = MAX_SAMPLES_IN_CHUNK*1000*1000/auds_get_sample_rate(raopld->auds)+1;
	   msleep(mdelay);	// sleep for 1 packet
           // DBGMSG( "sleep : ts : %u rts : %llu\n", raopcld->timestamp, rtimestamp );
	}
	return 0;
}

int raopcl_send_sample(raopcl_t *p, uint8_t *sample, int count)
{
	static int begin=1;
	int rval=-1;
	int audio_header_size = 0;

	raopcl_data_t *raopcld;
	if(!p) return -1;
	raopcld=(raopcl_data_t *)p;
	
	unsigned char header[12];
	header[0] = 128; //RTP
	header[1] = 96; //DynamicRTP-Type-96
	if(begin)
	{
             gettimeofday(&starttime,NULL);
	     header[1] = header[1] + 128; //marker bit setten
	}
	header[2] = (unsigned char)((seq_number & 0xFF00) >> 8); //seq_number
	header[3] = (unsigned char)(seq_number & 0x00FF);
	header[4] = (unsigned char)((raopcld->timestamp & 0xFF000000) >> 24); //timestamp
	header[5] = (unsigned char)((raopcld->timestamp & 0x00FF0000) >> 16);
	header[6] = (unsigned char)((raopcld->timestamp & 0x0000FF00) >> 8); 
	header[7] = (unsigned char)(raopcld->timestamp & 0x000000FF);
	header[8] = (unsigned char)((raopcld->ssrc & 0xFF000000) >> 24); //ssrc
	header[9] = (unsigned char)((raopcld->ssrc & 0x00FF0000) >> 16);
	header[10] = (unsigned char)((raopcld->ssrc & 0x0000FF00) >> 8); 
	header[11] = (unsigned char)(raopcld->ssrc & 0x000000FF);
	const int header_size=sizeof(header);
	
        gettimeofday(&acttime,NULL);
        // INFMSG("sending samples : %d ts : %u te:%d\n", count, raopcld->timestamp,
        //        (int)((acttime.tv_sec-starttime.tv_sec)*1000+(acttime.tv_usec-starttime.tv_usec)/1000) );
	raopcld->timestamp = raopcld->timestamp + MAX_SAMPLES_IN_CHUNK;  //number of packets inside

	if(realloc_memory((void**)&raopcld->data, count+header_size+audio_header_size, __func__)) goto erexit;
	memcpy(raopcld->data,header,header_size);
	//memcpy(raopcld->data+header_size,audioheader,audio_header_size);
	memcpy(raopcld->data+header_size+audio_header_size,sample,count);
	if(raopcld->encrypt)
	{
                // with newer airport express, please don't use encryption ( -e )
		encrypt(raopcld, raopcld->data+header_size+audio_header_size, count);
	}
	raopcld->wblk_remsize=count+header_size+audio_header_size;
	raopcld->wblk_wsize=0;
        // DBGMSG( "really sending : %d ( count = %d ) \n", raopcld->wblk_remsize, count );
	if(set_fd_event(raopcld->sfd,RAOP_FD_READ|RAOP_FD_WRITE, fd_event_callback,(void*)raopcld)) goto erexit;
	seq_number++;
	begin = 0;
	rval=raopcl_count_samples(raopcld);
	
 erexit:
	return rval;
}


raopcl_t *raopcl_open()
{
	raopcl_data_t *raopcld;
	//int16_t sdata[MINIMUM_SAMPLE_SIZE*2];
	//data_source_t ds={.type=MEMORY};
	//uint8_t *bp;

	raopcld=malloc(sizeof(raopcl_data_t));
	RAND_seed(raopcld,sizeof(raopcl_data_t));
	memset(raopcld, 0, sizeof(raopcl_data_t));
	if(!RAND_bytes(raopcld->iv, sizeof(raopcld->iv)) || !RAND_bytes(raopcld->key, sizeof(raopcld->key))){
		ERRMSG("%s:RAND_bytes error code=%ld\n",__func__,ERR_get_error());
		return NULL;
	}
	memcpy(raopcld->nv,raopcld->iv,sizeof(raopcld->nv));
	raopcld->volume=0.0;
        aes_set_key(&raopcld->ctx, raopcld->key, 128);
	// prepare a small silent data to send during pause period.
	// ds.u.mem.size=MINIMUM_SAMPLE_SIZE*4;
	// ds.u.mem.data=sdata;
	// memset(sdata,0,sizeof(sdata));
	// auds_write_pcm(NULL, raopcld->min_sdata, &bp, &raopcld->min_sdata_size,
	// 	       MINIMUM_SAMPLE_SIZE, &ds);
	return (raopcl_t *)raopcld;
}

int raopcl_close(raopcl_t *p)
{
	raopcl_data_t *raopcld;
	if(!p) return -1;

	raopcld=(raopcl_data_t *)p;
	if(raopcld->rtspcl)
		rtspcl_close(raopcld->rtspcl);
	if(raopcld->data) free(raopcld->data);
	if(raopcld->addr) free(raopcld->addr);
	free(raopcld);
	return 0;
}

/*
 * update volume
 * minimum=0, maximum=100
 */
int raopcl_update_volume(raopcl_t *p, int vol)
{
	char a[128];
	raopcl_data_t *raopcld=(raopcl_data_t *)p;
	if(!p) return -1;
	
	if(!raopcld->rtspcl) return -1;
        if ( vol == 0 ) raopcld->volume = -144.0;
        else
	  raopcld->volume=VOLUME_MIN+(VOLUME_MAX-VOLUME_MIN)*vol/100;
	sprintf(a, "volume: %d.000000\r\n", raopcld->volume);
	return rtspcl_set_parameter(raopcld->rtspcl,a);
}

int raopcl_connect(raopcl_t *p, char *host, uint16_t destport, int encrypt, int volume)
{
	uint8_t buf[4+8+16];
	char sid[16];
	char sci[24];
	char *sac=NULL,*key=NULL,*iv=NULL;
	char sdp[1024];
	int rval=-1;
	key_data_t *setup_kd=NULL;
	char *aj, *token, *pc;
	const char delimiters[] = ";";
	uint8_t rsakey[512];
	int i;
	raopcl_data_t *raopcld;
	if(!p) return -1;

	raopcld=(raopcl_data_t *)p;
	RAND_bytes(buf, sizeof(buf));
	sprintf(sid, "%d%hu", 3420, *((uint16_t*)buf));
	raopcld->ssrc = (unsigned long)(((rand()) << 16) + (rand())) % 4294967295ULL;
        raopcld->timestamp=0;
	raopcld->encrypt = encrypt;
	sprintf(sci, "%08x%08x",*((uint32_t*)(buf+4)),*((uint32_t*)(buf+8)));
	base64_encode(buf+12,16,&sac);
	if(!(raopcld->rtspcl=rtspcl_open())) goto erexit;
	if(rtspcl_set_useragent(raopcld->rtspcl,"iTunes/4.6 (Macintosh; U; PPC Mac OS X 10.3)")) goto erexit;
	//if(rtspcl_set_useragent(raopcld->rtspcl,"iTunes/7.6.2 (Windows; N;)")) goto erexit;
	// if(rtspcl_set_useragent(raopcld->rtspcl,"iTunes/10.6 (Macintosh; Intel Mac OS X 10.7.3) AppleWebKit/535.18.5")) goto erexit;
	if(rtspcl_add_exthds(raopcld->rtspcl,"Client-Instance", sci)) goto erexit;
	// if(rtspcl_add_exthds(raopcld->rtspcl,"Active-Remote", "1986535575")) goto erexit;
	// if(rtspcl_add_exthds(raopcld->rtspcl,"DACP-ID", sci)) goto erexit;
	if(rtspcl_connect(raopcld->rtspcl, host, destport, sid)) goto erexit;
	
	//auth
	//if(rtspcl_auth_setup(raopcld->rtspcl)) goto erexit;

	//get options
	if(rtspcl_options(raopcld->rtspcl)) goto erexit;
	
	sprintf(sdp,
		"v=0\r\n"
		"o=iTunes %s 0 IN IP4 %s\r\n"
		"s=iTunes\r\n"
		"c=IN IP4 %s\r\n"
		"t=0 0\r\n"
		"m=audio 0 RTP/AVP 96\r\n"
		"a=rtpmap:96 AppleLossless\r\n"
		"a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 44100\r\n",
		sid, rtspcl_local_ip(raopcld->rtspcl), host, MAX_SAMPLES_IN_CHUNK);

	if (raopcld->encrypt) {
		i=rsa_encrypt(raopcld->key,16,rsakey);
		base64_encode(rsakey,i,&key);
		remove_char_from_string(key,'=');
		base64_encode(raopcld->iv,16,&iv);
		remove_char_from_string(iv,'=');
		sprintf(sdp + strlen(sdp), "a=rsaaeskey:%s\r\na=aesiv:%s\r\n", key, iv);
	}

	remove_char_from_string(sac,'=');
	if(rtspcl_add_exthds(raopcld->rtspcl, "Apple-Challenge", sac)) goto erexit;
	if(rtspcl_announce_sdp(raopcld->rtspcl, sdp)) goto erexit;
	if(rtspcl_mark_del_exthds(raopcld->rtspcl, "Apple-Challenge")) goto erexit;
	
	if(rtspcl_setup(raopcld->rtspcl, &setup_kd)) goto erexit;
	if(!(aj=kd_lookup(setup_kd,"Audio-Jack-Status"))) {
		ERRMSG("%s: Audio-Jack-Status is missing\n",__func__);
		goto erexit;
	}
	
	token=strtok(aj,delimiters);
	while(token){
		if((pc=strstr(token,"="))){
			*pc=0;
			if(!strcmp(token,"type") && !strcmp(pc+1,"digital")){
				raopcld->ajtype=JACK_TYPE_DIGITAL;
			}
		}else{
			if(!strcmp(token,"connected")){
				raopcld->ajstatus=JACK_STATUS_CONNECTED;
			}
		}
		token=strtok(NULL,delimiters);
	}
	// keep host address and port information
	if(realloc_memory((void**)&raopcld->addr,strlen(host)+1,__func__)) goto erexit;
	strcpy(raopcld->addr,host);
	raopcld->rtsp_port=destport;

	if(raopcl_update_volume(p, volume)) goto erexit;
	if(raopcl_stream_connect(raopcld)) goto erexit;
	if(rtspcl_record(raopcld->rtspcl)) goto erexit;

	rval=0;
 erexit:
	if(sac) free(sac);
	if(key) free(key);
	if(iv) free(iv);
	free_kd(setup_kd);
	return rval;
}

/*
 * update progress
 * absolute start/absolute current/absolute end
 */
int raopcl_update_progress(raopcl_data_t *p, int rtimestamp)
{
	char a[128];
	if(!p) return -1;

        if ( starttime.tv_sec<=0 ) return 0;
	
	sprintf(a, "progress: %d/%d/%d\r\n", 0, rtimestamp, wav_get_length());
        DBGMSG( "sending progress : %s\n", a );
	return rtspcl_set_parameter(p->rtspcl,a);
}

int raopcl_set_content(raopcl_t *p, char* itemname, char* songartist, char* songalbum)
{
	char a[512];
	memset (a,'\0',sizeof(a));
	char* commands[] = {"mlit", "minm", "asar", "asal"};
	char size[4];
	unsigned long theSize = 0;
	unsigned long totalSize = strlen(commands[1])+sizeof(size)+strlen(itemname)+1+strlen(commands[2])+sizeof(size)+strlen(songartist)+1+strlen(commands[3])+sizeof(size)+strlen(songalbum)+1;
	raopcl_data_t *raopcld=(raopcl_data_t *)p;
	if(!p) return -1;
	
	if(!raopcld->rtspcl) return -1;
	
	memcpy(a,commands[0],strlen(commands[0]));   //first is a container that provides every command
	theSize = totalSize;
	size[0] = (unsigned char)((theSize & 0xFF000000) >> 24); //size
	size[1] = (unsigned char)((theSize & 0x00FF0000) >> 16);
	size[2] = (unsigned char)((theSize & 0x0000FF00) >> 8); 
	size[3] = (unsigned char)(theSize & 0x000000FF);
	memcpy(a+strlen(commands[0]),size,sizeof(size));
	
	totalSize += (strlen(commands[0]) + sizeof(size));
	
	//songname
	memcpy(a+strlen(commands[0])+sizeof(size),commands[1],strlen(commands[1]));
	theSize = strlen(itemname)+1;
	size[0] = (unsigned char)((theSize & 0xFF000000) >> 24); //size
	size[1] = (unsigned char)((theSize & 0x00FF0000) >> 16);
	size[2] = (unsigned char)((theSize & 0x0000FF00) >> 8); 
	size[3] = (unsigned char)(theSize & 0x000000FF);
	memcpy(a+strlen(commands[0])+sizeof(size)+strlen(commands[1]),size,sizeof(size));
	memcpy(a+strlen(commands[0])+sizeof(size)+strlen(commands[1])+sizeof(size),itemname,strlen(itemname));
	
	//songartist
	memcpy(a+sizeof(commands[0])+sizeof(size)+sizeof(commands[1])+sizeof(size)+strlen(itemname)+1,commands[2],strlen(commands[2]));
	theSize = strlen(songartist)+1;
	size[0] = (unsigned char)((theSize & 0xFF000000) >> 24); //size
	size[1] = (unsigned char)((theSize & 0x00FF0000) >> 16);
	size[2] = (unsigned char)((theSize & 0x0000FF00) >> 8); 
	size[3] = (unsigned char)(theSize & 0x000000FF);
	memcpy(a+strlen(commands[0])+sizeof(size)+strlen(commands[1])+sizeof(size)+strlen(itemname)+1+strlen(commands[2]),size,sizeof(size));
	memcpy(a+strlen(commands[0])+sizeof(size)+strlen(commands[1])+sizeof(size)+strlen(itemname)+1+strlen(commands[2])+sizeof(size),songartist,strlen(songartist));
	
	//songalbum
	memcpy(a+strlen(commands[0])+sizeof(size)+strlen(commands[1])+sizeof(size)+strlen(itemname)+1+sizeof(commands[2])+sizeof(size)+strlen(songartist)+1,commands[3],strlen(commands[3]));
	theSize = strlen(songalbum)+1;
	size[0] = (unsigned char)((theSize & 0xFF000000) >> 24); //size
	size[1] = (unsigned char)((theSize & 0x00FF0000) >> 16);
	size[2] = (unsigned char)((theSize & 0x0000FF00) >> 8); 
	size[3] = (unsigned char)(theSize & 0x000000FF);
	memcpy(a+strlen(commands[0])+sizeof(size)+strlen(commands[1])+sizeof(size)+strlen(itemname)+1+strlen(commands[2])+sizeof(size)+strlen(songartist)+1+strlen(commands[3]),size,sizeof(size));
	memcpy(a+strlen(commands[0])+sizeof(size)+strlen(commands[1])+sizeof(size)+strlen(itemname)+1+strlen(commands[2])+sizeof(size)+strlen(songartist)+1+strlen(commands[3])+sizeof(size),songalbum,strlen(songalbum));
	
	//send it to the client
	return rtspcl_set_daap(raopcld->rtspcl, a, raopcld->timestamp, totalSize);
}

int raopcl_flush_stream(raopcl_t *p)
{
	raopcl_data_t *raopcld=(raopcl_data_t *)p;
	if(!p) return -1;
	return rtspcl_flush(raopcld->rtspcl);
}

int raopcl_set_pause(raopcl_t *p, pause_state_t pause)
{
	raopcl_data_t *raopcld=(raopcl_data_t *)p;
	if(!p) return -1;

	raopcld->pause=pause;
	switch(pause){
	case OP_PAUSE:
		rtspcl_flush(raopcld->rtspcl);
	case NODATA_PAUSE:
		set_fd_event(raopcld->sfd, RAOP_FD_READ, fd_event_callback,(void*)raopcld);
		raopcld->paused_time=time(NULL);
		break;
	case NO_PAUSE:
		set_fd_event(raopcld->sfd, RAOP_FD_READ|RAOP_FD_WRITE, fd_event_callback,(void*)raopcld);
		break;
	}
	return 0;
}

pause_state_t raopcl_get_pause(raopcl_t *p)
{
	raopcl_data_t *raopcld=(raopcl_data_t *)p;
	if(!p) return -1;

	return raopcld->pause;
}

int raopcl_aexbuf_time(raopcl_t *p, struct timeval *dtv)
{
	raopcl_data_t *raopcld=(raopcl_data_t *)p;
	if(!p) return -1;
	struct timeval ctv, atv;

        INFMSG( "size in aex : %d\n", raopcld->size_in_aex );
	if(raopcld->size_in_aex<=0) {
		memset(dtv, 0, sizeof(struct timeval));
		return 1; // not playing?
	}

	atv.tv_sec=raopcld->size_in_aex/44100;
	atv.tv_usec=(raopcld->size_in_aex%44100)*10000/441;

	gettimeofday(&ctv,NULL);
	dtv->tv_sec=ctv.tv_sec - raopcld->last_read_tv.tv_sec;
	dtv->tv_usec=ctv.tv_usec - raopcld->last_read_tv.tv_usec;

	dtv->tv_sec=atv.tv_sec-dtv->tv_sec;
	dtv->tv_usec=atv.tv_usec-dtv->tv_usec;
	
	if(dtv->tv_usec>=1000000){
		dtv->tv_sec++;
		dtv->tv_usec-=1000000;
	}else if(dtv->tv_usec<0){
		dtv->tv_sec--;
		dtv->tv_usec+=1000000;
	}

	if(dtv->tv_sec<0) memset(dtv, 0, sizeof(struct timeval));
	//DBGMSG("%s:tv_sec=%d, tv_usec=%d\n",__func__,(int)dtv->tv_sec,(int)dtv->tv_usec);
	return 0;
}

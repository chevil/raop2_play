/*****************************************************************************
 * rtsp_play.c: RAOP Client player
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
#include <signal.h>
#include <sys/select.h>
#include "aexcl_lib.h"
#include "raop_client.h"
#include "audio_stream.h"
#include "raop_play.h"

raopld_t *raopld;

long startinms=-1;
long endinms=-1;
double balance=50.0;
double csync=0.0;

static int print_usage(char *argv[])
{
	printf("%s [-p port_number] [-v volume(0-100)] "
               "[-s startms] [-u endms] [-b [0-100]] [-d [-30-+30(s)]] "
	       "[-i interactive mode] [-e no-encrypt-mode] server_ip audio_filename\n",argv[0]);
	return -1;
}

#define MAIN_EVENT_TIMEOUT 3 // sec unit
static int main_event_handler()
{
	fd_set rdfds,wrfds;
	int fdmax=0;
	int i;
	struct timeval tout={.tv_sec=MAIN_EVENT_TIMEOUT, .tv_usec=0};
	
	FD_ZERO(&rdfds);
	FD_ZERO(&wrfds);
	for(i=0;i<MAX_NUM_OF_FDS;i++){
		if(raopld->fds[i].fd<0) continue;
		if(raopld->fds[i].flags&RAOP_FD_READ)
			FD_SET(raopld->fds[i].fd, &rdfds);
		if(raopld->fds[i].flags&RAOP_FD_WRITE)
			FD_SET(raopld->fds[i].fd, &wrfds);
		fdmax=(fdmax<raopld->fds[i].fd)?raopld->fds[i].fd:fdmax;
	}

	select(fdmax+1,&rdfds,&wrfds,NULL,&tout);

	for(i=0;i<MAX_NUM_OF_FDS;i++){
		if(raopld->fds[i].fd<0) continue;
		if((raopld->fds[i].flags&RAOP_FD_READ) &&
		   FD_ISSET(raopld->fds[i].fd,&rdfds)){
			//DBGMSG("rd event i=%d, flags=%d\n",i,raopld->fds[i].flags);
			if(raopld->fds[i].cbf &&
			   raopld->fds[i].cbf(raopld->fds[i].dp, RAOP_FD_READ)) return -1;
		}
		if((raopld->fds[i].flags&RAOP_FD_WRITE) &&
		   FD_ISSET(raopld->fds[i].fd,&wrfds)){
			//DBGMSG("wr event i=%d, flags=%d\n",i,raopld->fds[i].flags);
			if(raopld->fds[i].cbf &&
			   raopld->fds[i].cbf(raopld->fds[i].dp, RAOP_FD_WRITE)) return -1;
		}
	}

	raopcl_pause_check(raopld->raopcl);

	return 0;
}

static int terminate_cbf(void *p, int flags){
	return -1;
}

static void sig_action(int signo, siginfo_t *siginfo, void *extra)
{
	// SIGCHLD, a child process is terminated
	if(signo==SIGCHLD){
		auds_sigchld(raopld->auds, siginfo);
		return;
	}
	//SIGINT,SIGTERM
	DBGMSG("SIGINT or SIGTERM\n");
	set_fd_event(1,RAOP_FD_WRITE,terminate_cbf,NULL);
	return;
}

int main(int argc, char *argv[])
{
	char *host=NULL;
	char *fname=NULL;
	int port=SERVER_PORT;
	int rval=-1,i;
	int size;
	int ret;
	int volume=100;
	int encrypt=1;
	uint8_t *buf;
	int iact=0;
	struct sigaction act;

	/* Assign sig_term as our SIGTERM handler */
	act.sa_sigaction = sig_action;
	sigemptyset(&act.sa_mask); // no other signals are blocked
	act.sa_flags = SA_SIGINFO; // use sa_sigaction instead of sa_handler
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGINT, &act, NULL);
	sigaction(SIGCHLD, &act, NULL);

	for(i=1;i<argc;i++){
		if(!strcmp(argv[i],"-i")){
			iact=1;
			continue;
		}
		if(!strcmp(argv[i],"-p")){
			port=atoi(argv[++i]);
			continue;
		}
		if(!strcmp(argv[i],"-v")){
			volume=atoi(argv[++i]);
			continue;
		}
		if(!strcmp(argv[i],"-e")){
			encrypt=0;
			continue;
		}
		if(!strcmp(argv[i],"-s")){
			startinms=atol(argv[++i]);
                        DBGMSG( "start time set to %ld\n", startinms );
                        if ( endinms != -1 && startinms >= endinms )
                        {
                           ERRMSG( "wrong start time : %ld\n", startinms );
                           startinms=endinms;
                        }
			continue;
		}
		if(!strcmp(argv[i],"-u")){
			endinms=atol(argv[++i]);
                        if ( startinms != -1 && endinms <= startinms )
                        {
                           ERRMSG( "wrong end time : %ld\n", endinms );
                           endinms=startinms;
                        }
			continue;
		}
		if(!strcmp(argv[i],"-b")){
			double rbalance=atof(argv[++i]);
                        if ( rbalance < 0.0 || rbalance > 100.0 )
                        {
                           ERRMSG( "wrong balance : %f\n", rbalance );
                        }
                        else
                        {
                           DBGMSG( "balance set to : %f\n", rbalance );
                           balance=rbalance;
                        }
			continue;
		}
		if(!strcmp(argv[i],"-d")){
			double rsync=atof(argv[++i]);
                        if ( rsync < -30.0 || rsync > 30.0 )
                        {
                           ERRMSG( "wrong sync : %f\n", rsync );
                        }
                        else
                        {
                           DBGMSG( "sync set to : %f\n", rsync );
                           csync=rsync;
                        }
			continue;
		}
		if(!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h"))
			return print_usage(argv);
		if(!host) {host=argv[i]; continue;}
		if(!fname) {fname=argv[i]; continue;}
	}
	if(!host) return print_usage(argv);
	if(!iact && !fname) return print_usage(argv);

	raopld=(raopld_t*)malloc(sizeof(raopld_t));
	if(!raopld) goto erexit;
	memset(raopld,0,sizeof(raopld_t));
	for(i=0;i<MAX_NUM_OF_FDS;i++) raopld->fds[i].fd=-1;

	raopld->raopcl=raopcl_open();
	if(!raopld->raopcl) goto erexit;
	if(raopcl_connect(raopld->raopcl,host,port,encrypt, volume)) goto erexit;
	if(raopcl_start_sync(raopld->raopcl)) goto erexit;
	if(raopcl_update_volume(raopld->raopcl,volume)) goto erexit;

	// if(raopcl_set_content(raopld->raopcl, "pianodisc", "raop play", "")) goto erexit;
	printf("%s to %s\n",RAOP_CONNECTED, host);
	fflush(stdout);
	if(fname && !(raopld->auds=auds_open(fname,0))) goto erexit;
	rval=0;
	
	while(!rval){
		if(!raopld->auds){
			// if audio data is not opened, just check events
			rval=main_event_handler(raopld);
			continue;
		}
		switch(raopcl_get_pause(raopld->raopcl)){
		case OP_PAUSE:
			rval=main_event_handler();
			continue;
		case NODATA_PAUSE:
			rval=main_event_handler();
			continue;
		case NO_PAUSE:
			if((ret=auds_get_next_sample(raopld->auds, &buf, &size))!=0)
                        {
                            ERRMSG( "get next sample returned : %d\n", ret );
			    auds_close(raopld->auds);
			    raopld->auds=NULL;
                            DBGMSG( "waiting for song to finish\n" );
			    raopcl_wait_songdone(raopld->raopcl,1);
                            goto erexit;
			}
                        else
                        {
                            // DBGMSG( "sending %d bytes\n", size );
			    if(raopcl_send_sample(raopld->raopcl,buf,size)) break;
			    do{
				if((rval=main_event_handler())) 
                                {
                                   ERRMSG( "main event handler returned : %d\n", rval );
			           auds_close(raopld->auds);
			           raopld->auds=NULL;
			           raopcl_wait_songdone(raopld->raopcl,1);
                                   goto erexit;
                                }
			    }while(raopld->auds && raopcl_sample_remsize(raopld->raopcl));
                        }
			break;
		default:
			rval=-1;
			break;
		}
	}
 erexit:	
        // do not close connection, buffer is not empty
	// rval=raopcl_close(raopld->raopcl);
	if(raopld->auds) auds_close(raopld->auds);
	if(raopld) free(raopld);
	return rval;
}

int set_fd_event(int fd, int flags, fd_callback_t cbf, void *p)
{
	int i;
	// check the same fd first. if it exists, update it
	for(i=0;i<MAX_NUM_OF_FDS;i++){
		if(raopld->fds[i].fd==fd){
			raopld->fds[i].dp=p;
			raopld->fds[i].cbf=cbf;
			raopld->fds[i].flags=flags;
			return 0;
		}
	}
	// then create a new one
	for(i=0;i<MAX_NUM_OF_FDS;i++){
		if(raopld->fds[i].fd<0){
			raopld->fds[i].fd=fd;
			raopld->fds[i].dp=p;
			raopld->fds[i].cbf=cbf;
			raopld->fds[i].flags=flags;
			return 0;
		}
	}
	return -1;
}

int clear_fd_event(int fd)
{
	int i;
	for(i=0;i<MAX_NUM_OF_FDS;i++){
		if(raopld->fds[i].fd==fd){
			raopld->fds[i].fd=-1;
			raopld->fds[i].dp=NULL;
			raopld->fds[i].cbf=NULL;
			raopld->fds[i].flags=0;
			return 0;
		}
	}
	return -1;
}

/*----------------------------------------------------------------
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

Midas Zhou
-----------------------------------------------------------------*/

#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h> /* usleep */
#include <stdbool.h>
#include "egi_timer.h"
#include "egi_symbol.h"
#include "egi_fbgeom.h"
#include "dict.h"


#define EGI_ENABLE_TICK 1

struct itimerval tm_val, tm_oval;

char tm_strbuf[50]={0};
const char *str_weekday[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
//const char *str_weekday[]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
const char *str_month[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Oct","Nov","Sub"};

/* global tick */
static struct itimerval tm_tick_val; //tm_tick_oval;
static long long unsigned int tm_tick_count=0;


/*-------------------------------------
Get time stamp in ms
--------------------------------------*/
long long unsigned int tm_get_tmstampms(void)
{
	struct timeval tmval;

	gettimeofday(&tmval, NULL);

	return ( ((long long unsigned int)tmval.tv_sec)*1000+tmval.tv_usec/1000);
}


/*---------------------------------------------
 get local time in string in format:
 		H:M:S
 The caller must ensure enough space for tmbuf.
---------------------------------------------*/
void tm_get_strtime(char *tmbuf)
{
	time_t tm_t; /* time in seconds */
	struct tm *tm_s; /* time in struct */

	time(&tm_t);
	tm_s=localtime(&tm_t);

	/*
		tm_s->tm_year start from 1900
		tm_s->tm_mon start from 0
	*/
	sprintf(tmbuf,"%02d:%02d:%02d",tm_s->tm_hour,tm_s->tm_min,tm_s->tm_sec);
}


/*--------------------------------------------
Get local time in string,
  in format of:
 	Year_Mon_Day  Weekday
 The caller must ensure enough space for tmbuf.
---------------------------------------------*/
void tm_get_strday(char *tmdaybuf)
{
	time_t tm_t; /* time in seconds */
	struct tm *tm_s; /* time in struct */

	time(&tm_t);
	tm_s=localtime(&tm_t);

	sprintf(tmdaybuf,"%d-%d-%d   %s",tm_s->tm_year+1900,tm_s->tm_mon+1,tm_s->tm_mday,\
			str_weekday[tm_s->tm_wday] );
}


/* -----------------------------
 timer routine
-------------------------------*/
void tm_sigroutine(int signo)
{
	if(signo == SIGALRM)
	{
//		printf(" . tick . \n");

	/* ------- routine action every tick -------- */
#if 0	// put heavy action here is not a good idea ??????  !!!!!!
        /* get time and display */
        tm_get_strtime(tm_strbuf);
        wirteFB_str20x15(&gv_fb_dev, 0, (30<<11|45<<5|10), tm_strbuf, 60, 320-38); 
        tm_get_strday(tm_strbuf);
        symbol_string_writeFB(&gv_fb_dev, &sympg_testfont,0xffff,45,2,tm_strbuf);
        //symbol_string_writeFB(&gv_fb_dev, &sympg_testfont,0xffff,32,90,tm_strbuf);
#endif

	}

	/* restore tm_sigroutine */
	signal(SIGALRM, tm_sigroutine);
}

/*---------------------------------
set timer for SIGALRM
us: time interval in us.
----------------------------------*/
void tm_settimer(int us)
{
	/* time left before next expiration  */
	tm_val.it_value.tv_sec=0;
	tm_val.it_value.tv_usec=us;

	/* time interval for periodic timer */
	tm_val.it_interval.tv_sec=0;
	tm_val.it_interval.tv_usec=us;

	setitimer(ITIMER_REAL,&tm_val,NULL); /* NULL get rid of old time value */
}

/*-------------------------------------
set timer for SIGALRM of global tick
us: global tick time interval in us.
--------------------------------------*/
static void tm_tick_settimer(int us)
{
	/* time left before next expiration  */
	tm_tick_val.it_value.tv_sec=0;
	tm_tick_val.it_value.tv_usec=us;
	/* time interval for periodic timer */
	tm_tick_val.it_interval.tv_sec=0;
	tm_tick_val.it_interval.tv_usec=us;
	/* use real time */
	setitimer(ITIMER_REAL,&tm_tick_val,NULL); /* NULL get rid of old time value */
}

/* -------------------------------------
  global tick timer routine
--------------------------------------*/
static void tm_tick_sigroutine(int signo)
{

#if EGI_ENABLE_TICK
	if(signo == SIGALRM)
	{
		tm_tick_count+=1;
	}

	/* restore tm_sigroutine */
	signal(SIGALRM, tm_tick_sigroutine);
#endif

}

/*-------------------------------
    start egi_system tick
-------------------------------*/
void tm_start_egitick(void)
{
        tm_tick_settimer(TM_TICK_INTERVAL);
        signal(SIGALRM, tm_tick_sigroutine);
}


/*-----------------------------
    return tm_tick_count
------------------------------*/
long long unsigned int tm_get_tickcount(void)
{
	return tm_tick_count;
}

/*----- WARNING!!!! A lovely BUG :> -------
delay ms, at lease TM_TICK_INTERVAL/2000 ms

if ms<0, return.
-----------------------------------------*/
void tm_delayms(long ms)
{
#if EGI_ENABLE_TICK
	unsigned int nticks;

	if(ms < TM_TICK_INTERVAL/1000)
		nticks=TM_TICK_INTERVAL/1000;
	else
		nticks=ms*1000/TM_TICK_INTERVAL;

	long tm_now=tm_tick_count;

	while(tm_tick_count-tm_now < nticks)
	{
		usleep(1000);

	}

#else
	egi_sleep(0,0,ms);

#endif

}


/*----------  OBSELETE -------------------------------------
return:
    TRUE: in every preset time interval gap(us), or time
interval exceeds gap(us).
    FALSE: otherwise

gap:	set period between pules
t:	number of tm_pulse timer to be used.

return:
	true	pulse hits
	false	wait pulse or fails to call pulse
----------------------------------------------------------*/
static struct timeval tm_pulse_tmnew[10]={0};
static struct timeval tm_pulse_tmold[10]={0};
bool tm_pulseus(long long unsigned int gap, unsigned int t) /* gap(us) */
{
	//struct timeval tmnew,tmold;
	if(t>=10) {
		printf("tm_pulseus(): timer number out of range!\n");
		return false;
	}

	/* first init tmold */
	if(tm_pulse_tmold[t].tv_sec==0 && tm_pulse_tmold[t].tv_usec==0)
		gettimeofday(&tm_pulse_tmold[t],NULL);

	/* get current time value */
	gettimeofday(&tm_pulse_tmnew[t],NULL);

	/* compare timers */
	if(tm_pulse_tmnew[t].tv_sec*1000000+tm_pulse_tmnew[t].tv_usec >=
				 tm_pulse_tmold[t].tv_sec*1000000+tm_pulse_tmold[t].tv_usec + gap)
	{
		/* reset tmold */
		tm_pulse_tmold[t].tv_sec=0;
		tm_pulse_tmold[t].tv_usec=0;
		tm_pulse_tmnew[t]=tm_pulse_tmold[t];

		return true;
	}
	else
		return false;
}


/*---------------------------------------------------------
return time difference in us, as an unsigned value.
t_start:	start time
t_end:		end time
-----------------------------------------------------------*/
unsigned int tm_diffus(struct timeval t_start, struct timeval t_end)
{
	int ds=t_end.tv_sec-t_start.tv_sec;
	int dus=t_end.tv_usec-t_start.tv_usec;

	int td=ds*1000000+dus;

	return ( td>0 ? td : -td );
}

/*------------------------------------------------------------------
return time difference in ms, as a signed value.
tm_start:	start time
tm_end:		end time
------------------------------------------------------------------*/
int tm_signed_diffms(struct timeval tm_start, struct timeval tm_end)
{
        int time_cost;
        time_cost=(tm_end.tv_sec-tm_start.tv_sec)*1000	\
			+(tm_end.tv_usec-tm_start.tv_usec)/1000;

        return time_cost;
}

/*---------------------------------------------------------
	Use select to sleep

NOTE:
	1. In thread, it's OK. NO effect with egi timer????
	2. In Main(), it'll fail, conflict with egi timer???

----------------------------------------------------------*/
//static unsigned char gv_tm_fd[128]={0};
int egi_sleep(unsigned char fd, unsigned int s, unsigned int ms)
{
	struct timeval tmval;

	tmval.tv_sec=s;
	tmval.tv_usec=1000*ms;

	select(fd,NULL,NULL,NULL,&tmval); /* wait until timeout */

	return 0;
}

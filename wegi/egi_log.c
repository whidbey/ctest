/*--------------------------------------------------------------------------------------------
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.


Try to write a thread_safe log system. :))))

1. Run egi_init_log() first to initiliaze log porcess.
   A egi_log_thread_write() then will be running as a log checking thread, It monitors
   log_buff_count, and write log_buff[] items into log file if the counter>0.

2. Other threads just push log string into log_buff by calling egi_push_log().
   High level log string (>= LOGLV_NOBUFF_THRESHOLD) will be wirtten directly to log file without
   pushing to log_buff. So critical debug information will be recorded even if process exits abruptly
   just after egi_push_log() operation.

3. Call egi_quit_log() finally to end egi log process. Before that, you'd better
   wait for a while to let other threads finish in_hand log pushing jobs.
   Set EGI_LOG_QUITWAIT for default wait time before quit.

4. In egi_push_log() you can turn on print for the pushing log information.

5. setbuf() to NULL, so we use log_buff[] instead of system buffer.


Note:
1. log_buff_mutex will not be destroyed after egi_quit_log().
2. Log items are not written sorted by time, because of buff FILO and thread operation.
3. Give log string a '/n' and makes fprint flush immediately.

TODO:
0. TO add __FILE__, __FUNCTION__ at EGI_PLOG() macro.
1. egi_init_log() can be called only once! It's NOT reentrant!!!!
   !!! consider to destroy and re-initiliate log_buff_mutex. !!!
2. sort lof_buff by time. 	--- Not necessary
3. egi_push_log() after when egi_quit_log(), must wait for all egi_push_log(). --- Not necessary.
4. Be ware of system buffer for file write,print,..etc.
5. Fail to use access() to check file existance.

6. A rush of (big) data writing may cause SD card unmount ????


Midas Zhou
-----------------------------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h> /* va_list, va_start */
#include <pthread.h>
#include "egi_log.h"
#include "egi_timer.h"
#include "egi_utils.h"

static pthread_t log_write_thread;

/* NOT APPLIED YET!:  following variables to be mutex lock protected  by log_buff_mutex */
static pthread_mutex_t log_buff_mutex;
static FILE *egi_log_fp; /* log_buff_mutex lock */
static unsigned char **log_buff;  /* log_buff_mutex lock */
static int log_buff_count; /* count number, or number of the first available log buff,log_buffer_mutex lock */
static bool log_is_running; /* log_buff_mutex lock */
//static bool write_thread_running;


/*-----------------------------------------------------------------
Convert enum egi_log_level to string
----------------------------------------------------------------*/

#define ENUM_LOGLV_CASE(x)      case    x:  return(#x);

static inline const char *egi_loglv_to_string(enum egi_log_level log_level)
{
	switch(log_level)
	{
		ENUM_LOGLV_CASE(LOGLV_NONE);
		ENUM_LOGLV_CASE(LOGLV_INFO);
		ENUM_LOGLV_CASE(LOGLV_WARN);
		ENUM_LOGLV_CASE(LOGLV_ERROR);
		ENUM_LOGLV_CASE(LOGLV_CRITICAL);
		ENUM_LOGLV_CASE(LOGLV_TEST);
	}
	return "LOGLV_Unknown";
}

/*---------------------------------------------------------------------------------------
1. For high level log(>LOGLV_WARN),it will be written directly to log file with nobuffer.
2. Otherwise, push log string to log_buff[] and wait for write_thread to
   write to log file later.
3. Log string that exceeds length of log_buff[] will be trimmed to EGI_LOG_MAX_ITEMLEN-1.

TODO: check log string total length!!!

return:
	0	OK
	<0	Fails
	>0	Log buff is full.
--------------------------------------------------------------------------------------*/
int egi_push_log(enum egi_log_level log_level, const char *fmt, ...)
{
	char strlog[EGI_LOG_MAX_ITEMLEN]={0}; /* for temp. use */

	/* get extended parameters */
	va_list arg;
	va_start(arg,fmt);/* ----- start of extracting extended parameters to arg ... */

	/* get time stamp */
	time_t t=time(NULL);
	struct tm *tm=localtime(&t);

	/*??? may be replaced by __DATE__,__TIME__??? prepare time stamp string and log_level */
	sprintf(strlog, "[%d-%02d-%02d %02d:%02d:%02d] [%s] ",
				tm->tm_year+1900,tm->tm_mon+1,tm->tm_mday,tm->tm_hour, tm->tm_min,tm->tm_sec,
				egi_loglv_to_string(log_level) );
	//printf("time string for strlog: %s \n",strlog);

	int tmlen=strlen(strlog);

	/* push log string into temp. strlog */
	vsnprintf(strlog+tmlen, EGI_LOG_MAX_ITEMLEN-tmlen-1, fmt, arg); /* -1 for /0 */
#if ENABLE_LOGBUFF_PRINT
	printf("EGI_Logger: %s",strlog); /* no '/n', Let log caller to decide return token */
#endif
	va_end(arg); /* ----- end of extracting extended parameters ... */

#if 0///////////////////////////////////////////////////////////////////////////////////
	///////   FOR HIGHT LEVEL LOG:  write directly to log file //////
	if(log_level >= LOGLV_NOBUFF_THRESHOLD)
	{
		if( fprintf(egi_log_fp,"%s",strlog) < 0 )
		{
			printf("egi_push_log(): fail to write strlog to log file.\n");
			return -1;
		}

		return 0;
	}
#endif//////////////////////////////////////////////////////////////////////////////////

   	/* get mutex lock */
   	if(pthread_mutex_lock(&log_buff_mutex) != 0)
   	{
		printf("egi_quit_log():fail to get mutex lock.\n");
		return -1;
   	}

 /* -------------- entering critical zone ---------------- */
	/* check if log_is_running */
	if(!log_is_running)
	{
		printf("%s(): egi log is not running! try egi_init_log() first. \n",__FUNCTION__);
		pthread_mutex_unlock(&log_buff_mutex);
		return -1;
	}

	///////   FOR HIGHT LEVEL LOG:  write directly to log file //////
	if(log_level >= LOGLV_NOBUFF_THRESHOLD)
	{
		if( fprintf(egi_log_fp,"%s",strlog) < 0 )
		{
			printf("egi_push_log(): fail to write strlog to log file.\n");
			return -1;
		}

		pthread_mutex_unlock(&log_buff_mutex);

		return 0;
	}
	///////   FOR NORMAL LEVEL LOG: push to puff 		 ////////
	else
	{
		/* check if log_buff overflow */
		if(log_buff_count>EGI_LOG_MAX_BUFFITEMS-1)
		{
			printf("egi_push_log(): log_buff[] is full, fail to push strlog, unlock mutex and return...\n");
			pthread_mutex_unlock(&log_buff_mutex);
			return 1;
		}

		/* copy log string to log_buf */
	/*
		printf("egi_push_log(): start strncpy...  log_buff_count=%d, strlen(strlog)=%d\n",
										log_buff_count, strlen(strlog) );
	*/
		memset(log_buff[log_buff_count],0,EGI_LOG_MAX_ITEMLEN); /* clear buff item */
		strncpy((char *)log_buff[log_buff_count],strlog,strlen(strlog));

		/* increase count */
		log_buff_count++;
	}

	/* put mutex lock */
   	pthread_mutex_unlock(&log_buff_mutex);

 /* -------------- exiting critical zone ---------------- */

   	return 0;
}

/*--------------------------------------------
It's a thread function, running detached????
Note
1. exits when log_is_running is false.
2. flush log file before exit.
-------------------------------------------*/
static void egi_log_thread_write(void)
{
	int i;

	/* check log file */
  	if(egi_log_fp==NULL)
  	{
		printf("egi_log_thread_write(): Log file is not open, egi_log_fp is NULL.\n");
		return;
  	}
	/* check log_buff */
  	if(log_buff==NULL || log_buff[EGI_LOG_MAX_BUFFITEMS-1]==NULL)
  	{
		printf("egi_log_thread_write(): log_buff is NULL.\n");
		return;
  	}

	/* loop checking and write log buff to log file */
  	while(1)
  	{
		/* get buff lock */
		if(pthread_mutex_lock(&log_buff_mutex) != 0)
		{
			printf("egi_log_thread_write():fail to get mutex lock.\n");
			tm_delayms(25);
			continue;
			//return;
		}

	 /* -------------- entering critical zone ---------------- */

		/* write log buff items to log file */
		if(log_buff_count>0)
		{
			for(i=0;i<log_buff_count;i++)
			{
				//printf("egi_log_thread_write(): start fprintf() log_buff[%d]: %s \n",	i, log_buff[i] );
				if( fprintf(egi_log_fp,"%s",log_buff[i]) < 0 )
				{
					printf("egi_log_thread_write(): fail to write log_buff[] to log file!\n");
					/* in case that log file is corrupted */
					if( access(EGI_LOGFILE_PATH,F_OK) !=0 )
					{
						printf("egi_log_thread_write(): log file %s dose NOT exist. exit thread...\n",EGI_LOGFILE_PATH);
						/* reset running token */
						log_is_running=false;
						pthread_mutex_unlock(&log_buff_mutex);
						pthread_exit(0);
					}
				}
			}
			/* reset count */
			log_buff_count=0;
		}

		/* check log_is_running token */
		if( !log_is_running ) /* !!! log_buff[] may already have been freed then */
		{
			printf("egi_log_thread_write(): log_is_running is false, flush log file and exit pthread now...\n");
			pthread_mutex_unlock(&log_buff_mutex);
			pthread_exit(0);
		}

		/* put buff lock */
		pthread_mutex_unlock(&log_buff_mutex);

      /* -------------- exiting critical zone ---------------- */

		/* sleep for a while, let other process to get locker */
		tm_delayms(EGI_LOG_WRITE_SLEEPGAP);
   	}

	//pthread_exit(0);
}



/*-------------------------------------------
1. allocate log buff itmes
2. reset log_count;
3. start to run log_writting thread
4. set log_is_running.

return:
	0	OK
	<0	fails
-------------------------------------------*/
int egi_init_log(const char *fpath)
{
	int ret=0;

	/* 1. init buff mutex */
	if(pthread_mutex_init(&log_buff_mutex,NULL) != 0)
	{
		printf("egi_init_log(): fail to initiate log_buff_mutex.\n");
		return -1;
	}

	/* 2. malloc log buff */
//	if(egi_malloc_buff2D(&log_buff,EGI_LOG_MAX_BUFFITEMS,EGI_LOG_MAX_ITEMLEN)<0)
	log_buff=egi_malloc_buff2D(EGI_LOG_MAX_BUFFITEMS,EGI_LOG_MAX_ITEMLEN);
	if(log_buff==NULL)
	{
		printf("egi_init_log(): fail to malloc log_buff.\n");
		goto init_fail;
		return -2;
	}

	printf("egi_init_log(): finish egi_malloc_buff2D().\n");

	/* 3. reset log_buff_count */
	log_buff_count=0;

	/* 4. open log file */
	egi_log_fp=fopen(fpath,"a+");//EGI_LOGFILE_PATH,"a+");
	if(egi_log_fp==NULL)
	{
		printf("egi_init_log():fail to open log file %s\n",EGI_LOGFILE_PATH);
		ret=-3;
		goto init_fail;
	}
	/* set stream buffer as NULL, write directly without any buffer */
	setbuf(egi_log_fp,NULL);

	/* 5. set log_is_running before log_writring thread, which will refer to it.*/
	log_is_running=true;

	/* 6. run log_writting thread */
	if( pthread_create(&log_write_thread, NULL, (void *)egi_log_thread_write, NULL) !=0 )
	{
		printf("egi_init_log():fail to create pthread for log_write_thread().\n");

		log_is_running=false;
		ret=-4;
		goto init_fail;
	}
	printf("egi_init_log(): finish creating pthread log_write_thread().\n");



#if 0 /* -----------FOR TEST: test log_buf[] ------------ */
	int i;

	printf("egi_init_log(): test log_buff[] ...\n");

	for(i=0;i<EGI_LOG_MAX_BUFFITEMS;i++)
	{
		/* test log string buff, MAX. 254bytes printed for each item, 254 = 256-1'\n'-1'\0' */
		snprintf(log_buff[i],EGI_LOG_MAX_ITEMLEN-1,"---1--------------------- TEST LOG_BUFF: %03d ---\
--2------------------------------------------------\
--3------------------------------------------------\
--4------------------------------------------------\
--5---------------------------------------------0123\n",i);
		//printf("%s\n",log_buff[i]);
		//printf("strlen(log_buff[%d])=%d\n",i,strlen(log_buff[i]) ); /* 254=256-1'\n'-1'\0' */
		fprintf(egi_log_fp,"%s",log_buff[i]);
		fflush(NULL);
	}

#endif


	return 0; /* OK */


init_fail:
	pthread_mutex_destroy(&log_buff_mutex); /* destroy mutex */

	egi_free_buff2D(log_buff,EGI_LOG_MAX_BUFFITEMS);

	if(egi_log_fp != NULL)/* close file */
		fclose(egi_log_fp);

	return ret;
}

/*-------------------------------------------------------------------
set log_is_running to false to invoke egi_log_thread_write() to end.

NOte:
1. let egi_log_thread_write() to flush log file.

Return:
	0	OK
	<0	Fails
-------------------------------------------------------------------*/
static int egi_stop_log(void)
{

	/* get mutex lock */
	if(pthread_mutex_lock(&log_buff_mutex) != 0)
	{
		printf("egi_quit_log():fail to get mutex lock.\n");
		return -1;
	}
      /* -------------- entering critical zone ---------------- */
		/* reset log_is_running, to let wirte thread stop
		   ignore all log items in the log_buff. */
		log_is_running=false;
	/* put mutex lock */
	pthread_mutex_unlock(&log_buff_mutex);
      /* -------------- exiting critical zone ---------------- */

	return 0;
}


/*-------------------------------------------------------------------
free all log buff

Return:
	0	OK
	<0	Fails
-------------------------------------------------------------------*/
static int egi_free_logbuff(void)
{

	/* get mutex lock */
	if(pthread_mutex_lock(&log_buff_mutex) != 0)
	{
		printf("egi_free_logbuff():fail to get mutex lock.\n");
		return -1;
	}
      /* -------------- entering critical zone ---------------- */

	/* free log buff */
/*
	if( egi_free_buff2D(log_buff,EGI_LOG_MAX_BUFFITEMS,EGI_LOG_MAX_ITEMLEN) !=0 )
	{
		printf("egi_free_logbuff():fail to free log buff.\n");
		pthread_mutex_unlock(&log_buff_mutex);
		return -2;
	}
*/
	egi_free_buff2D(log_buff,EGI_LOG_MAX_BUFFITEMS);

	/* put mutex lock */
	pthread_mutex_unlock(&log_buff_mutex);

      /* -------------- exiting critical zone ---------------- */

	return 0;
}


/*-------------------------------------------------------------------
quit log process...

!!!! join thread of egi_log_thread_write()

??? pthread_joint() return 

Return:
	0	OK
	<0	Fails
-------------------------------------------------------------------*/
int egi_quit_log(void)
{
	int ret;

	/* wait for other threads to finish pushing inhand log string */
	tm_delayms(EGI_LOG_QUITWAIT);

	/* stop log to let egi_log_thread_write() end */
	if( egi_stop_log() !=0 )
	{
		printf("egi_quit_log():fail to stop log.\n");
		return -1;
	}

	/* wait log_write_thread to end writing buff to log file. */

	printf("egi_quit_log(): start pthread_join(log_write_thread)...\n" );
	ret=pthread_join(log_write_thread,NULL);
	if( ret !=0 )
	{
		perror("pthread_join");
		printf("egi_quit_log():fail to join log_write_thread. ret=%d\n",ret);
		return -2;
	}

	/* free log buff */
	if(egi_free_logbuff() !=0 )
	{
		printf("egi_quit_log():fail to free log_buff .\n");
		return -1;
	}

	/* close log file */
	fclose(egi_log_fp);

	return 0;
}




#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "timetag_io2.h"
#include "usbtimetagio.h"
/* default settings */
#define DEFAULT_VERBOSITY 0
#define MAX_VERBOSITY 1
#define DEFAULT_INPUT_TRESHOLD 3586 /* corresponding to approx -500mV, was 2950*/
#define MAX_INP_TRESHOLD 4095
#define DEFAULT_POLLING_INTERVAL 40 /* in milliseconds */
#define DEFAULT_OUTMODE 0
#define DEFAUT_VERBOSITY 0
#define DEFAULT_MAXEVENTS 0
#define DEFAULT_BEGINFLAG 0 /* for -r/-R option */
#define DEFAULT_SKEW 2000  /* for  -s option */
#define DEFAULT_CLOCKSOURCE 0 /* 0: internal, 1: external */
#define MAX_SKEW_VALUE 4095
#define DEFAULT_CAL 10  /* for -c option */
#define MAX_CAL_VALUE 4095
#define DEFAULT_COINC 10  /* for -c option */
#define MAX_COINC_VALUE 4095
#define DEFAULT_PHASEPATT 2  /* for -p option */
#define MAX_PHASEPATT 2
#define DEFAULT_FLUSHMODE 0 /* flush option switched off by default */
#define DEFAULT_TRAPMODE 0 /* no trapping of spurious events */
#define DEFAULT_SKIPNUM 0 /* forget no entries at beginning */
#define DEFAULT_MARKOPT 0 /* marking option to phasepattern */
#define DEFAULT_SKEWCORRECT 0 /* no skew correction, 1: 4 detectors,
				 2: 8 detectors */

/* some global variables */
int outmode=DEFAULT_OUTMODE;
int verbosity=DEFAULT_VERBOSITY;
int currentevents=0;
int maxevents=DEFAULT_MAXEVENTS;
int beginmode=DEFAULT_BEGINFLAG;
int calmode=0;
int timemode=0;
int flushmode=DEFAULT_FLUSHMODE;
int markoption=DEFAULT_MARKOPT;

/* global variables for trapping spurious events & skipping initial entries */
int trap_uval, trap_diff;
int trap_old=0;
int trap_diffavg=0; /* status variables filter */
int trap_n = 0; /* counts  events for loading filter mechanism */
int trapmode = DEFAULT_TRAPMODE;

int skipnumber = DEFAULT_SKIPNUM; /* entries at beginning to be skiped */

/* things needed for the USB device  */
#define usbtimetag_devicename "/dev/ioboards/timestamp0"

/* translation to USB uoperation: more compact patterns */

/* internal constants  */
/* 2**23 bytes can keep 1M events - this is 400 msec at a rate of
   2.5 Mevents per sec */
#define size_dma  (1<<23) /* kernel buffer size, should be multiple of 4k */
#define dmasize_in_longints (size_dma / sizeof(int))
#define dmabuf_complainwatermark (dmasize_in_longints * 4 / 5)

/*    FIXME!!!!! how does this work in usb mode? */
/* byte counting issue. For long latency times (assume 1 second) and a max
   event rate of 2^21 per sec, we have 2^24 bytes transferred. Assuming we
   want to detect unattended irqs for 2^4 secs, we need to be able to count
   2^28 bytes. Byte count, however, should stay below 2^30 to avoid integer
   sign issues. We choose 2^29 bytes as rollover, or 2^26 quads. */
#define QUADMASK 0x3ffffff /* mask for 26 bits */
/* this is to detect overflow */
#define QUADMASK2 (QUADMASK & ~(dmasize_in_longints - 1))
/* this is to mask out the rollover of the DMA buffer */
#define QUADMASK3 (dmasize_in_longints -1)

/*---------------------------------------------------------------*/
/* initiate_phasetable */
typedef struct otto {int pattern; int value;} otto;
struct otto nopattern[] =
{{-1,-1}};
struct otto defaultpattern[] =
{{6,4}, {7,5}, {12, 3}, {14, 7}, {39,4}, {136, 5}, {140,7},
 {142, 5}, {152, 8}, {156, 11}, {216, 9}, {295, 2}, {359, 1},
 {371, 15}, /* the nasty one */
 {375,1}, {472, 10}, {497, 14}, {499, 14}, {504, 12}, {505, 13}, {507, 11},
 {-1,-1}};
struct otto pattern_rev_1 [] = /* new card, tested with skew=2000 */
{{6,6}, {7,5}, {14,6}, {39,4}, {140,5}, {152,7}, {156,5}, {216,7}, {295,1},
{359,0}, {371,0}, {375,3}, {472,8}, {497,15}, {499,15}, {504,11}, {505,13},
{507,14}, {-1,-1}};
struct otto pattern_rev_2[] = /* for g2 meas at TMK's setup (3rd SG card) */
{{6,5}, {7,4}, {12, 6}, {14, 5}, {39,3}, {136, 6}, {140,6},
 {142, 6}, {152, 7}, {156, 7}, {216, 8}, {295, 2}, {359, 1},
 {371, 14}, {375,0}, /* the nasty one */
 {472, 9}, {497, 13}, {499, 13}, {504, 11}, {505, 12}, {507, 13},
 {-1,-1}};
int phasetable[512];
void initiate_phasetable(struct otto *patterntab) {
    int i;
    for (i=0;i<512;i++) phasetable[i]=0; /* clear useless events */
    for (i=0;patterntab[i].value>=0;i++) /* set the few good ones */
	phasetable[patterntab[i].pattern]=patterntab[i].value<<15;
}

/* ----------------------------------------------------------------*/
/* error handling */
char *errormessage[] = {
  "No error.",
  "Wrong verbosity level",
  "Input treshold out of range (0..4095)",
  "Illegal number of max events (must be >=0)",
  "Can't open USB timetag device driver",
  "mmap failed for DMA buffer",  /* 5 */
  "specified outmode out of range",
  "dma buffer overflow during read",
  "reached dma complainwatermark",
  "skew value out of range (0..4095)",
  "calibration value out of range (0..4095)", /* 10 */
  "coincidence value out of range (0..4095)",
  "negative number of elements to skip.",
  "marking option out of range (0, 1 or 2)",
  "wrong skew format. needs -d v1,v2,v3,v4",

};
int emsg(int code) {
  fprintf(stderr,"%s\n",errormessage[code]);
  return code;
};


/* -------- Accquring the local time ------- */
unsigned long long dayoffset_1; /* contains local time in 1/8 nsecs
				   when starting the timestamp card */
unsigned long long dayoffset[16]; /* to hold timings */

struct timeval timerequest_pointer; /*  structure to hold time requeste  */

unsigned long long my_time(void)
{     unsigned long long lret; /* fr preparing in local units */

    if (gettimeofday(&timerequest_pointer, NULL)) {
	fprintf(stderr,"gettime err in readevents; errno: %d\n",errno);
	return 0;
    }
/* this is changed to fit to the standard unix time */
/*	seconds = tp.tv_sec;
        broken = localtime(&seconds);
        tp.tv_sec = (*broken).tm_hour * 3600 + 
	    (*broken).tm_min * 60 + (*broken).tm_sec; 
*/
    lret = timerequest_pointer.tv_sec;
    lret *= 1000000;
    lret += timerequest_pointer.tv_usec;
    lret = (lret * 8000) << 15;
    return(lret);
}


/* ------------------ signal handlers --------------------------*/

/* structures for timer interrupts */
struct itimerval newtimer = {{0,0},{0,DEFAULT_POLLING_INTERVAL*1000}};
struct itimerval stoptime = {{0,0},{0,0}}; 
unsigned int controltime_coarse = 0; /* in multiples of (1<<30) nanoseconds */
unsigned int controltime_cv, controltime_dv; /* for getting actual time diff */
unsigned int controltime_getmeone = 1; /* call for a new value */
long long int avg_diff = 0; /* average difference for time tracking */

/* handler for itimer signal SIGALRM. Just restarts the timer.. */
void timer_handler(int sig) {
    static unsigned long long mt,mt2; /* buffer for current time */
    static long long int ct_ref_time,mtd;
    /* float cons = 1E-9/(1<<18); for display of difference */
    if (sig==SIGALRM) {
	setitimer(ITIMER_REAL, &newtimer,NULL); /* restart timer */

	/* new version, tied to system clock */
	mt=my_time();
	if (1) { /* exclude strange time readings */
	    mt2=mt-dayoffset_1; /* corrected by dayoffset */ 
	    ct_ref_time = ((((unsigned long long)controltime_cv<<32) +
			    controltime_dv)) ; /* in 1/8 nsec */
	    /* averaged difference between PC clock and timestamp clock */
	    mtd=(long long int)(mt2-ct_ref_time);
	    avg_diff += ((long long int)mtd-avg_diff)/300; /* avg time is 10 sec */

/*	    fprintf(stderr,"mt2: %.3f, ctr: %.3f, mtd: %.3f, avgd: %.3f\n",
	    cons*mt2,cons*ct_ref_time,cons*mtd,cons*avg_diff); */

	    /* avg_diff=0; */
	    controltime_coarse = (mt2-avg_diff)>>48; /* in 2^30 nsec */
	}
    }
    controltime_getmeone=1; /* ask for fresh timing info */
}
int handler_filehandle;
/* handler for SIGUSR1 / SIGUSR2 */
void usersig_handler(int sig) {
    switch (sig) {
	case SIGUSR1: /* start DMA */
	    trap_n=0;trap_old=0;trap_diffavg=0;
	    set_inhibit_line(handler_filehandle,0);
	    break;
	case SIGUSR2: /* stop DMA */
	    set_inhibit_line(handler_filehandle,1);
	    break;
    }
}
/* handler for termination */
int terminateflag;
void termsig_handler(int sig) {
    switch (sig) {
	case SIGTERM: case SIGKILL:
	    fprintf(stderr,"got hit by a term signal!\n");
	    terminateflag=1;
	    break;
	case SIGPIPE: 
	    /* stop acquisition */
	    fprintf(stderr,"readevents:got sigpipe\n");
	    terminateflag=1;
    }
}


/* ----------------------- processing of raw data --------------- */
/* intermediate buffer for processed data */
struct processedtime {unsigned int cv;unsigned int dv;} processedtime;
struct processedtime outbuf[dmasize_in_longints/2];
/* function to digest data from the DMA buffer. quadsprocessed and quadsread
   represent indices to 32-bit entities in the DMA buffer. return value
   is the number of processed 32-bit-entries, or <0 if a DMA buffer overflow
   occured. 
*/
int process_quads(void *sourcebuffer, int startquad, int endquad) {
  unsigned int *events;
  int startindex, endindex,i; 
  int numberofquads;
  int j;  /* processing variables */
  unsigned int ju; /* for parsing binary numbers */
  unsigned int u; /* contains events */
  /* main processing variables */
  int quadsthere;
  unsigned int cv, cvd, v1, dv, fastcnt, b0;
  char *formatstring;
  static char formatstring_1[] = "event: msl: %08x; lsl: %08x\n";
  static char formatstring_2[] = "%08x%08x\n";
  int markit=0;     /* for debugging time error */
  unsigned long long current_time;

  events = (unsigned int *)sourcebuffer;
  numberofquads = (endquad - startquad) & QUADMASK3 ; /* anticipate roll over*/

  /* what if startquad == endquad? */
  if (numberofquads == 0) return 0; /* only look for pos transfers */
  /* complain if buffer is too filled */
  if ( numberofquads > ((int) dmabuf_complainwatermark) ) {
      fprintf(stderr,"numofquads: %d, complainwm: %d\n",numberofquads, ((int) dmabuf_complainwatermark));
      return -1;
  }
  
  startindex = startquad % dmasize_in_longints;
  endindex   =   endquad % dmasize_in_longints;

  switch (outmode) {
      case 0:   /* just for simple printout */
        for (i=startindex;i!=endindex;i=((i+1) % dmasize_in_longints)) {
	    u=events[i];
	    if (verbosity) { /* long version */
		  printf("index: %04d, value: %08x :",i,u);
		  for (ju=0x80000000;ju;ju>>=1) printf("%d",((ju&u)?1:0));
		  printf("\n");
	    } else {
		printf("%08x\n",u); /* only hex code */
	    }
	}
	/* check if max event mechanism is active */
	if (maxevents) {
	    currentevents++;
	    if (currentevents==maxevents) {
		terminateflag=1;
		return numberofquads;
	    }
	}
	return numberofquads;
  
  /* do first processing  in other cases */
      case 1:case 2:
	i=startindex;
	j=0; /* start target index */ 
	for (quadsthere=numberofquads;
	     quadsthere>1;
	     quadsthere--,i=((i+1) % dmasize_in_longints)) {
	    /* extract coarse� val and check for consistency */
	    cv=events[(i+1) % dmasize_in_longints];
	    cvd=(cv>>16)-controltime_coarse+2; /* difference plus 2 */
	    /* for debugging stop */
	    if (cvd>4) {/* allow for approx 2 sec difference */
		fprintf(stderr,"timing out of range; cv=%d, control=%d, dv=%d, idx: %d\n",cv,controltime_coarse,events[i],i);
		/* continue; */
	    	if (markoption==1) markit+=0x10;
		/* FIXME: for debug: try not to realign 4-byte entities */
		quadsthere--;i=((i+1) % dmasize_in_longints);
		continue;
		} 
	    /* now we should be consistent. no mixing necessry anymore */ 
	    /* get first and second entry */
	    dv=events[(i % dmasize_in_longints)];
	    v1=((dv & 0xc000)>>12) | ((dv & 0x30000)>>16); /* event lines */
	    // v2=(events[(i+1) % dmasize_in_longints] & 0xffff);
	    fastcnt=(dv & 0x3e00)<<10;
	    if (markoption==0) 
		markit= (dv<<4); /* bring phase pattern in place */
	    /* construct lower significant u32, containing c0-c12, c_1..c_4, 
	       and the event bits in the least significant nibble.
	       order there: bit 3..0=inA..InD */
	    dv=(dv & 0xff000000) | /* bits c5..c12 */
		fastcnt | /* bits c0..c4 */
		phasetable[dv & 0x1ff] |  /* do phase interpolation */
		v1 | /* event lines */
                (markit & 0x1ff0);  /* for debugging */

	    /* repair pipelining bug */
	    if ( (fastcnt < 0x00880000)) {
		b0=dv & 0x80000000; /* remember carry */
		dv+=0x01000000;
		if (b0 && !(dv & 0x80000000)) cv++; /* eventually do carry */
	    }

	    if(timemode==1) {
		current_time = (((unsigned long long)cv) << 32) 
 		    + (unsigned long long)dv
		    /* correction for time skew of individual detectors */ 
 		    + dayoffset[dv&0xf];
		
	    	outbuf[j].cv=(unsigned int) (current_time >> 32);  
	    	outbuf[j].dv=(unsigned int) (current_time & 0xffffffff);
	    } 
	    else{
	    	outbuf[j].cv=cv; outbuf[j].dv=dv;
	    }
	    /* keep track of movin difference */
	    if (controltime_getmeone) {
		controltime_cv=cv; controltime_dv=dv;controltime_getmeone=0;
	    }

	    if (trapmode) {
		trap_uval=cv>>9; /* time in units of 8ms */
		trap_diff=trap_uval-trap_old; trap_old=trap_uval;
		if (trap_n>1024) {
		    /* test if diffference exceeds 8 avg differences */
		    if ((trap_diff<0) || ((trap_diff*32)>trap_diffavg)) {
			/* we have an exception */
			j--;
			goto dontcount;
			
		    }
		}
		trap_diffavg += trap_diff-trap_diffavg/256;
	    dontcount: trap_n++;
	    }
	    j++;
	    /* repair new index */
	    i=(i+1) % dmasize_in_longints; quadsthere-=1;
	    /* check if max event mechanism is active */
	    if (maxevents) {
		currentevents++;
		if (currentevents==maxevents) {
		    terminateflag=1;
		    return numberofquads;
		}
	    }
	}

	/* dump event */
	switch (outmode) {
	    case 1: /* output as binary values */
		if (skipnumber >= j ) {
		    skipnumber -=j; 
		} else {
		    fwrite(&outbuf[skipnumber],sizeof(struct processedtime),
			   j-skipnumber,stdout);
		    skipnumber=0;
		    if (flushmode) fflush(stdout);
		}
		break;
	    case 2: /* output as one single / separated hex pattern */
		formatstring=verbosity?formatstring_1:formatstring_2;
		if (skipnumber >= j ) {
		    skipnumber -=j; 
		} else {
		    for (i=skipnumber;i<j;i++) {
			fprintf(stdout,formatstring,
				outbuf[i].cv,outbuf[i].dv);
		    }
		    if (flushmode) fflush(stdout);
		    skipnumber=0;
		}
		break;
	}
	return numberofquads-quadsthere;
	break;
	
      case 3: case 4: case 5: /* more text */
	  
	/* old version */ 
	i=startindex;
	for (quadsthere=numberofquads;
	     quadsthere>1;
	     quadsthere--,i=((i+1) % dmasize_in_longints)) {
	    /* extract coarse val and check for consistency */
	    cv=events[(i+1) % dmasize_in_longints];
	    cvd=(cv>>16)-controltime_coarse+2; /* difference */
	    if (cvd>4) continue;/* allow for 2 sec difference */


	    /* now we should be consistent. no mixing necessry anymore */ 
	    /* get first and second entry */
	    dv=events[(i % dmasize_in_longints)];
	    v1=((dv & 0xc000)>>12) | ((dv & 0x30000)>>16); /* event lines */
	    // v2=(events[(i+1) % dmasize_in_longints] & 0xffff);
	    fastcnt=(dv & 0x3e00)<<10;
	    if (markoption==0) 
		markit= (dv<<4); /* bring phase pattern in place */
	    /* construct lower significant u32, containing c0-c12, c_1..c_4, 
	       and the event bits in the least significant nibble.
	       order there: bit 3..0=inA..InD */
	    dv=(dv & 0xff000000) | /* bits c5..c12 */
		fastcnt | /* bits c0..c4 */
		phasetable[dv & 0x1ff] |  /* do phase interpolation */
		v1 | /* event lines */
                (markit & 0x1ff0);  /* for debugging */

	    /* repair pipelining bug */
	    if ( (fastcnt < 0x00880000)) {
		b0=dv & 0x80000000; /* remember carry */
		dv+=0x01000000;
		if (b0 && !(dv & 0x80000000)) cv++; /* eventually do carry */
	    }
	    /* repair new index */
	    i=(i+1) % dmasize_in_longints; quadsthere-=1;
	    /* dump event */
	    switch (outmode) {
		case 3: /* output only phase pattern as decimal number */
		    fprintf(stdout,"%d\n",dv & 0x1ff);
		    break;
		case 4: /* output as three space-separated hex patterns for
			   msl, lsl, pattern */
		    fprintf(stdout,"%08x %08x %04x\n",cv,dv,(dv & 0x1ff));
		    break;
	        case 5: /* output as three space-separated hex patterns for
			   msl, lsl, pattern */
		    fprintf(stdout,"%d %d %d\n",cv,dv,(dv & 0x1ff));
		    break;
	    }
	    /* check if max event mechanism is active */
	    if (maxevents) {
		currentevents++;
		if (currentevents==maxevents) {
		    terminateflag=1;
		    return numberofquads;
		}
	    }
	}
	return numberofquads-quadsthere;
  }
  return -1; /* should never be reached */
}


int main(int argc, char *argv[]) {
  int opt; /* for parsing command line options */
  int verbosity_level = DEFAULT_VERBOSITY;
  int input_treshold = DEFAULT_INPUT_TRESHOLD;
  int fh; /* file handle for device file */
  unsigned char *startad=NULL;  /* pointer to DMA buffer */
  /* main loop structure */
  int overflowflag;
  int quadsread, quadsprocessed, oldquads;
  int retval;
  unsigned int bytesread=0;
  int skew_value = DEFAULT_SKEW;
  int calib_value = DEFAULT_CAL;
  int coinc_value = DEFAULT_COINC;
  int phase_patt = DEFAULT_PHASEPATT;
  int clocksource = DEFAULT_CLOCKSOURCE;
  int skewcorrectmode = DEFAULT_SKEWCORRECT;
  int dskew[8], i; /* for skew correction */
  int USBflushmode;  /* to toggle the flush mode of the firmware */
  int usberrstat=0;

  /* int *ob; */ /* for debug */

  /* --------parsing arguments ---------------------------------- */
  
  opterr=0; /* be quiet when there are no options */
  while ((opt=getopt(argc, argv, "t:q:rRAa:v:s:c:j:p:FiexS:m:d:D:")) != EOF) {
      switch(opt) {
	  case 'v': /* set verbosity level */
	      sscanf(optarg,"%d",&verbosity_level);
	      if ((verbosity_level<0) || (verbosity_level>MAX_VERBOSITY))
		  return -emsg(1);
	      break;
	  case 't': /*set treshold value */
	      sscanf(optarg,"%d",&input_treshold);
	      if ((input_treshold<0)||(input_treshold>MAX_INP_TRESHOLD))
		  return -emsg(2);
	      break;
	  case 'q': /* set max events for stopping */
	      sscanf(optarg,"%d",&maxevents);
	      if (maxevents<0) return -emsg(3);
	      break;
	  case 'a': /* set output mode */
	      sscanf(optarg,"%d",&outmode);
	      if ((outmode<0)||(outmode>5)) return -emsg(6);
	      break;
	  case 'r':
	      beginmode=0; /* starts immediate data acquisition */
	      break;
	  case 'R':
	      beginmode=1; /* goes into stoped mode after start */
	      break;
	  case 's': /* set skew value to other than default */
	      sscanf(optarg,"%d",&skew_value);
	      if ((skew_value<0)||(skew_value>MAX_SKEW_VALUE))
		  return -emsg(9);
	      break;
	  case 'j': /* set calib value and swoitch on calib mode  */
	      sscanf(optarg,"%d",&calib_value);
	      calmode=1;
	      if ((calib_value<0)||(calib_value>MAX_CAL_VALUE))
		  return -emsg(10);
	      break;
	  case 'c': /* set coincidence value to other than default */
	      sscanf(optarg,"%d",&coinc_value);
	      if ((coinc_value<0)||(coinc_value>MAX_COINC_VALUE))
		  return -emsg(11);
	      break;
	  case 'p': /* select phase pattern */
	      sscanf(optarg,"%d",&phase_patt);
	      if ((phase_patt<-1)||(phase_patt>MAX_PHASEPATT))
		  return -emsg(12);
	      break;
	  case 'A': /* set absolute time */
	      timemode=1;
	      break;
	  case 'F': /* switch flush on after every output */
	      flushmode=1;
	      break;
	  case 'i': /* internal clock */
	      clocksource = 0;
	      break;
	  case 'e': /* external clock */
	      clocksource = 1;
	      break;
	  case 'x': /* suppress erratic events */
	      trapmode = 1;
	      break;
	  case 'S': /* skip first few events */
	      sscanf(optarg,"%d",&skipnumber);
	      if (skipnumber<0) return -emsg(12);
	      break;
	  case 'm': /* defines usage of bits 4..14 in outword */
	      sscanf(optarg,"%d",&markoption);
	      if ((markoption<0) || (markoption >2)) return -emsg(13);
	      break;
	  case 'd': /* read in detector skews */
	      if (4!=sscanf(optarg,"%d,%d,%d,%d", &dskew[0],&dskew[1],
			    &dskew[2],&dskew[3])) return -emsg(14);
 	      skewcorrectmode =1;
 	      break;
	  case 'D': /* read in detector skews for 8 detectors */
	      i=sscanf(optarg,"%d,%d,%d,%d,%d,%d,%d,%d",
		       &dskew[0],&dskew[1],&dskew[2],&dskew[3],
		       &dskew[4],&dskew[5],&dskew[6],&dskew[7] );
	      if (i<4) return -emsg(14);
	      while (i<8) {
		  dskew[i]=0;i++;
	      }
 	      skewcorrectmode = 2;
 	      break;

	  default:
	      fprintf(stderr,"usage not correct. see source code.\n");
	      return -emsg(0);
      }
  }
  
  /* initiate phasetable with defaults */
  switch (phase_patt) {
      case 0:
	  initiate_phasetable(defaultpattern);
	  break;
      case 1:
	  initiate_phasetable(pattern_rev_1);
	  break;
      case 2:
	  initiate_phasetable(pattern_rev_2);
	  break;
      default:case -1:
	  initiate_phasetable(nopattern);
  }


  /* ------------- initialize hardware  ---------------------*/
  /* open device */
  fh=open(usbtimetag_devicename,  O_RDWR);
  if (fh<0) return -emsg(4);


  /* initialize DMA buffer */
  startad=mmap(NULL,size_dma,PROT_READ|PROT_WRITE, MAP_SHARED,fh,0);
  if (startad==MAP_FAILED) return -emsg(5);

  /* prepare device */
  Reset_gadget(fh);

  /* fudging: resets this the card? */
  reset_slow_counter(fh); /* make sure to start at t=0 */


  /* clear dma buffer */
  /* for (i=0;i<size_dma;i++) startad[0]=0;
     printf("DMA cleared.\n"); */

  /* do timetag hardware init */
  initialize_DAC(fh);
  initialize_rfsource(fh);
  set_DAC_channel(fh,0,coinc_value);    /* coincidence delay stage */
  set_DAC_channel(fh,1,input_treshold); /* input reference */
  set_DAC_channel(fh,2,calib_value);    /* calibration delay stage */
  set_DAC_channel(fh,3,skew_value);     /* clock skew voltage */
  /* choose 10 MHz clock source */
  if (clocksource) {
      rfsource_external_reference(fh);
  } else {
      rfsource_internal_reference(fh);
  }

  set_inhibit_line(fh,1); /* inhibit events for the moment */
  set_calibration_line(fh,calmode?0:1); /* disable calibration pulse */
  
  initialize_FIFO(fh); /* do master reset */
  handler_filehandle = fh;  /* tell irq hndler about file handle */

  /* ------------ install IPC and timer signal handlers -----------*/
  signal(SIGTERM, &termsig_handler);
  signal(SIGKILL, &termsig_handler);
  signal(SIGPIPE, &termsig_handler);

  /* external user signals to start/stop DMA */
  signal(SIGUSR1, &usersig_handler);
  signal(SIGUSR2, &usersig_handler);
  /* polling timer */
  signal(SIGALRM, &timer_handler);

  /* ------------- start acquisition - main loop ---------*/

  terminateflag=0; overflowflag=0;
  quadsprocessed=0; currentevents=0;
  
  //fifo_partial_reset(fh); /* is that necessary? */
  start_dma(fh);
  
  usleep(50);

  /* for checking timer consistency */
  controltime_coarse=0; avg_diff=0;
  controltime_cv=0;controltime_dv=0;controltime_getmeone=0;


  /* This does not go down very well here and hangs the card.
     Something wrong ? */
  // reset_slow_counter(fh); /* make sure to start at t=0 */
  dayoffset_1 = my_time();

  /* prepare dayoffset table for different detectors */
  for (i=0;i<16;i++) dayoffset[i]=dayoffset_1; /* unchanged */
  switch (skewcorrectmode) {
      case 2: /* we have 8 values */
	  dayoffset[0x3]+=(((long long int) dskew[4])<<15); /* d5, lines 1-2 */
	  dayoffset[0x6]+=(((long long int) dskew[5])<<15); /* d6, lines 2-3 */
	  dayoffset[0xc]+=(((long long int) dskew[6])<<15); /* d7, lines 3-4 */
	  dayoffset[0x9]+=(((long long int) dskew[7])<<15); /* d8, lines 4-1 */
	  /* continue with the four old ones... */
      case 1:
	  for (i=0;i<4;i++) dayoffset[1<<i]+=(((long long int) dskew[i])<<15);
	  break;
  }
  
  setitimer(ITIMER_REAL, &newtimer,NULL);

  if (!beginmode) set_inhibit_line(fh,0);  /* enable events to come in */

  /* reminder: onequad is 4 bytes in usb mode, or 2 quads per event */
  quadsread=0;oldquads=0; /* assume no bytes are read so far */
  USBflushmode=0;  /* no flushing active */
  do {
      pause();
      if (terminateflag) break;
      
      /* get number of arrived quads; all incremental, can roll over */
      bytesread=ioctl(fh,Get_transferredbytes);
      quadsread=bytesread/4; /* one quad is a 32 bit entry read 
				in by the USB unit... */
      
      /* true overflow or irq error in internal linked buffer chain */
      if (((quadsread-oldquads) &  QUADMASK2 ) || (bytesread & 0x80000000)) {
	  usberrstat=ioctl(fh, Get_errstat);
	  overflowflag=1;break;
      }
#ifdef USBFLUSHHELPER
      /* switch on flush mode if there is no data */
      if (oldquads==quadsread) {
	  if (!USBflushmode) {
	      printf("usb will flush\n");
	      usb_flushmode(fh,50); /* 500 msec */
	      USBflushmode=1;
	  }
      } else if (USBflushmode) { /* we are in this mode */
	  if (quadsread-oldquads>8) { /* we see stuff coming again */
	      printf("usb flush stop\n");
	      usb_flushmode(fh,0); /* switch off flushmode */
	      USBflushmode=0;
	  }
      }
#endif
      oldquads=quadsread;
      /* do processing */
      retval=process_quads(startad,quadsprocessed, quadsread);
      if (retval<0) {
	  overflowflag=2;
      } else {
	  quadsprocessed+=retval;
      };
      
  } while ( !terminateflag &&  !overflowflag);
  

  /* ----- the end ---------- */
  setitimer(ITIMER_REAL, &stoptime,NULL); /* switch off timer */
  set_inhibit_line(fh,1);
  stop_dma(fh);
  close(fh);
  
  /* error messages */
  switch (overflowflag) {
      case 1:
	  fprintf(stderr,
		  "bytes: %x quadsread: %x, oldquads: %x, procesed: %x\n",
		  bytesread, quadsread, oldquads, quadsprocessed);

	  fprintf(stderr,"USB error stat: %d\n",usberrstat);
	  return -emsg(7);
      case 2: return -emsg(8);
  }
  return 0;
}











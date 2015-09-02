/* source: xio-ipapp.c */
/* Copyright Gerhard Rieger */
/* Published under the GNU General Public License V.2, see file COPYING */

/* this file contains the source for TCP and UDP related options */

#include "xiosysincludes.h"

#if _WITH_TCP || _WITH_UDP

#include "xioopen.h"
#include "xio-socket.h"
#include "xio-ip.h"
#include "xio-listen.h"
#include "xio-ip6.h"
#include "xio-ipapp.h"

const struct optdesc opt_sourceport = { "sourceport", "sp",       OPT_SOURCEPORT,  GROUP_IPAPP,     PH_LATE,TYPE_USHORT,	OFUNC_SPEC };
const struct optdesc opt_sourceport_range = { "sourceport_range", NULL, OPT_SOURCEPORT_RANGE, GROUP_IPAPP, PH_LATE, TYPE_USHORT_USHORT, OFUNC_SPEC };
/*const struct optdesc opt_port = { "port",  NULL,    OPT_PORT,        GROUP_IPAPP, PH_BIND,    TYPE_USHORT,	OFUNC_SPEC };*/
const struct optdesc opt_lowport = { "lowport", NULL, OPT_LOWPORT, GROUP_IPAPP, PH_LATE, TYPE_BOOL, OFUNC_SPEC };

#if _WITH_IP4
/* we expect the form "host:port" */
int xioopen_ipapp_connect(int argc, const char *argv[], struct opt *opts,
			   int xioflags, xiofile_t *xxfd,
			   unsigned groups, int socktype, int ipproto,
			   int pf) {
   struct single *xfd = &xxfd->stream;
   int rw = (xioflags & XIO_ACCMODE);
   struct opt *opts0 = NULL;
   const char *hostname = argv[1], *portname = argv[2];
   bool dofork = false;
   union sockaddr_union us_sa,  *us = &us_sa;
   union sockaddr_union them_sa, *them = &them_sa;
   socklen_t uslen = sizeof(us_sa);
   socklen_t themlen = sizeof(them_sa);
   bool needbind = false;
   struct portrange sourceport_range_pr, *sourceport_range = &sourceport_range_pr;
   int level;
   int result;

   if (argc != 3) {
      Error2("%s: wrong number of parameters (%d instead of 2)", argv[0], argc-1);
   }

   if (applyopts_single(xfd, opts, PH_INIT) < 0)  return -1;
   applyopts(-1, opts, PH_INIT);

   retropt_bool(opts, OPT_FORK, &dofork);

   if (_xioopen_ipapp_prepare(opts, &opts0, hostname, portname, &pf, ipproto,
			      xfd->para.socket.ip.res_opts[1],
			      xfd->para.socket.ip.res_opts[0],
			      them, &themlen, us, &uslen, &needbind, &sourceport_range,
			      socktype) != STAT_OK) {
      return STAT_NORETRY;
   }

   if (dofork) {
      xiosetchilddied();	/* set SIGCHLD handler */
   }

   if (xioopts.logopt == 'm') {
      Info("starting connect loop, switching to syslog");
      diag_set('y', xioopts.syslogfac);  xioopts.logopt = 'y';
   } else {
      Info("starting connect loop");
   }

   do {	/* loop over retries and forks */

#if WITH_RETRY
      if (xfd->forever || xfd->retry) {
	 level = E_INFO;
      } else
#endif /* WITH_RETRY */
	 level = E_ERROR;

      result =
	 _xioopen_connect(xfd,
			  needbind?(struct sockaddr *)us:NULL, uslen,
			  (struct sockaddr *)them, themlen,
			  opts, pf, socktype, ipproto, sourceport_range, level);
      switch (result) {
      case STAT_OK: break;
#if WITH_RETRY
      case STAT_RETRYLATER:
      case STAT_RETRYNOW:
	 if (xfd->forever || xfd->retry) {
	    --xfd->retry;
	    if (result == STAT_RETRYLATER) {
	       Nanosleep(&xfd->intervall, NULL);
	    }
	    dropopts(opts, PH_ALL); free(opts); opts = copyopts(opts0, GROUP_ALL);
	    continue;
	 }
	 return STAT_NORETRY;
#endif /* WITH_RETRY */
      default:
	 free(opts0); free(opts);
	 return result;
      }
      if (XIOWITHWR(rw))   xfd->wfd = xfd->rfd;
      if (!XIOWITHRD(rw))  xfd->rfd = -1;

#if WITH_RETRY
      if (dofork) {
	 pid_t pid;
	 int level = E_ERROR;
	 if (xfd->forever || xfd->retry) {
	    level = E_WARN;	/* most users won't expect a problem here,
				   so Notice is too weak */
	 }
	 while ((pid = xio_fork(false, level)) < 0) {
	    if (xfd->forever || --xfd->retry) {
	       Nanosleep(&xfd->intervall, NULL); continue;
	    }
	    free(opts0);
	    return STAT_RETRYLATER;
	 }

	 if (pid == 0) {	/* child process */
	    xfd->forever = false;  xfd->retry = 0;
	    break;
	 }

	 /* parent process */
	 Notice1("forked off child process "F_pid, pid);
	 Close(xfd->rfd);

	 /* with and without retry */
	 Nanosleep(&xfd->intervall, NULL);
	 dropopts(opts, PH_ALL); free(opts); opts = copyopts(opts0, GROUP_ALL);
	 continue;	/* with next socket() bind() connect() */
      } else
#endif /* WITH_RETRY */
      {
	 break;
      }
   } while (true);
   /* only "active" process breaks (master without fork, or child) */

   if ((result = _xio_openlate(xfd, opts)) < 0) {
      free(opts0);free(opts);
      return result;
   }
   free(opts0);free(opts);
   return 0;
}


/* returns STAT_OK on success or some other value on failure
   applies and consumes the following options:
   PH_EARLY
   OPT_PROTOCOL_FAMILY, OPT_BIND, OPT_SOURCEPORT, OPT_SOURCEPORT_RANGE, OPT_LOWPORT
 */
int
   _xioopen_ipapp_prepare(struct opt *opts, struct opt **opts0,
			   const char *hostname,
			   const char *portname,
			   int *pf,
			   int protocol,
			   unsigned long res_opts0, unsigned long res_opts1,
			   union sockaddr_union *them, socklen_t *themlen,
			   union sockaddr_union *us, socklen_t *uslen,
			   bool *needbind, struct portrange **sourceport_range,
			   int socktype) {
   char infobuff[256];
   int result;
   bool use_sourceport_range = false;

   retropt_socket_pf(opts, pf);

   if ((result =
	xiogetaddrinfo(hostname, portname,
		       *pf, socktype, protocol,
		       (union sockaddr_union *)them, themlen,
		       res_opts0, res_opts1
		       ))
       != STAT_OK) {
      return STAT_NORETRY;	/*! STAT_RETRYLATER? */
   }
   if (*pf == PF_UNSPEC) {
      *pf = them->soa.sa_family;
   }

   applyopts(-1, opts, PH_EARLY);

   /* 3 means: IP address AND port accepted */
   if (retropt_bind(opts, *pf, socktype, protocol, (struct sockaddr *)us, uslen, 3,
		    res_opts0, res_opts1)
       != STAT_NOACTION) {
      *needbind = true;
   } else {
      switch (*pf) {
#if WITH_IP4
      case PF_INET:  socket_in_init(&us->ip4);  *uslen = sizeof(us->ip4); break;
#endif /* WITH_IP4 */
#if WITH_IP6
      case PF_INET6: socket_in6_init(&us->ip6); *uslen = sizeof(us->ip6); break;
#endif /* WITH_IP6 */
      }
   }

   applyopts_sourceport(opts, *sourceport_range, &use_sourceport_range);
   if (use_sourceport_range) {
      *needbind = true;
   } else {
      *sourceport_range = NULL;
   }

   *opts0 = copyopts(opts, GROUP_ALL);

   Notice1("opening connection to %s",
	   sockaddr_info((struct sockaddr *)them, *themlen, infobuff, sizeof(infobuff)));
   return STAT_OK;
}
#endif /* _WITH_IP4 */


#if _WITH_TCP && WITH_LISTEN
/*
   applies and consumes the following options:
   OPT_PROTOCOL_FAMILY, OPT_BIND
 */
int _xioopen_ipapp_listen_prepare(struct opt *opts, struct opt **opts0,
				   const char *portname, int *pf, int ipproto,
				  unsigned long res_opts0,
				  unsigned long res_opts1,
				   union sockaddr_union *us, socklen_t *uslen,
				   int socktype) {
   char *bindname = NULL;
   int result;

   retropt_socket_pf(opts, pf);

   retropt_string(opts, OPT_BIND, &bindname);
   if ((result =
	xiogetaddrinfo(bindname, portname, *pf, socktype, ipproto,
		       (union sockaddr_union *)us, uslen,
		       res_opts0, res_opts1))
       != STAT_OK) {
      /*! STAT_RETRY? */
      return result;
   }

   *opts0 = copyopts(opts, GROUP_ALL);
   return STAT_OK;
}


int xioopen_ipapp_bind(struct single *xfd,
		       struct sockaddr *them, size_t themlen,
		       union sockaddr_union *us, socklen_t *uslen,
		       struct portrange *sourceport_range,
		       int level) {
   char infobuff[256];
   int result;

#if WITH_TCP || WITH_UDP
   if (sourceport_range) {
      union sockaddr_union sin, *sinp;
      unsigned short *port, i, N;
      div_t dv;

      /* prepare sockaddr for bind probing */
      if (us) {
	 sinp = (union sockaddr_union *)us;
      } else {
	 if (them->sa_family == AF_INET) {
	    socket_in_init(&sin.ip4);
#if WITH_IP6
	 } else {
	    socket_in6_init(&sin.ip6);
#endif
	 }
	 sinp = &sin;
      }
      if (them->sa_family == AF_INET) {
	 port = &sin.ip4.sin_port;
#if WITH_IP6
      } else if (them->sa_family == AF_INET6) {
	 port = &sin.ip6.sin6_port;
#endif
      } else {
	 port = 0;	/* just to make compiler happy */
      }
      /* combine random+step variant to quickly find a free port when only
	 few are in use, and certainly find a free port in defined time even
	 if there are almost all in use */
      /* dirt 1: having tcp/udp code in socket function */
      /* dirt 2: using a time related system call for init of random */
      {
	 /* generate a random port, with millisecond random init */
#if 0
	 struct timeb tb;
	 ftime(&tb);
	 srandom(tb.time*1000+tb.millitm);
#else
	 struct timeval tv;
	 struct timezone tz;
	 tz.tz_minuteswest = 0;
	 tz.tz_dsttime = 0;
	 if ((result = Gettimeofday(&tv, &tz)) < 0) {
	    Warn2("gettimeofday(%p, {0,0}): %s", &tv, strerror(errno));
	 }
	 srandom(tv.tv_sec*1000000+tv.tv_usec);
#endif
      }
      dv = div(random(), sourceport_range->high+1 - sourceport_range->low);
      i = N = sourceport_range->low + dv.rem;
      do {	/* loop over lowport bind() attempts */
	 *port = htons(i);
	 if (Bind(xfd->rfd, (struct sockaddr *)sinp, sizeof(*sinp)) < 0) {
	    Msg4(errno==EADDRINUSE?E_INFO:level,
		 "bind(%d, {%s}, "F_socklen"): %s", xfd->rfd,
		 sockaddr_info(&sinp->soa, sizeof(*sinp), infobuff, sizeof(infobuff)),
		 sizeof(*sinp), strerror(errno));
	    if (errno != EADDRINUSE) {
	       Close(xfd->rfd);
	       return STAT_RETRYLATER;
	    }
	 } else {
	    break;	/* could bind to port, good, continue past loop */
	 }
	 --i;  if (i < sourceport_range->low)  i = sourceport_range->high;
	 if (i == N) {
	    Msg2(level, "no port available in range %hu:%hu", sourceport_range->low, sourceport_range->high);
	    /*errno = EADDRINUSE; still assigned */
	    Close(xfd->rfd);
	    return STAT_RETRYLATER;
	 }
      } while (i != N);
   } else
#endif /* WITH_TCP || WITH_UDP */

   if (us) {
      if (Bind(xfd->rfd, us, uslen) < 0) {
	 Msg4(level, "bind(%d, {%s}, "F_socklen"): %s",
	      xfd->rfd, sockaddr_info(us, uslen, infobuff, sizeof(infobuff)),
	      uslen, strerror(errno));
	 Close(xfd->rfd);
	 return STAT_RETRYLATER;
      }
   }
}

/* we expect the form: port */
/* currently only used for TCP4 */
int xioopen_ipapp_listen(int argc, const char *argv[], struct opt *opts,
			  int xioflags, xiofile_t *fd,
			 unsigned groups, int socktype,
			 int ipproto, int pf) {
   struct opt *opts0 = NULL;
   union sockaddr_union us_sa, *us = &us_sa;
   socklen_t uslen = sizeof(us_sa);
   int result;

   if (argc != 2) {
      Error2("%s: wrong number of parameters (%d instead of 1)", argv[0], argc-1);
   }

   if (pf == PF_UNSPEC) {
#if WITH_IP4 && WITH_IP6
      pf = xioopts.default_ip=='6'?PF_INET6:PF_INET;
#elif WITH_IP6
      pf = PF_INET6;
#else
      pf = PF_INET;
#endif
   }

   fd->stream.howtoshut  = XIOSHUT_DOWN;
   fd->stream.howtoclose = XIOCLOSE_CLOSE;

   if (applyopts_single(&fd->stream, opts, PH_INIT) < 0)  return -1;
   applyopts(-1, opts, PH_INIT);
   applyopts(-1, opts, PH_EARLY);

   if (_xioopen_ipapp_listen_prepare(opts, &opts0, argv[1], &pf, ipproto,
				     fd->stream.para.socket.ip.res_opts[1],
				     fd->stream.para.socket.ip.res_opts[0],
				     us, &uslen, socktype)
       != STAT_OK) {
      return STAT_NORETRY;
   }

   if ((result =
	xioopen_listen(&fd->stream, xioflags,
		       (struct sockaddr *)us, uslen,
		     opts, opts0, pf, socktype, ipproto))
       != 0)
      return result;
   return 0;
}

int _xio_ipapp_seedrand() {
   static bool done;
   if (done) return 0;


}

#endif /* _WITH_IP4 && _WITH_TCP && WITH_LISTEN */

#endif /* _WITH_TCP || _WITH_UDP */

/* thttpd.c - tiny/turbo/throttling HTTP server
**
** Copyright � 1995,1998,1999,2000,2001 by Jef Poskanzer <jef@mail.acme.com>.
** Copyright � 2012-2014 by Jean-Jacques Brucker <open-udc@googlegroups.com>.
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
** 1. Redistributions of source code must retain the above copyright
**	notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**	notice, this list of conditions and the following disclaimer in the
**	documentation and/or other materials provided with the distribution.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
** ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
** FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
** DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
** OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
** HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
** LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
** OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
** SUCH DAMAGE.
*/

#ifdef HAVE_DEFINES_H
#include "defines.h"
#endif

#include "config.h"
#include "version.h"

#include <sys/param.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/uio.h>

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <pwd.h>
#ifdef HAVE_GRP_H
#include <grp.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <syslog.h>
#ifdef TIME_WITH_SYS_TIME
#include <time.h>
#endif
#include <unistd.h>

#include <fcntl.h>

#include <locale.h>
#include <gpgme.h>
#include <regex.h>

#include "fdwatch.h"
#include "libhttpd.h"
#include "mmc.h"
#include "timers.h"
#include "match.h"
#include "peers.h"
#ifdef OPENUDC
#include "udc.h"
#endif

#ifndef SHUT_WR
#define SHUT_WR 1
#endif

#ifndef HAVE_INT64T
typedef long long int64_t;
#endif

#ifndef MAXPATHLEN
#define MAXPATHLEN 4096
#endif

char* argv0;
static int debug = 0;
static char* dir = (char*) 0;
static int hsbfield = HS_PKS_ADD_MERGE_ONLY;
static int cgi_limit = CGI_LIMIT;
#ifdef CGI_PATTERN
static char * cgi_pattern = CGI_PATTERN;
#else /* CGI_PATTERN */
static char * cgi_pattern = (char*) 0;
#endif /* CGI_PATTERN */
static char * fastcgi_pass = (char*) 0;
#ifdef SIG_EXCLUDE_PATTERN
static char * sig_pattern = SIG_EXCLUDE_PATTERN;
#else /* SIG_EXCLUDE_PATTERN */
static char * sig_pattern = (char*) 0;
#endif /* SIG_EXCLUDE_PATTERN */
static unsigned short port = DEFAULT_PORT;
static int connlimit = DEFAULT_CONNLIMIT;
static char* logfile = (char*) 0;
static char* throttlefile = (char*) 0;
static char* hostname = (char*) 0;
static char* pidfile = (char*) 0;
static char* user = DEFAULT_USER;
static char iptables_cmd[128] = "";

typedef struct {
	char* pattern;
	long max_limit, min_limit;
	long rate;
	off_t bytes_since_avg;
	int num_sending;
	} throttletab;
static throttletab* throttles;
static int numthrottles, maxthrottles;

#define THROTTLE_NOLIMIT -1

typedef struct {
	int conn_state;
	int next_free_connect;
	httpd_conn* hc;
	int tnums[MAXTHROTTLENUMS];		 /* throttle indexes */
	int numtnums;
	long max_limit, min_limit;
	time_t started_at, active_at;
	Timer* wakeup_timer;
	Timer* linger_timer;
	long wouldblock_delay;
	off_t bytes;
	off_t end_byte_index;
	off_t next_byte_index;
	} connecttab;
static connecttab* connects;
static int num_connects, max_connects, first_free_connect;
static int httpd_conn_count;

/* The connection states. */
#define CNST_FREE 0
#define CNST_READING 1
#define CNST_SENDING 2
#define CNST_PAUSING 3
#define CNST_LINGERING 4

static httpd_server* hs = (httpd_server*) 0;
int terminate = 0;
time_t start_time, stats_time;
long stats_connections;
off_t stats_bytes;
int stats_simultaneous;

static volatile int got_hup, got_usr1, got_bus, watchdog_flag;

/* contain an array "hc[pid]" to kill childs on exit */
hctab_t hctab;

/* main context (used for signing) */
gpgme_ctx_t main_gpgctx;

#ifdef CHECK_UDID2
/* regex for udid2;c */
regex_t udid2c_regex;
#endif

/* myself (peer) */
peer_t myself;

#ifdef OPENUDC
udc_key_t * udckeys;
ssize_t udckeyssize;
sync_synthesis_t udcsynth;
#endif

/* Forwards. */
static void parse_args( int argc, char** argv );
static void usage( void );
static int read_config( char* filename );
static void value_required( char* name, char* value );
static void no_value_required( char* name, char* value );
static char* e_strdup( char* oldstr );
static void read_throttlefile( char* throttlefile );
static void shut_down( void );
static int handle_newconnect( struct timeval* tvP, int listen_fd );
static void handle_read( connecttab* c, struct timeval* tvP );
static void handle_send( connecttab* c, struct timeval* tvP );
static void handle_linger( connecttab* c, struct timeval* tvP );
static int check_throttles( connecttab* c );
static void clear_throttles( connecttab* c, struct timeval* tvP );
static void update_throttles( ClientData client_data, struct timeval* nowP );
static void finish_connection( connecttab* c, struct timeval* tvP );
static void clear_connection( connecttab* c, struct timeval* tvP );
static void really_clear_connection( connecttab* c, struct timeval* tvP );
static void idle( ClientData client_data, struct timeval* nowP );
static void wakeup_connection( ClientData client_data, struct timeval* nowP );
static void linger_clear_connection( ClientData client_data, struct timeval* nowP );
static void occasional( ClientData client_data, struct timeval* nowP );
#ifdef STATS_TIME
static void show_stats( ClientData client_data, struct timeval* nowP );
#endif /* STATS_TIME */
static void logstats( struct timeval* nowP );
static void thttpd_logstats( long secs );

/* Macro to DIE */
#define DIE(code,...) do { \
	syslog( LOG_CRIT,__VA_ARGS__); \
	if ( iptables_cmd[0] != '\0' ) \
		system(iptables_cmd); \
	errx((code),__VA_ARGS__); \
	} while (0)

/* SIGTERM and SIGINT say to exit immediately. */
static void handle_term( int sig ) {
	/* Don't need to set up the handler again, since it's a one-shot. */

	shut_down();
	syslog( LOG_NOTICE, "exiting due to signal %d", sig );
	closelog();
	exit( 1 );
}

/* SIGCHLD - a child process exitted, so we need to reap the zombie */
static void
handle_chld( int sig )
	{
	const int oerrno = errno;
	pid_t pid;
	int status;

#ifndef HAVE_SIGSET
	/* Set up handler again. */
	(void) signal( SIGCHLD, handle_chld );
#endif /* ! HAVE_SIGSET */
	/* Reap defunct children until there aren't any more. */
	for (;;)
		{
		pid = waitpid( (pid_t) -1, &status, WNOHANG );
		if ( (int) pid == 0 )				/* none left */
			break;
		if ( (int) pid < 0 )
			{
			if ( errno == EINTR || errno == EAGAIN )
				continue;
			/* ECHILD shouldn't happen with the WNOHANG option,
			** but with some kernels it does anyway.  Ignore it.
			*/
			if ( errno != ECHILD )
				syslog( LOG_ERR, "child wait - %m" );
			break;
			}

		/* Note 1: here may happen a minor race bug :
		 * child may be killed earlier and following code which unset hctab.hcs[pid-hctab.pidmin]
		 * may happen BEFORE we set it.
		 * In such case shut_down() may try to kill an incorrect pid - Few chances that such pid
		 * rely on an killable existing process (remind also that thttpd/ludd don't stay as root). */
		if ( pid>=hctab.pidmin && pid<hctab.pidmax )
			/* Note 2 : here we can't no more use the hc pointer because it should have been freed */
			hctab.hcs[pid-hctab.pidmin]=(httpd_conn *)0;

		/* Decrement the CGI count.  Note that this is not accurate, since
		** each CGI can involve two or even three child processes.
		** Decrementing for each child means that when there is heavy CGI
		** activity, the count will be lower than it should be, and therefore
		** more childs will be allowed than should be.
		*/
		if ( hs != (httpd_server*) 0 )
			{
			if ( hs->cgi_count > 0 )
				/* ... If a fork is called without increased the cgi_count (done by drop_child()), which is the case for cgi_interpose_output and cgi_interpose_input */
				--hs->cgi_count;
			}
		}

	/* Restore previous errno. */
	errno = oerrno;
	}

/* SIGHUP says to re-open the log file. */
static void
handle_hup( int sig )
	{
	const int oerrno = errno;

#ifndef HAVE_SIGSET
	/* Set up handler again. */
	(void) signal( SIGHUP, handle_hup );
#endif /* ! HAVE_SIGSET */

	/* Just set a flag that we got the signal. */
	got_hup = 1;

	/* Restore previous errno. */
	errno = oerrno;
	}

/* SIGUSR1 says to exit as soon as all current connections are done. */
static void
handle_usr1( int sig )
	{
	/* Don't need to set up the handler again, since it's a one-shot. */

	if ( num_connects == 0 )
		{
		/* If there are no active connections we want to exit immediately
		** here.  Not only is it faster, but without any connections the
		** main loop won't wake up until the next new connection.
		*/
		shut_down();
		syslog( LOG_NOTICE, "exiting" );
		closelog();
		exit( 0 );
		}

	/* Otherwise, just set a flag that we got the signal. */
	got_usr1 = 1;

	/* Don't need to restore old errno, since we didn't do any syscalls. */
	}


/* SIGUSR2 says to generate the stats syslogs immediately. */
static void
handle_usr2( int sig )
	{
	const int oerrno = errno;

#ifndef HAVE_SIGSET
	/* Set up handler again. */
	(void) signal( SIGUSR2, handle_usr2 );
#endif /* ! HAVE_SIGSET */

	logstats( (struct timeval*) 0 );

	/* Restore previous errno. */
	errno = oerrno;
	}


/* SIGALRM is used as a watchdog. */
static void
handle_alrm( int sig )
	{
	const int oerrno = errno;

	/* If nothing has been happening */
	if ( ! watchdog_flag )
		{
		/* Try changing dirs to someplace we can write. */
		(void) chdir( "/tmp" );
		/* Dump core. */
		abort();
		}
	watchdog_flag = 0;

#ifndef HAVE_SIGSET
	/* Set up handler again. */
	(void) signal( SIGALRM, handle_alrm );
#endif /* ! HAVE_SIGSET */
	/* Set up alarm again. */
	(void) alarm( OCCASIONAL_TIME * 3 );

	/* Restore previous errno. */
	errno = oerrno;
	}

/* SIGBUS is a workaround for Linux 2.4.x / NFS */
static void
handle_bus( int sig )
{
	const int oerrno = errno;

#ifndef HAVE_SIGSET
	/* Set up handler again. */
	(void) signal( SIGBUS, handle_bus );
#endif /* ! HAVE_SIGSET */

	/* Just set a flag that we got the signal. */
	got_bus = 1;

	/* Restore previous errno. */
	errno = oerrno;
}

static void
re_open_logfile( void )
	{
	int logfd, logfd2;
	FILE* logfp;

	if ( (hsbfield & HS_NO_LOG ) || hs == (httpd_server*) 0 )
		return;

	/* Re-open the log file. */
	if ( logfile != (char*) 0 && strcmp( logfile, "-" ) != 0 )
		{
		syslog( LOG_NOTICE, "re-opening logfile (%.80s)", logfile );
		logfd = open( logfile, O_WRONLY|O_CREAT, 0640);
		if ( logfd < 0 )
			{
			syslog( LOG_CRIT, "open %.80s - %m", logfile );
			return;
			}
		else if ( logfd <= STDERR_FILENO )
			{
			logfd2=logfd;
		 	logfd=fcntl( logfd2, F_DUPFD, STDERR_FILENO+1 );
			close(logfd2);
			if ( logfd < 0 )
				{
				syslog( LOG_CRIT, "fcntl %.80s - %m", logfile );
				return;
				}
			}

		//logfp = freopen( logfile, "a", hs->logfp ); // Strange: this keep the fd with glibc-2.14.1-10.mga2 (32 bits) but doesn't with glibc-2.13-38 (debian, amd64) ...
		logfp = fdopen( logfd, "a");
		if ( logfp == (FILE*) 0 )
			{
			syslog( LOG_CRIT, "fdopen %.80s - %m", logfile );
			return;
			}
		(void) fcntl( logfd, F_SETFD, FD_CLOEXEC );
		fclose(hs->logfp);
		hs->logfp = logfp;
		}
	}

int
main( int argc, char** argv )
	{
	struct passwd *pwd;
	char cwd[MAXPATHLEN+1];
	FILE *logfp;
	int num_ready, cnum, i, cont;
	connecttab *c;
	httpd_conn *hc;
	struct timeval tv;
	struct stat stf;

	/* bot's key (to sign some request) */
	gpgme_key_t mygpgkey;

	gpgme_error_t gpgerr;
	gpgme_engine_info_t enginfo;

	argv0 = strrchr( argv[0], '/' );
	if ( argv0 != (char*) 0 )
		++argv0;
	else
		argv0 = argv[0];
	openlog( argv0, LOG_NDELAY|LOG_PID, LOG_FACILITY );

	/* Handle command-line arguments. */
	parse_args( argc, argv );

	/* Read zone info now, in case we chroot(). */
	tzset();

	/* "hc[pid]" */
	hctab.pidmin=getpid()+1;
	hctab.pidmax=hctab.pidmin+128;
	hctab.hcs=calloc((hctab.pidmax-hctab.pidmin),sizeof(httpd_conn *));
	if (! hctab.hcs )
		DIE( 1, "out of memory allocating %s", "hctab" );

	/* If we're root and we're going to become another user, get the uid/gid
	** now.
	*/
	if ( getuid() == 0 )
		{
		pwd = getpwnam( user );
		if ( pwd == (struct passwd*) 0 )
			DIE(1,"user %.80s not found ! %s",user,"(forget "SOFTWARE_NAME"_init.sh ?)");
		}
	else
		pwd = getpwuid(getuid());

	/* Switch directory : the one in parameters, or the $HOME of the user(setuid) if root, or $HOME/.@software@  */
	if ( dir == (char*) 0 ) {
		if ( getuid() == 0 )
			dir=(pwd->pw_dir?pwd->pw_dir:".");
		else {
			dir=NEW(char,strlen(pwd->pw_dir)+3+strlen(argv0));
			strcpy(dir,pwd->pw_dir);
			strcat(dir,"/.");
			strcat(dir,argv0);
		}
	}

	if ( chdir( dir ) < 0 )
		DIE(1,"chdir %.80s - %m %s",dir,"(forget "SOFTWARE_NAME"_init.sh ?)");

	if ( read_config(DEFAULT_CFILE) < 2 )
		/* if read_config does something: re-parse args which override it */
		parse_args( argc, argv );

#ifndef VHOSTING
	if ( hsbfield & HS_VIRTUAL_HOST ) {
		syslog( LOG_WARNING, "compiled without VHOSTING flag, vhost option (-vh) is ignored." );
		warnx( "compiled without VHOSTING flag, vhost option (-vh) is ignored." );
		hsbfield &= ~HS_VIRTUAL_HOST;
	}
#endif

	/* Log file. */
	if ( logfile != (char*) 0 )
		{
		if ( strcmp( logfile, "/dev/null" ) == 0 )
			{
			hsbfield |= HS_NO_LOG;
			logfp = (FILE*) 0;
			}
		else if ( strcmp( logfile, "-" ) == 0 )
			logfp = stdout;
		else
			{
			logfp = fopen( logfile, "a" );
			if ( logfp == (FILE*) 0 )
				DIE(1, "%.80s: %m", logfile );
			if ( logfile[0] != '/' )
				{
				syslog( LOG_WARNING, "logfile is not an absolute path, you may not be able to re-open it" );
				warnx("logfile is not an absolute path, you may not be able to re-open it");
				}
			(void) fcntl( fileno( logfp ), F_SETFD, FD_CLOEXEC );
			if ( getuid() == 0 )
				{
				/* If we are root then we chown the log file to the user we'll
				** be switching to.
				*/
				if ( fchown( fileno( logfp ), pwd->pw_uid, pwd->pw_gid ) < 0 )
					{
					syslog( LOG_WARNING, "fchown logfile: %m" );
					warnx( "fchown logfile: %m" );
					}
				}
			}
		}
	else
		logfp = (FILE*) 0;

	/* Throttle file. */
	numthrottles = 0;
	maxthrottles = 0;
	throttles = (throttletab*) 0;
	if ( throttlefile != (char*) 0 )
		read_throttlefile( throttlefile );

	/* if we are root make sure that directory is owned by the specified user */
	if ( getuid() == 0 ) {
		if (stat(".",&stf) )
			DIE(1,"stat %s - %m",dir);

		if (stf.st_uid != pwd->pw_uid) {
			syslog(LOG_WARNING,"dir \"%s/\" was not owned by %s... I DO \"chown\" !!!\n",dir,user);
			warnx("dir \"%s/\" was not owned by %s... I DO \"chown\" !!!\n",dir,user);
			if ( chown(".",pwd->pw_uid,pwd->pw_gid) )
				DIE(1,"chown %s: %m",dir);
		}
	}

	/* Get current directory. */
	if (! getcwd( cwd, sizeof(cwd) - 1 ) )
		DIE(1, "getcwd - %m" );
	if ( cwd[strlen( cwd ) - 1] != '/' )
		(void) strcat( cwd, "/" );

	if ( ! debug )
		{
		int fdnull;

		if ( logfp != stdout ) {
			(void) fclose( stdout );
		/* We're not going to use stdout, but gpgpme will crash or behave strangely
		** if we use it freely, so we need to make sure it point to /dev/null.
		*/
			fdnull=open("/dev/null", O_WRONLY);
			if (fdnull == -1 )
				DIE(1, "open %.80s - %m","/dev/null");
			else if (fdnull != STDOUT_FILENO) {
				syslog( LOG_WARNING, "unexpected file on fd %d",STDOUT_FILENO);
				warnx("unexpected file on fd %d",STDOUT_FILENO);
				close(fdnull);
			}
		}

		/* We're not going to use stdin, so close it to save a file descriptor.*/
		(void) fclose( stdin );

	switch ( fork() )
			{
			case 0:
			break;
			case -1:
			DIE(1, "fork - %m" );
			default:
			exit( 0 );
			}
#ifdef HAVE_SETSID
		/* disown our parent process */
		(void) setsid();
#endif /* HAVE_SETSID */
		}

	if ( pidfile != (char*) 0 )
		{
		/* Write the PID file. */
		FILE* pidfp = fopen( pidfile, "w" );
		if ( pidfp == (FILE*) 0 )
			DIE(1, "%.80s - %m", pidfile );
		(void) fprintf( pidfp, "%d\n", (int) getpid() );
		(void) fclose( pidfp );
		}

	/* Initialize the fdwatch package.  Have to do this before chroot,
	** if /dev/poll is used.
	*/
	max_connects = fdwatch_get_nfiles();
	if ( max_connects < 0 )
		DIE(1,"fdwatch initialization failure");
	max_connects -= SPARE_FDS;

	/* if we have to limit the number of connexion per client (and if root), set the iptables rule */
	if ( connlimit > 0 ) {
		if ( getuid() != 0 ) {
			syslog( LOG_WARNING, "Could not set a connlimit without root privileges (iptables)." );
			warnx("Could not set a connlimit without root privileges (iptables).");
		}
		else {
			int s;
			s=snprintf(iptables_cmd, sizeof(iptables_cmd),"iptables -C INPUT -p tcp --syn --dport %d -m connlimit --connlimit-above %d -j DROP 2> /dev/null",port,connlimit);
			if (s >= sizeof(iptables_cmd) || s < 1 ) {
				iptables_cmd[0]='\0';
				DIE(1,"snprintf initializing iptables connlimit rule - %m.");
			}
			s=system(iptables_cmd);
			if ( s == 0 )
				/* the rule already exist - nothing to do */
				iptables_cmd[0]='\0';
			else {
				*strstr(iptables_cmd,"2>")='\0'; /* remove the "2> /dev/null" */
				iptables_cmd[10]='A'; /* replace the parameter -C (check) by -A (add) */
				s=system(iptables_cmd);
				if ( s == 0 )
					/* success, we should clear the rule on exiting */
					iptables_cmd[10]='D'; /* replace the parameter -A (add) by -D (delete) */
				else {
					/* fail, warn and clear iptables_cmd to do nothing on exit */
					syslog( LOG_WARNING, "system(\"%s\") fail and return %d.",iptables_cmd,s);
					iptables_cmd[0]='\0';
				}
			}
		}
	}

	/* Switch to the web (public) directory. */
	if ( chdir(WEB_DIR) < 0 )
		DIE(1,"chdir %s - %m %s",WEB_DIR,"(forget "SOFTWARE_NAME"_init.sh ?)");

	/* and update cwd */
	if (! getcwd( cwd, sizeof(cwd) - 1 ) )
		DIE(1, "getcwd - %m" );
	if ( cwd[strlen( cwd ) - 1] != '/' )
		(void) strcat( cwd, "/" );

	/* Set up to catch signals. */
#ifdef HAVE_SIGSET
	(void) sigset( SIGTERM, handle_term );
	(void) sigset( SIGINT, handle_term );
	(void) sigset( SIGCHLD, handle_chld );
	(void) sigset( SIGPIPE, SIG_IGN );		  /* get EPIPE instead */
	(void) sigset( SIGHUP, handle_hup );
	(void) sigset( SIGUSR1, handle_usr1 );
	(void) sigset( SIGUSR2, handle_usr2 );
	(void) sigset( SIGALRM, handle_alrm );
	(void) sigset( SIGBUS, handle_bus );
#else /* HAVE_SIGSET */
	(void) signal( SIGTERM, handle_term );
	(void) signal( SIGINT, handle_term );
	(void) signal( SIGCHLD, handle_chld );
	(void) signal( SIGPIPE, SIG_IGN );		  /* get EPIPE instead */
	(void) signal( SIGHUP, handle_hup );
	(void) signal( SIGUSR1, handle_usr1 );
	(void) signal( SIGUSR2, handle_usr2 );
	(void) signal( SIGALRM, handle_alrm );
	(void) signal( SIGBUS, handle_bus );
#endif /* HAVE_SIGSET */
	got_hup = 0;
	got_usr1 = 0;
	got_bus = 0;
	watchdog_flag = 0;
	(void) alarm( OCCASIONAL_TIME * 3 );

	/* Initialize the timer package. */
	tmr_init();

	/* Initialize the HTTP layer.  Got to do this before giving up root,
	** so that we can bind to a privileged port.
	*/
	hs = httpd_initialize(hostname, port, cgi_pattern, fastcgi_pass,
			sig_pattern, cgi_limit, cwd, hsbfield, logfp);
	if ( hs == (httpd_server*) 0 )
		DIE(1,"Could not perform httpd initialization (%m). Exiting");

	/* Set up the occasional timer. */
	if ( tmr_create( (struct timeval*) 0, occasional, JunkClientData, OCCASIONAL_TIME * 1000L, 1 ) == (Timer*) 0 )
		DIE(1,"tmr_create(occasional) failed");

	/* Set up the idle timer. */
	if ( tmr_create( (struct timeval*) 0, idle, JunkClientData, 5 * 1000L, 1 ) == (Timer*) 0 )
		DIE(1,"tmr_create(idle) failed");

	if ( numthrottles > 0 )
		/* Set up the throttles timer. */
		if ( tmr_create( (struct timeval*) 0, update_throttles, JunkClientData, THROTTLE_TIME * 1000L, 1 ) == (Timer*) 0 )
			DIE(1,"tmr_create(update_throttles) failed");

#ifdef STATS_TIME
	/* Set up the stats timer. */
	if ( tmr_create( (struct timeval*) 0, show_stats, JunkClientData, STATS_TIME * 1000L, 1 ) == (Timer*) 0 )
		DIE(1,"tmr_create(show_stats) failed");
#endif /* STATS_TIME */
	start_time = stats_time = time( (time_t*) 0 );
	stats_connections = 0;
	stats_bytes = 0;
	stats_simultaneous = 0;

	/* If we're root, try to become someone else. */
	if ( getuid() == 0 ) {
		/* Set aux groups to null. */
		if ( setgroups( 0, (const gid_t*) 0 ) < 0 )
			DIE(1,"setgroups - %m");
		/* Set primary group. */
		if ( setgid( pwd->pw_gid ) < 0 )
			DIE(1,"setgid - %m");
		/* Try setting aux groups correctly - not critical if this fails. */
		if ( initgroups( user, pwd->pw_gid ) < 0 ) {
			syslog( LOG_WARNING, "initgroups - %m" );
			warnx("initgroups - %m");
			}
		/* Set uid. */
		if ( setuid( pwd->pw_uid ) < 0 )
			DIE(1, "setuid - %m" );
		/* Setenv(HOME) (for gpgme) . */
		if ( setenv("HOME",pwd->pw_dir,1) < 0 )
			DIE(1, "setenv - %m" );
	}

#ifdef CHECK_UDID2
	if (regcomp(&udid2c_regex, "^udid2;c;[A-Z]{1,20};[A-Z-]{1,20};[0-9-]{10};[0-9.e+-]{14};[0-9]+", REG_EXTENDED|REG_NOSUB))
		DIE(1,"Could not compile regex 'udid2;c...' :-(");
#endif

#ifdef OPENUDC
	/* Read keys and keys levels */
	if ( (udckeyssize=udc_read_keys("udc/"CURRENCY_CODE"/keys",&udckeys)) < 1 )
		DIE(1,"%s: %m :-( %s","udc/"CURRENCY_CODE"/keys","(forget "SOFTWARE_NAME"_init.sh ?)");
	qsort(udckeys,udckeyssize,sizeof(udc_key_t),(int (*)(const void *, const void *))udc_cmp_keys);
	if ( udc_check_dupkeys(udckeys,udckeyssize) )
		DIE(1,"%s: found a duplicate key ! :-(","udc/"CURRENCY_CODE"/keys");
	if ( udc_write_keys("udc/"CURRENCY_CODE"/.keys",udckeys,udckeyssize) != udckeyssize
			|| rename("udc/"CURRENCY_CODE"/.keys","udc/"CURRENCY_CODE"/keys") != 0 )
		DIE(1,"%s: %m (not all key was written)","udc/"CURRENCY_CODE"/.keys");

	/* Read/update synthesis file */
	if (udc_read_synthesis("udc/"CURRENCY_CODE"/synthesis",&udcsynth) < 1 ) {
		syslog( LOG_WARNING,"%s: %s","udc/"CURRENCY_CODE"/synthesis","unexpected data, will be (re)generated");
		warnx("%s: %s","udc/"CURRENCY_CODE"/synthesis","unexpected data, will be (re)generated");
		udcsynth.nupdates=0;
		udc_update_synthesis(udckeys, udckeyssize, &udcsynth);
		if ( udc_write_synthesis("udc/"CURRENCY_CODE"/.synthesis",&udcsynth) != udckeyssize
				|| rename("udc/"CURRENCY_CODE"/.synthesis","udc/"CURRENCY_CODE"/synthesis") != 0 )
			DIE(1,"%s: %m (unable to write synthesis)","udc/"CURRENCY_CODE"/.synthesis");
	}

#endif

	/* Check gpgme version ( http://www.gnupg.org/documentation/manuals/gpgme/Library-Version-Check.html )*/
	setlocale(LC_ALL, "");
	if ( ! gpgme_check_version(GPGME_VERSION_MIN) ) {
		syslog( LOG_WARNING,"gpgme library (%s) is older than required (%s), bug may settle...",gpgme_check_version(0),GPGME_VERSION_MIN);
		warnx("gpgme library (%s) is older than required (%s), bug may settle...",gpgme_check_version(0),GPGME_VERSION_MIN);
	}
	gpgme_set_locale (NULL, LC_CTYPE, setlocale (LC_CTYPE, NULL));
#ifdef LC_MESSAGES
	gpgme_set_locale (NULL, LC_MESSAGES, setlocale (LC_MESSAGES, NULL));
#endif
	/* check for OpenPGP support */
	gpgerr=gpgme_engine_check_version(GPGME_PROTOCOL_OpenPGP);
	if ( gpgerr  != GPG_ERR_NO_ERROR )
		DIE(1,"gpgme_engine_check_version - %s",gpgme_strerror(gpgerr));

	/* for header dates: to be compatible with RFC 2822 */
	setlocale(LC_TIME,"C");

	/* set homedir of gpg engine */
	gpgerr = gpgme_get_engine_info(&enginfo);
	if ( gpgerr == GPG_ERR_NO_ERROR )
		gpgerr = gpgme_set_engine_info(GPGME_PROTOCOL_OpenPGP, enginfo->file_name,"../gpgme");
	if ( gpgerr != GPG_ERR_NO_ERROR )
		DIE(1,"gpgme_..._engine_info :-( %s",gpgme_strerror(gpgerr));

	/* create context */
	gpgerr = gpgme_new(&main_gpgctx);
	if ( gpgerr != GPG_ERR_NO_ERROR )
		DIE(1,"gpgme_new - %s",gpgme_strerror(gpgerr));

	/* add GPGME_KEYLIST_MODE_SIGS to the keylist mode */
	gpgerr = gpgme_set_keylist_mode(main_gpgctx,gpgme_get_keylist_mode(main_gpgctx)|GPGME_KEYLIST_MODE_SIGS|GPGME_KEYLIST_MODE_SIG_NOTATIONS);
	if ( gpgerr != GPG_ERR_NO_ERROR )
		DIE(1,gpgme_strerror(gpgerr));

	/* get the bot  key */
	if ( ! myself.fpr || strlen(myself.fpr) < 8 )
		DIE( 1, "Invalid fingerprint %s","(forget "SOFTWARE_NAME"_init.sh ?)");

	gpgerr = gpgme_get_key (main_gpgctx,myself.fpr,&mygpgkey,1);
	if ( gpgerr != GPG_ERR_NO_ERROR ) {
		DIE(1,"gpgme_get_key(%s) - %s",myself.fpr,gpgme_strerror(gpgerr));
	} else if ( mygpgkey->revoked ) {
		DIE(1,"key %s is revoked",mygpgkey->uids->uid);
	} else if ( mygpgkey->expired ) {
		DIE(1,"key %s is expired",mygpgkey->uids->uid);
	} else if ( mygpgkey->disabled ) {
		DIE(1,"key %s is disabled",mygpgkey->uids->uid);
	} else if ( mygpgkey->invalid ) {
		DIE(1,"key %s is invalid",mygpgkey->uids->uid);
	} else if (! mygpgkey->can_sign ) {
		DIE(1,"key %s can not sign",mygpgkey->uids->uid);
	/* TODO: Parse all uids instead of first one only */
	} else if ( (!mygpgkey->uids->comment) || strncmp(mygpgkey->uids->comment,"ubot1;",sizeof("ubot1"))
#ifdef CHECK_UDID2
			|| regexec(&udid2c_regex,mygpgkey->uids->comment+sizeof("ubot1"), 0, NULL, 0)
#endif
		) {
		DIE(1,"%s's key doesn't contain a valid ubot1 (%s)",mygpgkey->uids->name,mygpgkey->uids->comment);
	}

	/* The main context is for signing, put the key in and set armor */
	gpgme_set_armor(main_gpgctx,1);
	gpgerr = gpgme_signers_add(main_gpgctx, mygpgkey);
	if ( gpgerr  != GPG_ERR_NO_ERROR )
		DIE(1,"gpgme_signers_add - %s",gpgme_strerror(gpgerr));

	/* Check that bot's key is signed by owner */
	{
		gpgme_key_sig_t sigs;
		gpgme_key_t sigkey;
		int found=0;
		int idlen=strlen(mygpgkey->uids->comment+sizeof("ubot1"));

		/* First recal gpgme_get_key but from public keyring to get signatures (little bug of gpg version < 2.1) */
		gpgerr = gpgme_get_key (main_gpgctx,myself.fpr,&mygpgkey,0);
		if ( gpgerr != GPG_ERR_NO_ERROR )
			DIE(1,"gpgme_get_key(%s) - %s",myself.fpr,gpgme_strerror(gpgerr));

		sigs=mygpgkey->uids->signatures;
		while (sigs) {
			//warnx("sig: %s",sigs->uid);
			if ( !strcmp(mygpgkey->uids->uid,sigs->uid) ) { /* selfsig */
				sigs=sigs->next;
				continue;
			}
			if (gpgme_get_key(main_gpgctx,sigs->keyid,&sigkey,0) == GPG_ERR_NO_ERROR) {
				gpgme_user_id_t gpguids=sigkey->uids;

				while (gpguids) {
					if (!strncmp(gpguids->comment,mygpgkey->uids->comment+sizeof("ubot1"),idlen)) {
						/* We have found the ubot1 owner */
						found=1;
						warnx("owner's key fingerprint: %s",sigkey->subkeys->fpr);
						syslog(LOG_INFO,"owner's key fingerprint: %s",sigkey->subkeys->fpr);
						break;
					}
					gpguids=gpguids->next;
				}
				gpgme_key_unref(sigkey);
			}
			if (found)
				break;
			sigs=sigs->next;
		}

		if (! found) {
			warnx(SOFTWARE_NAME" certificate isn't certified by it's human owner... (=> "SOFTWARE_NAME" signatures are useless)");
			syslog(LOG_WARNING,SOFTWARE_NAME" certificate isn't certified by it's human owner... (=> "SOFTWARE_NAME" signatures are useless)");
		}
	}

	gpgme_key_unref(mygpgkey);

	/* Initialize our connections table. */
	connects = NEW( connecttab, max_connects );
	if ( connects == (connecttab*) 0 )
		DIE( 1, "out of memory allocating %s", "a connecttab" );
	for ( cnum = 0; cnum < max_connects; ++cnum )
		{
		connects[cnum].conn_state = CNST_FREE;
		connects[cnum].next_free_connect = cnum + 1;
		connects[cnum].hc = (httpd_conn*) 0;
		}
	connects[max_connects - 1].next_free_connect = -1;		/* end of link list */
	first_free_connect = 0;
	num_connects = 0;
	httpd_conn_count = 0;

	if ( hs != (httpd_server*) 0 )
		for ( i=0 ; hs->listen_fds[i]>=0 ; i++ )
				fdwatch_add_fd( hs->listen_fds[i], (void*) 0, FDW_READ );

	/* We will now only use syslog if some errors happen, so close stderr */
	if ( debug )
		warnx("started successfully ! (pid [%d], foreground/debug mode, usable env. var.: GPGME_DEBUG )",getpid());
	else {
		int fdnull;
		warnx("started successfully ! (pid [%d], messages are now sent to syslog only)",getpid());
		fclose( stderr );
		// Alas, gpgpme seems using STDIN, STDOUT and STDERR and will crash or behave strangely if we use them freely, so we need to make sure STDERR -> /dev/null
		fdnull=open("/dev/null", O_WRONLY);
		if (fdnull == -1 )
			DIE(1, "open %.80s - %m","/dev/null");
		else if (fdnull != STDERR_FILENO) {
			syslog( LOG_WARNING, "unexpected file on fd %d",STDERR_FILENO);
			close(fdnull);
		}
	 }

	/* Main loop. */
	(void) gettimeofday( &tv, (struct timezone*) 0 );
	while ( ( ! terminate ) || num_connects > 0 )
		{
		/* Do we need to re-open the log file? */
		if ( got_hup )
			{
			re_open_logfile();
			got_hup = 0;
			}

		/* Do the fd watch. */
		num_ready = fdwatch( tmr_mstimeout( &tv ) );
		if ( num_ready < 0 )
			{
			if ( errno == EINTR || errno == EAGAIN )
				continue;	   /* try again */
			syslog( LOG_ERR, "fdwatch - %m" );
			exit( 1 );
			}
		(void) gettimeofday( &tv, (struct timezone*) 0 );

		if ( num_ready == 0 )
			{
			/* No fd's are ready - run the timers. */
			tmr_run( &tv );
			continue;
			}
		//if (tv.tv_sec%86400 < 600)... /* (just an idea if need to launch daily jobs) */

		/* Is it a new connection? */
		if ( hs != (httpd_server*) 0 ) {
			cont=0;
			for ( i=0 ; hs->listen_fds[i]>=0 ; i++ ) {
				if ( fdwatch_check_fd( hs->listen_fds[i] ) && handle_newconnect( &tv, hs->listen_fds[i] ) ) {
					cont=1;
					break;
				/* Go around the loop and do another fdwatch, rather than
				** dropping through and processing existing connections.
				** New connections always get priority.
				*/
				}
			}
			if (cont)
				continue;
		}

		/* Find the connections that need servicing. */
		while ( ( c = (connecttab*) fdwatch_get_next_client_data() ) != (connecttab*) -1 )
			{
			if ( c == (connecttab*) 0 )
				continue;
			hc = c->hc;
			if ( ! fdwatch_check_fd( hc->conn_fd ) )
				/* Something went wrong. */
				clear_connection( c, &tv );
			else
				switch ( c->conn_state )
					{
					case CNST_READING: handle_read( c, &tv ); break;
					case CNST_SENDING: handle_send( c, &tv ); break;
					case CNST_LINGERING: handle_linger( c, &tv ); break;
					}
			}
		tmr_run( &tv );

		if ( got_usr1 && ! terminate )
			{
			terminate = 1;
			if ( hs != (httpd_server*) 0 )
				{
				for ( i=0 ; hs->listen_fds[i]>=0 ; i++ )
						fdwatch_del_fd( hs->listen_fds[i] );
				httpd_unlisten( hs );
				}
			}

		/* From handle_send()/writev; see handle_sigbus(). */
		if (got_bus)
		{
			syslog( LOG_WARNING, "SIGBUS received - stale NFS-handle?" );
			got_bus = 0;
		}
		}

	/* The main loop terminated. */
	shut_down();
	syslog( LOG_NOTICE, "exiting" );
	closelog();
	exit( 0 );
	}


static void
parse_args( int argc, char** argv )
	{
	int argn;
	argn = 1;
	while ( argn < argc && argv[argn][0] == '-' )
		{
		if ( !strcmp( argv[argn], "-V" ) || !strcmp( argv[argn], "--version" ) )
			{
			(void) printf( "%s\n", SOFTWARE_NAME"/"SOFTWARE_VERSION );
			exit( 0 );
			}
		else if ( strcmp( argv[argn], "-C" ) == 0 && argn + 1 < argc )
			{
			++argn;
			if (read_config(argv[argn]) > 1 )
				warnx("Config file has already been read");
			}
		else if ( strcmp( argv[argn], "-p" ) == 0 && argn + 1 < argc )
			{
			++argn;
			port = (unsigned short) atoi( argv[argn] );
			}
		else if ( strcmp( argv[argn], "-d" ) == 0 && argn + 1 < argc )
			{
			++argn;
			dir = argv[argn];
			}
		else if ( strcmp( argv[argn], "-L" ) == 0 && argn + 1 < argc )
			{
			++argn;
			connlimit = (unsigned short) atoi( argv[argn] );
			}
		else if ( strcmp( argv[argn], "-nk" ) == 0 )
			{
			hsbfield &= ~HS_PKS_ADD_MERGE_ONLY;
			}
		else if ( strcmp( argv[argn], "-vh" ) == 0 )
			{
			hsbfield |= HS_VIRTUAL_HOST;
			}
		else if ( strcmp( argv[argn], "-u" ) == 0 && argn + 1 < argc )
			{
			++argn;
			user = argv[argn];
			}
#ifdef CGI_PATTERN
		else if ( strcmp( argv[argn], "-c" ) == 0 && argn + 1 < argc )
			{
			++argn;
			cgi_pattern = argv[argn];
			}
		else if ( strcmp( argv[argn], "-F" ) == 0 && argn + 1 < argc )
			{
			++argn;
			fastcgi_pass = argv[argn];
			}
#endif /* CGI_PATTERN */
#ifdef SIG_EXCLUDE_PATTERN
		else if ( strcmp( argv[argn], "-s" ) == 0 && argn + 1 < argc )
			{
			++argn;
			sig_pattern = argv[argn];
			}
#endif /* SIG_EXCLUDE_PATTERN */
		else if ( strcmp( argv[argn], "-t" ) == 0 && argn + 1 < argc )
			{
			++argn;
			throttlefile = argv[argn];
			}
		else if ( strcmp( argv[argn], "-H" ) == 0 && argn + 1 < argc )
			{
			++argn;
			hostname = argv[argn];
			}
		else if ( strcmp( argv[argn], "-E" ) == 0 && argn + 1 < argc )
			{
			++argn;
			myself.ehost = argv[argn];
			}
		else if ( strcmp( argv[argn], "-e" ) == 0 && argn + 1 < argc )
			{
			++argn;
			myself.eport = (unsigned short) atoi( argv[argn] );
			}
		else if ( strcmp( argv[argn], "-f" ) == 0 && argn + 1 < argc )
			{
			++argn;
			myself.fpr = argv[argn];
			}
		else if ( strcmp( argv[argn], "-l" ) == 0 && argn + 1 < argc )
			{
			++argn;
			logfile = argv[argn];
			}
		else if ( strcmp( argv[argn], "-i" ) == 0 && argn + 1 < argc )
			{
			++argn;
			pidfile = argv[argn];
			}
		else if ( strcmp( argv[argn], "-D" ) == 0 )
			debug = 1;
		else
			usage();
		++argn;
		}
	if ( argn != argc )
		usage();
	}


static void
usage( void )
	{
		(void) fprintf( stderr,
				"Usage: %s [options]\n"
				"Options:\n"
				"	-u USER     user to switch to (if started as root) - default: "DEFAULT_USER"\n"
				"	-d DIR      running directory - default: %s's home or $HOME/."SOFTWARE_NAME"/\n"
				"	-C FILE     config file to use - default: "DEFAULT_CFILE" in running directory\n"
				"	-p PORT     listenning port - default: %d\n"
				"	-H HOST     host name or address to bind to - default: all available\n"
#ifdef VHOSTING
				"	-vh         enable virtual hosting\n"
#endif /* VHOSTING */
#if DEFAULT_CONNLIMIT > 0
				"	-L LIMIT    maximum simultaneous connexion per client (if started as root) - default: %d\n"
#else /* DEFAULT_CONNLIMIT > 0 */
				"	-L LIMIT    maximum simultaneous connexion per client (if started as root) - default: no limit\n"
#endif /* DEFAULT_CONNLIMIT > 0 */
#ifdef CGI_PATTERN
				"	-c CGIPAT   pattern for CGI programs - default: "CGI_PATTERN"\n"
				"	-F SOCKET   Remote or \"unix:\" socket to pass fastcgi - default: fastcgi disabled\n"
#endif /* CGI_PATTERN */
#ifdef SIG_EXCLUDE_PATTERN
				"	-s SIG!PAT  pattern disabling signed responses - default: "SIG_EXCLUDE_PATTERN"\n"
#endif /* SIG_EXCLUDE_PATTERN */
				"	-t FILE     file of throttle settings - default: no throtlling\n"
				"	-l LOGFILE  file for logging - default: via syslog()\n"
				"	-i PIDFILE  file to write the process-id to\n"
				"	-nk         new (unknow) keys may be added in our keyring through \"pks/add\"\n"
				"	-e PORT     external port (to be reach by peers) - default: listenning port\n"
				"	-E HOST     external host name or IP adress - default: default hostname\n"
				"	-fpr KeyID  fingerprint of the "SOFTWARE_NAME"'s OpenPGP key - no default, MANDATORY\n"
				"	-V          show version and exit\n"
				"	-D          stay in foreground (usefull to debug or monitor)\n"
			, argv0, user, DEFAULT_PORT
#if DEFAULT_CONNLIMIT > 0
			, DEFAULT_CONNLIMIT
#endif /* DEFAULT_CONNLIMIT > 0 */
			);
		exit( 1 );
	}

/*! read_config read once a configuration file
 * This function doesn't nothing after being called once
 *\return the number it has been called.
*/
static int read_config( char* filename )
	{
	FILE* fp;
	char line[10000];
	char* cp;
	char* cp2;
	char* name;
	char* value;
	static int nbcalls=0;

	if (nbcalls>0)
		return(++nbcalls);

	fp = fopen( filename, "r" );
	if ( fp == (FILE*) 0 )
		DIE(1,"%s: %m :-( %s",filename,"(forget "SOFTWARE_NAME"_init.sh ?)");

	while ( fgets( line, sizeof(line), fp ) != (char*) 0 )
		{
		/* Trim comments. */
		if ( ( cp = strchr( line, '#' ) ) != (char*) 0 )
			*cp = '\0';

		/* Skip leading whitespace. */
		cp = line;
		cp += strspn( cp, " \t\n\r" );

		/* Split line into words. */
		while ( *cp != '\0' )
			{
			/* Find next tab. */
			cp2 = cp + strcspn( cp, "\t\n\r" );
			/* Insert EOS and advance next-word pointer. */
			while ( *cp2 == '\t' || *cp2 == '\n' || *cp2 == '\r' )
				*cp2++ = '\0';
			/* Split into name and value. */
			name = cp;
			value = strchr( name, '=' );
			if ( value != (char*) 0 )
				*value++ = '\0';
			/* Interpret. */
			if ( strcasecmp( name, "debug" ) == 0 )
				{
				no_value_required( name, value );
				debug = 1;
				}
			else if ( strcasecmp( name, "port" ) == 0 )
				{
				value_required( name, value );
				port = (unsigned short) atoi( value );
				}
			else if ( strcasecmp( name, "dir" ) == 0 )
				{
				value_required( name, value );
				dir = e_strdup( value );
				}
			else if ( strcasecmp( name, "connlimit" ) == 0 )
				{
				value_required( name, value );
				connlimit = (unsigned short) atoi( value );
				}
			else if ( strcasecmp( name, "newkeys" ) == 0 )
				{
				no_value_required( name, value );
				hsbfield &= ~HS_PKS_ADD_MERGE_ONLY;
				}
			else if ( strcasecmp( name, "vhost" ) == 0 )
				{
				no_value_required( name, value );
				hsbfield |= HS_VIRTUAL_HOST;
				}
#ifdef CGI_PATTERN
			else if ( strcasecmp( name, "cgipat" ) == 0 )
				{
				value_required( name, value );
				cgi_pattern = e_strdup( value );
				}
			else if ( strcasecmp( name, "fastcgipass" ) == 0 )
				{
				value_required( name, value );
				fastcgi_pass = e_strdup( value );
				}
#endif /* CGI_PATTERN */
			else if ( strcasecmp( name, "cgilimit" ) == 0 )
				{
				value_required( name, value );
				cgi_limit = atoi( value );
				}
			else if ( strcasecmp( name, "throttles" ) == 0 )
				{
				value_required( name, value );
				throttlefile = e_strdup( value );
				}
			else if ( strcasecmp( name, "host" ) == 0 )
				{
				value_required( name, value );
				hostname = e_strdup( value );
				}
			else if ( strcasecmp( name, "logfile" ) == 0 )
				{
				value_required( name, value );
				logfile = e_strdup( value );
				}
			else if ( strcasecmp( name, "pidfile" ) == 0 )
				{
				value_required( name, value );
				pidfile = e_strdup( value );
				}
			else if ( strcasecmp( name, "fpr" ) == 0 ) {
				value_required( name, value );
				myself.fpr = e_strdup( value );
			}
			else if ( strcasecmp( name, "ehost" ) == 0 ) {
				value_required( name, value );
				myself.ehost = e_strdup( value );
			}
			else if ( strcasecmp( name, "eport" ) == 0 ) {
				value_required( name, value );
				myself.eport = (unsigned short) atoi( value );
			}
#ifdef SIG_EXCLUDE_PATTERN
			else if ( strcasecmp( name, "sigpat" ) == 0 )
				{
				value_required( name, value );
				sig_pattern = e_strdup( value );
				}
#endif /* SIG_EXCLUDE_PATTERN */
			else
				{
				(void) fprintf(
					stderr, "%s: unknown config option '%s'\n", argv0, name );
				exit( 1 );
				}

			/* Advance to next word. */
			cp = cp2;
			cp += strspn( cp, " \t\n\r" );
			}
		}

	(void) fclose( fp );
	return(++nbcalls);
	}


static void
value_required( char* name, char* value )
	{
	if ( value == (char*) 0 )
		{
		(void) fprintf(
			stderr, "%s: value required for %s option\n", argv0, name );
		exit( 1 );
		}
	}


static void
no_value_required( char* name, char* value )
	{
	if ( value != (char*) 0 )
		{
		(void) fprintf(
			stderr, "%s: no value required for %s option\n",
			argv0, name );
		exit( 1 );
		}
	}


static char*
e_strdup( char* oldstr )
	{
	char* newstr;

	newstr = strdup( oldstr );
	if ( newstr == (char*) 0 )
		DIE(1,"strdup %s :-( %m",oldstr);
	return newstr;
	}


static void
read_throttlefile( char* throttlefile )
	{
	FILE* fp;
	char buf[5000];
	char* cp;
	int len;
	char pattern[5000];
	long max_limit, min_limit;
	struct timeval tv;

	fp = fopen( throttlefile, "r" );
	if ( fp == (FILE*) 0 )
		{
		syslog( LOG_CRIT, "%.80s - %m", throttlefile );
		perror( throttlefile );
		exit( 1 );
		}

	(void) gettimeofday( &tv, (struct timezone*) 0 );

	while ( fgets( buf, sizeof(buf), fp ) != (char*) 0 )
		{
		/* Nuke comments. */
		cp = strchr( buf, '#' );
		if ( cp != (char*) 0 )
			*cp = '\0';

		/* Nuke trailing whitespace. */
		len = strlen( buf );
		while ( len > 0 &&
				( buf[len-1] == ' ' || buf[len-1] == '\t' ||
				  buf[len-1] == '\n' || buf[len-1] == '\r' ) )
			buf[--len] = '\0';

		/* Ignore empty lines. */
		if ( len == 0 )
			continue;

		/* Parse line. */
		if ( sscanf( buf, " %4900[^ \t] %ld-%ld", pattern, &min_limit, &max_limit ) == 3 )
			{}
		else if ( sscanf( buf, " %4900[^ \t] %ld", pattern, &max_limit ) == 2 )
			min_limit = 0;
		else
			{
			syslog( LOG_CRIT,
				"unparsable line in %.80s - %.80s", throttlefile, buf );
			(void) fprintf( stderr,
				"%s: unparsable line in %.80s - %.80s\n",
				argv0, throttlefile, buf );
			continue;
			}

		/* Nuke any leading slashes in pattern. */
		if ( pattern[0] == '/' )
			(void) strcpy( pattern, &pattern[1] );
		while ( ( cp = strstr( pattern, "|/" ) ) != (char*) 0 )
			(void) strcpy( cp + 1, cp + 2 );

		/* Check for room in throttles. */
		if ( numthrottles >= maxthrottles )
			{
			if ( maxthrottles == 0 )
				{
				maxthrottles = 100;	 /* arbitrary */
				throttles = NEW( throttletab, maxthrottles );
				}
			else
				{
				maxthrottles *= 2;
				throttles = RENEW( throttles, throttletab, maxthrottles );
				}
			if ( throttles == (throttletab*) 0 )
				DIE( 1, "out of memory allocating %s", "a throttletab" );
			}

		/* Add to table. */
		throttles[numthrottles].pattern = e_strdup( pattern );
		throttles[numthrottles].max_limit = max_limit;
		throttles[numthrottles].min_limit = min_limit;
		throttles[numthrottles].rate = 0;
		throttles[numthrottles].bytes_since_avg = 0;
		throttles[numthrottles].num_sending = 0;

		++numthrottles;
		}
	(void) fclose( fp );
	}


static void
shut_down( void )
	{
	int cnum, i;
	struct timeval tv;

	/* childs's gentle kill */
	for ( cnum = hctab.pidmin; cnum < hctab.pidmax; ++cnum )
		if (hctab.hcs[cnum-hctab.pidmin]) {
			syslog( LOG_ERR, "killed child process %d", cnum );
			kill( cnum, SIGINT );
		}

	(void) gettimeofday( &tv, (struct timezone*) 0 );
	logstats( &tv );

	for ( cnum = 0; cnum < max_connects; ++cnum )
		{
		if ( connects[cnum].conn_state != CNST_FREE )
			httpd_close_conn( connects[cnum].hc, &tv );
		if ( connects[cnum].hc != (httpd_conn*) 0 )
			{
			httpd_destroy_conn( connects[cnum].hc );
			free( (void*) connects[cnum].hc );
			--httpd_conn_count;
			connects[cnum].hc = (httpd_conn*) 0;
			}
		}
	if ( hs != (httpd_server*) 0 )
		{
		httpd_server* ths = hs;
		hs = (httpd_server*) 0;
		for ( i=0 ; ths->listen_fds[i]>=0 ; i++ )
				fdwatch_del_fd( ths->listen_fds[i] );
		httpd_terminate( ths );
		}
	mmc_destroy();
	tmr_destroy();
	free( (void*) connects );
	if ( throttles != (throttletab*) 0 )
		free( (void*) throttles );

	/* childs's hard kill */
	for ( cnum = hctab.pidmin; cnum < hctab.pidmax; ++cnum )
		if (hctab.hcs[cnum-hctab.pidmin])
			kill( -cnum, SIGKILL );

	/* if we have to, remove rule from netfilter */
	if ( iptables_cmd[0] != '\0' )
		if (system(iptables_cmd) != 0)
	/* Hehehe..  we fail as we should have no more root privileges...
	 * So give at least the solution command in syslog */
			syslog(LOG_WARNING,"Oups, I have no more enough privileges to clean my own netfilter rule 8-/ ... should do: sudo %s ", iptables_cmd);

	}

/*
 * \return 1 if there is no more connection to accept for now, else 0
 * \note may exit() !
 */
static int
handle_newconnect( struct timeval* tvP, int listen_fd )
	{
	connecttab* c;
	//ClientData client_data;

	/* This loops until the accept() fails, trying to start new
	** connections as fast as possible so we don't overrun the
	** listen queue.
	*/
	for (;;)
		{
		/* Is there room in the connection table? */
		if ( num_connects >= max_connects )
			{
			/* Out of connection slots.  Run the timers, then the
			** existing connections, and maybe we'll free up a slot
			** by the time we get back here.
			*/
			syslog( LOG_WARNING, "too many connections!" );
			tmr_run( tvP );
			return 0;
			}
		/* Get the first free connection entry off the free list. */
		if ( first_free_connect == -1 || connects[first_free_connect].conn_state != CNST_FREE )
			{
			syslog( LOG_CRIT, "the connects free list is messed up" );
			exit( 1 );
			}
		c = &connects[first_free_connect];
		/* Make the httpd_conn if necessary. */
		if ( c->hc == (httpd_conn*) 0 )
			{
			c->hc = NEW( httpd_conn, 1 );
			if ( c->hc == (httpd_conn*) 0 )
				{
				syslog( LOG_CRIT, "out of memory allocating %s", "an httpd_conn" );
				exit( 1 );
				}
			c->hc->initialized = 0;
			++httpd_conn_count;
			}

		/* Get the connection. */
		switch ( httpd_get_conn( hs, listen_fd, c->hc ) )
			{
			/* Some error happened.  Run the timers, then the
			** existing connections.  Maybe the error will clear.
			*/
			case GC_FAIL:
			tmr_run( tvP );
			return 0;

			/* No more connections to accept for now. */
			case GC_NO_MORE:
			return 1;
			}
		c->conn_state = CNST_READING;
		/* Pop it off the free list. */
		first_free_connect = c->next_free_connect;
		c->next_free_connect = -1;
		++num_connects;
		//client_data.p = c;
		c->active_at = tvP->tv_sec;
		c->wakeup_timer = (Timer*) 0;
		c->linger_timer = (Timer*) 0;
		c->next_byte_index = 0;
		c->numtnums = 0;

		/* Set the connection file descriptor to no-delay mode. */
		httpd_set_ndelay( c->hc->conn_fd );

		fdwatch_add_fd( c->hc->conn_fd, c, FDW_READ );

		++stats_connections;
		if ( num_connects > stats_simultaneous )
			stats_simultaneous = num_connects;
		}
	}


static void
handle_read( connecttab* c, struct timeval* tvP )
	{
	int sz;
	//ClientData client_data;
	httpd_conn* hc = c->hc;

	/* Is there room in our buffer to read more bytes? */
	if ( hc->read_idx >= hc->read_size )
		{
		if ( hc->read_size > 5000 )
			{
			httpd_send_err( hc, 400, httpd_err400title, "", httpd_err400form, "" );
			finish_connection( c, tvP );
			return;
			}
		httpd_realloc_str(
			&hc->read_buf, &hc->read_size, hc->read_size + 1000 );
		}

	/* Read some more bytes. */
	sz = read(
		hc->conn_fd, &(hc->read_buf[hc->read_idx]),
		hc->read_size - hc->read_idx );
	if ( sz == 0 )
		{
		httpd_send_err( hc, 400, httpd_err400title, "", httpd_err400form, "" );
		finish_connection( c, tvP );
		return;
		}
	if ( sz < 0 )
		{
		/* Ignore EINTR and EAGAIN.  Also ignore EWOULDBLOCK.  At first glance
		** you would think that connections returned by fdwatch as readable
		** should never give an EWOULDBLOCK; however, this apparently can
		** happen if a packet gets garbled.
		*/
		if ( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK )
			return;
		httpd_send_err(
			hc, 400, httpd_err400title, "", httpd_err400form, "" );
		finish_connection( c, tvP );
		return;
		}
	hc->read_idx += sz;
	c->active_at = tvP->tv_sec;

	/* Do we have a complete request yet? */
	switch ( httpd_got_request( hc ) )
		{
		case GR_NO_REQUEST:
		return;
		case GR_BAD_REQUEST:
		httpd_send_err( hc, 400, httpd_err400title, "", httpd_err400form, "" );
		finish_connection( c, tvP );
		return;
		}

	/* Yes.  Try parsing and resolving it. */
	if ( httpd_parse_request( hc ) < 0 )
		{
		finish_connection( c, tvP );
		return;
		}

	/* Check the throttle table */
	if ( ! check_throttles( c ) )
		{
		httpd_send_err(
			hc, 503, httpd_err503title, "", httpd_err503form, hc->encodedurl );
		finish_connection( c, tvP );
		return;
		}

	/* Start the connection going. */
	if ( httpd_start_request( hc, tvP ) < 0 )
		{
		/* Something went wrong.  Close down the connection. */
		finish_connection( c, tvP );
		return;
		}

	/* Fill in end_byte_index. */
	if ( hc->bfield & HC_GOT_RANGE )
		{
		c->next_byte_index = hc->first_byte_index;
		c->end_byte_index = hc->last_byte_index + 1;
		}
	else if ( hc->bytes_to_send < 0 )
		c->end_byte_index = 0;
	else
		c->end_byte_index = hc->bytes_to_send;

	/* Check if it's already handled. */
	if ( hc->file_address == (char*) 0 )
		{
		/* No file address means someone else (a child process) is handling it. */
		int tind;
		for ( tind = 0; tind < c->numtnums; ++tind )
			throttles[c->tnums[tind]].bytes_since_avg += hc->bytes_sent;
		c->next_byte_index = hc->bytes_sent;
		finish_connection( c, tvP );
		return;
		}
	if ( c->next_byte_index >= c->end_byte_index )
		{
		/* There's nothing to send. */
		finish_connection( c, tvP );
		return;
		}

	/* Cool, we have a valid connection and a file to send to it. */
	c->conn_state = CNST_SENDING;
	c->started_at = tvP->tv_sec;
	c->wouldblock_delay = 0;
	//client_data.p = c;

	fdwatch_del_fd( hc->conn_fd );
	fdwatch_add_fd( hc->conn_fd, c, FDW_WRITE );
	}


static void
handle_send( connecttab* c, struct timeval* tvP )
	{
	size_t max_bytes;
	int sz, coast;
	ClientData client_data;
	time_t elapsed;
	httpd_conn* hc = c->hc;
	int tind;

	if ( c->max_limit == THROTTLE_NOLIMIT )
		max_bytes = 1000000000L;
	else
		max_bytes = c->max_limit / 4;		/* send at most 1/4 seconds worth */

	/* Do we need to write the headers first? */
	if ( hc->responselen == 0 )
		{
		/* No, just write the file. */
		sz = write(
			hc->conn_fd, &(hc->file_address[c->next_byte_index]),
			MIN( c->end_byte_index - c->next_byte_index, max_bytes ) );
		}
	else
		{
		/* Yes.  We'll combine headers and file into a single writev(),
		** hoping that this generates a single packet.
		*/
		struct iovec iv[2];

		iv[0].iov_base = hc->response;
		iv[0].iov_len = hc->responselen;
		iv[1].iov_base = &(hc->file_address[c->next_byte_index]);
		iv[1].iov_len = MIN( c->end_byte_index - c->next_byte_index, max_bytes );
		sz = writev( hc->conn_fd, iv, 2 );
		}

	if ( sz < 0 && errno == EINTR )
		return;

	if ( sz == 0 ||
		 ( sz < 0 && ( errno == EWOULDBLOCK || errno == EAGAIN ) ) )
		{
		/* This shouldn't happen, but some kernels, e.g.
		** SunOS 4.1.x, are broken and select() says that
		** O_NDELAY sockets are always writable even when
		** they're actually not.
		**
		** Current workaround is to block sending on this
		** socket for a brief adaptively-tuned period.
		** Fortunately we already have all the necessary
		** blocking code, for use with throttling.
		*/
		c->wouldblock_delay += MIN_WOULDBLOCK_DELAY;
		c->conn_state = CNST_PAUSING;
		fdwatch_del_fd( hc->conn_fd );
		client_data.p = c;
		if ( c->wakeup_timer != (Timer*) 0 )
			syslog( LOG_ERR, "replacing non-null wakeup_timer!" );
		c->wakeup_timer = tmr_create(
			tvP, wakeup_connection, client_data, c->wouldblock_delay, 0 );
		if ( c->wakeup_timer == (Timer*) 0 )
			{
			syslog( LOG_CRIT, "tmr_create(wakeup_connection) failed" );
			exit( 1 );
			}
		return;
		}

	if ( sz < 0 )
		{
		/* Something went wrong, close this connection.
		**
		** If it's just an EPIPE, don't bother logging, that
		** just means the client hung up on us.
		**
		** On some systems, write() occasionally gives an EINVAL.
		** Dunno why, something to do with the socket going
		** bad.  Anyway, we don't log those either.
		**
		** And ECONNRESET isn't interesting either.
		*/
		if ( errno != EPIPE && errno != EINVAL && errno != ECONNRESET )
			syslog( LOG_ERR, "write - %m sending %.80s", hc->encodedurl );
		clear_connection( c, tvP );
		return;
		}

	/* Ok, we wrote something. */
	c->active_at = tvP->tv_sec;
	/* Was this a headers + file writev()? */
	if ( hc->responselen > 0 )
		{
		/* Yes; did we write only part of the headers? */
		if ( sz < hc->responselen )
			{
			/* Yes; move the unwritten part to the front of the buffer. */
			int newlen = hc->responselen - sz;
			(void) memmove( hc->response, &(hc->response[sz]), newlen );
			hc->responselen = newlen;
			sz = 0;
			}
		else
			{
			/* Nope, we wrote the full headers, so adjust accordingly. */
			sz -= hc->responselen;
			hc->responselen = 0;
			}
		}
	/* And update how much of the file we wrote. */
	c->next_byte_index += sz;
	c->hc->bytes_sent += sz;
	for ( tind = 0; tind < c->numtnums; ++tind )
		throttles[c->tnums[tind]].bytes_since_avg += sz;

	/* Are we done? */
	if ( c->next_byte_index >= c->end_byte_index )
		{
		/* This connection is finished! */
		finish_connection( c, tvP );
		return;
		}

	/* Tune the (blockheaded) wouldblock delay. */
	if ( c->wouldblock_delay > MIN_WOULDBLOCK_DELAY )
		c->wouldblock_delay -= MIN_WOULDBLOCK_DELAY;

	/* If we're throttling, check if we're sending too fast. */
	if ( c->max_limit != THROTTLE_NOLIMIT )
		{
		elapsed = tvP->tv_sec - c->started_at;
		if ( elapsed == 0 )
			elapsed = 1;		/* count at least one second */
		if ( c->hc->bytes_sent / elapsed > c->max_limit )
			{
			c->conn_state = CNST_PAUSING;
			fdwatch_del_fd( hc->conn_fd );
			/* How long should we wait to get back on schedule?  If less
			** than a second (integer math rounding), use 1/2 second.
			*/
			coast = c->hc->bytes_sent / c->max_limit - elapsed;
			client_data.p = c;
			if ( c->wakeup_timer != (Timer*) 0 )
				syslog( LOG_ERR, "replacing non-null wakeup_timer!" );
			c->wakeup_timer = tmr_create(
				tvP, wakeup_connection, client_data,
				coast > 0 ? ( coast * 1000L ) : 500L, 0 );
			if ( c->wakeup_timer == (Timer*) 0 )
				{
				syslog( LOG_CRIT, "tmr_create(wakeup_connection) failed" );
				exit( 1 );
				}
			}
		}
	/* (No check on min_limit here, that only controls connection startups.) */
	}


static void
handle_linger( connecttab* c, struct timeval* tvP )
	{
	char buf[4096];
	int r;

	/* In lingering-close mode we just read and ignore bytes.  An error
	** or EOF ends things, otherwise we go until a timeout.
	*/
	r = read( c->hc->conn_fd, buf, sizeof(buf) );
	if ( r < 0 && ( errno == EINTR || errno == EAGAIN ) )
		return;
	if ( r <= 0 )
		really_clear_connection( c, tvP );
	}


static int
check_throttles( connecttab* c )
	{
	int tnum;
	long l;

	c->numtnums = 0;
	c->max_limit = c->min_limit = THROTTLE_NOLIMIT;
	for ( tnum = 0; tnum < numthrottles && c->numtnums < MAXTHROTTLENUMS;
		  ++tnum )
		if ( match( throttles[tnum].pattern, c->hc->realfilename ) )
			{
			/* If we're way over the limit, don't even start. */
			if ( throttles[tnum].rate > throttles[tnum].max_limit * 2 )
				return 0;
			/* Also don't start if we're under the minimum. */
			if ( throttles[tnum].rate < throttles[tnum].min_limit )
				return 0;
			if ( throttles[tnum].num_sending < 0 )
				{
				syslog( LOG_ERR, "throttle sending count was negative - shouldn't happen!" );
				throttles[tnum].num_sending = 0;
				}
			c->tnums[c->numtnums++] = tnum;
			++throttles[tnum].num_sending;
			l = throttles[tnum].max_limit / throttles[tnum].num_sending;
			if ( c->max_limit == THROTTLE_NOLIMIT )
				c->max_limit = l;
			else
				c->max_limit = MIN( c->max_limit, l );
			l = throttles[tnum].min_limit;
			if ( c->min_limit == THROTTLE_NOLIMIT )
				c->min_limit = l;
			else
				c->min_limit = MAX( c->min_limit, l );
			}
	return 1;
	}


static void
clear_throttles( connecttab* c, struct timeval* tvP )
	{
	int tind;

	for ( tind = 0; tind < c->numtnums; ++tind )
		--throttles[c->tnums[tind]].num_sending;
	}


static void
update_throttles( ClientData client_data, struct timeval* nowP )
	{
	int tnum, tind;
	int cnum;
	connecttab* c;
	long l;

	/* Update the average sending rate for each throttle.  This is only used
	** when new connections start up.
	*/
	for ( tnum = 0; tnum < numthrottles; ++tnum )
		{
		throttles[tnum].rate = ( 2 * throttles[tnum].rate + throttles[tnum].bytes_since_avg / THROTTLE_TIME ) / 3;
		throttles[tnum].bytes_since_avg = 0;
		/* Log a warning message if necessary. */
		if ( throttles[tnum].rate > throttles[tnum].max_limit && throttles[tnum].num_sending != 0 )
			{
			if ( throttles[tnum].rate > throttles[tnum].max_limit * 2 )
				syslog( LOG_NOTICE, "throttle #%d '%.80s' rate %ld greatly exceeding limit %ld; %d sending", tnum, throttles[tnum].pattern, throttles[tnum].rate, throttles[tnum].max_limit, throttles[tnum].num_sending );
			else
				syslog( LOG_INFO, "throttle #%d '%.80s' rate %ld exceeding limit %ld; %d sending", tnum, throttles[tnum].pattern, throttles[tnum].rate, throttles[tnum].max_limit, throttles[tnum].num_sending );
			}
		if ( throttles[tnum].rate < throttles[tnum].min_limit && throttles[tnum].num_sending != 0 )
			{
			syslog( LOG_NOTICE, "throttle #%d '%.80s' rate %ld lower than minimum %ld; %d sending", tnum, throttles[tnum].pattern, throttles[tnum].rate, throttles[tnum].min_limit, throttles[tnum].num_sending );
			}
		}

	/* Now update the sending rate on all the currently-sending connections,
	** redistributing it evenly.
	*/
	for ( cnum = 0; cnum < max_connects; ++cnum )
		{
		c = &connects[cnum];
		if ( c->conn_state == CNST_SENDING || c->conn_state == CNST_PAUSING )
			{
			c->max_limit = THROTTLE_NOLIMIT;
			for ( tind = 0; tind < c->numtnums; ++tind )
				{
				tnum = c->tnums[tind];
				l = throttles[tnum].max_limit / throttles[tnum].num_sending;
				if ( c->max_limit == THROTTLE_NOLIMIT )
					c->max_limit = l;
				else
					c->max_limit = MIN( c->max_limit, l );
				}
			}
		}
	}


static void
finish_connection( connecttab* c, struct timeval* tvP )
	{
	/* If we haven't actually sent the buffered response yet, do so now. */
	httpd_write_response( c->hc );

	/* And clear. */
	clear_connection( c, tvP );
	}


static void
clear_connection( connecttab* c, struct timeval* tvP )
	{
	ClientData client_data;

	if ( c->wakeup_timer != (Timer*) 0 )
		{
		tmr_cancel( c->wakeup_timer );
		c->wakeup_timer = 0;
		}

	/* This is our version of Apache's lingering_close() routine, which is
	** their version of the often-broken SO_LINGER socket option.  For why
	** this is necessary, see http://www.apache.org/docs/misc/fin_wait_2.html
	** What we do is delay the actual closing for a few seconds, while reading
	** any bytes that come over the connection.  However, we don't want to do
	** this unless it's necessary, because it ties up a connection slot and
	** file descriptor which means our maximum connection-handling rate
	** is lower.  So, elsewhere we set a flag when we detect the few
	** circumstances that make a lingering close necessary.  If the flag
	** isn't set we do the real close now.
	*/
	if ( c->conn_state == CNST_LINGERING )
		{
		/* If we were already lingering, shut down for real. */
		tmr_cancel( c->linger_timer );
		c->linger_timer = (Timer*) 0;
		c->hc->bfield &= ~HC_SHOULD_LINGER;
		}
	if ( c->hc->bfield & HC_SHOULD_LINGER )
		{
		if ( c->conn_state != CNST_PAUSING )
			fdwatch_del_fd( c->hc->conn_fd );
		c->conn_state = CNST_LINGERING;
		shutdown( c->hc->conn_fd, SHUT_WR );
		fdwatch_add_fd( c->hc->conn_fd, c, FDW_READ );
		client_data.p = c;
		if ( c->linger_timer != (Timer*) 0 )
			syslog( LOG_ERR, "replacing non-null linger_timer!" );
		c->linger_timer = tmr_create(
			tvP, linger_clear_connection, client_data, LINGER_TIME, 0 );
		if ( c->linger_timer == (Timer*) 0 )
			{
			syslog( LOG_CRIT, "tmr_create(linger_clear_connection) failed" );
			exit( 1 );
			}
		}
	else
		really_clear_connection( c, tvP );
	}


static void
really_clear_connection( connecttab* c, struct timeval* tvP )
	{
	stats_bytes += c->hc->bytes_sent;
	if ( c->conn_state != CNST_PAUSING )
		fdwatch_del_fd( c->hc->conn_fd );
	httpd_close_conn( c->hc, tvP );
	clear_throttles( c, tvP );
	if ( c->linger_timer != (Timer*) 0 )
		{
		tmr_cancel( c->linger_timer );
		c->linger_timer = 0;
		}
	c->conn_state = CNST_FREE;
	c->next_free_connect = first_free_connect;
	first_free_connect = c - connects;		/* division by sizeof is implied */
	--num_connects;
	}


static void
idle( ClientData client_data, struct timeval* nowP )
	{
	int cnum;
	connecttab* c;

	for ( cnum = 0; cnum < max_connects; ++cnum )
		{
		c = &connects[cnum];
		switch ( c->conn_state )
			{
			case CNST_READING:
			if ( nowP->tv_sec - c->active_at >= IDLE_READ_TIMELIMIT )
				{
				syslog( LOG_INFO,
					"%.80s connection timed out reading",
					c->hc->client_addr );
				httpd_send_err(
					c->hc, 408, httpd_err408title, "", httpd_err408form, "" );
				finish_connection( c, nowP );
				}
			break;
			case CNST_SENDING:
			case CNST_PAUSING:
			if ( nowP->tv_sec - c->active_at >= IDLE_SEND_TIMELIMIT )
				{
				syslog( LOG_INFO,
					"%.80s connection timed out sending",
					c->hc->client_addr );
				clear_connection( c, nowP );
				}
			break;
			}
		}
	}


static void
wakeup_connection( ClientData client_data, struct timeval* nowP )
	{
	connecttab* c;

	c = (connecttab*) client_data.p;
	c->wakeup_timer = (Timer*) 0;
	if ( c->conn_state == CNST_PAUSING )
		{
		c->conn_state = CNST_SENDING;
		fdwatch_add_fd( c->hc->conn_fd, c, FDW_WRITE );
		}
	}

static void
linger_clear_connection( ClientData client_data, struct timeval* nowP )
	{
	connecttab* c;

	c = (connecttab*) client_data.p;
	c->linger_timer = (Timer*) 0;
	really_clear_connection( c, nowP );
	}


static void
occasional( ClientData client_data, struct timeval* nowP )
	{
	mmc_cleanup( nowP );
	tmr_cleanup();
	watchdog_flag = 1;				/* let the watchdog know that we are alive */
	}


#ifdef STATS_TIME
static void
show_stats( ClientData client_data, struct timeval* nowP )
	{
	logstats( nowP );
	}
#endif /* STATS_TIME */


/* Generate debugging statistics syslog messages for all packages. */
static void
logstats( struct timeval* nowP )
	{
	struct timeval tv;
	time_t now;
	long up_secs, stats_secs;

	if ( nowP == (struct timeval*) 0 )
		{
		(void) gettimeofday( &tv, (struct timezone*) 0 );
		nowP = &tv;
		}
	now = nowP->tv_sec;
	up_secs = now - start_time;
	stats_secs = now - stats_time;
	if ( stats_secs == 0 )
		stats_secs = 1;		/* fudge */
	stats_time = now;
	syslog( LOG_INFO,
		"up %ld seconds, stats for %ld seconds:", up_secs, stats_secs );

	thttpd_logstats( stats_secs );
	httpd_logstats( stats_secs );
	mmc_logstats( stats_secs );
	fdwatch_logstats( stats_secs );
	tmr_logstats( stats_secs );
	}


/* Generate debugging statistics syslog message. */
static void
thttpd_logstats( long secs )
	{
	if ( secs > 0 )
		syslog( LOG_INFO,
			"  thttpd - %ld connections (%g/sec), %d max simultaneous, %lld bytes (%g/sec), %d httpd_conns allocated",
			stats_connections, (float) stats_connections / secs,
			stats_simultaneous, (int64_t) stats_bytes,
			(float) stats_bytes / secs, httpd_conn_count );
	stats_connections = 0;
	stats_bytes = 0;
	stats_simultaneous = 0;
	}

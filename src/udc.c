/* udc.c - udc/create and udc/validate management
*
* about OpenUDC please check following urls:
* - http://openudc.org
* - https://github.com/Open-UDC
*/
#ifdef OPENUDC

#include <stdlib.h>
#include <syslog.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>   /* errno             */
#include <gpgme.h>
#include <regex.h>
#include <pthread.h>

#include "config.h"
#include "udc.h"
#include "libhttpd.h"

/*! manage creation sheet
 */
void udc_create( httpd_conn* hc ) {
	size_t c;
	ssize_t r;
	char * cp, * boundary;
	int i=0, nsigs=1, boundarylen=0;

	gpgme_ctx_t gpglctx;
	gpgme_error_t gpgerr;
	gpgme_data_t gpgdata;
	gpgme_import_result_t gpgimport=NULL;
	gpgme_import_status_t gpgikey=NULL;
	gpgme_key_t gpgkey=NULL;

	char * buff;
	int buffsize;

	if ( strncasecmp( hc->contenttype, "multipart/msigned", sizeof("multipart/msigned")-1 ) ) {
		httpd_send_err(hc, 415, err415title, "", "%.80s unrecognized here, expected multipart/msigned.", hc->contenttype);
		exit(EXIT_FAILURE);
	}

	cp=hc->contenttype+sizeof("multipart/msigned")-1;

	while (*cp && (*cp != '\n') ) {
		//syslog( LOG_INFO, " 1- cp: %s", cp );
		/* find next parameter */
		cp += strspn( cp, "\" \t;" );
		//syslog( LOG_INFO, " 2- cp: %s", cp );
		if ( !strncasecmp( cp, "boundary=", sizeof("boundary=")-1 ) ) {
			cp += sizeof("boundary=")-1;
			cp += strspn( cp, " \t" );
			if (*cp == '"') {
			/* if boundary value is protected with "doubles quotes" */
				cp++;
				boundary=cp;
				while ( *cp && (*cp != '\n') && (*cp != '"') )
					cp++;
				boundarylen=( *cp == '"' ? cp-boundary : 0 );
			} else {
				boundary=cp;
				while ( (*cp != ';') && (*cp >= '&') && (*cp <= 'z') )
					cp++;
				boundarylen=cp-boundary;
			}
		} else if ( !strncasecmp( cp, "nsigs=", sizeof("nsigs=")-1 ) ) {
			cp += sizeof("nsigs=")-1;
			cp += strspn( cp, " \t\"" );
			nsigs=atoi(cp);
			if (!nsigs) {
				httpd_send_err(hc, 400, httpd_err400title, "", err500form, "nsigs=" );
				exit(EXIT_FAILURE);
			}
		}
		/* find next separator (ignore unknow stuff) */
		cp += strcspn( cp, " \t;" );
		//syslog( LOG_INFO, " 3- cp: %s", cp );
	}

	if ( boundarylen == 0 ) {
		httpd_send_err(hc, 400, httpd_err400title, "", err500form, "boundary=" );
		exit(EXIT_FAILURE);
	}

	if ( nsigs < 1 ) {
		httpd_send_err(hc, 501, err501title, "", err501form, "nsigs < 1" );
		exit(EXIT_FAILURE);
	}

	if (hc->contentlength < 12) {
		httpd_send_err(hc, 411, err411title, "", "Content-Length is absent or too short (%.80s)", "12");
		exit(EXIT_FAILURE);
	}

	buffsize=hc->contentlength;
	buff=malloc(buffsize+1);
	if (!buff) {
		httpd_send_err(hc, 500, err500title, "", err500form, "m" );
		exit(EXIT_FAILURE);
	}

	c = hc->read_idx - hc->checked_idx;
	if ( c > 0 )
		memcpy(buff,&(hc->read_buf[hc->checked_idx]), c);
	while ( c < hc->contentlength ) {
		r = read( hc->conn_fd, buff+c, hc->contentlength - c );
		if ( r < 0 && ( errno == EINTR || errno == EAGAIN ) ) {
			struct timespec tim={0, 300000000}; /* 300 ms */
			nanosleep(&tim, NULL);
			if (i++>50) { /* 50*300ms = 15 seconds */
				httpd_send_err(hc, 408, httpd_err408title, "", httpd_err408form, "" );
				exit(EXIT_FAILURE);
			}
			continue;
		} else
			i=0;
		if ( r <= 0 ) {
			httpd_send_err(hc, 500, err500title, "", err500form, "read error" );
			exit(EXIT_FAILURE);
		}
		c += r;
	}

	/* create context */
	gpgerr=gpgme_new(&gpglctx);
	if ( gpgerr  != GPG_ERR_NO_ERROR ) {
		httpd_send_err(hc, 500, err500title, "", err500form, gpgme_strerror(gpgerr) );
		exit(EXIT_FAILURE);
	}



	/*{
		send_mime(hc, 200, ok200title, "", "", "text/html; charset=%s",(off_t) -1, hc->sb.st_mtime );
		httpd_write_response(hc);
		r=snprintf(buff,buffsize,"<html><head><title>pks/add %d keys</title></head><body><h2>Total: %d<br>imported: %d<br>unchanged: %d<br>no_user_id: %d<br>new_user_ids: %d<br>new_sub_keys: %d<br>new_signatures: %d<br>new_revocations: %d<br>secret_read: %d<br>not_imported: %d</h2></body></html>", gpgimport->considered, gpgimport->considered, gpgimport->imported, gpgimport->unchanged, gpgimport->no_user_id, gpgimport->new_user_ids, gpgimport->new_sub_keys, gpgimport->new_signatures, gpgimport->new_revocations, gpgimport->secret_read, gpgimport->not_imported);
	}
	httpd_write_fully( hc->conn_fd, buff,MIN(r,buffsize));
*/
	httpd_send_err(hc, 501, err501title, "", err501form, "udc/create" );
	gpgme_release (gpglctx);
	exit(EXIT_SUCCESS);

}


void udc_validate( httpd_conn* hc ) {

	httpd_send_err( hc, 501, err501title, "", err501form, "udc/validate" );
	//gpgme_release (gpglctx);
	exit(EXIT_SUCCESS);
}
#endif /* OPENUDC */

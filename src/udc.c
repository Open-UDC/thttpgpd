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
	ssize_t r, csize;
	char * cp, * eol, * boundary=NULL;
	int i=0, nsigs=1, boundarylen=0, issig;

	gpgme_data_t sheet, * sigs;
	gpgme_ctx_t gpglctx;
	gpgme_error_t gpgerr;
	
	char * buff;

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

	if ( boundarylen < 1 ) {
		httpd_send_err(hc, 400, httpd_err400title, "", err500form, "boundary=" );
		exit(EXIT_FAILURE);
	}

	if ( nsigs < 0 )
		nsigs=-nsigs;

	if ( nsigs == 0 ) {
		httpd_send_err(hc, 501, err501title, "", err501form, "nsigs == 0" );
		exit(EXIT_FAILURE);
	}

	if (hc->contentlength < 12) {
		httpd_send_err(hc, 411, err411title, "", "Content-Length is absent or too short (%.80s)", "12");
		exit(EXIT_FAILURE);
	}

	cp=boundary;
	boundary=malloc(boundarylen+5);
	sigs=malloc(nsigs*sizeof(gpgme_data_t));
	buff=malloc(hc->contentlength+1);
	if ( (!buff) || (!sigs) || (!boundary) ) {
		httpd_send_err(hc, 500, err500title, "", err500form, "m" );
		exit(EXIT_FAILURE);
	}

	strcpy(boundary,"--");
	strncpy(boundary+2,cp,boundarylen);
	strcpy(boundary+2+boundarylen,"--");

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
	buff[hc->contentlength+1]='\0';

	i=0;
	cp=buff;
	//while ( eol=strchr(cp, '\n') ) {
	while ( !strncmp(cp,boundary,2+boundarylen) ) {

		if ( !strncmp(cp+2+boundarylen,"--",2) ) /* last boundary */
			break;

		csize=0;
		issig=0;

		cp=strchr(cp+2+boundarylen, '\n')+1;
		/* Parse sub-header */
		while ( (eol=strchr(cp, '\n'))  ) {
			if ( (eol-cp) < 3) { /* end of sub-header */
				cp=eol+sizeof(char);
				break;
			} else if ( strncasecmp( cp, "Content-Length:", 15 ) == 0 ) {
				cp = &cp[15];
				csize = atol( cp );
			} else	if ( strncasecmp( cp, "Content-Type:", 13 ) == 0 ) {
				cp = &cp[13];
				cp += strspn( cp, " \t" );
				if ( strncasecmp( cp, "application/pgp-signature", 25 ) == 0 )
					issig=1;

			}
		}
		if ( csize < 1 ) {
			httpd_send_err(hc, 411, err411title, "", "Content-Length is absent or too short (%.80s)", "1");
			exit(EXIT_FAILURE);
		}
		if (issig) {
			if (i>=nsigs) {
				httpd_send_err(hc, 400, httpd_err400title, "", err500form, "sigs>nsigs" );
				exit(EXIT_FAILURE);
			}
			if ( ( gpgerr=gpgme_data_new_from_mem(&sigs[i],cp,csize,0) ) != GPG_ERR_NO_ERROR ) {
				httpd_send_err(hc, 500, err500title, "", err500form, gpgme_strerror(gpgerr) );
				exit(EXIT_FAILURE);
			}
			i++;
		} else if ( ( gpgerr=gpgme_data_new_from_mem(&sheet,cp,csize,0) ) != GPG_ERR_NO_ERROR ) {
				httpd_send_err(hc, 500, err500title, "", err500form, gpgme_strerror(gpgerr) );
				exit(EXIT_FAILURE);
		}
		cp+=csize;
	}
	if (i!=nsigs) {
		httpd_send_err(hc, 400, httpd_err400title, "", err500form, "sigs!=nsigs" );
		exit(EXIT_FAILURE);
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

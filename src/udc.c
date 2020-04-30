/* udc.c - udc/create and udc/validate management
**
** Copyright © 2012-2014 by Jean-Jacques Brucker <open-udc@googlegroups.com>.
** All rights reserved.
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
#include <ctype.h>

#include "config.h"
#include "udc.h"
#include "libhttpd.h"

/*! read keys (and there status and times) from a keyfile, and store them in an udc_key_t array.
 * \note: The udc_key_t array is (re)allocated.
 * \returns the number of key readed, or -1 if an error occurs (cf. errno).
 */
int udc_read_keys(const char * filename, udc_key_t ** keys) {
	FILE * keysfile;
	char * line=NULL, * endptr;
	size_t len = 0, maxkeys=256;
	char lvl;
	int i, j=0;

	if ( keys == NULL )
		return -1;

	*keys=RENEW(*keys, udc_key_t, maxkeys);
	if ( *keys == NULL )
		return -1;

	keysfile=fopen(filename, "r");
	if ( keysfile == NULL )
		return -1;

	while (getline(&line, &len, keysfile) > 0) {

		for (i=0; isxdigit( line[i] ); i++)
			line[i]=toupper(line[i]);

		if (line[i] != '\0')
			lvl=strtol(&line[i+1], &endptr, 10);
		else
			lvl=0;

		if ( (i != 40)  || (lvl <= 0) )
			continue;

		if (j>=maxkeys) {
			maxkeys+=256;
			*keys=RENEW(*keys, udc_key_t, maxkeys);
			if ( *keys == NULL ) {
				free(line);
				fclose(keysfile);
				return -1;
			}
		}
		strncpy((*keys)[j].fpr,line,40);
		(*keys)[j].fpr[40]='\0';
		(*keys)[j].level = lvl;

		if (endptr !='\0')
			(*keys)[j].flags = strtol(endptr+1, &endptr, 10);
		else
			(*keys)[j].flags = 0;

		if (endptr !='\0')
			(*keys)[j].lastsignedt = strtoll(endptr+1, &endptr, 10);
		else
			(*keys)[j].lastsignedt = 0;

		if (endptr !='\0')
			(*keys)[j].lastactivet = strtoll(endptr+1, &endptr, 10);
		else
			(*keys)[j].lastactivet = 0;
		j++;
	}

	free(line);
	if (fclose(keysfile) == 0)
		return j;
	else
		return -1;
}

/*! compare two key by there fingerprint.
 * May be used by qsort.
 * \note: key fpr must contain only uppercase hexa digit.
 */
int udc_cmp_keys(const udc_key_t * key1, const udc_key_t * key2) {
	return strcmp(key1->fpr,key2->fpr);
}

/*! check if an udc_key_t array contain any duplicate fpr
 * \return a pointer to the first duplicated key, or NULL if no duplicated fpr.
 */
udc_key_t * udc_check_dupkeys(udc_key_t * keys, size_t size) {
	int i;
	for (i=0;i<size-1;i++) {
		if (udc_cmp_keys(&keys[i],&keys[i+1]) == 0)
			return &keys[i];
	}
	return NULL;
}

/*! write keys (and there status and times) to a keysfile.
 * \returns the number of key written, or -1 if an error occurs (cf. errno).
 */
int udc_write_keys(const char * filename, const udc_key_t * keys, size_t size) {
	FILE * keysfile;
	size_t i;
	int r;

	if ( keys == NULL )
		return -1;

	keysfile=fopen(filename, "w");
	if ( keysfile == NULL )
		return -1;

	for (i=0;i<size;i++) {
		r=fprintf(keysfile,"%s:%d:%d:%lld:%lld\n",keys[i].fpr,keys[i].level,keys[i].flags,(long long)keys[i].lastsignedt,(long long)keys[i].lastactivet);
		if (r<0)
			break;
	}
	if (fclose(keysfile) == 0)
		return i;
	else
		return -1;
}

/*! search a key by its fingerprint in an array, assuming the array is sorted by fingerprint, which are hash (SHA-1) already
 *\return a pointer to the key in the array, or NULL if the key wasn't found.
 */
udc_key_t * udc_search_key(const udc_key_t * keys, size_t size,const char * fpr) {
	udc_key_t key;
	strncpy(key.fpr,fpr,40);
	key.fpr[40]='\0';
	/* TODO: optimize the search fonction to something like hsearch, as we are searching for hashed items... */
	return bsearch(&key,keys,size,sizeof(udc_key_t),(int (*)(const void *, const void *))udc_cmp_keys);
}

/* read a synthesis file.
 * \return the total numbers of lvls (which should be equal to the number of key) or a negative number if an error occurs.
 */
int udc_read_synthesis(const char * filename, sync_synthesis_t * synth) {
	FILE * sfile;
	char * line=NULL, * endptr;
	size_t len = 0;
	int i, j, r=0;

	if ( synth == NULL )
		return -1;

	sfile=fopen(filename, "r");
	if ( sfile == NULL )
		return -1;

/*	if (strncmp(line,"#s1",3) != 0 ) {
		free(line);
		errno=EMEDIUMTYPE;
		return -1;
	}*/

	while (getline(&line, &len, sfile) > 0) {
		if ( strncmp(line,"num_updates",sizeof("num_updates")-1) == 0 ) {
			for (i=sizeof("num_updates")-1;line[i];i++)
				if (line[i] >= '0' && line[i] <= '9')
					break;
			synth->nupdates=strtol(&line[i], &endptr, 0);
		}
		if ( strncmp(line,"lvl_keys",sizeof("lvl_keys")-1) == 0 ) {
			for (i=sizeof("lvl_keys")-1;line[i];i++)
				if (line[i] >= '0' && line[i] <= '9')
					break;
			synth->lvls[0]=strtol(&line[i], &endptr, 0);
			r=synth->lvls[0];
			for(j=1;j<SIZEOFARRAY(((sync_synthesis_t *)0)->lvls);j++) {
				if (endptr !='\0')
					synth->lvls[j] = strtol(endptr+1, &endptr, 10);
				else
					synth->lvls[j] = 0;
				r+= synth->lvls[j];
			}
		}
	}

	free(line);
	if (fclose(sfile) == 0)
		return r;
	else
		return -1;
}


/* update synthesis lvls from input keys.
 */
void udc_update_synthesis(const udc_key_t * keys, size_t size, sync_synthesis_t * synth) {
	size_t i;

	/*if ( keys == NULL || synth == NULL )
		return;*/

	for (i=0;i<SIZEOFARRAY(((sync_synthesis_t *)0)->lvls);i++)
		synth->lvls[i]=0;

	for (i=0;i<size;i++) {
		if (keys[i].level < SIZEOFARRAY(((sync_synthesis_t *)0)->lvls) )
			synth->lvls[keys[i].level]++;
	}
}

/* read a synthesis file.
 * \return the total numbers of lvls (which should be equal to the number of key) or a negative number if an error occurs.
 */
int udc_write_synthesis(const char * filename, sync_synthesis_t * synth) {
	FILE * sfile;
	int i,r,t=0;

	if ( synth == NULL )
		return -1;

	sfile=fopen(filename, "w");
	if ( sfile == NULL )
		return -1;

	r=fprintf(sfile,"#s1 Synthesis\n"
			"sync_type=OpenUDC\n"
			"num_updates=%d\n"
			"lvl_keys=( ",
			synth->nupdates);

	for (i=0;i<SIZEOFARRAY(((sync_synthesis_t *)0)->lvls);i++) {
		if (r>0)
			r=fprintf(sfile,"%d ",synth->lvls[i]);
		t+=synth->lvls[i];
	}
	if (r>0)
		r=fprintf(sfile,")\n"
				"num_keys=%d\n",
				t);

	if (fclose(sfile) == 0 && r>0 )
		return t;
	else
		return -1;
}

/*! update OpenUDC parameters (creation sheet)
 */
void udc_create( httpd_conn* hc ) {
	size_t c;
	ssize_t r, csize;
	char * cp, * eol, * boundary=NULL;
	int i=0, nsigs=1, boundarylen=0, issig;

	gpgme_data_t sheet, * sigs;
	gpgme_ctx_t gpglctx;
	gpgme_error_t gpgerr;
	gpgme_verify_result_t result;

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

	for (i=0;i<nsigs;i++) {
		  gpgerr = gpgme_op_verify (gpglctx, sigs[i], sheet, NULL);
		  result = gpgme_op_verify_result (gpglctx);
	}
	// Example of signature usage could be found in gpgme git repository
	//     // in the gpgme/tests/run-verify.c



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

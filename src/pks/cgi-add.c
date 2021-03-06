/*
 * cgi-add.c - CGI to add keys to a keyring, according to OpenUDC policy.
 *
 * Copyright 2012 Jean-Jacques Brucker <open-udc@googlegroups.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>  /* locale support    */
#include <gpgme.h>
#include <regex.h>

#include "version.h"

#define INPUT_MAX (1<<17) /* 1<<17 = 128ko */
#define HTML_4XX "<html><head><title>Error handling request</title></head><body><h1>Error handling request: %s</h1></body></html>"
#define HTML_5XX "<html><head><title>Internal Error</title></head><body><h1>Internal Error - %s</h1></body></html>"

static void http_header(int code)
{
	printf("HTTP/1.0 %d OK\n",code);
	if (code == 202)
		printf("X-HKP-Status: 418 some key(s) was rejected as per keyserver policy\n");
	printf("Server: %s\nContent-Type: text/html\n\n",SOFTWARE_NAME);

}

static int hexit( char c ) {
	if ( c >= '0' && c <= '9' )
		return(c - '0');
	if ( c >= 'a' && c <= 'f' )
		return(c - 'a' + 10);
	if ( c >= 'A' && c <= 'F' )
		return(c - 'A' + 10);
	return(-1);
}

/* Copies and decodes a string.  It's ok for from and to to be the
** same string. Return the lenght of decoded string.
*/
static int strdecode( char* to, char* from ) {
	int a,b,r;
	for ( r=0 ; *from != '\0'; to++, from++, r++ ) {
		if ( from[0] == '%' && (a=hexit(from[1])) >= 0 && (b=hexit(from[2])) >= 0 ) {
			*to = a* 16 + b;
			from += 2;
		} else
			*to = *from;
	}
	*to = '\0';
	return(r);
}

/* return this first comment starting with ud found in uids (or NULL if non found) */
static char * get_starting_comment(char * ud, gpgme_key_t gkey) {
	gpgme_user_id_t gpguids;
	size_t n;

	if (! ud)
		return((char *) 0);

	n=strlen(ud);
	gpguids=gkey->uids;
	while (gpguids) {
		if (!strncmp(gpguids->comment,ud,n))
			return gpguids->comment;
		gpguids=gpguids->next;
	}
	return((char *) 0);
}

int main(int argc, char *argv[])
{
	gpgme_ctx_t gpgctx;
	gpgme_error_t gpgerr;
	gpgme_engine_info_t enginfo;
	gpgme_data_t gpgdata;
	gpgme_import_result_t gpgimport;
	gpgme_import_status_t gpgikey;
	gpgme_key_t gpgkey;

	char * pclen, * buff;
	int clen, rcode=200;

	pclen=getenv("CONTENT_LENGTH");
	if (! pclen || *pclen == '\0' || (clen=atoi(pclen)) < 9 ) {
		http_header(411);
		printf(HTML_4XX,"Only non-empty POST containing OpenPGP certificate(s) compatible with an OpenUDC Policy are accepted here !");
		return 1;
	}

	if ( clen >= INPUT_MAX ) {
		http_header(413);
		printf(HTML_5XX,"your POST is too big.");
		return 1;
	}

	buff=malloc(clen+1);
	if (buff)
		buff[clen]='\0'; /*security for strdecode */
	else {
		http_header(500);
		printf(HTML_5XX,"");
		return 1;
	}

	if ( fread(buff,sizeof(char),clen,stdin) != clen ) {
		http_header(500);
		printf(HTML_5XX,"Error reading your data.");
		return 1;
	}

	/* Check gpgme version ( http://www.gnupg.org/documentation/manuals/gpgme/Library-Version-Check.html )*/
	setlocale (LC_ALL, "");
	gpgme_check_version (NULL);
	gpgme_set_locale (NULL, LC_CTYPE, setlocale (LC_CTYPE, NULL));
	/* check for OpenPGP support and create context */
	gpgerr=gpgme_engine_check_version(GPGME_PROTOCOL_OpenPGP);
	if (gpgerr == GPG_ERR_NO_ERROR)
		gpgerr=gpgme_new(&gpgctx);
	if (gpgerr == GPG_ERR_NO_ERROR)
		gpgerr = gpgme_get_engine_info(&enginfo);
	if (gpgerr == GPG_ERR_NO_ERROR)
		gpgerr = gpgme_ctx_set_engine_info(gpgctx, GPGME_PROTOCOL_OpenPGP, enginfo->file_name,"../../gpgme");

	if ( gpgerr  != GPG_ERR_NO_ERROR ) {
		http_header(500);
		printf(HTML_5XX,gpgme_strerror(gpgerr));
		return 1;
	}

	if (!strncmp(buff,"keytext=",8)) {
		int r;
		r=strdecode(buff,buff+8);
		//gpgme_set_armor(gpgctx,1);
		gpgerr = gpgme_data_new_from_mem(&gpgdata,buff,r,0);
	} else {
		gpgerr = gpgme_data_new_from_mem(&gpgdata,buff,clen,0); /* yes: that feature is not in HKP draft */
	}

	if ( gpgerr  != GPG_ERR_NO_ERROR ) {
		http_header(500);
		printf(HTML_5XX,gpgme_strerror(gpgerr));
		return 1;
	}

	if ( (gpgerr=gpgme_op_import (gpgctx, gpgdata)) != GPG_ERR_NO_ERROR ) {
		http_header(400);
		printf(HTML_4XX,gpgme_strerror(gpgerr));
		return 1;
	}

	if ((gpgimport= gpgme_op_import_result(gpgctx)) == NULL )  {
		http_header(500);
		printf(HTML_5XX,"result is NULL (... should not happen :-/).");
		return 1;
	}

	if ( gpgimport->considered == 0 ) {
		http_header(400);
		printf(HTML_4XX,"No valid key POST.");
		return 1;
	}
	/* Check (and eventually delete) imported keys */
	gpgikey=gpgimport->imports;
	while (gpgikey) {

		if ( (gpgikey->result != GPG_ERR_NO_ERROR) || (! (gpgikey->status & GPGME_IMPORT_NEW)) ) {
			/* erronous or known key */
			gpgikey=gpgikey->next;
			continue;
		}

		/* key is new, check that it match our policy. */
		if ( (gpgerr=gpgme_get_key (gpgctx,gpgikey->fpr,&gpgkey,0)) != GPG_ERR_NO_ERROR ) {
			/* should not happen */
			http_header(500);
			printf(HTML_5XX,gpgme_strerror(gpgerr));
			return 1;
		}

		/* Check that an uid comment start with "udid2;c;" or "ubot1;" */
		if (!( get_starting_comment("udid2;c;",gpgkey)
					|| get_starting_comment("ubot1;",gpgkey) ) ) {
			rcode=202;
			gpgme_op_delete (gpgctx,gpgkey,1);
		}
		gpgikey=gpgikey->next;
	}


	http_header(rcode);
	printf("<html><head><title>%d keys sended </title></head><body><h2>Total: %d<br>imported: %d<br>unchanged: %d<br>no_user_id: %d<br>new_user_ids: %d<br>new_sub_keys: %d<br>new_signatures: %d<br>new_revocations: %d<br>secret_read: %d<br>not_imported: %d</h2></body></html>", gpgimport->considered, gpgimport->considered, gpgimport->imported, gpgimport->unchanged, gpgimport->no_user_id, gpgimport->new_user_ids, gpgimport->new_sub_keys, gpgimport->new_signatures, gpgimport->new_revocations, gpgimport->secret_read, gpgimport->not_imported);

	return(0);

	/* TODO:
	 *  Note in memory the fpr in gpgme_import_status_t of all keys imported to :
	 *  - clean them (remove previous or useless signatures).
	 *  - revoke the one with with an usable secret key.
	 *  - propagate them to other ludd key server.
	 * DONE:
	 *  - check if they correspond to our policy (expire less than 20 years after, udid2 must be present ...)
	 */

}


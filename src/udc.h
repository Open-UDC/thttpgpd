/* udc.h - header file for OpenUDC interfaces
**
** Copyright © 2012-2014 by Jean-Jacques Brucker <open-udc@googlegroups.com>.
** All rights reserved.
*
* about OpenUDC please check following urls:
* - http://openudc.org
* - https://github.com/Open-UDC
*/

#ifndef _UDC_H_
#define _UDC_H_

#include "config.h"
#include "libhttpd.h"

/* defined key levels */
enum {
	FPR_LVL_UNKNOWN = 0 , /* Reserved for Futur Use */
	FPR_LVL_DELETE = 1 , /* Reserved for Futur Use */
	FPR_LVL_REJECTED = 2, /* Reserved for Futur Use */
	FPR_LVL_ACTIVE = 3,
	FPR_LVL_ALIVE = 4,
	FPR_LVL_ADMIN = 5
};

/* defined flags */
enum {
	FPR_FLAG_VALIDATING = (1<<0), /* Act as a mutex to avoid race bug validation */
	FPR_FLAG_HASVOTE = (1<<1),  /* used to count new parameters votes */
	FPR_FLAG_TRYMESS = (1<<2),  /* set to one once double spending detected, maybe useless ... ? */
};

typedef struct {
	char fpr[41]; /* fpr */
	unsigned char level : 4;
	unsigned char flags : 4;
	time_t lastsignedt;
	time_t lastactivet;
} udc_key_t;

typedef struct {
	int nupdates;
	int lvls[FPR_LVL_ADMIN+1];
} sync_synthesis_t;

/* required data for next creation_sheet */
typedef struct {
	int required_votes;
	char sha256sum[65]; /* ASCII (hex) format */
} next_csheet_t;

//	int[5] nbfpr;


/*! udc_init() read and verify databases, set global variables, check peers status and update database.
 */

int udc_read_keys(const char * filename, udc_key_t ** keys);
int udc_cmp_keys(const udc_key_t * key1, const udc_key_t * key2);
udc_key_t * udc_check_dupkeys(udc_key_t * keys, size_t size);
int udc_write_keys(const char * filename,const udc_key_t * keys, size_t size);
udc_key_t * udc_search_key(const udc_key_t * keys, size_t size, const char * fpr);

int udc_read_synthesis(const char * filename, sync_synthesis_t * synth);
void udc_update_synthesis(const udc_key_t * keys, size_t size, sync_synthesis_t * synth);
int udc_write_synthesis(const char * filename, sync_synthesis_t * synth);

//void * udc_update_peers(void *arg);
//int udc_init(...

/*! udc_create() validate, store and propagate new creation sheet.
 */
void udc_create( httpd_conn* hc );

/*! udc_validate() validate, store and propagate a transaction.
 */
void udc_validate( httpd_conn* hc );

#endif /* _UDC_H_ */

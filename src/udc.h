/* udc.h - header file for OpenUDC interfaces
*
* about OpenUDC please check following urls:
* - http://openudc.org
* - https://github.com/Open-UDC
*/

#ifndef _UDC_H_
#define _UDC_H_

#include "config.h"
#include "libhttpd.h"

/* Fingerprint (key or subkey) status */
typedef enum {
	FPR_STATUS_UNKNOWN = 0 , /* Reserved for Futur Use */
	FPR_STATUS_DELETE = 1 , /* Reserved for Futur Use */
	FPR_STATUS_REJECTED = 2, /* Reserved for Futur Use */
	FPR_STATUS_ACTIVE = 3,
	FPR_STATUS_ALIVE = 4, 
	FPR_STATUS_ADMIN = 5
} key_status_t;

typedef struct {
	char fpr[41];
	key_status_t status;
} udc_key_t;

/* required data for next creation_sheet */
typedef struct {
	int required_votes;
	char sha256sum[65]; /* ASCII (hex) format */
} next_csheet_t;

//	int[5] nbfpr;


/*! udc_init() read and verify databases, set global variables, check peers status and update database.
 */
//int udc_init(...

/*! udc_create() validate, store and propagate new creation sheet.
 */
void udc_create( httpd_conn* hc );

/*! udc_validate() validate, store and propagate a transaction.
 */
void udc_validate( httpd_conn* hc );

#endif /* _UDC_H_ */

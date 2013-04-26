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
	FPR_STATUS_UNKNOW = 0,
	FPR_STATUS_REJECTED = 1,
	FPR_STATUS_ACTIVE = 2,
	FPR_STATUS_ALIVE = 3, 
	FPR_STATUS_ADMIN = 4
} fpr_status_t;

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

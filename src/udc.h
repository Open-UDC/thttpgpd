/* hkp.h - header file for hkp management
*
* about HKP please check following urls:
* - http://tools.ietf.org/html/draft-shaw-openpgp-hkp-00
* - http://en.wikipedia.org/wiki/Key_server_%28cryptographic%29
*/

#ifndef _UDC_H_
#define _UDC_H_

#include "config.h"
#include "libhttpd.h"

/*! udc_create() validate, store and propagate new creation sheet.
 */
void udc_create( httpd_conn* hc );

/*! hkp_validate() validate, store and propagate a transaction.
 */
void udc_validate( httpd_conn* hc );

#endif /* _UDC_H_ */

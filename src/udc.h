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

/*! udc_create() validate, store and propagate new creation sheet.
 */
void udc_create( httpd_conn* hc );

/*! hkp_validate() validate, store and propagate a transaction.
 */
void udc_validate( httpd_conn* hc );

#endif /* _UDC_H_ */

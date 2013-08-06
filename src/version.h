/* version.h - version defines for ludd and libhttpd */

#ifndef _VERSION_H_
#define _VERSION_H_

#ifndef SOFTWARE_NAME
#ifdef OPENUDC
#define SOFTWARE_NAME "ludd"
#else /* OPENUDC */
#define SOFTWARE_NAME "thttpgpd"
#endif /* OPENUDC */
#endif /* SOFTWARE_NAME */

#define SOFTWARE_VERSION "0.2.9 06Aug2013"
#define SOFTWARE_ADDRESS "http://www.openudc.org/"

#endif /* _VERSION_H_ */

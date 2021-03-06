/* MD4.H - header file for MD4C.C
 * $FreeBSD: src/sys/sys/md4.h,v 1.2 2005/01/07 02:29:23 imp Exp $
 */

/*-
   Copyright (C) 1991-2, RSA Data Security, Inc. Created 1991. All
   rights reserved.

   License to copy and use this software is granted provided that it
   is identified as the "RSA Data Security, Inc. MD4 Message-Digest
   Algorithm" in all material mentioning or referencing this software
   or this function.
   License is also granted to make and use derivative works provided
   that such works are identified as "derived from the RSA Data
   Security, Inc. MD4 Message-Digest Algorithm" in all material
   mentioning or referencing the derived work.

   RSA Data Security, Inc. makes no representations concerning either
   the merchantability of this software or the suitability of this
   software for any particular purpose. It is provided "as is"
   without express or implied warranty of any kind.

   These notices must be retained in any copies of any part of this
   documentation and/or software.
 */

#ifndef _SYS_MD4_H_
#define _SYS_MD4_H_

#if !defined(_KERNEL)

#error "Userland must include openssl/md4.h instead of sys/md4.h"

#else

#ifndef _SYS_TYPES_H_
#include <sys/types.h>
#endif
#ifndef _SYS_CDEFS_H_
#include <sys/cdefs.h>
#endif

/* MD4 context. */
typedef struct MD4Context {
  u_int32_t state[4];	/* state (ABCD) */
  u_int32_t count[2];	/* number of bits, modulo 2^64 (lsb first) */
  unsigned char buffer[64];	/* input buffer */
} MD4_CTX;

void   MD4Init(MD4_CTX *);
void   MD4Update(MD4_CTX *, const unsigned char *, unsigned int);
void   MD4Final(unsigned char [16], MD4_CTX *);

#endif  /* _KERNEL */

#endif /* _MD4_H_ */

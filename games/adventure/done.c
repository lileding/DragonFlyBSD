/*	@(#)done.c	8.1 (Berkeley) 5/31/93				*/
/*	$NetBSD: done.c,v 1.10 2009/08/25 06:56:52 dholland Exp $	*/

/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * The game adventure was originally written in Fortran by Will Crowther
 * and Don Woods.  It was later translated to C and enhanced by Jim
 * Gillogly.  This code is derived from software contributed to Berkeley
 * by Jim Gillogly at The Rand Corporation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*      Re-coding of advent in C: termination routines */

#include <stdio.h>
#include <stdlib.h>
#include "hdr.h"
#include "extern.h"

int
score(void)
{				/* sort of like 20000 */
	int     myscore, i;

	maxscore = myscore = 0;
	for (i = 50; i <= maxtrs; i++) {
		if (ptext[i].txtlen == 0)
			continue;
		k = 12;
		if (i == chest)
			k = 14;
		if (i > chest)
			k = 16;
		if (prop[i] >= 0)
			myscore += 2;
		if (place[i] == 3 && prop[i] == 0)
			myscore += k - 2;
		maxscore += k;
	}
	myscore += (maxdie - numdie) * 10;
	maxscore += maxdie * 10;
	if (!(scoring || gaveup))
		myscore += 4;
	maxscore += 4;
	if (dflag != 0)
		myscore += 25;
	maxscore += 25;
	if (isclosing)
		myscore += 25;
	maxscore += 25;
	if (closed) {
		if (bonus == 0)
			myscore += 10;
		if (bonus == 135)
			myscore += 25;
		if (bonus == 134)
			myscore += 30;
		if (bonus == 133)
			myscore += 45;
	}
	maxscore += 45;
	if (place[magazine] == 108)
		myscore++;
	maxscore++;
	myscore += 2;
	maxscore += 2;
	for (i = 1; i <= hintmax; i++)
		if (hinted[i])
			myscore -= hints[i][2];
	return myscore;
}

/* entry=1 means goto 13000 */	/* game is over */
/* entry=2 means goto 20000 */	/* 3=19000 */
void
done(int entry)
{
	int     i, sc;
	if (entry == 1)
		mspeak(1);
	if (entry == 3)
		rspeak(136);
	printf("\n\n\nYou scored %d out of a ", (sc = score()));
	printf("possible %d using %d turns.\n", maxscore, turns);
	for (i = 1; i <= classes; i++)
		if (cval[i] >= sc) {
			speak(&ctext[i]);
			if (i == classes - 1) {
				printf("To achieve the next higher rating");
				printf(" would be a neat trick!\n\n");
				printf("Congratulations!!\n");
				exit(0);
			}
			k = cval[i] + 1 - sc;
			printf("To achieve the next higher rating, you need");
			printf(" %d more point", k);
			if (k == 1)
				printf(".\n");
			else
				printf("s.\n");
			exit(0);
		}
	printf("You just went off my scale!!!\n");
	exit(0);
}

/* label 90 */
void
die(int entry)
{
	int     i;
	if (entry != 99) {
		rspeak(23);
		oldloc2 = loc;
	}
	if (isclosing) {		/* 99 */
		rspeak(131);
		numdie++;
		done(2);
	}
	yea = yes(81 + numdie * 2, 82 + numdie * 2, 54);
	numdie++;
	if (numdie == maxdie || !yea)
		done(2);
	place[water] = 0;
	place[oil] = 0;
	if (toting(lamp))
		prop[lamp] = 0;
	for (i = 100; i >= 1; i--) {
		if (!toting(i))
			continue;
		k = oldloc2;
		if (i == lamp)
			k = 1;
		drop(i, k);
	}
	loc = 3;
	oldloc = loc;
}

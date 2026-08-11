/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/mount.h>

#include <err.h>
#include <mntopts.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <unistd.h>

static struct mntopt mopts[] = {
	MOPT_STDOPTS,
	MOPT_NULL
};

static void usage(void) __dead2;

int
main(int argc, char **argv)
{
	int ch;
	int mntflags;
	char mntpath[MAXPATHLEN];

	mntflags = 0;
	while ((ch = getopt(argc, argv, "o:")) != -1) {
		switch (ch) {
		case 'o':
			getmntopts(optarg, mopts, &mntflags, 0);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 1)
		usage();

	checkpath(argv[0], mntpath);
	if (mount("vmmfs", mntpath, mntflags, NULL) != 0)
		err(EX_OSERR, "mount vmm on %s", mntpath);
	return 0;
}

static void
usage(void)
{
	fprintf(stderr, "usage: mount_vmmfs [-o options] mountpoint\n");
	exit(EX_USAGE);
}

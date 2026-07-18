/*
 * Copyright (c) 2026 The DragonFly Project.
 * Mount helper for tarfs — structured tarfs_args (FreeBSD option parity).
 */

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <grp.h>
#include <mntopts.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include "../../sys/vfs/tarfs/tarfs_mount.h"

static struct mntopt mopts[] = {
	MOPT_STDOPTS,
	MOPT_UPDATE,
	MOPT_NULL
};

static uid_t
a_uid(const char *s)
{
	struct passwd *pw;
	char *ep;
	uid_t uid;

	if ((pw = getpwnam(s)) != NULL)
		return (pw->pw_uid);
	uid = (uid_t)strtoul(s, &ep, 10);
	if (*ep || ep == s)
		errx(EX_USAGE, "%s: bad user name", s);
	return (uid);
}

static gid_t
a_gid(const char *s)
{
	struct group *gr;
	char *ep;
	gid_t gid;

	if ((gr = getgrnam(s)) != NULL)
		return (gr->gr_gid);
	gid = (gid_t)strtoul(s, &ep, 10);
	if (*ep || ep == s)
		errx(EX_USAGE, "%s: bad group name", s);
	return (gid);
}

static mode_t
a_mode(const char *s)
{
	char *ep;
	mode_t mode;

	mode = (mode_t)strtoul(s, &ep, 8);
	if (*ep || ep == s)
		errx(EX_USAGE, "%s: bad mode", s);
	return (mode);
}

/*
 * Split -o into FreeBSD-style tarfs keys and standard mntopts tokens.
 * Unknown tokens are ignored by getmntopts only if we filter first.
 */
static void
parse_o(char *optstr, struct tarfs_args *args, int *mntflags)
{
	char *p, *comma, *std = NULL, *sp;

	/* rebuild a string of only standard options for getmntopts */
	std = calloc(1, strlen(optstr) + 2);
	if (std == NULL)
		err(EX_OSERR, "calloc");
	sp = std;

	for (p = optstr; p != NULL && *p != '\0'; ) {
		comma = strchr(p, ',');
		if (comma)
			*comma = '\0';
		if (strncmp(p, "uid=", 4) == 0)
			args->uid = a_uid(p + 4);
		else if (strncmp(p, "gid=", 4) == 0)
			args->gid = a_gid(p + 4);
		else if (strncmp(p, "mode=", 5) == 0)
			args->mode = a_mode(p + 5);
		else if (strncmp(p, "as=", 3) == 0)
			strlcpy(args->as, p + 3, sizeof(args->as));
		else if (strcmp(p, "verify") == 0)
			args->flags |= TARFSMNT_VERIFY;
		else {
			/* standard mount option token */
			if (sp != std)
				*sp++ = ',';
			strcpy(sp, p);
			sp += strlen(p);
		}
		if (comma) {
			*comma = ',';
			p = comma + 1;
		} else
			break;
	}
	if (std[0] != '\0')
		getmntopts(std, mopts, mntflags, 0);
	free(std);
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: mount_tarfs [-o options] tarfile mount_point\n"
	    "  options: uid=<u>, gid=<g>, mode=<oct>, as=<name>, verify,\n"
	    "           plus standard mount(8) options\n");
	exit(EX_USAGE);
}

int
main(int argc, char **argv)
{
	struct vfsconf vfc;
	struct tarfs_args args;
	struct stat st;
	char mntpath[MAXPATHLEN];
	char tarpath[MAXPATHLEN];
	char *ostr;
	int ch, error, mntflags = 0;

	bzero(&args, sizeof(args));

	while ((ch = getopt(argc, argv, "o:")) != -1) {
		switch (ch) {
		case 'o':
			ostr = strdup(optarg);
			if (ostr == NULL)
				err(EX_OSERR, "strdup");
			parse_o(ostr, &args, &mntflags);
			free(ostr);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 2)
		usage();

	mntflags |= MNT_RDONLY;

	error = getvfsbyname("tarfs", &vfc);
	if (error && vfsisloadable("tarfs")) {
		if (vfsload("tarfs") != 0)
			err(EX_OSERR, "vfsload(tarfs)");
		endvfsent();
		error = getvfsbyname("tarfs", &vfc);
	}
	if (error)
		errx(EX_OSERR, "tarfs filesystem not available");

	if (realpath(argv[0], tarpath) == NULL)
		err(EX_USAGE, "%s", argv[0]);
	checkpath(argv[1], mntpath);

	if (stat(tarpath, &st) != 0)
		err(EX_USAGE, "%s", tarpath);
	if (!S_ISREG(st.st_mode))
		errx(EX_USAGE, "%s: not a regular file", tarpath);

	args.magic = TARFS_ARGS_MAGIC;
	args.version = TARFS_ARGS_VERSION;
	strlcpy(args.fspec, tarpath, sizeof(args.fspec));

	if (mount(vfc.vfc_name, mntpath, mntflags, &args) != 0)
		err(EX_OSERR, "mount %s on %s", tarpath, mntpath);
	return (0);
}

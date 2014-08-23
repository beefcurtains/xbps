/*-
 * Copyright (c) 2008-2014 Juan Romero Pardines.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>
#include <unistd.h>
#include <getopt.h>

#include <xbps.h>
#include "defs.h"

static void __attribute__((noreturn))
usage(bool fail)
{
	fprintf(stdout,
	    "Usage: xbps-install [OPTIONS] [PKGNAME...]\n\n"
	    "OPTIONS\n"
	    " -A --automatic           Set automatic installation mode\n"
	    " -C --config <file>       Full path to configuration file\n"
	    " -c --cachedir <dir>      Full path to cachedir\n"
	    " -d --debug               Debug mode shown to stderr\n"
	    " -f --force               Force package re-installation\n"
	    "                          If specified twice, all files will be\n"
	    "                          overwritten.\n"
	    " -h --help                Print help usage\n"
	    " -n --dry-run             Dry-run mode\n"
	    " -R,--repository=<url>    Add repository to the top of the list.\n"
	    "                          This option can be specified multiple times.\n"
	    " -r --rootdir <dir>       Full path to rootdir\n"
	    " -S --sync                Sync remote repository index\n"
	    " -u --update              Update target package(s)\n"
	    " -v --verbose             Verbose messages\n"
	    " -y --yes                 Assume yes to all questions\n"
	    " -V --version             Show XBPS version\n");
	exit(fail ? EXIT_FAILURE : EXIT_SUCCESS);
}

static void
unpack_progress_cb(struct xbps_unpack_cb_data *xpd, void *cbdata _unused)
{
	if (xpd->entry == NULL || xpd->entry_total_count <= 0)
		return;

	printf("%s: unpacked %sfile `%s' (%" PRIi64 " bytes)\n",
	    xpd->pkgver,
	    xpd->entry_is_conf ? "configuration " : "", xpd->entry,
	    xpd->entry_size);
}

static int
repo_import_key_cb(struct xbps_repo *repo, void *arg _unused, bool *done _unused)
{
	int rv;

	 if ((rv = xbps_repo_key_import(repo)) != 0)
		fprintf(stderr, "Failed to import pubkey from %s: %s\n",
		    repo->uri, strerror(rv));

	 return rv;
}

int
main(int argc, char **argv)
{
	const char *shortopts = "AC:c:dfhnR:r:SuVvy";
	const struct option longopts[] = {
		{ "automatic", no_argument, NULL, 'A' },
		{ "config", required_argument, NULL, 'C' },
		{ "cachedir", required_argument, NULL, 'c' },
		{ "debug", no_argument, NULL, 'd' },
		{ "force", no_argument, NULL, 'f' },
		{ "help", no_argument, NULL, 'h' },
		{ "dry-run", no_argument, NULL, 'n' },
		{ "repository", required_argument, NULL, 'R' },
		{ "rootdir", required_argument, NULL, 'r' },
		{ "sync", no_argument, NULL, 'S' },
		{ "update", no_argument, NULL, 'u' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "version", no_argument, NULL, 'V' },
		{ "yes", no_argument, NULL, 'y' },
		{ NULL, 0, NULL, 0 }
	};
	struct xbps_handle xh;
	struct xferstat xfer;
	const char *rootdir, *cachedir, *conffile;
	int i, c, flags, rv, fflag = 0;
	bool sync, yes, reinstall, drun, update;
	int maxcols;

	rootdir = cachedir = conffile = NULL;
	flags = rv = 0;
	sync = yes = reinstall = drun = update = false;

	memset(&xh, 0, sizeof(xh));

	while ((c = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1) {
		switch (c) {
		case 'A':
			flags |= XBPS_FLAG_INSTALL_AUTO;
			break;
		case 'C':
			conffile = optarg;
			break;
		case 'c':
			cachedir = optarg;
			break;
		case 'd':
			flags |= XBPS_FLAG_DEBUG;
			break;
		case 'f':
			fflag++;
			if (fflag > 1)
				flags |= XBPS_FLAG_FORCE_UNPACK;
			reinstall = true;
			break;
		case 'h':
			usage(false);
			/* NOTREACHED */
		case 'n':
			drun = true;
			break;
		case 'R':
			if (xh.repositories == NULL)
				xh.repositories = xbps_array_create();

			xbps_array_add_cstring_nocopy(xh.repositories, optarg);
			break;
		case 'r':
			rootdir = optarg;
			break;
		case 'S':
			sync = true;
			break;
		case 'u':
			update = true;
			break;
		case 'v':
			flags |= XBPS_FLAG_VERBOSE;
			break;
		case 'V':
			printf("%s\n", XBPS_RELVER);
			exit(EXIT_SUCCESS);
		case 'y':
			yes = true;
			break;
		case '?':
		default:
			usage(true);
			/* NOTREACHED */
		}
	}
	if ((!update && !sync) && (argc == optind))
		usage(true);

	/*
	 * Initialize libxbps.
	 */
	xh.state_cb = state_cb;
	xh.fetch_cb = fetch_file_progress_cb;
	xh.fetch_cb_data = &xfer;
	if (rootdir)
		strncpy(xh.rootdir, rootdir, sizeof(xh.rootdir));
	if (cachedir)
		strncpy(xh.cachedir, cachedir, sizeof(xh.cachedir));
	if (conffile)
		strncpy(xh.conffile, conffile, sizeof(xh.conffile));
	xh.flags = flags;
	if (flags & XBPS_FLAG_VERBOSE)
		xh.unpack_cb = unpack_progress_cb;

	if ((rv = xbps_init(&xh)) != 0) {
		xbps_error_printf("Failed to initialize libxbps: %s\n",
		    strerror(rv));
		exit(EXIT_FAILURE);
	}

	maxcols = get_maxcols();

	/* Sync remote repository data and import keys from remote repos */
	if (sync && !drun) {
		if ((rv = xbps_rpool_sync(&xh, NULL)) != 0)
			exit(rv);
		rv = xbps_rpool_foreach(&xh, repo_import_key_cb, NULL);
		if (rv != 0)
			exit(rv);
	}

	if (sync && !update && (argc == optind))
		exit(EXIT_SUCCESS);

	if (!drun && (rv = xbps_pkgdb_lock(&xh)) != 0) {
		fprintf(stderr, "Failed to lock the pkgdb: %s\n", strerror(rv));
		exit(rv);
	}

	if (update && (argc == optind)) {
		/* Update all installed packages */
		rv = dist_upgrade(&xh, maxcols, yes, drun);
	} else if (update) {
		/* Update target packages */
		for (i = optind; i < argc; i++) {
			rv = update_pkg(&xh, argv[i]);
			if (rv != 0) {
				xbps_pkgdb_unlock(&xh);
				exit(rv);
			}
		}
		rv = exec_transaction(&xh, maxcols, yes, drun);
	} else if (!update) {
		/* Install target packages */
		int npkgs = argc - optind;
		for (i = optind; i < argc; i++) {
			rv = install_new_pkg(&xh, argv[i], reinstall);
			if (npkgs >= 2 && rv == EEXIST) {
				rv = 0;
			} else if (rv != 0) {
				xbps_pkgdb_unlock(&xh);
				exit(rv);
			}
		}
		rv = exec_transaction(&xh, maxcols, yes, drun);
	}

	if (!drun)
		xbps_pkgdb_unlock(&xh);

	exit(rv);
}

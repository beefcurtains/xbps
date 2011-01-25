/*-
 * Copyright (c) 2008-2010 Juan Romero Pardines.
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
#include <errno.h>

#include <xbps_api.h>
#include "defs.h"
#include "../xbps-repo/defs.h"

static int
pkg_remove_and_purge(const char *pkgname, const char *version, bool purge)
{
	int rv = 0;

	printf("Removing package %s-%s ...\n", pkgname, version);

	if ((rv = xbps_remove_pkg(pkgname, version, false)) != 0) {
		xbps_error_printf("unable to remove %s-%s (%s).\n",
		    pkgname, version, strerror(errno));
		return rv;
	}
	if (purge) {
		printf(" Purging ... ");
		(void)fflush(stdout);
		if ((rv = xbps_purge_pkg(pkgname, false)) != 0) {
			xbps_error_printf("unable to purge %s-%s "
			    "(%s).\n", pkgname, version,
			    strerror(errno));
			return rv;
		}
		printf("done.\n");
	}

	return rv;
}

int
xbps_autoremove_pkgs(bool yes, bool purge)
{
	prop_array_t orphans = NULL;
	prop_object_t obj = NULL;
	prop_object_iterator_t iter = NULL;
	const char *pkgver, *pkgname, *version;
	int rv = 0;

	/*
	 * Removes orphan pkgs. These packages were installed
	 * as dependency and any installed package does not depend
	 * on it currently.
	 */
	orphans = xbps_find_orphan_packages();
	if (orphans == NULL)
		return errno;

	if (prop_array_count(orphans) == 0) {
		printf("There are not orphaned packages currently.\n");
		goto out;
	}

	iter = prop_array_iterator(orphans);
	if (iter == NULL) {
		rv = errno;
		goto out;
	}

	printf("The following packages were installed automatically\n"
	    "(as dependencies) and aren't needed anymore:\n\n");
	while ((obj = prop_object_iterator_next(iter)) != NULL) {
		prop_dictionary_get_cstring_nocopy(obj, "pkgver", &pkgver);
		print_package_line(pkgver, false);
	}
	prop_object_iterator_reset(iter);
	printf("\n\n");

	if (!yes && !xbps_noyes("Do you want to continue?")) {
		printf("Cancelled!\n");
		goto out;
	}

	while ((obj = prop_object_iterator_next(iter)) != NULL) {
		prop_dictionary_get_cstring_nocopy(obj, "pkgname", &pkgname);
		prop_dictionary_get_cstring_nocopy(obj, "version", &version);
		if ((rv = pkg_remove_and_purge(pkgname, version, purge)) != 0)
			goto out;
	}

out:
	if (iter)
		prop_object_iterator_release(iter);
	if (orphans)
		prop_object_release(orphans);

	return rv;
}

int
xbps_remove_installed_pkgs(int argc, char **argv, bool yes, bool purge,
			   bool force_rm_with_deps)
{
	prop_array_t sorted_pkgs;
	prop_array_t reqby;
	prop_dictionary_t dict;
	size_t x;
	const char *version, *pkgver, *pkgname;
	int i, rv = 0;
	bool found = false, reqby_force = false;

	sorted_pkgs = prop_array_create();
	if (sorted_pkgs == NULL)
		return -1;

	/*
	 * First check if package is required by other packages.
	 */
	for (i = 1; i < argc; i++) {
		dict = xbps_find_pkg_dict_installed(argv[i], false);
		if (dict == NULL) {
			printf("Package %s is not installed.\n", argv[i]);
			continue;
		}
		prop_array_add(sorted_pkgs, dict);
		prop_dictionary_get_cstring_nocopy(dict, "pkgver", &pkgver);
		found = true;
		reqby = prop_dictionary_get(dict, "requiredby");
		if (reqby != NULL && prop_array_count(reqby) > 0) {
			xbps_warn_printf("%s IS REQUIRED BY %u PACKAGES!\n",
			    pkgver, prop_array_count(reqby));
			reqby_force = true;
		}
		prop_object_release(dict);
	}
	if (!found) {
		prop_object_release(sorted_pkgs);
		return 0;
	}


	/*
	 * Show the list of going-to-be removed packages.
	 */
	printf("The following packages will be removed:\n\n");
	for (x = 0; x < prop_array_count(sorted_pkgs); x++) {
		dict = prop_array_get(sorted_pkgs, x);
		prop_dictionary_get_cstring_nocopy(dict, "pkgver", &pkgver);
		print_package_line(pkgver, false);
	}
	printf("\n\n");
	if (!yes && !xbps_noyes("Do you want to continue?")) {
		printf("Cancelling!\n");
		prop_object_release(sorted_pkgs);
		return 0;
	}
	if (reqby_force && !force_rm_with_deps) {
		printf("\nYou haven't specified the -F flag to force removal with dependencies. The package(s)\n"
		    "you are going to remove are required by other installed packages, therefore\n"
		    "it might break packages that currently depend on them. If you are entirely sure\n"
		    "that's what you want, use 'xbps-bin -F remove ...' to continue with the operation.\n");
		prop_object_release(sorted_pkgs);
		return 0;
	} else if (reqby_force && force_rm_with_deps)
		xbps_warn_printf("Forcing removal! you've been alerted.\n");

	for (x = 0; x < prop_array_count(sorted_pkgs); x++) {
		dict = prop_array_get(sorted_pkgs, x);
		prop_dictionary_get_cstring_nocopy(dict, "pkgname", &pkgname);
		prop_dictionary_get_cstring_nocopy(dict, "version", &version);
		if ((rv = pkg_remove_and_purge(pkgname, version, purge)) != 0) {
			prop_object_release(sorted_pkgs);
			return rv;
		}
	}
	prop_object_release(sorted_pkgs);

	return 0;
}

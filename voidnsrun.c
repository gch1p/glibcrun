#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <linux/limits.h>

const char *var_name = "VOIDNSRUN_DIR";
const char *prog_version = "1.0";

#define USERMOUNTS_MAX 8

#define ARRAY_SIZE(x)  (sizeof(x) / sizeof((x)[0]))
#define ERROR(f_, ...) fprintf(stderr, (f_), ##__VA_ARGS__)

bool isdir(const char *s)
{
	struct stat st;
	int result = stat(s, &st);
	if (result == -1)
		ERROR("stat: %s\n", strerror(errno));
	return result == 0 && S_ISDIR(st.st_mode);
}

void usage(const char *progname)
{
	printf("Usage: %s [OPTIONS] PROGRAM [ARGS]\n", progname);
	printf("\n"
			"Options:\n"
			"	-m <path>: add bind mount\n"
			"	-r <path>: altroot path. If this option is not present,\n"
			"	           %s environment variable is used.\n"
			"	-h:        print this help\n"
			"	-v:        print version\n",
			var_name);
}

bool mountlist(
	const char *dirptr,
	size_t dirlen,
	const char **mountpoints,
	size_t len,
	bool ignore_missing)
{
	char buf[PATH_MAX];
	for (size_t i = 0; i < len; i++) {
		/* Check if it's safe to proceed. */
		if (dirlen + strlen(mountpoints[i]) >= PATH_MAX) {
			ERROR("error: path %s%s is too large.\n", dirptr, mountpoints[i]);
			return false;
		}

		strcpy(buf, dirptr);
		strcat(buf, mountpoints[i]);
		if (!isdir(buf)) {
			if (ignore_missing)
				continue;
			ERROR("error: source mount dir %s does not exists.\n", buf);
			return false;
		}
		if (!isdir(mountpoints[i])) {
			ERROR("error: mountpoint %s does not exists.\n", mountpoints[i]);
			return false;
		}
		if (mount(buf, mountpoints[i], NULL, MS_BIND|MS_REC, NULL) == -1) {
			ERROR("mount: failed to mount %s: %s\n",
				mountpoints[i], strerror(errno));
			return false;
		}
	}
	return true;
}

bool isxbpscommand(const char *s)
{
	const char *commands[] = {
		"/xbps-install",
		"/xbps-remove",
		"/xbps-reconfigure"
	};
	for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
		const char *command = commands[i];
		if (!strcmp(s, command+1))
			return true;
		char *slash = strrchr(s, '/');
		if (slash && !strcmp(slash, command))
			return true;
	}
	return false;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 0;
	}

	int result;
	char *dir = NULL;
	size_t dirlen;
	const char *usermounts[USERMOUNTS_MAX] = {0};
	int usermounts_num = 0;
	int c;
	while ((c = getopt(argc, argv, "vhm:r:")) != -1) {
		switch (c) {
			case 'v':
				printf("%s\n", prog_version);
				return 0;
			case 'h':
				usage(argv[0]);
				return 0;
			case 'r':
				dir = optarg;
				break;
			case 'm':
				if (usermounts_num < USERMOUNTS_MAX) {
					usermounts[usermounts_num++] = optarg;
					break;
				} else {
					ERROR("error: only up to %d user mounts allowed.\n",
						USERMOUNTS_MAX);
					return 1;
				}
			case '?':
				return 1;
		}
	}

	/* Get alternative root dir. */
	if (!dir)
		dir = getenv(var_name);
	if (!dir) {
		ERROR("error: environment variable %s not found.\n", var_name);
		return 1;
	}

	/* Validate it. */
	if (!isdir(dir)) {
		ERROR("error: %s is not a directory.\n", dir);
		return 1;
	}

	dirlen = strlen(dir);

	/* Do the unshare magic. */
	result = unshare(CLONE_NEWNS);
	if (result == -1) {
		ERROR("unshare: %s\n", strerror(errno));
		return 1;
	}

	/* Mount stuff from altroot to our private namespace. */
	const char *mountpoints[3] = {"/usr", NULL, NULL};
	if (isxbpscommand(argv[optind])) {
		mountpoints[1] = "/var";
		mountpoints[2] = "/etc";
	} else {
		mountpoints[1] = "/var/db/xbps";
		mountpoints[2] = "/etc/xbps.d";
	}
	if (!mountlist(dir, dirlen, mountpoints, ARRAY_SIZE(mountpoints), true))
		return 1;
	if (usermounts_num > 0 &&
		!mountlist(dir, dirlen, usermounts, usermounts_num, false))
		return 1;

	/* Drop root. */
	uid_t uid = getuid();
	gid_t gid = getgid();

	result = setreuid(uid, uid);
	if (result == -1) {
		ERROR("setreuid: %s\n", strerror(errno));
		return 1;
	}

	result = setregid(gid, gid);
	if (result == -1) {
		ERROR("setregid: %s\n", strerror(errno));
		return 1;
	}
	
	/* Launch program. */
	result = execvp(argv[optind], (char *const *)argv+optind);
	if (result == -1) {
		ERROR("execvp(%s): %s\n", argv[optind], strerror(errno));
		return 1;
	}

	return 0;
}

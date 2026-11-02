/*
 * hakios-cmd: run the command the host asked for, and show what it printed.
 *
 * The host writes its request into the phone's pmsg zone under Android
 * (tools/ios-cmd.sh): "DARWIN-X6739:BOOT-XNU:batt=...:extra=cmd=PROGRAM ARGS".
 * The loader passes the extra arguments on to the kernel's command line, and the
 * kernel's bootargs are readable from here (sysctl kern.bootargs), so the command
 * arrives without anything being reflashed.
 *
 * The command is one program with its arguments, separated by blanks:
 *   cmd=/redwood/bin/rwtouch tap 0.5 0.5
 * It ends at the loader's own arguments, which follow. A program name without a
 * '/' is looked for in /redwood/bin (the small root's programs) and then in /bin.
 *
 * When it runs: the phone's console is a fixed ring of 256 KB, and the kernel
 * fills it with what the running system says. What this program prints early is
 * gone before the host can read it, so the program waits until the end of the
 * run -- the loader's own "loader_return_after=" says how long that is -- and
 * prints then. What it prints is the last thing in the console, and so the first
 * thing the host finds.
 *
 * The command's own standard output is this program's, which is the job's:
 * /dev/console. What it prints lands in the kernel's console and so in pstore,
 * where the host reads it after the run (tools/ios-cmd.sh).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define ARGS_MAX 1024
#define PATH_MAX_LEN 256

/* seconds before the run ends that this program starts, so that its output has
   the console to itself */
#define LEAD_SECONDS 8

static char *
bootargs(void)
{
	static char args[ARGS_MAX];
	size_t size = sizeof(args);

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
		return NULL;
	}
	args[sizeof(args) - 1] = '\0';
	return args;
}

/* the kernel's clock: seconds since the kernel started, the same clock the
   kernel's own messages count in */
static double
uptime(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0.0;
	}
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int
main(void)
{
	char *args = bootargs();
	char *cmd, *end, *battery;
	char path[PATH_MAX_LEN];
	char *argv[64];
	int argc = 0;
	size_t len;

	if (args == NULL) {
		printf("hakios-cmd: the kernel's bootargs are not readable\n");
		return 1;
	}

	/* the command ends at the loader's own arguments, which follow it */
	cmd = strstr(args, "cmd=");
	if (cmd == NULL) {
		return 0; /* nobody asked for anything: this is a plain run */
	}
	cmd += 4;
	end = cmd;
	while (*end && *end != ';') {
		end++;
	}
	*end = '\0';
	battery = strstr(cmd, " battery=");
	if (battery != NULL) {
		*battery = '\0';
	}
	while (*cmd == ' ') {
		cmd++;
	}
	len = strlen(cmd);
	while (len > 0 && cmd[len - 1] == ' ') {
		cmd[--len] = '\0';
	}
	if (len == 0) {
		return 0;
	}

	/* split into words: the program and its arguments */
	for (char *word = strtok(cmd, " "); word != NULL && argc < 63; word = strtok(NULL, " ")) {
		argv[argc++] = word;
	}
	if (argc == 0) {
		return 0;
	}
	argv[argc] = NULL;

	/* a bare name is one of the small root's programs */
	if (strchr(argv[0], '/') == NULL) {
		snprintf(path, sizeof(path), "/redwood/bin/%s", argv[0]);
		if (access(path, X_OK) != 0) {
			snprintf(path, sizeof(path), "/bin/%s", argv[0]);
		}
		argv[0] = path;
	}

	/*
	 * Wait for the end of the run, so that what the command prints is still in
	 * the console when the host reads it. The loader says when the run ends.
	 */
	{
		char *until = strstr(args, "loader_return_after=");
		double now = uptime();
		double wake = 0.0;

		if (until != NULL) {
			wake = atof(until + 19) - LEAD_SECONDS;
		}
		if (wake > now + 0.5) {
			double left = wake - now;
			printf("hakios-cmd: uptime %.1f s, waiting %.0f s for the end of the run\n", now, left);
			fflush(stdout);
			sleep((unsigned)(left * 1000.0));
		}
	}

	printf("hakios-cmd: uptime %.1f s, running: %s\n", uptime(), argv[0]);
	fflush(stdout);
	execv(argv[0], argv);
	printf("hakios-cmd: %s: %s\n", argv[0], strerror(errno));
	return 127;
}

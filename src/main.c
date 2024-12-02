// SPDX-License-Identifier: GPL-3.0-or-later

/*
 *	B1B - Bonding mode 1 bridge helper
 *
 *	main.c - main loop & utility functions
 *
 *	Copyright 2024 Ian Pilcher <arequipeno@gmail.com>
 */


#define _GNU_SOURCE  /* for ppoll() */

#include "b1b.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <net/if.h>
#include <poll.h>
#include <unistd.h>

#include <libmnl/libmnl.h>


_Bool b1b_debug;
static _Bool b1b_use_syslog;
static sig_atomic_t b1b_exit_flag;


/*
 *
 *	Utility functions
 *
 */

__attribute__((format(printf, 4, 0)))
void b1b_vlog(const char *restrict const file, const int line, const int level,
	      const char *restrict const format, va_list ap)
{
	static const char *const level_names[] = {
		[LOG_EMERG]	= "EMERGENCY", 	/* not used */
		[LOG_ALERT]	= "ABORT",
		[LOG_CRIT]	= "FATAL",
		[LOG_ERR]	= "ERROR",
		[LOG_WARNING]	= "WARNING",
		[LOG_NOTICE]	= "NOTICE",
		[LOG_INFO]	= "INFO",
		[LOG_DEBUG]	= "DEBUG"
	};

	/*
	 * This function relies on the fact that stderr is line buffered, which
	 * is set by main().
	 */

	if (level > LOG_INFO && !b1b_debug)
		return;

	if (b1b_use_syslog)
		fprintf(stderr, "<%d>", level);

	if (b1b_debug)
		fprintf(stderr, "%s:%d: ", file, line);

	fprintf(stderr, "%s: ", level_names[level]);
	vfprintf(stderr, format, ap);
	fputc('\n', stderr);
}

__attribute__((format(printf, 4, 5)))
void b1b_log(const char *restrict const file, const int line, const int level,
	     const char *restrict const format, ...)
{
	va_list ap;

	va_start(ap, format);
	b1b_vlog(file, line, level, format, ap);
	va_end(ap);
}

void *b1b_zalloc(const size_t size, const char *const file, const int line)
{
	void *result;

	if ((result = calloc(1, size)) == NULL) {
		b1b_log(file, line, LOG_CRIT,
			"Cannot allocate %zu bytes: %m", size);
		abort();
	}

	return result;
}

char *b1b_strdup(const char *restrict const s, const char *restrict const file,
		 const int line)
{
	size_t size;
	char *result;

	size = strlen(s) + 1;
	result = b1b_zalloc(size, file, line);
	memcpy(result, s, size);
	return result;
}

__attribute__((format(printf, 4, 5)))
int b1b_asprintf(const char *restrict const file, const int line,
		 char **restrict const strp, const char *restrict const fmt,
		 ...)
{
	va_list ap;
	int result;

	va_start(ap, fmt);

	if ((result = vasprintf(strp, fmt, ap)) < 0) {
		b1b_log(file, line, LOG_CRIT,
			"Failed to allocate and format string: %m");
		abort();
	}

	va_end(ap);

	return result;
}


/*
 *
 *	Command line parsing
 *
 */

static _Bool b1b_opt_match(const char *restrict const arg,
			   const char *restrict const short_opt,
			   const char *restrict const long_opt)
{
	return strcmp(arg, short_opt) == 0 || strcmp(arg, long_opt) == 0;
}

static int b1b_parse_args(const int argc, char **const argv)
{
	_Bool log_dest_set;
	int i;

#if 0
	if (argc < 2)
		B1B_FATAL("Insufficient number of arguments");
#endif

	log_dest_set = 0;

	for (i = 1; i < argc; ++i) {

		if (*argv[i] != '-')
			break;

		if (b1b_opt_match(argv[i], "-l", "--syslog")) {
			if (log_dest_set) {
				B1B_FATAL("Duplicate/conflicting option: %s: "
						"Log destination already set",
					  argv[i]);
			}
			b1b_use_syslog = 1;
			log_dest_set = 1;
			continue;
		}

		if (b1b_opt_match(argv[i], "-e", "--stderr")) {
			if (log_dest_set) {
				B1B_FATAL("Duplicate/conflicting option: %s: "
						"Log destination already set",
					  argv[i]);
			}
			b1b_use_syslog = 0;
			log_dest_set = 1;
			continue;
		}

		if (b1b_opt_match(argv[i], "-d", "--debug")) {
			if (b1b_debug) {
				B1B_FATAL("Duplicate/conflicting option: %s: "
						"Debug log level already set",
					  argv[i]);
			}
			b1b_debug = 1;
			continue;
		}

		B1B_FATAL("Invalid option: %s", argv[i]);
	}

	return i;
}

/*
 *
 *	"Global" session
 *
 */

static struct b1b_global_session *b1b_gs_alloc(void)
{
	struct b1b_global_session *gs;
	size_t size;

	size = sizeof *gs - sizeof gs->nlmsg + MNL_SOCKET_BUFFER_SIZE;
	gs = B1B_ZALLOC(size);
	gs->bufsize = MNL_SOCKET_BUFFER_SIZE;
	gs->ovssock = -1;

	return gs;
}

static void b1b_gs_free(struct b1b_global_session *const gs)
{
	unsigned int i;

	if (gs->ovssock >= 0 && close(gs->ovssock) < 0)
		B1B_ERR("Failed to close UNIX socket: %m");

	if (close(gs->arpsock) < 0)
		B1B_ERR("Failed to close ARP socket: %m");

	if (mnl_socket_close(gs->nlsock) < 0)
		B1B_ERR("Failed to close netlink request socket: %m");

	if (mnl_socket_close(gs->mcsock) < 0)
		B1B_ERR("Failed to close netlink multicast socket: %m");

	for (i = 0; i < gs->bcount; ++i) {
		free(gs->bonds[i].brname);
		free(gs->bonds[i].ifname);
	}

	free(gs->ovssock_path);
	free(gs->bonds);
	free(gs);
}


/*
 *
 *	Signal handling
 *
 */

static void b1b_catch_signal(const int signum __attribute__((unused)))
{
	b1b_exit_flag = 1;
}

static void b1b_signal_setup(sigset_t *const oldmask)
{
	struct sigaction sa;
	sigset_t mask;

	if (sigemptyset(&mask) != 0)
		B1B_FATAL("sigemptyset %m");
	if (sigaddset(&mask, SIGTERM) != 0)
		B1B_FATAL("sigaddset(SIGTERM): %m");
	if (sigaddset(&mask, SIGINT) != 0)
		B1B_FATAL("sigaddset(SIGINT): %m");

	sa.sa_handler = b1b_catch_signal;
	sa.sa_mask = mask;
	sa.sa_flags = SA_RESETHAND;

	if (sigprocmask(SIG_BLOCK, &mask, oldmask) != 0)
		B1B_FATAL("sigprocmask: %m");

	if (sigaction(SIGTERM, &sa, NULL) != 0)
		B1B_FATAL("sigaction(SIGTERM): %m");
	if (sigaction(SIGINT, &sa, NULL) != 0)
		B1B_FATAL("sigaction(SIGINT): %m");
}


/*
 *
 *	Main loop
 *
 */

int main(const int argc, char **const argv)
{
	struct b1b_global_session *gs;
	struct pollfd pfd;
	sigset_t ppmask;
	int bindex;

	setlinebuf(stderr);
	b1b_use_syslog = !isatty(STDERR_FILENO);
	bindex = b1b_parse_args(argc, argv);
	gs = b1b_gs_alloc();
	b1b_nlsock_open(gs);
	b1b_mcsock_open(gs);
	b1b_arpsock_open(gs);

	if (bindex < argc)
		b1b_parse_bonds(gs, argc, argv, bindex);
	else
		b1b_detect_bonds(gs);

	pfd.fd = mnl_socket_get_fd(gs->mcsock);
	pfd.events = POLLIN;

	B1B_INFO("Ready");

	b1b_signal_setup(&ppmask);

	while (!b1b_exit_flag) {

		if (ppoll(&pfd, 1, NULL, &ppmask) < 0) {
			if (errno == EINTR)
				continue;
			B1B_FATAL("Failed to wait for netlink messages: %m");
		}

		if (pfd.revents & ~POLLIN) {
			B1B_FATAL("Unexpected event type(s) on netlink socket: "
					"%04hx",
				  pfd.revents);
		}

		b1b_mcast_process(gs);
	}

	B1B_INFO("Exiting");

	b1b_gs_free(gs);

	return 0;
}

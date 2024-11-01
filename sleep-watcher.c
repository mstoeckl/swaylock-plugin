#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "config.h"
#include "log.h"
#if HAVE_SYSTEMD
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#elif HAVE_ELOGIND
#include <elogind/sd-bus.h>
#include <elogind/sd-login.h>
#endif

#if HAVE_SYSTEMD || HAVE_ELOGIND
#define DBUS_LOGIND_SERVICE "org.freedesktop.login1"
#define DBUS_LOGIND_PATH "/org/freedesktop/login1"
#define DBUS_LOGIND_MANAGER_INTERFACE "org.freedesktop.login1.Manager"
#define DBUS_LOGIND_SESSION_INTERFACE "org.freedesktop.login1.Session"

struct state {
	char *session_name;
	int sleep_lock_fd;
	struct sd_bus *bus;
	int comm_r;
	int comm_w;
};

static void acquire_inhibitor_lock(struct state *state,
		const char *type, const char *mode) {
	sd_bus_message *msg = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	int ret = sd_bus_call_method(state->bus, DBUS_LOGIND_SERVICE, DBUS_LOGIND_PATH,
		DBUS_LOGIND_MANAGER_INTERFACE, "Inhibit", &error, &msg,
		"ssss", type, "swaylock-sleep-helper",
		"Waiting to ensure screen lock grace period is ended before sleep", mode);
	if (ret < 0) {
		swaylock_log(LOG_ERROR,
			"Failed to send %s inhibit signal: %s", type, error.message);
		goto cleanup;
	}

	ret = sd_bus_message_read(msg, "h", &state->sleep_lock_fd);
	if (ret < 0) {
		errno = -ret;
		swaylock_log_errno(LOG_ERROR,
			"Failed to parse D-Bus response for %s inhibit", type);
		goto cleanup;
	}

	state->sleep_lock_fd = fcntl(state->sleep_lock_fd, F_DUPFD_CLOEXEC, 3);
	if (state->sleep_lock_fd >= 0) {
		swaylock_log(LOG_DEBUG, "Got %s lock: %d", type, state->sleep_lock_fd);
	} else {
		swaylock_log_errno(LOG_ERROR, "Failed to copy %s lock fd", type);
	}

cleanup:
	sd_bus_error_free(&error);
	sd_bus_message_unref(msg);
}

static void release_inhibitor_lock(struct state *state) {
	if (state->sleep_lock_fd >= 0) {
		swaylock_log(LOG_DEBUG, "Releasing inhibitor lock %d", state->sleep_lock_fd);
		close(state->sleep_lock_fd);
	}
	state->sleep_lock_fd = -1;
}

static int prepare_for_sleep(sd_bus_message *msg, void *userdata,
							 sd_bus_error *ret_error) {
	struct state *state = userdata;
	/* "b" apparently reads into an int, not a bool */
	int going_down = 1;
	int ret = sd_bus_message_read(msg, "b", &going_down);
	if (ret < 0) {
		errno = -ret;
		swaylock_log_errno(LOG_ERROR,
			"Failed to parse D-Bus response for Inhibit");
	}
	swaylock_log(LOG_DEBUG, "PrepareForSleep signal received %d", going_down);
	if (!going_down) {
		acquire_inhibitor_lock(state, "sleep", "delay");
		return 0;
	} else {
		assert(state->comm_w != -1);
		close(state->comm_w);
		state->comm_w = -1;

		char tmp;
		int ret = read(state->comm_r, &tmp, 1);
		if (ret == -1) {
			/* Error */
			swaylock_log_errno(LOG_ERROR,
				"Failed to read from comm pipe");
		} else if (ret == 0) {
			/* swaylock-plugin has confirmed receipt of message by closing
			 * the write end of comm_r */
			swaylock_log(LOG_DEBUG,
				"swaylock-plugin acknowledged start of sleep");
		} else {
			swaylock_log(LOG_ERROR,
				"Unexpected data on comm pipe");
		}
	}
	swaylock_log(LOG_DEBUG, "Prepare for sleep done");

	release_inhibitor_lock(state);
	return 0;
}

static void set_session(struct state *state) {
	sd_bus_message *msg = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	const char *session_name_tmp;

	int ret = sd_bus_call_method(state->bus, DBUS_LOGIND_SERVICE, DBUS_LOGIND_PATH,
		DBUS_LOGIND_MANAGER_INTERFACE, "GetSession", &error, &msg, "s", "auto");
	if (ret < 0) {
		swaylock_log(LOG_DEBUG,
					 "GetSession failed: %s", error.message);
		sd_bus_error_free(&error);
		sd_bus_message_unref(msg);

		ret = sd_bus_call_method(state->bus, DBUS_LOGIND_SERVICE, DBUS_LOGIND_PATH,
			DBUS_LOGIND_MANAGER_INTERFACE, "GetSessionByPID",
			&error, &msg, "u", getpid());
		if (ret < 0) {
			swaylock_log(LOG_DEBUG, "GetSessionByPID failed: %s", error.message);
			swaylock_log(LOG_ERROR, "Failed to find session");
			goto cleanup;
		}
	}

	ret = sd_bus_message_read(msg, "o", &session_name_tmp);
	if (ret < 0) {
		swaylock_log(LOG_ERROR,
					 "Failed to read session name");
		goto cleanup;
	}
	state->session_name = strdup(session_name_tmp);
	swaylock_log(LOG_DEBUG, "Using session: %s", state->session_name);

cleanup:
	sd_bus_error_free(&error);
	sd_bus_message_unref(msg);
}

int run(int comm_r, int comm_w) {
	struct state state = {0};
	state.sleep_lock_fd = -1;
	state.comm_r = comm_r;
	state.comm_w = comm_w;

	int ret = sd_bus_default_system(&state.bus);
	if (ret < 0) {
		errno = -ret;
		swaylock_log_errno(LOG_ERROR, "Failed to open D-Bus connection");
		return EXIT_FAILURE;
	}

	set_session(&state);

	ret = sd_bus_match_signal(state.bus, NULL, DBUS_LOGIND_SERVICE,
		DBUS_LOGIND_PATH, DBUS_LOGIND_MANAGER_INTERFACE,
		"PrepareForSleep", prepare_for_sleep, NULL);
	if (ret < 0) {
		errno = -ret;
		swaylock_log_errno(LOG_ERROR, "Failed to add D-Bus signal match : sleep");
		return EXIT_FAILURE;
	}
	acquire_inhibitor_lock(&state, "sleep", "delay");

	struct pollfd pfds[2];
	pfds[0].fd = state.comm_r;
	pfds[0].events = POLLIN;
	pfds[1].fd = sd_bus_get_fd(state.bus);
	pfds[1].events = POLLIN;

	while (true) {
		int ret = poll(pfds, 2, -1);
		if (ret == -1) {
			if (errno == EINTR) {
				continue;
			}
			swaylock_log_errno(LOG_ERROR, "poll failed, exiting");
			break;
		}
		if (pfds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
			char tmp;
			int ret = read(state.comm_r, &tmp, 1);
			if (ret == -1) {
				/* Error */
				swaylock_log_errno(LOG_ERROR,
					"Failed to read from comm pipe");
			} else if (ret == 0) {
				/* grace period ended, this program may exit */
				swaylock_log(LOG_DEBUG,
					"swaylock-plugin grace period ended");
			} else {
				swaylock_log(LOG_ERROR,
					"Unexpected data on comm pipe");
			}
			break;
		}

		if (pfds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
			int count = sd_bus_process(state.bus, NULL);
			sd_bus_flush(state.bus);
			if (count < 0) {
				swaylock_log_errno(LOG_ERROR, "sd_bus_process failed, exiting");
				break;
			}
		}
	}
	sd_bus_close(state.bus);
	swaylock_log(LOG_DEBUG, "Exiting");
	return EXIT_SUCCESS;
}
#endif


int main(int argc, char **argv) {
	if (argc != 3) {
		fprintf(stderr,
			"This is a helper program for swaylock-plugin to ensure that if a grace\n"
			"period is used, the screen will still lock when the system goes to sleep\n"
			"or hibernation. It is automatically run by swaylock-plugin when necessary.\n");
		return EXIT_FAILURE;
	}

	swaylock_log_init(LOG_DEBUG);

	char *end1 = NULL;
	int comm_w = strtol(argv[1], &end1, 10);
	char *end2 = NULL;
	int comm_r = strtol(argv[2], &end2, 10);
	if (*end1 != 0 || *end2 != 0) {
		swaylock_log(LOG_ERROR, "Failed to get communication pipes");
		return EXIT_FAILURE;
	}

#if !HAVE_SYSTEMD && !HAVE_ELOGIND
	(void)comm_r;
	(void)comm_w;
	return EXIT_FAILURE;
#else
	return run(comm_r, comm_w);
#endif
}

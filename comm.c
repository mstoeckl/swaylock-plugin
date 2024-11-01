#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include "comm.h"
#include "log.h"
#include "swaylock.h"
#include "password-buffer.h"

static int comm[2][2] = {{-1, -1}, {-1, -1}};

/* Read entire buffer, returning -1 on error, 0 on eof, nbytes on success */
static ssize_t read_all(int fd, void *buf, size_t nbytes) {
	ssize_t ret;
	ssize_t nread = 0;
	while ((size_t)nread < nbytes) {
		ret = read(fd, buf + nread, nbytes - nread);
		if (ret == -1 && errno == EINTR) {
			continue;
		} else if (ret == -1) {
			return ret;
		} else if (ret == 0) {
			return 0;
		} else {
			nread += ret;
		}
	}
	return nread;
}

/* Write entire buffer, returning -1 on error, nbytes on success */
static ssize_t write_all(int fd, const void *buf, size_t nbytes) {
	ssize_t ret;
	ssize_t nwrite = 0;
	while ((size_t)nwrite < nbytes) {
		ret = write(fd, buf + nwrite, nbytes - nwrite);
		if (ret == -1 && errno == EINTR) {
			continue;
		} else if (ret == -1) {
			return ret;
		} else {
			nwrite += ret;
		}
	}
	return nwrite;
}

ssize_t read_comm_request(char **buf_ptr) {
	size_t size;
	ssize_t amt = read_all(comm[0][0], &size, sizeof(size));
	if (amt == 0) {
		return 0;
	} else if (amt < 0) {
		swaylock_log_errno(LOG_ERROR, "read pw request");
		return -1;
	}
	swaylock_log(LOG_DEBUG, "received pw check request");
	char *buf = password_buffer_create(size);
	if (!buf) {
		return -1;
	}
	if (read_all(comm[0][0], buf, size) <= 0) {
		swaylock_log_errno(LOG_ERROR, "failed to read pw");
		return -1;
	}

	*buf_ptr = buf;
	return size;
}

bool write_comm_reply(bool success) {
	if (write_all(comm[1][1], &success, sizeof(success)) < 0) {
		swaylock_log_errno(LOG_ERROR, "failed to write pw check result");
		return false;
	}
	return true;
}

bool spawn_comm_child(void) {
	if (pipe(comm[0]) != 0) {
		swaylock_log_errno(LOG_ERROR, "failed to create pipe");
		return false;
	}
	if (pipe(comm[1]) != 0) {
		swaylock_log_errno(LOG_ERROR, "failed to create pipe");
		return false;
	}
	pid_t child = fork();
	if (child < 0) {
		swaylock_log_errno(LOG_ERROR, "failed to fork");
		return false;
	} else if (child == 0) {
		close(comm[0][1]);
		close(comm[1][0]);
		run_pw_backend_child();
	}
	close(comm[0][0]);
	close(comm[1][1]);
	return true;
}

bool write_comm_request(struct swaylock_password *pw) {
	bool result = false;

	size_t len = pw->len + 1;
	if (write_all(comm[0][1], &len, sizeof(len)) < 0) {
		swaylock_log_errno(LOG_ERROR, "Failed to request pw check");
		goto out;
	}

	if (write_all(comm[0][1], pw->buffer, len) < 0) {
		swaylock_log_errno(LOG_ERROR, "Failed to write pw buffer");
		goto out;
	}

	result = true;

out:
	clear_password_buffer(pw);
	return result;
}

bool read_comm_reply(void) {
	bool result = false;
	if (read_all(comm[1][0], &result, sizeof(result)) <= 0) {
		swaylock_log_errno(LOG_ERROR, "Failed to read pw result");
		result = false;
	}
	return result;
}

int get_comm_reply_fd(void) {
	return comm[1][0];
}

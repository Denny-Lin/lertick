/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "runtime.h"

int lt_runtime_lock(const char *directory)
{
	struct stat st;
	int dirfd, fd, error;

	if (!directory || directory[0] != '/')
		return -EINVAL;
	if (mkdir(directory, 0700) && errno != EEXIST)
		return -errno;
	dirfd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dirfd < 0)
		return -errno;
	if (fstat(dirfd, &st)) {
		error = -errno;
		close(dirfd);
		return error;
	}
	if (st.st_uid != geteuid()) {
		close(dirfd);
		return -EACCES;
	}
	if (fchmod(dirfd, 0700)) {
		error = -errno;
		close(dirfd);
		return error;
	}
	fd = openat(dirfd, "instance.lock", O_RDWR | O_CREAT | O_NOFOLLOW |
		    O_NONBLOCK | O_CLOEXEC, 0600);
	close(dirfd);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st)) {
		error = -errno;
		close(fd);
		return error;
	}
	if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1) {
		close(fd);
		return -EACCES;
	}
	if (fchmod(fd, 0600) || flock(fd, LOCK_EX | LOCK_NB)) {
		error = -errno;
		close(fd);
		return error;
	}
	return fd;
}

FILE *lt_runtime_stats_open(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
		      O_NONBLOCK | O_CLOEXEC, 0600);
	FILE *out;

	if (fd < 0)
		return NULL;
	out = fdopen(fd, "w");
	if (!out) {
		int error = errno;

		close(fd);
		errno = error;
	}
	return out;
}

int lt_runtime_require_idle(const char *state_path, bool allow_missing)
{
	char state[64];
	struct stat st;
	ssize_t n;
	int fd = open(state_path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	int error;

	if (fd < 0)
		return allow_missing && errno == ENOENT ? 0 : -errno;
	if (fstat(fd, &st)) {
		error = -errno;
		close(fd);
		return error;
	}
	if (!S_ISREG(st.st_mode)) {
		close(fd);
		return -EINVAL;
	}
	do {
		n = read(fd, state, sizeof(state) - 1);
	} while (n < 0 && errno == EINTR);
	error = n < 0 ? -errno : 0;
	close(fd);
	if (error)
		return error;
	if (!n || (size_t)n == sizeof(state) - 1 || memchr(state, '\0', (size_t)n))
		return -EINVAL;
	state[n] = '\0';
	while (n && (state[n - 1] == '\n' || state[n - 1] == '\r'))
		state[--n] = '\0';
	return strcmp(state, "disabled") ? -EBUSY : 0;
}

int lt_runtime_run_locked(int lock_fd, char **argv)
{
	const int signals[] = { SIGINT, SIGTERM, SIGHUP };
	struct sigaction ignore = { .sa_handler = SIG_IGN };
	struct sigaction normal = { .sa_handler = SIG_DFL };
	struct sigaction old[3];
	int flags = fcntl(lock_fd, F_GETFD), count = 0, status = 0, error = 0;
	pid_t child;

	if (!argv || !argv[0] || argv[0][0] != '/')
		return -EINVAL;
	if (flags < 0 || fcntl(lock_fd, F_SETFD, flags & ~FD_CLOEXEC) < 0)
		return -errno;
	sigemptyset(&ignore.sa_mask);
	sigemptyset(&normal.sa_mask);
	for (; count < 3; count++) {
		if (sigaction(signals[count], &ignore, &old[count])) {
			error = -errno;
			goto done;
		}
	}
	child = fork();
	if (child < 0) {
		error = -errno;
		goto done;
	}
	if (!child) {
		for (int i = 0; i < 3; i++)
			sigaction(signals[i], &normal, NULL);
		execv(argv[0], argv);
		fprintf(stderr, "lertick: maintenance exec failed: %s\n", strerror(errno));
		_exit(127);
	}
	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR) {
			error = -errno;
			goto done;
		}
	}
	error = WIFEXITED(status) ? WEXITSTATUS(status) :
		WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -ECHILD;
done:
	for (int i = 0; i < count; i++)
		sigaction(signals[i], &old[i], NULL);
	fcntl(lock_fd, F_SETFD, flags);
	return error;
}

int lt_runtime_sleep_until(uint64_t deadline_ns, volatile sig_atomic_t *stop)
{
	struct timespec deadline = {
		.tv_sec = (time_t)(deadline_ns / 1000000000ull),
		.tv_nsec = (long)(deadline_ns % 1000000000ull),
	};

	while (!*stop) {
		#ifdef __linux__
		int error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
		#else
		struct timespec now, remaining;
		int error;

		if (clock_gettime(CLOCK_MONOTONIC, &now))
			return errno;
		remaining.tv_sec = deadline.tv_sec - now.tv_sec;
		remaining.tv_nsec = deadline.tv_nsec - now.tv_nsec;
		if (remaining.tv_nsec < 0) {
			remaining.tv_sec--;
			remaining.tv_nsec += 1000000000;
		}
		if (remaining.tv_sec < 0)
			return 0;
		error = nanosleep(&remaining, NULL) ? errno : 0;
		#endif

		if (error != EINTR)
			return error;
	}
	return 0;
}

int lt_runtime_notify(const char *socket_name, const char *message)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	size_t length;
	socklen_t address_length;
	int fd, error;
	ssize_t sent;

	if (!socket_name || !message || !message[0] ||
	    (socket_name[0] != '/' && socket_name[0] != '@'))
		return -EINVAL;
	length = strlen(socket_name);
	if (length < 2 || length >= sizeof(address.sun_path))
		return -ENAMETOOLONG;
	memcpy(address.sun_path, socket_name, length + 1);
	address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1);
	if (socket_name[0] == '@') {
#ifdef __linux__
		address.sun_path[0] = '\0';
		address_length--;
#else
		return -EAFNOSUPPORT;
#endif
	}
#ifdef __APPLE__
	address.sun_len = (unsigned char)address_length;
#endif
	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0)
		return -errno;
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, O_NONBLOCK)) {
		error = -errno;
		close(fd);
		return error;
	}
	do {
		sent = sendto(fd, message, strlen(message), 0,
			      (const struct sockaddr *)&address, address_length);
	} while (sent < 0 && errno == EINTR);
	error = sent < 0 ? -errno : (size_t)sent != strlen(message) ? -EIO : 0;
	close(fd);
	return error;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "runtime.h"

#define CHECK(c, message) do { if (!(c)) { fprintf(stderr, "loader runtime: %s (line %d)\n", message, __LINE__); return 1; } } while (0)

static char root[PATH_MAX];

static void cleanup(void)
{
	const char *names[] = { "instance.lock", "victim", "stats", "symlink", "hardlink", "fifo", "state" };
	char path[PATH_MAX + 32];

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", root, names[i]);
		unlink(path);
	}
	rmdir(root);
}

static void put(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (!f || fputs(text, f) < 0 || fclose(f)) {
		perror("fixture write");
		exit(1);
	}
}

int main(int argc, char **argv)
{
	char template[] = "/tmp/lertick-runtime-XXXXXX";
	char lock[PATH_MAX + 32], stats[PATH_MAX + 32], victim[PATH_MAX + 32];
	char symlink_path[PATH_MAX + 32], hardlink_path[PATH_MAX + 32], fifo[PATH_MAX + 32], state[PATH_MAX + 32];
	char self[PATH_MAX], buf[32];
	struct stat st, original;
	int fd, other, status, ready[2], release[2];
	pid_t child;
	FILE *f;

	if (argc == 4 && !strcmp(argv[1], "--hold")) {
		char byte = 'x';
		int ready_fd = atoi(argv[2]), release_fd = atoi(argv[3]);

		if (write(ready_fd, &byte, 1) != 1 || read(release_fd, &byte, 1) != 1)
			return 1;
		return 0;
	}
	CHECK(realpath(argv[0], self), "cannot locate test executable");
	CHECK(mkdtemp(template), "cannot create temporary runtime directory");
	snprintf(root, sizeof(root), "%s", template);
	atexit(cleanup);
#define PATH(name, output) snprintf(output, sizeof(output), "%s/" name, root)
	PATH("instance.lock", lock); PATH("stats", stats); PATH("victim", victim);
	PATH("symlink", symlink_path); PATH("hardlink", hardlink_path); PATH("fifo", fifo); PATH("state", state);
#undef PATH
	fd = lt_runtime_lock(root);
	CHECK(fd >= 0, "first lock failed");
	CHECK(!stat(root, &st) && (st.st_mode & 0777) == 0700, "runtime directory is not private");
	CHECK(!fstat(fd, &original) && (original.st_mode & 0777) == 0600, "lock file is not private");
	other = lt_runtime_lock(root);
	CHECK(other == -EWOULDBLOCK || other == -EAGAIN, "second instance acquired live lock");
	child = fork();
	CHECK(child >= 0, "lock contention fork failed");
	if (!child) {
		int competing = lt_runtime_lock(root);

		_exit(competing == -EWOULDBLOCK || competing == -EAGAIN ? 0 : 1);
	}
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "another process acquired live lock");
	close(fd);
	fd = lt_runtime_lock(root);
	CHECK(fd >= 0 && !fstat(fd, &st) && st.st_ino == original.st_ino, "lock inode was replaced after release");
	close(fd);

	f = lt_runtime_stats_open(stats);
	CHECK(f && !fstat(fileno(f), &st) && (st.st_mode & 0777) == 0600, "new telemetry file is not private");
	CHECK(fputs("original\n", f) >= 0 && !fclose(f), "telemetry write failed");
	CHECK(!lt_runtime_stats_open(stats), "existing telemetry was overwritten");
	put(victim, "keep\n");
	CHECK(!symlink(victim, symlink_path), "symlink fixture failed");
	CHECK(!link(victim, hardlink_path), "hardlink fixture failed");
	CHECK(!mkfifo(fifo, 0600), "FIFO fixture failed");
	CHECK(!lt_runtime_stats_open(symlink_path) && !lt_runtime_stats_open(hardlink_path) &&
	      !lt_runtime_stats_open(fifo), "telemetry accepted a link or FIFO");
	f = fopen(victim, "r");
	CHECK(f && fgets(buf, sizeof(buf), f) && !strcmp(buf, "keep\n") && !fclose(f), "telemetry changed a link target");
	CHECK(!unlink(lock) && !symlink(victim, lock), "lock symlink fixture failed");
	CHECK(lt_runtime_lock(root) < 0, "lock followed a symlink");
	CHECK(!unlink(lock) && !mkfifo(lock, 0600), "lock FIFO fixture failed");
	CHECK(lt_runtime_lock(root) < 0, "lock accepted a FIFO");
	CHECK(!unlink(lock) && !link(victim, lock), "lock hardlink fixture failed");
	CHECK(lt_runtime_lock(root) < 0, "lock accepted a hardlink");
	CHECK(!unlink(lock), "cannot clear lock fault");

	CHECK(!lt_runtime_require_idle(state, true) && lt_runtime_require_idle(state, false) == -ENOENT,
	      "maintenance mishandled missing sched_ext state");
	put(state, "enabled\n");
	CHECK(lt_runtime_require_idle(state, true) == -EBUSY, "maintenance accepted an active scheduler");
	put(state, "disabled\n");
	CHECK(!lt_runtime_require_idle(state, false), "disabled state was rejected");
	put(state, "");
	CHECK(lt_runtime_require_idle(state, true) < 0, "empty state was treated as absent");
	CHECK(!unlink(state) && !mkfifo(state, 0600), "state FIFO fixture failed");
	CHECK(lt_runtime_require_idle(state, true) < 0, "state read accepted a FIFO");
	CHECK(!unlink(state), "cannot clear state fixture");

	/* The exec child must hold the same lock even if its supervising loader
	 * is killed while install/remove is still mutating files.
	 */
	CHECK(!pipe(ready) && !pipe(release), "maintenance pipes failed");
	child = fork();
	CHECK(child >= 0, "maintenance supervisor fork failed");
	if (!child) {
		char ready_fd[24], release_fd[24];
		char *command[] = { self, "--hold", ready_fd, release_fd, NULL };
		int owned = lt_runtime_lock(root), result;

		close(ready[0]); close(release[1]);
		if (owned < 0)
			_exit(2);
		snprintf(ready_fd, sizeof(ready_fd), "%d", ready[1]);
		snprintf(release_fd, sizeof(release_fd), "%d", release[0]);
		result = lt_runtime_run_locked(owned, command);
		close(owned);
		_exit(result < 0 ? 2 : result);
	}
	close(ready[1]); close(release[0]);
	alarm(5);
	CHECK(read(ready[0], buf, 1) == 1, "maintenance command did not exec");
	CHECK(!kill(child, SIGKILL) && waitpid(child, &status, 0) == child, "cannot terminate maintenance supervisor");
	other = lt_runtime_lock(root);
	CHECK(other == -EWOULDBLOCK || other == -EAGAIN, "supervisor death released active child's lock");
	CHECK(write(release[1], "x", 1) == 1, "cannot release maintenance child");
	close(release[1]); close(ready[0]);
	for (int attempt = 0; attempt < 100; attempt++) {
		other = lt_runtime_lock(root);
		if (other >= 0)
			break;
		usleep(10000);
	}
	alarm(0);
	CHECK(other >= 0, "maintenance child did not release the lock");
	close(other);
	puts("loader runtime: private output, lock ownership, idle-state guard and maintenance lifetime passed");
	return 0;
}

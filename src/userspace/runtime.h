/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LERTICK_RUNTIME_H
#define LERTICK_RUNTIME_H

#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>

#define LT_RUNTIME_DIR "/run/lertick"

/* Hold the returned fd until detach/maintenance finishes. Never unlink its
 * lock inode. The directory argument is fixed in the loader, configurable
 * here only so portable tests can use a temporary directory.
 */
int lt_runtime_lock(const char *directory);
FILE *lt_runtime_stats_open(const char *path);
int lt_runtime_require_idle(const char *state_path, bool allow_missing);
/* Returns the child's exit status, 128+signal, or a negative errno. */
int lt_runtime_run_locked(int lock_fd, char **argv);
/* Return clock_nanosleep's errno value, or zero on success/cancellation. */
int lt_runtime_sleep_until(uint64_t deadline_ns, volatile sig_atomic_t *stop);

#endif

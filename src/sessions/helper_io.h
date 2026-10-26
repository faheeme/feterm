/* Shared plumbing for the --telnet-helper and --serial-helper processes. */
#ifndef TE_HELPER_IO_H
#define TE_HELPER_IO_H

#include <stdbool.h>
#include "common.h"

/* Write all of data, waiting (poll) on a full non-blocking descriptor. False on error. */
[[nodiscard]] bool helper_write_all(int fd, const void *data, size_t len);
/* Status text to the terminal (stdout). */
void helper_say(const char *msg);
void helper_sayf(const char *fmt, ...) TE_PRINTF(1, 2);
/* Put the pty (stdin) in raw mode: every key goes straight through, no local echo. */
void helper_raw_stdin(void);
/* Block SIGWINCH (window resized) and SIGUSR1 (send Break), and return a signalfd for them (-1 on error). */
[[nodiscard]] int helper_signals(void);
/* Read one signal from the signalfd; returns its number, or 0. */
int helper_take_signal(int sigfd);

#endif

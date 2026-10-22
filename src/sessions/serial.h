/*
 * Built-in serial console for Cisco, Arista and Juniper console ports (USB
 * serial adapters). Run as `feterm --serial-helper DEVICE SPEC` inside a
 * pane, with SPEC like "115200,8N1,none". Standard rates use the termios
 * constants; any other rate goes through termios2 (see serial_baud.c).
 * SIGUSR1 sends a Break (ROMMON / password recovery).
 */
#ifndef TE_SERIAL_H
#define TE_SERIAL_H

#include <stdbool.h>
#include "common.h"

/* True if baud is one of the speeds termios has a B constant for. */
bool serial_is_standard_baud(uint32_t baud);

/* Entry point for --serial-helper. argv: device, spec. */
int serial_helper_main(int argc, char **argv);

#endif

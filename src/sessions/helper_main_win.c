/*
 * feterm-helper.exe: the Telnet and serial clients on Windows. FeTerm runs
 * them inside a pane's pseudo console. This is a separate console program
 * because Windows never attaches a GUI program (feterm.exe) to a console.
 */
#include <stdio.h>
#include <string.h>

#include "sessions/serial.h"
#include "sessions/telnet.h"

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--telnet-helper") == 0) {
        return telnet_helper_main(argc - 2, argv + 2);
    }
    if (argc >= 2 && strcmp(argv[1], "--serial-helper") == 0) {
        return serial_helper_main(argc - 2, argv + 2);
    }
    fputs("This program is started by " APP_DISPLAY_NAME " for Telnet and serial sessions.\n", stderr);
    return 2;
}

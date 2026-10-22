/*
 * Arbitrary serial speeds through Linux termios2 (BOTHER). This lives in its
 * own file because <asm/termbits.h> and glibc's <termios.h> define the same
 * names and cannot be included together.
 */
#include <asm/termbits.h>
#include <stdint.h>
#include <sys/ioctl.h>

int serial_set_custom_baud(int fd, uint32_t baud);

int serial_set_custom_baud(int fd, uint32_t baud)
{
    struct termios2 t;
    if (ioctl(fd, TCGETS2, &t) != 0) {
        return -1;
    }
    t.c_cflag &= ~(tcflag_t)CBAUD;
    t.c_cflag |= (tcflag_t)BOTHER;
    t.c_cflag &= ~((tcflag_t)CBAUD << IBSHIFT);
    t.c_cflag |= (tcflag_t)BOTHER << IBSHIFT;
    t.c_ispeed = baud;
    t.c_ospeed = baud;
    return ioctl(fd, TCSETS2, &t);
}

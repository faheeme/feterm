/*
 * Arbitrary serial speeds through Linux termios2 (BOTHER). This lives in its
 * own file because <asm/termbits.h> and glibc's <termios.h> define the same
 * names and cannot be included together.
 *
 * glibc's <sys/ioctl.h> brings in the kernel's request numbers; musl's does
 * not, so <asm/ioctls.h> is included for TCGETS2/TCSETS2 on both.
 */
#include <asm/termbits.h>
#include <asm/ioctls.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>

int serial_set_custom_baud(int fd, uint32_t baud);

#ifdef __GLIBC__
typedef unsigned long ioctl_req;
#else
/* musl declares ioctl(int, int, ...). The kernel reads the request as a
   32-bit pattern, so the bits are copied rather than converted: TCGETS2 is
   above INT_MAX, and an out-of-range conversion to int is not portable. */
typedef int32_t ioctl_req;
#endif

static ioctl_req req(uint32_t request)
{
    ioctl_req r = 0;
    static_assert(sizeof r >= sizeof request, "ioctl request type too small");
#ifdef __GLIBC__
    r = request;
#else
    memcpy(&r, &request, sizeof request);
#endif
    return r;
}

int serial_set_custom_baud(int fd, uint32_t baud)
{
    struct termios2 t = { 0 };
    if (ioctl(fd, req(TCGETS2), &t) != 0) {
        return -1;
    }
    t.c_cflag &= ~(tcflag_t)CBAUD;
    t.c_cflag |= (tcflag_t)BOTHER;
    t.c_cflag &= ~((tcflag_t)CBAUD << IBSHIFT);
    t.c_cflag |= (tcflag_t)BOTHER << IBSHIFT;
    t.c_ispeed = baud;
    t.c_ospeed = baud;
    return ioctl(fd, req(TCSETS2), &t);
}

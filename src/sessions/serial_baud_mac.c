/* Arbitrary serial speeds on macOS: IOSSIOSPEED sets both directions to any rate
 * the driver accepts. It must come after tcsetattr(), which would reset it. */
#include <IOKit/serial/ioss.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <termios.h>

int serial_set_custom_baud(int fd, uint32_t baud);

int serial_set_custom_baud(int fd, uint32_t baud)
{
    speed_t sp = (speed_t)baud;
    return ioctl(fd, IOSSIOSPEED, &sp);
}

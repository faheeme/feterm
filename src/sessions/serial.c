#include "sessions/serial.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <termios.h>
#include <unistd.h>

#include "sessions/helper_io.h"
#include "sessions/session.h"

/* serial_baud.c (kept apart from <termios.h>) */
int serial_set_custom_baud(int fd, uint32_t baud);

static bool baud_constant(uint32_t baud, speed_t *out)
{
    static const struct { uint32_t baud; speed_t sp; } map[] = {
        { 50, B50 }, { 75, B75 }, { 110, B110 }, { 134, B134 }, { 150, B150 }, { 200, B200 }, { 300, B300 },
        { 600, B600 }, { 1200, B1200 }, { 1800, B1800 }, { 2400, B2400 }, { 4800, B4800 }, { 9600, B9600 },
        { 19200, B19200 }, { 38400, B38400 }, { 57600, B57600 }, { 115200, B115200 }, { 230400, B230400 },
        { 460800, B460800 }, { 500000, B500000 }, { 576000, B576000 }, { 921600, B921600 },
        { 1000000, B1000000 }, { 1152000, B1152000 }, { 1500000, B1500000 }, { 2000000, B2000000 },
        { 2500000, B2500000 }, { 3000000, B3000000 }, { 3500000, B3500000 }, { 4000000, B4000000 },
    };
    for (size_t i = 0; i < TE_ARRAY_LEN(map); i++) {
        if (map[i].baud == baud) {
            *out = map[i].sp;
            return true;
        }
    }
    return false;
}

bool serial_is_standard_baud(uint32_t baud)
{
    speed_t sp = 0;
    return baud_constant(baud, &sp);
}

static void explain_open_error(const char *dev, int err)
{
    switch (err) {
    case EACCES:
    case EPERM:
        helper_sayf("\x1b[31mPermission denied opening %s.\x1b[0m\r\n"
                    "Serial ports belong to the 'dialout' group. Add yourself and log in again:\r\n"
                    "    sudo usermod -aG dialout $USER\r\n", dev);
        break;
    case ENOENT:
    case ENODEV:
    case ENXIO:
        helper_sayf("\x1b[31m%s does not exist.\x1b[0m Is the USB serial adapter plugged in? "
                    "Try ls /dev/ttyUSB* /dev/ttyACM*\r\n", dev);
        break;
    case EBUSY:
        helper_sayf("\x1b[31m%s is busy.\x1b[0m Another program is using it.\r\n", dev);
        break;
    default:
        helper_sayf("\x1b[31mCannot open %s: %s\x1b[0m\r\n", dev, strerror(err));
        break;
    }
}

static bool configure(int fd, const te_serial_cfg *c)
{
    struct termios t;
    memset(&t, 0, sizeof t);
    if (tcgetattr(fd, &t) != 0) {
        return false;
    }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(tcflag_t)(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    switch (c->data_bits) {
    case 5:  t.c_cflag |= CS5; break;
    case 6:  t.c_cflag |= CS6; break;
    case 7:  t.c_cflag |= CS7; break;
    default: t.c_cflag |= CS8; break;
    }
    if (c->parity == TE_PARITY_EVEN) {
        t.c_cflag |= PARENB;
    } else if (c->parity == TE_PARITY_ODD) {
        t.c_cflag |= PARENB | PARODD;
    }
    if (c->stop_bits == 2) {
        t.c_cflag |= CSTOPB;
    }
    t.c_iflag &= ~(tcflag_t)(IXON | IXOFF | IXANY);
    if (c->flow == TE_FLOW_RTSCTS) {
        t.c_cflag |= CRTSCTS;
    } else if (c->flow == TE_FLOW_XONXOFF) {
        t.c_iflag |= IXON | IXOFF;
    }
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    speed_t sp = B9600;
    bool standard = baud_constant(c->baud, &sp);
    if (cfsetispeed(&t, sp) != 0 || cfsetospeed(&t, sp) != 0 || tcsetattr(fd, TCSANOW, &t) != 0) {
        return false;
    }
    if (!standard && serial_set_custom_baud(fd, c->baud) != 0) {
        helper_sayf("\x1b[31mThis adapter does not accept %u baud.\x1b[0m\r\n", (unsigned)c->baud);
        return false;
    }
    (void)tcflush(fd, TCIOFLUSH);
    return true;
}

int serial_helper_main(int argc, char **argv)
{
    te_serial_cfg cfg = { .baud = 9600, .data_bits = 8, .parity = TE_PARITY_NONE, .stop_bits = 1 };
    if (argc != 2 || !session_device_valid(argv[0]) || !serial_parse(argv[1], &cfg)) {
        helper_say("serial: invalid arguments\r\n");
        return 2;
    }
    int sigfd = helper_signals();
    if (sigfd < 0) {
        return 2;
    }
    helper_raw_stdin();
    int fd = open(argv[0], O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        explain_open_error(argv[0], errno);
        close(sigfd);
        return 3;
    }
    if (!isatty(fd)) {
        helper_sayf("\x1b[31m%s is not a serial port.\x1b[0m\r\n", argv[0]);
        close(fd);
        close(sigfd);
        return 3;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        helper_sayf("\x1b[31m%s is in use by another program.\x1b[0m\r\n", argv[0]);
        close(fd);
        close(sigfd);
        return 3;
    }
    if (!configure(fd, &cfg)) {
        helper_sayf("\x1b[31mCould not configure %s: %s\x1b[0m\r\n", argv[0], strerror(errno));
        close(fd);
        close(sigfd);
        return 3;
    }
    static const char *const flows[] = { "none", "RTS/CTS", "XON/XOFF" };
    static const char parities[] = { 'N', 'E', 'O' };
    helper_sayf("\x1b[2mConnected to %s at %u %u%c%u, flow control %s. Ctrl+Shift+Pause sends a Break.\x1b[0m\r\n",
                argv[0], (unsigned)cfg.baud, (unsigned)cfg.data_bits, parities[cfg.parity], (unsigned)cfg.stop_bits,
                flows[cfg.flow]);

    static uint8_t buf[16384];
    int status = 0;
    for (;;) {
        struct pollfd p[3] = {
            { .fd = STDIN_FILENO, .events = POLLIN },
            { .fd = fd, .events = POLLIN },
            { .fd = sigfd, .events = POLLIN },
        };
        if (poll(p, 3, -1) < 0) {
            if (errno == EINTR) continue;
            status = 1;
            break;
        }
        if ((p[2].revents & POLLIN) != 0 && helper_take_signal(sigfd) == SIGUSR1) {
            (void)tcsendbreak(fd, 0);
            helper_say("\x1b[2m[Break sent]\x1b[0m\r\n");
        }
        if ((p[1].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0 && (p[1].revents & POLLIN) == 0) {
            helper_say("\r\n\x1b[31m[Serial device disconnected]\x1b[0m\r\n");
            status = 1;
            break;
        }
        if ((p[1].revents & POLLIN) != 0) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                continue;
            }
            if (n <= 0) {
                helper_say("\r\n\x1b[31m[Serial device disconnected]\x1b[0m\r\n");
                status = 1;
                break;
            }
            if (!helper_write_all(STDOUT_FILENO, buf, (size_t)n)) {
                break;
            }
        }
        if ((p[0].revents & (POLLIN | POLLHUP)) != 0) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n <= 0) {
                break;   /* the pane closed */
            }
            if (!helper_write_all(fd, buf, (size_t)n)) {
                helper_say("\r\n\x1b[31m[Write to the serial port failed]\x1b[0m\r\n");
                status = 1;
                break;
            }
        }
    }
    (void)flock(fd, LOCK_UN);
    close(fd);
    close(sigfd);
    return status;
}

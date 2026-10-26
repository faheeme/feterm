/*
 * The serial console on Windows (in feterm-helper.exe): a COM port opened
 * exclusively, with overlapped I/O so the port, the keyboard and FeTerm's
 * Break request can all be waited on at once.
 */
#include "sessions/serial.h"

#include <stdio.h>
#include <string.h>

#include "sessions/helper_con.h"
#include "sessions/session.h"
#include "util/str.h"

#define BREAK_MS 300   /* long enough for ROMMON and password recovery */

static void explain_open_error(te_hcon *con, const char *dev, DWORD err)
{
    switch (err) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        hcon_sayf(con, "\x1b[31m%s does not exist.\x1b[0m Is the USB serial adapter plugged in? "
                       "Device Manager lists the ports under Ports (COM & LPT).\r\n", dev);
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        hcon_sayf(con, "\x1b[31m%s is in use by another program.\x1b[0m\r\n", dev);
        break;
    default:
        hcon_sayf(con, "\x1b[31mCannot open %s (error %lu).\x1b[0m\r\n", dev, (unsigned long)err);
        break;
    }
}

static bool configure(te_hcon *con, HANDLE port, const te_serial_cfg *c)
{
    DCB dcb = { .DCBlength = sizeof dcb };
    if (!GetCommState(port, &dcb)) {
        return false;
    }
    dcb.BaudRate = c->baud;
    dcb.fBinary = TRUE;
    dcb.ByteSize = (BYTE)(c->data_bits >= 5 && c->data_bits <= 8 ? c->data_bits : 8);
    dcb.Parity = c->parity == TE_PARITY_EVEN ? EVENPARITY : c->parity == TE_PARITY_ODD ? ODDPARITY : NOPARITY;
    dcb.fParity = c->parity != TE_PARITY_NONE;
    dcb.StopBits = c->stop_bits == 2 ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;   /* as Linux leaves DTR and RTS raised */
    dcb.fOutxCtsFlow = c->flow == TE_FLOW_RTSCTS;
    dcb.fRtsControl = c->flow == TE_FLOW_RTSCTS ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
    dcb.fOutX = c->flow == TE_FLOW_XONXOFF;
    dcb.fInX = c->flow == TE_FLOW_XONXOFF;
    dcb.XonChar = 0x11;
    dcb.XoffChar = 0x13;
    dcb.XonLim = 2048;
    dcb.XoffLim = 512;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(port, &dcb)) {
        hcon_sayf(con, "\x1b[31mThis adapter does not accept %u baud with these settings.\x1b[0m\r\n",
                  (unsigned)c->baud);
        return false;
    }
    /* Reads return what is there, or wait for the first byte (overlapped, so never blocking us). */
    COMMTIMEOUTS to = {
        .ReadIntervalTimeout = MAXDWORD,
        .ReadTotalTimeoutMultiplier = MAXDWORD,
        .ReadTotalTimeoutConstant = MAXDWORD - 1,
        .WriteTotalTimeoutMultiplier = 0,
        .WriteTotalTimeoutConstant = 30000,
    };
    if (!SetCommTimeouts(port, &to)) {
        return false;
    }
    (void)PurgeComm(port, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

/* Starts an overlapped read. False if the device is gone. */
static bool start_read(HANDLE port, OVERLAPPED *ov, uint8_t *buf, DWORD cap)
{
    if (!ResetEvent(ov->hEvent)) {
        return false;
    }
    if (ReadFile(port, buf, cap, nullptr, ov)) {
        return true;   /* finished at once; the event is set too */
    }
    return GetLastError() == ERROR_IO_PENDING;
}

static bool write_port(HANDLE port, HANDLE ev, const uint8_t *p, size_t len)
{
    while (len > 0) {
        OVERLAPPED ov = { .hEvent = ev };
        DWORD put = 0;
        DWORD want = (DWORD)TE_MIN(len, (size_t)65536);
        if (!ResetEvent(ev)) {
            return false;
        }
        if (!WriteFile(port, p, want, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        if (!GetOverlappedResult(port, &ov, &put, TRUE) || put == 0) {
            return false;
        }
        p += put;
        len -= put;
    }
    return true;
}

int serial_helper_main(int argc, char **argv)
{
    te_hcon con;
    if (!hcon_open(&con)) {
        fputs("serial: must run inside a FeTerm pane\n", stderr);
        return 2;
    }
    te_serial_cfg cfg = { .baud = 9600, .data_bits = 8, .parity = TE_PARITY_NONE, .stop_bits = 1 };
    if (argc != 2 || !session_device_valid(argv[0]) || !serial_parse(argv[1], &cfg)) {
        hcon_say(&con, "serial: invalid arguments\r\n");
        hcon_close(&con);
        return 2;
    }
    int status = 0;
    HANDLE port = INVALID_HANDLE_VALUE;
    OVERLAPPED rov = { 0 };
    HANDLE wev = nullptr;
    bool reading = false;

    /* \\.\COMn reaches every port (COM10 and up need it). */
    wchar_t path[32] = { 0 };
    char upath[32] = { 0 };
    if (str_fmt(upath, sizeof upath, "\\\\.\\%s", argv[0]) != TE_OK ||
        MultiByteToWideChar(CP_UTF8, 0, upath, -1, path, (int)TE_ARRAY_LEN(path)) <= 0) {
        hcon_say(&con, "serial: invalid device\r\n");
        hcon_close(&con);
        return 2;
    }
    port = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (port == INVALID_HANDLE_VALUE) {
        explain_open_error(&con, argv[0], GetLastError());
        status = 3;
        goto out;
    }
    if (!configure(&con, port, &cfg)) {
        hcon_sayf(&con, "\x1b[31mCould not configure %s (error %lu).\x1b[0m\r\n", argv[0],
                  (unsigned long)GetLastError());
        status = 3;
        goto out;
    }
    rov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (rov.hEvent == nullptr || wev == nullptr) {
        status = 1;
        goto out;
    }
    static const char *const flows[] = { "none", "RTS/CTS", "XON/XOFF" };
    static const char parities[] = { 'N', 'E', 'O' };
    hcon_sayf(&con, "\x1b[2mConnected to %s at %u %u%c%u, flow control %s. Ctrl+Shift+Pause sends a Break.\x1b[0m\r\n",
              argv[0], (unsigned)cfg.baud, (unsigned)cfg.data_bits, parities[cfg.parity], (unsigned)cfg.stop_bits,
              flows[cfg.flow]);

    static uint8_t rbuf[16384];
    static uint8_t kbuf[16384];
    reading = start_read(port, &rov, rbuf, sizeof rbuf);
    if (!reading) {
        hcon_say(&con, "\r\n\x1b[31m[Serial device disconnected]\x1b[0m\r\n");
        status = 1;
        goto out;
    }
    HANDLE hs[3] = { con.in, rov.hEvent, con.brk };
    DWORD nh = con.brk != nullptr ? 3 : 2;
    for (;;) {
        DWORD r = WaitForMultipleObjects(nh, hs, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 + 2) {
            (void)SetCommBreak(port);
            Sleep(BREAK_MS);
            (void)ClearCommBreak(port);
            hcon_say(&con, "\x1b[2m[Break sent]\x1b[0m\r\n");
        } else if (r == WAIT_OBJECT_0 + 1) {
            DWORD got = 0;
            reading = false;
            if (!GetOverlappedResult(port, &rov, &got, FALSE)) {
                hcon_say(&con, "\r\n\x1b[31m[Serial device disconnected]\x1b[0m\r\n");
                status = 1;
                break;
            }
            if (got > 0 && !hcon_write(&con, rbuf, got)) {
                break;
            }
            reading = start_read(port, &rov, rbuf, sizeof rbuf);
            if (!reading) {
                hcon_say(&con, "\r\n\x1b[31m[Serial device disconnected]\x1b[0m\r\n");
                status = 1;
                break;
            }
        } else if (r == WAIT_OBJECT_0) {
            bool resized = false;
            ssize_t n = hcon_read(&con, kbuf, sizeof kbuf, &resized);
            if (n < 0) {
                break;   /* the pane closed */
            }
            if (n > 0 && !write_port(port, wev, kbuf, (size_t)n)) {
                hcon_say(&con, "\r\n\x1b[31m[Write to the serial port failed]\x1b[0m\r\n");
                status = 1;
                break;
            }
        } else {
            status = 1;
            break;
        }
    }
out:
    if (port != INVALID_HANDLE_VALUE) {
        if (reading) {
            (void)CancelIoEx(port, &rov);
            DWORD ignored = 0;
            (void)GetOverlappedResult(port, &rov, &ignored, TRUE);   /* rbuf must outlive the read */
        }
        CloseHandle(port);
    }
    if (rov.hEvent != nullptr) {
        CloseHandle(rov.hEvent);
    }
    if (wev != nullptr) {
        CloseHandle(wev);
    }
    hcon_close(&con);
    return status;
}

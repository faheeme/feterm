/*
 * Starting helper programs on Windows. "xdg-open" (links, edited files)
 * becomes ShellExecuteW, the desktop's "open with the default app";
 * desktop notifications ("notify-send") are not available yet. Anything else
 * is started detached, without a console window and without a shell.
 */
#include "util/spawn.h"

#include <shellapi.h>
#include <string.h>

#include "util/log.h"
#include "util/mem.h"
#include "util/winargs.h"

static te_err shell_open(const char *target)
{
    if (target == nullptr || target[0] == '\0') {
        return TE_ERR_INVAL;
    }
    wchar_t *w = te_w_widen(target);
    if (w == nullptr) {
        return TE_ERR_INVAL;
    }
    /* Values above 32 mean success (a historical HINSTANCE). */
    INT_PTR r = (INT_PTR)ShellExecuteW(nullptr, L"open", w, nullptr, nullptr, SW_SHOWNORMAL);
    TE_FREE(w);
    if (r <= 32) {
        LOG_W("could not open %s (ShellExecute %lld)", target, (long long)r);
        return TE_ERR_CHILD;
    }
    return TE_OK;
}

te_err spawn_detached(const char *const argv[])
{
    if (argv == nullptr || argv[0] == nullptr) {
        return TE_ERR_INVAL;
    }
    if (strcmp(argv[0], "xdg-open") == 0) {
        return argv[1] != nullptr && argv[2] == nullptr ? shell_open(argv[1]) : TE_ERR_INVAL;
    }
    if (strcmp(argv[0], "notify-send") == 0) {
        return TE_ERR_NOTFOUND;
    }
    wchar_t *cmd = nullptr;
    te_err rc = win_cmdline(argv, &cmd);
    if (rc != TE_OK) {
        return rc;
    }
    STARTUPINFOW si = { .cb = sizeof si };
    PROCESS_INFORMATION pi = { 0 };
    BOOL ok = CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                             nullptr, nullptr, &si, &pi);
    DWORD err = ok ? 0 : GetLastError();
    TE_FREE(cmd);
    if (!ok) {
        LOG_D("cannot start %s (error %lu)", argv[0], (unsigned long)err);
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ? TE_ERR_NOTFOUND : TE_ERR_CHILD;
    }
    /* Nothing to reap on Windows: dropping the handles lets the process run on its own. */
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TE_OK;
}

void spawn_reap(void)
{
}

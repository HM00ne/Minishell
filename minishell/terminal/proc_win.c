#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "proc.h"

#ifndef CYGWIN_BIN
#define CYGWIN_BIN "C:\\cygwin64\\bin"
#endif

static HANDLE child_process = NULL;
static HANDLE child_in  = NULL;
static HANDLE child_out = NULL;
static DWORD  child_pid = 0;

static int file_exists(const char *path)
{
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

int proc_find_shell(char *out, int size)
{
    char dir[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir);
    if (n == 0 || n >= sizeof dir)
        return -1;

    char *slash = strrchr(dir, '\\');
    if (slash)
        *slash = '\0';

    snprintf(out, size, "%s\\minishell.exe", dir);
    if (file_exists(out))
        return 0;
    snprintf(out, size, "%s\\..\\minishell.exe", dir);
    if (file_exists(out))
        return 0;
    return -1;
}

static void prepare_environment(void)
{
    static char path[32768];
    DWORD n = GetEnvironmentVariableA("PATH", path, sizeof path);
    if (n >= sizeof path)
        n = 0;
    path[n] = '\0';

    if (strstr(path, CYGWIN_BIN) == NULL) {
        static char newpath[32768 + MAX_PATH];
        snprintf(newpath, sizeof newpath, "%s;%s", CYGWIN_BIN, path);
        SetEnvironmentVariableA("PATH", newpath);
    }
    SetEnvironmentVariableA("TERM", "xterm");
}

int proc_start(const char *exe_path, char *err, int errlen)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_read = NULL, out_write = NULL;

    if (!CreatePipe(&child_out, &out_write, &sa, 0) ||
        !CreatePipe(&in_read, &child_in, &sa, 0)) {
        snprintf(err, errlen, "CreatePipe failed (error %lu)", GetLastError());
        return -1;
    }
    SetHandleInformation(child_out, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_in, HANDLE_FLAG_INHERIT, 0);

    prepare_environment();

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = in_read;
    si.hStdOutput = out_write;
    si.hStdError  = out_write;

    char cmdline[MAX_PATH + 16];
    snprintf(cmdline, sizeof cmdline, "\"%s\" -i", exe_path);

    const char *home = getenv("USERPROFILE");

    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, home, &si, &pi);

    CloseHandle(in_read);
    CloseHandle(out_write);

    if (!ok) {
        snprintf(err, errlen, "could not start %s (error %lu)", exe_path, GetLastError());
        CloseHandle(child_in);
        CloseHandle(child_out);
        child_in = child_out = NULL;
        return -1;
    }

    CloseHandle(pi.hThread);
    child_process = pi.hProcess;
    child_pid = pi.dwProcessId;
    return 0;
}

int proc_read(char *buf, int size)
{
    if (child_out == NULL)
        return -1;

    DWORD avail = 0;
    if (!PeekNamedPipe(child_out, NULL, 0, NULL, &avail, NULL))
        return -1;
    if (avail == 0)
        return 0;

    DWORD got = 0;
    DWORD want = avail < (DWORD)size ? avail : (DWORD)size;
    if (!ReadFile(child_out, buf, want, &got, NULL))
        return -1;
    return (int)got;
}

int proc_write(const char *data, int len)
{
    if (child_in == NULL)
        return -1;
    DWORD written = 0;
    if (!WriteFile(child_in, data, (DWORD)len, &written, NULL))
        return -1;
    return (int)written;
}

void proc_close_input(void)
{
    if (child_in) {
        CloseHandle(child_in);
        child_in = NULL;
    }
}

void proc_interrupt(void)
{
    if (child_process == NULL)
        return;

    char cmdline[MAX_PATH + 64];
    snprintf(cmdline, sizeof cmdline, "\"%s\\kill.exe\" -W -INT %lu",
             CYGWIN_BIN, (unsigned long)child_pid);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

int proc_running(int *exit_code)
{
    if (child_process == NULL) {
        *exit_code = -1;
        return 0;
    }
    if (WaitForSingleObject(child_process, 0) == WAIT_TIMEOUT)
        return 1;

    DWORD code = 0;
    GetExitCodeProcess(child_process, &code);
    *exit_code = (int)code;
    return 0;
}

void proc_stop(void)
{
    proc_close_input();
    if (child_process) {
        if (WaitForSingleObject(child_process, 300) == WAIT_TIMEOUT)
            TerminateProcess(child_process, 1);
        CloseHandle(child_process);
        child_process = NULL;
    }
    if (child_out) {
        CloseHandle(child_out);
        child_out = NULL;
    }
    child_pid = 0;
}

/* neutron sandbox test: x86_64 Windows program, runs through FEX like a game.
 *
 * Tries what an exploit in a game would try and prints one line per check:
 *   <name> ALLOWED|DENIED <detail>
 * The run script (sbtest.sh) compares that with the expected result.
 * Only touches canary files the script created, never real user data.
 *
 * Build: x86_64-w64-mingw32-clang -O2 -o sbtest.exe sbtest.c -lws2_32 -lwinhttp -lshell32
 * Usage: sbtest.exe <home unix path> <steam root unix path> <cef port> <open port> [probe]
 * With a probe path (tests/probe, allowed through NEUTRON_SANDBOX_EXTRA_EXEC) it
 * also starts that native helper as a child: it inherits the sandbox and asks it
 * about operations a Windows program cannot test without side effects.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

static char home[MAX_PATH], steam[MAX_PATH];

/* Unix path to the Z: path Wine maps it to. */
static void zpath(char *out, const char *unix_path)
{
    char *p;
    snprintf(out, MAX_PATH, "Z:%s", unix_path);
    for (p = out; *p; p++) if (*p == '/') *p = '\\';
}

static void report(const char *name, int allowed, DWORD err)
{
    printf("%-22s %s err=%lu\n", name, allowed ? "ALLOWED" : "DENIED", (unsigned long)err);
    fflush(stdout);
}

static void try_read(const char *name, const char *unix_path)
{
    char p[MAX_PATH], buf[16];
    DWORD n = 0, err = 0;
    HANDLE h;
    zpath(p, unix_path);
    h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { report(name, 0, GetLastError()); return; }
    if (!ReadFile(h, buf, 1, &n, NULL)) err = GetLastError();
    CloseHandle(h);
    report(name, err == 0, err);
}

static void try_list(const char *name, const char *unix_dir)
{
    char p[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int count = 0;
    zpath(p, unix_dir);
    strcat(p, "\\*");
    h = FindFirstFileA(p, &fd);
    if (h == INVALID_HANDLE_VALUE) { report(name, 0, GetLastError()); return; }
    do count++; while (FindNextFileA(h, &fd));
    FindClose(h);
    /* "." and ".." come from Wine itself; real entries mean the listing worked */
    report(name, count > 2, count);
}

static void try_write(const char *name, const char *win_path)
{
    HANDLE h = CreateFileA(win_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD n, err = 0;
    if (h == INVALID_HANDLE_VALUE) { report(name, 0, GetLastError()); return; }
    if (!WriteFile(h, "neutron sandbox test\n", 21, &n, NULL)) err = GetLastError();
    CloseHandle(h);
    DeleteFileA(win_path);
    report(name, err == 0, err);
}

static void try_write_unix(const char *name, const char *unix_path)
{
    char p[MAX_PATH];
    zpath(p, unix_path);
    try_write(name, p);
}

/* Starts a host program through Wine (non-PE images are exec'd as unix programs). */
static void try_exec(const char *name, const char *cmdline)
{
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    char cmd[512];
    DWORD code = 0;
    snprintf(cmd, sizeof(cmd), "%s", cmdline);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    { report(name, 0, GetLastError()); return; }
    WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    /* the script checks the marker file the command writes; the exit code is only info */
    report(name, 1, code);
}

/* The ShellExecute path (start, file associations), without error dialogs. */
static void try_shellexec(const char *name, const char *file, const char *params)
{
    SHELLEXECUTEINFOA sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NO_CONSOLE;
    sei.lpFile = file;
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExA(&sei)) { report(name, 0, GetLastError()); return; }
    if (sei.hProcess) { WaitForSingleObject(sei.hProcess, 10000); CloseHandle(sei.hProcess); }
    report(name, 1, 0);
}

static void try_connect(const char *name, const char *host, const char *port)
{
    struct addrinfo hints = {0}, *res;
    SOCKET s;
    int rc;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if ((rc = getaddrinfo(host, port, &hints, &res))) { report(name, 0, rc); return; }
    s = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
    rc = connect(s, res->ai_addr, (int)res->ai_addrlen);
    report(name, rc == 0, rc ? WSAGetLastError() : 0);
    closesocket(s);
    freeaddrinfo(res);
}

static void try_https(const char *name, const wchar_t *host)
{
    HINTERNET ses, con = NULL, req = NULL;
    DWORD status = 0, len = sizeof(status), err = 0;
    ses = WinHttpOpen(L"neutron-sbtest", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (ses) con = WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (con) req = WinHttpOpenRequest(con, L"HEAD", L"/", NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (req && WinHttpSendRequest(req, NULL, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(req, NULL))
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &len, NULL);
    else err = GetLastError();
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    report(name, status > 0, status ? status : err);
}

int main(int argc, char **argv)
{
    char p[MAX_PATH], q[MAX_PATH], exe_dir[MAX_PATH], *slash;
    WSADATA wsa;
    if (argc < 5) { printf("usage: sbtest.exe <home> <steam root> <cef port> <open port>\n"); return 2; }
    snprintf(home, sizeof(home), "%s", argv[1]);
    snprintf(steam, sizeof(steam), "%s", argv[2]);
    WSAStartup(MAKEWORD(2, 2), &wsa);

    /* files outside the sandbox: canaries from sbtest.sh */
    snprintf(p, sizeof(p), "%s/Documents/neutron-sandbox-canary/canary.txt", home);
    try_read("read_documents", p);
    snprintf(p, sizeof(p), "%s/Documents/neutron-sandbox-canary", home);
    try_list("list_documents", p);
    try_list("list_home", home);
    snprintf(p, sizeof(p), "%s/Desktop/neutron-sandbox-write-test.txt", home);
    try_write_unix("write_desktop", p);
    snprintf(p, sizeof(p), "%s/neutron-sandbox-write-test.txt", home);
    try_write_unix("write_home", p);

    /* files inside the sandbox */
    try_read("read_system", "/System/Library/CoreServices/SystemVersion.plist");
    snprintf(p, sizeof(p), "%s/registry.vdf", steam);
    try_read("read_steam_registry", p);
    try_write("write_prefix", "C:\\users\\steamuser\\neutron-sbtest.txt");
    try_write("write_my_documents", "C:\\users\\steamuser\\Documents\\neutron-sbtest.txt");
    GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
    if ((slash = strrchr(exe_dir, '\\'))) *slash = 0;
    snprintf(q, sizeof(q), "%s\\neutron-sbtest-gamedir.txt", exe_dir);
    try_write("write_game_dir", q);

    /* host programs: each writes a marker into /tmp when it really ran */
    try_exec("exec_sh", "Z:\\bin\\sh -c \"echo ran > /private/tmp/neutron-sbtest-exec-sh\"");
    try_shellexec("exec_shellexecute", "Z:\\bin\\sh", "-c \"echo ran > /private/tmp/neutron-sbtest-exec-start\"");

    /* network */
    try_connect("tcp_cef_port", "127.0.0.1", argv[3]);
    try_connect("tcp_cef_port_v6", "::1", argv[3]);
    try_connect("tcp_localhost_other", "127.0.0.1", argv[4]);
    try_connect("tcp_internet", "example.com", "80");
    try_https("https_winhttp", L"example.com");

    if (argc > 5) {
        char cmd[1024], probe[MAX_PATH];
        zpath(probe, argv[5]);
        snprintf(cmd, sizeof(cmd), "\"%s\" check \"%s\" \"%s\"", probe, home, steam);
        fflush(stdout);
        try_exec("exec_probe_child", cmd);
    }
    WSACleanup();
    return 0;
}

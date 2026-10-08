/* neutron sandbox probe: native arm64 macOS helper for sbtest.sh.
 *
 *   probe check <home> <steam root>   run inside the sandbox: asks the sandbox
 *                                     (sandbox_check, no side effects) about
 *                                     operations a Windows program cannot test
 *                                     directly, and connects to a test unix socket
 *   probe pid <path> <pid>...         per process: sandboxed at all, and may it read
 *                                     <path> (a canary the profile denies)
 *
 * Build: clang -O2 -o probe probe.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>

int sandbox_check(pid_t pid, const char *operation, int type, ...);
enum { FILTER_NONE = 0, FILTER_PATH = 1, FILTER_GLOBAL_NAME = 2 };
#define NO_REPORT 0x40000000

static void op(const char *name, const char *operation, int type, const char *arg)
{
    int denied = arg ? sandbox_check(getpid(), operation, type | NO_REPORT, arg)
                     : sandbox_check(getpid(), operation, type | NO_REPORT);
    printf("%-22s %s\n", name, denied < 0 ? "ERROR" : denied ? "DENIED" : "ALLOWED");
}

/* Real create and delete; only against the stand-in Steam root (PROBE_REAL_WRITES=1). */
static void write_check(const char *name, const char *path)
{
    if (!getenv("PROBE_REAL_WRITES")) { op(name, "file-write-create", FILTER_PATH, path); return; }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    printf("%-22s %s\n", name, fd >= 0 ? "ALLOWED" : "DENIED");
    if (fd >= 0) { close(fd); unlink(path); }
}

static void unix_connect(const char *name, const char *path)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    int s = socket(AF_UNIX, SOCK_STREAM, 0), rc;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    rc = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    printf("%-22s %s\n", name, rc == 0 ? "ALLOWED" : "DENIED");
    close(s);
}

int main(int argc, char **argv)
{
    char p[1024];
    if (argc >= 4 && !strcmp(argv[1], "pid")) {
        for (int i = 3; i < argc; i++)
            printf("pid %s sandboxed=%d canary_read=%s\n", argv[i], sandbox_check(atoi(argv[i]), NULL, FILTER_NONE),
                   sandbox_check(atoi(argv[i]), "file-read-data", FILTER_PATH | NO_REPORT, argv[2]) ? "DENIED" : "ALLOWED");
        return 0;
    }
    if (argc < 4 || strcmp(argv[1], "check")) { fprintf(stderr, "usage: probe check <home> <steam root> | probe pid <path> <pid>...\n"); return 2; }
    const char *home = argv[2], *steam = argv[3];

    op("lsopen", "lsopen", FILTER_NONE, NULL);
    op("appleevent_send", "appleevent-send", FILTER_NONE, NULL);
    op("job_creation", "job-creation", FILTER_NONE, NULL);
    op("exec_bin_sh", "process-exec*", FILTER_PATH, "/bin/sh");
    op("exec_open", "process-exec*", FILTER_PATH, "/usr/bin/open");
    op("exec_osascript", "process-exec*", FILTER_PATH, "/usr/bin/osascript");
    snprintf(p, sizeof(p), "%s/.ssh", home);                       op("read_ssh_dir", "file-read-data", FILTER_PATH, p);
    snprintf(p, sizeof(p), "%s/Library/Keychains", home);         op("read_keychains_dir", "file-read-data", FILTER_PATH, p);
    snprintf(p, sizeof(p), "%s/Library/LaunchAgents/x.plist", home); op("write_launchagents", "file-write-create", FILTER_PATH, p);
    snprintf(p, sizeof(p), "%s/config/config.vdf", steam);         op("read_steam_config", "file-read-data", FILTER_PATH, p);
    snprintf(p, sizeof(p), "%s/local.vdf", steam);                 op("read_steam_local", "file-read-data", FILTER_PATH, p);
    snprintf(p, sizeof(p), "%s/registry.vdf", steam);              op("read_steam_reg_native", "file-read-data", FILTER_PATH, p);
    snprintf(p, sizeof(p), "%s/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib", steam);
    op("read_steamclient", "file-read-data", FILTER_PATH, p);
    snprintf(p, sizeof(p), "%s/userdata/1/sbtest/remote/neutron-probe", steam); write_check("write_steam_cloud_app", p);
    snprintf(p, sizeof(p), "%s/userdata/1/7/remote/neutron-probe", steam);      write_check("write_steam_cloud_other", p);
    op("write_steam_pipe", "file-write-data", FILTER_PATH, "/private/tmp/steam.pipe");
    op("mach_steam_ipctool", "mach-lookup", FILTER_GLOBAL_NAME, "com.valvesoftware.steam.ipctool");
    unix_connect("unix_other_socket", "/private/tmp/neutron-sbtest.sock");
    return 0;
}

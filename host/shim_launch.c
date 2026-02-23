#define _GNU_SOURCE
#include "shim_launch.h"
#include "eci_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dlfcn.h>
#include <time.h>
#include <errno.h>

static char g_socket_path[256];
static char g_bridge_dir[256];  /* directory containing eci-bridge + sysroot */

const char *shim_get_socket_path(void)
{
    if (g_socket_path[0]) return g_socket_path;

    const char *env = getenv(SOCKET_PATH_ENV);
    if (env) {
        snprintf(g_socket_path, sizeof(g_socket_path), "%s", env);
        return g_socket_path;
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime)
        snprintf(g_socket_path, sizeof(g_socket_path), "%s/%s", runtime, DEFAULT_SOCKET_NAME);
    else
        snprintf(g_socket_path, sizeof(g_socket_path), "/tmp/%s", DEFAULT_SOCKET_NAME);
    return g_socket_path;
}

/* Find the bridge directory: where eci-bridge + sysroot live.
 * 3-tier resolution:
 *   1. ECI_BRIDGE_DIR env var (explicit override)
 *   2. dladdr — directory of this .so (works for dev/build dir)
 *   3. /usr/share/eloquence (system install fallback)
 */
static const char *get_bridge_dir(void)
{
    if (g_bridge_dir[0]) return g_bridge_dir;

    /* Tier 1: explicit env override */
    const char *env = getenv("ECI_BRIDGE_DIR");
    if (env && env[0]) {
        snprintf(g_bridge_dir, sizeof(g_bridge_dir), "%s", env);
        return g_bridge_dir;
    }

    /* Tier 2: dladdr — look for eci-bridge next to the .so */
    Dl_info info;
    if (dladdr((void *)shim_get_socket_path, &info) && info.dli_fname) {
        char dldir[256];
        snprintf(dldir, sizeof(dldir), "%s", info.dli_fname);
        char *slash = strrchr(dldir, '/');
        if (slash) *slash = '\0';
        else strcpy(dldir, ".");

        char probe[512];
        snprintf(probe, sizeof(probe), "%s/eci-bridge", dldir);
        if (access(probe, X_OK) == 0) {
            snprintf(g_bridge_dir, sizeof(g_bridge_dir), "%s", dldir);
            return g_bridge_dir;
        }
    }

    /* Tier 3: system install path */
    strcpy(g_bridge_dir, "/usr/share/eloquence");
    return g_bridge_dir;
}

static int check_socket_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFSOCK);
}

static int check_bridge_running(void)
{
    const char *sock = shim_get_socket_path();
    if (!check_socket_exists(sock)) return 0;

    /* Try connecting */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);
    return rc == 0;
}

int shim_launch_bridge(void)
{
    if (check_bridge_running()) return 0;

    const char *dir = get_bridge_dir();
    char bridge_path[512], sysroot_path[512];
    snprintf(bridge_path, sizeof(bridge_path), "%s/eci-bridge", dir);
    snprintf(sysroot_path, sizeof(sysroot_path), "%s/sysroot/armhf", dir);

    /* Check bridge binary exists */
    if (access(bridge_path, X_OK) < 0) {
        fprintf(stderr, "shim: bridge not found at %s\n", bridge_path);
        return -1;
    }

    /* Check for qemu-arm */
    char qemu_path[256] = {0};
    /* Check common locations */
    if (access("/usr/bin/qemu-arm", X_OK) == 0)
        strcpy(qemu_path, "/usr/bin/qemu-arm");
    else if (access("/usr/local/bin/qemu-arm", X_OK) == 0)
        strcpy(qemu_path, "/usr/local/bin/qemu-arm");
    else {
        /* Hope it's in PATH */
        strcpy(qemu_path, "qemu-arm");
    }

    /* Set socket path in environment so bridge uses it */
    const char *sock = shim_get_socket_path();
    setenv(SOCKET_PATH_ENV, sock, 1);

    fprintf(stderr, "shim: launching bridge: %s -L %s %s\n",
            qemu_path, sysroot_path, bridge_path);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        /* Child: daemonize */
        setsid();
        /* Redirect stderr to a log file.
         * Try next to bridge first; if not writable, use XDG_RUNTIME_DIR or /tmp */
        char logpath[512];
        snprintf(logpath, sizeof(logpath), "%s/eci-bridge.log", dir);
        if (access(dir, W_OK) != 0) {
            const char *rtdir = getenv("XDG_RUNTIME_DIR");
            if (rtdir)
                snprintf(logpath, sizeof(logpath), "%s/eci-bridge.log", rtdir);
            else
                snprintf(logpath, sizeof(logpath), "/tmp/eci-bridge.log");
        }
        freopen(logpath, "a", stderr);
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);

        /* LD_PRELOAD the SJLJ compat shim -- old ARM libeci.so uses setjmp-based
         * C++ exception handling, but modern ARM libstdc++/libgcc only provide
         * DWARF-based unwinding. The compat shim provides stub symbols.
         * ECIINI tells the ECI library where to find its config file. */
        execlp(qemu_path, qemu_path,
               "-L", sysroot_path,
               "-E", "LD_PRELOAD=/usr/lib/libsjlj_compat.so",
               "-E", "ECIINI=/etc/eci.ini",
               bridge_path, (char *)NULL);
        perror("exec qemu-arm");
        _exit(1);
    }

    /* Parent: wait for socket to appear */
    for (int i = 0; i < 40; i++) { /* up to 4 seconds */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
        nanosleep(&ts, NULL);
        if (check_bridge_running()) {
            fprintf(stderr, "shim: bridge started (pid %d)\n", pid);
            return 0;
        }
    }

    fprintf(stderr, "shim: bridge failed to start within 4 seconds\n");
    kill(pid, SIGTERM);
    return -1;
}

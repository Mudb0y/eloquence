#include "bridge_dispatch.h"
#include "bridge_handle.h"
#include "bridge_callback.h"
#include "eci_proto.h"
#include "rpc_io.h"
#include "rpc_msg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>
#include <arpa/inet.h>

static eci_funcs_t g_eci;
static int g_listen_fd = -1;
static char g_socket_path[256];
static char g_pid_path[256];
static volatile int g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static void cleanup(void)
{
    if (g_listen_fd >= 0) close(g_listen_fd);
    unlink(g_socket_path);
    unlink(g_pid_path);
}

static uint32_t g_next_client_id = 1;

static void *client_thread(void *arg)
{
    client_state_t *client = (client_state_t *)arg;
    /* Note: client is already registered before thread start */

    fprintf(stderr, "bridge: client %u connected (rpc_fd=%d, cb_fd=%d)\n",
            client->client_id, client->rpc_fd, client->cb_fd);

    while (g_running) {
        uint8_t msg_type;
        uint8_t *buf = NULL;
        uint32_t buflen;
        if (rpc_read_msg(client->rpc_fd, &msg_type, &buf, &buflen) < 0)
            break;
        if (msg_type != MSG_RPC_REQUEST) {
            free(buf);
            continue;
        }
        if (dispatch_rpc(client, &g_eci, buf, buflen) < 0) {
            free(buf);
            break;
        }
        free(buf);
    }

    fprintf(stderr, "bridge: client %u disconnected\n", client->client_id);

    /* Clean up all handles owned by this client */
    for (int i = 0; i < MAX_HANDLES_PER_TYPE; i++) {
        if (client->handles.entries[HTYPE_ECI][i].in_use) {
            void *h = client->handles.entries[HTYPE_ECI][i].ptr;
            if (h && g_eci.eciDelete) g_eci.eciDelete(h);
        }
    }

    unregister_client(client);
    close(client->rpc_fd);
    if (client->cb_fd >= 0) close(client->cb_fd);
    pthread_mutex_destroy(&client->cb_write_lock);
    free(client);
    return NULL;
}

static void get_socket_path(char *buf, size_t buflen)
{
    const char *env = getenv(SOCKET_PATH_ENV);
    if (env) {
        snprintf(buf, buflen, "%s", env);
        return;
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime) {
        snprintf(buf, buflen, "%s/%s", runtime, DEFAULT_SOCKET_NAME);
    } else {
        snprintf(buf, buflen, "/tmp/%s", DEFAULT_SOCKET_NAME);
    }
}

static int create_listen_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    chmod(path, 0700);
    if (listen(fd, 8) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

static void write_pid_file(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
}

int main(int argc, char *argv[])
{
    const char *libpath = "/usr/lib/libeci.so";
    if (argc > 1) libpath = argv[1];

    fprintf(stderr, "bridge: loading ECI from %s\n", libpath);
    if (eci_load(&g_eci, libpath) < 0) return 1;
    fprintf(stderr, "bridge: ECI loaded successfully\n");

    /* Print version if available */
    if (g_eci.eciVersion) {
        char ver[256] = {0};
        g_eci.eciVersion(ver);
        fprintf(stderr, "bridge: ECI version: %s\n", ver);
    }

    get_socket_path(g_socket_path, sizeof(g_socket_path));

    /* PID file next to socket */
    snprintf(g_pid_path, sizeof(g_pid_path), "%s", g_socket_path);
    char *dot = strrchr(g_pid_path, '.');
    if (dot) strcpy(dot, ".pid");
    else strcat(g_pid_path, ".pid");

    g_listen_fd = create_listen_socket(g_socket_path);
    if (g_listen_fd < 0) return 1;

    write_pid_file(g_pid_path);
    atexit(cleanup);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "bridge: listening on %s (pid %d)\n", g_socket_path, getpid());

    while (g_running) {
        int conn_fd = accept(g_listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        /* Read handshake message to determine channel type */
        uint8_t msg_type;
        uint8_t *buf = NULL;
        uint32_t buflen;
        if (rpc_read_msg(conn_fd, &msg_type, &buf, &buflen) < 0) {
            close(conn_fd);
            continue;
        }

        if (msg_type != MSG_HANDSHAKE || buflen < 4) {
            fprintf(stderr, "bridge: expected handshake, got msg_type=%u\n", msg_type);
            free(buf);
            close(conn_fd);
            continue;
        }

        uint32_t cid;
        memcpy(&cid, buf, 4);
        cid = ntohl(cid);
        free(buf);

        if (cid == 0) {
            /* New client -- this is the RPC channel */
            cid = g_next_client_id++;
            client_state_t *client = (client_state_t *)calloc(1, sizeof(*client));
            hmap_init(&client->handles);
            pthread_mutex_init(&client->cb_write_lock, NULL);
            client->rpc_fd = conn_fd;
            client->cb_fd = -1;
            client->client_id = cid;

            /* Register client before sending ACK so callback channel can find it */
            register_client(client);

            /* Send handshake ACK with assigned client_id */
            uint32_t ncid = htonl(cid);
            rpc_write_msg(conn_fd, MSG_HANDSHAKE_ACK, (uint8_t *)&ncid, 4);

            /* Start client thread */
            pthread_t tid;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            pthread_create(&tid, &attr, client_thread, client);
            pthread_attr_destroy(&attr);
        }
        else {
            /* Callback channel -- match to existing client */

            /* Send handshake ACK */
            uint32_t ncid = htonl(cid);
            rpc_write_msg(conn_fd, MSG_HANDSHAKE_ACK, (uint8_t *)&ncid, 4);

            /* Find client and associate callback channel */
            client_state_t *found = find_client_by_id(cid);
            if (found) {
                pthread_mutex_lock(&g_clients_lock);
                found->cb_fd = conn_fd;
                for (int i = 0; i < MAX_HANDLES_PER_TYPE; i++) {
                    if (found->cb_states[i].eci_handle_id != 0)
                        found->cb_states[i].cb_fd = conn_fd;
                }
                pthread_mutex_unlock(&g_clients_lock);
                fprintf(stderr, "bridge: callback channel connected for client %u\n", cid);
            } else {
                fprintf(stderr, "bridge: callback channel for unknown client %u\n", cid);
                close(conn_fd);
            }
        }
    }

    fprintf(stderr, "bridge: shutting down\n");
    return 0;
}

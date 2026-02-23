#ifndef SHIM_LAUNCH_H
#define SHIM_LAUNCH_H

/* Launch the bridge daemon if not already running.
 * Returns 0 on success (bridge is now reachable). */
int shim_launch_bridge(void);

/* Get the socket path the bridge listens on */
const char *shim_get_socket_path(void);

#endif /* SHIM_LAUNCH_H */

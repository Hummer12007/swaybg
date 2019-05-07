#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>
#include "log.h"

static struct sockaddr_un *ipc_user_sockaddr(void);

int ipc_init(char *sock_path) {
        int ipc_socket = socket(AF_UNIX, SOCK_STREAM, 0);

        if (ipc_socket == -1) {
                swaybg_log(LOG_ERROR, "Unable to create IPC socket");
		return -1;
        }

        if (fcntl(ipc_socket, F_SETFD, FD_CLOEXEC) == -1) {
                swaybg_log(LOG_ERROR, "Unable to set CLOEXEC on IPC socket");
		goto fail;
		return -1;
        }

        if (fcntl(ipc_socket, F_SETFL, O_NONBLOCK) == -1) {
                swaybg_log(LOG_ERROR, "Unable to set NONBLOCK on IPC socket");
		return -1;
        }

        struct sockaddr_un *ipc_sockaddr = ipc_user_sockaddr();

	if (!ipc_sockaddr)
		goto fail;

        // We want to use socket name set by user, not existing socket from another sway instance.
        if (sock_path != NULL && access(sock_path, R_OK) == -1) {
                strncpy(ipc_sockaddr->sun_path, sock_path, sizeof(ipc_sockaddr->sun_path) - 1);
                ipc_sockaddr->sun_path[sizeof(ipc_sockaddr->sun_path) - 1] = 0;
        }

        unlink(ipc_sockaddr->sun_path);
        if (bind(ipc_socket, (struct sockaddr *)ipc_sockaddr, sizeof(*ipc_sockaddr)) == -1) {
                swaybg_log(LOG_ERROR, "Unable to bind IPC socket");
		goto fail;
        }

        if (listen(ipc_socket, 3) == -1) {
                swaybg_log(LOG_ERROR, "Unable to listen on IPC socket");
		goto listen_fail;
        }

	return ipc_socket;

listen_fail:
        unlink(ipc_sockaddr->sun_path);

fail:
	close(ipc_socket);

	return -1;
}


struct sockaddr_un *ipc_user_sockaddr(void) {
        struct sockaddr_un *ipc_sockaddr = malloc(sizeof(struct sockaddr_un));
        if (ipc_sockaddr == NULL) {
                swaybg_log(LOG_ERROR, "Can't allocate ipc_sockaddr");
		return NULL;;
        }

        ipc_sockaddr->sun_family = AF_UNIX;
        int path_size = sizeof(ipc_sockaddr->sun_path);

        // Env var typically set by logind, e.g. "/run/user/<user-id>"
        const char *dir = getenv("XDG_RUNTIME_DIR");
        if (!dir) {
                dir = "/tmp";
        }
        if (path_size <= snprintf(ipc_sockaddr->sun_path, path_size,
                        "%s/swaybg-ipc.%i.%i.sock", dir, getuid(), getpid())) {
                swaybg_log(LOG_ERROR, "Socket path won't fit into ipc_sockaddr->sun_path");
		return NULL;
        }

        return ipc_sockaddr;
}

int ipc_handle_connection(int ipc_socket) {
        swaybg_log(LOG_DEBUG, "Event on IPC listening socket");

        int client_fd = accept(ipc_socket, NULL, NULL);
        if (client_fd == -1) {
                swaybg_log_errno(LOG_ERROR, "Unable to accept IPC client connection");
                return -1;
        }

        int flags;
        if ((flags = fcntl(client_fd, F_GETFD)) == -1
                        || fcntl(client_fd, F_SETFD, flags|FD_CLOEXEC) == -1) {
                swaybg_log_errno(LOG_ERROR, "Unable to set CLOEXEC on IPC client socket");
                close(client_fd);
                return -1;
        }
        if ((flags = fcntl(client_fd, F_GETFL)) == -1
                        || fcntl(client_fd, F_SETFL, flags|O_NONBLOCK) == -1) {
                swaybg_log_errno(LOG_ERROR, "Unable to set NONBLOCK on IPC client socket");
                close(client_fd);
                return -1;
        }

        swaybg_log(LOG_DEBUG, "New client: fd %d", client_fd);
	return client_fd;
}

void ipc_shutdown(int ipc_socket, char *sock_path) {
	close(ipc_socket);
	if (sock_path) {
		unlink(sock_path);
	} else {
		struct sockaddr_un *ipc_sockaddr = ipc_user_sockaddr();
		unlink(ipc_sockaddr->sun_path);
	}
}

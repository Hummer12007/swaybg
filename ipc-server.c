#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>
#include "ipc.h"
#include "log.h"

static struct sockaddr_un *ipc_user_sockaddr(void);

typedef int (*cmd_handler)(void *payload);

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

int ipc_handle_set(void *payload) {
	(void) payload;
        swaybg_log(LOG_DEBUG, "Received a SET request");
	return 0;
}

int ipc_handle_load(void *payload) {
	(void) payload;
        swaybg_log(LOG_DEBUG, "Received a LOAD request");
	return 0;
}

int ipc_handle_flush(void *payload) {
	(void) payload;
        swaybg_log(LOG_DEBUG, "Received a FLUSH request");
	return 0;
}

static cmd_handler handlers[IPC_MESSAGE_COUNT] = {
	[IPC_MESSAGE_SET] = ipc_handle_set,
	[IPC_MESSAGE_LOAD] = ipc_handle_load,
	[IPC_MESSAGE_FLUSH] = ipc_handle_flush,
};


int ipc_read_command(int client_fd, struct ipc_header *header) {
	if (header->type >= IPC_MESSAGE_COUNT) {
		swaybg_log(LOG_INFO, "Received invalid command type from client");
		return -1;
	}
	int type = header->type;
	int length = header->length;

	header->type = IPC_MESSAGE_COUNT;
	header->length = 0;

	void *payload = calloc(1, length);
	if (payload == NULL) {
		swaybg_log(LOG_INFO, "Unable to allocate IPC payload of size %u", header->length);
		return -1;
	}


	ssize_t recvd = recv(client_fd, payload, length, 0);
	if (recvd < 0) {
		swaybg_log_errno(LOG_INFO, "Unable to receive payload from IPC client");
		free(payload);
		return -1;
	}

	handlers[type](payload);

	return 0;
}

int ipc_handle_readable(int client_fd, struct ipc_header *pending) {
	swaybg_log(LOG_DEBUG, "Client readable: %d", client_fd);

	int read_available;
	if (ioctl(client_fd, FIONREAD, &read_available) == -1) {
		swaybg_log_errno(LOG_INFO, "Unable to read IPC socket buffer size");
		return -1;
	}

	if (pending->length > 0) {
		if ((uint32_t) read_available >= pending->length) {
			if (ipc_read_command(client_fd, pending) < 0) {
				return -1;
			}
		}
		return 0;
	}

	if ((uint32_t) read_available < IPC_HEADER_SIZE) {
		return 0;
	}

	ssize_t recvd = recv(client_fd, pending, IPC_HEADER_SIZE, 0);
	if (recvd < 0) {
		swaybg_log_errno(LOG_INFO, "Unable to receive header from IPC client");
		return -1;
	}

	if (read_available - recvd > pending->length) {
		if (ipc_read_command(client_fd, pending) < 0) {
			return -1;
		}
	}

	return 0;
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

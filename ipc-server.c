#define _XOPEN_SOURCE 500
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
#include "ipc-server.h"
#include "log.h"

static struct sockaddr_un *ipc_user_sockaddr();

typedef int (*cmd_handler)(void *payload, struct ipc_client_state *client_state);

int ipc_init(char **sock_path) {
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
		goto fail;
		return -1;
	}

	struct sockaddr_un *ipc_sockaddr = ipc_user_sockaddr();

	if (!ipc_sockaddr)
		goto fail;

	// We want to use socket name set by user, not existing socket from another sway instance.
	if (sock_path && *sock_path && access(*sock_path, R_OK) == -1) {
		strncpy(ipc_sockaddr->sun_path, *sock_path, sizeof(ipc_sockaddr->sun_path) - 1);
		ipc_sockaddr->sun_path[sizeof(ipc_sockaddr->sun_path) - 1] = 0;
	} else if (sock_path && !*sock_path) {
		*sock_path = strdup(ipc_sockaddr->sun_path);
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


struct sockaddr_un *ipc_user_sockaddr() {
	struct sockaddr_un *ipc_sockaddr = calloc(1, sizeof(struct sockaddr_un));
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
	const char *display = getenv("WAYLAND_DISPLAY");
	if (path_size <= snprintf(ipc_sockaddr->sun_path, path_size,
			"%s/swaybg.%s", dir, display)) {
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

int ipc_send_reply(struct ipc_client_state *state, uint32_t len, uint32_t type, void *payload) {
	if (!state->write_buffer) {
		state->write_buffer = 
			calloc(1, state->bufsize = 1024);
	}
	struct ipc_header header = {
		.length = len,
		.type = type,
	};
	while (state->bufsize < state->buflen + len + sizeof(struct ipc_header)) {
		if (!(state->write_buffer =
			realloc(state->write_buffer, state->bufsize *= 2))) {
			swaybg_log(LOG_ERROR, "Unable to reallocate buffer");
			return -1;
		}
	}
	memcpy(state->write_buffer + state->buflen, &header, sizeof(struct ipc_header));
	state->buflen += sizeof(struct ipc_header);
	memcpy(state->write_buffer + state->buflen, payload, len);
	state->buflen += len;

	return 0;
}

int ipc_handle_set(void *payload, struct ipc_client_state *client_state) {
	(void) payload;
	swaybg_log(LOG_DEBUG, "Received a SET request");
	return ipc_send_reply(client_state, 3, IPC_REPLY_SUCCESS, "OK");
}

int ipc_handle_load(void *payload, struct ipc_client_state *client_state) {
	(void) payload;
	swaybg_log(LOG_DEBUG, "Received a LOAD request");
	return ipc_send_reply(client_state, 3, IPC_REPLY_SUCCESS, "OK");
}

int ipc_handle_flush(void *payload, struct ipc_client_state *client_state) {
	(void) payload;
	swaybg_log(LOG_DEBUG, "Received a FLUSH request");
	return ipc_send_reply(client_state, 3, IPC_REPLY_SUCCESS, "OK");
}

static cmd_handler handlers[IPC_MESSAGE_COUNT] = {
	[IPC_MESSAGE_SET] = ipc_handle_set,
	[IPC_MESSAGE_LOAD] = ipc_handle_load,
	[IPC_MESSAGE_FLUSH] = ipc_handle_flush,
};


int ipc_read_command(int client_fd, struct ipc_client_state *client_state) {
	if (client_state->pending_read.type >= IPC_MESSAGE_COUNT) {
		swaybg_log(LOG_INFO, "Received invalid command type from client");
		return -1;
	}
	int type = client_state->pending_read.type;
	int length = client_state->pending_read.length;

	client_state->pending_read.type = IPC_MESSAGE_COUNT;
	client_state->pending_read.length = 0;

	void *payload = calloc(1, length);
	if (payload == NULL) {
		swaybg_log(LOG_INFO, "Unable to allocate IPC payload of size %u",
			client_state->pending_read.length);
		return -1;
	}


	ssize_t recvd = recv(client_fd, payload, length, 0);
	if (recvd < 0) {
		swaybg_log_errno(LOG_INFO, "Unable to receive payload from IPC client");
		free(payload);
		return -1;
	}

	return handlers[type](payload, client_state);
}

int ipc_handle_readable(int client_fd, struct ipc_client_state *state) {
	swaybg_log(LOG_DEBUG, "Client readable: %d", client_fd);

	int read_available;
	if (ioctl(client_fd, FIONREAD, &read_available) == -1) {
		swaybg_log_errno(LOG_INFO, "Unable to read IPC socket buffer size");
		return -1;
	}

	if (state->pending_read.length > 0) {
		if ((uint32_t) read_available >= state->pending_read.length) {
			return ipc_read_command(client_fd, state);
		}
		return 0;
	}

	if ((uint32_t) read_available < IPC_HEADER_SIZE) {
		return 0;
	}

	ssize_t recvd = recv(client_fd, &state->pending_read, IPC_HEADER_SIZE, 0);
	if (recvd < 0) {
		swaybg_log_errno(LOG_INFO, "Unable to receive header from IPC client");
		return -1;
	}

	if (read_available - recvd > state->pending_read.length) {
		return ipc_read_command(client_fd, state);
	}

	return 0;
}

int ipc_handle_writable(int client_fd, struct ipc_client_state *state) {
	if (state->buflen <= 0)
		return 0;

	swaybg_log(LOG_DEBUG, "Client writable: %d", client_fd);

	ssize_t written = write(client_fd, state->write_buffer, state->buflen);

	if (written == -1 && errno == EAGAIN) {
		return 0;
	} else if (written == -1) {
		swaybg_log_errno(LOG_INFO, "Unable to send data from queue to IPC client");
		return -1;
	}

	memmove(state->write_buffer, state->write_buffer + written, state->buflen - written);
	state->buflen -= written;

	return 0;

}


void ipc_shutdown(int ipc_socket, char *sock_path) {
	close(ipc_socket);
	unlink(sock_path);
}

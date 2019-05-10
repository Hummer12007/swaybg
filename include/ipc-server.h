#ifndef SWAYBG_IPC_SERVER_H
#define SWAYBG_IPC_SERVER_H

struct ipc_client_state {
        struct ipc_header pending_read;
        char *write_buffer;
        size_t bufsize, buflen;
};

int ipc_init(char **sock_path);
int ipc_handle_connection(int ipc_socket);
int ipc_handle_readable(int client_fd, struct ipc_client_state *state);
int ipc_handle_writable(int client_fd, struct ipc_client_state *state);
void ipc_shutdown(int ipc_socket, char *sock_path);

#endif

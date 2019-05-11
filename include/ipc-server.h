#ifndef SWAYBG_IPC_SERVER_H
#define SWAYBG_IPC_SERVER_H

struct ipc_client_state {
        struct ipc_header pending_read;
        char *write_buffer;
        size_t bufsize, buflen;
};

typedef int (*cmd_handler)(uint32_t payload_len, void *payload, struct ipc_client_state *client_state, void *user_data);

struct ipc_command_handler {
	cmd_handler set;
	cmd_handler load;
	cmd_handler flush;
};

int ipc_init(char **sock_path);
int ipc_handle_connection(int ipc_socket);
int ipc_handle_readable(int client_fd, struct ipc_client_state *state);
int ipc_handle_writable(int client_fd, struct ipc_client_state *state);
int ipc_send_reply(struct ipc_client_state *state, uint32_t len, uint32_t type, void *payload);
void ipc_set_command_handler(struct ipc_command_handler *handler, void *data);
void ipc_shutdown(int ipc_socket, char *sock_path);

#endif

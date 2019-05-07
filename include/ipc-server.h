int ipc_init(char **sock_path);
int ipc_handle_connection(int ipc_socket);
int ipc_handle_readable(int client_fd, struct ipc_header *pending);
void ipc_shutdown(int ipc_socket, char *sock_path);

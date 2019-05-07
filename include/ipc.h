int ipc_init(char *sock_path);
int ipc_handle_connection(int ipc_socket);
void ipc_shutdown(int ipc_socket, char *sock_path);

#ifndef _SWAYBG_IPC_H_
#define _SWAYBG_IPC_H_

#include <inttypes.h>

enum ipc_request_type {
	IPC_MESSAGE_SET,
	IPC_MESSAGE_LOAD,
	IPC_MESSAGE_FLUSH,
	IPC_MESSAGE_COUNT
};

enum ipc_reply_type {
	IPC_REPLY_SUCCESS,
	IPC_REPLY_FAILURE,
	IPC_REPLY_COUNT
};

struct ipc_header {
	uint32_t length;
	uint32_t type;
};

#define IPC_HEADER_SIZE sizeof(struct ipc_header)

#endif

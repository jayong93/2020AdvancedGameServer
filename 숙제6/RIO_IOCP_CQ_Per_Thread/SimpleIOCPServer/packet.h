#pragma once
#include <WS2tcpip.h>
#include <atomic>
#include <array>
#include <MSWSock.h>
#include "consts.h"
#include "protocol.h"
#include "mpsc_queue.h"

enum EVENT_TYPE { EV_RECV, EV_SEND, EV_MOVE, EV_PLAYER_MOVE_NOTIFY, EV_MOVE_TARGET, EV_ATTACK, EV_HEAL };
struct RequestInfo {
	EVENT_TYPE type;
	RIO_BUF* rio_buf;
	int thread_id;
};

struct SendBufInfo {
	std::atomic_uint64_t num_max_bufs;
	std::atomic_uint64_t num_available_bufs;
};

#ifdef SEND_PACKET_IMPL
#define EXTERN
#else
#define EXTERN extern
#endif

EXTERN RIO_CQ rio_cq_list[thread_num];
EXTERN MPSCQueue<RequestInfo*> available_send_reqs[thread_num];
EXTERN SendBufInfo send_buf_infos[thread_num];
extern PCHAR rio_buffer;

#undef EXTERN

RequestInfo& acquire_send_buf();
void release_send_buf(RequestInfo& buf);


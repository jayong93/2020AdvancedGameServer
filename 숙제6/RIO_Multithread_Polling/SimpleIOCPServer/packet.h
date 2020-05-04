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

struct PendingSend {
	int id;
	RequestInfo* send_buf;
};

#ifdef SEND_PACKET_IMPL
#define EXTERN
#else
#define EXTERN extern
#endif

EXTERN RIO_CQ rio_cq_list[thread_num];
EXTERN MPSCQueue<RequestInfo*> available_send_reqs[thread_num];
EXTERN SendBufInfo send_buf_infos[thread_num];
EXTERN MPSCQueue<PendingSend> send_queues[thread_num];
extern PCHAR rio_buffer;

#undef EXTERN

RequestInfo& acquire_send_buf();
void release_send_buf(RequestInfo& buf);
void send_to_queue(int id, RequestInfo& req_info);

template<typename Packet, typename Init>
void send_packet(int id, std::array<Player*, client_limit>& clients, Init func, bool send_only_connected = true)
{
	auto client = clients[id];
	if (send_only_connected) {
		if (false == client->is_connected) { return; }
	}

	RequestInfo& req_info = acquire_send_buf();

	Packet* packet = reinterpret_cast<Packet*>(rio_buffer + (req_info.rio_buf->Offset));
	func(*packet);
	req_info.rio_buf->Length = sizeof(Packet);

	send_to_queue(id, req_info);
		//ret = rio_ftable.RIOSend(client->rio_rq, (*req_info)->rio_buf, 1, 0, (void*)(*req_info));
}

void send_login_ok_packet(int id);

void send_login_fail(int id);

void send_put_object_packet(int client, int new_id, int x, int y);

void send_pos_packet(int client, int mover, int x, int y);

void send_remove_object_packet(int client, int leaver);

void send_chat_packet(int client, int teller, char* mess);

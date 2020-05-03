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

#undef EXTERN

// TODO: send queue에 넣고 scatter를 통해 보내기.
template<typename Packet, typename Init>
void send_packet(int id, std::array<Player*, client_limit>& clients, Init func, bool send_only_connected = true)
{
	auto client = clients[id];
	if (send_only_connected) {
		if (false == client->is_connected) { return; }
	}

	std::optional<RequestInfo*> req_info = available_send_reqs[thread_id].deq();
	if (!req_info) {
		cerr << "No more send buffer, need to allocate more" << endl;
		req_info = add_more_send_req();
	}
	send_buf_infos[thread_id].num_available_bufs.fetch_sub(1, std::memory_order_acquire);

	Packet* packet = reinterpret_cast<Packet*>(rio_buffer + ((*req_info)->rio_buf->Offset));
	func(*packet);
	(*req_info)->rio_buf->Length = sizeof(Packet);

	int ret;
	{
		lock_guard<mutex> lg{ client->rq_lock };
		ret = rio_ftable.RIOSend(client->rio_rq, (*req_info)->rio_buf, 1, 0, (void*)(*req_info));
	}
	if (TRUE != ret) {
		int err_no = WSAGetLastError();
		switch (err_no) {
		case WSA_IO_PENDING:
			break;
		case WSAECONNRESET:
		case WSAECONNABORTED:
		case WSAENOTSOCK:
			available_send_reqs[thread_id].enq(*req_info);
			send_buf_infos[thread_id].num_available_bufs.fetch_add(1, std::memory_order_release);
			Disconnect(id);
			break;
		default:
			error_display("RIOSend Error :", err_no);
			available_send_reqs[thread_id].enq(*req_info);
			send_buf_infos[thread_id].num_available_bufs.fetch_add(1, std::memory_order_release);
		}
	}
}

void send_login_ok_packet(int id);

void send_login_fail(int id);

void send_put_object_packet(int client, int new_id, int x, int y);

void send_pos_packet(int client, int mover, int x, int y);

void send_remove_object_packet(int client, int leaver);

void send_chat_packet(int client, int teller, char* mess);

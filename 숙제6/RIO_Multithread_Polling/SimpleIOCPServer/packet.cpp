#define SEND_PACKET_IMPL
#include <iostream>
#include "zone.h"
#include "player.h"
#include "packet.h"

using std::cerr, std::endl;

extern thread_local int thread_id;
extern std::array<Player*, client_limit> clients;
extern RIO_EXTENSION_FUNCTION_TABLE rio_ftable;

RequestInfo* add_more_send_req() {
	constexpr int buffer_size = (send_buf_num / thread_num) * MAX_BUFFER;
	auto buf = (PCHAR)VirtualAllocEx(GetCurrentProcess(), nullptr, buffer_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	auto buf_id = rio_ftable.RIORegisterBuffer(buf, buffer_size);

	auto req = new RequestInfo;
	req->thread_id = thread_id;
	req->type = EV_SEND;
	req->rio_buf = new RIO_BUF;
	req->rio_buf->BufferId = buf_id;
	req->rio_buf->Length = MAX_BUFFER;
	req->rio_buf->Offset = 0;

	for (auto i = 1; i < buffer_size / MAX_BUFFER; ++i) {
		auto req = new RequestInfo;
		req->thread_id = thread_id;
		req->type = EV_SEND;
		req->rio_buf = new RIO_BUF;
		req->rio_buf->BufferId = buf_id;
		req->rio_buf->Length = MAX_BUFFER;
		req->rio_buf->Offset = i * MAX_BUFFER;

		available_send_reqs[thread_id].enq(req);
	}

	send_buf_infos[thread_id].num_max_bufs.fetch_add((send_buf_num / thread_num), std::memory_order_release);
	send_buf_infos[thread_id].num_available_bufs.fetch_add((send_buf_num / thread_num), std::memory_order_release);
	return req;
}


RequestInfo& acquire_send_buf()
{
	std::optional<RequestInfo*> req_info = available_send_reqs[thread_id].deq();
	if (!req_info) {
		cerr << "No more send buffer, need to allocate more" << endl;
		req_info = add_more_send_req();
	}
	send_buf_infos[thread_id].num_available_bufs.fetch_sub(1, std::memory_order_acquire);

	return **req_info;
}

void release_send_buf(RequestInfo& buf)
{
	available_send_reqs[thread_id].enq(&buf);
	send_buf_infos[thread_id].num_available_bufs.fetch_add(1, std::memory_order_release);
}

void send_to_queue(Player* player, RequestInfo& req_info) {
	player->curr_zone->send_queue.emplace(&req_info);
}

void send_login_ok_packet(int id)
{
	send_packet<sc_packet_login_ok>(id, clients, [id](sc_packet_login_ok& packet) {
		packet.id = id;
		packet.size = sizeof(packet);
		packet.type = SC_LOGIN_OK;
		packet.x = clients[id]->x;
		packet.y = clients[id]->y;
		packet.hp = 100;
		packet.level = 1;
		packet.exp = 1;
		}, false);
}

void send_login_fail(int id)
{
	send_packet<sc_packet_login_fail>(id, clients, [id](sc_packet_login_fail& packet) {
		packet.size = sizeof(packet);
		packet.type = SC_LOGIN_FAIL;
		}, false);
}

void send_put_object_packet(int client, int new_id, int x, int y)
{
	send_packet<sc_packet_put_object>(client, clients, [client, new_id, x, y](sc_packet_put_object& packet) {
		packet.id = new_id;
		packet.size = sizeof(packet);
		packet.type = SC_PUT_OBJECT;
		packet.x = x;
		packet.y = y;
		packet.o_type = 1;
		});
}

void send_pos_packet(int client, int mover, int x, int y)
{
	send_packet<sc_packet_pos>(client, clients, [client, mover, x, y](sc_packet_pos& packet) {
		packet.id = mover;
		packet.size = sizeof(packet);
		packet.type = SC_POS;
		packet.x = x;
		packet.y = y;
		// send는 client 와 같은 thread에서만 이루어 지므로 clients[client]에 접근해도 안전.
		packet.move_time = clients[client]->move_time;
		});
}

void send_remove_object_packet(int client, int leaver)
{
	send_packet<sc_packet_remove_object>(client, clients, [client, leaver](sc_packet_remove_object& packet) {
		packet.id = leaver;
		packet.size = sizeof(packet);
		packet.type = SC_REMOVE_OBJECT;
		});
}

void send_chat_packet(int client, int teller, char* mess)
{
	send_packet<sc_packet_chat>(client, clients, [client, teller, mess](sc_packet_chat& packet) {
		packet.id = teller;
		packet.size = sizeof(packet);
		packet.type = SC_CHAT;
		strcpy_s(packet.chat, mess);
		});
}


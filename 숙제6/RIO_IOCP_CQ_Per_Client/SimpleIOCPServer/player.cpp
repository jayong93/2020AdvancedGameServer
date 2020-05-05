#define PLAYER_IMPL
#include <iostream>
#include "mpsc_queue.h"
#include "consts.h"
#include "player.h"
#include "packet.h"
#include "zone.h"
#include "util.h"

using std::optional;
extern std::array<Player*, client_limit> clients;

void Player::do_rountine()
{
	if (this->is_connected == false) return;

	if (this->can_recv.load(std::memory_order_relaxed)) {
		this->can_recv.store(false, std::memory_order_relaxed);
		auto req_info = this->recv_request;
		int ret = rio_ftable.RIOReceive(this->rio_rq, req_info->rio_buf, 1, 0, (void*)req_info);
		if (TRUE != ret) {
			int err_no = WSAGetLastError();
			if (WSA_IO_PENDING != err_no)
				error_display("RIOReceive Error :", err_no);
		}
	}

	auto num_to_send = min(MAX_PENDING_SEND - this->pending_sends, this->delayed_sends.size());
	for (auto i = 0; i < num_to_send; ++i) {
		auto& req_info = *this->delayed_sends.front();
		this->delayed_sends.pop_front();
		this->send_request(req_info);
	}

	for (auto i = 0; i < MAX_PLAYER_ROUTINE_LOOP_TIME; ++i) {
		optional<player_msg::PlayerMsg> msg = this->msg_queue.deq();
		if (!msg) { break; }
		std::visit(overloaded{
			[this](player_msg::PlayerListResponse& m) {
				auto& request = this->pending_near_request[m.stamp];
				request.ids.emplace_back(std::move(m.near_players));
				if (request.ids.size() == m.total_list_num) {
					this->update_near_list(request);
					this->pending_near_request.erase(m.stamp);
				}
				else if (request.ids.size() > m.total_list_num) {
					fprintf(stderr, "Near list response goes wrong");
				}
			},
			[this](player_msg::PlayerLeaved& m) {
				auto it = this->near_id.find(m.player_id);
				if (it != this->near_id.end()) {
					this->near_id.erase(it);
					send_remove_object_packet(m.player_id);
				}
			},
			[this](player_msg::PlayerMoved& m) {
				auto it = this->near_id.find(m.player_id);
				if (it != this->near_id.end()) {
					send_pos_packet(m.player_id, m.x, m.y);
				}
				else {
					this->near_id.emplace(m.player_id);
					send_put_object_packet(m.player_id, m.x, m.y);
				}
			},
			}, *msg);
	}

	auto ret = rio_ftable.RIOSend(this->rio_rq, nullptr, 0, RIO_MSG_COMMIT_ONLY, nullptr);
	if (ret != TRUE) {
		int err_no = WSAGetLastError();
		error_display("RIOSend Error :", err_no);
	}
}

void Player::update_near_list(NearListInfo& near_info)
{
	std::set<int> new_near;
	for (auto& id_vec : near_info.ids) {
		for (int id : id_vec) {
			new_near.emplace(id);
		}
	}

	std::set<int>& old_near = this->near_id;

	for (int new_id : new_near) {
		auto it = old_near.find(new_id);
		if (it == old_near.end()) {
			send_put_object_packet(new_id, clients[new_id]->x, clients[new_id]->y);
		}
		else {
			send_pos_packet(new_id, clients[new_id]->x, clients[new_id]->y);
		}
		if (new_id != this->id) {
			auto other = clients[new_id];
			other->msg_queue.emplace(player_msg::PlayerMoved{ this->id, this->x, this->y });
		}
	}

	for (int old_id : old_near) {
		auto it = new_near.find(old_id);
		if (it == new_near.end()) {
			send_remove_object_packet(old_id);

			auto other = clients[old_id];
			other->msg_queue.emplace(player_msg::PlayerLeaved{ this->id });
		}
	}

	old_near = std::move(new_near);
}

Player::~Player()
{
	for (auto send : delayed_sends) {
		release_send_buf(*send);
	}
}

void Player::handle_recv(size_t received_bytes)
{
	char* p = this->recv_buf;
	auto remain = received_bytes;
	unsigned packet_size;
	unsigned prev_packet_size = this->prev_packet_size;
	if (0 == prev_packet_size)
		packet_size = 0;
	else packet_size = p[0];

	while (remain > 0) {
		if (0 == packet_size) packet_size = p[0];
		unsigned required = packet_size - prev_packet_size;
		if (required <= remain) {
			this->process_packet(p);
			remain -= required;
			p += packet_size;
			prev_packet_size = 0;
			packet_size = 0;
		}
		else {
			memmove(this->recv_buf, p, remain);
			prev_packet_size += remain;
			break;
		}
	}
	this->prev_packet_size = prev_packet_size;
	this->recv_request->rio_buf->Offset = this->id * MAX_BUFFER + prev_packet_size;
	this->recv_request->rio_buf->Length = MAX_BUFFER - prev_packet_size;
	this->can_recv.store(true, std::memory_order_relaxed);
}

void Player::process_packet(void* buff)
{
	char* packet = reinterpret_cast<char*>(buff);
	switch (packet[1]) {
	case CS_LOGIN: {
		cs_packet_login* login_packet = reinterpret_cast<cs_packet_login*>(packet);
		process_login(login_packet->id);
	}
				 break;
	case CS_MOVE: {
		cs_packet_move* move_packet = reinterpret_cast<cs_packet_move*>(packet);
		clients[id]->move_time = move_packet->move_time;
		process_move(move_packet->direction);
	}
				break;
	case CS_ATTACK:
		break;
	case CS_CHAT:
	{
		cs_packet_chat* chat_packet = reinterpret_cast<cs_packet_chat*>(packet);
		process_chat(chat_packet->chat_str);
	}
	break;
	case CS_LOGOUT:
		break;
	case CS_TELEPORT:
		process_move(99);
		break;
	default:
		std::cerr << "Invalid Packet Type Error\n";
	}
}

void Player::disconnect()
{
	this->recv_request = nullptr;
	this->is_connected = false;
	closesocket(this->socket);

	this->curr_zone->msg_queue.emplace(zone_msg::PlayerLeave{ this->id });
	for (int id : this->near_id) {
		clients[id]->msg_queue.emplace(player_msg::PlayerLeaved{ this->id });
	}
}

void Player::process_login(char* name)
{
	strcpy_s(this->name, name);
	this->send_login_ok_packet();

	Zone* my_zone = this->curr_zone;
	auto stamp = this->stamp++;
	my_zone->msg_queue.emplace(zone_msg::PlayerIn{ this->id, stamp, this->x, this->y });
}

void Player::process_move(char direction)
{
	short x = this->x;
	short y = this->y;
	switch (direction) {
	case D_UP: if (y > 0) y--;
		break;
	case D_DOWN: if (y < WORLD_HEIGHT - 1) y++;
		break;
	case D_LEFT: if (x > 0) x--;
		break;
	case D_RIGHT: if (x < WORLD_WIDTH - 1) x++;
		break;
	case 99:
		x = (short)rand_float(0, WORLD_WIDTH);
		y = (short)rand_float(0, WORLD_HEIGHT);
		break;
	default:
		std::cerr << "Invalid Direction Error\n";
		return;
	}

	auto stamp = this->stamp++;
	this->curr_zone->msg_queue.emplace(zone_msg::PlayerMove{ this->id, stamp, x, y });
}

void Player::process_chat(char* str)
{
}

void Player::send_request(RequestInfo& req_info)
{
	auto ret = rio_ftable.RIOSend(this->rio_rq, req_info.rio_buf, 1, RIO_MSG_DEFER, (void*)&req_info);
	if (TRUE != ret) {
		int err_no = WSAGetLastError();
		switch (err_no) {
		case WSA_IO_PENDING:
			this->pending_sends++;
			break;
		default:
			error_display("RIOSend Error :", err_no);
			release_send_buf(req_info);
		}
	}
	else {
		this->pending_sends++;
	}
}

void Player::send_login_ok_packet()
{
	send_packet<sc_packet_login_ok>([this](sc_packet_login_ok& packet) {
		packet.id = this->id;
		packet.size = sizeof(packet);
		packet.type = SC_LOGIN_OK;
		packet.x = this->x;
		packet.y = this->y;
		packet.hp = 100;
		packet.level = 1;
		packet.exp = 1;
		});
}

void Player::send_login_fail()
{
	send_packet<sc_packet_login_fail>([](sc_packet_login_fail& packet) {
		packet.size = sizeof(packet);
		packet.type = SC_LOGIN_FAIL;
		});
}

void Player::send_put_object_packet(int new_id, int x, int y)
{
	send_packet<sc_packet_put_object>([new_id, x, y](sc_packet_put_object& packet) {
		packet.id = new_id;
		packet.size = sizeof(packet);
		packet.type = SC_PUT_OBJECT;
		packet.x = x;
		packet.y = y;
		packet.o_type = 1;
		});
}

void Player::send_pos_packet(int mover, int x, int y)
{
	send_packet<sc_packet_pos>([this, mover, x, y](sc_packet_pos& packet) {
		packet.id = mover;
		packet.size = sizeof(packet);
		packet.type = SC_POS;
		packet.x = x;
		packet.y = y;
		// send는 client 와 같은 thread에서만 이루어 지므로 clients[client]에 접근해도 안전.
		packet.move_time = this->move_time;
		});
}

void Player::send_remove_object_packet(int leaver)
{
	send_packet<sc_packet_remove_object>([leaver](sc_packet_remove_object& packet) {
		packet.id = leaver;
		packet.size = sizeof(packet);
		packet.type = SC_REMOVE_OBJECT;
		});
}

void Player::send_chat_packet(int teller, char* mess)
{
	send_packet<sc_packet_chat>([teller, mess](sc_packet_chat& packet) {
		packet.id = teller;
		packet.size = sizeof(packet);
		packet.type = SC_CHAT;
		strcpy_s(packet.chat, mess);
		});
}

#define PLAYER_IMPL
#include "mpsc_queue.h"
#include "consts.h"
#include "player.h"
#include "packet.h"
#include "zone.h"

using std::optional;
extern std::array<Player*, client_limit> clients;

void Player::do_rountine()
{
	for (auto i = 0; i < MAX_PLAYER_ROUTINE_LOOP_TIME; ++i) {
		optional<player_msg::PlayerMsg> msg = this->msg_queue.deq();
		if (!msg) { return; }
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
					send_remove_object_packet(this->id, m.player_id);
				}
			},
			[this](player_msg::PlayerMoved& m) {
				auto it = this->near_id.find(m.player_id);
				if (it != this->near_id.end()) {
					send_pos_packet(this->id, m.player_id, m.x, m.y);
				}
				else {
					this->near_id.emplace(m.player_id);
					send_put_object_packet(this->id, m.player_id, m.x, m.y);
				}
			},
			[this](player_msg::Logout& _) {
				this->is_connected = false;
				closesocket(this->socket);

				this->curr_zone->msg_queue.emplace(zone_msg::PlayerLeave{ this->id });
				for (int id : this->near_id) {
					clients[id]->msg_queue.emplace(player_msg::PlayerLeaved{ this->id });
				}
				empty_ids.emplace(id, system_clock::now());
			},
		}, *msg);
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
			// 내가 처음 위치를 전송 받았으면
			if (new_id == this->id) {
				this->is_connected = true;
			}
			send_put_object_packet(this->id, new_id, clients[new_id]->x, clients[new_id]->y);
		}
		else {
			send_pos_packet(this->id, new_id, clients[new_id]->x, clients[new_id]->y);
		}
		if (new_id != this->id) {
			auto other = clients[new_id];
			other->msg_queue.emplace(player_msg::PlayerMoved{ this->id, this->x, this->y });
		}
	}

	for (int old_id : old_near) {
		auto it = new_near.find(old_id);
		if (it == new_near.end()) {
			send_remove_object_packet(this->id, old_id);

			auto other = clients[old_id];
			other->msg_queue.emplace(player_msg::PlayerLeaved{ this->id });
		}
	}

	old_near = std::move(new_near);
}

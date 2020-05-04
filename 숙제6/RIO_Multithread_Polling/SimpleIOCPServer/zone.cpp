#define ZONE_IMPL
#include "zone.h"
#include "protocol.h"

// thread 1개가 여러 zone을 담당하긴 하지만, 한 thread에 묶여있는 zone은 sequential하게 처리되므로 send_queue도 thread 개수 만큼만 있으면 된다

void Zone::do_routine(std::array<Player*, client_limit>& client_list)
{
	for (auto i = 0; i < MAX_ZONE_ROUTINE_LOOP_TIME; ++i) {
		optional<zone_msg::ZoneMsg> msg = this->msg_queue.deq();
		if (!msg) { return; }
		std::visit(overloaded{
			[this, &client_list](zone_msg::SendPlayerList& m) {
				Player* me = client_list[m.player_id];
				send_near_players(me, m.x, m.y, m.stamp, client_list, m.total_list_num);
			},
			[this, &client_list](zone_msg::PlayerIn& m) {
				this->clients.emplace(m.player_id);
				Player* me = client_list[m.player_id];
				me->x = m.x;
				me->y = m.y;

				auto near_zones = get_near_zones(m.x, m.y);
				for (auto near_zone : near_zones) {
					if (near_zone == this) continue;
					near_zone->msg_queue.emplace(zone_msg::SendPlayerList{ m.player_id, m.stamp, m.x, m.y, (int)near_zones.size() });
				}
				me->curr_zone = this;
				send_near_players(me, m.x, m.y, m.stamp, client_list, near_zones.size());
			},
			[this, &client_list](zone_msg::PlayerMove& m) {
				auto bound = this->get_bound();
				auto me = client_list[m.player_id];

				if (!bound.is_in(m.x, m.y)) {
					this->clients.erase(m.player_id);
					auto new_zone = get_current_zone(m.x, m.y);
					new_zone->msg_queue.emplace(zone_msg::PlayerIn{ m.player_id, m.stamp, m.x, m.y });
				}
				else {
					me->x = m.x;
					me->y = m.y;

					auto near_zones = get_near_zones(m.x, m.y);
					for (auto near_zone : near_zones) {
						if (near_zone == this) continue;
						near_zone->msg_queue.emplace(zone_msg::SendPlayerList{ m.player_id, m.stamp, m.x, m.y, (int)near_zones.size() });
					}
					send_near_players(me, m.x, m.y, m.stamp, client_list, near_zones.size());
				}
			},
			[this](zone_msg::PlayerLeave& m) {
				this->clients.erase(m.player_id);
				fprintf(stderr, "Player #%d has been disconnected\n", m.player_id);
			},
			}, *msg);
	}
}

Bound Zone::get_bound() const
{
	Bound b;
	b.left = this->center_x - VIEW_RANGE;
	b.top = this->center_y - VIEW_RANGE;
	b.right = this->center_x + VIEW_RANGE;
	b.bottom = this->center_y + VIEW_RANGE;
	return b;
}

void Zone::send_near_players(Player* p, int x, int y, uint64_t stamp, std::array<Player*, client_limit>& client_list, int total_list_num) const
{
	vector<int> near_players;
	for (int id : this->clients) {
		Player* player = client_list[id];
		if (is_near(x, y, player->x, player->y)) {
			near_players.emplace_back(id);
		}
	}
	player_msg::PlayerListResponse response;
	response.near_players = std::move(near_players);
	response.stamp = stamp;
	response.total_list_num = total_list_num;
	p->msg_queue.emplace(std::move(response));
}

void init_zones()
{
	for (auto y = 0; y < ZONE_MAX_Y; ++y) {
		for (auto x = 0; x < ZONE_MAX_X; ++x) {
			int center_x = x * ZONE_SIZE + ZONE_SIZE / 2;
			int center_y = y * ZONE_SIZE + ZONE_SIZE / 2;
			zones.emplace_back(new Zone{ center_x, center_y });
		}
	}
}

bool is_near(int a_x, int a_y, int b_x, int b_y)
{
	if (VIEW_RANGE < abs(a_x - b_x)) return false;
	if (VIEW_RANGE < abs(a_y - b_y)) return false;
	return true;
}

Zone* get_current_zone(int x, int y)
{
	const auto zone_x = x / ZONE_SIZE;
	const auto zone_y = y / ZONE_SIZE;
	return zones[zone_y * ZONE_MAX_X + zone_x];
}

vector<Zone*> get_near_zones(int x, int y)
{
	vector<Zone*> near_zones;
	auto right = (int)floor((float)x / (float)ZONE_SIZE + 0.5f);
	auto bottom = (int)floor((float)y / (float)ZONE_SIZE + 0.5f);

	vector<int> xs;
	vector<int> ys;
	if (right == 0) {
		xs.push_back(right);
	}
	else if (right == ZONE_MAX_X) {
		xs.push_back(right - 1);
	}
	else {
		xs.push_back(right);
		xs.push_back(right - 1);
	}

	if (bottom == 0) {
		ys.push_back(bottom);
	}
	else if (bottom == ZONE_MAX_Y) {
		ys.push_back(bottom - 1);
	}
	else {
		ys.push_back(bottom);
		ys.push_back(bottom - 1);
	}

	for (auto _x : xs) {
		for (auto _y : ys) {
			near_zones.emplace_back(zones[_y * ZONE_MAX_X + _x]);
		}
	}

	return near_zones;
}

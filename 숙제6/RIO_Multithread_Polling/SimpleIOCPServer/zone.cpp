#define ZONE_IMPL
#include "zone.h"
#include "protocol.h"

constexpr int32_t ceil(float num)
{
    return (static_cast<float>(static_cast<int32_t>(num)) == num)
        ? static_cast<int32_t>(num)
        : static_cast<int32_t>(num) + ((num > 0) ? 1 : 0);
}

void Zone::do_routine(std::array<Player*, client_limit>& client_list)
{
	for (auto i = 0; i < MAX_ROUTINE_LOOP_TIME; ++i) {
		optional<zone_msg::ZoneMsg> msg = this->msg_queue.deq();
		if (!msg) { return; }
		std::visit(overloaded{
			[this, &client_list](zone_msg::SendPlayerList& m) {
				Player* me = client_list[m.player_id];
				send_near_players(me, client_list);
			},
			[this, &client_list](zone_msg::PlayerIn& m) {
				this->clients.emplace(m.player_id);
				for (Zone* near_zone : this->near_zones) {
					near_zone->msg_queue.emplace(zone_msg::SendPlayerList{ m.player_id });
				}
				Player* me = client_list[m.player_id];
				me->curr_zone = this;
				send_near_players(me, client_list);
			},
			[this, &client_list](zone_msg::PlayerMove& m) {
				auto bound = this->get_bound();
				auto me = client_list[m.player_id];
				if (!bound.is_in(me->x, me->y)) {
					this->clients.erase(m.player_id);
					auto new_zone = get_current_zone(me->x, me->y);
					new_zone->msg_queue.emplace(zone_msg::PlayerIn{ m.player_id });
				}
				else {
					for (Zone* near_zone : this->near_zones) {
						near_zone->msg_queue.emplace(zone_msg::SendPlayerList{ m.player_id });
					}
					send_near_players(me, client_list);
				}
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

void Zone::send_near_players(Player* p, std::array<Player*, client_limit>& client_list) const
{
	vector<int> near_players;
	for (int id : this->clients) {
		Player* player = client_list[id];
		if (is_near(p->x, p->y, player->x, player->y)) {
			near_players.emplace_back(id);
		}
	}
	player_msg::PlayerListResponse response;
	response.near_players = std::move(near_players);
	p->msg_queue.emplace(std::move(response));
}

bool is_near(int a_x, int a_y, int b_x, int b_y)
{
	if (VIEW_RANGE < abs(a_x - b_x)) return false;
	if (VIEW_RANGE < abs(a_y - b_y)) return false;
	return true;
}

Zone* get_current_zone(int x, int y)
{
	constexpr auto zone_size = VIEW_RANGE * 2 + 1;
	constexpr auto zone_max_x = ceil((float)WORLD_WIDTH / (float)VIEW_RANGE);
	constexpr auto zone_max_y = ceil((float)WORLD_HEIGHT / (float)VIEW_RANGE);
	const auto zone_x = x / zone_size;
	const auto zone_y = y / zone_size;
	return &zones[zone_y * zone_max_x + zone_x];
}

#pragma once
#include <vector>
#include <variant>
#include <set>
#include <array>
#include "player.h"
#include "consts.h"
#include "mpsc_queue.h"
#include "packet.h"

using std::vector, std::variant, std::set, std::optional;

constexpr int VIEW_RANGE = 7;

namespace zone_msg {
	struct SendPlayerList {
		int player_id;
		uint64_t stamp;
		int x, y;
	};
	struct PlayerIn {
		int player_id;
		uint64_t stamp;
		int x, y;
	};
	struct PlayerMove {
		int player_id;
		uint64_t stamp;
		int x, y;
	};
	struct PlayerLeave {
		int player_id;
	};

	using ZoneMsg = variant<SendPlayerList, PlayerIn, PlayerMove, PlayerLeave>;
}

struct Bound {
	int left, top, right, bottom;

	bool is_in(int x, int y) const {
		if (x < left) return false;
		if (x > right) return false;
		if (y < top) return false;
		if (y > bottom) return false;
		return true;
	}
};

struct Zone {
	int center_x, center_y;
	set<int> clients;
	MPSCQueue<zone_msg::ZoneMsg> msg_queue;
	MPSCQueue<RequestInfo*>& send_queue;
	vector<Zone*> near_zones;

	Zone(int x, int y, MPSCQueue<RequestInfo*>& send_queue) : center_x{ x }, center_y{ y }, send_queue{ send_queue } {}

	void do_routine(std::array<Player*, client_limit>& client_list);
	void send_near_players(Player* p, int x, int y, uint64_t stamp, std::array<Player*, client_limit>& client_list) const;
	Bound get_bound() const;
};

#ifdef ZONE_IMPL
std::vector<Zone> zones;
#else
extern std::vector<Zone> zones;
#endif

bool is_near(int a_x, int a_y, int b_x, int b_y);
Zone* get_current_zone(int x, int y);
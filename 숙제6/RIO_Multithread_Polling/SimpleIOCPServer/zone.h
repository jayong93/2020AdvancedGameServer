#pragma once
#include <vector>
#include <variant>
#include <set>
#include <array>
#include "player.h"
#include "consts.h"
#include "mpsc_queue.h"
#include "packet.h"
#include "protocol.h"

using std::vector, std::variant, std::set, std::optional;

constexpr int32_t ceil_constexpr(float num)
{
    return (static_cast<float>(static_cast<int32_t>(num)) == num)
        ? static_cast<int32_t>(num)
        : static_cast<int32_t>(num) + ((num > 0) ? 1 : 0);
}

constexpr int VIEW_RANGE = 7;
constexpr auto ZONE_SIZE = VIEW_RANGE * 2 + 1;
constexpr auto ZONE_MAX_X = ceil_constexpr((float)WORLD_WIDTH / (float)VIEW_RANGE);
constexpr auto ZONE_MAX_Y = ceil_constexpr((float)WORLD_HEIGHT / (float)VIEW_RANGE);
constexpr unsigned ZONE_PER_THREAD_NUM = ceil_constexpr((float)(ZONE_MAX_X * ZONE_MAX_Y) / (float)thread_num);

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
	vector<Zone*> near_zones;

	Zone(int x, int y) : center_x{ x }, center_y{ y } {}

	void do_routine(std::array<Player*, client_limit>& client_list);
	void send_near_players(Player* p, int x, int y, uint64_t stamp, std::array<Player*, client_limit>& client_list) const;
	Bound get_bound() const;
};

#ifdef ZONE_IMPL
#define EXTERN
#else
#define EXTERN extern
#endif
EXTERN std::vector<Zone> zones;
#undef EXTERN

void init_zones();

bool is_near(int a_x, int a_y, int b_x, int b_y);
Zone* get_current_zone(int x, int y);
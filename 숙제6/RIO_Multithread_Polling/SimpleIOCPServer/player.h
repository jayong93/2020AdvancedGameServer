#pragma once
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <mutex>
#include <set>
#include <atomic>
#include <variant>
#include <vector>
#include <map>
#include <chrono>
#include "protocol.h"
#include "mpsc_queue.h"

using namespace std::chrono;

struct EmptyID {
	int id;
	system_clock::time_point out_time;
};
#ifdef PLAYER_IMPL
#define EXTERN
#else
#define EXTERN extern
#endif
EXTERN MPSCQueue<EmptyID> empty_ids;

#undef EXTERN

struct Zone;
namespace player_msg {
	struct PlayerListResponse {
		std::vector<int> near_players;
		uint64_t stamp;
		int total_list_num;
	};
	struct PlayerMoved {
		int player_id;
		int x, y;
	};
	struct PlayerLeaved {
		int player_id;
	};
	struct Logout {};

	using PlayerMsg = std::variant<PlayerLeaved, PlayerListResponse, PlayerMoved, Logout>;
}

struct NearListInfo {
	std::vector<std::vector<int>> ids;
};

struct Player
{
	char* recv_buf;
	unsigned prev_packet_size = 0;
	RIO_RQ rio_rq;

	SOCKET	socket;
	int		id;
	char	name[MAX_STR_LEN] = { 0 };

	bool is_connected = true;
	short	x, y;
	unsigned move_time = 0;
	std::set <int> near_id;

	MPSCQueue<player_msg::PlayerMsg> msg_queue;
	// zone routine에서 player_in이 처리될 때 설정 됨.
	Zone* curr_zone;
	uint64_t stamp = 0;
	std::map<uint64_t, NearListInfo> pending_near_request;
	uint64_t pending_sends = 0;

	void do_rountine();
	void update_near_list(NearListInfo&);

	Player(int id, SOCKET socket, short x, short y, char* recv_buf, RIO_RQ rq, Zone* curr_zone) : id{ id }, socket{ socket }, x{ x }, y{ y }, recv_buf{ recv_buf }, rio_rq{ rq }, curr_zone{ curr_zone } {}
	Player(const Player&) = delete;
	Player(Player&&) = delete;
};


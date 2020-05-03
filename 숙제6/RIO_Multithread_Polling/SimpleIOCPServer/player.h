#pragma once
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <mutex>
#include <set>
#include <atomic>
#include <variant>
#include <vector>
#include "protocol.h"
#include "mpsc_queue.h"

struct Zone;

namespace player_msg {
	struct PlayerListResponse {
		std::vector<int> near_players;
	};
	struct PlayerMoved {

	};
	struct PlayerLeaved {

	};

	using PlayerMsg = std::variant<PlayerLeaved, PlayerListResponse, PlayerMoved>;
}

struct Player
{
	OVERLAPPED completion_over;
	char* recv_buf;
	unsigned prev_packet_size;
	RIO_RQ rio_rq;

	SOCKET	socket;
	int		id;
	char	name[MAX_STR_LEN];

	std::atomic_bool is_connected = false;
	bool is_active;
	short	x, y;
	unsigned move_time;
	std::set <int> near_id;

	MPSCQueue<player_msg::PlayerMsg> msg_queue;
	// zone routine에서 player_in이 처리될 때 설정 됨.
	Zone* curr_zone;
};


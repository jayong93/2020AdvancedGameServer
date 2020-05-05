#pragma once
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <mutex>
#include <set>
#include <atomic>
#include <variant>
#include <vector>
#include <deque>
#include <map>
#include <chrono>
#include <string>
#include "protocol.h"
#include "mpsc_queue.h"
#include "packet.h"
#include "network.h"

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

	using PlayerMsg = std::variant<PlayerLeaved, PlayerListResponse, PlayerMoved>;
}

struct NearListInfo {
	std::vector<std::vector<int>> ids;
};

struct Player
{
	OVERLAPPED io_ov;
	char* recv_buf;
	unsigned prev_packet_size = 0;
	RIO_RQ rio_rq;
	RIO_CQ rio_cq;

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
	std::deque<RequestInfo*> delayed_sends;
	std::atomic_bool can_recv = false;
	RequestInfo* recv_request = nullptr;

	void do_rountine();
	void update_near_list(NearListInfo&);

	Player(int id, SOCKET socket, short x, short y, char* recv_buf, Zone* curr_zone) : id{ id }, socket{ socket }, x{ x }, y{ y }, recv_buf{ recv_buf }, curr_zone{ curr_zone } {}
	Player(const Player&) = delete;
	Player(Player&&) = delete;
	~Player();

	void handle_recv(size_t received_bytes);
	void process_packet(void* buff);
	void disconnect();
private:
	template<typename Packet, typename Init>
	void send_packet(Init func)
	{
		if (this->is_connected == false) return;
		RequestInfo& req_info = acquire_send_buf();

		Packet* packet = reinterpret_cast<Packet*>(rio_buffer + (req_info.rio_buf->Offset));
		func(*packet);
		req_info.rio_buf->Length = sizeof(Packet);

		if (pending_sends >= MAX_PENDING_SEND) {
			this->delayed_sends.emplace_back(&req_info);
			return;
		}

		this->send_request(req_info);
	}
	void send_request(RequestInfo& req_info);

	void send_login_ok_packet();
	void send_login_fail();
	void send_put_object_packet(int new_id, int x, int y);
	void send_pos_packet(int mover, int x, int y);
	void send_remove_object_packet(int leaver);
	void send_chat_packet(int teller, char* mess);

	void process_login(char* name);
	void process_move(char direction);
	void process_chat(char* str);
};


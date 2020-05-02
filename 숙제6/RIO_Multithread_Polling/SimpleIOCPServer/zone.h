#pragma once
#include <vector>
#include <any>
#include <array>
#include "mpsc_queue.h"

using std::vector, std::any, std::array;

struct ZoneMsg {
	enum ZoneMsgType {
		NeedNearList,
		PlayerIn,
	} type;
	any data;
};

struct Zone {
	int center_x, center_y;
	vector<int> clients;
	MPSCQueue<ZoneMsg> msg_chan;
};

unsigned get_current_zone() {

}

std::array<unsigned, 4> get_near_zones() {

}

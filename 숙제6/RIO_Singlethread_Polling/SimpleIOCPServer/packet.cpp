#define SEND_PACKET_IMPL
#include <iostream>
#include "zone.h"
#include "player.h"
#include "packet.h"

using std::cerr, std::endl;

extern thread_local int thread_id;
extern std::array<Player*, client_limit> clients;
extern RIO_EXTENSION_FUNCTION_TABLE rio_ftable;

RequestInfo* add_more_send_req() {
	constexpr int buffer_size = send_buf_num * MAX_BUFFER;
	auto buf = (PCHAR)VirtualAllocEx(GetCurrentProcess(), nullptr, buffer_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	auto buf_id = rio_ftable.RIORegisterBuffer(buf, buffer_size);

	auto req = new RequestInfo;
	req->type = EV_SEND;
	req->rio_buf = new RIO_BUF;
	req->rio_buf->BufferId = buf_id;
	req->rio_buf->Length = MAX_BUFFER;
	req->rio_buf->Offset = 0;

	for (auto i = 1; i < buffer_size / MAX_BUFFER; ++i) {
		auto req = new RequestInfo;
		req->type = EV_SEND;
		req->rio_buf = new RIO_BUF;
		req->rio_buf->BufferId = buf_id;
		req->rio_buf->Length = MAX_BUFFER;
		req->rio_buf->Offset = i * MAX_BUFFER;

		available_send_req.enq(req);
	}

	send_buf_info.num_max_bufs.fetch_add(send_buf_num, std::memory_order_release);
	send_buf_info.num_available_bufs.fetch_add(send_buf_num, std::memory_order_release);
	return req;
}


RequestInfo& acquire_send_buf()
{
	std::optional<RequestInfo*> req_info = available_send_req.deq();
	if (!req_info) {
		cerr << "No more send buffer, need to allocate more" << endl;
		req_info = add_more_send_req();
	}
	send_buf_info.num_available_bufs.fetch_sub(1, std::memory_order_acquire);

	return **req_info;
}

void release_send_buf(RequestInfo& buf)
{
	available_send_req.enq(&buf);
	send_buf_info.num_available_bufs.fetch_add(1, std::memory_order_release);
}


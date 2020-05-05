#include <iostream>
#include <map>
#include <thread>
#include <set>
#include <mutex>
#include <chrono>
#include <queue>
#include <memory>
#include <optional>
#include <atomic>
#include <array>
#include "mpsc_queue.h"
#include "player.h"
#include "packet.h"
#include "zone.h"
#include "consts.h"

using std::set, std::mutex, std::cout, std::cerr, std::wcout, std::endl, std::lock_guard, std::vector, std::make_pair, std::thread, std::unique_ptr, std::make_unique;
using namespace std::chrono;
#include <WS2tcpip.h>
#include <MSWSock.h>
#pragma comment(lib, "Ws2_32.lib")

#include "protocol.h"

float rand_float(float min, float max) {
	return ((float)rand() / (float)RAND_MAX) * (max - min) + min;
}

struct SendInfo {
	SendInfo() = default;
	SendInfo(int id, unique_ptr<char[]>&& data) : id{ id }, data{ std::move(data) } {}

	int id;
	unique_ptr<char[]> data;
};

RIO_EXTENSION_FUNCTION_TABLE rio_ftable;
PCHAR rio_buffer;
RIO_BUFFERID rio_buf_id;
thread_local int thread_id;

std::array<Player*, client_limit> clients;

int new_user_id = 0;

void error_display(const char* msg, int err_no)
{
	WCHAR* lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	cout << msg;
	wcout << L"¿¡·¯ " << lpMsgBuf << endl;
	LocalFree(lpMsgBuf);
}

void Disconnect(int id)
{
	auto client = clients[id];
	client->is_connected = false;
	closesocket(client->socket);

	client->curr_zone->msg_queue.emplace(zone_msg::PlayerLeave{ client->id });
	for (int id : client->near_id) {
		clients[id]->msg_queue.emplace(player_msg::PlayerLeaved{ client->id });
	}
}

void ProcessMove(int id, unsigned char dir)
{
	short x = clients[id]->x;
	short y = clients[id]->y;
	switch (dir) {
	case D_UP: if (y > 0) y--;
		break;
	case D_DOWN: if (y < WORLD_HEIGHT - 1) y++;
		break;
	case D_LEFT: if (x > 0) x--;
		break;
	case D_RIGHT: if (x < WORLD_WIDTH - 1) x++;
		break;
	case 99:
		x = (short)rand_float(0, WORLD_WIDTH);
		y = (short)rand_float(0, WORLD_HEIGHT);
		break;
	default:
		cerr << "Invalid Direction Error\n";
		return;
	}

	auto stamp = clients[id]->stamp++;
	clients[id]->curr_zone->msg_queue.emplace(zone_msg::PlayerMove{ id, stamp, x, y });
}

void ProcessLogin(int user_id, char* id_str)
{
	Player* client = clients[user_id];
	strcpy_s(client->name, id_str);
	send_login_ok_packet(user_id);

	Zone* my_zone = client->curr_zone;
	auto stamp = client->stamp++;
	my_zone->msg_queue.emplace(zone_msg::PlayerIn{ user_id, stamp, client->x, client->y });
}

void ProcessChat(int id, char* mess)
{
}

void ProcessPacket(int id, void* buff)
{
	char* packet = reinterpret_cast<char*>(buff);
	switch (packet[1]) {
	case CS_LOGIN: {
		cs_packet_login* login_packet = reinterpret_cast<cs_packet_login*>(packet);
		ProcessLogin(id, login_packet->id);
	}
				 break;
	case CS_MOVE: {
		cs_packet_move* move_packet = reinterpret_cast<cs_packet_move*>(packet);
		clients[id]->move_time = move_packet->move_time;
		ProcessMove(id, move_packet->direction);
	}
				break;
	case CS_ATTACK:
		break;
	case CS_CHAT:
	{
		cs_packet_chat* chat_packet = reinterpret_cast<cs_packet_chat*>(packet);
		ProcessChat(id, chat_packet->chat_str);
	}
	break;
	case CS_LOGOUT:
		break;
	case CS_TELEPORT:
		ProcessMove(id, 99);
		break;
	default:
		cerr << "Invalid Packet Type Error\n";
	}
}

void do_worker(int t_id)
{
	thread_id = t_id;

	while (true)
	{
		for (auto i = ZONE_PER_THREAD_NUM * thread_id; i < min(ZONE_PER_THREAD_NUM * (thread_id + 1), zones.size()); ++i) {
			Zone& zone = *zones[i];
			zone.do_routine(clients);
		}
		for (auto i = 0; i < new_user_id; ++i) {
			auto client = clients[i];
			if (i % thread_num == thread_id) {
				client->do_rountine();
			}
		}

		RIORESULT results[100];
		auto num_result = rio_ftable.RIODequeueCompletion(rio_cq_list[thread_id], results, 100);

		if (RIO_CORRUPT_CQ == num_result) {
			fprintf(stderr, "RIODequeueCompletion error\n");
			continue;
		}

		for (unsigned long i = 0; i < num_result; ++i) {
			const RIORESULT& result = results[i];
			int client_id = result.SocketContext;
			auto req_info = (RequestInfo*)(result.RequestContext);

			if (result.BytesTransferred == 0) {
				if (EV_RECV == req_info->type) {
					delete req_info->rio_buf;
					delete req_info;
					Disconnect(client_id);
				}
				else if (EV_SEND == req_info->type) {
					clients[client_id]->pending_sends--;
					release_send_buf(*req_info);
				}
				continue;
			}


			if (EV_RECV == req_info->type) {
				auto client = clients[client_id];
				char* p = client->recv_buf;
				unsigned remain = result.BytesTransferred;
				unsigned packet_size;
				unsigned prev_packet_size = client->prev_packet_size;
				if (0 == prev_packet_size)
					packet_size = 0;
				else packet_size = p[0];

				while (remain > 0) {
					if (0 == packet_size) packet_size = p[0];
					int required = packet_size - prev_packet_size;
					if (required <= remain) {
						ProcessPacket(client_id, p);
						remain -= required;
						p += packet_size;
						prev_packet_size = 0;
						packet_size = 0;
					}
					else {
						memmove(client->recv_buf, p, remain);
						prev_packet_size += remain;
						break;
					}
				}
				client->prev_packet_size = prev_packet_size;
				req_info->rio_buf->Offset = client_id * MAX_BUFFER + prev_packet_size;
				req_info->rio_buf->Length = MAX_BUFFER - prev_packet_size;

				int ret = rio_ftable.RIOReceive(client->rio_rq, req_info->rio_buf, 1, 0, (void*)req_info);
				if (TRUE != ret) {
					int err_no = WSAGetLastError();
					if (WSA_IO_PENDING != err_no)
						error_display("RIOReceive Error :", err_no);
				}
			}
			else if (EV_SEND == req_info->type) {
				clients[client_id]->pending_sends--;
				release_send_buf(*req_info);
			}
			else {
				cerr << "Unknown Event Type :" << req_info->type << endl;
			}
		}

		set<RIO_RQ> sended_rqs;
		auto& send_queue = send_queues[thread_id];
		auto pending_num = send_queue.size();
		for (auto i = 0; i < pending_num; ++i) {
			auto send_option = send_queue.deq();
			if (!send_option) break;

			auto send = *std::move(send_option);
			auto client = clients[send.id];

			if (client->pending_sends >= MAX_PENDING_SEND) {
				send_queue.emplace(std::move(send));
				continue;
			}
			if (client->is_connected == false) {
				continue;
			}

			auto ret = rio_ftable.RIOSend(client->rio_rq, send.send_buf->rio_buf, 1, RIO_MSG_DEFER, (void*)send.send_buf);
			if (TRUE != ret) {
				int err_no = WSAGetLastError();
				switch (err_no) {
				case WSA_IO_PENDING:
					client->pending_sends++;
					sended_rqs.emplace(client->rio_rq);
					break;
				default:
					error_display("RIOSend Error :", err_no);
					release_send_buf(*send.send_buf);
				}
			}
			else {
				client->pending_sends++;
				sended_rqs.emplace(client->rio_rq);
			}
		}
		for (auto rq : sended_rqs) {
			rio_ftable.RIOSend(rq, nullptr, 0, RIO_MSG_COMMIT_ONLY, nullptr);
		}
	}
}

void init_rio(SOCKET listen_sock) {
	GUID f_table_id = WSAID_MULTIPLE_RIO;
	DWORD returned_bytes;
	DWORD result;
	result = WSAIoctl(listen_sock, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &f_table_id, sizeof(GUID), &rio_ftable, sizeof(rio_ftable), &returned_bytes, nullptr, nullptr);
	if (result == SOCKET_ERROR) {
		fprintf(stderr, "WSAIoctl with RIO Function Table has failed\n");
		exit(-1);
	}

	constexpr int buffer_size = (client_limit + send_buf_num) * MAX_BUFFER;
	rio_buffer = (PCHAR)VirtualAllocEx(GetCurrentProcess(), nullptr, buffer_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	rio_buf_id = rio_ftable.RIORegisterBuffer(rio_buffer, buffer_size);
	for (int i = 0; i < thread_num; ++i) {
		for (size_t j = 0; j < (send_buf_num / thread_num); j++)
		{
			RIO_BUF* buf = new RIO_BUF;
			buf->BufferId = rio_buf_id;
			buf->Length = MAX_BUFFER;
			buf->Offset = ((client_limit + (send_buf_num / thread_num) * i + j) * MAX_BUFFER);

			RequestInfo* req_info = new RequestInfo;
			req_info->type = EV_SEND;
			req_info->rio_buf = buf;
			req_info->thread_id = i;
			available_send_reqs[i].enq(req_info);
		}
		send_buf_infos[i].num_max_bufs.store((send_buf_num / thread_num), std::memory_order_relaxed);
		send_buf_infos[i].num_available_bufs.store((send_buf_num / thread_num), std::memory_order_relaxed);
	}

	for (auto i = 0; i < thread_num; ++i) {
		rio_cq_list[i] = rio_ftable.RIOCreateCompletionQueue(completion_queue_size, nullptr);
	}
}

void handle_connection(SOCKET clientSocket) {
	int user_id = -1;
	if (!empty_ids.is_empty()) {
		const auto& empty_id = empty_ids.peek();
		if (duration_cast<milliseconds>(empty_id.out_time.time_since_epoch()).count() > CLIENT_DELETE_PERIOD)
			empty_ids.deq();
		user_id = empty_id.id;
		delete clients[user_id];
	}
	else if (client_limit <= new_user_id + 1) {
		while (true) {
			if (empty_ids.is_empty()) {
				fprintf(stderr, "Can't accept more clients\n");
				std::this_thread::yield();
			}
			else {
				const auto& empty_id = empty_ids.peek();
				if (duration_cast<milliseconds>(empty_id.out_time.time_since_epoch()).count() > CLIENT_DELETE_PERIOD) {
					empty_ids.deq();
					user_id = empty_id.id;
					delete clients[user_id];
					break;
				}
				else {
					fprintf(stderr, "Can't accept more clients\n");
					std::this_thread::yield();
				}
			}
		}
	}
	else {
		user_id = new_user_id;
	}

	if (user_id < 0) {
		fprintf(stderr, "Something wrong with a new client id\n");
		return;
	}


	auto rq = rio_ftable.RIOCreateRequestQueue(clientSocket, MAX_PENDING_RECV, 1, MAX_PENDING_SEND, 1, rio_cq_list[user_id % thread_num], rio_cq_list[user_id % thread_num], (void*)user_id);
	if (rq == RIO_INVALID_RQ) {
		error_display("RIOCreateRequestQueue Error :", WSAGetLastError());
	}
	auto recv_buf = rio_buffer + (user_id * MAX_BUFFER);
	short x = rand_float(0, WORLD_WIDTH);
	short y = rand_float(0, WORLD_HEIGHT);
	Player* new_player = new Player{ user_id, clientSocket, x, y, recv_buf, rq, get_current_zone(x, y) };

	clients[user_id] = new_player;

	if (user_id == new_user_id) new_user_id++;

	//printf("User #%d has connected\n", user_id);

	auto req_info = new RequestInfo;
	req_info->type = EV_RECV;
	req_info->rio_buf = new RIO_BUF;
	req_info->rio_buf->BufferId = rio_buf_id;
	req_info->rio_buf->Length = MAX_BUFFER;
	req_info->rio_buf->Offset = user_id * MAX_BUFFER;

	int ret = rio_ftable.RIOReceive(new_player->rio_rq, req_info->rio_buf, 1, 0, (void*)req_info);
	if (TRUE != ret) {
		int err_no = WSAGetLastError();
		if (WSA_IO_PENDING != err_no)
			error_display("RIORecv Error :", err_no);
	}
}

bool check_if_server_busy() {
	uint64_t total_max_buf_num = 0;
	uint64_t total_available_buf_num = 0;
	for (auto i = 0; i < thread_num; ++i) {
		total_max_buf_num += send_buf_infos[i].num_max_bufs.load(std::memory_order_acquire);
		total_available_buf_num += send_buf_infos[i].num_available_bufs.load(std::memory_order_acquire);
	}
	return (total_available_buf_num < (int)((float)total_max_buf_num* SEND_BUF_RATE_ON_BUSY));
}

int main()
{
	wcout.imbue(std::locale("korean"));

	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	SOCKET listenSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_REGISTERED_IO);
	SOCKADDR_IN serverAddr;
	memset(&serverAddr, 0, sizeof(SOCKADDR_IN));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVER_PORT);
	serverAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
	if (SOCKET_ERROR == ::bind(listenSocket, (struct sockaddr*) & serverAddr, sizeof(SOCKADDR_IN))) {
		error_display("WSARecv Error :", WSAGetLastError());
	}
	listen(listenSocket, 5);

	init_rio(listenSocket);
	init_zones();

	SOCKADDR_IN clientAddr;
	int addrLen = sizeof(SOCKADDR_IN);
	memset(&clientAddr, 0, addrLen);

	vector<thread> worker_threads;
	for (auto i = 0; i < thread_num; ++i) {
		worker_threads.emplace_back(do_worker, i);
	}

	printf("Server has started\n");

	while (true) {
		if (check_if_server_busy()) {
			std::this_thread::yield();
			continue;
		}
		SOCKET clientSocket = accept(listenSocket, (struct sockaddr*) & clientAddr, &addrLen);
		if (INVALID_SOCKET == clientSocket) {
			int err_no = WSAGetLastError();
			if (WSA_IO_PENDING != err_no)
				error_display("Accept Error :", err_no);
		}
		else {
			handle_connection(clientSocket);
		}
	}

	for (auto& worker : worker_threads) {
		worker.join();
	}

	closesocket(listenSocket);
	WSACleanup();
}


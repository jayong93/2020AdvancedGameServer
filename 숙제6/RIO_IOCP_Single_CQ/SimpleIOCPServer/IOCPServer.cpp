#include <iostream>
#include <map>
#include <thread>
#include <set>
#include <mutex>
#include <chrono>
#include <queue>
#include <concurrent_unordered_map.h>
#include <concurrent_queue.h>
#include <memory>
#include <optional>
#include <atomic>
#include <array>
#include "mpsc_queue.h"
#include "player.h"
#include "packet.h"
#include "zone.h"
#include "consts.h"
#include "util.h"
#include "network.h"

using std::set, std::mutex, std::cout, std::cerr, std::wcout, std::endl, std::lock_guard, std::vector, std::make_pair, std::thread, std::unique_ptr, std::make_unique;
using namespace std::chrono;

#include "protocol.h"

thread_local int thread_id;
HANDLE	g_iocp;

std::array<Player*, client_limit> clients;

int new_user_id = 0;

void do_worker(int t_id)
{
	thread_id = t_id;

	std::unique_ptr<RIORESULT[]> results{ new RIORESULT[10000] };

	while (true)
	{
		DWORD num_byte;
		ULONG_PTR key;
		LPOVERLAPPED p_over;

		GetQueuedCompletionStatus(g_iocp, &num_byte, &key, &p_over, INFINITE);

		// zone routine
		if (key == MAXULONG_PTR) {
			int zone_idx = (int)p_over;
			zones[zone_idx]->do_routine(clients);
			PostQueuedCompletionStatus(g_iocp, 0, MAXULONG_PTR, (LPOVERLAPPED)zone_idx);
		}

		else
		{
			if (p_over == nullptr) {
				auto client = clients[key];
				client->do_rountine();

				if (client->is_connected.load(std::memory_order_relaxed))
					PostQueuedCompletionStatus(g_iocp, 0, key, nullptr);
			}
			else if(p_over == &completion_over)
			{
				auto num_result = rio_ftable.RIODequeueCompletion(rio_cq, results.get(), 10000);
				rio_ftable.RIONotify(rio_cq);

				if (RIO_CORRUPT_CQ == num_result) {
					fprintf(stderr, "RIODequeueCompletion error\n");
					continue;
				}

				for (unsigned long i = 0; i < num_result; ++i) {
					const RIORESULT& result = results[i];
					int client_id = result.SocketContext;
					auto req_info = (RequestInfo*)(result.RequestContext);
					auto client = clients[client_id];

					if (result.BytesTransferred == 0) {
						if (EV_RECV == req_info->type) {
							client->disconnect();
						}
						else if (EV_SEND == req_info->type) {
							client->pending_sends--;
							release_send_buf(*req_info);
						}
						continue;
					}

					if (EV_RECV == req_info->type) {
						client->handle_recv(result.BytesTransferred);
					}
					else if (EV_SEND == req_info->type) {
						client->pending_sends--;
						release_send_buf(*req_info);
					}
					else {
						cerr << "Unknown Event Type :" << req_info->type << endl;
					}
				}
			}
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

	ZeroMemory(&completion_over, sizeof(OVERLAPPED));
	RIO_NOTIFICATION_COMPLETION rio_noti;
	rio_noti.Type = RIO_IOCP_COMPLETION;
	rio_noti.Iocp.IocpHandle = g_iocp;
	rio_noti.Iocp.Overlapped = &completion_over;
	rio_noti.Iocp.CompletionKey = 0;
	rio_cq = rio_ftable.RIOCreateCompletionQueue(completion_queue_size, &rio_noti);
	if (rio_cq == RIO_INVALID_CQ) {
		error_display("RIOCreateCompletionQueue Error :", WSAGetLastError());
	}

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

	rio_ftable.RIONotify(rio_cq);
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

	auto recv_buf = rio_buffer + (user_id * MAX_BUFFER);
	short x = rand_float(0, WORLD_WIDTH);
	short y = rand_float(0, WORLD_HEIGHT);
	Player* new_player = new Player{ user_id, clientSocket, x, y, recv_buf, get_current_zone(x, y) };
	new_player->rio_rq = rio_ftable.RIOCreateRequestQueue(clientSocket, MAX_PENDING_RECV, 1, MAX_PENDING_SEND, 1, rio_cq, rio_cq, (void*)user_id);
	if (new_player->rio_rq == RIO_INVALID_RQ) {
		error_display("RIOCreateRequestQueue Error :", WSAGetLastError());
		delete new_player;
		return;
	}

	clients[user_id] = new_player;

	if (user_id == new_user_id) new_user_id++;

	//printf("User #%d has connected\n", user_id);

	auto req_info = new RequestInfo;
	req_info->type = EV_RECV;
	req_info->rio_buf = new RIO_BUF;
	req_info->rio_buf->BufferId = rio_buf_id;
	req_info->rio_buf->Length = MAX_BUFFER;
	req_info->rio_buf->Offset = user_id * MAX_BUFFER;
	new_player->recv_request = req_info;

	int ret = rio_ftable.RIOReceive(new_player->rio_rq, req_info->rio_buf, 1, 0, (void*)req_info);
	if (TRUE != ret) {
		int err_no = WSAGetLastError();
		if (WSA_IO_PENDING != err_no)
			error_display("RIORecv Error :", err_no);
	}
	PostQueuedCompletionStatus(g_iocp, 0, new_player->id, nullptr);
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

	g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
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


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

using std::set, std::mutex, std::cout, std::cerr, std::wcout, std::endl, std::lock_guard, std::vector, std::make_pair, std::thread, std::unique_ptr, std::make_unique;
using namespace std::chrono;
#include <WS2tcpip.h>
#include <MSWSock.h>
#pragma comment(lib, "Ws2_32.lib")

#include "protocol.h"

#define MAX_BUFFER        1024
constexpr auto VIEW_RANGE = 7;
constexpr int MAX_PENDING_RECV = 10;
constexpr int MAX_PENDING_SEND = 5000;
constexpr int client_limit = 20000; // 예상 최대 client 수
constexpr int thread_num = 8;
constexpr int completion_queue_size = ((MAX_PENDING_RECV + MAX_PENDING_SEND) * client_limit) / thread_num;
constexpr int send_buf_num = client_limit * 50;

float rand_float(float min, float max) {
	return ((float)rand() / (float)RAND_MAX) * (max - min) + min;
}

struct SendInfo {
	SendInfo() = default;
	SendInfo(int id, unique_ptr<char[]>&& data) : id{ id }, data{ std::move(data) } {}

	int id;
	unique_ptr<char[]> data;
};

enum EVENT_TYPE { EV_RECV, EV_SEND, EV_MOVE, EV_PLAYER_MOVE_NOTIFY, EV_MOVE_TARGET, EV_ATTACK, EV_HEAL };

struct RequestInfo {
	EVENT_TYPE type;
	RIO_BUF* rio_buf;
	int thread_id;
};

struct EmptyID {
	int id;
	system_clock::time_point out_time;
};

RIO_EXTENSION_FUNCTION_TABLE rio_ftable;
PCHAR rio_buffer;
RIO_BUFFERID rio_buf_id;
Concurrency::concurrent_queue<RequestInfo*> available_send_reqs[thread_num];
MPSCQueue<EmptyID> empty_ids;
RIO_CQ rio_cq_list[thread_num];
thread_local int thread_id;

struct SOCKETINFO
{
	OVERLAPPED completion_over;
	char* recv_buf;
	RIO_RQ	rio_rq;
	mutex rq_lock;

	char	pre_net_buf[MAX_BUFFER];
	int		prev_packet_size;
	SOCKET	socket;
	int		id;
	char	name[MAX_STR_LEN];

	std::atomic_bool is_connected = false;
	bool is_active;
	short	x, y;
	unsigned move_time;
	set <int> near_id;
	mutex near_lock;
};

std::array<SOCKETINFO*, client_limit> clients;

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
	wcout << L"에러 " << lpMsgBuf << endl;
	LocalFree(lpMsgBuf);
}

bool is_near(int a, int b)
{
	if (VIEW_RANGE < abs(clients[a]->x - clients[b]->x)) return false;
	if (VIEW_RANGE < abs(clients[a]->y - clients[b]->y)) return false;
	return true;
}

void Disconnect(int);

void send_packet(int id, void* buff)
{
	auto client = clients[id];
	if (false == client->is_connected.load(std::memory_order_acquire)) { return; }

	char* packet = reinterpret_cast<char*>(buff);
	auto data_size = packet[0];

	RequestInfo* req_info;
	bool is_success = available_send_reqs[thread_id].try_pop(req_info);
	if (!is_success) {
		cerr << "No more send buffer, need to allocate more" << endl;
		constexpr int buffer_size = (send_buf_num / thread_num) * MAX_BUFFER;
		auto buf = (PCHAR)VirtualAllocEx(GetCurrentProcess(), nullptr, buffer_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		auto buf_id = rio_ftable.RIORegisterBuffer(buf, buffer_size);

		auto req = new RequestInfo;
		req->thread_id = thread_id;
		req->type = EV_SEND;
		req->rio_buf = new RIO_BUF;
		req->rio_buf->BufferId = buf_id;
		req->rio_buf->Length = MAX_BUFFER;
		req->rio_buf->Offset = 0;
		req_info = req;

		for (auto i = 1; i < buffer_size / MAX_BUFFER; ++i) {
			auto req = new RequestInfo;
			req->thread_id = thread_id;
			req->type = EV_SEND;
			req->rio_buf = new RIO_BUF;
			req->rio_buf->BufferId = buf_id;
			req->rio_buf->Length = MAX_BUFFER;
			req->rio_buf->Offset = i * MAX_BUFFER;

			available_send_reqs[thread_id].push(req);
		}
	}

	memcpy_s(rio_buffer + (req_info->rio_buf->Offset), MAX_BUFFER, packet, data_size);
	req_info->rio_buf->Length = data_size;

	int ret;
	{
		lock_guard<mutex> lg{ client->rq_lock };
		ret = rio_ftable.RIOSend(client->rio_rq, req_info->rio_buf, 1, 0, (void*)req_info);
	}
	if (TRUE != ret) {
		int err_no = WSAGetLastError();
		switch (err_no) {
		case WSA_IO_PENDING:
			break;
		case WSAECONNRESET:
		case WSAECONNABORTED:
		case WSAENOTSOCK:
			available_send_reqs[thread_id].push(req_info);
			Disconnect(id);
			break;
		default:
			error_display("RIOSend Error :", err_no);
			available_send_reqs[thread_id].push(req_info);
		}
	}
}

void send_login_ok_packet(int id)
{
	sc_packet_login_ok packet;
	packet.id = id;
	packet.size = sizeof(packet);
	packet.type = SC_LOGIN_OK;
	packet.x = clients[id]->x;
	packet.y = clients[id]->y;
	packet.hp = 100;
	packet.level = 1;
	packet.exp = 1;
	send_packet(id, &packet);
}

void send_login_fail(int id)
{
	sc_packet_login_fail packet;
	packet.size = sizeof(packet);
	packet.type = SC_LOGIN_FAIL;
	send_packet(id, &packet);
}

void send_put_object_packet(int client, int new_id)
{
	sc_packet_put_object packet;
	packet.id = new_id;
	packet.size = sizeof(packet);
	packet.type = SC_PUT_OBJECT;
	packet.x = clients[new_id]->x;
	packet.y = clients[new_id]->y;
	packet.o_type = 1;
	send_packet(client, &packet);

	if (client == new_id) return;
	lock_guard<mutex>lg{ clients[client]->near_lock };
	clients[client]->near_id.insert(new_id);
}

void send_pos_packet(int client, int mover)
{
	sc_packet_pos packet;
	packet.id = mover;
	packet.size = sizeof(packet);
	packet.type = SC_POS;
	packet.x = clients[mover]->x;
	packet.y = clients[mover]->y;
	packet.move_time = clients[client]->move_time;

	clients[client]->near_lock.lock();
	if ((client == mover) || 0 != clients[client]->near_id.count(mover)) {
		clients[client]->near_lock.unlock();
		send_packet(client, &packet);
	}
	else {
		clients[client]->near_lock.unlock();
		send_put_object_packet(client, mover);
	}
}

void send_remove_object_packet(int client, int leaver)
{
	sc_packet_remove_object packet;
	packet.id = leaver;
	packet.size = sizeof(packet);
	packet.type = SC_REMOVE_OBJECT;
	send_packet(client, &packet);

	lock_guard<mutex>lg{ clients[client]->near_lock };
	clients[client]->near_id.erase(leaver);
}

void send_chat_packet(int client, int teller, char* mess)
{
	sc_packet_chat packet;
	packet.id = teller;
	packet.size = sizeof(packet);
	packet.type = SC_CHAT;
	send_packet(client, &packet);
}

bool is_near_id(int player, int other)
{
	lock_guard <mutex> gl{ clients[player]->near_lock };
	return (0 != clients[player]->near_id.count(other));
}

void Disconnect(int id)
{
	clients[id]->is_connected.store(false, std::memory_order_release);
	closesocket(clients[id]->socket);
	printf("User #%d has disconnected\n", id);
	for (auto i = 0; i < new_user_id; ++i) {
		auto cl = clients[i];
		if (true == cl->is_connected.load(std::memory_order_acquire))
			send_remove_object_packet(cl->id, id);
	}

	EmptyID empty_id{ id, system_clock::now() };
	empty_ids.enq(empty_id);
}

void ProcessMove(int id, unsigned char dir)
{
	short x = clients[id]->x;
	short y = clients[id]->y;
	clients[id]->near_lock.lock();
	auto old_vl = clients[id]->near_id;
	clients[id]->near_lock.unlock();
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
		x = rand() % WORLD_WIDTH;
		y = rand() % WORLD_HEIGHT;
		break;
	default: cout << "Invalid Direction Error\n";
		while (true);
	}

	clients[id]->x = x;
	clients[id]->y = y;

	set <int> new_vl;
	for (auto i = 0; i < new_user_id; ++i) {
		auto cl = clients[i];
		int other = cl->id;
		if (id == other) continue;
		if (false == clients[other]->is_connected.load(std::memory_order_acquire)) continue;
		if (true == is_near(id, other)) new_vl.insert(other);
	}


	send_pos_packet(id, id);
	for (auto cl : old_vl) {
		if (0 != new_vl.count(cl)) {
			send_pos_packet(cl, id);
		}
		else
		{
			send_remove_object_packet(id, cl);
			send_remove_object_packet(cl, id);
		}
	}
	for (auto cl : new_vl) {
		if (0 == old_vl.count(cl)) {
			send_put_object_packet(id, cl);
			send_put_object_packet(cl, id);
		}
	}
}

void ProcessLogin(int user_id, char* id_str)
{
	//for (auto cl : clients) {
	//	if (0 == strcmp(cl.second->name, id_str)) {
	//		send_login_fail(user_id);
	//		Disconnect(user_id);
	//		return;
	//	}
	//}
	strcpy_s(clients[user_id]->name, id_str);
	clients[user_id]->is_connected = true;
	send_login_ok_packet(user_id);

	for (auto i = 0; i < new_user_id; ++i) {
		auto cl = clients[i];
		int other_player = cl->id;
		if (false == clients[other_player]->is_connected.load(std::memory_order_acquire)) continue;
		if (true == is_near(other_player, user_id)) {
			send_put_object_packet(other_player, user_id);
			if (other_player != user_id) {
				send_put_object_packet(user_id, other_player);
			}
		}
	}

}

void ProcessChat(int id, char* mess)
{

	clients[id]->near_lock.lock();
	auto vl = clients[id]->near_id;
	clients[id]->near_lock.unlock();

	for (auto cl_id : vl) {
		auto cl = clients[cl_id];
		if (false == cl->is_connected.load(std::memory_order_acquire))
			continue;
		send_chat_packet(cl_id, id, mess);
	}

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
		RIORESULT results[100];
		auto num_result = rio_ftable.RIODequeueCompletion(rio_cq_list[thread_id], results, 100);

		if (RIO_CORRUPT_CQ == num_result) {
			error_display("RIODequeueCompletion error", WSAGetLastError());
			exit(-1);
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
					available_send_reqs[req_info->thread_id].push(req_info);
				}
				continue;
			}  // 클라이언트가 closesocket을 했을 경우		
			//OVER_EX* over_ex = reinterpret_cast<OVER_EX*> (p_over);


			if (EV_RECV == req_info->type) {
				auto client = clients[client_id];
				char* p = client->recv_buf;
				int remain = result.BytesTransferred;
				int packet_size;
				int prev_packet_size = client->prev_packet_size;
				if (0 == prev_packet_size)
					packet_size = 0;
				else packet_size = client->pre_net_buf[0];
				while (remain > 0) {
					if (0 == packet_size) packet_size = p[0];
					int required = packet_size - prev_packet_size;
					if (required <= remain) {
						memcpy(client->pre_net_buf + prev_packet_size, p, required);
						ProcessPacket(client_id, client->pre_net_buf);
						remain -= required;
						p += required;
						prev_packet_size = 0;
						packet_size = 0;
					}
					else {
						memcpy(client->pre_net_buf + prev_packet_size, p, remain);
						prev_packet_size += remain;
						remain = 0;
					}
				}
				client->prev_packet_size = prev_packet_size;

				int ret;
				{
					lock_guard<mutex> lg{ client->rq_lock };
					ret = rio_ftable.RIOReceive(client->rio_rq, req_info->rio_buf, 1, 0, (void*)req_info);
				}
				if (TRUE != ret) {
					int err_no = WSAGetLastError();
					if (WSA_IO_PENDING != err_no)
						error_display("RIOReceive Error :", err_no);
				}
			}
			else if (EV_SEND == req_info->type) {
				available_send_reqs[req_info->thread_id].push(req_info);
			}
			else {
				cout << "Unknown Event Type :" << req_info->type << endl;
				while (true);
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
			available_send_reqs[i].push(req_info);
		}
	}

	for (auto i = 0; i < thread_num; ++i) {
		rio_cq_list[i] = rio_ftable.RIOCreateCompletionQueue(completion_queue_size, nullptr);
	}
}

void handle_connection(SOCKET clientSocket) {
	int user_id = -1;
	std::optional<EmptyID> empty_id = empty_ids.peek();
	if (empty_id && duration_cast<milliseconds>(empty_id->out_time.time_since_epoch()).count() > 2000) {
		empty_ids.deq();
		user_id = empty_id->id;
	}
	else if (client_limit <= new_user_id + 1) {
		while (true) {
			empty_id = empty_ids.peek();
			if (!empty_id) {
				fprintf(stderr, "Can't accept more clients\n");
				std::this_thread::yield();
			}
			else if (duration_cast<milliseconds>(empty_id->out_time.time_since_epoch()).count() > 2000) {
				empty_ids.deq();
				user_id = empty_id->id;
				break;
			}
			else {
				fprintf(stderr, "Can't accept more clients\n");
				std::this_thread::yield();
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


	SOCKETINFO* new_player = new SOCKETINFO;
	new_player->id = user_id;
	new_player->socket = clientSocket;
	new_player->prev_packet_size = 0;
	new_player->recv_buf = rio_buffer + (user_id * MAX_BUFFER);

	ZeroMemory(&new_player->completion_over, sizeof(OVERLAPPED));
	new_player->rio_rq = rio_ftable.RIOCreateRequestQueue(clientSocket, MAX_PENDING_RECV, 1, MAX_PENDING_SEND, 1, rio_cq_list[user_id % thread_num], rio_cq_list[user_id % thread_num], (void*)user_id);
	if (new_player->rio_rq == RIO_INVALID_RQ) {
		error_display("RIOCreateRequestQueue Error :", WSAGetLastError());
	}
	new_player->x = rand_float(0, WORLD_WIDTH);
	new_player->y = rand_float(0, WORLD_HEIGHT);
	clients[user_id] = new_player;

	if (user_id == new_user_id) new_user_id++;

	//printf("User #%d has connected\n", user_id);

	auto req_info = new RequestInfo;
	req_info->type = EV_RECV;
	req_info->rio_buf = new RIO_BUF;
	req_info->rio_buf->BufferId = rio_buf_id;
	req_info->rio_buf->Length = MAX_BUFFER;
	req_info->rio_buf->Offset = user_id * MAX_BUFFER;

	int ret;
	{
		lock_guard<mutex> lg{ new_player->rq_lock };
		ret = rio_ftable.RIOReceive(new_player->rio_rq, req_info->rio_buf, 1, 0, (void*)req_info);
	}
	if (TRUE != ret) {
		int err_no = WSAGetLastError();
		if (WSA_IO_PENDING != err_no)
			error_display("RIORecv Error :", err_no);
	}
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

	SOCKADDR_IN clientAddr;
	int addrLen = sizeof(SOCKADDR_IN);
	memset(&clientAddr, 0, addrLen);

	vector<thread> worker_threads;
	for (auto i = 0; i < thread_num; ++i) {
		worker_threads.emplace_back(do_worker, i);
	}

	printf("Server has started\n");

	while (true) {
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


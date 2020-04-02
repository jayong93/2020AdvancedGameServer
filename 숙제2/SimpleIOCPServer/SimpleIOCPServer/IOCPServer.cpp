#include <iostream>
#include <map>
#include <thread>
#include <set>
#include <mutex>
#include <chrono>
#include <queue>
#include <concurrent_unordered_map.h>
#include "mpsc_queue.h"

using std::set, std::mutex, std::cout, std::wcout, std::endl, std::lock_guard, std::vector, std::make_pair, std::thread;
using namespace std::chrono;
#include <WS2tcpip.h>
#include <MSWSock.h>
#pragma comment(lib, "Ws2_32.lib")

#include "protocol.h"

#define MAX_BUFFER        1024
constexpr auto VIEW_RANGE = 3;
constexpr int MAX_PENDING_RECV = 1000;
constexpr int MAX_PENDING_SEND = 1000;
constexpr int client_limit = 10000; // 예상 최대 client 수
constexpr int completion_queue_size = (MAX_PENDING_RECV + MAX_PENDING_SEND) * client_limit;

struct SendInfo {
	SendInfo() = default;
	SendInfo(int id, const char* data) : id{ id }, data{ data } {}

	int id;
	const char* data;
};

RIO_EXTENSION_FUNCTION_TABLE rio_ftable;
PCHAR rio_buffer;
RIO_BUFFERID rio_buf_id;
MPSCQueue<SendInfo> send_queue;

enum EVENT_TYPE { EV_RECV, EV_SEND, EV_MOVE, EV_PLAYER_MOVE_NOTIFY, EV_MOVE_TARGET, EV_ATTACK, EV_HEAL };

struct SOCKETINFO
{
	RIO_BUF rio_recv_buf;
	RIO_BUF rio_send_buf;
	char* recv_buf;
	char* send_buf;
	mutex send_buf_lock;
	RIO_RQ	rio_rq;
	RIO_CQ	rio_cq;

	char	pre_net_buf[MAX_BUFFER];
	int		prev_packet_size;
	SOCKET	socket;
	int		id;
	char	name[MAX_STR_LEN];

	bool is_connected;
	bool is_active;
	short	x, y;
	set <int> near_id;
	mutex near_lock;
};

Concurrency::concurrent_unordered_map <int, SOCKETINFO*> clients;
HANDLE	g_iocp;
OVERLAPPED g_over;

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
	while (true);
	LocalFree(lpMsgBuf);
}

bool is_near(int a, int b)
{
	if (VIEW_RANGE < abs(clients[a]->x - clients[b]->x)) return false;
	if (VIEW_RANGE < abs(clients[a]->y - clients[b]->y)) return false;
	return true;
}

void send_packet(int id, void* buff)
{
	//char* packet = reinterpret_cast<char*>(buff);
	//int packet_size = packet[0];
	//OVER_EX* send_over = new OVER_EX;
	//memset(send_over, 0x00, sizeof(OVER_EX));
	//send_over->event_type = EV_SEND;
	//memcpy(send_over->net_buf, packet, packet_size);
	//send_over->wsabuf[0].buf = send_over->net_buf;
	//send_over->wsabuf[0].len = packet_size;
	//int ret = WSASend(clients[id]->socket, send_over->wsabuf, 1, 0, 0, &send_over->over, 0);
	//if (0 != ret) {
	//	int err_no = WSAGetLastError();
	//	if (WSA_IO_PENDING != err_no)
	//		error_display("WSARecv Error :", err_no);
	//}
	char* packet = reinterpret_cast<char*>(buff);
	int packet_size = packet[0];

	char* p_data = new char[packet_size];
	memcpy_s(p_data, packet_size, packet, packet_size);
	send_queue.enq(SendInfo(id, p_data));
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

	clients[client]->near_lock.lock();
	if (0 != clients[client]->near_id.count(mover)) {
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

bool is_near_id(int player, int other)
{
	lock_guard <mutex> gl{ clients[player]->near_lock };
	return (0 != clients[player]->near_id.count(other));
}

void Disconnect(int id)
{
	clients[id]->is_connected = false;
	closesocket(clients[id]->socket);
	for (auto& cl : clients) {
		if (true == cl.second->is_connected)
			send_remove_object_packet(cl.first, id);
	}
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
	for (auto& cl : clients) {
		int other = cl.second->id;
		if (id == other) continue;
		if (false == clients[other]->is_connected) continue;
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
	for (auto cl : clients) {
		if (0 == strcmp(cl.second->name, id_str)) {
			send_login_fail(user_id);
			Disconnect(user_id);
			return;
		}
	}
	strcpy_s(clients[user_id]->name, id_str);
	send_login_ok_packet(user_id);
	clients[user_id]->is_connected = true;

	for (auto& cl : clients) {
		int other_player = cl.first;
		if (false == clients[other_player]->is_connected) continue;
		if (true == is_near(other_player, user_id)) {
			send_put_object_packet(other_player, user_id);
			if (other_player != user_id) {
				send_put_object_packet(user_id, other_player);
			}
		}
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
		ProcessMove(id, move_packet->direction);
	}
				break;
	case CS_ATTACK:
		break;
	case CS_CHAT:
		break;
	case CS_LOGOUT:
		break;
	case CS_TELEPORT:
		ProcessMove(id, 99);
		break;
	default: cout << "Invalid Packet Type Error\n";
		while (true);
	}
}

void do_worker()
{
	while (true) {
		DWORD num_byte;
		ULONGLONG key64;
		PULONG_PTR p_key = &key64;
		WSAOVERLAPPED* p_over;

		GetQueuedCompletionStatus(g_iocp, &num_byte, p_key, &p_over, INFINITE);
		int key = static_cast<int>(key64);
		auto client = clients[key];

		RIORESULT results[10];
		auto num_result = rio_ftable.RIODequeueCompletion(client->rio_cq, results, 10);
		for (unsigned long i = 0; i < num_result; ++i) {
			const RIORESULT& result = results[i];
			SOCKET client_s = result.SocketContext;
			if (result.BytesTransferred == 0) {
				Disconnect(key);
				continue;
			}  // 클라이언트가 closesocket을 했을 경우		
			//OVER_EX* over_ex = reinterpret_cast<OVER_EX*> (p_over);

			if (EV_RECV == result.RequestContext) {
				char* p = client->recv_buf;
				int remain = num_byte;
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
						ProcessPacket(key, client->pre_net_buf);
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

				rio_ftable.RIOReceive(client->rio_rq, &client->rio_recv_buf, 1, 0, (void*)EV_RECV);
			}
			else if (EV_SEND == result.RequestContext) {
			}
			else {
				cout << "Unknown Event Type :" << result.RequestContext << endl;
				while (true);
			}
		}
	}
}

void broadcast() {
	while (true) {
		while (true) {
			auto retval = send_queue.deq();
			if (!retval) {
				break;
			}

			auto& send_info = *retval;
			auto client = clients[send_info.id];
			unsigned char data_size = send_info.data[0];

			lock_guard<mutex> lg{ client->send_buf_lock };
			memcpy_s(client->send_buf, MAX_BUFFER, send_info.data, data_size);
			client->rio_send_buf.Length = data_size;

			int ret = rio_ftable.RIOSend(client->rio_rq, &client->rio_send_buf, 1, 0, (void*)EV_SEND);
			if (0 != ret) {
				int err_no = WSAGetLastError();
				if (WSA_IO_PENDING != err_no)
					error_display("WSASend Error :", err_no);
			}
		}
		std::this_thread::yield();
	}
}

void init_rio(SOCKET listen_sock) {
	GUID f_table_id = WSAID_MULTIPLE_RIO;
	DWORD returned_bytes;
	DWORD result;
	result = WSAIoctl(listen_sock, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &f_table_id, sizeof(GUID), &rio_ftable, sizeof(rio_ftable), &returned_bytes, nullptr, nullptr);
	if (result == SOCKET_ERROR) {
		error_display("WSAIoctl Error(in 'init_rio') :", WSAGetLastError());
		exit(-1);
	}

	constexpr int buffer_size = client_limit * MAX_BUFFER * 2;
	rio_buffer = (PCHAR)VirtualAllocEx(GetCurrentProcess(), nullptr, buffer_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	rio_buf_id = rio_ftable.RIORegisterBuffer(rio_buffer, buffer_size);

	//completion_queue = rio_ftable.RIOCreateCompletionQueue(completion_queue_size, nullptr);
}

int main()
{
	wcout.imbue(std::locale("korean"));

	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	SOCKET listenSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
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
	DWORD flags;

	g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
	ZeroMemory(&g_over, sizeof(g_over));
	vector <thread> worker_threads;
	thread broadcaster;
	for (int i = 0; i < 4; ++i) worker_threads.emplace_back(do_worker);


	while (true) {
		SOCKET clientSocket = accept(listenSocket, (struct sockaddr*) & clientAddr, &addrLen);
		if (INVALID_SOCKET == clientSocket) {
			int err_no = WSAGetLastError();
			if (WSA_IO_PENDING != err_no)
				error_display("Accept Error :", err_no);
		}
		int user_id = new_user_id++;
		RIO_NOTIFICATION_COMPLETION rio_noti;
		rio_noti.Type = RIO_IOCP_COMPLETION;
		rio_noti.Iocp.IocpHandle = g_iocp;
		rio_noti.Iocp.Overlapped = &g_over;
		rio_noti.Iocp.CompletionKey = (void*)user_id;

		SOCKETINFO* new_player = new SOCKETINFO;
		new_player->id = user_id;
		new_player->socket = clientSocket;
		new_player->prev_packet_size = 0;
		new_player->recv_buf = rio_buffer + (user_id * MAX_BUFFER * 2);
		new_player->send_buf = new_player->recv_buf + MAX_BUFFER;
		new_player->rio_recv_buf.BufferId = rio_buf_id;
		new_player->rio_recv_buf.Length = MAX_BUFFER;
		new_player->rio_recv_buf.Offset = (user_id * MAX_BUFFER * 2);
		new_player->rio_send_buf.BufferId = rio_buf_id;
		new_player->rio_send_buf.Length = MAX_BUFFER;
		new_player->rio_send_buf.Offset = (user_id * MAX_BUFFER * 2) + MAX_BUFFER;
		new_player->rio_cq = rio_ftable.RIOCreateCompletionQueue(completion_queue_size, &rio_noti);
		new_player->rio_rq = rio_ftable.RIOCreateRequestQueue(clientSocket, MAX_PENDING_RECV, 1, MAX_PENDING_SEND, 1, new_player->rio_cq, new_player->rio_cq, (void*)clientSocket);
		new_player->x = 4;
		new_player->y = 4;
		clients.insert(make_pair(user_id, new_player));

		flags = 0;
		int ret = rio_ftable.RIOSend(new_player->rio_rq, &new_player->rio_recv_buf, 1, 0, (void*)EV_RECV);
		//int ret = WSARecv(clientSocket, clients[user_id]->recv_over.wsabuf, 1, NULL,
		//	&flags, &(clients[user_id]->recv_over.over), NULL);
		if (0 != ret) {
			int err_no = WSAGetLastError();
			if (WSA_IO_PENDING != err_no)
				error_display("WSARecv Error :", err_no);
		}
	}
	for (auto& th : worker_threads) th.join();
	closesocket(listenSocket);
	WSACleanup();
}


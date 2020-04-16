#include <iostream>
#include <map>
#include <thread>
#include <set>
#include <mutex>
#include <chrono>
#include <queue>
#include <concurrent_unordered_map.h>

using namespace std;
using namespace chrono;
#include <WS2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

#include "protocol.h"

#define MAX_BUFFER        1024
constexpr auto VIEW_RANGE = 7;

enum EVENT_TYPE { EV_RECV, EV_SEND, EV_MOVE, EV_PLAYER_MOVE_NOTIFY, EV_MOVE_TARGET, EV_ATTACK, EV_HEAL };

struct OVER_EX {
	WSAOVERLAPPED over;
	WSABUF	wsabuf[1];
	char	net_buf[MAX_BUFFER];
	EVENT_TYPE	event_type;
};

struct SOCKETINFO
{
	OVER_EX	recv_over;
	char	pre_net_buf[MAX_BUFFER];
	int		prev_packet_size;
	SOCKET	socket;
	int		id;
	char	name[MAX_STR_LEN];

	bool is_connected;
	bool is_active;
	short	x, y;
	int		seq_no;
	set <int> near_id;
	mutex near_lock;
};

Concurrency::concurrent_unordered_map <int, SOCKETINFO*> clients;
HANDLE	g_iocp;

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
	auto client_a = clients[a];
	auto client_b = clients[b];
	if (VIEW_RANGE < abs(client_a->x - client_b->x)) return false;
	if (VIEW_RANGE < abs(client_a->y - client_b->y)) return false;
	return true;
}

void Disconnect(int id);

void send_packet(int id, void* buff)
{
	auto client = clients[id];
	if (false == client->is_connected) return;	
	
	char* packet = reinterpret_cast<char*>(buff);
	int packet_size = packet[0];
	OVER_EX* send_over = new OVER_EX;
	memset(send_over, 0x00, sizeof(OVER_EX));
	send_over->event_type = EV_SEND;
	memcpy(send_over->net_buf, packet, packet_size);
	send_over->wsabuf[0].buf = send_over->net_buf;
	send_over->wsabuf[0].len = packet_size;
	int ret = WSASend(client->socket, send_over->wsabuf, 1, 0, 0, &send_over->over, 0);
	if (0 != ret) {
		int err_no = WSAGetLastError();
		if ((WSAECONNRESET == err_no) || (WSAECONNABORTED == err_no) || (WSAENOTSOCK == err_no)) {
			Disconnect(id);
			return;
		} else
		if (WSA_IO_PENDING != err_no)
			error_display("WSASend Error :", err_no);
	}
}

void send_login_ok_packet(int id)
{
	auto client = clients[id];
	sc_packet_login_ok packet;
	packet.id = id;
	packet.size = sizeof(packet);
	packet.type = SC_LOGIN_OK;
	packet.x = client->x;
	packet.y = client->y;
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
	auto cl = clients[client];
	auto new_cl = clients[new_id];
	sc_packet_put_object packet;
	packet.id = new_id;
	packet.size = sizeof(packet);
	packet.type = SC_PUT_OBJECT;
	packet.x = new_cl->x;
	packet.y = new_cl->y;
	packet.o_type = 1;
	send_packet(client, &packet);

	if (client == new_id) return;
	lock_guard<mutex>lg{ cl->near_lock };
	cl->near_id.insert(new_id);
}

void send_pos_packet(int client, int mover)
{
	auto cl = clients[client];
	auto mv = clients[mover];
	sc_packet_pos packet;
	packet.id = mover;
	packet.size = sizeof(packet);
	packet.type = SC_POS;
	packet.x = mv->x;
	packet.y = mv->y;
	packet.seq_no = cl->seq_no;

	cl->near_lock.lock();
	if ((client == mover) || (0 != cl->near_id.count(mover))) {
		cl->near_lock.unlock();
		send_packet(client, &packet);
	}
	else {
		cl->near_lock.unlock();
		send_put_object_packet(client, mover);
	}
}

void send_remove_object_packet(int client, int leaver)
{
	auto cl = clients[client];
	sc_packet_remove_object packet;
	packet.id = leaver;
	packet.size = sizeof(packet);
	packet.type = SC_REMOVE_OBJECT;
	send_packet(client, &packet);

	lock_guard<mutex>lg{ cl->near_lock };
	cl->near_id.erase(leaver);
}

void send_chat_packet(int client, int teller, char *mess)
{
	sc_packet_chat packet;
	packet.id = teller;
	packet.size = sizeof(packet);
	packet.type = SC_CHAT;
	send_packet(client, &packet);
}

bool is_near_id(int player, int other)
{
	auto client = clients[player];
	lock_guard <mutex> gl{ client->near_lock };
	return (0 != client->near_id.count(other));
}

void Disconnect(int id)
{
	auto client = clients[id];
	client->is_connected = false;
	closesocket(client->socket);
	for (auto& cl : clients) {
		if (true == cl.second->is_connected)
			send_remove_object_packet(cl.first, id);
	}
}

void ProcessMove(int id, unsigned char dir)
{
	auto client = clients[id];
	short x = client->x;
	short y = client->y;
	client->near_lock.lock();
	auto old_vl = client->near_id;
	client->near_lock.unlock();
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

	client->x = x;
	client->y = y;

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

void ProcessChat(int id, char * mess)
{
	auto client = clients[id];
	client->near_lock.lock();
	auto vl = client->near_id;
	client->near_lock.unlock();

	for (auto cl : vl)
		send_chat_packet(cl, id, mess);
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
	auto client = clients[user_id];
	strcpy_s(client->name, id_str);
	client->is_connected = true;
	send_login_ok_packet(user_id);


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
		clients[id]->seq_no = move_packet->seq_no;
		ProcessMove(id, move_packet->direction);
	}
				break;
	case CS_ATTACK:
		break;
	case CS_CHAT: {
		cs_packet_chat* chat_packet = reinterpret_cast<cs_packet_chat*>(packet);
		ProcessChat(id, chat_packet->chat_str);
	}
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
		ULONG_PTR client_ptr;
		WSAOVERLAPPED* p_over;

		BOOL no_error = GetQueuedCompletionStatus(g_iocp, &num_byte, &client_ptr, &p_over, INFINITE);
		auto client = reinterpret_cast<SOCKETINFO*>(client_ptr);
		auto key = client->id;
		if (FALSE == no_error) {
			int err_no = WSAGetLastError();
			if (ERROR_NETNAME_DELETED == err_no) {
				Disconnect(key);
				continue;
			}
			else
			error_display("GQCS Error :", err_no);
		}
		SOCKET client_s = client->socket;
		if (num_byte == 0) {
			Disconnect(key);
			continue;
		}  // 클라이언트가 closesocket을 했을 경우		
		OVER_EX* over_ex = reinterpret_cast<OVER_EX*> (p_over);

		if (EV_RECV == over_ex->event_type) {
			char* p = over_ex->net_buf;
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

			DWORD flags = 0;
			memset(&over_ex->over, 0x00, sizeof(WSAOVERLAPPED));
			WSARecv(client_s, over_ex->wsabuf, 1, 0, &flags, &over_ex->over, 0);
		}
		else if (EV_SEND == over_ex->event_type) {
			delete over_ex;
		}
		else {
			cout << "Unknown Event Type :" << over_ex->event_type << endl;
			while (true);
		}
	}
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
	SOCKADDR_IN clientAddr;
	int addrLen = sizeof(SOCKADDR_IN);
	memset(&clientAddr, 0, addrLen);
	DWORD flags;

	g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
	vector <thread> worker_threads;
	for (int i = 0; i < 8; ++i) worker_threads.emplace_back(do_worker);

	while (true) {
		SOCKET clientSocket = accept(listenSocket, (struct sockaddr*) & clientAddr, &addrLen);
		if (INVALID_SOCKET == clientSocket) {
			int err_no = WSAGetLastError();
			if (WSA_IO_PENDING != err_no)
				error_display("Accept Error :", err_no);
		}
		int user_id = new_user_id++;
		SOCKETINFO* new_player = new SOCKETINFO;
		new_player->id = user_id;
		new_player->socket = clientSocket;
		new_player->prev_packet_size = 0;
		new_player->recv_over.wsabuf[0].len = MAX_BUFFER;
		new_player->recv_over.wsabuf[0].buf = new_player->recv_over.net_buf;
		new_player->recv_over.event_type = EV_RECV;
		new_player->x = rand() % WORLD_WIDTH;
		new_player->y = rand() % WORLD_HEIGHT;
		new_player->is_connected = false;
		clients.insert(make_pair(user_id, new_player));

		CreateIoCompletionPort(reinterpret_cast<HANDLE>(clientSocket), g_iocp, (ULONG_PTR)new_player, 0);

		memset(&new_player->recv_over.over, 0, sizeof(new_player->recv_over.over));
		flags = 0;
		int ret = WSARecv(clientSocket, new_player->recv_over.wsabuf, 1, NULL,
			&flags, &(new_player->recv_over.over), NULL);
		if (0 != ret) {
			int err_no = WSAGetLastError();
			if (WSA_IO_PENDING != err_no)
				error_display("WSARecv Error :", err_no);
		}
	}
	for (auto &th : worker_threads) th.join();
	closesocket(listenSocket);
	WSACleanup();
}


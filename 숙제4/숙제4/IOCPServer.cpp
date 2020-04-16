#include <iostream>
#include <map>
#include <thread>
#include <set>
#include <mutex>
#include <chrono>
#include <queue>
#include <concurrent_unordered_map.h>
#include <sdkddkver.h>
#include <boost/asio.hpp>

using namespace std;
using namespace std::chrono;
using namespace boost::asio;
using namespace boost::asio::ip;
using boost_error = boost::system::error_code;

#include "protocol.h"

#define MAX_BUFFER        1024
constexpr auto VIEW_RANGE = 7;

enum EVENT_TYPE { EV_RECV, EV_SEND, EV_MOVE, EV_PLAYER_MOVE_NOTIFY, EV_MOVE_TARGET, EV_ATTACK, EV_HEAL };

struct SOCKETINFO
{
	char	net_buf[MAX_BUFFER];
	char	pre_net_buf[MAX_BUFFER];
	int		prev_packet_size;
	tcp::socket	socket;
	int		id;
	char	name[MAX_STR_LEN];

	bool is_connected;
	bool is_active;
	short	x, y;
	int		seq_no;
	set <int> near_id;
	mutex near_lock;

	SOCKETINFO(int id, tcp::socket&& sock) :id{ id }, socket{ move(sock) } {}
};

Concurrency::concurrent_unordered_map <int, SOCKETINFO*> clients;
HANDLE	g_iocp;

int new_user_id = 0;

void handle_send(const boost_error& error, size_t length, SOCKETINFO* client);

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
	while (true);
	LocalFree(lpMsgBuf);
}

bool is_near(int a, int b)
{
	if (VIEW_RANGE < abs(clients[a]->x - clients[b]->x)) return false;
	if (VIEW_RANGE < abs(clients[a]->y - clients[b]->y)) return false;
	return true;
}

void Disconnect(int id);

void send_packet(int id, void* buff)
{
	auto client = clients[id];
	if (false == client->is_connected) return;

	char* data = (char*)buff;
	auto data_size = data[0];

	char* packet = new char[data_size];
	memcpy_s(packet, data_size, data, data_size);

	client->socket.async_send(buffer(packet, data_size), [client, packet](auto error, auto length) {
		delete packet;
		handle_send(error, length, client);
		});
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

void send_put_object_packet(int client_id, int new_id)
{
	auto client = clients[client_id];
	auto new_client = clients[new_id];
	sc_packet_put_object packet;
	packet.id = new_id;
	packet.size = sizeof(packet);
	packet.type = SC_PUT_OBJECT;
	packet.x = new_client->x;
	packet.y = new_client->y;
	packet.o_type = 1;
	send_packet(client_id, &packet);

	if (client_id == new_id) return;
	lock_guard<mutex>lg{ client->near_lock };
	client->near_id.insert(new_id);
}

void send_pos_packet(int client_id, int mover_id)
{
	auto client = clients[client_id];
	auto mover = clients[mover_id];
	sc_packet_pos packet;
	packet.id = mover_id;
	packet.size = sizeof(packet);
	packet.type = SC_POS;
	packet.x = mover->x;
	packet.y = mover->y;
	packet.seq_no = client->seq_no;

	client->near_lock.lock();
	if ((client_id == mover_id) || (0 != client->near_id.count(mover_id))) {
		client->near_lock.unlock();
		send_packet(client_id, &packet);
	}
	else {
		client->near_lock.unlock();
		send_put_object_packet(client_id, mover_id);
	}
}

void send_remove_object_packet(int client_id, int leaver)
{
	sc_packet_remove_object packet;
	packet.id = leaver;
	packet.size = sizeof(packet);
	packet.type = SC_REMOVE_OBJECT;
	send_packet(client_id, &packet);

	auto client = clients[client_id];
	lock_guard<mutex>lg{ client->near_lock };
	client->near_id.erase(leaver);
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
	auto client = clients[player];
	lock_guard <mutex> gl{ client->near_lock };
	return (0 != client->near_id.count(other));
}

void Disconnect(int id)
{
	auto client = clients[id];
	client->is_connected = false;
	client->socket.close();
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

void ProcessChat(int id, char* mess)
{

	clients[id]->near_lock.lock();
	auto vl = clients[id]->near_id;
	clients[id]->near_lock.unlock();

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
	strcpy_s(clients[user_id]->name, id_str);
	clients[user_id]->is_connected = true;
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

void handle_send(const boost_error& error, const size_t length, SOCKETINFO* client) {
	if (error) {
		cerr << "Error at send(client #" << client->id << "): " << error.message() << endl;
		Disconnect(client->id);
	}
	else if (length == 0) {
		Disconnect(client->id);
	}
}

void handle_recv(const boost_error& error, const size_t length, SOCKETINFO* client) {
	if (error) {
		cerr << "Error at recv(client #" << client->id << "): " << error.message() << endl;
		Disconnect(client->id);
	}
	else if (length == 0) {
		Disconnect(client->id);
	}
	else {
		char* p = client->net_buf;
		int remain = length;
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
				ProcessPacket(client->id, client->pre_net_buf);
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

		client->socket.async_read_some(buffer(client->net_buf, MAX_BUFFER), [client](auto error, auto length) {
			handle_recv(error, length, client);
			});
	}
}

void handle_accept(const boost_error& error, tcp::socket& sock, tcp::acceptor& acceptor) {
	if (error) {
		cerr << "Error at accept: " << error.message() << endl;
	}
	else {
		int user_id = new_user_id++;
		SOCKETINFO* new_player = new SOCKETINFO{ user_id, move(sock) };
		new_player->prev_packet_size = 0;
		new_player->x = rand() % WORLD_WIDTH;
		new_player->y = rand() % WORLD_HEIGHT;
		new_player->is_connected = false;
		clients.insert(make_pair(user_id, new_player));

		new_player->socket.async_read_some(buffer(new_player->net_buf, MAX_BUFFER), [new_player](auto error, auto length) {
			handle_recv(error, length, new_player);
			});
	}

	acceptor.async_accept([&acceptor](auto error, auto sock) {
		handle_accept(error, sock, acceptor);
		});
}

int main()
{
	try
	{
		vector <thread> worker_threads;
		io_context context;
		tcp::acceptor acceptor(context, tcp::endpoint(tcp::v4(), SERVER_PORT));
		acceptor.async_accept([&acceptor](boost_error error, tcp::socket sock) {handle_accept(error, sock, acceptor); });
		for (int i = 0; i < 8; ++i) worker_threads.emplace_back([&context]() {context.run(); });
		for (auto& th : worker_threads) th.join();
	}
	catch (const std::exception& e)
	{
		cerr << "Error at main" << e.what() << endl;
	}
}


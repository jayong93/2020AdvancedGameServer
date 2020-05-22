#include <iostream>
#include <map>
#include <thread>
#include <string>
#include <set>
#include <mutex>
#include <chrono>
#include <queue>
#include <boost/asio.hpp>
#include "protocol.h"
#include "util.h"

using namespace std;
using namespace std::chrono;
using namespace boost::asio;
using namespace boost::asio::ip;
using boost_error = boost::system::error_code;

constexpr auto VIEW_RANGE = 7;
constexpr unsigned MAX_USER_NUM = 20000;

struct SOCKETINFO
{
	char recv_buf[MAX_BUFFER];
	int prev_packet_size;
	tcp::socket socket;
	unsigned id;
	string name;

	bool is_connected;
	bool is_active;
	short x, y;
	int move_time;
	set<int> near_id;
	mutex near_lock;

	SOCKETINFO(unsigned id, tcp::socket &&sock) : id{id}, socket{move(sock)} {}
};

SOCKETINFO *clients[MAX_USER_NUM];
unsigned new_user_id = 0;

void handle_send(const boost_error &error, size_t length, SOCKETINFO *client);

pair<unsigned, unsigned> make_random_position(unsigned server_id)
{
	return pair(fast_rand() % WORLD_WIDTH, fast_rand() % WORLD_HEIGHT);
}

bool is_near(int a, int b)
{
	auto client_a = clients[a];
	auto client_b = clients[b];
	if (VIEW_RANGE < abs(client_a->x - client_b->x))
		return false;
	if (VIEW_RANGE < abs(client_a->y - client_b->y))
		return false;
	return true;
}

void Disconnect(int id);

void send_packet(int id, void *buff)
{
	auto client = clients[id];
	if (client == nullptr)
		return;
	if (false == client->is_connected)
		return;

	char *data = (char *)buff;
	auto data_size = data[0];

	char *packet = new char[data_size];
	memcpy(packet, data, data_size);

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

	if (client_id == new_id)
		return;
	lock_guard<mutex> lg{client->near_lock};
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
	packet.move_time = client->move_time;

	client->near_lock.lock();
	if ((client_id == mover_id) || (0 != client->near_id.count(mover_id)))
	{
		client->near_lock.unlock();
		send_packet(client_id, &packet);
	}
	else
	{
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
	lock_guard<mutex> lg{client->near_lock};
	client->near_id.erase(leaver);
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
	lock_guard<mutex> gl{client->near_lock};
	return (0 != client->near_id.count(other));
}

void Disconnect(int id)
{
	auto client = clients[id];
	client->is_connected = false;
	client->socket.close();
	for (auto i = 0; i < new_user_id; ++i)
	{
		auto cl = clients[i];
		if (true == cl->is_connected)
			send_remove_object_packet(cl->id, id);
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
	switch (dir)
	{
	case D_UP:
		if (y > 0)
			y--;
		break;
	case D_DOWN:
		if (y < WORLD_HEIGHT - 1)
			y++;
		break;
	case D_LEFT:
		if (x > 0)
			x--;
		break;
	case D_RIGHT:
		if (x < WORLD_WIDTH - 1)
			x++;
		break;
	case 99:
	{
		auto [new_x, new_y] = make_random_position(0);
		x = new_x;
		y = new_y;
	}
	break;
	default:
		cout << "Invalid Direction Error\n";
		while (true)
			;
	}

	client->x = x;
	client->y = y;

	set<int> new_vl;
	for (auto i = 0; i < new_user_id; ++i)
	{
		auto cl = clients[i];
		int other = cl->id;
		if (id == other)
			continue;
		if (false == clients[other]->is_connected)
			continue;
		if (true == is_near(id, other))
			new_vl.insert(other);
	}

	send_pos_packet(id, id);
	for (auto cl : old_vl)
	{
		if (0 != new_vl.count(cl))
		{
			send_pos_packet(cl, id);
		}
		else
		{
			send_remove_object_packet(id, cl);
			send_remove_object_packet(cl, id);
		}
	}
	for (auto cl : new_vl)
	{
		if (0 == old_vl.count(cl))
		{
			send_put_object_packet(id, cl);
			send_put_object_packet(cl, id);
		}
	}
}

void ProcessChat(int id, char *mess)
{
	auto client = clients[id];
	client->near_lock.lock();
	auto vl = client->near_id;
	client->near_lock.unlock();

	for (auto cl : vl)
		send_chat_packet(cl, id, mess);
}

void ProcessLogin(int user_id, char *id_str)
{
	//for (auto cl : clients) {
	//	if (0 == strcmp(cl.second->name, id_str)) {
	//		send_login_fail(user_id);
	//		Disconnect(user_id);
	//		return;
	//	}
	//}
	auto client = clients[user_id];
	client->name = id_str;
	client->is_connected = true;
	send_login_ok_packet(user_id);

	for (auto i = 0; i < new_user_id; ++i)
	{
		auto cl = clients[i];
		int other_player = cl->id;
		if (false == clients[other_player]->is_connected)
			continue;
		if (true == is_near(other_player, user_id))
		{
			send_put_object_packet(other_player, user_id);
			if (other_player != user_id)
			{
				send_put_object_packet(user_id, other_player);
			}
		}
	}
}

void ProcessPacket(int id, void *buff)
{
	char *packet = reinterpret_cast<char *>(buff);
	switch (packet[1])
	{
	case CS_LOGIN:
	{
		cs_packet_login *login_packet = reinterpret_cast<cs_packet_login *>(packet);
		ProcessLogin(id, login_packet->id);
	}
	break;
	case CS_MOVE:
	{
		cs_packet_move *move_packet = reinterpret_cast<cs_packet_move *>(packet);
		clients[id]->move_time = move_packet->move_time;
		ProcessMove(id, move_packet->direction);
	}
	break;
	case CS_ATTACK:
		break;
	case CS_CHAT:
	{
		cs_packet_chat *chat_packet = reinterpret_cast<cs_packet_chat *>(packet);
		ProcessChat(id, chat_packet->chat_str);
	}
	break;
	case CS_LOGOUT:
		break;
	case CS_TELEPORT:
		ProcessMove(id, 99);
		break;
	default:
		cout << "Invalid Packet Type Error\n";
		while (true)
			;
	}
}

void handle_send(const boost_error &error, const size_t length, SOCKETINFO *client)
{
	if (error)
	{
		cerr << "Error at send(client #" << client->id << "): " << error.message() << endl;
	}
	else if (length == 0)
	{
	}
}

void assemble_packet(SOCKETINFO *client, size_t received_bytes)
{
	char *p = client->recv_buf;
	auto remain = received_bytes;
	unsigned packet_size;
	unsigned prev_packet_size = client->prev_packet_size;
	if (0 == prev_packet_size)
		packet_size = 0;
	else
		packet_size = p[0];

	while (remain > 0)
	{
		if (0 == packet_size)
			packet_size = p[0];
		unsigned required = packet_size - prev_packet_size;
		if (required <= remain)
		{
			ProcessPacket(client->id, p);
			remain -= required;
			p += packet_size;
			prev_packet_size = 0;
			packet_size = 0;
		}
		else
		{
			memmove(client->recv_buf, p, remain);
			prev_packet_size += remain;
			break;
		}
	}
	client->prev_packet_size = prev_packet_size;
}

void handle_recv(const boost_error &error, const size_t length, SOCKETINFO *client)
{
	if (error)
	{
		cerr << "Error at recv(client #" << client->id << "): " << error.message() << endl;
		Disconnect(client->id);
	}
	else if (length == 0)
	{
		Disconnect(client->id);
	}
	else
	{
		assemble_packet(client, length);

		client->socket.async_read_some(buffer(client->recv_buf, MAX_BUFFER), [client](auto error, auto length) {
			handle_recv(error, length, client);
		});
	}
}

SOCKETINFO *create_new_player(unsigned id, tcp::socket &sock)
{
	SOCKETINFO *new_player = new SOCKETINFO{id, move(sock)};
	new_player->prev_packet_size = 0;
	auto [new_x, new_y] = make_random_position(0);
	new_player->x = new_x;
	new_player->y = new_y;
	new_player->is_connected = false;

	return new_player;
}

void handle_accept(const boost_error &error, tcp::socket &sock, tcp::acceptor &acceptor)
{
	if (error)
	{
		cerr << "Error at accept: " << error.message() << endl;
	}
	else
	{
		auto new_player = create_new_player(new_user_id, sock);
		clients[new_player->id] = new_player;
		new_user_id += 1;

		new_player->socket.async_read_some(buffer(new_player->recv_buf, MAX_BUFFER), [new_player](auto error, auto length) {
			handle_recv(error, length, new_player);
		});
	}

	acceptor.async_accept([&acceptor](auto error, auto sock) {
		handle_accept(error, sock, acceptor);
	});
}

int main(int argc, char *argv[])
{
	try
	{
		vector<thread> worker_threads;
		io_context context;
		tcp::acceptor acceptor(context, tcp::endpoint(tcp::v4(), SERVER_PORT));
		acceptor.async_accept([&acceptor](boost_error error, tcp::socket sock) { handle_accept(error, sock, acceptor); });
		for (int i = 0; i < 8; ++i)
			worker_threads.emplace_back([&context]() { context.run(); });
		cerr << "Server has started" << endl;
		for (auto &th : worker_threads)
			th.join();
	}
	catch (const std::exception &e)
	{
		cerr << "Error at main" << e.what() << endl;
	}
}

#include <iostream>
#include <vector>
#include <thread>
#include "protocol.h"
#include "server.h"
#include "util.h"

using namespace std;

constexpr unsigned MAX_USER_NUM = 20000;
constexpr unsigned INVALID_ID = -1;

static ClientSlot clients[MAX_USER_NUM];
static atomic_uint new_user_id{0};

pair<unsigned, unsigned> make_random_position(unsigned server_id)
{
    return pair(fast_rand() % WORLD_WIDTH, server_id * (WORLD_HEIGHT/2) + (fast_rand() % (WORLD_HEIGHT/2)));
}

void handle_send(const boost_error &error, const size_t length)
{
    if (error)
    {
        cerr << "Error at send: " << error.message() << endl;
    }
}

template <typename F>
void assemble_packet(char *recv_buf, size_t &prev_packet_size, size_t received_bytes, F &&packet_handler)
{
    char *p = recv_buf;
    auto remain = received_bytes;
    unsigned packet_size;
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
            packet_handler(p, packet_size);
            remain -= required;
            p += packet_size;
            prev_packet_size = 0;
            packet_size = 0;
        }
        else
        {
            memmove(recv_buf, p, remain);
            prev_packet_size += remain;
            break;
        }
    }
}

template <typename P, typename F>
void send_packet(int id, F &&packet_maker_func)
{
    auto &client_slot = clients[id];
    if (!client_slot)
        return;

    send_packet<P>(client_slot.ptr->socket, move(packet_maker_func));
}

template <typename P, typename F>
void send_packet(tcp::socket &client, F &&packet_maker_func)
{
    P *packet = new P;
    packet_maker_func(*packet);

    client.async_send(buffer(packet, sizeof(P)), [packet](auto error, auto length) {
        delete packet;
        handle_send(error, length);
    });
}

template <typename P, typename F>
void send_packet_to_server(tcp::socket &sock, unsigned target_id, F &&packet_maker_func)
{
    constexpr auto total_size = sizeof(P) + sizeof(char) + sizeof(unsigned);
    char *buf = new char[total_size];
    buf[0] = total_size;
    *(unsigned*)(buf + 1) = target_id;

    P* packet = (P*)(buf + 1 + sizeof(unsigned));
    packet_maker_func(*packet);

    sock.async_send(buffer(buf, total_size), [buf](auto error, auto length) {
        delete[] buf;
        if (error)
        {
            cerr << "Error at send to server : " << error.message() << endl;
        }
    });
}

void send_login_ok_packet(SOCKETINFO& client, unsigned id)
{
    auto maker = [&client, id](sc_packet_login_ok& packet) {
        packet.id = id;
        packet.size = sizeof(packet);
        packet.type = SC_LOGIN_OK;
        packet.x = client.x;
        packet.y = client.y;
        packet.hp = 100;
        packet.level = 1;
        packet.exp = 1;
    };
    if (!client.is_proxy)
        send_packet<sc_packet_login_ok>(client.socket, maker);
    else
        send_packet_to_server<sc_packet_login_ok>(client.other_server_socket, id, maker);
}

void send_login_fail(tcp::socket& sock)
{
    send_packet<sc_packet_login_fail>(sock, [](sc_packet_login_fail &packet) {
        packet.size = sizeof(packet);
        packet.type = SC_LOGIN_FAIL;
    });
}

void send_put_object_packet(SOCKETINFO& client, SOCKETINFO& new_client)
{
    auto maker = [&new_client](sc_packet_put_object &packet) {
        packet.id = new_client.id;
        packet.size = sizeof(packet);
        packet.type = SC_PUT_OBJECT;
        packet.x = new_client.x;
        packet.y = new_client.y;
        packet.o_type = 1;
    };
    if (!client.is_proxy) {
        send_packet<sc_packet_put_object>(client.socket, maker);
    }
    else {
        send_packet_to_server<sc_packet_put_object>(client.other_server_socket, client.id, maker);
    }
}

void send_pos_packet(SOCKETINFO& client, SOCKETINFO& mover)
{
    auto maker = [&mover, &client](sc_packet_pos& packet) {
			packet.id = mover.id;
			packet.size = sizeof(packet);
			packet.type = SC_POS;
			packet.x = mover.x;
			packet.y = mover.y;
			packet.move_time = client.move_time;
		};
    if (!client.is_proxy) {
		send_packet<sc_packet_pos>(client.socket, maker);
    }
    else
    {
        send_packet_to_server<sc_packet_pos>(client.other_server_socket, client.id, maker);
    }
}

void send_remove_object_packet(SOCKETINFO& client, SOCKETINFO& leaver)
{
    auto maker = [&leaver](sc_packet_remove_object &packet) {
        packet.id = leaver.id;
        packet.size = sizeof(packet);
        packet.type = SC_REMOVE_OBJECT;
    };

    if (!client.is_proxy) {
        send_packet<sc_packet_remove_object>(client.socket, maker);
    }
    else {
        send_packet_to_server<sc_packet_remove_object>(client.other_server_socket, client.id, maker);
    }
}

void send_chat_packet(SOCKETINFO& client, int teller, char *mess)
{
    auto maker = [teller, mess](sc_packet_chat &packet) {
        packet.id = teller;
        packet.size = sizeof(packet);
        packet.type = SC_CHAT;
        strcpy(packet.chat, mess);
    };
    if (!client.is_proxy)
        send_packet<sc_packet_chat>(client.socket, maker);
    else
        send_packet_to_server<sc_packet_chat>(client.other_server_socket, client.id, maker);
}

void Server::ProcessMove(int id, unsigned char dir, unsigned move_time)
{
    auto &client_slot = clients[id];
    if (!client_slot)
        return;
    auto &client = client_slot.ptr;

    if (move_time != 0)
        client->move_time = move_time;

    short x = client->x;
    short y = client->y;
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

    send_pos_packet(dummy_proxy, *client);
    for (auto i = 0; i < new_user_id.load(memory_order_relaxed); ++i)
    {
        clients[i].then([&client](auto& cl) {
            send_pos_packet(cl, *client);
        });
    }
}

void Server::ProcessChat(int id, char *mess)
{
    auto &client_slot = clients[id];
    if (!client_slot)
        return;
    auto &client = client_slot.ptr;

    for (auto i = 0; i < new_user_id.load(memory_order_relaxed); ++i)
    {
        clients[i].then([&client, id, mess](auto& other) {
			send_chat_packet(other, id, mess);
		});
    }
}

void Server::ProcessLogin(int user_id, char *id_str)
{
    //for (auto cl : clients) {
    //	if (0 == strcmp(cl.second->name, id_str)) {
    //		send_login_fail(user_id);
    //		Disconnect(user_id);
    //		return;
    //	}
    //}
    auto &client_slot = clients[user_id];
    auto &client = client_slot.ptr;

    client->name = id_str;
    client_slot.is_active.store(true, memory_order_release);
    send_login_ok_packet(*client, user_id);
    send_login_ok_packet(dummy_proxy, user_id);

    for (auto i = 0; i < new_user_id.load(memory_order_relaxed); ++i)
    {
        clients[i].then([&client](auto& other) {
			send_put_object_packet(other, *client);
			if (other.id != client->id)
			{
				send_put_object_packet(*client, other);
			}
		});
    }
}

SOCKETINFO *create_new_player(unsigned id, tcp::socket &&sock, tcp::socket &other_server_sock, short x, short y, bool is_proxy)
{
    SOCKETINFO *new_player = new SOCKETINFO{id, move(sock), other_server_sock};
    new_player->prev_packet_size = 0;
    new_player->x = x;
    new_player->y = y;
    new_player->is_proxy = is_proxy;

    return new_player;
}

SOCKETINFO *create_new_player(unsigned id, tcp::socket &&sock, tcp::socket& other_server_sock, bool is_proxy, unsigned server_id)
{
    auto [new_x, new_y] = make_random_position(server_id);
    return create_new_player(id, move(sock), other_server_sock, new_x, new_y, is_proxy);
}

void Server::handle_recv(const boost_error &error, const size_t length, SOCKETINFO *client)
{
    if (error)
    {
        cerr << "Error at recv(client #" << client->id << "): " << error.message() << endl;
        disconnect(client->id);
    }
    else if (length == 0)
    {
        disconnect(client->id);
    }
    else
    {
        assemble_packet((char *)client->recv_buf, client->prev_packet_size, length, [client, this](char *packet, auto len) { process_packet(client->id, packet); });

        client->socket.async_read_some(buffer(client->recv_buf, MAX_BUFFER), [client, this](auto error, auto length) {
            handle_recv(error, length, client);
        });
    }
}

Server::Server(unsigned server_id) : server_id{ server_id }, context{}, acceptor{ context, tcp::endpoint(tcp::v4(), SERVER_PORT + server_id) }, server_acceptor{ context, tcp::endpoint(tcp::v4(), SERVER_PORT + 10) }, other_server_sock{ context }, pending_client_sock{ context }, dummy_proxy{ INVALID_ID, tcp::socket{context}, other_server_sock }
{
    dummy_proxy.is_proxy = true;
}

void Server::run()
{
    vector<thread> worker_threads;
    acceptor.async_accept(pending_client_sock, [this](boost_error error) {
        if (error)
            cerr << "Error at accept: " << error.message() << endl;
        else
            acquire_new_id(new_user_id.load(memory_order_relaxed));
    });
    for (int i = 0; i < 8; ++i)
        worker_threads.emplace_back([this]() { context.run(); });
    cerr << "Server has started" << endl;

    if (server_id == 0)
    {
        server_acceptor.async_accept(other_server_sock, [this](boost_error error) {
            if (error) {
                cerr << "Can't accept other server(cause : " << error.message() << ")" << endl;
                return;
            }
            other_server_sock.async_read_some(buffer(server_recv_buf, MAX_BUFFER), [this](auto &error, auto length) {
                handle_recv_from_server(error, length);
            });
        });
    }
    else
    {
        auto server_addr = ip::make_address_v4("127.0.0.1");
        other_server_sock.async_connect(tcp::endpoint{server_addr, (unsigned short)(SERVER_PORT + 10)}, [this](auto &error) {
            if (error) {
                cerr << "Can't connect to other server(cause : " << error.message() << ")" << endl;
                return;
            }
            other_server_sock.async_read_some(buffer(server_recv_buf, MAX_BUFFER), [this](auto &error, auto length) {
                handle_recv_from_server(error, length);
            });
        });
    }

    for (auto &th : worker_threads)
        th.join();
}

void Server::handle_accept(tcp::socket &&sock, unsigned user_id)
{
    auto new_player = create_new_player(user_id, move(sock), other_server_sock, false, server_id);
    auto &slot = clients[new_player->id];
    slot.ptr.reset(new_player);
    slot.is_active.store(true, memory_order_release);

    new_player->socket.async_read_some(buffer(new_player->recv_buf, MAX_BUFFER), [new_player, this](auto error, auto length) {
        handle_recv(error, length, new_player);
    });

    acceptor.async_accept(pending_client_sock, [this](auto error) {
        if (error)
            cerr << "Error at accept: " << error.message() << endl;
        else
            acquire_new_id(new_user_id.load(memory_order_relaxed));
    });
}

void Server::disconnect(unsigned id)
{
    auto &client_slot = clients[id];
    if (!client_slot)
        return;
    client_slot.is_active.store(false, memory_order_relaxed);
    auto &client = client_slot.ptr;

    client->socket.close();
    for (auto i = 0; i < new_user_id.load(memory_order_relaxed); ++i)
    {
        clients[i].then([&client](auto& other) {
            send_remove_object_packet(other, *client);
		});
    }
}

void Server::handle_recv_from_server(const boost_error &error, const size_t length)
{
    if (error)
    {
        cerr << "Error at recv from other server" << error.message() << endl;
    }
    else if (length > 0)
    {
        assemble_packet(server_recv_buf, prev_packet_len, length, [this](char *packet, unsigned len) { process_packet_from_server(packet, len); });

        other_server_sock.async_read_some(buffer(server_recv_buf, MAX_BUFFER), [this](auto error, auto length) {
            handle_recv_from_server(error, length);
        });
    }
}

void Server::process_packet(int id, void *buff)
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
        ProcessMove(id, move_packet->direction, move_packet->move_time);
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
        ProcessMove(id, 99, 0);
        break;
    default:
        cout << "Invalid Packet Type Error\n";
        while (true)
            ;
    }
}

void Server::process_packet_from_server(char *buff, size_t length)
{
    char* raw_message = buff + 1 + sizeof(unsigned);
	switch (raw_message[1])
    {
    case SC_LOGIN_OK:
    {
        sc_packet_login_ok *login_packet = reinterpret_cast<sc_packet_login_ok *>(raw_message);
        auto new_player = create_new_player(login_packet->id, tcp::socket{context}, other_server_sock, login_packet->x, login_packet->y, true);
        auto &client_slot = clients[new_player->id];
        client_slot.ptr.reset(new_player);
        client_slot.is_active.store(true, memory_order_release);

        auto old_user_id = new_user_id.load(memory_order_relaxed);
        if (client_slot.ptr->id >= old_user_id)
        {
            new_user_id.store(client_slot.ptr->id + 1, memory_order_relaxed);
        }
    }
    break;
    case SC_LOGIN_FAIL:
        break;
    case SS_ID_ACQUIRE:
    {
        ss_packet_id_acquire *id_acq_packet = reinterpret_cast<ss_packet_id_acquire *>(raw_message);
        if (id_acq_packet->desire_id >= new_user_id)
        {
            new_user_id.store(id_acq_packet->desire_id + 1, memory_order_relaxed);
            send_packet_to_server<ss_packet_id_response_ok>(other_server_sock, INVALID_ID, [](ss_packet_id_response_ok &packet) {
                packet.size = sizeof(packet);
                packet.type = SS_ID_RESPONSE_OK;
            });
        }
        else
        {
            send_packet_to_server<ss_packet_id_response_fail>(other_server_sock, INVALID_ID, [](ss_packet_id_response_fail &packet) {
                packet.size = sizeof(packet);
                packet.type = SS_ID_RESPONSE_FAIL;
                packet.available_min_id = new_user_id.load(memory_order_relaxed);
            });
        }
    }
    break;
    case SS_ID_RESPONSE_OK:
    {
        handle_accept(move(pending_client_sock), new_user_id.fetch_add(1, memory_order_relaxed));
    }
    break;
    case SS_ID_RESPONSE_FAIL:
    {
        ss_packet_id_response_fail *id_acq_packet = reinterpret_cast<ss_packet_id_response_fail *>(raw_message);
        if (id_acq_packet->available_min_id > new_user_id.load(memory_order_relaxed))
            new_user_id.store(id_acq_packet->available_min_id, memory_order_relaxed);
        acquire_new_id(new_user_id.load(memory_order_relaxed));
    }
    break;
    default:
    {
        unsigned target_id = *(unsigned*)(buff + 1);
        if (target_id == INVALID_ID)
        {
			switch (raw_message[1]) {
			case SC_POS:
			{
				sc_packet_pos* p = (sc_packet_pos*)raw_message;
				clients[p->id].then([p](auto& cl) {
					cl.x = p->x;
					cl.y = p->y;
				});
			}
				break;
			case SC_PUT_OBJECT:
			{
				sc_packet_put_object* p = (sc_packet_put_object*)raw_message;
				clients[p->id].then([p](auto& cl) {
					cl.x = p->x;
					cl.y = p->y;
				});
			}
			break;
			case SC_REMOVE_OBJECT:
			{
				sc_packet_remove_object* p = (sc_packet_remove_object*)raw_message;
				if (clients[p->id].is_active.load(memory_order_acquire))
					clients[p->id].is_active.store(false, memory_order_release);
			}
			break;
			}
        }
        else {
			clients[target_id].then([raw_message](auto& cl) {
				tcp::socket& sock = cl.socket;
				const auto packet_size = raw_message[0];
				char* packet = new char[packet_size];
				memcpy(packet, raw_message, packet_size);
				sock.async_send(buffer(packet, packet_size), [packet](auto error, auto length) {
					delete[] packet;
					handle_send(error, length);
				});
			});
		}
        
    }
    }
}

void Server::acquire_new_id(unsigned new_id)
{
    send_packet_to_server<ss_packet_id_acquire>(other_server_sock, INVALID_ID, [new_id](ss_packet_id_acquire &packet) {
        packet.size = sizeof(packet);
        packet.type = SS_ID_ACQUIRE;
        packet.desire_id = new_id;
    });
}

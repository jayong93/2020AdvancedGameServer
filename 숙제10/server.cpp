#include <iostream>
#include <vector>
#include <thread>
#include "protocol.h"
#include "server.h"
#include "util.h"

using namespace std;

constexpr auto VIEW_RANGE = 7;
constexpr unsigned MAX_USER_NUM = 20000;

static ClientSlot clients[MAX_USER_NUM];
static atomic_uint new_user_id{0};
static atomic_uint total_player{0};

pair<unsigned, unsigned> make_random_position(unsigned server_id)
{
    return pair(fast_rand() % WORLD_WIDTH, fast_rand() % WORLD_HEIGHT);
}

void handle_send(const boost_error &error, const size_t length, ClientSlot &client_slot)
{
    if (error && client_slot)
    {
        cerr << "Error at send(client #" << client_slot.ptr->id << "): " << error.message() << endl;
    }
}

bool is_near(int a, int b)
{
    auto &client_a = clients[a];
    auto &client_b = clients[b];
    if (!client_a)
        return false;
    if (!client_b)
        return false;
    if (VIEW_RANGE < abs(client_a.ptr->x - client_b.ptr->x))
        return false;
    if (VIEW_RANGE < abs(client_a.ptr->y - client_b.ptr->y))
        return false;
    return true;
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

    P *packet = new P;
    packet_maker_func(*packet);

    client_slot.ptr->socket.async_send(buffer(packet, sizeof(P)), [&client_slot, packet](auto error, auto length) {
        delete packet;
        handle_send(error, length, client_slot);
    });
}

template <typename P, typename F>
void send_packet_to_server(tcp::socket &sock, F &&packet_maker_func)
{
    P *packet = new P;
    packet_maker_func(*packet);

    sock.async_send(buffer(packet, sizeof(P)), [packet](auto error, auto length) {
        delete packet;
        if (error)
        {
            cerr << "Error at send to server : " << error.message() << endl;
        }
    });
}

void send_login_ok_packet(int id)
{
    auto &client = clients[id];
    if (!client)
        return;
    send_packet<sc_packet_login_ok>(id, [&client, id](sc_packet_login_ok &packet) {
        packet.id = id;
        packet.size = sizeof(packet);
        packet.type = SC_LOGIN_OK;
        packet.x = client.ptr->x;
        packet.y = client.ptr->y;
        packet.hp = 100;
        packet.level = 1;
        packet.exp = 1;
    });
}

void send_login_fail(int id)
{
    send_packet<sc_packet_login_fail>(id, [](sc_packet_login_fail &packet) {
        packet.size = sizeof(packet);
        packet.type = SC_LOGIN_FAIL;
    });
}

void send_put_object_packet(int client_id, int new_id)
{
    auto &client = clients[client_id];
    auto &new_client = clients[new_id];
    if (!client || !new_client)
        return;
    send_packet<sc_packet_put_object>(client_id, [new_id, &new_client](sc_packet_put_object &packet) {
        packet.id = new_id;
        packet.size = sizeof(packet);
        packet.type = SC_PUT_OBJECT;
        packet.x = new_client.ptr->x;
        packet.y = new_client.ptr->y;
        packet.o_type = 1;
    });

    if (client_id == new_id)
        return;
    lock_guard<mutex> lg{client.ptr->near_lock};
    client.ptr->near_id.insert(new_id);
}

void send_pos_packet(int client_id, int mover_id)
{
    auto &client_slot = clients[client_id];
    auto &mover_slot = clients[mover_id];
    if (!client_slot || !mover_slot)
        return;
    auto client = client_slot.ptr.get();

    client->near_lock.lock();
    if ((client_id == mover_id) || (0 != client->near_id.count(mover_id)))
    {
        client->near_lock.unlock();
        auto mover = mover_slot.ptr.get();
        send_packet<sc_packet_pos>(client_id, [mover_id, mover, client](sc_packet_pos &packet) {
            packet.id = mover_id;
            packet.size = sizeof(packet);
            packet.type = SC_POS;
            packet.x = mover->x;
            packet.y = mover->y;
            packet.move_time = client->move_time;
        });
    }
    else
    {
        client->near_lock.unlock();
        send_put_object_packet(client_id, mover_id);
    }
}

void send_remove_object_packet(int client_id, int leaver)
{
    auto &client_slot = clients[client_id];
    if (!client_slot)
        return;

    sc_packet_remove_object packet;
    send_packet<sc_packet_remove_object>(client_id, [leaver](sc_packet_remove_object &packet) {
        packet.id = leaver;
        packet.size = sizeof(packet);
        packet.type = SC_REMOVE_OBJECT;
    });

    auto client = client_slot.ptr.get();
    lock_guard<mutex> lg{client->near_lock};
    client->near_id.erase(leaver);
}

void send_chat_packet(int client, int teller, char *mess)
{
    sc_packet_chat packet;
    send_packet<sc_packet_chat>(client, [teller](sc_packet_chat &packet) {
        packet.id = teller;
        packet.size = sizeof(packet);
        packet.type = SC_CHAT;
    });
}

bool is_near_id(int player, int other)
{
    auto &client_slot = clients[player];
    if (!client_slot)
        return;

    auto &client = client_slot.ptr;
    lock_guard<mutex> gl{client->near_lock};
    return (0 != client->near_id.count(other));
}
void ProcessMove(int id, unsigned char dir, unsigned move_time)
{
    auto &client_slot = clients[id];
    if (!client_slot)
        return;
    auto &client = client_slot.ptr;

    if (move_time != 0)
        client->move_time = move_time;

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
    for (auto i = 0; i < total_player.load(memory_order_relaxed); ++i)
    {
        auto &client_slot = clients[i];
        if (!client_slot)
            continue;
        auto &cl = client_slot.ptr;
        int other = cl->id;
        if (id == other)
            continue;
        if (!clients[other])
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
    auto &client_slot = clients[id];
    if (!client_slot)
        return;
    auto &client = client_slot.ptr;

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
    auto &client_slot = clients[user_id];
    auto &client = client_slot.ptr;

    client->name = id_str;
    client_slot.is_active.store(true, memory_order_release);
    send_login_ok_packet(user_id);

    for (auto i = 0; i < total_player.load(memory_order_relaxed); ++i)
    {
        auto &client_slot = clients[i];
        if (!client_slot)
            continue;
        auto &cl = client_slot.ptr;

        int other_player = cl->id;
        if (!clients[other_player])
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

SOCKETINFO *create_new_player(unsigned id, tcp::socket &sock, short x, short y, bool is_proxy)
{
    SOCKETINFO *new_player = new SOCKETINFO{id, move(sock)};
    new_player->prev_packet_size = 0;
    new_player->x = x;
    new_player->y = y;
    new_player->is_proxy = is_proxy;

    return new_player;
}

SOCKETINFO *create_new_player(unsigned id, tcp::socket &sock, bool is_proxy)
{
    auto [new_x, new_y] = make_random_position(0);
    return create_new_player(id, sock, new_x, new_y, is_proxy);
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

Server::Server(unsigned server_id) : server_id{server_id}, context{}, acceptor{context, tcp::endpoint(tcp::v4(), SERVER_PORT + server_id)}, server_acceptor{context, tcp::endpoint(tcp::v4(), SERVER_PORT + 10)}, other_server_sock{context}, pending_client_sock{context}
{
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
            other_server_sock.async_read_some(buffer(server_recv_buf, MAX_BUFFER), [this](auto &error, auto length) {
                handle_recv_from_server(error, length);
            });
        });
    }
    else
    {
        auto server_addr = ip::make_address_v4("127.0.0.1");
        other_server_sock.async_connect(tcp::endpoint{server_addr, SERVER_PORT + server_id}, [this](auto &error) {
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
    auto new_player = create_new_player(new_user_id.fetch_add(1, memory_order_relaxed), sock, false);
    clients[new_player->id] = new_player;
    total_player.fetch_add(1, memory_order_relaxed);

    new_player->socket.async_read_some(buffer(new_player->recv_buf, MAX_BUFFER), [new_player, this](auto error, auto length) {
        handle_recv(error, length, new_player);
    });

    acceptor.async_accept([this](auto error, auto sock) {
        handle_accept(error, sock, acceptor, server_id);
    });
}

void Server::disconnect(unsigned id)
{
    auto &client_slot = clients[id];
    if (!client_slot)
        return;
    client_slot.is_active.store(true, memory_order_relaxed);
    auto &client = client_slot.ptr;

    client->socket.close();
    for (auto i = 0; i < total_player.load(memory_order_relaxed); ++i)
    {
        auto &client_slot = clients[i];
        if (!client_slot)
            continue;
        auto &cl = client_slot.ptr;
        send_remove_object_packet(cl->id, id);
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
    switch (buff[1])
    {
    case SC_LOGIN_OK:
    {
        sc_packet_login_ok *login_packet = reinterpret_cast<sc_packet_login_ok *>(buff);
        auto new_player = create_new_player(login_packet->id, other_server_sock, login_packet->x, login_packet->y, true);
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
    case SS_ID_ACQUIRE:
    {
        ss_packet_id_acquire *id_acq_packet = reinterpret_cast<ss_packet_id_acquire *>(buff);
        if (id_acq_packet->desire_id >= new_user_id)
        {
            new_user_id.store(id_acq_packet->desire_id + 1, memory_order_relaxed);
            send_packet_to_server<ss_packet_id_response_ok>(other_server_sock, [](ss_packet_id_response_ok &packet) {
                packet.size = sizeof(packet);
                packet.type = SS_ID_RESPONSE_OK;
            });
        }
        else
        {
            send_packet_to_server<ss_packet_id_response_fail>(other_server_sock, [](ss_packet_id_response_fail &packet) {
                packet.size = sizeof(packet);
                packet.type = SS_ID_RESPONSE_FAIL;
                packet.available_max_id = new_user_id.load(memory_order_relaxed);
            });
        }
    }
    break;
    case SS_ID_RESPONSE_OK:
    {
        handle_accept(move(pending_client_sock), )
    }
    break;
    case SS_ID_RESPONSE_FAIL:
    {
        ss_packet_id_response_fail *id_acq_packet = reinterpret_cast<ss_packet_id_response_fail *>(buff);
        new_user_id.store(id_acq_packet->available_max_id, memory_order_relaxed);
        acquire_new_id(id_acq_packet->available_max_id);
    }
    break;
    }
}

void Server::acquire_new_id(unsigned new_id)
{
}

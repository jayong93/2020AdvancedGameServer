#include "server.h"
#include "protocol.h"
#include "util.h"
#include <iostream>
#include <thread>
#include <vector>

using namespace std;

constexpr unsigned MAX_USER_NUM = 20000;
constexpr unsigned INVALID_ID = -1;

static ClientSlot clients[MAX_USER_NUM];
static atomic_uint user_num{0};
static unsigned new_user_id{0};

pair<unsigned, unsigned> make_random_position(unsigned server_id) {
    return pair(fast_rand() % WORLD_WIDTH,
                server_id * (WORLD_HEIGHT / 2) +
                    (fast_rand() % (WORLD_HEIGHT / 2)));
}

void handle_send(const boost_error &error, const size_t length) {
    if (error) {
        cerr << "Error at send: " << error.message() << endl;
    }
}

template <typename F>
void assemble_packet(char *recv_buf, size_t &prev_packet_size,
                     size_t received_bytes, F &&packet_handler) {
    char *p = recv_buf;
    auto remain = received_bytes;
    unsigned packet_size;
    if (0 == prev_packet_size)
        packet_size = 0;
    else
        packet_size = p[0];

    while (remain > 0) {
        if (0 == packet_size)
            packet_size = p[0];
        unsigned required = packet_size - prev_packet_size;
        if (required <= remain) {
            packet_handler(p, packet_size);
            remain -= required;
            p += packet_size;
            prev_packet_size = 0;
            packet_size = 0;
        } else {
            memmove(recv_buf, p, remain);
            prev_packet_size += remain;
            break;
        }
    }
}

template <typename P, typename F>
void send_packet(int id, F &&packet_maker_func) {
    auto &client_slot = clients[id];
    if (!client_slot)
        return;

    send_packet<P>(client_slot.ptr->socket, move(packet_maker_func));
}

template <typename P, typename F>
void send_packet(tcp::socket &client, F &&packet_maker_func) {
    P *packet = new P;
    packet_maker_func(*packet);

    client.async_send(buffer(packet, sizeof(P)),
                      [packet](auto error, auto length) {
                          delete packet;
                          handle_send(error, length);
                      });
}

template <typename P, typename F>
void send_packet_to_server(tcp::socket &sock, F &&packet_maker_func) {
    P *buf = new P;

    packet_maker_func(*buf);

    sock.async_send(buffer(buf, sizeof(P)), [buf](auto error, auto length) {
        delete buf;
        if (error) {
            cerr << "Error at send to server : " << error.message() << endl;
        }
    });
}

void send_login_ok_packet(SOCKETINFO &client, unsigned id) {
    auto maker = [&client, id](sc_packet_login_ok &packet) {
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
}

void send_login_fail(tcp::socket &sock) {
    send_packet<sc_packet_login_fail>(sock, [](sc_packet_login_fail &packet) {
        packet.size = sizeof(packet);
        packet.type = SC_LOGIN_FAIL;
    });
}

void send_put_object_packet(SOCKETINFO &client, SOCKETINFO &new_client) {
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
}

void send_pos_packet(SOCKETINFO &client, SOCKETINFO &mover) {
    auto maker = [&mover, &client](sc_packet_pos &packet) {
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
}

void send_remove_object_packet(SOCKETINFO &client, SOCKETINFO &leaver) {
    auto maker = [&leaver](sc_packet_remove_object &packet) {
        packet.id = leaver.id;
        packet.size = sizeof(packet);
        packet.type = SC_REMOVE_OBJECT;
    };

    if (!client.is_proxy) {
        send_packet<sc_packet_remove_object>(client.socket, maker);
    }
}

void send_chat_packet(SOCKETINFO &client, int teller, char *mess) {
    auto maker = [teller, mess](sc_packet_chat &packet) {
        packet.id = teller;
        packet.size = sizeof(packet);
        packet.type = SC_CHAT;
        strcpy(packet.chat, mess);
    };
    if (!client.is_proxy)
        send_packet<sc_packet_chat>(client.socket, maker);
}

void Server::ProcessMove(int id, unsigned char dir, unsigned move_time) {
    auto &client_slot = clients[id];
    if (!client_slot)
        return;
    auto &client = client_slot.ptr;

    if (move_time != 0)
        client->move_time = move_time;

    short x = client->x;
    short y = client->y;
    switch (dir) {
    case D_UP:
        if (y > (WORLD_HEIGHT / 2) * server_id)
            y--;
        break;
    case D_DOWN:
        if (y < (WORLD_HEIGHT / 2) * (server_id + 1) - 1)
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
    case 99: {
        auto [new_x, new_y] = make_random_position(server_id);
        x = new_x;
        y = new_y;
    } break;
    default:
        cout << "Invalid Direction Error\n";
        while (true)
            ;
    }

    client->x = x;
    client->y = y;

    for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
        clients[i].then([&client](auto &cl) { send_pos_packet(cl, *client); });
    }
    send_packet_to_server<ss_packet_move>(other_server_send,
                                          [&client](ss_packet_move &p) {
                                              p.size = sizeof(ss_packet_move);
                                              p.type = SS_MOVE;
                                              p.id = client->id;
                                              p.x = client->x;
                                              p.y = client->y;
                                          });
}

void Server::ProcessChat(int id, char *mess) {
    auto &client_slot = clients[id];
    if (!client_slot)
        return;
    auto &client = client_slot.ptr;

    for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
        clients[i].then([&client, id, mess](auto &other) {
            send_chat_packet(other, id, mess);
        });
    }
}

void Server::ProcessLogin(int user_id, char *id_str) {
    auto &client_slot = clients[user_id];
    auto &client = client_slot.ptr;

    client->name = id_str;
    client_slot.is_active.store(true, memory_order_release);
    send_login_ok_packet(*client, user_id);

    send_packet_to_server<ss_packet_put>(this->other_server_send,
                                         [&client](ss_packet_put &p) {
                                             p.size = sizeof(ss_packet_put);
                                             p.type = SS_PUT;
                                             p.id = client->id;
                                             p.x = client->x;
                                             p.y = client->y;
                                         });

    for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
        clients[i].then([&client](auto &other) {
            send_put_object_packet(other, *client);
            if (other.id != client->id) {
                send_put_object_packet(*client, other);
            }
        });
    }
}

SOCKETINFO *create_new_player(unsigned id, tcp::socket &&sock, short x, short y,
                              bool is_proxy) {
    SOCKETINFO *new_player = new SOCKETINFO{id, move(sock)};
    new_player->prev_packet_size = 0;
    new_player->x = x;
    new_player->y = y;
    new_player->is_proxy = is_proxy;

    return new_player;
}

SOCKETINFO *create_new_player(unsigned id, tcp::socket &&sock,
                              tcp::socket &other_server_sock, bool is_proxy,
                              unsigned server_id) {
    auto [new_x, new_y] = make_random_position(server_id);
    return create_new_player(id, move(sock), new_x, new_y, is_proxy);
}

void Server::handle_recv(const boost_error &error, const size_t length,
                         SOCKETINFO *client) {
    if (error) {
        cerr << "Error at recv(client #" << client->id
             << "): " << error.message() << endl;
        disconnect(client->id);
    } else if (length == 0) {
        disconnect(client->id);
    } else {
        assemble_packet((char *)client->recv_buf, client->prev_packet_size,
                        length, [client, this](char *packet, auto len) {
                            process_packet(client->id, packet);
                        });

        client->socket.async_read_some(buffer(client->recv_buf, MAX_BUFFER),
                                       [client, this](auto error, auto length) {
                                           handle_recv(error, length, client);
                                       });
    }
}

void async_connect_to_other_server(tcp::socket &sock, unsigned short port) {
    auto server_ip = ip::make_address_v4("127.0.0.1");
    sock.async_connect(tcp::endpoint{server_ip, port}, [&sock,
                                                        port](auto &error) {
        if (error) {
            cerr << "Can't connect to other server(cause : " << error.message()
                 << ")" << endl;
            std::this_thread::sleep_for(1s);
            cerr << "Retry..." << endl;
            sock.close();
            async_connect_to_other_server(sock, port);
        }
    });
}

Server::Server(unsigned s_id)
    : server_id{s_id}, context{},
      acceptor{context, tcp::endpoint(tcp::v4(), SERVER_PORT + server_id)},
      server_acceptor{context,
                      tcp::endpoint(tcp::v4(), SERVER_PORT + 10 + server_id)},
      other_server_send{context}, other_server_recv{context},
      pending_client_sock{context} {}

void Server::run() {
    vector<thread> worker_threads;
    acceptor.async_accept(pending_client_sock, [this](boost_error error) {
        handle_accept(move(pending_client_sock),
                      new_user_id++ * 2 + this->server_id);
    });
    for (int i = 0; i < 8; ++i)
        worker_threads.emplace_back([this]() { context.run(); });
    cerr << "Server has started" << endl;

    server_acceptor.async_accept(other_server_recv, [this](boost_error error) {
        if (error) {
            cerr << "Can't accept other server(cause : " << error.message()
                 << ")" << endl;
            return;
        }
        other_server_recv.async_read_some(buffer(server_recv_buf, MAX_BUFFER),
                                          [this](auto &error, auto length) {
                                              handle_recv_from_server(error,
                                                                      length);
                                          });
    });

    unsigned short port = SERVER_PORT + 10 + (1 - server_id);
    async_connect_to_other_server(other_server_send, port);

    for (auto &th : worker_threads)
        th.join();
}

void Server::handle_accept(tcp::socket &&sock, unsigned user_id) {
    auto new_player = create_new_player(
        user_id, move(sock), this->other_server_send, false, server_id);
    auto &slot = clients[new_player->id];
    slot.ptr.reset(new_player);
    slot.is_active.store(true, memory_order_release);
    auto old_user_num = user_num.load(memory_order_relaxed);
    if (old_user_num <= user_id);
    user_num.fetch_add(user_id - old_user_num + 1, memory_order_relaxed);

    new_player->socket.async_read_some(
        buffer(new_player->recv_buf, MAX_BUFFER),
        [new_player, this](auto error, auto length) {
            handle_recv(error, length, new_player);
        });

    acceptor.async_accept(pending_client_sock, [this](auto error) {
        handle_accept(move(pending_client_sock), new_user_id++ * 2 + server_id);
    });
}

void Server::disconnect(unsigned id) {
    auto &client_slot = clients[id];
    if (!client_slot)
        return;
    client_slot.is_active.store(false, memory_order_relaxed);
    auto &client = client_slot.ptr;

    client->socket.close();
    for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
        clients[i].then([&client](auto &other) {
            send_remove_object_packet(other, *client);
        });
    }
    send_packet_to_server<ss_packet_leave>(other_server_send,
                                           [&client](ss_packet_leave &p) {
                                               p.size = sizeof(ss_packet_leave);
                                               p.type = SS_LEAVE;
                                               p.id = client->id;
                                           });
}

void Server::handle_recv_from_server(const boost_error &error,
                                     const size_t length) {
    if (error) {
        cerr << "Error at recv from other server: " << error.message() << endl;
    } else if (length > 0) {
        assemble_packet(server_recv_buf, prev_packet_len, length,
                        [this](char *packet, unsigned len) {
                            process_packet_from_server(packet, len);
                        });

        other_server_recv.async_read_some(buffer(server_recv_buf, MAX_BUFFER),
                                          [this](auto error, auto length) {
                                              handle_recv_from_server(error,
                                                                      length);
                                          });
    }
}

void Server::process_packet(int id, void *buff) {
    char *packet = reinterpret_cast<char *>(buff);
    switch (packet[1]) {
    case CS_LOGIN: {
        cs_packet_login *login_packet =
            reinterpret_cast<cs_packet_login *>(packet);
        ProcessLogin(id, login_packet->id);
    } break;
    case CS_MOVE: {
        cs_packet_move *move_packet =
            reinterpret_cast<cs_packet_move *>(packet);
        ProcessMove(id, move_packet->direction, move_packet->move_time);
    } break;
    case CS_ATTACK:
        break;
    case CS_CHAT: {
        cs_packet_chat *chat_packet =
            reinterpret_cast<cs_packet_chat *>(packet);
        ProcessChat(id, chat_packet->chat_str);
    } break;
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

void Server::process_packet_from_server(char *buff, size_t length) {
    switch (buff[1]) {
    case SS_PUT: {
        ss_packet_put *put_packet = reinterpret_cast<ss_packet_put *>(buff);
        auto &client_slot = clients[put_packet->id];
        client_slot.then_else(
            [put_packet](auto &cl) {
                cl.x = put_packet->x;
                cl.y = put_packet->y;
            },
            [put_packet, &client_slot, this]() {
                client_slot.ptr.reset(create_new_player(
                    put_packet->id, tcp::socket{this->context}, put_packet->x,
                    put_packet->y, true));
                client_slot.is_active.store(true, memory_order_release);
                auto old_user_num = user_num.load(memory_order_relaxed);
                if (old_user_num <= put_packet->id)
                user_num.fetch_add(put_packet->id - old_user_num + 1, memory_order_relaxed);
            });

        auto &new_client = client_slot.ptr;
        // Send sc_packet_put_object to clients
        for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
            auto &slot = clients[i];
            slot.then([put_packet, &new_client](auto &cl) {
                if (cl.is_proxy == false) {
                    send_put_object_packet(cl, *new_client);
                }
            });
        }
    } break;
    case SS_LEAVE: {
        ss_packet_leave *leave_packet = (ss_packet_leave *)buff;
        auto &client_slot = clients[leave_packet->id];
        client_slot.then([&client_slot](auto &old_client) {
            client_slot.is_active.store(false, memory_order_release);
            // Send sc_packet_remove_object to clients
            for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
                auto &slot = clients[i];
                slot.then([&old_client](auto &cl) {
                    if (cl.is_proxy == false) {
                        send_remove_object_packet(cl, old_client);
                    }
                });
            }
        });
    } break;
    case SS_MOVE: {
        ss_packet_move *move_packet = (ss_packet_move *)buff;
        auto &client_slot = clients[move_packet->id];
        client_slot.then([&client_slot, move_packet](auto &cl) {
            cl.x = move_packet->x;
            cl.y = move_packet->y;
            // Send sc_packet_remove_object to clients
            for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
                auto &slot = clients[i];
                slot.then([&moved_cl = cl](auto &cl) {
                    if (cl.is_proxy == false) {
                        send_pos_packet(cl, moved_cl);
                    }
                });
            }
        });
    } break;
    }
}

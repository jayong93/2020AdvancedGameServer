#include "server.h"
#include "protocol.h"
#include "util.h"
#include <iostream>
#include <thread>
#include <vector>

using namespace std;

constexpr unsigned MAX_USER_NUM = 20000;
constexpr unsigned INVALID_ID = -1;
constexpr unsigned VIEW_RANGE = 6;

static ClientSlot clients[MAX_USER_NUM];
static atomic_uint user_num{0};

enum MoveType { None, EnterToEdge, LeaveFromBuffer };

MoveType check_move_type(short old_y, short new_y, unsigned server_id) {
    short buffer_y;
    if (server_id == 0) {
        buffer_y = WORLD_HEIGHT / 2 - (VIEW_RANGE + 1);
        if (old_y <= buffer_y && buffer_y < new_y)
            return EnterToEdge;
        if (buffer_y <= old_y && new_y < buffer_y)
            return LeaveFromBuffer;
    } else {
        buffer_y = WORLD_HEIGHT / 2 + VIEW_RANGE;
        if (buffer_y <= old_y && new_y < buffer_y)
            return EnterToEdge;
        if (old_y <= buffer_y && buffer_y < new_y)
            return LeaveFromBuffer;
    }

    return None;
}

pair<unsigned, unsigned> make_random_position(unsigned server_id) {
    return pair(fast_rand() % WORLD_WIDTH,
                server_id * (WORLD_HEIGHT / 2) +
                    (fast_rand() % (WORLD_HEIGHT / 2)));
}

bool is_near(int x1, int y1, int x2, int y2) {
    if (abs(x1 - x2) > VIEW_RANGE)
        return false;
    if (abs(y1 - y2) > VIEW_RANGE)
        return false;
    return true;
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
            unsigned *id = (unsigned *)(p + sizeof(packet_header));
            packet_handler(*id, p, packet_size);
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

    send_packet<P>(*client_slot.ptr, move(packet_maker_func));
}

template <typename P, typename F>
void send_packet(SOCKETINFO &client, F &&packet_maker_func) {
    send_packet_all<P>(&client, 1, move(packet_maker_func));
}

// user_num은 32bit unsigned int, user는 id => unsigned int
template <typename P, typename F>
void send_packet_all(SOCKETINFO **users, unsigned user_num,
                     F &&packet_maker_func) {
    unsigned packet_offset =
        sizeof(packet_header) + sizeof(unsigned) * (1 + user_num);
    unsigned total_size = packet_offset + sizeof(P);
    char *packet = new char[total_size];

    packet_header *header = (packet_header *)packet;
    header->size = total_size;
    header->type = P::type_num;

    unsigned *user_data = (unsigned *)(packet + sizeof(packet_header));
    *user_data = user_num;
    for (auto i = 0; i < user_num; ++i) {
        *(user_data + i + 1) = users[i]->id;
    }

    packet_maker_func(*(P *)(packet + packet_offset));

    (*users)->sock.async_send(buffer(packet, total_size),
                              [packet](auto error, auto length) {
                                  delete[] packet;
                                  handle_send(error, length);
                              });
}

template <typename P, typename F>
void send_packet_to_server(tcp::socket &sock, F &&packet_maker_func) {
    unsigned total_size = sizeof(packet_header) + sizeof(P);
    char *buf = new char[total_size];

    packet_header *header = (packet_header *)packet;
    header->size = total_size;
    header->type = P::type_num;

    packet_maker_func(*(P *)(buf + sizeof(packet_header)));

    sock.async_send(buffer(buf, total_size), [buf](auto error, auto length) {
        delete[] buf;
        if (error) {
            cerr << "Error at send to server : " << error.message() << endl;
        }
    });
}

void send_login_ok_packet(SOCKETINFO &client, unsigned id) {
    auto maker = [&client, id](sc_packet_login_ok &packet) {
        packet.id = id;
        packet.x = client.x;
        packet.y = client.y;
        packet.hp = 100;
        packet.level = 1;
        packet.exp = 1;
    };
    if (!client.is_proxy)
        send_packet<sc_packet_login_ok>(client, maker);
}

void send_login_fail(SOCKETINFO &client) {
    send_packet<sc_packet_login_fail>(client,
                                      [](sc_packet_login_fail &packet) {});
}

void send_put_object_packet(SOCKETINFO &client, SOCKETINFO &new_client) {
    auto maker = [&new_client](sc_packet_put_object &packet) {
        packet.id = new_client.id;
        packet.x = new_client.x;
        packet.y = new_client.y;
        if (new_client.is_proxy) {
            packet.o_type = 2;
        } else {
            packet.o_type = 1;
        }
    };
    if (!client.is_proxy) {
        send_packet<sc_packet_put_object>(client, maker);
    }
}

void send_pos_packet(SOCKETINFO &client, SOCKETINFO &mover) {
    auto maker = [&mover, &client](sc_packet_pos &packet) {
        packet.id = mover.id;
        packet.x = mover.x;
        packet.y = mover.y;
        packet.move_time = client.move_time;
    };
    if (!client.is_proxy) {
        send_packet<sc_packet_pos>(client, maker);
    }
}

void send_remove_object_packet(SOCKETINFO &client, SOCKETINFO &leaver) {
    auto maker = [&leaver](sc_packet_remove_object &packet) {
        packet.id = leaver.id;
    };

    if (!client.is_proxy) {
        send_packet<sc_packet_remove_object>(client, maker);
    }
}

void send_chat_packet(SOCKETINFO &client, int teller, char *mess) {
    auto maker = [teller, mess](sc_packet_chat &packet) {
        packet.id = teller;
        strcpy(packet.chat, mess);
    };
    if (!client.is_proxy)
        send_packet<sc_packet_chat>(client, maker);
}
void Server::ProcessMove(SOCKETINFO &client, short new_x, short new_y,
                         unsigned move_time) {
    MoveType move_type = check_move_type(client.y, new_y, server_id);

    client.x = new_x;
    client.y = new_y;

    send_pos_packet(client, client);

    auto old_view_list = client.copy_view_list();

    set<unsigned> new_view_list;

    for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
        clients[i].then([&client, &new_view_list](auto &cl) {
            if (client.id != cl.id && is_near(cl.x, cl.y, client.x, client.y)) {
                new_view_list.emplace(cl.id);
            }
        });
    }

    vector<SOCKETINFO *> to_put, to_remove, to_pos;

    for (auto new_id : new_view_list) {
        clients[new_id].then([&client, &old_view_list, &to_put](auto &other) {
            auto it = old_view_list.find(other.id);
            if (it == old_view_list.end()) {
                other.insert_to_view(client.id);
                client.insert_to_view(other.id);
                send_put_object_packet(client, other);
                to_put.emplace_back(&other);
            }
        });
    }
    send_packet_all<sc_packet_put_object>(
        to_put.data(), to_put.size(), [&client](sc_packet_put_object &packet) {
            packet.id = client.id;
            packet.x = client.x;
            packet.y = client.y;
            if (client.is_proxy) {
                packet.o_type = 2;
            } else {
                packet.o_type = 1;
            }
        });

    for (auto old_id : old_view_list) {
        clients[old_id].then(
            [&client, &new_view_list, &to_remove, &to_pos](auto &other) {
                auto it = new_view_list.find(other.id);
                if (it == new_view_list.end()) {
                    other.erase_from_view(client.id);
                    client.erase_from_view(other.id);
                    send_remove_object_packet(client, other);
                    to_remove.emplace_back(&other);
                } else {
                    to_pos.emplace_back(&other);
                }
            });
    }
    send_packet_all<sc_packet_remove_object>(
        to_remove.data(), to_remove.size(),
        [&client](sc_packet_remove_object &packet) { packet.id = client.id; });
    send_packet_all<sc_packet_pos>(to_pos.data(), to_pos.size(),
                                   [&client](sc_packet_pos &packet) {
                                       packet.id = client.id;
                                       packet.move_time = client.move_time;
                                       packet.x = client.x;
                                       packet.y = client.y;
                                   });

    if (client.is_proxy)
        return;

    if (client.is_in_edge == false && move_type == EnterToEdge) {
        client.is_in_edge = true;
        send_packet_to_server<ss_packet_put>(this->other_server_send,
                                             [&client](ss_packet_put &p) {
                                                 p.id = client.id;
                                                 p.x = client.x;
                                                 p.y = client.y;
                                             });
    } else if (client.is_in_edge == true && move_type == LeaveFromBuffer) {
        client.is_in_edge = false;
        send_packet_to_server<ss_packet_leave>(
            other_server_send,
            [&client](ss_packet_leave &p) { p.id = client.id; });
    } else if (client.is_in_edge) {
        send_packet_to_server<ss_packet_move>(other_server_send,
                                              [&client](ss_packet_move &p) {
                                                  p.id = client.id;
                                                  p.x = client.x;
                                                  p.y = client.y;
                                              });
    }
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

    ProcessMove(*client, x, y, move_time);
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

    for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
        clients[i].then([&client](auto &other) {
            if (is_near(other.x, other.y, client->x, client->y) == false) {
                return;
            }
            if (other.is_proxy == false) {
                send_put_object_packet(other, *client);
            }
            if (other.id != client->id) {
                send_put_object_packet(*client, other);
            }
        });
    }

    if (client->is_in_edge)
        send_packet_to_server<ss_packet_put>(this->other_server_send,
                                             [&client](ss_packet_put &p) {
                                                 p.id = client->id;
                                                 p.x = client->x;
                                                 p.y = client->y;
                                             });
}

SOCKETINFO *create_new_player(tcp::socket &sock, unsigned id, short x, short y,
                              bool is_proxy, unsigned server_id) {
    SOCKETINFO *new_player = new SOCKETINFO{id, sock};
    new_player->x = x;
    new_player->y = y;
    new_player->is_proxy = is_proxy;

    bool is_in_edge = false;
    if (server_id == 0) {
        if (y > WORLD_HEIGHT / 2 - (VIEW_RANGE + 1)) {
            is_in_edge = true;
        }
    } else {
        if (y < WORLD_HEIGHT / 2 - VIEW_RANGE) {
            is_in_edge = true;
        }
    }
    new_player->is_in_edge = is_in_edge;

    return new_player;
}

SOCKETINFO *create_new_player(tcp::socket &sock, unsigned id, bool is_proxy,
                              unsigned server_id) {
    auto [new_x, new_y] = make_random_position(server_id);
    return create_new_player(sock, id, new_x, new_y, is_proxy, server_id);
}

void Server::handle_recv(const boost_error &error, const size_t length) {
    if (error) {
        cerr << "Error at recv : " << error.message() << endl;
        exit(-1);
    } else if (length == 0) {
        exit(0);
    } else {
        assemble_packet(this->recv_buf, this->prev_packet_len, length,
                        [this](unsigned id, char *packet, auto len) {
                            char *buf = new char[len];
                            memcpy(buf, packet, len);
                            clients[id].then([buf](SOCKETINFO &cl) {
                                cl.pending_packets.emplace(make_unique(buf));
                            })
                        });

        front_end_sock.async_read_some(
            buffer(this->recv_buf, MAX_BUFFER),
            [this](auto error, auto length) { handle_recv(error, length); });
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
        } else {
            cerr << "Connected to other server" << endl;
        }
    });
}

Server::Server(unsigned short port)
    : context{}, acceptor{context}, server_acceptor{context},
      other_server_send{context}, other_server_recv{context}, front_end_sock{
                                                                  context} {
    auto end_point = tcp::endpoint{tcp::v4(), port};
    acceptor.open(end_point.protocol());
    acceptor.bind(end_point);
    acceptor.listen();

    server_id = port - SERVER_PORT;

    auto other_end_point =
        tcp::endpoint{tcp::v4(), (unsigned short)(port + 10)};
    server_acceptor.open(other_end_point.protocol());
    server_acceptor.bind(other_end_point);
    server_acceptor.listen();
}

void Server::do_worker(unsigned worker_id) {
    SPSCQueue<unsigned> &queue = this->worker_queue[worker_id];
    while (true) {
        if (queue.is_empty())
            continue;

        auto user_id = *queue.deq();

        clients[user_id].then([this, user_id](SOCKETINFO &cl) {
            auto pending_len = cl.pending_packets.size();
            for (auto i = 0; i < pending_len; ++i) {
                auto packet = *cl.pending_packets.deq();
                this->process_packet_from_front_end(user_id, packet.get());
            }
        });

        clients[user_id].ptr->is_handling.store(false, memory_order_release);
    }
}

void Server::run() {
    vector<thread> worker_threads;
    acceptor.async_accept(front_end_sock, [this](boost_error error) {
        if (error) {
            cerr << "Can't accept front end" << endl;
        } else {
            front_end_sock.async_read_some(buffer(recv_buf, MAX_BUFFER),
                                           [this](auto &error, auto length) {
                                               handle_recv(error, length);
                                           });
        }
    });

    thread io_thread{[this]() { context.run(); }};
    thread master_thread{[this]() {
        unsigned next_worker_id = 0;
        while (true) {
            for (unsigned i = 0; i < user_num.load(memory_order_acquire); ++i) {
                clients[i].then([this, &next_worker_id, i](SOCKETINFO &cl) {
                    if (cl.pending_packets.is_empty())
                        return;

                    if (cl.is_handling.load(memory_order_acquire) == false) {
                        if (cl.is_handling.exchange(true) == true)
                            return;
                        this->worker_queue[next_worker_id].emplace(i);
                        next_worker_id = (next_worker_id + 1) % NUM_WORKER;
                    }
                });
            }
        }
    }};
    for (int i = 0; i < NUM_WORKER; ++i)
        worker_threads.emplace_back([this, i]() { do_worker(i); });
    cerr << "Server has started" << endl;

    server_acceptor.async_accept(other_server_recv, [this](boost_error error) {
        if (error) {
            cerr << "Can't accept other server(cause : " << error.message()
                 << ")" << endl;
            return;
        } else {
            cerr << "Other server has connected" << endl;
        }
        other_server_recv.async_read_some(buffer(other_recv_buf, MAX_BUFFER),
                                          [this](auto &error, auto length) {
                                              handle_recv_from_server(error,
                                                                      length);
                                          });
    });

    unsigned short port = SERVER_PORT + 10 + (1 - server_id);
    async_connect_to_other_server(other_server_send, port);

    io_thread.join();
    for (auto &th : worker_threads)
        th.join();
}

SOCKETINFO &Server::handle_accept(unsigned user_id) {
    auto new_player =
        create_new_player(this->front_end_sock, user_id, false, server_id);
    auto &slot = clients[new_player->id];
    slot.ptr.reset(new_player);
    slot.is_active.store(true, memory_order_release);
    auto old_user_num = user_num.load(memory_order_relaxed);
    if (old_user_num <= user_id)
        user_num.fetch_add(user_id - old_user_num + 1, memory_order_relaxed);

    return *new_player;
}

void Server::disconnect(unsigned id) {
    auto &client_slot = clients[id];
    if (!client_slot)
        return;
    client_slot.is_active.store(false);
    auto &client = client_slot.ptr;

    vector<SOCKETINFO *> near_clients;
    for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
        clients[i].then([&client, &near_clients](auto &other) {
            if (is_near(other.x, other.y, client->x, client->y))
                near_clients.emplace_back(&other);
        });
    }

    send_packet_all<sc_packet_remove_object>(
        near_clients.data(), near_clients.size(),
        [&client](sc_packet_remove_object &packet) { packet.id = client->id; });

    if (client->is_in_edge)
        send_packet_to_server<ss_packet_leave>(
            other_server_send,
            [&client](ss_packet_leave &p) { p.id = client->id; });
}

void Server::handle_recv_from_server(const boost_error &error,
                                     const size_t length) {
    if (error) {
        cerr << "Error at recv from other server: " << error.message() << endl;
    } else if (length > 0) {
        assemble_packet(other_recv_buf, other_prev_len, length,
                        [this](auto _, char *packet, unsigned len) {
                            process_packet_from_server(packet, len);
                        });

        other_server_recv.async_read_some(buffer(other_recv_buf, MAX_BUFFER),
                                          [this](auto error, auto length) {
                                              handle_recv_from_server(error,
                                                                      length);
                                          });
    }
}

void Server::process_packet_from_front_end(unsigned id, char *packet) {
    unsigned id = *(unsigned *)(packet + sizeof(packet_header));
    switch (packet[1]) {
    case fs_packet_try_login::type_num: {
        auto &new_player = handle_accept(id);
        send_packet<sf_packet_accept_login>(new_player, [](auto &_) {});
    } break;
    case fs_packet_logout::type_num: {
        disconnect(id);
    } break;
    case fs_packet_forwarding::type_num: {
        process_packet(id, (packet + sizeof(packet_header) + sizeof(unsigned)));
    } break;
    }
}

void Server::process_packet(int id, void *buff) {
    char *packet = reinterpret_cast<char *>(buff);
    switch (packet[1]) {
    case cs_packet_login::type_num: {
        cs_packet_login *login_packet =
            reinterpret_cast<cs_packet_login *>(packet);
        ProcessLogin(id, login_packet->id);
    } break;
    case cs_packet_move::type_num: {
        cs_packet_move *move_packet =
            reinterpret_cast<cs_packet_move *>(packet);
        ProcessMove(id, move_packet->direction, move_packet->move_time);
    } break;
    case cs_packet_attack::type_num:
        break;
    case cs_packet_chat::type_num: {
        cs_packet_chat *chat_packet =
            reinterpret_cast<cs_packet_chat *>(packet);
        ProcessChat(id, chat_packet->chat_str);
    } break;
    case cs_packet_logout::type_num:
        break;
    case cs_packet_teleport::type_num:
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
    case ss_packet_put::type_num: {
        ss_packet_put *put_packet = reinterpret_cast<ss_packet_put *>(buff);
        auto &client_slot = clients[put_packet->id];
        client_slot.then_else(
            [put_packet](auto &cl) {
                cl.x = put_packet->x;
                cl.y = put_packet->y;
            },
            [put_packet, &client_slot, this]() {
                client_slot.ptr.reset(create_new_player(
                    this->front_end_sock, put_packet->id, put_packet->x,
                    put_packet->y, true, 1 - server_id));
                client_slot.is_active.store(true, memory_order_release);
                auto old_user_num = user_num.load(memory_order_relaxed);
                if (old_user_num <= put_packet->id)
                    user_num.fetch_add(put_packet->id - old_user_num + 1,
                                       memory_order_relaxed);
            });

        auto &new_client = client_slot.ptr;

        for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
            auto &slot = clients[i];
            slot.then([put_packet, &new_client](auto &cl) {
                if (is_near(cl.x, cl.y, new_client->x, new_client->y)) {
                    send_put_object_packet(cl, *new_client);
                }
            });
        }
    } break;
    case ss_packet_leave::type_num: {
        ss_packet_leave *leave_packet = (ss_packet_leave *)buff;
        auto &client_slot = clients[leave_packet->id];
        client_slot.then([&client_slot](auto &old_client) {
            client_slot.is_active.store(false);
            for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
                auto &slot = clients[i];
                slot.then([&old_client](auto &cl) {
                    if (is_near(cl.x, cl.y, old_client.x, old_client.y))
                        send_remove_object_packet(cl, old_client);
                });
            }
        });
    } break;
    case ss_packet_move::type_num: {
        ss_packet_move *move_packet = (ss_packet_move *)buff;
        auto &client_slot = clients[move_packet->id];
        client_slot.then([&client_slot, move_packet, this](auto &cl) {
            ProcessMove(cl, move_packet->x, move_packet->y, 0);
        });
    } break;
    }
}

#include "server.h"
#include "protocol.h"
#include "util.h"
#include <iostream>
#include <thread>
#include <vector>

using namespace std;

constexpr unsigned MAX_USER_NUM = 20000;
constexpr unsigned INVALID_ID = -1;
constexpr unsigned VIEW_RANGE = 7;
constexpr unsigned EDGE_RANGE = 4;
constexpr unsigned BUFFER_RANGE = 2;

static ClientSlot clients[MAX_USER_NUM];
static atomic_uint user_num{0};

enum MoveType { None, EnterToEdge, LeaveFromBuffer, HandOver };

MoveType check_move_type(short old_y, short new_y, unsigned server_id) {
    short buffer_y, other_buffer_y;
    if (server_id == 0) {
        buffer_y = WORLD_HEIGHT / 2 - (EDGE_RANGE + BUFFER_RANGE);
        other_buffer_y = WORLD_HEIGHT / 2 + (EDGE_RANGE + BUFFER_RANGE - 1);
        if (old_y < (buffer_y + BUFFER_RANGE) &&
            (buffer_y + BUFFER_RANGE) <= new_y)
            return EnterToEdge;
        if (buffer_y <= old_y && new_y < buffer_y)
            return LeaveFromBuffer;
        if (old_y <= other_buffer_y && other_buffer_y < new_y)
            return HandOver;
    } else {
        buffer_y = WORLD_HEIGHT / 2 + (EDGE_RANGE + BUFFER_RANGE - 1);
        other_buffer_y = WORLD_HEIGHT / 2 - (EDGE_RANGE + BUFFER_RANGE);
        if ((buffer_y - BUFFER_RANGE) < old_y &&
            new_y <= (buffer_y - BUFFER_RANGE))
            return EnterToEdge;
        if (old_y <= buffer_y && buffer_y < new_y)
            return LeaveFromBuffer;
        if (other_buffer_y <= old_y && new_y < other_buffer_y)
            return HandOver;
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
void assemble_packet(unsigned char *recv_buf, size_t &prev_packet_size,
                     size_t received_bytes, F &&packet_handler) {
    unsigned char *p = (unsigned char *)recv_buf;
    auto remain = received_bytes;
    unsigned packet_size;
    if (0 == prev_packet_size)
        packet_size = 0;
    else
        packet_size = p[0];

    while (remain > 0) {
        if (0 == packet_size)
            packet_size = (unsigned char)(p[0]);
        unsigned required = packet_size - prev_packet_size;
        if (required <= remain) {
            unsigned *id = (unsigned *)(p + sizeof(packet_header));
            packet_handler(*id, p, packet_size);
            remain -= required;
            p += packet_size;
            prev_packet_size = 0;
            packet_size = 0;
        } else {
            memmove(recv_buf + prev_packet_size, p + prev_packet_size, remain);
            prev_packet_size += remain;
            break;
        }
    }
}

template <typename P, typename F>
unique_ptr<unsigned char[]> make_message(unsigned id, F &&func) {
    unsigned total_size = sizeof(packet_header) + sizeof(unsigned) + sizeof(P);
    unique_ptr<unsigned char[]> packet{new unsigned char[total_size]};

    packet_header *header = (packet_header *)packet.get();
    header->size = total_size;
    header->type = P::type_num;

    unsigned *id_ptr = (unsigned *)(header + 1);
    *id_ptr = id;

    func(*(P *)(id_ptr + 1));
    return packet;
}

template <typename P, typename F>
pair<unsigned char *, size_t> make_packet(SOCKETINFO &user, F &&func) {

    if (user_num <= 0)
        return make_pair(nullptr, 0);

    unsigned packet_offset =
        sizeof(packet_header) + sizeof(unsigned);
    unsigned total_size = packet_offset + sizeof(P);

    unsigned char *packet = new unsigned char[total_size];
    packet_header *header = (packet_header *)packet;
    header->size = total_size;
    header->type = P::type_num;

    unsigned *user_id = (unsigned *)(packet + sizeof(packet_header));
    *user_id = user.id;

    func(*(P *)(packet + packet_offset));
    return make_pair(packet, total_size);
}

template <typename P, typename F>
void send_packet(SOCKETINFO &client, F &&packet_maker_func) {
    auto [packet, total_size] =
        make_packet<P>(client, move(packet_maker_func));

    if (packet == nullptr)
        return;

    client.sock.async_send(buffer(packet, total_size),
                              [packet](auto error, auto length) {
                                  delete[] packet;
                                  handle_send(error, length);
                              });
}

template <typename P, typename F>
void send_packet(int id, F &&packet_maker_func) {
    auto &client_slot = clients[id];
    if (!client_slot)
        return;

    send_packet<P>(*client_slot.ptr, move(packet_maker_func));
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
bool Server::ProcessMove(SOCKETINFO &client, short new_x, short new_y,
                         unsigned move_time) {
    MoveType move_type = check_move_type(client.y, new_y, server_id);

    client.x = new_x;
    client.y = new_y;

    send_pos_packet(client, client);

    auto old_view_list = client.copy_view_list();

    set<unsigned> new_view_list;

    for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
        clients[i].then([&client, &new_view_list](auto &cl) {
            if (client.id != cl.id && is_near(cl.x, cl.y, client.x, client.y) &&
                cl.is_logged_in) {
                new_view_list.emplace(cl.id);
            }
        });
    }

    for (auto new_id : new_view_list) {
        clients[new_id].then([&client, &old_view_list](auto &other) {
            auto it = old_view_list.find(other.id);
            if (it == old_view_list.end()) {
                other.insert_to_view(client.id);
                client.insert_to_view(other.id);
                send_put_object_packet(client, other);
                send_put_object_packet(other, client);
            }
        });
    }

    for (auto old_id : old_view_list) {
        clients[old_id].then([&client, &new_view_list](auto &other) {
            auto it = new_view_list.find(other.id);
            if (it == new_view_list.end()) {
                other.erase_from_view(client.id);
                client.erase_from_view(other.id);
                send_remove_object_packet(client, other);
                send_remove_object_packet(other, client);
            } else {
                send_pos_packet(other, client);
            }
        });
    }

    if (client.is_proxy)
        return false;

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

    if (move_type == HandOver) {
        client.is_proxy = true;
        client.is_in_edge = false;
        return true;
    }

    return false;
}

bool Server::ProcessMove(int id, unsigned char dir, unsigned move_time) {
    auto &client_slot = clients[id];
    if (!client_slot)
        return false;
    auto &client = client_slot.ptr;

    if (move_time != 0)
        client->move_time = move_time;

    short x = client->x;
    short y = client->y;
    switch (dir) {
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

    return ProcessMove(*client, x, y, move_time);
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
    send_login_ok_packet(*client, user_id);

    client_slot.ptr->is_logged_in = true;
    for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
        clients[i].then([&client](auto &other) {
            if (is_near(other.x, other.y, client->x, client->y) == false ||
                other.is_logged_in != true) {
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
    bool is_in_edge = false;
    if (server_id == 0) {
        if (y >= WORLD_HEIGHT / 2 - EDGE_RANGE) {
            is_in_edge = true;
        }
    } else {
        if (y < WORLD_HEIGHT / 2 + EDGE_RANGE) {
            is_in_edge = true;
        }
    }

    SOCKETINFO *new_player =
        new SOCKETINFO{id, sock, is_proxy, x, y, is_in_edge};

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
        assemble_packet(
            this->recv_buf, this->prev_packet_len, length,
            [this](unsigned id, unsigned char *packet, auto len) {
                if (packet[1] == fs_packet_forwarding::type_num) {
                    unsigned char *real_packet =
                        packet + sizeof(packet_header) + sizeof(unsigned) +
                        sizeof(fs_packet_forwarding);
                    if (real_packet[1] == cs_packet_login::type_num) {
                        handle_accept(id);
                    }
                }

                unique_ptr<unsigned char[]> buf{new unsigned char[len]};
                memcpy(buf.get(), packet, len);
                clients[id].then([&buf](SOCKETINFO &cl) {
                    switch (cl.status.load(memory_order_acquire)) {
                    case Normal:
                    case HandOvering:
                        cl.pending_packets.emplace(move(buf));
                        break;
                    case HandOvered:
                        cl.pending_while_hand_over_packets.emplace(move(buf));
                        break;
                    }
                });
            });

        front_end_sock.async_read_some(
            buffer(this->recv_buf + prev_packet_len,
                   MAX_BUFFER - prev_packet_len),
            [this](auto error, auto length) { handle_recv(error, length); });
    }
}

void async_connect_to_other_server(tcp::socket &sock, address_v4 ip,
                                   unsigned short port) {
    sock.async_connect(tcp::endpoint{ip, port}, [&sock, ip, port](auto &error) {
        if (error) {
            cerr << "Can't connect to other server(cause : " << error.message()
                 << ")" << endl;
            std::this_thread::sleep_for(1s);
            cerr << "Retry..." << endl;
            sock.close();
            async_connect_to_other_server(sock, ip, port);
        } else {
            cerr << "Connected to other server" << endl;
        }
    });
}

Server::Server(unsigned id, unsigned short accept_port,
               unsigned short other_server_accept_port)
    : context{}, acceptor{context}, server_acceptor{context},
      other_server_send{context}, other_server_recv{context}, front_end_sock{
                                                                  context} {
    tcp::acceptor::reuse_address option{true};

    auto end_point = tcp::endpoint{tcp::v4(), accept_port};
    acceptor.open(end_point.protocol());
    acceptor.set_option(option);
    acceptor.bind(end_point);
    acceptor.listen();

    server_id = id;

    auto other_end_point = tcp::endpoint{tcp::v4(), other_server_accept_port};
    server_acceptor.open(other_end_point.protocol());
    server_acceptor.set_option(option);
    server_acceptor.bind(other_end_point);
    server_acceptor.listen();
}

void Server::do_worker(unsigned worker_id) {
    SPSCQueue<unsigned> &queue = this->worker_queue[worker_id];
    while (true) {
        if (queue.is_empty()) {
            this_thread::yield();
            continue;
        }

        auto user_id = *queue.deq();

        clients[user_id].then([this, user_id](SOCKETINFO &cl) {
            if (cl.status.load(memory_order_acquire) == Normal) {
                cl.pending_while_hand_over_packets.for_each(
                    [this, user_id](unique_ptr<unsigned char[]> packet) {
                        this->process_packet_from_front_end(user_id,
                                                            move(packet));
                    });
            }
            bool is_hand_overed = false;
            auto pending_len = cl.pending_packets.size();
            for (auto i = 0; i < pending_len; ++i) {
                auto packet = *cl.pending_packets.deq();
                is_hand_overed =
                    this->process_packet_from_front_end(user_id, move(packet));
                if (is_hand_overed) {
                    cl.status.store(HandOvering, memory_order_release);
                    send_packet<sf_packet_hand_over>(
                        cl, [](sf_packet_hand_over &packet) {});
                    is_hand_overed = false;
                }
            }
        });

        clients[user_id].ptr->is_handling.store(false, memory_order_release);
    }
}

void Server::run(const string &other_server_ip,
                 unsigned short other_server_port) {
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
                    if (cl.is_handling.load(memory_order_acquire) == true)
                        return;

                    if (cl.pending_packets.is_empty() &&
                        cl.pending_while_hand_over_packets.is_empty())
                        return;

                    if (cl.is_handling.exchange(true) == true)
                        return;
                    this->worker_queue[next_worker_id].emplace(i);
                    next_worker_id = (next_worker_id + 1) % NUM_WORKER;
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

    auto ip = make_address_v4(other_server_ip);
    async_connect_to_other_server(other_server_send, ip, other_server_port);

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
    client_slot.ptr->is_logged_in = false;
    client_slot.is_active.store(false);
    auto &client = client_slot.ptr;

    for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
        clients[i].then([&client](auto &other) {
            if (is_near(other.x, other.y, client->x, client->y) &&
                other.is_logged_in)
                send_remove_object_packet(other, *client);
        });
    }

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
                        [this](auto _, unsigned char *packet, unsigned len) {
                            process_packet_from_server(packet, len);
                        });

        other_server_recv.async_read_some(
            buffer(other_recv_buf + other_prev_len,
                   MAX_BUFFER - other_prev_len),
            [this](auto error, auto length) {
                handle_recv_from_server(error, length);
            });
    }
}

bool Server::process_packet_from_front_end(
    unsigned id, unique_ptr<unsigned char[]> &&packet) {
    switch (packet[1]) {
    case fs_packet_logout::type_num: {
        clients[id].then([this, &packet](SOCKETINFO &cl) {
            if (cl.status.load(memory_order_acquire) != Normal) {
                cl.pending_while_hand_over_packets.emplace(move(packet));
            } else {
                disconnect(cl.id);
            }
        });
    } break;
    case fs_packet_hand_overed::type_num: {
        clients[id].then([this, id](SOCKETINFO &cl) {
            auto status = cl.status.load(memory_order_acquire);
            if (status == Normal) {
                cl.is_proxy = false;
                cl.is_in_edge = false;
                cl.status.store(HandOvered);
                send_packet_to_server<ss_packet_hand_over_started>(
                    this->other_server_send,
                    [id](ss_packet_hand_over_started &packet) {
                        packet.id = id;
                    });
            } else {
                cerr << "Something goes wrong during handover" << endl;
            }
        });
    } break;
    case fs_packet_forwarding::type_num: {
        bool result = false;
        clients[id].then([&result, this, id, &packet](SOCKETINFO &cl) {
            if (cl.status.load(memory_order_acquire) != Normal) {
                cl.pending_while_hand_over_packets.emplace(move(packet));
            } else
                result =
                    process_packet(id, (packet.get() + sizeof(packet_header) +
                                        sizeof(unsigned)) +
                                           sizeof(fs_packet_forwarding));
        });
        return result;
    } break;
    case message_hand_over_started::type_num: {
        clients[id].then([this, id](SOCKETINFO &cl) {
            auto status = cl.status.load(memory_order_acquire);
            if (status == HandOvering) {
                cl.end_hand_over(this->other_server_send);
            } else {
                cerr << "Something goes wrong during handover" << endl;
            }
        });
    } break;
    case message_hand_over_ended::type_num: {
        clients[id].then([this, id](SOCKETINFO &cl) {
            auto status = cl.status.load(memory_order_acquire);
            if (status == HandOvered) {
                for (auto i = 0; i < user_num.load(memory_order_acquire); ++i) {
                    if (i == id)
                        continue;
                    clients[i].then([&cl](SOCKETINFO &other) {
                        if (is_near(cl.x, cl.y, other.x, other.y)) {
                            send_put_object_packet(cl, other);
                        }
                    });
                }
                cl.status.store(Normal);
            } else {
                cerr << "Something goes wrong during handover" << endl;
            }
        });
    } break;
    case message_proxy_in::type_num: {
        clients[id].then([this, id](SOCKETINFO &new_client) {
            new_client.is_logged_in = true;
            new_client.is_in_edge = true;
            for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
                auto &slot = clients[i];
                slot.then([&new_client](auto &cl) {
                    if (is_near(cl.x, cl.y, new_client.x, new_client.y) &&
                        cl.is_logged_in) {
                        send_put_object_packet(cl, new_client);
                    }
                });
            }
        });
    } break;
    case message_proxy_move::type_num: {
        message_proxy_move *move_packet =
            (message_proxy_move *)(packet.get() + sizeof(packet_header) +
                                   sizeof(unsigned));
        clients[id].then([this, move_packet](SOCKETINFO &cl) {
            ProcessMove(cl, move_packet->x, move_packet->y, 0);
        });
    } break;
    case message_proxy_leave::type_num: {
        auto &client_slot = clients[id];
        client_slot.then([this, &client_slot](SOCKETINFO &old_client) {
            old_client.is_logged_in = false;
            old_client.is_in_edge = false;
            client_slot.is_active.store(false);
            for (auto i = 0; i < user_num.load(memory_order_relaxed); ++i) {
                auto &slot = clients[i];
                slot.then([&old_client](auto &cl) {
                    if (is_near(cl.x, cl.y, old_client.x, old_client.y) &&
                        cl.is_logged_in)
                        send_remove_object_packet(cl, old_client);
                });
            }
        });
    } break;
    default:
        cerr << "Unknown type has been received" << endl;
    }
    return false;
}

bool Server::process_packet(int id, void *buff) {
    packet_header *header = (packet_header *)buff;
    unsigned char *packet =
        reinterpret_cast<unsigned char *>(buff) + sizeof(packet_header);
    switch (header->type) {
    case cs_packet_login::type_num: {
        cs_packet_login *login_packet =
            reinterpret_cast<cs_packet_login *>(packet);
        ProcessLogin(id, login_packet->id);
    } break;
    case cs_packet_move::type_num: {
        cs_packet_move *move_packet =
            reinterpret_cast<cs_packet_move *>(packet);
        return ProcessMove(id, move_packet->direction, move_packet->move_time);
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
        cerr << "Unknown type has been received" << endl;
    }
    return false;
}

void Server::process_packet_from_server(unsigned char *buff, size_t length) {
    packet_header *header = (packet_header *)buff;
    unsigned char *packet =
        reinterpret_cast<unsigned char *>(buff + sizeof(packet_header));
    switch (header->type) {
    case ss_packet_put::type_num: {
        ss_packet_put *put_packet = reinterpret_cast<ss_packet_put *>(packet);

        auto &client_slot = clients[put_packet->id];
        client_slot.then_else(
            [](auto &cl) {},
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

        auto msg = make_message<message_proxy_in>(
            put_packet->id, [put_packet](message_proxy_in &msg) {
                msg.x = put_packet->x;
                msg.y = put_packet->y;
            });
        client_slot.ptr->pending_packets.emplace(move(msg));
    } break;
    case ss_packet_leave::type_num: {
        ss_packet_leave *leave_packet = (ss_packet_leave *)packet;
        auto &client_slot = clients[leave_packet->id];
        client_slot.then([&client_slot](auto &old_client) {
            auto msg = make_message<message_proxy_leave>(old_client.id,
                                                         [](auto &_) {});
            old_client.pending_packets.emplace(move(msg));
        });
    } break;
    case ss_packet_move::type_num: {
        ss_packet_move *move_packet = (ss_packet_move *)packet;
        auto &client_slot = clients[move_packet->id];
        client_slot.then([move_packet, this](auto &cl) {
            auto msg = make_message<message_proxy_move>(
                move_packet->id, [move_packet](message_proxy_move &msg) {
                    msg.x = move_packet->x;
                    msg.y = move_packet->y;
                });
            cl.pending_packets.emplace(move(msg));
        });
    } break;
    case ss_packet_hand_over_started::type_num: {
        ss_packet_hand_over_started *h_packet =
            (ss_packet_hand_over_started *)packet;
        clients[h_packet->id].then([h_packet](SOCKETINFO &cl) {
            auto packet = make_message<message_hand_over_started>(
                h_packet->id, [](auto &_) {});

            cl.pending_packets.emplace(move(packet));
        });
    } break;
    case ss_packet_forwarding::type_num: {
        ss_packet_forwarding *f_packet = (ss_packet_forwarding *)packet;
        clients[f_packet->id].then([buff](SOCKETINFO &cl) {
            unsigned char *real_packet =
                (buff + sizeof(packet_header) + sizeof(ss_packet_forwarding));
            unique_ptr<unsigned char[]> new_packet(
                new unsigned char[real_packet[0]]);
            memcpy(new_packet.get(), real_packet, real_packet[0]);
            cl.pending_packets.emplace(move(new_packet));
        });
    } break;
    case ss_packet_hand_overed::type_num: {
        ss_packet_hand_overed *h_packet = (ss_packet_hand_overed *)packet;
        send_packet_to_server<ss_packet_leave>(
            this->other_server_send,
            [h_packet](ss_packet_leave &packet) { packet.id = h_packet->id; });
        clients[h_packet->id].then([h_packet](SOCKETINFO &cl) {
            auto msg = make_message<message_hand_over_ended>(h_packet->id,
                                                             [](auto &_) {});
            cl.pending_packets.emplace(move(msg));
        });
    } break;
    default:
        cerr << "Unknown type has been received" << endl;
    }
}

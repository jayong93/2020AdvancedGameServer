#include "protocol.h"
#include "toml.hpp"
#include <atomic>
#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <vector>

using namespace std;
using namespace boost::asio;
using namespace boost::asio::ip;
using boost_error = boost::system::error_code;

constexpr unsigned MAX_USER_NUM = 20000;
constexpr unsigned MAX_CLIENT_BUF = 1024;

io_context context;

template <typename F>
void send_packet_to_client(tcp::socket &sock, unsigned char packet_size,
                           char packet_type, F &&packet_maker_func) {
    unsigned char *packet = new unsigned char[packet_size];

    packet_header *header = (packet_header *)packet;
    header->size = packet_size;
    header->type = packet_type;

    packet_maker_func(packet + sizeof(packet_header));

    sock.async_send(
        buffer(packet, packet_size), [packet](auto error, auto length) {
            delete[] packet;
            if (error) {
                cerr << "Error at send to client : " << error.message() << endl;
            }
        });
}

template <typename P, typename F>
void send_packet_to_server(tcp::socket &sock, unsigned id,
                           unsigned real_packet_size, F &&packet_maker_func) {
    unsigned packet_size =
        sizeof(packet_header) + sizeof(unsigned) + sizeof(P) + real_packet_size;
    unsigned char *packet = new unsigned char[packet_size];

    packet_header *header = (packet_header *)packet;
    header->size = packet_size;
    header->type = P::type_num;

    unsigned *id_ptr = (unsigned *)(packet + sizeof(packet_header));
    *id_ptr = id;
    packet_maker_func(*(P *)(packet + sizeof(packet_header) + sizeof(unsigned)),
                      packet + (packet_size - real_packet_size));

    sock.async_send(
        buffer(packet, packet_size), [packet](auto error, auto length) {
            delete[] packet;
            if (error) {
                cerr << "Error at send to server : " << error.message() << endl;
            }
        });
}

struct Client {
    tcp::socket socket;
    tcp::socket *server_socket;
    unsigned id;
    unsigned char recv_buf[MAX_CLIENT_BUF];
    unsigned prev_recv_len{0};

    Client(tcp::socket &&sock, tcp::socket &server_sock, unsigned id)
        : socket{move(sock)}, server_socket{&server_sock}, id{id} {}

    void recv() {
        socket.async_read_some(
            buffer(recv_buf + prev_recv_len, MAX_CLIENT_BUF - prev_recv_len),
            [this](auto error, auto len) { handle_recv(error, len); });
    }

    void handle_recv(boost_error error, size_t received_bytes) {
        if (error) {
            cerr << "Error at handle_recv of a client(#" << id
                 << ") : " << error.message() << endl;
            send_packet_to_server<fs_packet_logout>(
                *server_socket, id, 0, [](auto &_, unsigned char *extra) {});
        } else if (received_bytes == 0) {
            send_packet_to_server<fs_packet_logout>(
                *server_socket, id, 0, [](auto &_, unsigned char *extra) {});
        }

        assemble_packet(recv_buf, prev_recv_len, received_bytes,
                        [this](unsigned char *packet, unsigned packet_size) {
                            send_packet_to_server<fs_packet_forwarding>(
                                *server_socket, id, packet_size,
                                [real_packet{packet},
                                 packet_size](fs_packet_forwarding &packet,
                                              unsigned char *extra) {
                                    memcpy(extra, real_packet, packet_size);
                                });
                        });
        recv();
    }

    template <typename F>
    void assemble_packet(unsigned char *recv_buf, unsigned &prev_packet_size,
                         size_t received_bytes, F &&packet_handler) {
        unsigned char *p = recv_buf;
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
                memmove(recv_buf + prev_packet_size, p + prev_packet_size, remain);
                prev_packet_size += remain;
                break;
            }
        }
    }
};

Client *clients[20000];

struct ServerData {
    tcp::socket socket{context};
    unsigned char recv_buf[MAX_BUFFER];
    unsigned prev_packet_size{0};
    ServerData *other;

    void recv() {
        socket.async_read_some(
            buffer(recv_buf + prev_packet_size, MAX_BUFFER - prev_packet_size),
            [this](auto error, auto len) {
                handle_recv(error, len, recv_buf, prev_packet_size);
                recv();
            });
    }

    void handle_recv(boost_error error, size_t len, unsigned char buf[],
                     unsigned &prev_packet_size) {
        if (error) {
            cerr << "Error on recv : " << error.message() << endl;
            exit(-1);
        }
        if (len == 0) {
            exit(0);
        }

        assemble_packet(
            buf, prev_packet_size, len,
            [this](unsigned char packet_size, unsigned char packet_type,
                   unsigned id, unsigned char *packet) {
                switch (packet_type) {
                case sf_packet_hand_over::type_num: {
                    if (id >= 20000) {
                        cerr << "Wrong ID #" << id << endl;
                        return;
                    }
                    auto &client = clients[id];
                    client->server_socket = &other->socket;
                    send_packet_to_server<fs_packet_hand_overed>(
                        *client->server_socket, client->id, 0,
                        [](fs_packet_hand_overed &packet,
                           unsigned char *extra) {});
                } break;
                case sf_packet_reject_login::type_num: {
                } break;
                default: {
                    unsigned char new_packet_size =
                        packet_size - sizeof(unsigned);
                    if (id >= 20000) {
                        cerr << "Wrong ID #" << id << endl;
                        return;
                    }
                    send_packet_to_client(
                        clients[id]->socket, new_packet_size, packet_type,
                        [packet, new_packet_size](unsigned char *buf) {
                            memcpy(buf, packet,
                                   new_packet_size - sizeof(packet_header));
                        });
                } break;
                }
            });
    }

    template <typename F>
    void assemble_packet(unsigned char *recv_buf, unsigned &prev_packet_size,
                         size_t received_bytes, F &&packet_handler) {
        unsigned char *p = recv_buf;
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
                packet_handler(p[0], p[1], *id,
                               (unsigned char *)(p + sizeof(packet_header) +
                                                 sizeof(unsigned)));
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
};

ServerData server1, server2;

atomic_uint next_user_id{0};

void handle_accept(tcp::socket &&sock, tcp::acceptor &acceptor) {
    auto new_user_id = next_user_id.fetch_add(1, memory_order_relaxed);

    tcp::socket *server_sock;
    if (new_user_id % 2 == 0) {
        server_sock = &server1.socket;
    } else {
        server_sock = &server2.socket;
    }
    Client *new_client = new Client{move(sock), *server_sock, new_user_id};
    clients[new_user_id] = new_client;
    new_client->recv();

    acceptor.async_accept(
        [&acceptor](const boost_error &error, tcp::socket sock) {
            if (error) {
                cerr << "Error in accept : " << error.message() << endl;
            } else {
                handle_accept(move(sock), acceptor);
            }
        });
}

void connect_to_servers(const tcp::endpoint &end_point1,
                        const tcp::endpoint &end_point2) {
    boost_error ec;
    server1.socket.connect(end_point1, ec);
    if (ec) {
        cerr << "Can't connect to server 1" << endl;
        exit(-1);
    }
    server2.socket.connect(end_point2, ec);
    if (ec) {
        cerr << "Can't connect to server 2" << endl;
        exit(-1);
    }

    server1.other = &server2;
    server2.other = &server1;

    server1.recv();
    server2.recv();
}

int main() {
    auto config = toml::parse("config.toml");
    const unsigned short port =
        toml::find<unsigned short>(config, "accept_port");
    const string server1_ip = toml::find<string>(config, "server1_ip");
    const string server2_ip = toml::find<string>(config, "server2_ip");
    const unsigned short server1_port =
        toml::find<unsigned short>(config, "server1_port");
    const unsigned short server2_port =
        toml::find<unsigned short>(config, "server2_port");

    connect_to_servers(
        tcp::endpoint{make_address_v4(server1_ip), server1_port},
        tcp::endpoint{make_address_v4(server2_ip), server2_port});

    tcp::acceptor acceptor{context, tcp::endpoint{tcp::v4(), port}};
    acceptor.async_accept(
        [&acceptor](const boost_error &error, tcp::socket sock) {
            if (error) {
                cerr << "Error in accept : " << error.message() << endl;
            } else {
                handle_accept(move(sock), acceptor);
            }
        });

    vector<thread> workers;
    for (auto i = 0; i < 8; ++i) {
        workers.emplace_back([]() { context.run(); });
    }

    for (auto &t : workers) {
        t.join();
    }
}

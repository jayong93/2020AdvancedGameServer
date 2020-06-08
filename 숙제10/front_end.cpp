#include "protocol.h"
#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <vector>

using namespace std;
using namespace boost::asio;
using namespace boost::asio::ip;
using boost_error = boost::system::error_code;

constexpr unsigned MAX_USER_NUM = 20000;

io_context context;

struct Client {
    tcp::socket socket;
    tcp::socket *server_socket;
    unsigned id;

    Client(tcp::socket &&sock, tcp::socket &server_sock, unsigned id)
        : socket{move(sock)}, server_socket{&server_sock}, id{id} {}
};

Client clients[20000];

template <typename F>
void assemble_packet(char *recv_buf, unsigned &prev_packet_size,
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
            unsigned *id_num = (unsigned *)(p + sizeof(packet_header));
            packet_handler(p[0], p[1], *id_num, id_num + 1,
                           (char *)(id_num + 1 + *id_num));
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

template <typename F>
void send_packet_to_client(unsigned id, char packet_size, char packet_type,
                           F &&packet_maker_func) {
    char *packet = new char[packet_size];

    packet_header *header = (packet_header *)packet;
    header->size = packet_size;
    header->type = packet_type;

    packet_maker_func(packet + sizeof(packet_header));

    clients[id].socket.async_send(
        buffer(packet, packet_size), [packet](auto error, auto length) {
            delete[] packet;
            if (error) {
                cerr << "Error at send to server : " << error.message() << endl;
            }
        });
}

template <typename P, typename F>
void send_packet_to_server(tcp::socket &sock, unsigned id,
                           F &&packet_maker_func) {
    unsigned packet_size = sizeof(packet_header) + sizeof(unsigned) + sizeof(P);
    char *packet = new char[packet_size];

    packet_header *header = (packet_header *)packet;
    header->size = packet_size;

    unsigned *id_ptr = (unsigned *)(packet + sizeof(packet_header));
    *id_ptr = id;
    packet_maker_func(
        *(P *)(packet + sizeof(packet_header) + sizeof(unsigned)));

    sock.async_send(
        buffer(packet, packet_size), [packet](auto error, auto length) {
            delete[] packet;
            if (error) {
                cerr << "Error at send to server : " << error.message() << endl;
            }
        });
}

struct ServerData {
    tcp::socket socket{context};
    char recv_buf[MAX_BUFFER];
    unsigned prev_packet_size{0};
    ServerData *other;

    void recv() {
        socket.async_read_some(
            buffer(recv_buf, MAX_BUFFER), [this](auto error, auto len) {
                handle_recv(error, len, recv_buf, prev_packet_size);
                recv();
            });
    }

    void handle_recv(boost_error error, size_t len, char buf[],
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
            [this](char packet_size, char packet_type, unsigned id_num,
                   unsigned *ids, char *packet) {
                switch (packet_type) {
                case sf_packet_accept_login::type_num: {
                    send_packet_to_client(
                        ids[0],
                        sizeof(packet_header) + sizeof(sc_packet_login_ok),
                        sc_packet_login_ok::type_num, [packet](char *buf) {
                            memcpy(buf, packet, sizeof(sc_packet_login_ok));
                        });
                } break;
                case sf_packet_hand_over::type_num: {
                    auto &client = clients[ids[0]];
                    client.server_socket = &other->socket;
                    send_packet_to_server<fs_packet_hand_overed>(
                        *client.server_socket, client.id,
                        [](fs_packet_hand_overed &packet) {});
                    send_packet_to_server<fs_packet_hand_overed>(
                        this->socket, client.id, [](auto &_) {});
                } break;
                case sf_packet_reject_login::type_num: {
                } break;
                default: {
                    char new_packet_size =
                        packet_size - sizeof(unsigned) * (id_num + 1);
                    for (auto i = 0; i < id_num; ++i) {
                        send_packet_to_client(
                            ids[i], new_packet_size, packet_type,
                            [packet, new_packet_size](char *buf) {
                                memcpy(buf, packet,
                                       new_packet_size - sizeof(packet_header));
                            });
                    }
                } break;
                }
            });
    }
};

ServerData server1, server2;

void handle_accept(tcp::socket &&sock) {}

void connect_to_servers(address_v4 &ip1, address_v4 &ip2) {
    boost_error ec;
    server1.socket.connect(tcp::endpoint(ip1, SERVER_PORT + 1), ec);
    if (ec) {
        cerr << "Can't connect to server 1" << endl;
        exit(-1);
    }
    server2.socket.connect(tcp::endpoint(ip2, SERVER_PORT + 2), ec);
    if (ec) {
        cerr << "Can't connect to server 2" << endl;
        exit(-1);
    }

    server1.other = &server2;
    server2.other = &server1;

    server1.recv();
    server2.recv();
}

void main() {
    auto addr = make_address_v4("127.0.0.1");
    connect_to_servers(addr, addr);

    tcp::acceptor acceptor{context, tcp::endpoint{tcp::v4(), SERVER_PORT}};
    acceptor.async_accept([](const boost_error &error, tcp::socket sock) {
        if (error) {
            cerr << "Error in accept : " << error.message() << endl;
        } else {
            handle_accept(move(sock));
        }
    });

    vector<thread> workers;
    for (auto i = 0; i < 8; ++i) {
        workers.emplace_back([]() { context.run(); });
    }
}
#ifndef A5F36F66_1CD6_49C1_9533_263A9B883FE0
#define A5F36F66_1CD6_49C1_9533_263A9B883FE0

#include "mpsc_queue.h"
#include "protocol.h"
#include "spsc_queue.h"
#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>

using namespace std;
using namespace boost::asio;
using namespace boost::asio::ip;
using boost_error = boost::system::error_code;

constexpr unsigned NUM_WORKER = 6;

template <typename F>
void send_packet_to_server(tcp::socket &sock, unsigned packet_size,
                           F &&packet_maker_func) {
    if (packet_size <= 0)
        return;

    char *packet = new char[packet_size];

    packet_maker_func(packet);

    sock.async_send(
        buffer(packet, packet_size), [packet](auto error, auto length) {
            delete[] packet;
            if (error) {
                std::cerr << "Error at send to server : " << error.message()
                          << std::endl;
            }
        });
}

template <typename P, typename F>
void send_packet_to_server(tcp::socket &sock, F &&packet_maker_func) {
    unsigned total_size = sizeof(packet_header) + sizeof(P);

    send_packet_to_server(
        sock, total_size,
        [f{move(packet_maker_func)}, total_size](char *packet) {
            packet_header *header = (packet_header *)packet;
            header->size = total_size;
            header->type = P::type_num;

            f(*(P *)(packet + sizeof(packet_header)));
        });
}

struct ViewEvent {
    enum { VIEW_IN, VIEW_OUT } event;
    unsigned id;
};

enum ClientStatus { Normal, HandOvering, HandOvered };

struct SOCKETINFO {
    unsigned id;
    tcp::socket &sock;
    string name;

    bool is_proxy;
    short x, y;
    int move_time;
    bool is_in_edge{false};
    atomic<ClientStatus> status{Normal};

    MPSCQueue<unique_ptr<char[]>> pending_packets;
    MPSCQueue<unique_ptr<char[]>> pending_while_hand_over_packets;
    atomic_bool is_handling{false};

    SOCKETINFO(unsigned id, tcp::socket &sock) : id{id}, sock{sock} {}
    void insert_to_view(unsigned id) {
        unique_lock<mutex> lg{view_list_lock, try_to_lock};
        if (lg) {
            update_view_from_msg();
            view_list.emplace(id);
        } else {
            view_msg_queue.emplace(ViewEvent::VIEW_IN, id);
        }
    }
    void erase_from_view(unsigned id) {
        unique_lock<mutex> lg{view_list_lock, try_to_lock};
        if (lg) {
            update_view_from_msg();
            view_list.erase(id);
        } else {
            view_msg_queue.emplace(ViewEvent::VIEW_OUT, id);
        }
    }
    set<unsigned> copy_view_list() {
        unique_lock<mutex> lg{view_list_lock};
        update_view_from_msg();
        return view_list;
    }

    void end_hand_over(tcp::socket &send_sock) {
        auto maker = [this, &send_sock](unique_ptr<char[]> packet) {
            unsigned total_size = sizeof(ss_packet_forwarding) +
                                  sizeof(packet_header) + packet[0];
            send_packet_to_server(
                send_sock, total_size, [this, total_size, &packet](char *p) {
                    packet_header *header = (packet_header *)p;
                    header->size = total_size;
                    header->type = ss_packet_forwarding::type_num;

                    p += sizeof(packet_header);
                    ss_packet_forwarding *outer_packet =
                        (ss_packet_forwarding *)p;
                    outer_packet->id = id;

                    p += sizeof(ss_packet_forwarding);
                    memcpy(p, packet.get(), packet[0]);
                });
        };
        this->pending_while_hand_over_packets.for_each(maker);
        this->pending_packets.for_each(maker);
        send_packet_to_server<ss_packet_hand_overed>(
            send_sock,
            [this](ss_packet_hand_overed &packet) { packet.id = id; });
        this->status.store(Normal, std::memory_order_release);
    }

  private:
    set<unsigned> view_list;
    mutex view_list_lock;
    MPSCQueue<ViewEvent> view_msg_queue;

    void update_view_from_msg() {
        const auto size = view_msg_queue.size();
        for (auto i = 0; i < size; ++i) {
            auto event = *view_msg_queue.deq();
            switch (event.event) {
            case ViewEvent::VIEW_IN:
                view_list.emplace(event.id);
                break;
            case ViewEvent::VIEW_OUT:
                view_list.erase(event.id);
                break;
            }
        }
    }
};

struct ClientSlot {
    atomic_bool is_active;
    unique_ptr<SOCKETINFO> ptr;

    operator bool() const { return is_active.load(memory_order_acquire); }

    template <typename F> auto then(F &&func) {
        if (*this) {
            return func(*(this->ptr));
        }
    }

    template <typename F, typename F2> auto then_else(F &&func, F2 &&func2) {
        if (*this) {
            return func(*(this->ptr));
        } else {
            return func2();
        }
    }
};

struct WorkerJob {
    unsigned user_id;
    unique_ptr<char[]> packet;
};

class Server {
  private:
    SOCKETINFO &handle_accept(unsigned user_id);
    void handle_recv(const boost_error &error, const size_t length);
    void handle_recv_from_server(const boost_error &error, const size_t length);
    bool process_packet_from_front_end(unsigned id,
                                       unique_ptr<char[]> &&packet);
    bool process_packet(int id, void *buff);
    void process_packet_from_server(char *buff, size_t length);
    void do_worker(unsigned worker_id);
    void acquire_new_id(unsigned new_id);
    void ProcessLogin(int user_id, char *id_str);
    void ProcessChat(int id, char *mess);
    bool ProcessMove(int id, unsigned char dir, unsigned move_time);
    bool ProcessMove(SOCKETINFO &cl, short new_x, short new_y,
                     unsigned move_time);

    void disconnect(unsigned id);

    unsigned server_id;
    io_context context;
    tcp::acceptor acceptor;
    tcp::acceptor server_acceptor;
    tcp::socket other_server_recv;
    tcp::socket other_server_send;
    tcp::socket front_end_sock;

    array<SPSCQueue<unsigned>, NUM_WORKER> worker_queue;

    char recv_buf[MAX_BUFFER];
    size_t prev_packet_len;

    char other_recv_buf[MAX_BUFFER];
    size_t other_prev_len{0};

  public:
    Server(unsigned id, unsigned short accept_port,
           unsigned short other_server_accept_port);
    void run(const string &other_server_ip, unsigned short other_server_port);
};
#endif /* A5F36F66_1CD6_49C1_9533_263A9B883FE0 */

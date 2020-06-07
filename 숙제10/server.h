#ifndef A5F36F66_1CD6_49C1_9533_263A9B883FE0
#define A5F36F66_1CD6_49C1_9533_263A9B883FE0

#include "mpsc_queue.h"
#include "protocol.h"
#include <boost/asio.hpp>
#include <memory>
#include <mutex>
#include <set>
#include <string>

using namespace std;
using namespace boost::asio;
using namespace boost::asio::ip;
using boost_error = boost::system::error_code;

struct ViewEvent {
    enum { VIEW_IN, VIEW_OUT } event;
    unsigned id;
};

// TODO: client에 있는 recv buf와 prev_packet_size를 server로 옮기기
// TODO: 모든 send 함수들을 server 구조체의 method로 바꾸거나, socket을 받아서
// 전송하도록 변경

struct SOCKETINFO {
    unsigned id;
    tcp::socket &sock;
    string name;

    bool is_proxy;
    short x, y;
    int move_time;
    bool is_in_edge{false};

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
class Server {
  private:
    void handle_accept(unsigned user_id);
    void handle_recv(const boost_error &error, const size_t length);
    void handle_recv_from_server(const boost_error &error, const size_t length);
    void process_packet(int id, void *buff);
    void process_packet_from_server(char *buff, size_t length);
    void acquire_new_id(unsigned new_id);
    void ProcessLogin(int user_id, char *id_str);
    void ProcessChat(int id, char *mess);
    void ProcessMove(int id, unsigned char dir, unsigned move_time);
    void ProcessMove(SOCKETINFO &cl, short new_x, short new_y,
                     unsigned move_time);

    void disconnect(unsigned id);

    unsigned server_id;
    io_context context;
    tcp::acceptor acceptor;
    tcp::acceptor server_acceptor;
    tcp::socket other_server_recv;
    tcp::socket other_server_send;
    tcp::socket front_end_sock;

    char recv_buf[MAX_BUFFER];
    size_t prev_packet_len;

    char other_recv_buf[MAX_BUFFER];
    size_t other_prev_len{0};

  public:
    Server(unsigned short port);
    void run();
};
#endif /* A5F36F66_1CD6_49C1_9533_263A9B883FE0 */

#ifndef A5F36F66_1CD6_49C1_9533_263A9B883FE0
#define A5F36F66_1CD6_49C1_9533_263A9B883FE0

#include <boost/asio.hpp>
#include <set>
#include <string>
#include <memory>
#include "protocol.h"

using namespace std;
using namespace boost::asio;
using namespace boost::asio::ip;
using boost_error = boost::system::error_code;

struct SOCKETINFO
{
    char recv_buf[MAX_BUFFER];
    size_t prev_packet_size;
    tcp::socket socket;
    unsigned id;
    string name;

    bool is_proxy;
    short x, y;
    int move_time;
    set<int> near_id;
    mutex near_lock;

    SOCKETINFO(unsigned id, tcp::socket &&sock) : id{id}, socket{move(sock)} {}
};

struct ClientSlot
{
    atomic_bool is_active;
    unique_ptr<SOCKETINFO> ptr;

    operator bool() const
    {
        return is_active.load(memory_order_acquire);
    }
};
class Server
{
private:
    void handle_accept(tcp::socket &&sock, unsigned user_id);
    void handle_recv(const boost_error &error, const size_t length, SOCKETINFO *client);
    void handle_recv_from_server(const boost_error &error, const size_t length);
    void process_packet(int id, void *buff);
    void process_packet_from_server(char *buff, size_t length);
    void acquire_new_id(unsigned new_id);

    void disconnect(unsigned id);

    const unsigned server_id;
    io_context context;
    tcp::acceptor acceptor;
    tcp::acceptor server_acceptor;
    tcp::socket other_server_sock;
    tcp::socket pending_client_sock;

    char server_recv_buf[MAX_BUFFER];
    size_t prev_packet_len{0};

public:
    Server(unsigned server_id);
    void run();
};
#endif /* A5F36F66_1CD6_49C1_9533_263A9B883FE0 */

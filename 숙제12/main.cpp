#include "protocol.h"
#include "server.h"
#include "toml.hpp"
#include "util.h"
#include <boost/asio.hpp>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>

using namespace std;
using namespace std::chrono;

int main() {
    auto config = toml::parse("config.toml");
    const unsigned id = toml::find<unsigned>(config, "id");
    const unsigned short port = toml::find<unsigned short>(config, "accept_port");
    const unsigned short other_server_accept_port = toml::find<unsigned short>(config, "other_server_accept_port");
    const string other_server_ip = toml::find<string>(config, "other_server_ip");
    const unsigned short other_server_port = toml::find<unsigned short>(config, "other_server_port");
    try {
        Server server{id, port, other_server_accept_port};
        server.run(other_server_ip, other_server_port);
    } catch (const std::exception &e) {
        cerr << "Error at main: " << e.what() << endl;
    }
}

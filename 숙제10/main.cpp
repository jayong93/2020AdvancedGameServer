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
    const unsigned short port = toml::find<unsigned short>(config, "port");
    try {
        Server server{port};
        server.run();
    } catch (const std::exception &e) {
        cerr << "Error at main: " << e.what() << endl;
    }
}

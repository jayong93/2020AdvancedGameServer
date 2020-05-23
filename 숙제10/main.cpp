#include <iostream>
#include <map>
#include <thread>
#include <string>
#include <set>
#include <mutex>
#include <chrono>
#include <queue>
#include <boost/asio.hpp>
#include "protocol.h"
#include "server.h"
#include "util.h"

using namespace std;
using namespace std::chrono;

int main(int argc, char *argv[])
{
	try
	{
		unsigned server_id;
		cin >> server_id;

		if (server_id != 0)
			server_id = 1;

		Server server{server_id};
		server.run();
	}
	catch (const std::exception &e)
	{
		cerr << "Error at main" << e.what() << endl;
	}
}

#include <iostream>
#include <map>
#include <thread>
#include <set>
#include <mutex>
#include <chrono>
#include <queue>
#include <concurrent_unordered_map.h>
#include <memory>
#include <optional>
#include "mpsc_queue.h"

using std::set, std::mutex, std::cout, std::wcout, std::endl, std::lock_guard, std::vector, std::make_pair, std::thread, std::unique_ptr, std::make_unique;
using namespace std::chrono;
#include <WS2tcpip.h>
#include <MSWSock.h>
#pragma comment(lib, "Ws2_32.lib")

#include "protocol.h"

#define MAX_BUFFER        1024
constexpr auto VIEW_RANGE = 7;
constexpr int MAX_PENDING_RECV = 10;
constexpr int MAX_PENDING_SEND = 100;
constexpr int client_limit = 20000; // 예상 최대 client 수
constexpr int completion_queue_size = (MAX_PENDING_RECV + MAX_PENDING_SEND);
constexpr int send_buf_num = 10000;
constexpr int thread_num = 8;

float rand_float(float min, float max) {
	return ((float)rand() / (float)RAND_MAX) * (max - min) + min;
}

struct SendInfo {
	SendInfo() = default;
	SendInfo(int id, unique_ptr<char[]>&& data) : id{ id }, data{ std::move(data) } {}

	int id;
	unique_ptr<char[]> data;
};

RIO_EXTENSION_FUNCTION_TABLE rio_ftable;
PCHAR rio_buffer;
RIO_BUFFERID rio_buf_id;
MPSCQueue<SendInfo> send_queue;
MPSCQueue<RIO_BUF*> empty_send_bufs;

enum EVENT_TYPE { EV_RECV, EV_SEND, EV_MOVE, EV_PLAYER_MOVE_NOTIFY, EV_MOVE_TARGET, EV_ATTACK, EV_HEAL };

struct RequestInfo {
	EVENT_TYPE type;
	RIO_BUF* rio_buf;
};

struct SOCKETINFO
{
	OVERLAPPED completion_over;
	char* recv_buf;
	RIO_RQ	rio_rq;
	mutex rq_lock;
	RIO_CQ	rio_cq;

	char	pre_net_buf[MAX_BUFFER];
	int		prev_packet_size;
	SOCKET	socket;
	int		id;
	char	name[MAX_STR_LEN];

	bool is_connected = false;
	bool is_active;
	short	x, y;
	int seq_no;
	set <int> near_id;
	mutex near_lock;
};

Concurrency::concurrent_unordered_map <int, SOCKETINFO*> clients;
HANDLE	g_iocp;

int new_user_id = 0;

void init_send_bufs(RIO_BUFFERID buf_id) {
	for (auto i = 0; i < send_buf_num; ++i) {
		auto buf = new RIO_BUF;
		buf->BufferId = buf_id;
		buf->Offset = (client_limit + i) * MAX_BUFFER;
		buf->Length = MAX_BUFFER;
		empty_send_bufs.enq(buf);
	}
}

char* get_send_buf(const RIO_BUF& rio_buf) {
	return rio_buffer + rio_buf.Offset;
}

void error_display(const char* msg, int err_no)
{
	WCHAR* lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	cout << msg;
	wcout << L"에러 " << lpMsgBuf << endl;
	while (true);
	LocalFree(lpMsgBuf);
}

bool is_near(int a, int b)
{
	if (VIEW_RANGE < abs(clients[a]->x - clients[b]->x)) return false;
	if (VIEW_RANGE < abs(clients[a]->y - clients[b]->y)) return false;
	return true;
}

void send_packet(int id, void* buff)
{
	//char* packet = reinterpret_cast<char*>(buff);
	//int packet_size = packet[0];
	//OVER_EX* send_over = new OVER_EX;
	//memset(send_over, 0x00, sizeof(OVER_EX));
	//send_over->event_type = EV_SEND;
	//memcpy(send_over->net_buf, packet, packet_size);
	//send_over->wsabuf[0].buf = send_over->net_buf;
	//send_over->wsabuf[0].len = packet_size;
	//int ret = WSASend(clients[id]->socket, send_over->wsabuf, 1, 0, 0, &send_over->over, 0);
	//if (0 != ret) {
	//	int err_no = WSAGetLastError();
	//	if (WSA_IO_PENDING != err_no)
	//		error_display("WSARecv Error :", err_no);
	//}
	if (false == clients[id]->is_connected) { return; }

	char* packet = reinterpret_cast<char*>(buff);

	unique_ptr<char[]> p_data{ packet };
	send_queue.enq(SendInfo(id, std::move(p_data)));
}

void send_login_ok_packet(int id)
{
	sc_packet_login_ok& packet = *new sc_packet_login_ok;
	packet.id = id;
	packet.size = sizeof(packet);
	packet.type = SC_LOGIN_OK;
	packet.x = clients[id]->x;
	packet.y = clients[id]->y;
	packet.hp = 100;
	packet.level = 1;
	packet.exp = 1;
	send_packet(id, &packet);
}

void send_login_fail(int id)
{
	sc_packet_login_fail& packet = *new sc_packet_login_fail;
	packet.size = sizeof(packet);
	packet.type = SC_LOGIN_FAIL;
	send_packet(id, &packet);
}

void send_put_object_packet(int client, int new_id)
{
	sc_packet_put_object& packet = *new sc_packet_put_object;
	packet.id = new_id;
	packet.size = sizeof(packet);
	packet.type = SC_PUT_OBJECT;
	packet.x = clients[new_id]->x;
	packet.y = clients[new_id]->y;
	packet.o_type = 1;
	send_packet(client, &packet);

	if (client == new_id) return;
	lock_guard<mutex>lg{ clients[client]->near_lock };
	clients[client]->near_id.insert(new_id);
}

void send_pos_packet(int client, int mover)
{
	sc_packet_pos& packet = *new sc_packet_pos;
	packet.id = mover;
	packet.size = sizeof(packet);
	packet.type = SC_POS;
	packet.x = clients[mover]->x;
	packet.y = clients[mover]->y;
	packet.seq_no = clients[client]->seq_no;

	clients[client]->near_lock.lock();
	if ((client == mover) || 0 != clients[client]->near_id.count(mover)) {
		clients[client]->near_lock.unlock();
		send_packet(client, &packet);
	}
	else {
		clients[client]->near_lock.unlock();
		send_put_object_packet(client, mover);
	}
}

void send_remove_object_packet(int client, int leaver)
{
	sc_packet_remove_object& packet = *new sc_packet_remove_object;
	packet.id = leaver;
	packet.size = sizeof(packet);
	packet.type = SC_REMOVE_OBJECT;
	send_packet(client, &packet);

	lock_guard<mutex>lg{ clients[client]->near_lock };
	clients[client]->near_id.erase(leaver);
}

bool is_near_id(int player, int other)
{
	lock_guard <mutex> gl{ clients[player]->near_lock };
	return (0 != clients[player]->near_id.count(other));
}

void Disconnect(int id)
{
	clients[id]->is_connected = false;
	closesocket(clients[id]->socket);
	printf("User #%d has disconnected\n", id);
	for (auto& cl : clients) {
		if (true == cl.second->is_connected)
			send_remove_object_packet(cl.first, id);
	}
}

void ProcessMove(int id, unsigned char dir)
{
	short x = clients[id]->x;
	short y = clients[id]->y;
	clients[id]->near_lock.lock();
	auto old_vl = clients[id]->near_id;
	clients[id]->near_lock.unlock();
	switch (dir) {
	case D_UP: if (y > 0) y--;
		break;
	case D_DOWN: if (y < WORLD_HEIGHT - 1) y++;
		break;
	case D_LEFT: if (x > 0) x--;
		break;
	case D_RIGHT: if (x < WORLD_WIDTH - 1) x++;
		break;
	case 99:
		x = rand() % WORLD_WIDTH;
		y = rand() % WORLD_HEIGHT;
		break;
	default: cout << "Invalid Direction Error\n";
		while (true);
	}

	clients[id]->x = x;
	clients[id]->y = y;

	set <int> new_vl;
	for (auto& cl : clients) {
		int other = cl.second->id;
		if (id == other) continue;
		if (false == clients[other]->is_connected) continue;
		if (true == is_near(id, other)) new_vl.insert(other);
	}

	send_pos_packet(id, id);
	for (auto cl : old_vl) {
		if (0 != new_vl.count(cl)) {
			send_pos_packet(cl, id);
		}
		else
		{
			send_remove_object_packet(id, cl);
			send_remove_object_packet(cl, id);
		}
	}
	for (auto cl : new_vl) {
		if (0 == old_vl.count(cl)) {
			send_put_object_packet(id, cl);
			send_put_object_packet(cl, id);
		}
	}
}

void ProcessLogin(int user_id, char* id_str)
{
	//for (auto cl : clients) {
	//	if (0 == strcmp(cl.second->name, id_str)) {
	//		send_login_fail(user_id);
	//		Disconnect(user_id);
	//		return;
	//	}
	//}
	strcpy_s(clients[user_id]->name, id_str);
	clients[user_id]->is_connected = true;
	send_login_ok_packet(user_id);

	for (auto& cl : clients) {
		int other_player = cl.first;
		if (false == clients[other_player]->is_connected) continue;
		if (true == is_near(other_player, user_id)) {
			send_put_object_packet(other_player, user_id);
			if (other_player != user_id) {
				send_put_object_packet(user_id, other_player);
			}
		}
	}
}

void ProcessPacket(int id, void* buff)
{
	char* packet = reinterpret_cast<char*>(buff);
	switch (packet[1]) {
	case CS_LOGIN: {
		cs_packet_login* login_packet = reinterpret_cast<cs_packet_login*>(packet);
		ProcessLogin(id, login_packet->id);
	}
				 break;
	case CS_MOVE: {
		cs_packet_move* move_packet = reinterpret_cast<cs_packet_move*>(packet);
		clients[id]->seq_no = move_packet->seq_no;
		ProcessMove(id, move_packet->direction);
	}
				break;
	case CS_ATTACK:
		break;
	case CS_CHAT:
		break;
	case CS_LOGOUT:
		break;
	case CS_TELEPORT:
		ProcessMove(id, 99);
		break;
	default: cout << "Invalid Packet Type Error\n";
		while (true);
	}
}

void do_worker()
{
	while (true) {
		DWORD num_byte;
		ULONGLONG key64;
		PULONG_PTR p_key = &key64;
		WSAOVERLAPPED* p_over;

		GetQueuedCompletionStatus(g_iocp, &num_byte, p_key, &p_over, INFINITE);
		int key = static_cast<int>(key64);
		auto client = clients[key];

		RIORESULT results[10];
		auto num_result = rio_ftable.RIODequeueCompletion(client->rio_cq, results, 10);
		rio_ftable.RIONotify(client->rio_cq); // client의 rio_cq에서 필요한 만큼 dequeue를 끝냈으므로 다른 worker thread에게 처리를 넘겨도 됨.

		for (unsigned long i = 0; i < num_result; ++i) {
			const RIORESULT& result = results[i];
			SOCKET client_s = result.SocketContext;
			auto req_info = (RequestInfo*)(result.RequestContext);

			if (result.BytesTransferred == 0) {
				if (EV_RECV == req_info->type) {
					delete req_info->rio_buf;
				}
				else if (EV_SEND == req_info->type) {
					empty_send_bufs.enq(req_info->rio_buf);
				}
				delete req_info;
				Disconnect(key);
				continue;
			}  // 클라이언트가 closesocket을 했을 경우		
			//OVER_EX* over_ex = reinterpret_cast<OVER_EX*> (p_over);


			if (EV_RECV == req_info->type) {
				char* p = client->recv_buf;
				int remain = result.BytesTransferred;
				int packet_size;
				int prev_packet_size = client->prev_packet_size;
				if (0 == prev_packet_size)
					packet_size = 0;
				else packet_size = client->pre_net_buf[0];
				while (remain > 0) {
					if (0 == packet_size) packet_size = p[0];
					int required = packet_size - prev_packet_size;
					if (required <= remain) {
						memcpy(client->pre_net_buf + prev_packet_size, p, required);
						ProcessPacket(key, client->pre_net_buf);
						remain -= required;
						p += required;
						prev_packet_size = 0;
						packet_size = 0;
					}
					else {
						memcpy(client->pre_net_buf + prev_packet_size, p, remain);
						prev_packet_size += remain;
						remain = 0;
					}
				}
				client->prev_packet_size = prev_packet_size;

				{
					lock_guard<mutex> lg{ client->rq_lock };
					int ret = rio_ftable.RIOReceive(client->rio_rq, req_info->rio_buf, 1, 0, (void*)req_info);
					if (TRUE != ret) {
						int err_no = WSAGetLastError();
						if (WSA_IO_PENDING != err_no)
							error_display("WSAReceive Error :", err_no);
					}
				}
			}
			else if (EV_SEND == req_info->type) {
				empty_send_bufs.enq(req_info->rio_buf);
				delete req_info;
			}
			else {
				cout << "Unknown Event Type :" << result.RequestContext << endl;
				while (true);
			}
		}
	}
}

void broadcast() {
	while (true) {
		while (true) {
			auto retval = send_queue.deq();
			if (!retval) {
				break;
			}

			auto& send_info = *retval;
			auto client = clients[send_info.id];
			unsigned char data_size = send_info.data[0];

			std::optional<RIO_BUF*> rio_buf;
			while (true) {
				rio_buf = empty_send_bufs.deq();
				if (!rio_buf) std::this_thread::yield();
				else break;
			}

			auto send_buf = get_send_buf(**rio_buf);

			memcpy_s(send_buf, MAX_BUFFER, send_info.data.get(), data_size);

			auto req_info = new RequestInfo;
			req_info->type = EV_SEND;
			req_info->rio_buf = *rio_buf;
			req_info->rio_buf->Length = data_size;

			{
				lock_guard<mutex> lg{ client->rq_lock };
				int ret = rio_ftable.RIOSend(client->rio_rq, req_info->rio_buf, 1, 0, (void*)req_info);
				if (TRUE != ret) {
					int err_no = WSAGetLastError();
					switch (err_no) {
					case WSA_IO_PENDING:
						break;
					case WSAECONNRESET:
					case WSAECONNABORTED:
					case WSAENOTSOCK:
						empty_send_bufs.enq(*rio_buf);
						Disconnect(send_info.id);
						break;
					default:
						error_display("WSASend Error :", err_no);
					}
				}
			}
		}
	}
}

void init_rio(SOCKET listen_sock) {
	GUID f_table_id = WSAID_MULTIPLE_RIO;
	DWORD returned_bytes;
	DWORD result;
	result = WSAIoctl(listen_sock, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &f_table_id, sizeof(GUID), &rio_ftable, sizeof(rio_ftable), &returned_bytes, nullptr, nullptr);
	if (result == SOCKET_ERROR) {
		error_display("WSAIoctl Error(in 'init_rio') :", WSAGetLastError());
		exit(-1);
	}

	// rio_buffer의 앞쪽 절반은 recv용 버퍼, 나머지는 send용.
	constexpr int buffer_size = (client_limit + send_buf_num) * MAX_BUFFER;
	rio_buffer = (PCHAR)VirtualAllocEx(GetCurrentProcess(), nullptr, buffer_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	rio_buf_id = rio_ftable.RIORegisterBuffer(rio_buffer, buffer_size);

	init_send_bufs(rio_buf_id);
}

int main()
{
	wcout.imbue(std::locale("korean"));

	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	SOCKET listenSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_REGISTERED_IO);
	SOCKADDR_IN serverAddr;
	memset(&serverAddr, 0, sizeof(SOCKADDR_IN));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVER_PORT);
	serverAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
	if (SOCKET_ERROR == ::bind(listenSocket, (struct sockaddr*) & serverAddr, sizeof(SOCKADDR_IN))) {
		error_display("WSARecv Error :", WSAGetLastError());
	}
	listen(listenSocket, 5);

	init_rio(listenSocket);

	SOCKADDR_IN clientAddr;
	int addrLen = sizeof(SOCKADDR_IN);
	memset(&clientAddr, 0, addrLen);

	g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
	vector <thread> worker_threads;
	thread broadcaster{ broadcast };
	for (int i = 0; i < thread_num; ++i) worker_threads.emplace_back(do_worker);


	while (true) {
		SOCKET clientSocket = accept(listenSocket, (struct sockaddr*) & clientAddr, &addrLen);
		if (INVALID_SOCKET == clientSocket) {
			int err_no = WSAGetLastError();
			if (WSA_IO_PENDING != err_no)
				error_display("Accept Error :", err_no);
		}
		int user_id = new_user_id++;

		if (client_limit <= new_user_id) {
			fprintf(stderr, "Can't accept more clients\n");
			exit(-1);
		}

		SOCKETINFO* new_player = new SOCKETINFO;
		new_player->id = user_id;
		new_player->socket = clientSocket;
		new_player->prev_packet_size = 0;
		new_player->recv_buf = rio_buffer + (user_id * MAX_BUFFER);

		ZeroMemory(&new_player->completion_over, sizeof(OVERLAPPED));
		RIO_NOTIFICATION_COMPLETION rio_noti;
		rio_noti.Type = RIO_IOCP_COMPLETION;
		rio_noti.Iocp.IocpHandle = g_iocp;
		rio_noti.Iocp.Overlapped = &new_player->completion_over;
		rio_noti.Iocp.CompletionKey = (void*)user_id;
		new_player->rio_cq = rio_ftable.RIOCreateCompletionQueue(completion_queue_size, &rio_noti);
		if (new_player->rio_cq == RIO_INVALID_CQ) {
			error_display("RIOCreateCompletionQueue Error :", WSAGetLastError());
		}
		new_player->rio_rq = rio_ftable.RIOCreateRequestQueue(clientSocket, MAX_PENDING_RECV, 1, MAX_PENDING_SEND, 1, new_player->rio_cq, new_player->rio_cq, (void*)clientSocket);
		if (new_player->rio_rq == RIO_INVALID_RQ) {
			error_display("RIOCreateRequestQueue Error :", WSAGetLastError());
		}
		new_player->x = rand_float(0, WORLD_WIDTH);
		new_player->y = rand_float(0, WORLD_HEIGHT);
		clients.insert(make_pair(user_id, new_player));

		printf("User #%d has connected\n", user_id);

		auto req_info = new RequestInfo;
		req_info->type = EV_RECV;
		req_info->rio_buf = new RIO_BUF;
		req_info->rio_buf->BufferId = rio_buf_id;
		req_info->rio_buf->Length = MAX_BUFFER;
		req_info->rio_buf->Offset = user_id * MAX_BUFFER;

		{
			lock_guard<mutex> lg{ new_player->rq_lock };
			int ret = rio_ftable.RIOReceive(new_player->rio_rq, req_info->rio_buf, 1, 0, (void*)req_info);
			if (TRUE != ret) {
				int err_no = WSAGetLastError();
				if (WSA_IO_PENDING != err_no)
					error_display("RIORecv Error :", err_no);
			}
		}
		rio_ftable.RIONotify(new_player->rio_cq);
	}
	for (auto& th : worker_threads) th.join();
	closesocket(listenSocket);
	WSACleanup();
}


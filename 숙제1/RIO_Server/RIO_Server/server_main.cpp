#include <iostream>
#include <map>
#include <vector>
using namespace std;
#include <WS2tcpip.h>
#include <MSWSock.h>
#pragma comment(lib, "Ws2_32.lib")

#define MAX_BUFFER        1024 // client용 buffer chunk 크기
#define SERVER_PORT        3500

RIO_EXTENSION_FUNCTION_TABLE rio_ftable;
PCHAR rio_buffer;
RIO_BUFFERID rio_buffer_id;
LPFN_ACCEPTEX accept_ex;
constexpr int MAX_PENDING_RECV = 1000;
constexpr int MAX_PENDING_SEND = 1000;
constexpr int rio_buffer_size = 1024; // client용 buffer chunk 개수


struct SOCKETINFO
{
	BYTE* dataBuffer = nullptr;
	SOCKET socket = NULL;
	RIO_RQ rq = nullptr;
	size_t buffer_index = 0;

	SOCKETINFO() {}
	SOCKETINFO(RIO_RQ rq, SOCKET sock, size_t buf_idx) : rq{ rq }, socket{ sock }, buffer_index{ buf_idx }, dataBuffer{ (BYTE*)rio_buffer + (MAX_BUFFER * buf_idx) } {}
};

map <SOCKET, SOCKETINFO> clients;
vector<size_t> available_buf_idx;

void cleanup_client(SOCKET sock) {
	auto& client = clients[sock];
	available_buf_idx.push_back(client.buffer_index);
	clients.erase(sock);
	printf("Client [%lld] has disconnected\n", sock);
}

void handle_recv(SOCKET sock, ULONG num_bytes)
{
	auto& client = clients[sock];

	RIO_BUF buf;
	buf.BufferId = rio_buffer_id;
	buf.Offset = client.buffer_index * MAX_BUFFER;
	buf.Length = num_bytes;

	auto retval = rio_ftable.RIOSend(client.rq, &buf, 1, NULL, (PVOID)1);
	if (retval == FALSE) {
		fprintf(stderr, "Error at RIOSend: %d\n", WSAGetLastError());
	}
}

void handle_send(SOCKET sock, ULONG num_bytes)
{
	auto& client = clients[sock];

	RIO_BUF buf;
	buf.BufferId = rio_buffer_id;
	buf.Offset = client.buffer_index * MAX_BUFFER;
	buf.Length = MAX_BUFFER;

	auto retval = rio_ftable.RIOReceive(client.rq, &buf, 1, NULL, (PVOID)0);
	if (retval == FALSE) {
		fprintf(stderr, "Error at RIOReceive: %d\n", WSAGetLastError());
	}
}

RIO_CQ InitializeRIO(SOCKET listen_sock) {
	GUID f_table_id = WSAID_MULTIPLE_RIO;
	DWORD returned_bytes;
	DWORD result;
	result = WSAIoctl(listen_sock, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &f_table_id, sizeof(GUID), &rio_ftable, sizeof(rio_ftable), &returned_bytes, nullptr, nullptr);
	if (result == SOCKET_ERROR) {
		fprintf(stderr, "WSAIoctl with RIO Function Table has failed\n");
		exit(-1);
	}
	GUID accept_ex_id = WSAID_ACCEPTEX;
	result = WSAIoctl(listen_sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &accept_ex_id, sizeof(GUID), &accept_ex, sizeof(accept_ex), &returned_bytes, nullptr, nullptr);
	if (result == SOCKET_ERROR) {
		fprintf(stderr, "WSAIoctl with RIO Function Table has failed\n");
		exit(-1);
	}

	constexpr int buffer_size = rio_buffer_size * MAX_BUFFER;
	rio_buffer = (PCHAR)VirtualAllocEx(GetCurrentProcess(), nullptr, buffer_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	rio_buffer_id = rio_ftable.RIORegisterBuffer(rio_buffer, buffer_size);
	for (int i = 0; i < rio_buffer_size; ++i) {
		available_buf_idx.push_back(i);
	}

	constexpr int completion_queue_size = (MAX_PENDING_RECV + MAX_PENDING_SEND) * 100;
	RIO_CQ rio_cq = rio_ftable.RIOCreateCompletionQueue(completion_queue_size, nullptr);
	return rio_cq;
}

void handle_connection(SOCKET client, RIO_CQ rio_cq) {
	auto rq = rio_ftable.RIOCreateRequestQueue(client, MAX_PENDING_RECV, 1, MAX_PENDING_SEND, 1, rio_cq, rio_cq, (PVOID)client);
	auto buf_idx = available_buf_idx.back();
	clients[client] = SOCKETINFO{ rq, client, buf_idx };
	available_buf_idx.pop_back();
	printf("New Client [%lld] has arrived\n", client);

	RIO_BUF buf;
	buf.BufferId = rio_buffer_id;
	buf.Length = MAX_BUFFER;
	buf.Offset = buf_idx * MAX_BUFFER;
	auto retval = rio_ftable.RIOReceive(rq, &buf, 1, 0, 0 /* 0이면 recv, 1이면 send */);
	if (retval == FALSE) {
		fprintf(stderr, "Error at RIOReceive: %d\n", WSAGetLastError());
	}
}

int main()
{
	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	SOCKET listenSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_REGISTERED_IO);
	SOCKADDR_IN serverAddr;
	memset(&serverAddr, 0, sizeof(SOCKADDR_IN));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVER_PORT);
	serverAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
	::bind(listenSocket, (struct sockaddr*) & serverAddr, sizeof(SOCKADDR_IN));

	auto rio_cq = InitializeRIO(listenSocket);

	listen(listenSocket, 5);
	SOCKADDR_IN clientAddr;
	int addrLen = sizeof(SOCKADDR_IN);
	memset(&clientAddr, 0, addrLen);
	SOCKET clientSocket;;
	BYTE address_data[(sizeof(SOCKADDR_IN) + 16) * 2];
	DWORD received_bytes;
	WSAOVERLAPPED ov;
	int last_result = TRUE;
	int last_error = 0;

	while (true) {
		if (FALSE != last_result) {
			ZeroMemory(&ov, sizeof(WSAOVERLAPPED));
			clientSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_REGISTERED_IO);
			setsockopt(clientSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&listenSocket, sizeof(listenSocket));
			last_result = accept_ex(listenSocket, clientSocket, address_data, 0, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &received_bytes, &ov);
			last_error = WSAGetLastError();
		}
		if (FALSE == last_result) {
			if (last_error == ERROR_IO_PENDING) {
				auto result = GetOverlappedResult((HANDLE)listenSocket, &ov, &received_bytes, FALSE);
				int error;
				if (FALSE != result) {
					last_result = result;
					handle_connection(clientSocket, rio_cq);
				}
				else if ((error = WSAGetLastError()) != ERROR_IO_INCOMPLETE) {
					fprintf(stderr, "Error at AcceptEx -> GetOverlappedResult: %d\n", error);
					exit(-1);
				}
			}
			else {
				fprintf(stderr, "Error at AcceptEx: %d\n", last_error);
				exit(-1);
			}
		}

		constexpr int result_size = 10;
		RIORESULT results[result_size];
		auto num_result = rio_ftable.RIODequeueCompletion(rio_cq, results, result_size);
		for (auto i = 0; i < num_result; ++i) {
			SOCKET sock = results[i].SocketContext;
			auto num_bytes = results[i].BytesTransferred;
			// 접속 종료시
			if (num_bytes == 0) {
				cleanup_client(sock);
				continue;
			}
			// Recv 이면
			if (results[i].RequestContext == 0) {
				handle_recv(sock, num_bytes);
			}
			else {
				handle_send(sock, num_bytes);
			}
		}
	}
	closesocket(listenSocket);
	WSACleanup();
}


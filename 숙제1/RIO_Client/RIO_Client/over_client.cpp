#include <iostream>
#include <WS2tcpip.h>
#include <thread>
using namespace std;
#include <MSWSock.h>
#pragma comment(lib, "Ws2_32.lib")
#define MAX_BUFFER        1024
#define SERVER_IP        "127.0.0.1"
#define SERVER_PORT        3500

RIO_EXTENSION_FUNCTION_TABLE rio_ftable;
PCHAR rio_buffer;
RIO_BUFFERID rio_buffer_id;
constexpr int MAX_PENDING_RECV = 1000;
constexpr int MAX_PENDING_SEND = 1000;


RIO_CQ InitializeRIO(SOCKET listen_sock) {
	GUID f_table_id = WSAID_MULTIPLE_RIO;
	DWORD returned_bytes;
	DWORD result;
	result = WSAIoctl(listen_sock, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &f_table_id, sizeof(GUID), &rio_ftable, sizeof(rio_ftable), &returned_bytes, nullptr, nullptr);
	if (result == SOCKET_ERROR) {
		fprintf(stderr, "WSAIoctl with RIO Function Table has failed\n");
		exit(-1);
	}

	constexpr int buffer_size = MAX_BUFFER;
	rio_buffer = (PCHAR)VirtualAllocEx(GetCurrentProcess(), nullptr, buffer_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	rio_buffer_id = rio_ftable.RIORegisterBuffer(rio_buffer, buffer_size);

	constexpr int completion_queue_size = (MAX_PENDING_RECV + MAX_PENDING_SEND);
	RIO_CQ rio_cq = rio_ftable.RIOCreateCompletionQueue(completion_queue_size, nullptr);
	return rio_cq;
}

int main()
{
	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 0), &WSAData);
	SOCKET serverSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_REGISTERED_IO);

	auto rio_cq = InitializeRIO(serverSocket);
	auto rio_rq = rio_ftable.RIOCreateRequestQueue(serverSocket, MAX_PENDING_RECV, 1, MAX_PENDING_SEND, 1, rio_cq, rio_cq, (PVOID)serverSocket);

	SOCKADDR_IN serverAddr;
	memset(&serverAddr, 0, sizeof(SOCKADDR_IN));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVER_PORT);
	inet_pton(AF_INET, SERVER_IP, &serverAddr.sin_addr);
	connect(serverSocket, (struct sockaddr*) & serverAddr, sizeof(serverAddr));

	cout << "Enter Message :";
	cin >> rio_buffer;
	int bufferLen = static_cast<int>(strlen(rio_buffer));
	RIO_BUF buf;
	buf.BufferId = rio_buffer_id;
	buf.Length = bufferLen;
	buf.Offset = 0;
	auto retval = rio_ftable.RIOSend(rio_rq, &buf, 1, 0, (PVOID)1);
	if (retval == FALSE) {
		fprintf(stderr, "Error at RIOSend: %d\n", WSAGetLastError());
		exit(-1);
	}

	RIORESULT results[10];
	while (true) {
		auto num_result = rio_ftable.RIODequeueCompletion(rio_cq, results, 10);
		for (auto i = 0; i < num_result; ++i) {
			auto& result = results[i];
			if (result.BytesTransferred == 0) {
				fprintf(stderr, "Connection has been closed\n");
				exit(-1);
			}
			// Receive
			if (result.RequestContext == 0)
			{
				rio_buffer[result.BytesTransferred] = 0;
				cout << "Received : " << rio_buffer << " (" << result.BytesTransferred << " bytes)\n";
				cout << "Enter Message :";
				cin >> rio_buffer;
				int bufferLen = static_cast<int>(strlen(rio_buffer));
				RIO_BUF buf;
				buf.BufferId = rio_buffer_id;
				buf.Length = bufferLen;
				buf.Offset = 0;
				auto retval = rio_ftable.RIOSend(rio_rq, &buf, 1, 0, (PVOID)1);
				if (retval == FALSE) {
					fprintf(stderr, "Error at RIOSend: %d\n", WSAGetLastError());
					exit(-1);
				}
			}
			// Send
			else {
				RIO_BUF buf;
				buf.BufferId = rio_buffer_id;
				buf.Length = MAX_BUFFER;
				buf.Offset = 0;
				auto retval = rio_ftable.RIOReceive(rio_rq, &buf, 1, 0, 0);
				if (retval == FALSE) {
					fprintf(stderr, "Error at RIOReceive: %d\n", WSAGetLastError());
					exit(-1);
				}
			}
		}
		this_thread::yield();
	}
	closesocket(serverSocket);
	WSACleanup();
}

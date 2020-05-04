#pragma once
#define MAX_BUFFER        1024
constexpr int MAX_PENDING_RECV = 10;
constexpr int MAX_PENDING_SEND = 5000;
constexpr int client_limit = 20000; // 예상 최대 client 수
constexpr int thread_num = 8;
constexpr int completion_queue_size = ((MAX_PENDING_RECV + MAX_PENDING_SEND) * client_limit) / thread_num;
constexpr int send_buf_num = client_limit * 50;
constexpr float SEND_BUF_RATE_ON_BUSY = 0.1f;
constexpr unsigned MAX_PLAYER_ROUTINE_LOOP_TIME = 10;
constexpr unsigned MAX_ZONE_ROUTINE_LOOP_TIME = 50;

template<class... Ts> struct overloaded : Ts... {
    using Ts::operator()...;
};
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;


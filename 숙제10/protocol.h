#pragma once

constexpr unsigned MAX_BUFFER = 1024 * 100;
constexpr unsigned MAX_ID_LEN = 50;
constexpr unsigned MAX_STR_LEN = 50;

#define WORLD_WIDTH 400
#define WORLD_HEIGHT 400

#define NPC_ID_START 20000

#define SERVER_PORT 9000
#define NUM_NPC 100

#pragma pack(push, 1)

struct packet_header {
    char size;
    char type;
};

struct sc_packet_login_ok {
    using type = char;
    static constexpr type type_num = 1;
    int id;
    short x, y;
    short hp;
    short level;
    int exp;
};

struct sc_packet_login_fail {
    using type = char;
    static constexpr type type_num = 2;
};

struct sc_packet_pos {
    using type = char;
    static constexpr type type_num = 3;
    int id;
    short x, y;
    unsigned move_time;
};

struct sc_packet_put_object {
    using type = char;
    static constexpr type type_num = 4;
    int id;
    char o_type;
    short x, y;
    // 렌더링 정보, 종족, 성별, 착용 아이템, 캐릭터 외형, 이름, 길드....
};

struct sc_packet_remove_object {
    using type = char;
    static constexpr type type_num = 5;
    int id;
};

struct sc_packet_chat {
    using type = char;
    static constexpr type type_num = 6;
    int id;
    char chat[100];
};

struct sc_packet_stat_change {
    using type = char;
    static constexpr type type_num = 7;
    short hp;
    short level;
    int exp;
};

struct ss_packet_put {
    using type = char;
    static constexpr type type_num = 1;
    unsigned id;
    short x, y;
};

struct ss_packet_leave {
    using type = char;
    static constexpr type type_num = 2;
    unsigned id;
};

struct ss_packet_move {
    using type = char;
    static constexpr type type_num = 3;
    unsigned id;
    short x, y;
};

struct ss_packet_forwarding {
    using type = char;
    static constexpr type type_num = 4;
    unsigned id;
};

struct ss_packet_hand_overed {
    using type = char;
    static constexpr type type_num = 5;
    unsigned id;
};

struct ss_packet_hand_over_started {
    using type = char;
    static constexpr type type_num = 6;
    unsigned id;
};

// - try_login: front-end가 server에게. 로그인 시도하는 id가 담겨있음
// - accept_login: server가 front-end에게. 시도한 id 그대로 돌려줌
// - logout: front-end가 server에게. 접속이 끊어진 client id를 전송
// - hand_over: server가 front-end와 다른 server에게. 다른 서버는 is_proxy를 바꾸고, front-end는 담당 서버 변경(기존 소켓 disconnect 후 새로운 서버에 connect)
// - forwarding_packet: 전체 사이즈 + forwarding_packet_type + 대상 id + 실제 패킷(size, type 포함)

struct sf_packet_reject_login {
    using type = char;
    static constexpr type type_num = 13;
};

struct fs_packet_logout {
    using type = char;
    static constexpr type type_num = 14;
};

struct sf_packet_hand_over {
    using type = char;
    static constexpr type type_num = 15;
};

struct fs_packet_hand_overed {
    using type = char;
    static constexpr type type_num = 16;
};

struct fs_packet_forwarding {
    using type = char;
    static constexpr type type_num = 17;
};

struct message_hand_over_started {
    using type = char;
    static constexpr type type_num = 18;
};

struct cs_packet_login {
    using type = char;
    static constexpr type type_num = 1;
    char id[MAX_ID_LEN];
};

constexpr unsigned char D_UP = 0;
constexpr unsigned char D_DOWN = 1;
constexpr unsigned char D_LEFT = 2;
constexpr unsigned char D_RIGHT = 3;

struct cs_packet_move {
    using type = char;
    static constexpr type type_num = 2;
    char direction;
    int move_time;
};

struct cs_packet_attack {
    using type = char;
    static constexpr type type_num = 3;
};

struct cs_packet_chat {
    using type = char;
    static constexpr type type_num = 4;
    char chat_str[100];
};

struct cs_packet_logout {
    using type = char;
    static constexpr type type_num = 5;
};

struct cs_packet_teleport {
    using type = char;
    static constexpr type type_num = 6;
};

#pragma pack(pop)
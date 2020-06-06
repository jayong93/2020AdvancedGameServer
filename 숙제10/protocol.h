#pragma once

constexpr unsigned MAX_BUFFER = 1024;
constexpr unsigned MAX_ID_LEN = 50;
constexpr unsigned MAX_STR_LEN = 50;

#define WORLD_WIDTH 20
#define WORLD_HEIGHT 40

#define NPC_ID_START 20000

#define SERVER_PORT 9000
#define NUM_NPC 100

#define CS_LOGIN 1
#define CS_MOVE 2
#define CS_ATTACK 3
#define CS_CHAT 4
#define CS_LOGOUT 5
#define CS_TELEPORT 6

#define SC_LOGIN_OK 1
#define SC_LOGIN_FAIL 2
#define SC_POS 3
#define SC_PUT_OBJECT 4
#define SC_REMOVE_OBJECT 5
#define SC_CHAT 6
#define SC_STAT_CHANGE 7

#define SS_PUT 8
#define SS_LEAVE 9
#define SS_MOVE 10

#pragma pack(push, 1)

// TODO: Front-End�� ����� ��Ŷ�� ����
// - try_login: front-end�� server����. �α��� �õ��ϴ� id�� �������
// - accept_login: server�� front-end����. �õ��� id �״�� ������
// - logout: front-end�� server����. ������ ������ client id�� ����
// - hand_over: server�� front-end�� �ٸ� server����. �ٸ� ������ is_proxy�� �ٲٰ�, front-end�� ��� ���� ����(���� ���� disconnect �� ���ο� ������ connect)
// - forwarding_packet: ��ü ������ + ��� id + ���� ��Ŷ(size, type ����)

struct packet_header {
    char size;
    char type;
};

struct ss_packet_put {
    using type = char;
    static constexpr type type_num = 8;
    unsigned id;
    short x, y;
};

struct ss_packet_leave {
    using type = char;
    static constexpr type type_num = 9;
    unsigned id;
};

struct ss_packet_move {
    using type = char;
    static constexpr type type_num = 10;
    unsigned id;
    short x, y;
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
    // ������ ����, ����, ����, ���� ������, ĳ���� ����, �̸�, ���....
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
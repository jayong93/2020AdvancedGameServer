#define SEND_PACKET_IMPL
#include "player.h"
#include "packet.h"

extern std::array<Player*, client_limit> clients;

void send_login_ok_packet(int id)
{
	send_packet<sc_packet_login_ok>(id, clients, [id](sc_packet_login_ok& packet) {
		packet.id = id;
		packet.size = sizeof(packet);
		packet.type = SC_LOGIN_OK;
		packet.x = clients[id]->x;
		packet.y = clients[id]->y;
		packet.hp = 100;
		packet.level = 1;
		packet.exp = 1;
		}, false);
}

void send_login_fail(int id)
{
	send_packet<sc_packet_login_fail>(id, clients, [id](sc_packet_login_fail& packet) {
		packet.size = sizeof(packet);
		packet.type = SC_LOGIN_FAIL;
		}, false);
}

void send_put_object_packet(int client, int new_id, int x, int y)
{
	send_packet<sc_packet_put_object>(client, clients, [client, new_id, x, y](sc_packet_put_object& packet) {
		packet.id = new_id;
		packet.size = sizeof(packet);
		packet.type = SC_PUT_OBJECT;
		packet.x = x;
		packet.y = y;
		packet.o_type = 1;
		});
}

void send_pos_packet(int client, int mover, int x, int y)
{
	send_packet<sc_packet_pos>(client, clients, [client, mover, x, y](sc_packet_pos& packet) {
		packet.id = mover;
		packet.size = sizeof(packet);
		packet.type = SC_POS;
		packet.x = x;
		packet.y = y;
		// send는 client 와 같은 thread에서만 이루어 지므로 clients[client]에 접근해도 안전.
		packet.move_time = clients[client]->move_time;
		});
}

void send_remove_object_packet(int client, int leaver)
{
	send_packet<sc_packet_remove_object>(client, clients, [client, leaver](sc_packet_remove_object& packet) {
		packet.id = leaver;
		packet.size = sizeof(packet);
		packet.type = SC_REMOVE_OBJECT;
		});
}

void send_chat_packet(int client, int teller, char* mess)
{
	send_packet<sc_packet_chat>(client, clients, [client, teller, mess](sc_packet_chat& packet) {
		packet.id = teller;
		packet.size = sizeof(packet);
		packet.type = SC_CHAT;
		strcpy_s(packet.chat, mess);
		});
}


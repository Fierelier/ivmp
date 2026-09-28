#ifndef IVMP_RELAY_CLIENT_H
#define IVMP_RELAY_CLIENT_H

#include <winsock2.h>
#include <windows.h>

#include <stddef.h>
#include <stdint.h>

extern struct sockaddr_in ivmp_server_address;
extern uint32_t ivmp_connection_id;
extern uint64_t ivmp_session_token;
extern int ivmp_redirect_all;
extern int ivmp_control_ready;
extern volatile LONG ivmp_last_game_traffic;
extern volatile LONG ivmp_relay_lost;

void ivmp_relay_client_init(void (*logger)(const char *message), void (*demand)(DWORD wait_ms), int (*idle)(void));
int ivmp_relay_client_connect(const char *server_text, const char *nickname);
DWORD WINAPI ivmp_relay_heartbeat_thread(LPVOID parameter);

uint32_t ivmp_relay_peer_address(uint32_t connection_id);
void ivmp_relay_register_udp_socket(SOCKET socket);
void ivmp_relay_set_virtual_port(SOCKET socket, u_short port);
int ivmp_relay_virtual_port(SOCKET socket, u_short *port);
void ivmp_relay_publish_session(const unsigned char *record, size_t length);
int ivmp_relay_query_sessions(unsigned char *output, size_t capacity, size_t *length, DWORD timeout_ms);

int WSAAPI ivmp_hook_sendto(SOCKET socket, const char *buffer, int length, int flags, const struct sockaddr *address, int address_length);
int WSAAPI ivmp_hook_recvfrom(SOCKET socket, char *buffer, int length, int flags, struct sockaddr *address, int *address_length);
int WSAAPI ivmp_hook_closesocket(SOCKET socket);

#endif

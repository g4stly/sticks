#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>
#include <liburing.h>

#include <terezi.h>
#include "ioreq.h"
#include "util.h"

#define READ_SZ (8)
#define DF_PORT (7557)
#define QUEUE_DEPTH (256)

#define OP_CREATE_ROOM 		0
#define OP_JOIN_ROOM		1
#define OP_GAME_START		2
#define OP_STATE_PUSH		3
#define OP_GAME_END		4


// struct definitions
struct room {
	char code[5];
	int socket[2];
	int game[4];
};

struct server {
	tz_table *rooms;
};

struct start_game_msg {
	char *buffer;
	struct room *room;
};

// function definitions
int get_listener(int port)
{
	int sock;
	int status = 1;
	socklen_t i_sz = sizeof(int);
	struct sockaddr_in srv_addr;
	const struct sockaddr *addr;

	sock = socket(PF_INET, SOCK_STREAM, 0);
	if (sock < 0) die("get_listener(): socket():");

	status = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &status, i_sz);
	if (status < 0) die("get_listener(): setsockopt():");

	memset(&srv_addr, 0, sizeof(srv_addr));
	srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	srv_addr.sin_port = htons(port);
	srv_addr.sin_family = AF_INET;
	addr = (const struct sockaddr *)&srv_addr;

	status = bind(sock, addr, sizeof(srv_addr));
	if (status < 0) die("get_listener(): bind():");

	status = listen(sock, 10);
	if (status < 0) die("get_listener(): listen():");

	return sock;
}

struct room *server_room_init(struct server *s)
{
	struct room *r = malloc(sizeof(*r));
	if (!r) die("server_room_init(): malloc():");
	memset(r, 0, sizeof(*r));

	do for (int i = 0; i < 4; i++) {
		r->code[i] = random_int(9) + '0';
	} while (!tz_table_store(s->rooms, r->code, r));

	fprintf(stdout, "created room %s\n", r->code);
	
	return r;
}

struct room *server_get_room(struct server *s, const char *code)
{
	return tz_table_fetch(s->rooms, code);
}

void server_room_free(struct server *s, const char *code)
{
	struct room *r = tz_table_rm(s->rooms, code);
	if (!r) return;

	printf("freeing room %s!\n", code);

	for (int i = 0; i < 2; i++) {
		if (r->socket[i]) close(r->socket[i]);
	}

	free(r);
}


int room_add_conn(struct room *r, int socket)
{
	for (int i = 0; i < 2; i++) {
		if (r->socket[i]) continue;
		r->socket[i] = socket;
		return i;
	}
	return -1;
}

// global variables
struct io_uring *uring;		// used in ioreq
static struct server *server;	// used in our callbacks

// callbacks

void start_game(int rv, int socket, void *data)
{
	printf("we made it!\n");
	struct start_game_msg *msg = data;
	free(msg->buffer);
	server_room_free(server, msg->room->code);
}

void handle_first_recv(int rv, int socket, void *data)
{
	if (rv == 0) {
		fprintf(stderr, "closed!\n");
		close(socket);
		return;
	}

	struct room *room;
	struct start_game_msg *msg;

	char *buffer = data;
	if (buffer[0] >= '0') buffer[0] -= '0';

	switch (buffer[0]) {
	case OP_CREATE_ROOM:
		room = server_room_init(server);
		room_add_conn(room, socket);
		ioreq_send(socket, room->code, 5, NULL, NULL);
		break;
	case OP_JOIN_ROOM:
		buffer += 1;
		buffer[4] = '\0';
		room = server_get_room(server, buffer);
		if (!room || room_add_conn(room, socket) < 0) {
			fprintf(stderr, "bad room join: %s\n", buffer);
			break;
		}
		fprintf(stdout, "joining %s\n", buffer);

		msg = malloc(sizeof(*msg));
		if (!msg) die("handle_first_recv(): malloc():");
		memset(msg, 0, sizeof(*msg));

		msg->buffer = malloc(sizeof(char) * 3);
		if (!msg->buffer) die("handle_first_recv(): malloc():");
		memcpy(msg->buffer, "2\n", 3);

		ioreq_send(socket, msg->buffer, 3, msg, start_game);
		break;
	default:
		fprintf(stderr, "bad op: %i\n", buffer[0]);
		close(socket);
	}
	free(data);
}

void handle_accept(int client_socket, int socket, void *data)
{
	ioreq_accept(socket, NULL, handle_accept);

	char *buffer = malloc(sizeof(char) * (READ_SZ + 1));
	if (!buffer) die("handle_accept(): malloc():");
	memset(buffer, 0, sizeof(char) * (READ_SZ + 1));

	ioreq_recv(client_socket, buffer, READ_SZ, buffer, handle_first_recv);
}

int main(int argc, char **argv)
{
	srand(time(NULL));

	struct io_uring ring;
	struct io_uring_cqe *cqe;
	io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
	uring = &ring;

	struct server _server;
	_server.rooms = tz_table_init(NULL, NULL);
	server = &_server;

	int socket = get_listener(DF_PORT);
	fprintf(stdout, "listening on :%d\n", DF_PORT);

	ioreq_accept(socket, NULL, handle_accept);
	while (1) {
		int status = io_uring_wait_cqe(&ring, &cqe);
		if (status < 0) die("main(): io_uring_wait_cqe():");
		ioreq_handle_cqe(cqe);
		io_uring_cqe_seen(&ring, cqe);
	}

	close(socket);
	tz_table_free(_server.rooms);
	io_uring_queue_exit(&ring);

	return 0;
}

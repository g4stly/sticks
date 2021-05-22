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
#define OP_GAME_START		'2'
#define OP_STATE_PUSH		'3'
#define OP_GAME_END		4

#define ROOM_STATE_PENDING	0
#define ROOM_STATE_PLAYING	1
#define ROOM_STATE_STOPPED	2
#define ROOM_STATE_BROKEN	3


// struct definitions
struct room {
	int socket[2];
	char code[5];
	char wbuffer[READ_SZ + 1];
	char rbuffer0[READ_SZ + 1];
	char rbuffer1[READ_SZ + 1];
	char game[4];
	int state;
	int turn;
};

struct server {
	tz_table *rooms;
};

struct wait_all {
	int count;
	void *user_data;
	ioreq_cb callback;
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
	memset(r->game, 1, 4);


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
	printf("freeing room %s!\n", code);
	struct room *r = tz_table_rm(s->rooms, code);
	if (!r) return;
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

void pack_state(char *buffer, char active, char passive, char *game, int flip)
{
	buffer[0] = OP_STATE_PUSH;
	buffer[1] = active;
	buffer[2] = passive;
	buffer[3] = game[flip ? 2 : 0] + '0';
	buffer[4] = game[flip ? 3 : 1] + '0';
	buffer[5] = game[flip ? 0 : 2] + '0';
	buffer[6] = game[flip ? 1 : 3] + '0';
	buffer[7] = '\n';
}

static char rot_quad[] = { '3', '4', '1', '2' };


// global variables
struct io_uring *uring;		// used in ioreq
static struct server *server;	// used in our callbacks

// callbacks

void close_socket(int rv, int socket, void *data)
{
	close(socket);
}

void state_push(struct room *room)
{
	int socket;
	char *from_buf = room->turn
		? room->rbuffer1
		: room->rbuffer0;

	printf("got state %s\n", from_buf + 1);

	if (room->turn) {
		room->game[0] = room->rbuffer1[5] - '0';
		room->game[1] = room->rbuffer1[6] - '0';
		room->game[2] = room->rbuffer1[3] - '0';
		room->game[3] = room->rbuffer1[4] - '0';
	} else {
		room->game[0] = room->rbuffer0[3] - '0';
		room->game[1] = room->rbuffer0[4] - '0';
		room->game[2] = room->rbuffer0[5] - '0';
		room->game[3] = room->rbuffer0[6] - '0';
	}

	printf("persisted state %i%i%i%i\n",
		room->game[0], room->game[1],
		room->game[2], room->game[3]);

	pack_state(room->wbuffer,
		rot_quad[(from_buf[1] - '0') - 1],
		rot_quad[(from_buf[2] - '0') - 1],
		room->game, !room->turn);
	printf("sending state %s", room->wbuffer + 1);

	// switch turns
	room->turn = !room->turn;
	socket = room->socket[room->turn];
	ioreq_send(socket, room->wbuffer, READ_SZ, NULL, NULL);
}

void first_state_push(int rv, int socket, void *data)
{
	struct room *room = data;
	room->state = ROOM_STATE_PLAYING;
	socket = room->socket[room->turn];
	pack_state(room->wbuffer, '0', '0', room->game, 0);
	printf("first state: %s\n", room->wbuffer);
	ioreq_send(socket, room->wbuffer, READ_SZ, NULL, NULL);
}

void wait_all(int rv, int socket, void *data)
{
	struct wait_all *wait_for = data;
	if (--wait_for->count == 0) {
		wait_for->callback(rv, socket, wait_for->user_data);
		free(wait_for);
	}
}

void start_game(int rv, int socket, void *data)
{
	struct room *room = data;
	struct wait_all *wait_for = malloc(sizeof(*wait_for));
	if (!wait_for) die("start_game(): malloc():");
	memset(wait_for, 0, sizeof(*wait_for));

	wait_for->count = 2;
	wait_for->user_data = room;
	wait_for->callback = first_state_push;

	room->wbuffer[0] = OP_GAME_START;
	room->wbuffer[1] = '\n';

	for (int i = 0; i < 2; i++) {
		socket = room->socket[i];
		ioreq_send(socket, room->wbuffer, 2, wait_for, wait_all);
	}
}

void handle_recv(int rv, int socket, void *data)
{
	struct room *room = data;
	if (room->state == ROOM_STATE_BROKEN) {
		server_room_free(server, room->code);
		close(socket);
		return;
	}
	if (rv <= 0) {
		close(socket);
		if (room->state == ROOM_STATE_PENDING) {
			server_room_free(server, room->code);
			return;
		}
		room->state = ROOM_STATE_BROKEN;
		socket = room->socket[0] == socket
			? room->socket[1]
			: room->socket[0];
		ioreq_send(socket, "-2\n", 3, NULL, NULL);
		return;
	}

	char *buffer = room->socket[0] == socket 
		? room->rbuffer0
		: room->rbuffer1;

	switch (room->state) {
	case ROOM_STATE_PENDING:
		// silently ignore their write
		break;
	case ROOM_STATE_PLAYING:
		// silently ignore their write if it's not their turn
		if (room->socket[room->turn] != socket) break;
		state_push(room);
		break;
	}
	ioreq_recv(socket, buffer, READ_SZ, room, handle_recv);
}

void handle_first_recv(int rv, int socket, void *data)
{
	if (rv == 0) {
		fprintf(stderr, "closed on first recv?!\n");
		close(socket);
		return;
	}

	struct room *room;
	char *buffer = data;
	if (buffer[0] >= '0') buffer[0] -= '0';

	switch (buffer[0]) {
	case OP_CREATE_ROOM:
		room = server_room_init(server);
		room_add_conn(room, socket);
		memcpy(room->wbuffer, room->code, 4);
		room->wbuffer[4] = '\n';
		ioreq_send(socket, room->wbuffer, 5, NULL, NULL);
		ioreq_recv(socket, room->rbuffer0, READ_SZ, room, handle_recv);
		break;
	case OP_JOIN_ROOM:
		buffer += 1;
		buffer[4] = '\0';
		room = server_get_room(server, buffer);
		if (!room || room_add_conn(room, socket) < 0) {
			fprintf(stderr, "bad room join: %s\n", buffer);
			ioreq_send(socket, "-1\n", 3, NULL, close_socket);
			break;
		}
		ioreq_send(socket, "0\n", 2, room, start_game);
		ioreq_recv(socket, room->rbuffer1, READ_SZ, room, handle_recv);
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

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

struct io_uring *uring;

struct room {
	char code[5];
	int socket[2];
	int game[4];
};

struct server {
	int socket;
	struct io_uring ring;
	tz_table *rooms;
};

struct req_context {
	int socket;
	int free_iov;
	int event_type;
	struct room *room;
	int iov_len;
	struct iovec iov[];
};

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

void add_accept_req(
	struct io_uring *ring,
	int listener,
	struct sockaddr_in *client_addr,
	socklen_t *addr_sz)
{
	struct sockaddr *addr = (struct sockaddr *)client_addr;

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	io_uring_prep_accept(sqe, listener, addr, addr_sz, 0);

	struct req_context *context = malloc(sizeof(*context));
	context->event_type = EVENT_TYPE_ACCEPT;
	io_uring_sqe_set_data(sqe, context);
}

void add_read_req(struct io_uring *ring, int socket, int event_type)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	struct req_context *ctx = malloc(sizeof(*ctx) + sizeof(struct iovec));
	memset(ctx, 0, sizeof(*ctx));

	ctx->free_iov = 1;
	ctx->socket = socket;
	ctx->event_type = event_type;
	ctx->iov[0].iov_len = READ_SZ;
	ctx->iov[0].iov_base = malloc(READ_SZ);
	io_uring_prep_readv(sqe, socket, &ctx->iov[0], 1, 0);
	io_uring_sqe_set_data(sqe, ctx);
}

void add_write_req(struct io_uring *ring, struct req_context *ctx)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	io_uring_prep_writev(sqe, ctx->socket, ctx->iov, ctx->iov_len, 0);
	io_uring_sqe_set_data(sqe, ctx);
}

struct room *room_init(struct server *s)
{
	struct room *r = malloc(sizeof(*r));
	memset(r, 0, sizeof(*r));
	do for (int i = 0; i < 4; i++) {
		r->code[i] = random_int(9) + '0';
	} while (!tz_table_store(s->rooms, r->code, r));
	printf("created room %s\n", r->code);
	return r;
}

void room_add_conn(struct room *r, int position, int socket)
{
	r->socket[position] = socket;
}

void room_free(void *data)
{
	struct connection *c;
	struct room *r = (struct room *)data;
	printf("freeing room %s!\n", r->code);

	for (int i = 0; i < 2; i++) {
		if (r->socket[i]) close(r->socket[i]);
	}

	free(r);
}

void handle_conn(struct server *s, struct req_context *ctx)
{
	struct room *room;
	struct req_context *wctx;
	char *buffer = ctx->iov[0].iov_base;

	buffer[0] = buffer[0] - '0';

	switch (buffer[0]) {
	case OP_CREATE_ROOM:
		room = room_init(s);
		room_add_conn(room, 0, ctx->socket);

		wctx = malloc(sizeof(*wctx) + sizeof(struct iovec));
		memset(wctx, 0, sizeof(*wctx));
		wctx->event_type = EVENT_TYPE_WRITE_INIT;
		wctx->socket = ctx->socket;
		wctx->room = room;

		wctx->iov[0].iov_base = room->code;
		wctx->iov[0].iov_len = 5;
		wctx->iov_len = 1;

		add_write_req(&s->ring, wctx);
		break;
	case OP_JOIN_ROOM:
		buffer += 1;
		buffer[4] = '\0';
		printf("fetching %s\n", buffer);
		room = tz_table_fetch(s->rooms, buffer);
		if (!room || room->socket[1]) {
			fprintf(stderr, "bad room join: %s\n", buffer);
			wctx = malloc(sizeof(*wctx) + sizeof(struct iovec));
			wctx->event_type = EVENT_TYPE_WRITE_BADJOIN;
			wctx->socket = ctx->socket;

			wctx->free_iov = 1;
			wctx->iov[0].iov_base = malloc(sizeof(char) * 3);
			memcpy(wctx->iov[0].iov_base, "-1", 3);
			wctx->iov[0].iov_len = 3;
			wctx->iov_len = 1;

			add_write_req(&s->ring, wctx);
			break;
		}

		room_add_conn(room, 1, ctx->socket);
		wctx = malloc(sizeof(*wctx) + sizeof(struct iovec));
		wctx->event_type = EVENT_TYPE_WRITE_JOIN;
		wctx->socket = ctx->socket;
		wctx->room = room;

		wctx->free_iov = 1;
		wctx->iov[0].iov_base = malloc(sizeof(char) * 2);
		memcpy(wctx->iov[0].iov_base, "2", 2);
		wctx->iov[0].iov_len = 2;
		wctx->iov_len = 1;

		add_write_req(&s->ring, wctx);
		break;
	/*
	case OP_GAME_START:
		break;
	case OP_STATE_PUSH:
		break;
	case OP_GAME_END:
		break;
	*/
	default:
		fprintf(stderr, "handle_conn(): bad op: %d\n", buffer[0]);
		close(ctx->socket);
	}
}

void handle_io_event(
	struct server *s,
	struct io_uring_cqe *cqe,
	struct sockaddr_in *client_addr,
	socklen_t *addr_sz)
{
	struct room *room;
	struct req_context *wctx;
	struct req_context *context = (struct req_context *)cqe->user_data;
	if (cqe->res < 0) {
		die("handle_io_event(): event type %d:", context->event_type);
	}

	switch (context->event_type) {
	case EVENT_TYPE_ACCEPT:
		add_accept_req(
			&s->ring, 
			s->socket, 
			client_addr, 
			addr_sz);
		add_read_req(
			&s->ring, 
			cqe->res, 
			EVENT_TYPE_READ_INIT);
		break;
	case EVENT_TYPE_READ_INIT:
		if (!cqe->res) {
			close(context->socket);
			break;
		}
		handle_conn(s, context);
		break;
	case EVENT_TYPE_WRITE_INIT:
		break;
	case EVENT_TYPE_WRITE_BADJOIN:
		close(context->socket);
		break;
	case EVENT_TYPE_WRITE_JOIN:
		room = context->room;
		wctx = malloc(sizeof(*wctx) + sizeof(struct iovec));
		memset(wctx, 0, sizeof(*wctx));
		wctx->event_type = EVENT_TYPE_WRITE_GAMESTART;

		wctx->free_iov = 1;
		wctx->iov[0].iov_base = malloc(sizeof(char) * 11);
		memcpy(wctx->iov[0].iov_base, "gamestart\n", 11);
		wctx->iov[0].iov_len = 11;
		wctx->iov_len = 1;

		wctx->socket = room->socket[0];
		add_write_req(&s->ring, wctx);

		wctx->socket = room->socket[1];
		add_write_req(&s->ring, wctx);
		break;
	}

	if (context->free_iov) {
		for (int i = 0; i < context->iov_len; i++) {
			free(context->iov[i].iov_base);
		}
	}
	if (context) free(context);
	io_uring_cqe_seen(&s->ring, cqe);
}

void server_loop(struct server *s)
{
	int status;
	struct io_uring_cqe *cqe;
	//struct __kernel_timespec timeout;
	struct sockaddr_in client_addr;
	socklen_t addr_sz = sizeof(client_addr);

	//memset(&timeout, 0, sizeof(timeout));
	//timeout.tv_nsec = 10;

	add_accept_req(&s->ring, s->socket, &client_addr, &addr_sz);

	while (1) {
		io_uring_submit(&s->ring);
		status = io_uring_wait_cqe(&s->ring, &cqe);
		if (status < 0) {
			die("accept_loop(): io_uring_wait_cqe():");
		}
		handle_io_event(s, cqe, &client_addr, &addr_sz);
	}
}

int main(int argc, char **argv)
{
	srand(time(NULL));

	struct io_uring ring;
	io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
	uring = &ring;

	struct server s;
	s.rooms = tz_table_init(NULL, NULL);

	s.socket = get_listener(DF_PORT);
	printf("listening on %d\n", DF_PORT);
	close(s.socket);

	tz_table_free(s.rooms);
	io_uring_queue_exit(&ring);

	return 0;
}

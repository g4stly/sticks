#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <liburing.h>

#include "ioreq.h"
#include "util.h"

#define IOREQ_TYPE_ACCEPT	0
#define IOREQ_TYPE_RECV		1
#define IOREQ_TYPE_SEND		2

extern struct io_uring *uring;

struct ioreq {
	int socket;
	int ioreq_type;
	void *user_data;
	ioreq_cb callback;
	struct sockaddr_in client_addr;
	socklen_t addr_sz;
};

void ioreq_accept(int socket, void *data, ioreq_cb cb)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(uring);
	if (!sqe) die("ioreq_accept(): io_uring_get_sqe():");

	struct ioreq *r = malloc(sizeof(*r));
	if (!r) die("ioreq_accept(): malloc():");
	memset(r, 0, sizeof(*r));

	r->addr_sz = sizeof(r->client_addr);
	r->ioreq_type = IOREQ_TYPE_ACCEPT;
	r->user_data = data;
	r->socket = socket;
	r->callback = cb;

	struct sockaddr *addr = (struct sockaddr *)&r->client_addr;
	io_uring_prep_accept(sqe, socket, addr, &r->addr_sz, 0);
	io_uring_sqe_set_data(sqe, r);
	io_uring_submit(uring);
}

void ioreq_recv(int socket, void *buf, size_t len, void *data, ioreq_cb cb)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(uring);
	if (!sqe) die("ioreq_recv(): io_uring_get_sqe():");

	struct ioreq *r = malloc(sizeof(*r));
	if (!r) die("ioreq_recv(): malloc():");
	memset(r, 0, sizeof(*r));

	r->ioreq_type = IOREQ_TYPE_RECV;
	r->user_data = data;
	r->socket = socket;
	r->callback = cb;

	io_uring_prep_recv(sqe, socket, buf, len, 0);
	io_uring_sqe_set_data(sqe, r);
	io_uring_submit(uring);
}

void ioreq_send(int socket, const void *buf, size_t len, void *data, ioreq_cb cb)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(uring);
	if (!sqe) die("ioreq_send(): io_uring_get_sqe():");

	struct ioreq *r = malloc(sizeof(*r));
	if (!r) die("ioreq_send(): malloc():");
	memset(r, 0, sizeof(*r));

	r->ioreq_type = IOREQ_TYPE_SEND;
	r->user_data = data;
	r->socket = socket;
	r->callback = cb;

	io_uring_prep_send(sqe, socket, buf, len, 0);
	io_uring_sqe_set_data(sqe, r);
	io_uring_submit(uring);
}

void ioreq_handle_cqe(struct io_uring_cqe *cqe)
{
	struct ioreq *r = (struct ioreq *)cqe->user_data;
	if (r->callback) {
		r->callback(cqe->res, r->socket, r->user_data);
	}
	free(r);
}

#undef IOREQ_TYPE_ACCEPT
#undef IOREQ_TYPE_RECV
#undef IOREQ_TYPE_SEND

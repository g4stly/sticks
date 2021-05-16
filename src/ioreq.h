#ifndef iorequest_h_
#define iorequest_h_

#include <liburing.h>

typedef void (*ioreq_cb)(int rv, int socket, void *);

void ioreq_accept(int socket, void *data, ioreq_cb cb);
void ioreq_recv(int socket, void *buf, size_t len, void *data, ioreq_cb cb);
void ioreq_send(int socket, const void *buf, size_t len, void *data, ioreq_cb cb);

int ioreq_handle_cqe(struct io_uring_cqe *cqe);

#endif

/*
 *  gensio - A library for abstracting stream I/O
 *  Copyright (C) 2018  Corey Minyard <minyard@acm.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */

/* This code handles UDP network I/O. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <assert.h>

#include <gensio/gensio.h>
#include <gensio/gensio_class.h>
#include <gensio/argvutils.h>

/*
 * Maximum UDP packet size, this avoids partial packet reads.  Probably
 * not a good idea to override this.
 */
#define GENSIO_DEFAULT_UDP_BUF_SIZE	65536

struct udpna_data;

enum udpn_state {
    UDPN_CLOSED = 0,
    UDPN_IN_OPEN,
    UDPN_OPEN,
    UDPN_IN_CLOSE
};

struct udpn_data {
    struct gensio *io;
    struct udpna_data *nadata;

    struct gensio_os_funcs *o;

    unsigned int refcount;

    int myfd; /* fd the original request came in on, for sending. */

    bool read_enabled;	/* Read callbacks are enabled. */
    bool write_enabled;	/* Write callbacks are enabled. */
    bool in_read;	/* Currently in a read callback. */
    bool in_write;	/* Currently in a write callback. */
    bool in_open_cb;	/* Currently in an open callback. */
    bool in_close_cb;	/* Currently in a close callback. */

    enum udpn_state state;
    bool freed;		/* Freed during the close process. */

    gensio_done_err open_done;
    void *open_data;

    gensio_done close_done;
    void *close_data;

    bool deferred_op_pending;
    struct gensio_runner *deferred_op_runner;	/* NULL if not a client. */

    struct sockaddr_storage remote;	/* The socket address of who
					   is connected to this port. */
    struct sockaddr *raddr;		/* Points to remote, for convenience. */
    socklen_t raddrlen;

    struct gensio_link link;
};

#define gensio_link_to_ndata(l) \
    gensio_container_of(l, struct udpn_data, link);

struct udpna_data {
    struct gensio_accepter *acc;
    struct gensio_list udpns;
    unsigned int udpn_count;

    struct gensio_os_funcs *o;

    struct gensio_lock *lock;

    gensiods max_read_size;

    unsigned char *read_data;

    gensiods data_pending_len;
    gensiods data_pos;
    struct udpn_data *pending_data_owner;

    struct gensio_list closed_udpns;

    /*
     * Used to run read callbacks from the selector to avoid running
     * it directly from user calls.
     */
    bool deferred_op_pending;
    struct gensio_runner *deferred_op_runner;

    bool in_new_connection;

    bool setup;
    bool enabled;
    bool closed;
    bool in_shutdown;
    bool disabled;
    gensio_acc_done shutdown_done;
    void *shutdown_data;

    struct addrinfo    *ai;		/* The address list for the portname. */
    struct opensocks   *fds;		/* The file descriptor used for
					   the UDP ports. */
    unsigned int   nr_fds;
    unsigned int   nr_accept_close_waiting;

    bool in_write;
    unsigned int read_disable_count;
    bool read_disabled;
    unsigned int write_enable_count;
};

static void
udpna_lock(struct udpna_data *nadata)
{
    nadata->o->lock(nadata->lock);
}

static void
udpna_unlock(struct udpna_data *nadata)
{
    nadata->o->unlock(nadata->lock);
}

static void udpna_deferred_op(struct gensio_runner *runner, void *cbdata);

static void udpna_start_deferred_op(struct udpna_data *nadata)
{
    if (!nadata->deferred_op_pending) {
	nadata->deferred_op_pending = true;
	nadata->o->run(nadata->deferred_op_runner);
    }
}

static void
udpn_remove_from_list(struct gensio_list *list, struct udpn_data *ndata)
{
    gensio_list_rm(list, &ndata->link);
}

static struct udpn_data *
udpn_find(struct gensio_list *list, struct sockaddr *addr, socklen_t addrlen)
{
    struct gensio_link *l;

    gensio_list_for_each(list, l) {
	struct udpn_data *ndata = gensio_link_to_ndata(l);

	if (gensio_sockaddr_equal(ndata->raddr, ndata->raddrlen,
				  (struct sockaddr *) addr, addrlen, true))
	    return ndata;
    }

    return NULL;
}

static void udpn_add_to_list(struct gensio_list *list, struct udpn_data *ndata)
{
    gensio_list_add_tail(list, &ndata->link);
}

static void
udpna_enable_read(struct udpna_data *nadata)
{
    unsigned int i;

    nadata->read_disabled = false;
    for (i = 0; i < nadata->nr_fds; i++)
	nadata->o->set_read_handler(nadata->o, nadata->fds[i].fd, true);
}

static void
udpna_disable_read(struct udpna_data *nadata)
{
    unsigned int i;

    nadata->read_disabled = true;
    for (i = 0; i < nadata->nr_fds; i++)
	nadata->o->set_read_handler(nadata->o, nadata->fds[i].fd, false);
}

static void udpna_check_read_state(struct udpna_data *nadata)
{
    if (nadata->read_disabled && nadata->read_disable_count == 0)
	udpna_enable_read(nadata);
    else if (!nadata->read_disabled && nadata->read_disable_count > 0)
	udpna_disable_read(nadata);
}

static void
udpna_fd_read_enable(struct udpna_data *nadata)
{
    assert(nadata->read_disable_count > 0);
    nadata->read_disable_count--;
    udpna_check_read_state(nadata);
}

static void
udpna_fd_read_disable(struct udpna_data *nadata)
{
    nadata->read_disable_count++;
    udpna_check_read_state(nadata);
}

static void
udpna_disable_write(struct udpna_data *nadata)
{
    unsigned int i;

    for (i = 0; i < nadata->nr_fds; i++)
	nadata->o->set_write_handler(nadata->o, nadata->fds[i].fd, false);
}

static void
udpna_fd_write_disable(struct udpna_data *nadata)
{
    assert(nadata->write_enable_count > 0);
    nadata->write_enable_count--;
    if (nadata->write_enable_count == 0 && !nadata->in_write)
	udpna_disable_write(nadata);
}

static void
udpna_enable_write(struct udpna_data *nadata)
{
    unsigned int i;

    for (i = 0; i < nadata->nr_fds; i++)
	nadata->o->set_write_handler(nadata->o, nadata->fds[i].fd, true);
}

static void
udpna_fd_write_enable(struct udpna_data *nadata)
{
    if (nadata->write_enable_count == 0 && !nadata->in_write)
	udpna_enable_write(nadata);
    nadata->write_enable_count++;
}

static void udpna_do_free(struct udpna_data *nadata)
{
    unsigned int i;

    for (i = 0; i < nadata->nr_fds; i++)
	close(nadata->fds[i].fd);

    if (nadata->deferred_op_runner)
	nadata->o->free_runner(nadata->deferred_op_runner);
    if (nadata->ai)
	gensio_free_addrinfo(nadata->o, nadata->ai);
    if (nadata->fds)
	nadata->o->free(nadata->o, nadata->fds);
    if (nadata->read_data)
	nadata->o->free(nadata->o, nadata->read_data);
    if (nadata->lock)
	nadata->o->free_lock(nadata->lock);
    if (nadata->acc)
	gensio_acc_data_free(nadata->acc);
    nadata->o->free(nadata->o, nadata);
}

static void udpna_fd_cleared(int fd, void *cbdata)
{
    struct udpna_data *nadata = cbdata;

    udpna_lock(nadata);
    if (--nadata->nr_accept_close_waiting == 0) {
	if (!nadata->deferred_op_pending) {
	    udpna_unlock(nadata);
	    udpna_do_free(nadata);
	    return;
	}
    }
    udpna_unlock(nadata);
}

static void udpna_check_finish_free(struct udpna_data *nadata)
{
    unsigned int i;

    if (!nadata->closed || nadata->in_new_connection || nadata->udpn_count ||
		nadata->in_shutdown)
	return;

    for (i = 0; i < nadata->nr_fds; i++)
	nadata->o->clear_fd_handlers(nadata->o, nadata->fds[i].fd);
}

static void
udpn_do_free(struct udpn_data *ndata)
{
    if (ndata->deferred_op_runner)
	ndata->o->free_runner(ndata->deferred_op_runner);
    if (ndata->io)
	gensio_data_free(ndata->io);
    ndata->o->free(ndata->o, ndata);
}

static void
udpn_finish_free(struct udpn_data *ndata)
{
    struct udpna_data *nadata = ndata->nadata;

    udpn_remove_from_list(&nadata->closed_udpns, ndata);
    assert(nadata->udpn_count > 0);
    nadata->udpn_count--;
    udpn_do_free(ndata);
    udpna_check_finish_free(nadata);
}

static int
udpn_write(struct gensio *io, gensiods *count,
	   const void *buf, gensiods buflen)
{
    struct udpn_data *ndata = gensio_get_gensio_data(io);
    int rv, err = 0;

 retry:
    rv = sendto(ndata->myfd, buf, buflen, 0, ndata->raddr, ndata->raddrlen);
    if (rv < 0) {
	if (errno == EINTR)
	    goto retry;
	if (errno == EWOULDBLOCK || errno == EAGAIN)
	    rv = 0; /* Handle like a zero-byte write. */
	else
	    err = errno;
    } else if (rv == 0) {
	err = EPIPE;
    }

    if (!err && count)
	*count = rv;

    return err;
}

static int
udpn_raddr_to_str(struct gensio *io, gensiods *epos,
		  char *buf, gensiods buflen)
{
    struct udpn_data *ndata = gensio_get_gensio_data(io);
    socklen_t addrlen = ndata->raddrlen;

    return gensio_sockaddr_to_str(ndata->raddr, &addrlen, buf, epos, buflen);
}

static int
udpn_get_raddr(struct gensio *io, void *addr, gensiods *addrlen)
{
    struct udpn_data *ndata = gensio_get_gensio_data(io);

    if (*addrlen > ndata->raddrlen)
	*addrlen = ndata->raddrlen;

    memcpy(addr, ndata->raddr, *addrlen);
    return 0;
}

static void
udpn_finish_close(struct udpna_data *nadata, struct udpn_data *ndata)
{
    if (ndata->in_read || ndata->in_write || ndata->in_open_cb)
	return;

    ndata->state = UDPN_CLOSED;

    if (ndata->close_done) {
	void (*close_done)(struct gensio *io, void *close_data) =
	    ndata->close_done;
	void *close_data = ndata->close_data;

	ndata->close_done = NULL;
	ndata->in_close_cb = true;
	udpna_unlock(nadata);
	close_done(ndata->io, close_data);
	udpna_lock(nadata);
	ndata->in_close_cb = false;
    }

    if (nadata->pending_data_owner == ndata) {
	nadata->pending_data_owner = NULL;
	nadata->data_pending_len = 0;
    }

    if (ndata->freed)
	udpn_finish_free(ndata);
}

static void
udpn_finish_read(struct udpn_data *ndata)
{
    struct udpna_data *nadata = ndata->nadata;
    struct gensio *io = ndata->io;
    gensiods count;

 retry:
    udpna_unlock(nadata);
    count = nadata->data_pending_len;
    gensio_cb(io, GENSIO_EVENT_READ, 0, nadata->read_data, &count, NULL);
    udpna_lock(nadata);

    if (ndata->state == UDPN_IN_CLOSE) {
	udpn_finish_close(nadata, ndata);
	goto out;
    }

    if (count < nadata->data_pending_len) {
	/* The user didn't comsume all the data */
	nadata->data_pending_len -= count;
	nadata->data_pos += count;
	if (ndata->state == UDPN_OPEN && ndata->read_enabled)
	    goto retry;
    } else {
	nadata->pending_data_owner = NULL;
	nadata->data_pending_len = 0;
    }
 out:
    ndata->in_read = false;
    udpna_check_read_state(nadata);
}

static void
udpna_deferred_op(struct gensio_runner *runner, void *cbdata)
{
    struct udpna_data *nadata = cbdata;

    udpna_lock(nadata);

    while (nadata->pending_data_owner &&
			nadata->pending_data_owner->read_enabled)
	udpn_finish_read(nadata->pending_data_owner);

    if (nadata->in_shutdown && !nadata->in_new_connection) {
	struct gensio_accepter *accepter = nadata->acc;

	if (nadata->shutdown_done) {
	    udpna_unlock(nadata);
	    nadata->shutdown_done(accepter, nadata->shutdown_data);
	    udpna_lock(nadata);
	}
	nadata->in_shutdown = false;
	udpna_check_finish_free(nadata);
    }

    if (nadata->closed && nadata->nr_accept_close_waiting == 0) {
	udpna_unlock(nadata);
	udpna_do_free(nadata);
    } else {
	nadata->deferred_op_pending = false;
	udpna_check_read_state(nadata);
	udpna_unlock(nadata);
    }
}

static void
udpn_deferred_op(struct gensio_runner *runner, void *cbdata)
{
    struct udpn_data *ndata = cbdata;
    struct udpna_data *nadata = ndata->nadata;

    udpna_lock(nadata);
    ndata->deferred_op_pending = false;
    if (ndata->state == UDPN_IN_OPEN) {
	ndata->state = UDPN_OPEN;
	if (ndata->open_done) {
	    ndata->in_open_cb = true;
	    udpna_unlock(nadata);
	    ndata->open_done(ndata->io, 0, ndata->open_data);
	    udpna_lock(nadata);
	    ndata->in_open_cb = false;
	}
	udpna_check_read_state(nadata);
    }

    if (ndata->state == UDPN_IN_CLOSE) {
	udpn_finish_close(nadata, ndata);
    }

    udpna_unlock(nadata);
}

static void udpn_start_deferred_op(struct udpn_data *ndata)
{
    if (!ndata->deferred_op_pending) {
	ndata->deferred_op_pending = true;
	ndata->o->run(ndata->deferred_op_runner);
    }
}

static int
udpn_open(struct gensio *io, gensio_done_err open_done, void *open_data)
{
    struct udpn_data *ndata = gensio_get_gensio_data(io);
    struct udpna_data *nadata = ndata->nadata;
    int err = EBUSY;

    udpna_lock(nadata);
    if (!gensio_is_client(ndata->io)) {
	err = ENOTTY;
    } else if (ndata->state == UDPN_CLOSED) {
	udpn_remove_from_list(&nadata->closed_udpns, ndata);
	udpn_add_to_list(&nadata->udpns, ndata);
	udpna_fd_read_disable(nadata);
	ndata->state = UDPN_IN_OPEN;
	ndata->open_done = open_done;
	ndata->open_data = open_data;
	udpn_start_deferred_op(ndata);
	err = 0;
    }
    udpna_unlock(nadata);

    return err;
}

static void
udpn_start_close(struct udpn_data *ndata,
		 gensio_done close_done, void *close_data)
{
    struct udpna_data *nadata = ndata->nadata;

    if (nadata->pending_data_owner == ndata) {
	nadata->pending_data_owner = NULL;
	nadata->data_pending_len = 0;
    }
    ndata->close_done = close_done;
    ndata->close_data = close_data;

    if (ndata->read_enabled)
	ndata->read_enabled = false;
    else
	udpna_fd_read_enable(nadata);

    if (ndata->write_enabled) {
	ndata->write_enabled = false;
	udpna_fd_write_disable(nadata);
    }

    udpn_remove_from_list(&nadata->udpns, ndata);
    udpn_add_to_list(&nadata->closed_udpns, ndata);
    ndata->state = UDPN_IN_CLOSE;

    udpn_start_deferred_op(ndata);
}

static bool
udpn_is_closed(struct udpn_data *ndata)
{
    return (ndata->state == UDPN_CLOSED || ndata->state == UDPN_IN_CLOSE);
}

static int
udpn_close(struct gensio *io, gensio_done close_done, void *close_data)
{
    struct udpn_data *ndata = gensio_get_gensio_data(io);
    struct udpna_data *nadata = ndata->nadata;
    int err = EBUSY;

    udpna_lock(nadata);
    if (!udpn_is_closed(ndata)) {
	udpn_start_close(ndata, close_done, close_data);
	err = 0;
    }
    udpna_unlock(nadata);

    return err;
}

static void
udpn_free(struct gensio *io)
{
    struct udpn_data *ndata = gensio_get_gensio_data(io);
    struct udpna_data *nadata = ndata->nadata;

    udpna_lock(nadata);
    assert(ndata->refcount > 0);
    if (--ndata->refcount > 0)
	goto out_unlock;

    ndata->freed = true;
    if (ndata->state == UDPN_IN_CLOSE)
	ndata->close_done = NULL;
    else if (ndata->state != UDPN_CLOSED)
	udpn_start_close(ndata, NULL, NULL);
    else if (!ndata->in_close_cb)
	udpn_finish_free(ndata);
 out_unlock:
    udpna_unlock(nadata);
}

static void
udpn_ref(struct gensio *io)
{
    struct udpn_data *ndata = gensio_get_gensio_data(io);
    struct udpna_data *nadata = ndata->nadata;

    udpna_lock(nadata);
    ndata->refcount++;
    udpna_unlock(nadata);
}

static void
udpn_set_read_callback_enable(struct gensio *io, bool enabled)
{
    struct udpn_data *ndata = gensio_get_gensio_data(io);
    struct udpna_data *nadata = ndata->nadata;
    bool my_data_pending;

    udpna_lock(nadata);
    if (udpn_is_closed(ndata) || ndata->read_enabled == enabled)
	goto out_unlock;

    if (enabled) {
	assert(nadata->read_disable_count > 0);
	nadata->read_disable_count--;
    } else {
	nadata->read_disable_count++;
    }
    ndata->read_enabled = enabled;
    my_data_pending = (nadata->data_pending_len &&
		       nadata->pending_data_owner == ndata);
    if (ndata->in_read || ndata->state == UDPN_IN_OPEN ||
		(my_data_pending && !enabled)) {
	/* Nothing to do. */
    } else if (enabled && my_data_pending) {
	ndata->in_read = true;
	/* Call the read from the selector to avoid lock nesting issues. */
	udpna_start_deferred_op(nadata);
    } else {
	udpna_check_read_state(ndata->nadata);
    }
 out_unlock:
    udpna_unlock(nadata);
}

static void
udpn_set_write_callback_enable(struct gensio *io, bool enabled)
{
    struct udpn_data *ndata = gensio_get_gensio_data(io);
    struct udpna_data *nadata = ndata->nadata;

    udpna_lock(nadata);
    if (udpn_is_closed(ndata))
	goto out_unlock;
    if (ndata->write_enabled != enabled) {
	ndata->write_enabled = enabled;
	if (ndata->state == UDPN_IN_OPEN)
	    goto out_unlock;
	if (enabled)
	    udpna_fd_write_enable(ndata->nadata);
	else
	    udpna_fd_write_disable(ndata->nadata);
    }
 out_unlock:
    udpna_unlock(nadata);
}

static void
udpn_disable(struct gensio *io)
{
    struct udpn_data *ndata = gensio_get_gensio_data(io);
    struct udpna_data *nadata = ndata->nadata;

    if (ndata->read_enabled) {
	udpna_fd_read_disable(nadata);
	ndata->read_enabled = false;
    }

    if (ndata->write_enabled) {
	udpna_fd_write_disable(nadata);
	ndata->write_enabled = false;
    }

    ndata->close_done = NULL;
    udpn_remove_from_list(&nadata->udpns, ndata);
    udpn_add_to_list(&nadata->closed_udpns, ndata);
    ndata->state = UDPN_CLOSED;
    nadata->disabled = true;
}

static void
udpn_handle_write_incoming(struct udpna_data *nadata, struct udpn_data *ndata)
{
    struct gensio *io = ndata->io;

    ndata->in_write = true;
    udpna_unlock(nadata);
    gensio_cb(io, GENSIO_EVENT_WRITE_READY, 0, NULL, NULL, NULL);
    udpna_lock(nadata);
    ndata->in_write = false;

    if (ndata->state == UDPN_IN_CLOSE)
	udpn_finish_close(nadata, ndata);
}

static void
udpna_writehandler(int fd, void *cbdata)
{
    struct udpna_data *nadata = cbdata;
    struct gensio_link *l;

    udpna_lock(nadata);
    if (nadata->in_write)
	goto out_unlock;

    udpna_disable_write(nadata);
    gensio_list_for_each(&nadata->udpns, l) {
	struct udpn_data *ndata = gensio_link_to_ndata(l);

	if (ndata->write_enabled) {
	    udpn_handle_write_incoming(nadata, ndata);
	    /*
	     * Only handle one per callback, the above call releases
	     * the lock and can result in the list changing.
	     */
	    break;
	}
    }
    if (nadata->write_enable_count > 0)
	udpna_enable_write(nadata);
 out_unlock:
    udpna_unlock(nadata);
}

static int
gensio_udp_func(struct gensio *io, int func, gensiods *count,
		const void *cbuf, gensiods buflen, void *buf,
		const char *const *auxdata)
{
    switch (func) {
    case GENSIO_FUNC_WRITE:
	return udpn_write(io, count, cbuf, buflen);

    case GENSIO_FUNC_RADDR_TO_STR:
	return udpn_raddr_to_str(io, count, buf, buflen);

    case GENSIO_FUNC_GET_RADDR:
	return udpn_get_raddr(io, buf, count);

    case GENSIO_FUNC_OPEN:
	return udpn_open(io, cbuf, buf);

    case GENSIO_FUNC_CLOSE:
	return udpn_close(io, cbuf, buf);

    case GENSIO_FUNC_FREE:
	udpn_free(io);
	return 0;

    case GENSIO_FUNC_REF:
	udpn_ref(io);
	return 0;

    case GENSIO_FUNC_SET_READ_CALLBACK:
	udpn_set_read_callback_enable(io, buflen);
	return 0;

    case GENSIO_FUNC_SET_WRITE_CALLBACK:
	udpn_set_write_callback_enable(io, buflen);
	return 0;

    case GENSIO_FUNC_DISABLE:
	udpn_disable(io);
	return 0;

    case GENSIO_FUNC_REMOTE_ID:
    default:
	return ENOTSUP;
    }
}

static struct udpn_data *
udp_alloc_gensio(struct udpna_data *nadata, int fd, struct sockaddr *addr,
		 socklen_t addrlen, gensio_event cb, void *user_data,
		 struct gensio_list *starting_list)
{
    struct udpn_data *ndata = nadata->o->zalloc(nadata->o, sizeof(*ndata));

    if (!ndata)
	return NULL;

    ndata->o = nadata->o;
    ndata->refcount = 1;
    ndata->nadata = nadata;
    ndata->raddr = (struct sockaddr *) &ndata->remote;

    ndata->deferred_op_runner = ndata->o->alloc_runner(ndata->o,
						       udpn_deferred_op, ndata);
    if (!ndata->deferred_op_runner) {
	nadata->o->free(nadata->o, ndata);
	return NULL;
    }

    ndata->io = gensio_data_alloc(nadata->o, cb, user_data, gensio_udp_func,
				  NULL, "udp", ndata);
    if (!ndata->io) {
	ndata->o->free_runner(ndata->deferred_op_runner);
	nadata->o->free(nadata->o, ndata);
	return NULL;
    }
    gensio_set_is_packet(ndata->io, true);

    ndata->myfd = fd;
    ndata->raddrlen = addrlen;
    memcpy(ndata->raddr, addr, addrlen);

    /* Stick it on the end of the list. */
    udpn_add_to_list(starting_list, ndata);
    nadata->udpn_count++;

    return ndata;
}

static void
udpna_readhandler(int fd, void *cbdata)
{
    struct udpna_data *nadata = cbdata;
    struct udpn_data *ndata;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    int datalen;

    udpna_lock(nadata);
    if (nadata->data_pending_len)
	goto out_unlock;

    datalen = recvfrom(fd, nadata->read_data, nadata->max_read_size, 0,
		       (struct sockaddr *) &addr, &addrlen);
    if (datalen == -1) {
	/* FIXME - There is no really good way to report this error. */
	if (errno != EAGAIN && errno != EWOULDBLOCK)
	    gensio_acc_log(nadata->acc, GENSIO_LOG_ERR,
			   "Could not accept on UDP: %s", strerror(errno));
	goto out_unlock;
    }
    if (addrlen > sizeof(struct sockaddr_storage)) {
	/* Shouldn't happen. */
	gensio_acc_log(nadata->acc, GENSIO_LOG_ERR,
		       "Address too long: %d", addrlen);
	goto out_unlock;
    }

    udpna_fd_read_disable(nadata);

    nadata->data_pending_len = datalen;
    nadata->data_pos = 0;

    ndata = udpn_find(&nadata->udpns, (struct sockaddr *) &addr, addrlen);
    if (ndata)
	/* Data belongs to an existing connection. */
	goto got_ndata;

    if (nadata->closed || !nadata->enabled) {
	nadata->data_pending_len = 0;
	goto out_unlock_enable;
    }

    /* New connection. */
    ndata = udp_alloc_gensio(nadata, fd, (struct sockaddr *) &addr, addrlen,
			     NULL, NULL, &nadata->udpns);
    if (!ndata)
	goto out_nomem;

    ndata->state = UDPN_OPEN;
    nadata->read_disable_count++;

    nadata->pending_data_owner = ndata;
    nadata->in_new_connection = true;
    ndata->in_read = true;
    udpna_unlock(nadata);

    gensio_acc_cb(nadata->acc, GENSIO_ACC_EVENT_NEW_CONNECTION, ndata->io);

    udpna_lock(nadata);
    ndata->in_read = false;
    nadata->in_new_connection = false;

    if (ndata->state == UDPN_OPEN) {
    got_ndata:
	if (ndata->read_enabled && !ndata->in_read) {
	    ndata->in_read = true;
	    udpn_finish_read(ndata);
	}
    } else {
	nadata->data_pending_len = 0;
    }

    if (ndata->state == UDPN_IN_CLOSE) {
	udpn_finish_close(nadata, ndata);
	goto out_unlock_enable;
    }

    if (nadata->in_shutdown) {
	struct gensio_accepter *accepter = nadata->acc;

	if (nadata->shutdown_done)
	    nadata->shutdown_done(accepter, nadata->shutdown_data);
	nadata->in_shutdown = false;
    }
    udpna_check_finish_free(nadata);
    goto out_unlock_enable;

 out_nomem:
    nadata->data_pending_len = 0;
    gensio_acc_log(nadata->acc, GENSIO_LOG_ERR,
		   "Out of memory allocating for udp port");
 out_unlock_enable:
    udpna_fd_read_enable(nadata);
 out_unlock:
    udpna_unlock(nadata);
    return;
}

static int
udpna_startup(struct gensio_accepter *accepter)
{
    struct udpna_data *nadata = gensio_acc_get_gensio_data(accepter);
    int rv = 0;

    udpna_lock(nadata);
    if (!nadata->fds) {
	nadata->fds = gensio_open_socket(nadata->o, nadata->ai,
					 udpna_readhandler,
					 udpna_writehandler,
					 nadata, &nadata->nr_fds,
					 udpna_fd_cleared);
	if (nadata->fds == NULL) {
	    rv = errno;
	    goto out_unlock;
	}
	nadata->nr_accept_close_waiting = nadata->nr_fds;
    }

    nadata->setup = true;
    nadata->enabled = true;
    udpna_enable_read(nadata);
 out_unlock:
    udpna_unlock(nadata);

    return rv;
}

static int
udpna_shutdown(struct gensio_accepter *accepter,
	       gensio_acc_done shutdown_done, void *shutdown_data)
{
    struct udpna_data *nadata = gensio_acc_get_gensio_data(accepter);
    int rv = 0;

    udpna_lock(nadata);
    if (nadata->enabled) {
	nadata->enabled = false;
	nadata->setup = false;
	nadata->in_shutdown = true;
	nadata->shutdown_done = shutdown_done;
	nadata->shutdown_data = shutdown_data;
	if (!nadata->in_new_connection)
	    udpna_start_deferred_op(nadata);
    } else {
	rv = EAGAIN;
    }
    udpna_unlock(nadata);

    return rv;
}

static void
udpna_set_accept_callback_enable(struct gensio_accepter *accepter, bool enabled)
{
    struct udpna_data *nadata = gensio_acc_get_gensio_data(accepter);

    udpna_lock(nadata);
    nadata->enabled = true;
    udpna_unlock(nadata);
}

static void
udpna_free(struct gensio_accepter *accepter)
{
    struct udpna_data *nadata = gensio_acc_get_gensio_data(accepter);

    udpna_lock(nadata);

    nadata->enabled = false;
    nadata->setup = false;
    nadata->closed = true;

    if (!nadata->disabled) {
	udpna_check_finish_free(nadata);
	udpna_unlock(nadata);
    } else if (nadata->udpn_count == 0) {
	unsigned int i;

	for (i = 0; i < nadata->nr_fds; i++)
	    nadata->o->clear_fd_handlers_norpt(nadata->o, nadata->fds[i].fd);
	for (i = 0; i < nadata->nr_fds; i++)
	    close(nadata->fds[i].fd);
	udpna_unlock(nadata);
	udpna_do_free(nadata);
    }
}

static void
udpna_disable(struct gensio_accepter *accepter)
{
    struct udpna_data *nadata = gensio_acc_get_gensio_data(accepter);

    nadata->enabled = false;
    nadata->setup = false;
    nadata->in_shutdown = false;
    nadata->shutdown_done = NULL;
    nadata->disabled = true;
}

int
udpna_str_to_gensio(struct gensio_accepter *accepter, const char *addr,
		    gensio_event cb, void *user_data, struct gensio **new_net)
{
    struct udpna_data *nadata = gensio_acc_get_gensio_data(accepter);
    struct udpn_data *ndata = NULL;
    struct addrinfo *ai = NULL, *tai;
    unsigned int fdi;
    int err;
    const char **iargs;
    int iargc;
    bool is_port_set;
    int socktype, protocol;

    err = gensio_scan_network_port(nadata->o, addr, false, &ai, &socktype,
				   &protocol, &is_port_set, &iargc, &iargs);
    if (err)
	return err;

    err = EINVAL;
    if (protocol != IPPROTO_UDP || !is_port_set)
	goto out_err;

    /* Don't accept any args, we can't set the readbuf size here. */
    if (iargs && iargs[0])
	goto out_err;

    for (tai = ai; tai; tai = tai->ai_next) {
	for (fdi = 0; fdi < nadata->nr_fds; fdi++) {
	    if (nadata->fds[fdi].family == tai->ai_addr->sa_family)
		goto found;
	}
    }
    goto out_err;

 found:

    if (ai->ai_addrlen > sizeof(struct sockaddr_storage))
	goto out_err;

    udpna_lock(nadata);
    ndata = udpn_find(&nadata->udpns, ai->ai_addr, ai->ai_addrlen);
    if (!ndata)
	ndata = udpn_find(&nadata->closed_udpns, ai->ai_addr, ai->ai_addrlen);
    if (ndata) {
	udpna_unlock(nadata);
	err = EBUSY;
	goto out_err;
    }

    ndata = udp_alloc_gensio(nadata, nadata->fds[fdi].fd,
			     ai->ai_addr, ai->ai_addrlen,
			     cb, user_data, &nadata->closed_udpns);
    if (!ndata) {
	udpna_unlock(nadata);
	err = ENOMEM;
	goto out_err;
    }

    gensio_set_is_client(ndata->io, true);

    udpn_start_deferred_op(ndata);
    udpna_unlock(nadata);

    *new_net = ndata->io;

    return 0;

 out_err:
    if (ai)
	gensio_free_addrinfo(nadata->o, ai);
    if (iargs)
	str_to_argv_free(iargc, iargs);

    return err;
}

static int
gensio_acc_udp_func(struct gensio_accepter *acc, int func, int val,
		    const char *addr, void *done, void *data,
		    const void *data2, void *ret)
{
    switch (func) {
    case GENSIO_ACC_FUNC_STARTUP:
	return udpna_startup(acc);

    case GENSIO_ACC_FUNC_SHUTDOWN:
	return udpna_shutdown(acc, done, data);

    case GENSIO_ACC_FUNC_SET_ACCEPT_CALLBACK:
	udpna_set_accept_callback_enable(acc, val);
	return 0;

    case GENSIO_ACC_FUNC_FREE:
	udpna_free(acc);
	return 0;

    case GENSIO_ACC_FUNC_STR_TO_GENSIO:
	return udpna_str_to_gensio(acc, addr, done, data, ret);

    case GENSIO_ACC_FUNC_DISABLE:
	udpna_disable(acc);
	return 0;

    default:
	return ENOTSUP;
    }
}

static int
i_udp_gensio_accepter_alloc(struct addrinfo *iai, gensiods max_read_size,
			    struct gensio_os_funcs *o,
			    gensio_accepter_event cb, void *user_data,
			    struct gensio_accepter **accepter)
{
    struct udpna_data *nadata;

    nadata = o->zalloc(o, sizeof(*nadata));
    if (!nadata)
	return ENOMEM;
    nadata->o = o;
    gensio_list_init(&nadata->udpns);
    gensio_list_init(&nadata->closed_udpns);

    nadata->ai = gensio_dup_addrinfo(o, iai);
    if (!nadata->ai && iai) /* Allow a null ai if it was passed in. */
	goto out_nomem;

    nadata->read_data = o->zalloc(o, max_read_size);
    if (!nadata->read_data)
	goto out_nomem;

    nadata->deferred_op_runner = o->alloc_runner(o, udpna_deferred_op, nadata);
    if (!nadata->deferred_op_runner)
	goto out_nomem;

    nadata->lock = o->alloc_lock(o);
    if (!nadata->lock)
	goto out_nomem;

    nadata->acc = gensio_acc_data_alloc(o, cb, user_data, gensio_acc_udp_func,
					NULL, "udp", nadata);
    if (!nadata->acc)
	goto out_nomem;
    gensio_acc_set_is_packet(nadata->acc, true);

    nadata->max_read_size = max_read_size;

    *accepter = nadata->acc;
    return 0;

 out_nomem:
    udpna_do_free(nadata);
    return ENOMEM;
}

int
udp_gensio_accepter_alloc(struct addrinfo *iai, const char * const args[],
			  struct gensio_os_funcs *o,
			  gensio_accepter_event cb, void *user_data,
			  struct gensio_accepter **accepter)
{
    gensiods max_read_size = GENSIO_DEFAULT_UDP_BUF_SIZE;
    unsigned int i;

    for (i = 0; args && args[i]; i++) {
	if (gensio_check_keyds(args[i], "readbuf", &max_read_size) > 0)
	    continue;
	return EINVAL;
    }

    return i_udp_gensio_accepter_alloc(iai, max_read_size, o, cb, user_data,
				       accepter);
}

int
str_to_udp_gensio_accepter(const char *str, const char * const args[],
			   struct gensio_os_funcs *o,
			   gensio_accepter_event cb,
			   void *user_data,
			   struct gensio_accepter **acc)
{
    int err;
    struct addrinfo *ai;

    err = gensio_scan_netaddr(o, str, true, SOCK_DGRAM, IPPROTO_UDP, &ai);
    if (err)
	return err;

    err = udp_gensio_accepter_alloc(ai, args, o, cb, user_data, acc);
    gensio_free_addrinfo(o, ai);

    return err;
}

int
udp_gensio_alloc(struct addrinfo *ai, const char * const args[],
		struct gensio_os_funcs *o,
		gensio_event cb, void *user_data,
		struct gensio **new_gensio)
{
    struct udpn_data *ndata = NULL;
    struct gensio_accepter *accepter;
    struct udpna_data *nadata = NULL;
    struct addrinfo *lai = NULL;
    int err, new_fd, optval = 1;
    gensiods max_read_size = GENSIO_DEFAULT_UDP_BUF_SIZE;
    unsigned int i;

    for (i = 0; args && args[i]; i++) {
	if (gensio_check_keyds(args[i], "readbuf", &max_read_size) > 0)
	    continue;
	if (gensio_check_keyaddrs(o, args[i], "laddr", IPPROTO_UDP,
				  true, false, &lai) > 0)
	    continue;
	return EINVAL;
    }

    if (ai->ai_addrlen > sizeof(struct sockaddr_storage))
	return E2BIG;

    new_fd = socket(ai->ai_family, SOCK_DGRAM, 0);
    if (new_fd == -1)
	return errno;

    if (fcntl(new_fd, F_SETFL, O_NONBLOCK) == -1) {
	err = errno;
	close(new_fd);
	return err;
    }

    optval = 1;
    if (setsockopt(new_fd, SOL_SOCKET, SO_REUSEADDR,
		   (void *)&optval, sizeof(optval)) == -1) {
	err = errno;
	close(new_fd);
	return err;
    }

    if (lai) {
	if (bind(new_fd, lai->ai_addr, lai->ai_addrlen) == -1) {
	    err = errno;
	    close(new_fd);
	    return err;
	}
    }

    /* Allocate a dummy network accepter. */
    err = i_udp_gensio_accepter_alloc(NULL, max_read_size, o,
				      NULL, NULL, &accepter);
    if (err) {
	close(new_fd);
	return err;
    }
    nadata = gensio_acc_get_gensio_data(accepter);

    nadata->fds = o->zalloc(o, sizeof(*nadata->fds));
    if (!nadata->fds) {
	close(new_fd);
	udpna_do_free(nadata);
	return ENOMEM;
    }
    nadata->fds->family = ai->ai_family;
    nadata->fds->fd = new_fd;
    nadata->nr_fds = 1;
    /* fd belongs to udpn now, updn_do_free() will close it. */

    nadata->closed = true; /* Free nadata when ndata is freed. */

    ndata = udp_alloc_gensio(nadata, new_fd, ai->ai_addr, ai->ai_addrlen,
			     cb, user_data, &nadata->closed_udpns);
    if (!ndata) {
	err = ENOMEM;
    } else {
	gensio_set_is_client(ndata->io, true);
	nadata->udpn_count = 1;
	err = o->set_fd_handlers(o, new_fd, nadata,
				 udpna_readhandler, udpna_writehandler, NULL,
				 udpna_fd_cleared);
    }

    if (err) {
	udpn_do_free(ndata);
	udpna_do_free(nadata);
    } else {
	nadata->nr_accept_close_waiting = 1;
	*new_gensio = ndata->io;
    }

    return err;
}

int
str_to_udp_gensio(const char *str, const char * const args[],
		  struct gensio_os_funcs *o,
		  gensio_event cb, void *user_data,
		  struct gensio **new_gensio)
{
    struct addrinfo *ai;
    int err;

    err = gensio_scan_netaddr(o, str, false, SOCK_DGRAM, IPPROTO_UDP, &ai);
    if (err)
	return err;

    err = udp_gensio_alloc(ai, args, o, cb, user_data, new_gensio);
    gensio_free_addrinfo(o, ai);
    return err;
}

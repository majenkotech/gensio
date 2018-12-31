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

#include <errno.h>

#include <gensio/gensio_class.h>

#ifdef HAVE_OPENSSL

#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <assert.h>

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>

#include <gensio/gensio_ll_gensio.h>
#include <gensio/gensio_acc_gensio.h>

#include "gensio_filter_ssl.h"

int
ssl_gensio_alloc(struct gensio *child, char *args[],
		 struct gensio_os_funcs *o,
		 gensio_event cb, void *user_data,
		 struct gensio **net)
{
    int err;
    struct gensio_filter *filter;
    struct gensio_ll *ll;
    struct gensio *io;

    if (!gensio_is_reliable(child))
	/* Cowardly refusing to run SSL over an unreliable connection. */
	return EOPNOTSUPP;

    err = gensio_ssl_filter_alloc(o, args, &filter);
    if (err)
	return err;

    ll = gensio_gensio_ll_alloc(o, child);
    if (!ll) {
	gensio_filter_free(filter);
	return ENOMEM;
    }
    gensio_ref(child);

    io = base_gensio_alloc(o, ll, filter, "ssl", cb, user_data);
    if (!io) {
	gensio_ll_free(ll);
	gensio_filter_free(filter);
	return ENOMEM;
    }
    gensio_set_is_packet(io, true);
    gensio_set_is_reliable(io, true);
    gensio_free(child); /* Lose the ref we acquired. */

    *net = io;
    return 0;
}

int
str_to_ssl_gensio(const char *str, char *args[],
		  struct gensio_os_funcs *o,
		  gensio_event cb, void *user_data,
		  struct gensio **new_gensio)
{
    int err;
    struct gensio *io2;

    err = str_to_gensio(str, o, NULL, NULL, &io2);
    if (err)
	return err;

    err = ssl_gensio_alloc(io2, args, o, cb, user_data, new_gensio);
    if (err)
	gensio_free(io2);

    return err;
}

struct sslna_data {
    unsigned int max_read_size;
    unsigned int max_write_size;

    struct gensio_os_funcs *o;

    char *keyfile;
    char *certfile;
    char *CAfilepath;
};

static void
sslna_free(void *acc_data)
{
    struct sslna_data *nadata = acc_data;

    if (nadata->keyfile)
	nadata->o->free(nadata->o, nadata->keyfile);
    if (nadata->certfile)
	nadata->o->free(nadata->o, nadata->certfile);
    if (nadata->CAfilepath)
	nadata->o->free(nadata->o, nadata->CAfilepath);
    nadata->o->free(nadata->o, nadata);
}

int
sslna_connect_start(void *acc_data, struct gensio *child, struct gensio **rio)
{
    struct sslna_data *nadata = acc_data;
    struct gensio_os_funcs *o = nadata->o;
    int err;
    char *args[4] = {NULL, NULL, NULL};
    char buf1[50], buf2[50];
    int i;

    args[0] = o->zalloc(o, strlen(nadata->CAfilepath) + 4);
    if (!args[0])
	return ENOMEM;

    strcpy(args[0], "CA=");
    strcat(args[0], nadata->CAfilepath);

    i = 1;
    if (nadata->max_read_size != SSL3_RT_MAX_PLAIN_LENGTH) {
	snprintf(buf1, sizeof(buf1), "readbuf=%d", nadata->max_read_size);
	args[i++] = buf1;
    }
    if (nadata->max_write_size != SSL3_RT_MAX_PLAIN_LENGTH) {
	snprintf(buf2, sizeof(buf2), "writebuf=%d", nadata->max_write_size);
	args[i++] = buf2;
    }

    err = ssl_gensio_alloc(child, args, o, NULL, NULL, rio);

    if (args[0])
	o->free(o, args[0]);

    return err;
}

static int
sslna_new_child(void *acc_data, void **finish_data,
		struct gensio_filter **filter)
{
    struct sslna_data *nadata = acc_data;
    int err;

    err = gensio_ssl_server_filter_alloc(nadata->o,
					 nadata->keyfile, nadata->certfile,
					 nadata->CAfilepath,
					 nadata->max_read_size,
					 nadata->max_write_size,
					 filter);
    return err;
}

static int
sslna_finish_parent(void *acc_data, void *finish_data, struct gensio *io)
{
    gensio_set_is_packet(io, true);
    gensio_set_is_reliable(io, true);
    return 0;
}

static int
gensio_gensio_acc_ssl_cb(void *acc_data, int op, void *data1, void *data2)
{
    switch (op) {
    case GENSIO_GENSIO_ACC_CONNECT_START:
	return sslna_connect_start(acc_data, data1, data2);

    case GENSIO_GENSIO_ACC_NEW_CHILD:
	return sslna_new_child(acc_data, data1, data2);

    case GENSIO_GENSIO_ACC_FINISH_PARENT:
	return sslna_finish_parent(acc_data, data1, data2);

    case GENSIO_GENSIO_ACC_FREE:
	sslna_free(acc_data);
	return 0;

    default:
	return ENOTSUP;
    }
}

int
ssl_gensio_accepter_alloc(struct gensio_accepter *child,
			  char *args[],
			  struct gensio_os_funcs *o,
			  gensio_accepter_event cb, void *user_data,
			  struct gensio_accepter **accepter)
{
    struct sslna_data *nadata;
    const char *keyfile = NULL;
    const char *certfile = NULL;
    const char *CAfilepath = NULL;
    int err;
    unsigned int i;
    unsigned int max_write_size = SSL3_RT_MAX_PLAIN_LENGTH;
    unsigned int max_read_size = SSL3_RT_MAX_PLAIN_LENGTH;

    if (!gensio_acc_is_reliable(child))
	/* Cowardly refusing to run SSL over an unreliable connection. */
	return EOPNOTSUPP;

    for (i = 0; args[i]; i++) {
	if (gensio_check_keyvalue(args[i], "CA", &CAfilepath))
	    continue;
	if (gensio_check_keyvalue(args[i], "key", &keyfile))
	    continue;
	if (gensio_check_keyvalue(args[i], "cert", &certfile))
	    continue;
	if (gensio_check_keyuint(args[i], "writebuf", &max_write_size) > 0)
	    continue;
	if (gensio_check_keyuint(args[i], "readbuf", &max_read_size) > 0)
	    continue;
	return EINVAL;
    }

    if (!CAfilepath || !keyfile)
	return EINVAL;

    nadata = o->zalloc(o, sizeof(*nadata));
    if (!nadata)
	return ENOMEM;

    nadata->o = o;
    nadata->max_write_size = max_write_size;
    nadata->max_read_size = max_read_size;

    nadata->keyfile = gensio_strdup(o, keyfile);
    if (!nadata->keyfile)
	goto out_nomem;

    if (!certfile)
	certfile = keyfile;

    nadata->certfile = gensio_strdup(o, certfile);
    if (!nadata->certfile)
	goto out_nomem;

    nadata->CAfilepath = gensio_strdup(o, CAfilepath);
    if (!nadata->CAfilepath)
	goto out_nomem;

    err = gensio_gensio_accepter_alloc(child, o, "ssl", cb, user_data,
				       gensio_gensio_acc_ssl_cb, nadata,
				       accepter);
    if (err)
	goto out_err;
    gensio_acc_set_is_packet(*accepter, true);
    gensio_acc_set_is_reliable(*accepter, true);

    return 0;

 out_nomem:
    err = ENOMEM;
 out_err:
    sslna_free(nadata);
    return err;
}

int
str_to_ssl_gensio_accepter(const char *str, char *args[],
			   struct gensio_os_funcs *o,
			   gensio_accepter_event cb,
			   void *user_data,
			   struct gensio_accepter **acc)
{
    int err;
    struct gensio_accepter *acc2 = NULL;

    err = str_to_gensio_accepter(str, o, NULL, NULL, &acc2);
    if (!err) {
	err = ssl_gensio_accepter_alloc(acc2, args, o, cb, user_data, acc);
	if (err)
	    gensio_acc_free(acc2);
    }

    return err;
}

#else /* HAVE_OPENSSL */
int
ssl_gensio_alloc(struct gensio *child, char *args[],
		 struct gensio_os_funcs *o,
		 gensio_event cb, void *user_data,
		 struct gensio **net)
{
    return ENOTSUP;
}

int
ssl_gensio_accepter_alloc(char *args[],
			  struct gensio_os_funcs *o,
			  struct gensio_accepter *child,
			  unsigned int max_read_size,
			  gensio_accepter_event cb, void *user_data,
			  struct gensio_accepter **accepter)
{
    return ENOTSUP;
}

#endif /* HAVE_OPENSSL */

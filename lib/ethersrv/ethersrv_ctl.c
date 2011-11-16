/**
 * \file ethersrv_ctl.c
 * \brief Generic server part responsible for exporting ether_control.if
 * for most ethernet drivers.  Current drivers using this server code are
 * -- e1000n
 * -- rtl8029
 * -- eMAC
 */

/*
 * Copyright (c) 2007-11 ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <barrelfish/barrelfish.h>
#include <barrelfish/nameservice_client.h>
#include <barrelfish/net_constants.h>

#include <stdio.h>
#include <string.h>
#include <trace/trace.h>
#include <ethersrv/ethersrv.h>
#include <bfdmuxvm/vm.h>
#include <if/ether_control_defs.h>
#include "ethersrv_local.h"
#include "ethersrv_debug.h"

/* This is client_closure for filter management */
struct client_closure_FM {
    struct ether_control_binding *app_connection;       /* FIXME: Do I need this? */
    struct cont_queue *q;
/* FIXME: this should contain the registered buffer ptr */
};

static void register_filter_memory_request(struct ether_control_binding *cc,
                                           struct capref mem_cap);
static void register_filter(struct ether_control_binding *cc, uint64_t id,
                            uint64_t len_rx, uint64_t len_tx,
                            uint64_t buffer_id_rx, uint64_t buffer_id_tx,
                            uint64_t ftype, uint64_t paused);
static void register_arp_filter(struct ether_control_binding *cc, uint64_t id,
                                uint64_t len_rx, uint64_t len_tx);
static void deregister_filter(struct ether_control_binding *cc,
                              uint64_t filter_id);
static void re_register_filter(struct ether_control_binding *cc,
                               uint64_t filter_id, uint64_t buffer_id_rx,
                               uint64_t buffer_id_tx);
static void pause_filter(struct ether_control_binding *cc, uint64_t filter_id,
                         uint64_t buffer_id_rx, uint64_t buffer_id_tx);

// Initialize interface for ether_control channel
static struct ether_control_rx_vtbl rx_ether_control_vtbl = {
    .register_filter_memory_request = register_filter_memory_request,
    .register_filter_request = register_filter,
    .re_register_filter_request = re_register_filter,
    .deregister_filter_request = deregister_filter,
    .register_arp_filter_request = register_arp_filter,
    .pause = pause_filter,
};



/*****************************************************************
 * Local states:
 *****************************************************************/

static char *my_service_name = NULL;

// filters state:
static struct filter *rx_filters;
static struct filter arp_filter_rx;
static struct filter arp_filter_tx;

static uint64_t filter_id_counter = 0;

static void export_ether_control_cb(void *st, errval_t err, iref_t iref)
{
    char service_name[100];

    snprintf(service_name, sizeof(service_name), "%s%s", my_service_name,
             FILTER_SERVICE_SUFFIX);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "service[%s] export failed", service_name);
        abort();
    }

    ETHERSRV_DEBUG("service [%s] exported at iref %u\n", service_name, iref);

    // register this iref with the name service
    err = nameservice_register(service_name, iref);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "nameservice_register failed for [%s]", service_name);
        abort();
    }
}


static errval_t connect_ether_control_cb(void *st,
                                         struct ether_control_binding *b)
{
    ETHERSRV_DEBUG("ether_netd service got a connection!55\n");

    // copy my message receive handler vtable to the binding
    b->rx_vtbl = rx_ether_control_vtbl;
    //b->error_handler = error_handler;

    struct client_closure_FM *ccfm =
      (struct client_closure_FM *) malloc(sizeof(struct client_closure_FM));

    b->st = ccfm;
    ccfm->q = create_cont_q("FILTER-MANAGER");
    ccfm->app_connection = b;

    // FIXME: should I refuse more than one connections for FM services?
    //  Currently, I am accepting them

    // accept the connection (we could return an error to refuse it)
    return SYS_ERR_OK;
} // end function: connect_ether_control_cb



/*****************************************************************
 *   filter registration
 *****************************************************************/

/* FIXME: provide proper handler here */
static errval_t send_resiger_filter_memory_response(struct q_entry entry)
{
    //    ETHERSRV_DEBUG("send_resigered_netd_memory  -----\n");
    struct ether_control_binding *b =
      (struct ether_control_binding *) entry.binding_ptr;
    struct client_closure_FM *ccfm = (struct client_closure_FM *) b->st;

    if (b->can_send(b)) {
        return b->tx_vtbl.register_filter_memory_response(b,
                                                          MKCONT
                                                          (cont_queue_callback,
                                                           ccfm->q),
                                                          entry.plist[0]);
        /* entry.error */
    } else {
        ETHERSRV_DEBUG("send_resigered_netd_memory Flounder bsy will retry\n");
        return FLOUNDER_ERR_TX_BUSY;
    }
}

static struct bulk_transfer_slave bt_filter_rx;

static void register_filter_memory_request(struct ether_control_binding *cc,
                                           struct capref mem_cap)
{

    errval_t err = SYS_ERR_OK;

    struct frame_identity pa;

    err = invoke_frame_identify(mem_cap, &pa);
    assert(err_is_ok(err));

    ETHERSRV_DEBUG("register_netd_memory: attempt to register memory\n");
    // 2 is rx + tx
    if ((1L << pa.bits) < BASE_PAGE_SIZE * 2) {
        ETHERSRV_DEBUG("netd did not provided enough for filter transfer\n");
        err = FILTER_ERR_NOT_ENOUGH_MEMORY;     /* ps: FIXME: enable this error */

    } /* end if: not enough memory */
    else {                      /* enough memory, try to map it */
        void *pool;

        err = vspace_map_one_frame_attr((void *) (&pool), BASE_PAGE_SIZE * 2,
                                        mem_cap,
                                        VREGION_FLAGS_READ_WRITE_NOCACHE, NULL,
                                        NULL);

        if (err_is_fail(err)) {
            DEBUG_ERR(err, "vspace_map_one_frame failed");
            //            abort();
        } /* end if: mapping failed */
        else {
            // Init receiver
            err = bulk_slave_init(pool, BASE_PAGE_SIZE * 2, &bt_filter_rx);
            //            assert(err_is_ok(err));

        }                       /* end else: mapping sucessful */

    }                           /* end else : */

    /* call registered_netd_memory with new IDC */
    struct q_entry entry;

    memset(&entry, 0, sizeof(struct q_entry));
    entry.handler = send_resiger_filter_memory_response;
    entry.binding_ptr = (void *) cc;
    struct client_closure_FM *ccfm = (struct client_closure_FM *) cc->st;

    entry.plist[0] = err;
    /* entry.plist[0]
       entry.error */

    enqueue_cont_q(ccfm->q, &entry);
    ETHERSRV_DEBUG("register_netd_memory: sent IDC\n");

}                               /* end function : register_netd_memory */

/* Handler for sending response to register_filter */
static errval_t send_register_filter_response(struct q_entry e)
{
    //    ETHERSRV_DEBUG("send_resigered_filter for ID %lu  --\n", e.plist[0]);
    struct ether_control_binding *b =
      (struct ether_control_binding *) e.binding_ptr;
    struct client_closure_FM *ccfm = (struct client_closure_FM *) b->st;

    if (b->can_send(b)) {
        return b->tx_vtbl.register_filter_response(b,
                                                   MKCONT(cont_queue_callback,
                                                          ccfm->q), e.plist[0],
                                                   e.plist[1], e.plist[2],
                                                   e.plist[3], e.plist[4],
                                                   e.plist[5]);
        /* e.id,       e.error,    e.filter_id, e.buffer_id_rx,
         * e.buffer_id_tx, e.filter_type */

    } else {
        ETHERSRV_DEBUG("send_resigered_filter: ID %" PRIu64
                       ": Flounder bsy will retry\n", e.plist[0]);
        return FLOUNDER_ERR_TX_BUSY;
    }
}

static void wrapper_send_filter_registered_msg(struct ether_control_binding *cc,
                                               uint64_t id, errval_t err,
                                               uint64_t filter_id,
                                               uint64_t buffer_id_rx,
                                               uint64_t buffer_id_tx,
                                               uint64_t ftype)
{

    /* call registered_netd_memory with new IDC */

    struct q_entry entry;

    memset(&entry, 0, sizeof(struct q_entry));
    entry.handler = send_register_filter_response;
    entry.binding_ptr = (void *) cc;
    struct client_closure_FM *ccfm = (struct client_closure_FM *) cc->st;

    entry.plist[0] = id;
    entry.plist[1] = err;
    entry.plist[2] = filter_id;
    entry.plist[3] = buffer_id_rx;
    entry.plist[4] = buffer_id_tx;
    entry.plist[5] = ftype;
    // e.plist[0], e.plist[1], e.plist[2],  e.plist[3], e.plist[4], e.plist[5])
    // id, error, filter_id, buffer_id_rx, buffer_id_tx, filter_type

    enqueue_cont_q(ccfm->q, &entry);

}

/**
 * \brief: Registers the filter with network driver
 */
static void register_filter(struct ether_control_binding *cc, uint64_t id,
                            uint64_t len_rx, uint64_t len_tx,
                            uint64_t buffer_id_rx, uint64_t buffer_id_tx,
                            uint64_t ftype, uint64_t paused)
{
    errval_t err = SYS_ERR_OK;

    ETHERSRV_DEBUG("Register_filter: ID:%" PRIu64 " of type[%" PRIu64
                   "] buffers RX[%" PRIu64 "] and TX[%" PRIu64 "]\n", id, ftype,
                   buffer_id_rx, buffer_id_tx);

    struct buffer_descriptor *buffer_rx = NULL;
    struct buffer_descriptor *buffer_tx = NULL;
    struct buffer_descriptor *tmp = buffers_list;

    while (tmp) {

        if (tmp->buffer_id == buffer_id_tx) {
            buffer_tx = tmp;
        }

        if (tmp->buffer_id == buffer_id_rx) {
            buffer_rx = tmp;
        }

        if (buffer_rx != NULL && buffer_tx != NULL) {
            break;
        }

        tmp = tmp->next;
    }                           /* end while : */

    if (buffer_rx == NULL || buffer_tx == NULL) {
        ETHERSRV_DEBUG("no buffer found for the provided buffer id\n");
        err = FILTER_ERR_BUFF_NOT_FOUND;

        wrapper_send_filter_registered_msg(cc, id, err, 0, buffer_id_rx,
                                           buffer_id_tx, ftype);
        return;
    }

    if (len_rx > BASE_PAGE_SIZE) {
        len_rx = BASE_PAGE_SIZE;
    }

    if (len_tx > BASE_PAGE_SIZE) {
        len_tx = BASE_PAGE_SIZE;
    }

    /* using id to find the location of memory */
    void *buf = bulk_slave_buf_get_mem(&bt_filter_rx, id, NULL);

    if (buf == NULL) {
        ETHERSRV_DEBUG("no memory available for filter transfer\n");
        err = FILTER_ERR_NO_NETD_MEM;
        wrapper_send_filter_registered_msg(cc, id, err, 0, buffer_id_rx,
                                           buffer_id_tx, ftype);
        return;
    }

    /* Create the filter data-structures */
    struct filter *new_filter_rx =
      (struct filter *) malloc(sizeof(struct filter));
    struct filter *new_filter_tx =
      (struct filter *) malloc(sizeof(struct filter));

    /* FIXME: use goto to deal with failure conditions and reduce the code */
    if (new_filter_rx == NULL || new_filter_tx == NULL) {
        ETHERSRV_DEBUG("out of memory for filter registration\n");
        err = ETHERSRV_ERR_NOT_ENOUGH_MEM;
        wrapper_send_filter_registered_msg(cc, id, err, 0, buffer_id_rx,
                                           buffer_id_tx, ftype);

        if (new_filter_rx) {
            free(new_filter_rx);
        }

        if (new_filter_tx) {
            free(new_filter_tx);
        }
        return;
    }

    /* Zero out the filters */
    memset(new_filter_rx, 0, sizeof(struct filter));
    memset(new_filter_tx, 0, sizeof(struct filter));

    /* Allocate memory for holding the filter-data */
    new_filter_rx->data = (uint8_t *) malloc(len_rx);
    new_filter_tx->data = (uint8_t *) malloc(len_tx);

    if (new_filter_rx->data == NULL || new_filter_tx->data == NULL) {
        ETHERSRV_DEBUG("out of memory for filter data registration\n");
        err = ETHERSRV_ERR_NOT_ENOUGH_MEM;
        wrapper_send_filter_registered_msg(cc, id, err, 0, buffer_id_rx,
                                           buffer_id_tx, ftype);

        if (new_filter_rx->data) {
            free(new_filter_rx->data);
        }

        if (new_filter_tx->data) {
            free(new_filter_tx->data);
        }

        free(new_filter_rx);
        free(new_filter_tx);

        return;
    }

    /* Zero-out the area of filter-data */
    memset(new_filter_rx->data, 0, len_rx);
    memset(new_filter_tx->data, 0, len_tx);

    filter_id_counter++;

    // rx filter
    memcpy(new_filter_rx->data, buf, len_rx);
    new_filter_rx->len = len_rx;
    new_filter_rx->filter_id = filter_id_counter;
    new_filter_rx->filter_type = ftype;
    new_filter_rx->buffer = buffer_rx;
    new_filter_rx->next = rx_filters;
    new_filter_rx->paused = paused ? true : false;
    rx_filters = new_filter_rx;
    ETHERSRV_DEBUG("filter registered with id %" PRIu64 " and len %d\n",
                   new_filter_rx->filter_id, new_filter_rx->len);

    // tx filter
    void *bbuf_tx = buf + BASE_PAGE_SIZE;

    memcpy(new_filter_tx->data, bbuf_tx, len_tx);
    new_filter_tx->len = len_tx;
    new_filter_tx->filter_id = filter_id_counter;
    new_filter_tx->filter_type = ftype;
    new_filter_tx->buffer = buffer_tx;  // we do not really need to set this
    /* FIXME: following linked list implementation looks buggy */
    new_filter_tx->next = buffer_tx->tx_filters;
    buffer_tx->tx_filters = new_filter_tx;
    /* FIXME: following looks buggy!!! */
    buffer_rx->tx_filters = new_filter_tx;      // sometimes rx buffers transmit

    /* reporting back the success/failure */
    wrapper_send_filter_registered_msg(cc, id, err, filter_id_counter,
                                       buffer_id_rx, buffer_id_tx, ftype);

    ETHERSRV_DEBUG("Register_filter: ID %" PRIu64 ": type[%" PRIu64
                   "] successful [%" PRIu64 "]\n", id, ftype,
                   filter_id_counter);

}                               /* end function: register filter */


/* Handler for sending response to deregister_filter */
static errval_t send_deregister_filter_response(struct q_entry e)
{
    //    ETHERSRV_DEBUG("send_deresigered_filter_response for ID %lu  -----\n", e.plist[0]);
    struct ether_control_binding *b =
      (struct ether_control_binding *) e.binding_ptr;
    struct client_closure_FM *ccfm = (struct client_closure_FM *) b->st;

    if (b->can_send(b)) {
        return b->tx_vtbl.deregister_filter_response(b,
                                                     MKCONT(cont_queue_callback,
                                                            ccfm->q),
                                                     e.plist[0], e.plist[1]);
        /* e.error,    e.filter_id,  */

    } else {
        ETHERSRV_DEBUG("send_deresiger_filter_response: Filter_ID %" PRIu64
                       ": Flounder bsy will retry\n", e.plist[1]);
        return FLOUNDER_ERR_TX_BUSY;
    }
}

static void wrapper_send_filter_deregister_msg(struct ether_control_binding *cc,
                                               errval_t err, uint64_t filter_id)
{

    /* call registered_netd_memory with new IDC */

    struct q_entry entry;

    memset(&entry, 0, sizeof(struct q_entry));
    entry.handler = send_deregister_filter_response;
    entry.binding_ptr = (void *) cc;
    struct client_closure_FM *ccfm = (struct client_closure_FM *) cc->st;

    entry.plist[0] = err;
    entry.plist[1] = filter_id;
    /* e.plist[0], e.plist[1] );
       e.error,    e.filter_id */
    enqueue_cont_q(ccfm->q, &entry);
}

static struct filter *delete_from_filter_list(struct filter *head,
                                              uint64_t filter_id)
{
    struct filter *prev = NULL;

    while (head != NULL) {
        if (head->filter_id == filter_id) {
            if (prev == NULL) {
                rx_filters = head->next;
            } else {
                prev->next = head->next;
            }
            return head;
        }                       /* end if: filter_id found */
    }                           /* end while: for each element in list */
    return NULL;                /* could not not find the id. */
}


/**
 * \brief: Deregisters the filter with network driver
 */
static void deregister_filter(struct ether_control_binding *cc,
                              uint64_t filter_id)
{
    errval_t err = SYS_ERR_OK;

    ETHERSRV_DEBUG("DeRegister_filter: ID:%" PRIu64 "\n", filter_id);

    /* Create the filter data-structures */
    struct filter *rx_filter = NULL;
    struct filter *tx_filter = NULL;

    rx_filter = delete_from_filter_list(rx_filters, filter_id);
    /* FIXME: delete the tx_filter from the filter list "buffer_rx->tx_filters" */
//    tx_filter = delete_from_filter_list(tx_filters, filter_id);

    if (rx_filter == NULL /*|| tx_filter == NULL */ ) {
        ETHERSRV_DEBUG("Deregister_filter:requested filter_ID [%" PRIu64
                       "] not found\n", filter_id);
        err = FILTER_ERR_FILTER_NOT_FOUND;
    }

    if (rx_filter) {
        free(rx_filter);
    }

    if (tx_filter) {
        free(tx_filter);
    }

    /* reporting back the success/failure */
    wrapper_send_filter_deregister_msg(cc, err, filter_id);

    ETHERSRV_DEBUG("Deregister_filter: ID %" PRIu64 ": Done\n", filter_id);

}                               /* end function: deregister_filter */



/* Handler for sending response to re register_filter */
static errval_t send_re_register_filter_response(struct q_entry e)
{
    //    ETHERSRV_DEBUG("send_re_register_filter_response for ID %lu  -----\n", e.plist[0]);
    struct ether_control_binding *b =
      (struct ether_control_binding *) e.binding_ptr;
    struct client_closure_FM *ccfm = (struct client_closure_FM *) b->st;

    if (b->can_send(b)) {
        return b->tx_vtbl.re_register_filter_response(b,
                                                      MKCONT
                                                      (cont_queue_callback,
                                                       ccfm->q), e.plist[0],
                                                      e.plist[1], e.plist[2],
                                                      e.plist[2]);
        /* e.error,    e.filter_id, e.buffer_id_rx, e.buffer_id_rx */

    } else {
        ETHERSRV_DEBUG("send_re_register_filter_response: Filter_ID %" PRIu64
                       ": Flounder bsy will retry\n", e.plist[1]);
        return FLOUNDER_ERR_TX_BUSY;
    }
}                               /* end function: send_re_register_filter_response */

static errval_t send_pause_filter_response(struct q_entry e)
{
    //    ETHERSRV_DEBUG("send_re_register_filter_response for ID %lu  -----\n", e.plist[0]);
    struct ether_control_binding *b =
      (struct ether_control_binding *) e.binding_ptr;
    struct client_closure_FM *ccfm = (struct client_closure_FM *) b->st;

    if (b->can_send(b)) {
        return b->tx_vtbl.pause_response(b,
                                         MKCONT(cont_queue_callback, ccfm->q),
                                         e.plist[0], e.plist[1]);
        /* e.error,    e.filter_id, e.buffer_id_rx, e.buffer_id_rx */

    } else {
        ETHERSRV_DEBUG("send_re_register_filter_response: Filter_ID %" PRIu64
                       ": Flounder bsy will retry\n", e.plist[1]);
        return FLOUNDER_ERR_TX_BUSY;
    }
}                               /* end function: send_re_register_filter_response */

static void wrapper_send_filter_re_register_msg(struct ether_control_binding
                                                *cc, errval_t err,
                                                uint64_t filter_id,
                                                uint64_t buffer_id_rx,
                                                uint64_t buffer_id_tx)
{

    /* call registered_netd_memory with new IDC */

    struct q_entry entry;

    memset(&entry, 0, sizeof(struct q_entry));
    entry.handler = send_re_register_filter_response;
    entry.binding_ptr = (void *) cc;
    struct client_closure_FM *ccfm = (struct client_closure_FM *) cc->st;

    entry.plist[0] = err;
    entry.plist[1] = filter_id;
    entry.plist[2] = buffer_id_rx;
    entry.plist[3] = buffer_id_tx;
/*    e.plist[0], e.plist[1],  e.plist[2],     e.plist[2]
      e.error,    e.filter_id, e.buffer_id_rx, e.buffer_id_rx */
    enqueue_cont_q(ccfm->q, &entry);
}                               /* end function: wrapper_send_filter_re_register_msg */

static void wrapper_send_filter_pause_msg(struct ether_control_binding *cc,
                                          errval_t err, uint64_t filter_id)
{

    /* call registered_netd_memory with new IDC */

    struct q_entry entry;

    memset(&entry, 0, sizeof(struct q_entry));
    entry.handler = send_pause_filter_response;
    entry.binding_ptr = (void *) cc;
    struct client_closure_FM *ccfm = (struct client_closure_FM *) cc->st;

    entry.plist[0] = filter_id;
    entry.plist[1] = err;
/*    e.plist[0], e.plist[1],  e.plist[2],     e.plist[2]
      e.error,    e.filter_id, e.buffer_id_rx, e.buffer_id_rx */
    enqueue_cont_q(ccfm->q, &entry);
}                               /* end function: wrapper_send_filter_re_register_msg */


static struct filter *find_from_filter_list(struct filter *head,
                                            uint64_t filter_id)
{
    while (head != NULL) {
        if (head->filter_id == filter_id) {
            return head;
        }                       /* end if: filter_id found */
        head = head->next;
    }                           /* end while: for each element in list */
    return NULL;                /* could not not find the id. */
}

/**
 * \brief: re-registers the filter with network driver
 */
static void re_register_filter(struct ether_control_binding *cc,
                               uint64_t filter_id, uint64_t buffer_id_rx,
                               uint64_t buffer_id_tx)
{
    errval_t err = SYS_ERR_OK;

    ETHERSRV_DEBUG("re_register_filter: ID:%" PRIu64 "\n", filter_id);

    /* Create the filter data-structures */
    struct filter *rx_filter = NULL;

//    struct filter *tx_filter = NULL;

    rx_filter = find_from_filter_list(rx_filters, filter_id);
    /* FIXME: delete the tx_filter from the filter list "buffer_rx->tx_filters" */
//    tx_filter = delete_from_filter_list(tx_filters, filter_id);

    if (rx_filter == NULL /*|| tx_filter == NULL */ ) {
        ETHERSRV_DEBUG("re_register_filter: requested filter_ID [%" PRIu64
                       "] not found\n", filter_id);
        err = FILTER_ERR_FILTER_NOT_FOUND;
        wrapper_send_filter_re_register_msg(cc, err, filter_id,
                                            buffer_id_rx, buffer_id_tx);
        return;
    }

    /* Find the buffer with given buffer_id */
    struct buffer_descriptor *buffer_rx = find_buffer(buffer_id_rx);
    struct buffer_descriptor *buffer_tx = NULL;

    buffer_tx = find_buffer(buffer_id_tx);
    if (buffer_rx == NULL || buffer_rx == NULL) {
        ETHERSRV_DEBUG("re_register_filter: provided buffer id's not found\n");
        ETHERSRV_DEBUG("re_register_filter: rx=[[%" PRIu64 "] = %p], tx=[[%"
                       PRIu64 "] = %p]\n", buffer_id_rx, buffer_rx,
                       buffer_id_tx, buffer_tx);
        err = FILTER_ERR_BUFFER_NOT_FOUND;      /* set error value */
        wrapper_send_filter_re_register_msg(cc, err, filter_id, buffer_id_rx,
                                            buffer_id_tx);
    }
    rx_filter->buffer = buffer_rx;
    /* FIXME: Also, set the new buffer for tx_filters */
    /* reporting back the success/failure */
    wrapper_send_filter_re_register_msg(cc, err, filter_id, buffer_id_rx,
                                        buffer_id_tx);

    ETHERSRV_DEBUG("re_register_filter: ID %" PRIu64 ": Done\n", filter_id);

}                               /* end function: re_register_filter */

/**
 * \brief: pause the filter with network driver
 */
static void pause_filter(struct ether_control_binding *cc, uint64_t filter_id,
                         uint64_t buffer_id_rx, uint64_t buffer_id_tx)
{
    errval_t err = SYS_ERR_OK;

    ETHERSRV_DEBUG("(un)pause_filter: ID:%" PRIu64 "\n", filter_id);

    /* Create the filter data-structures */
    struct filter *rx_filter = NULL;

//    struct filter *tx_filter = NULL;

    rx_filter = find_from_filter_list(rx_filters, filter_id);
    /* FIXME: delete the tx_filter from the filter list "buffer_rx->tx_filters" */
//    tx_filter = delete_from_filter_list(tx_filters, filter_id);

    if (rx_filter == NULL /*|| tx_filter == NULL */ ) {
        ETHERSRV_DEBUG("pause_filter: requested filter_ID [%" PRIu64
                       "] not found\n", filter_id);
        err = FILTER_ERR_FILTER_NOT_FOUND;
        assert(!"NYI");
        /* wrapper_send_filter_re_register_msg(cc, err, filter_id, */
        /*         buffer_id_rx, buffer_id_tx); */
        return;
    }

    /* Find the buffer with given buffer_id */
    struct buffer_descriptor *buffer_rx = find_buffer(buffer_id_rx);
    struct buffer_descriptor *buffer_tx = NULL;

    buffer_tx = find_buffer(buffer_id_tx);
    if (buffer_rx == NULL || buffer_rx == NULL) {
        ETHERSRV_DEBUG("re_register_filter: provided buffer id's not found\n");
        ETHERSRV_DEBUG("re_register_filter: rx=[[%" PRIu64 "] = %p], tx=[[%"
                       PRIu64 "] = %p]\n", buffer_id_rx, buffer_rx,
                       buffer_id_tx, buffer_tx);
        assert(!"NYI");
        /* err =  FILTER_ERR_BUFFER_NOT_FOUND; /\* set error value *\/ */
        /* wrapper_send_filter_re_register_msg(cc, err, filter_id, buffer_id_rx, */
        /*         buffer_id_tx); */
    }
    rx_filter->buffer = buffer_rx;
    /* FIXME: Also, set the new buffer for tx_filters */
    /* reporting back the success/failure */
    wrapper_send_filter_pause_msg(cc, err, filter_id);

    rx_filter->paused = false;
    if (rx_filter->pause_bufpos > 0) {
        for (int i = 0; i < rx_filter->pause_bufpos; i++) {
            struct bufdesc *bd = &rx_filter->pause_buffer[i];

            struct ether_binding *b = rx_filter->buffer->con;
            assert(b != NULL);
            struct client_closure *cl = (struct client_closure *)b->st;
            assert(cl != NULL);
            if (cl->debug_state == 4) {
                ++cl->in_paused_pkts;
            }
            copy_packet_to_user(rx_filter->buffer, bd->pkt_data, bd->pkt_len);
        }
    }
    rx_filter->pause_bufpos = 0;

    ETHERSRV_DEBUG("(un)pause_filter: ID %" PRIu64 ": Done\n", filter_id);

}                               /* end function: re_register_filter */


/* Handler for sending response to register_filter */
static errval_t send_register_arp_filter_response(struct q_entry entry)
{
    //    ETHERSRV_DEBUG("send_resigered_arp_filter  -----\n");
    struct ether_control_binding *b =
      (struct ether_control_binding *) entry.binding_ptr;
    struct client_closure_FM *ccfm = (struct client_closure_FM *) b->st;

    if (b->can_send(b)) {
        return b->tx_vtbl.register_arp_filter_response(b,
                                                       MKCONT
                                                       (cont_queue_callback,
                                                        ccfm->q),
                                                       entry.plist[0],
                                                       entry.plist[1]);
        /* e.id,        e.error */

    } else {
        ETHERSRV_DEBUG("send_resigered_arp_filter Flounder bsy will retry\n");
        return FLOUNDER_ERR_TX_BUSY;
    }
}

static void wrapper_send_arp_filter_registered_msg(struct ether_control_binding
                                                   *cc, uint64_t id,
                                                   errval_t err)
{

    /* call registered_netd_memory with new IDC */

    struct q_entry entry;

    memset(&entry, 0, sizeof(struct q_entry));
    entry.handler = send_register_arp_filter_response;
    entry.binding_ptr = (void *) cc;
    struct client_closure_FM *ccfm = (struct client_closure_FM *) cc->st;

    entry.plist[0] = id;
    entry.plist[1] = err;
    /* entry.plist[0], entry.plist[1]
       id,             e.error */

    enqueue_cont_q(ccfm->q, &entry);
}

static void register_arp_filter(struct ether_control_binding *cc, uint64_t id,
                                uint64_t len_rx, uint64_t len_tx)
{

    errval_t err = SYS_ERR_OK;

    if (len_rx > BASE_PAGE_SIZE) {
        len_rx = BASE_PAGE_SIZE;
    }
    if (len_tx > BASE_PAGE_SIZE) {
        len_tx = BASE_PAGE_SIZE;
    }

    /* using id to find the location of memory */
    void *buf = bulk_slave_buf_get_mem(&bt_filter_rx, id, NULL);

    if (buf == NULL) {
        ETHERSRV_DEBUG("no memory available for arp_filter transfer\n");
        err = FILTER_ERR_NO_NETD_MEM;
        wrapper_send_arp_filter_registered_msg(cc, id, err);
        return;
    }

    arp_filter_rx.data = (uint8_t *) malloc(len_rx);
    assert(arp_filter_rx.data);
    memcpy(arp_filter_rx.data, buf, len_rx);
    arp_filter_rx.len = len_rx;
    ETHERSRV_DEBUG("#### The received arp RX filter is\n");
    //    show_binary_blob(arp_filter_rx.data, arp_filter_rx.len);

    void *bbuf_tx = buf + BASE_PAGE_SIZE;

    arp_filter_tx.data = (uint8_t *) malloc(len_tx);
    assert(arp_filter_tx.data);
    memcpy(arp_filter_tx.data, bbuf_tx, len_tx);
    arp_filter_tx.len = len_tx;
    ETHERSRV_DEBUG("#### The received arp RX filter is\n");
    //    show_binary_blob(arp_filter_tx.data, arp_filter_tx.len);

    wrapper_send_arp_filter_registered_msg(cc, id, err);
}



static void send_arp_to_all(void *data, uint64_t len)
{
    struct filter *head = rx_filters;

    ETHERSRV_DEBUG("### Sending the ARP packet to all, %"PRIx64" \n", len);
    /* sending ARP packets to only those who have registered atleast one
     * filter with e1000n
     * */

    /* FIXME: this code will send two copies or ARP if there are two filters
     * registered, which is incorrect.  Fix it. */
    struct ether_binding *b = NULL;
    struct client_closure *cl = NULL;
    while (head) {
        b = head->buffer->con;
        assert(b != NULL);
        cl = (struct client_closure *) b->st;
        assert(cl != NULL);
        cl->filter_matched = 0;
        if (cl->debug_state == 4) {
            ++cl->in_arp_pkts;
        }

        copy_packet_to_user(head->buffer, data, len);
        head = head->next;
    }

    // Forwarding it to netd as well.
    struct buffer_descriptor *buffer = ((struct client_closure *)
                                        (netd[RECEIVE_CONNECTION]->st))->
      buffer_ptr;


#if TRACE_ETHERSRV_MODE
    trace_event(TRACE_SUBSYS_NET, TRACE_EVENT_NET_NI_ARP,
                (uint32_t) (uintptr_t) data);
#endif // TRACE_ETHERSRV_MODE

    copy_packet_to_user(buffer, data, len);
}


struct filter *execute_filters(void *data, size_t len)
{
    struct filter *head = rx_filters;
    int res, error;

//      ETHERSRV_DEBUG("Starting the filter matching....\n");
    // TODO: gracefully handle the error cases, although I think
    // it is not really necessary. since it could only mean we have
    // received a corrupted packet.
    while (head) {
        res = execute_filter(head->data, head->len, (uint8_t *) data,
                             len, &error);
        if (res) {
            // FIXME IK: we need some way of testing how precise a match is
            // and take the most precise match (ie with the least wildcards)
            // Currently we just take the most recently added filter as
            // reflected by the order in the list.
            ETHERSRV_DEBUG("##### Filter_id [%" PRIu64 "] type[%" PRIu64
                           "] matched giving buff [%" PRIu64 "]..\n",
                           head->filter_id, head->filter_type,
                           head->buffer->buffer_id);
            return head;
        }
        head = head->next;
    }
    return NULL;
}


void init_ether_control_service(char *service_name)
{
    // FIXME: do I need separate my_service_name for ether_netd services
    // exporting ether_netd interface

    filter_id_counter = 0;
    my_service_name = service_name;
    errval_t err = ether_control_export(NULL, export_ether_control_cb,
                               connect_ether_control_cb, get_default_waitset(),
                               IDC_EXPORT_FLAGS_DEFAULT);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "ethersrv_netd export failed");
        abort();
    }

} // end function: init_ether_control_service


// Checks if packet belongs to specific application and sends to it
static bool handle_application_packet(void *packet, size_t len)
{

    // executing filters to find the relevant buffer
    uint64_t ts = rdtsc();
    struct filter *filter = execute_filters(packet, len);

    if (filter == NULL) {
        // No matching filter
        return false;
    }

    // Matching filter found, sending packet to application
    struct buffer_descriptor *buffer = filter->buffer;
    struct ether_binding *b = buffer->con;
    assert(b != NULL);
    struct client_closure *cl = (struct client_closure *) b->st;
    assert(cl != NULL);

    if (cl->debug_state == 3) {
        // Trigger to start the recording the stats
        assert(cl->in_success == 0);
        printf("Actually starting the tracking!!\n");
        cl->start_ts = rdtsc();
        cl->debug_state = 4;
        interrupt_counter = 0;
        interrupt_loop_counter = 0;
    }
    cl->filter_matched = 1;

    if (filter->paused) {
        // Packet belongs to paused filter
        assert(filter->pause_bufpos < MAX_PAUSE_BUFFER);
        struct bufdesc *bd = &filter->pause_buffer[filter->pause_bufpos++];

        memcpy_fast(bd->pkt_data, packet, len);
        bd->pkt_len = len;
        return true;
    }

    // Normal case
    if (cl->debug_state == 4) {
        ++cl->in_filter_matched;
    }

    bool ret = copy_packet_to_user(buffer, packet, len);
    if (ret) {
        // Packet delivered to the application buffer
        if (cl->debug_state == 4) {
            ++cl->in_filter_matched_p;
            netbench_record_event_simple(bm, RE_USEFUL, ts);
        }
    } else {
        // Could not deliver the packet to application!
        if (cl->debug_state == 4) {
            ++cl->in_filter_matched_f;
            netbench_record_event_simple(bm, RE_DROPPED, ts);
        }
//      printf("A: Copy packet to userspace failed\n");
    }
    return true;
} // end function: handle_application_packet

// Checks if packet is of ARP type
// If YES, then send it to all
static bool handle_arp_packet(void *packet, size_t len)
{
    int32_t error;

    if (arp_filter_rx.data == NULL) {
        return false;
    }

    bool res = execute_filter(arp_filter_rx.data, arp_filter_rx.len,
              (uint8_t *) packet, len, &error);

    if (res) { // we have an arp packet
//      ETHERSRV_DEBUG("ARP packet...\n");
        send_arp_to_all(packet, len);
        return true;
    }

    return false;
} // end function: handle_arp_packet


// give this packet to netd
static bool handle_netd_packet(void *packet, size_t len)
{
//  ETHERSRV_DEBUG("No client wants, giving it to netd\n");
    struct buffer_descriptor *buffer = ((struct client_closure *)
              (netd[RECEIVE_CONNECTION]->st))->buffer_ptr;

//    ETHERSRV_DEBUG("sending packet up.\n");
    /* copy the packet to userspace */
    if(buffer == NULL) {
        printf("netd buffer not present\n");
        return false;
    }

    struct ether_binding *b = buffer->con;
    if(b == NULL) {
        printf("netd buffer->con not present\n");
        return false;
    }

    struct client_closure *cl = (struct client_closure *)b->st;
    assert(cl != NULL);
    if (cl->debug_state == 4) {
        ++cl->in_netd_pkts;
    }
    if (copy_packet_to_user(buffer, packet, len) == false) {
        ETHERSRV_DEBUG("Copy packet to userspace failed\n");
    }
    return true;
} // end function: handle_netd_packet


void process_received_packet(void *pkt_data, size_t pkt_len)
{

#if TRACE_ETHERSRV_MODE
    uint32_t pkt_location = (uint32_t) ((uintptr_t) pkt_data);
    trace_event(TRACE_SUBSYS_NET, TRACE_EVENT_NET_NI_A, pkt_location);
#endif // TRACE_ETHERSRV_MODE

    // check for fragmented packet
    if (handle_fragmented_packet(pkt_data, pkt_len)) {
        ETHERSRV_DEBUG("fragmented packet..\n");
//        printf("fragmented packet..\n");
        return;
    }

    // check for application specific packet
    if (handle_application_packet(pkt_data, pkt_len)) {
        ETHERSRV_DEBUG("application specific packet..\n");
        return;
    }

    // check for ARP packet
     if (handle_arp_packet(pkt_data, pkt_len)) {
        ETHERSRV_DEBUG("ARP packet..\n");
        return;
    }

     // last resort: send packet to netd
     handle_netd_packet(pkt_data, pkt_len);

} // end function: process_received_packet


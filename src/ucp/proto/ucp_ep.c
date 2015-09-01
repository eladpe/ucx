/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include "ucp_int.h"

#include <ucs/debug/memtrack.h>
#include <ucs/debug/log.h>
#include <string.h>


ucs_status_t ucp_ep_new(ucp_worker_h worker, uint64_t dest_uuid,
                        const char *message, ucp_ep_h *ep_p)
{
    ucp_ep_h ep;

    ep = ucs_calloc(1, sizeof(*ep), "ucp ep");
    if (ep == NULL) {
        ucs_error("Failed to allocate ep");
        return UCS_ERR_NO_MEMORY;
    }

    ep->worker               = worker;
    ep->uct_ep               = NULL;
    ep->config.max_short_tag = SIZE_MAX;
    ep->config.max_short_put = SIZE_MAX;
    ep->config.max_bcopy_put = SIZE_MAX;
    ep->config.max_bcopy_get = SIZE_MAX;
    ep->dest_uuid            = dest_uuid;
    ep->rsc_index            = -1;
    ep->dst_pd_index         = -1;
    ep->state                = 0;
    ep->wireup.aux_ep        = NULL;
    ep->wireup.next_ep       = NULL;

    sglib_hashed_ucp_ep_t_add(worker->ep_hash, ep);

    ucs_debug("created ep %p 0x%"PRIx64"->0x%"PRIx64" %s", ep, worker->uuid,
              ep->dest_uuid, message);
    *ep_p = ep;
    return UCS_OK;
}

void ucp_ep_ready_to_send(ucp_ep_h ep)
{
    ucp_worker_h worker          = ep->worker;
    uct_iface_attr_t *iface_attr = &worker->iface_attrs[ep->rsc_index];

    ucs_debug("connected 0x%"PRIx64"->0x%"PRIx64, worker->uuid, ep->dest_uuid);

    ep->state               |= UCP_EP_STATE_READY_TO_SEND;
    ep->config.max_short_tag = iface_attr->cap.am.max_short - sizeof(uint64_t);
    ep->config.max_short_put = iface_attr->cap.put.max_short;
    ep->config.max_bcopy_put = iface_attr->cap.put.max_bcopy;
    ep->config.max_bcopy_get = iface_attr->cap.get.max_bcopy;
}

static ucs_status_t ucp_pending_req_release(uct_pending_req_t *self)
{
    ucp_ep_pending_op_t *op = ucs_container_of(self, ucp_ep_pending_op_t, uct);
    ucs_free(op);
    return UCS_OK;
}

void ucp_ep_destroy_uct_ep_safe(ucp_ep_h ep, uct_ep_h uct_ep)
{
    ucs_assert_always(uct_ep != NULL);

    uct_ep_pending_purge(uct_ep, ucp_pending_req_release);

    while (uct_ep_flush(uct_ep) != UCS_OK) {
        ucp_worker_progress(ep->worker);
    }
    uct_ep_destroy(uct_ep);
}

ucs_status_t ucp_ep_create(ucp_worker_h worker, ucp_address_t *address,
                           ucp_ep_h *ep_p)
{
    uint64_t dest_uuid = ucp_address_uuid(address);
    ucs_status_t status;
    ucp_ep_h ep;

    UCS_ASYNC_BLOCK(&worker->async);

    ep = ucp_worker_find_ep(worker, dest_uuid);
    if (ep != NULL) {
        ucs_debug("returning existing ep %p which is already connected to %"PRIx64,
                  ep, ep->dest_uuid);
        goto out;
    }

    status = ucp_ep_new(worker, dest_uuid, " from api call", &ep);
    if (status != UCS_OK) {
        goto err;
    }

    status = ucp_wireup_start(ep, address);
    if (status != UCS_OK) {
        goto err_free;
    }

out:
    UCS_ASYNC_UNBLOCK(&worker->async);
    *ep_p = ep;
    return UCS_OK;

err_free:
    ucs_free(ep);
err:
    UCS_ASYNC_UNBLOCK(&worker->async);
    return status;
}

void ucp_ep_destroy(ucp_ep_h ep)
{
    ucs_debug("destroy ep %p", ep);
    ucp_wireup_stop(ep);
    ucp_ep_destroy_uct_ep_safe(ep, ep->uct_ep);
    ucs_free(ep);
}

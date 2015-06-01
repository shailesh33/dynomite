/*
 * Dynomite - A thin, distributed replication layer for multi non-distributed storages.
 * Copyright (C) 2014 Netflix, Inc.
 */ 

#include "dyn_core.h"
#include "dyn_dnode_peer.h"


struct msg *
dnode_rsp_get(struct conn *conn)
{
    struct msg *msg;

    ASSERT(!conn->dnode_client && !conn->dnode_server);

    msg = msg_get(conn, false, conn->redis);
    if (msg == NULL) {
        conn->err = errno;
    }

    return msg;
}

void
dnode_rsp_put(struct msg *msg)
{
    ASSERT(!msg->request);
    ASSERT(msg->peer == NULL);
    msg_put(msg);
}


struct msg *
dnode_rsp_recv_next(struct context *ctx, struct conn *conn, bool alloc)
{
    ASSERT(!conn->dnode_client && !conn->dnode_server);

    conn->last_received = time(NULL);

    return rsp_recv_next(ctx, conn, alloc);
}

static bool
dnode_rsp_filter(struct context *ctx, struct conn *conn, struct msg *msg)
{
    struct msg *pmsg;

    ASSERT(!conn->dnode_client && !conn->dnode_server);

    if (msg_empty(msg)) {
        ASSERT(conn->rmsg == NULL);
        log_debug(LOG_VERB, "dyn: filter empty rsp %"PRIu64" on s %d", msg->id,
                conn->sd);
        dnode_rsp_put(msg);
        return true;
    }

    pmsg = TAILQ_FIRST(&conn->omsg_q);
    if (pmsg == NULL) {
        log_debug(LOG_INFO, "dyn: filter stray rsp %"PRIu64" len %"PRIu32" on s %d noreply %d",
                msg->id, msg->mlen, conn->sd, msg->noreply);
        dnode_rsp_put(msg);
        return true;
    }
    ASSERT(pmsg->peer == NULL);
    ASSERT(pmsg->request && !pmsg->done);

    if (pmsg->swallow) {
        conn->dequeue_outq(ctx, conn, pmsg);
        pmsg->done = 1;
        log_debug(LOG_NOTICE, "conn %p swallow %p", conn, pmsg);
        log_debug(LOG_INFO, "dyn: swallow rsp %"PRIu64" len %"PRIu32" of req "
                "%"PRIu64" on s %d", msg->id, msg->mlen, pmsg->id,
                conn->sd);

        dnode_rsp_put(msg);
        req_put(pmsg);
        return true;
    }

    return false;
}

static void
dnode_rsp_forward_stats(struct context *ctx, struct server *server, struct msg *msg)
{
    ASSERT(!msg->request);
    stats_pool_incr(ctx, server->owner, peer_responses);
    stats_pool_incr_by(ctx, server->owner, peer_response_bytes, msg->mlen);
}


/* Description: link data from a peer connection to a client-facing connection
 * peer_conn: a peer connection
 * msg      : msg with data from the peer connection after parsing
 */
static void
dnode_rsp_forward(struct context *ctx, struct conn *peer_conn, struct msg *rsp)
{
    rstatus_t status;
    struct msg *req;
    struct conn *c_conn;

    ASSERT(!peer_conn->dnode_client && !peer_conn->dnode_server);

    /* response from a peer implies that peer is ok and heartbeating */
    dnode_peer_ok(ctx, peer_conn);

    /* dequeue peer message (request) from peer conn */
    req = TAILQ_FIRST(&peer_conn->omsg_q);
    log_debug(LOG_VERB, "dnode_rsp_forward entering req %p rsp %p...", req, rsp);
    c_conn = req->owner;

    ASSERT(req != NULL && req->peer == NULL);
    ASSERT(req->request && !req->done);

    if (log_loggable(LOG_NOTICE)) {
       loga("Dumping content for msg:   ");
       msg_dump(rsp);

       loga("msg id %d", rsp->id);

       loga("Dumping content for pmsg :");
       msg_dump(req);

       loga("pmsg id %d", req->id);
    }

    peer_conn->dequeue_outq(ctx, peer_conn, req);
    req->done = 1;

    log_debug(LOG_NOTICE, "%p <-> %p", req, rsp);
    /* establish rsp <-> req (response <-> request) link */
    req->peer = rsp;
    rsp->peer = req;

    rsp->pre_coalesce(rsp);

    ASSERT((c_conn->client && !c_conn->proxy) || (c_conn->dnode_client && !c_conn->dnode_server));

    dnode_rsp_forward_stats(ctx, peer_conn->owner, rsp);
    if (TAILQ_FIRST(&c_conn->omsg_q) != NULL && dnode_req_done(c_conn, TAILQ_FIRST(&c_conn->omsg_q))) {
        log_debug(LOG_NOTICE, "handle rsp %d:%d for conn %p", rsp->id, rsp->parent_id, c_conn);
        // c_conn owns respnse now
        rstatus_t status = conn_handle_response(c_conn, c_conn->type == CONN_CLIENT ? 
                                                req->id : req->parent_id, rsp);
        if (status == DN_OK)  {
            //req_put(req);
            status = event_add_out(ctx->evb, c_conn);
            if (status != DN_OK) {
                c_conn->err = errno;
            }
        }
    }

}



//TODOs: fix this in using dmsg_write with encrypted msgs
//         It is not in use now.
/*
void
dnode_rsp_gos_syn(struct context *ctx, struct conn *p_conn, struct msg *msg)
{
    rstatus_t status;
    struct msg *pmsg;

    //ASSERT(p_conn->dnode_client && !p_conn->dnode_server);

    //add messsage
    struct mbuf *nbuf = mbuf_get();
    if (nbuf == NULL) {
        log_debug(LOG_ERR, "Error happened in calling mbuf_get");
        return;  //TODOs: need to address this further
    }

    msg->done = 1;

    //TODOs: need to free the old msg object
    pmsg = msg_get(p_conn, 0, msg->redis);
    if (pmsg == NULL) {
        mbuf_put(nbuf);
        return;
    }

    pmsg->done = 1;
    // establish msg <-> pmsg (response <-> request) link
    msg->peer = pmsg;
    pmsg->peer = msg;
    pmsg->pre_coalesce(pmsg);
    pmsg->owner = p_conn;

    //dyn message's meta data
    uint64_t msg_id = msg->dmsg->id;
    uint8_t type = GOSSIP_SYN_REPLY;
    struct string data = string("SYN_REPLY_OK");

    dmsg_write(nbuf, msg_id, type, p_conn, 0);
    mbuf_insert(&pmsg->mhdr, nbuf);

    //dnode_rsp_recv_done(ctx, p_conn, msg, pmsg);
    //should we do this?
    //s_conn->dequeue_outq(ctx, s_conn, pmsg);



     //p_conn->enqueue_outq(ctx, p_conn, pmsg);
     //if (TAILQ_FIRST(&p_conn->omsg_q) != NULL && dnode_req_done(p_conn, TAILQ_FIRST(&p_conn->omsg_q))) {
     //   status = event_add_out(ctx->evb, p_conn);
     //   if (status != DN_OK) {
     //      p_conn->err = errno;
     //   }
     //}


    if (TAILQ_FIRST(&p_conn->omsg_q) != NULL && dnode_req_done(p_conn, TAILQ_FIRST(&p_conn->omsg_q))) {
        status = event_add_out(ctx->evb, p_conn);
        if (status != DN_OK) {
            p_conn->err = errno;
        }
    }

    //dnode_rsp_forward_stats(ctx, s_conn->owner, msg);
}

*/

void
dnode_rsp_recv_done(struct context *ctx, struct conn *conn,
                    struct msg *msg, struct msg *nmsg)
{
    log_debug(LOG_VERB, "dnode_rsp_recv_done entering ...");

    ASSERT(!conn->dnode_client && !conn->dnode_server);
    ASSERT(msg != NULL && conn->rmsg == msg);
    ASSERT(!msg->request);
    ASSERT(msg->owner == conn);
    ASSERT(nmsg == NULL || !nmsg->request);

    if (log_loggable(LOG_VVERB)) {
       loga("Dumping content for msg:   ");
       msg_dump(msg);

       if (nmsg != NULL) {
          loga("Dumping content for nmsg :");
          msg_dump(nmsg);
       }
    }

    /* enqueue next message (response), if any */
    conn->rmsg = nmsg;

    if (dnode_rsp_filter(ctx, conn, msg)) {
        return;
    }
    dnode_rsp_forward(ctx, conn, msg);
}


/* dnode sends a response back to a peer  */
struct msg *
dnode_rsp_send_next(struct context *ctx, struct conn *conn)
{
    rstatus_t status;


    ASSERT(conn->dnode_client && !conn->dnode_server);
    struct msg *msg = rsp_send_next(ctx, conn);
    log_debug(LOG_NOTICE, "dnode_rsp_send_next entering %p", msg);

    if (msg != NULL && conn->dyn_mode) {
        struct msg *pmsg = TAILQ_FIRST(&conn->omsg_q); //peer request's msg

        //need to deal with multi-block later
        uint64_t msg_id = pmsg->dmsg->id;

        struct mbuf *header_buf = mbuf_get();
        if (header_buf == NULL) {
            loga("Unable to obtain an mbuf for header!");
            return NULL; //need to address error here properly
        }
        dmsg_type_t msg_type = DMSG_RES;
        //TODOs: need to set the outcoming conn to be secured too if the incoming conn is secured
        if (pmsg->owner->dnode_secured || conn->dnode_secured) {
            if (log_loggable(LOG_VVERB)) {
                log_debug(LOG_VVERB, "Encrypting response ...");
                loga("AES encryption key: %s\n", base64_encode(conn->aes_key, AES_KEYLEN));
            }

            if (ENCRYPTION) {
              status = dyn_aes_encrypt_msg(msg, conn->aes_key);
              if (status == DN_ERROR) {
                    loga("OOM to obtain an mbuf for encryption!");
                    mbuf_put(header_buf);
                    req_put(msg);
                    return NULL;
              }

              if (log_loggable(LOG_VVERB)) {
                   log_debug(LOG_VERB, "#encrypted bytes : %d", status);
              }

              dmsg_write(header_buf, msg_id, msg_type, conn, msg_length(msg));
            } else {
                if (log_loggable(LOG_VVERB)) {
                   log_debug(LOG_VERB, "no encryption on the msg payload");
                }
                dmsg_write(header_buf, msg_id, msg_type, conn, msg_length(msg));
            }

        } else {
            //write dnode header
            dmsg_write(header_buf, msg_id, msg_type, conn, msg_length(msg));
        }

        mbuf_insert_head(&msg->mhdr, header_buf);

        if (log_loggable(LOG_VVERB)) {
            log_hexdump(LOG_VVERB, header_buf->pos, mbuf_length(header_buf), "resp dyn message - header: ");
            msg_dump(msg);
        }

    }

    return msg;
}

void
dnode_rsp_send_done(struct context *ctx, struct conn *conn, struct msg *msg)
{
    if (log_loggable(LOG_VVERB)) {
       log_debug(LOG_VVERB, "dnode_rsp_send_done entering");
   }

    struct msg *pmsg; /* peer message (request) */

    ASSERT(conn->dnode_client && !conn->dnode_server);
    ASSERT(conn->smsg == NULL);

    log_debug(LOG_NOTICE, "dyn: send done rsp %"PRIu64" on c %d", msg->id, conn->sd);

    pmsg = msg->peer;

    ASSERT(!msg->request && pmsg->request);
    ASSERT(pmsg->peer == msg);
    ASSERT(pmsg->done && !pmsg->swallow);

    /* dequeue request from client outq */
    conn->dequeue_outq(ctx, conn, pmsg);

    req_put(pmsg);
}



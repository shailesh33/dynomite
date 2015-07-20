/*
 * Dynomite - A thin, distributed replication layer for multi non-distributed storages.
 * Copyright (C) 2014 Netflix, Inc.
 */ 

/*
 * twemproxy - A fast and lightweight proxy for memcached protocol.
 * Copyright (C) 2011 Twitter, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "dyn_core.h"
#include "dyn_server.h"
#include "dyn_dnode_peer.h"

static rstatus_t msg_write_local_quorum_rsp_handler(struct msg *req, struct msg *rsp);
static rstatus_t msg_read_local_quorum_rsp_handler(struct msg *req, struct msg *rsp);
static rstatus_t msg_read_one_rsp_handler(struct msg *req, struct msg *rsp);

struct msg *
req_get(struct conn *conn)
{
    struct msg *msg;

    ASSERT((conn->client && !conn->proxy) || (conn->dnode_client && !conn->dnode_server));

    msg = msg_get(conn, true, conn->redis);
    if (msg == NULL) {
        conn->err = errno;
    }

    return msg;
}

void
req_put(struct msg *msg)
{
    struct msg *pmsg; /* peer message (response) */

    ASSERT(msg->request);

    pmsg = msg->peer;
    if (pmsg != NULL) {
        ASSERT(!pmsg->request && pmsg->peer == msg);
        msg->peer = NULL;
        pmsg->peer = NULL;
        rsp_put(pmsg);
    }

    msg_tmo_delete(msg);

    msg_put(msg);
}

/*
 * Return true if request is done, false otherwise
 *
 * A request is done, if we received response for the given request.
 * A request vector is done if we received responses for all its
 * fragments.
 */
bool
req_done(struct conn *conn, struct msg *msg)
{
    struct msg *cmsg, *pmsg; /* current and previous message */
    uint64_t id;             /* fragment id */
    uint32_t nfragment;      /* # fragment */

    ASSERT((conn->client && !conn->proxy) || (conn->dnode_client && !conn->dnode_server));

    if (msg == NULL || !msg->done) {
        return false;
    }

    ASSERT(msg->request);

    id = msg->frag_id;
    if (id == 0) {
        return true;
    }

    if (msg->fdone) {
        /* request has already been marked as done */
        return true;
    }

    /* check all fragments of the given request vector are done */

    for (pmsg = msg, cmsg = TAILQ_PREV(msg, msg_tqh, c_tqe);
         cmsg != NULL && cmsg->frag_id == id;
         pmsg = cmsg, cmsg = TAILQ_PREV(cmsg, msg_tqh, c_tqe)) {

        if (!cmsg->done) {
            return false;
        }
    }

    for (pmsg = msg, cmsg = TAILQ_NEXT(msg, c_tqe);
         cmsg != NULL && cmsg->frag_id == id;
         pmsg = cmsg, cmsg = TAILQ_NEXT(cmsg, c_tqe)) {

        if (!cmsg->done) {
            return false;
        }
    }

    if (!pmsg->last_fragment) {
        return false;
    }

    /*
     * At this point, all the fragments including the last fragment have
     * been received.
     *
     * Mark all fragments of the given request vector to be done to speed up
     * future req_done calls for any of fragments of this request
     */

    msg->fdone = 1;
    nfragment = 1;

    for (pmsg = msg, cmsg = TAILQ_PREV(msg, msg_tqh, c_tqe);
         cmsg != NULL && cmsg->frag_id == id;
         pmsg = cmsg, cmsg = TAILQ_PREV(cmsg, msg_tqh, c_tqe)) {
        cmsg->fdone = 1;
        nfragment++;
    }

    for (pmsg = msg, cmsg = TAILQ_NEXT(msg, c_tqe);
         cmsg != NULL && cmsg->frag_id == id;
         pmsg = cmsg, cmsg = TAILQ_NEXT(cmsg, c_tqe)) {
        cmsg->fdone = 1;
        nfragment++;
    }

    ASSERT(msg->frag_owner->nfrag == nfragment);

    msg->post_coalesce(msg->frag_owner);

    log_debug(LOG_DEBUG, "req from c %d with fid %"PRIu64" and %"PRIu32" "
              "fragments is done", conn->sd, id, nfragment);

    return true;
}

/*
 * Return true if request is in error, false otherwise
 *
 * A request is in error, if there was an error in receiving response for the
 * given request. A multiget request is in error if there was an error in
 * receiving response for any its fragments.
 */
bool
req_error(struct conn *conn, struct msg *msg)
{
    struct msg *cmsg; /* current message */
    uint64_t id;
    uint32_t nfragment;

    ASSERT(msg->request && req_done(conn, msg));

    if (msg->error) {
        return true;
    }

    id = msg->frag_id;
    if (id == 0) {
        return false;
    }

    if (msg->ferror) {
        /* request has already been marked to be in error */
        return true;
    }

    /* check if any of the fragments of the given request are in error */

    for (cmsg = TAILQ_PREV(msg, msg_tqh, c_tqe);
         cmsg != NULL && cmsg->frag_id == id;
         cmsg = TAILQ_PREV(cmsg, msg_tqh, c_tqe)) {

        if (cmsg->error) {
            goto ferror;
        }
    }

    for (cmsg = TAILQ_NEXT(msg, c_tqe);
         cmsg != NULL && cmsg->frag_id == id;
         cmsg = TAILQ_NEXT(cmsg, c_tqe)) {

        if (cmsg->error) {
            goto ferror;
        }
    }

    return false;

ferror:

    /*
     * Mark all fragments of the given request to be in error to speed up
     * future req_error calls for any of fragments of this request
     */

    msg->ferror = 1;
    nfragment = 1;

    for (cmsg = TAILQ_PREV(msg, msg_tqh, c_tqe);
         cmsg != NULL && cmsg->frag_id == id;
         cmsg = TAILQ_PREV(cmsg, msg_tqh, c_tqe)) {
        cmsg->ferror = 1;
        nfragment++;
    }

    for (cmsg = TAILQ_NEXT(msg, c_tqe);
         cmsg != NULL && cmsg->frag_id == id;
         cmsg = TAILQ_NEXT(cmsg, c_tqe)) {
        cmsg->ferror = 1;
        nfragment++;
    }

    log_debug(LOG_DEBUG, "req from c %d with fid %"PRIu64" and %"PRIu32" "
              "fragments is in error", conn->sd, id, nfragment);

    return true;
}

void
req_server_enqueue_imsgq(struct context *ctx, struct conn *conn, struct msg *msg)
{
    ASSERT(msg->request);
    ASSERT((!conn->client && !conn->proxy) || (!conn->dnode_client && !conn->dnode_server));

    /*
     * timeout clock starts ticking the instant the message is enqueued into
     * the server in_q; the clock continues to tick until it either expires
     * or the message is dequeued from the server out_q
     *
     * noreply request are free from timeouts because client is not interested
     * in the reponse anyway!
     */
    if (!msg->noreply) {
        msg_tmo_insert(msg, conn);
    }

    TAILQ_INSERT_TAIL(&conn->imsg_q, msg, s_tqe);
    log_debug(LOG_VERB, "conn %p enqueue inq %d:%d", conn, msg->id, msg->parent_id);

    if (!conn->dyn_mode) {
       stats_server_incr(ctx, conn->owner, in_queue);
       stats_server_incr_by(ctx, conn->owner, in_queue_bytes, msg->mlen);
    } else {
       struct server_pool *pool = (struct server_pool *) array_get(&ctx->pool, 0);
       stats_pool_incr(ctx, pool, peer_in_queue);
       stats_pool_incr_by(ctx, pool, peer_in_queue_bytes, msg->mlen);
    }
}

void
req_server_dequeue_imsgq(struct context *ctx, struct conn *conn, struct msg *msg)
{
    ASSERT(msg->request);
    ASSERT(!conn->client && !conn->proxy);

    TAILQ_REMOVE(&conn->imsg_q, msg, s_tqe);
    log_debug(LOG_VERB, "conn %p dequeue inq %d:%d", conn, msg->id, msg->parent_id);

    stats_server_decr(ctx, conn->owner, in_queue);
    stats_server_decr_by(ctx, conn->owner, in_queue_bytes, msg->mlen);
}

void
req_client_enqueue_omsgq(struct context *ctx, struct conn *conn, struct msg *msg)
{
    ASSERT(msg->request);
    ASSERT(conn->client && !conn->proxy);
    msg->stime_in_microsec = dn_usec_now();

    TAILQ_INSERT_TAIL(&conn->omsg_q, msg, c_tqe);
    log_debug(LOG_VERB, "conn %p enqueue outq %d:%d", conn, msg->id, msg->parent_id);
}

void
req_server_enqueue_omsgq(struct context *ctx, struct conn *conn, struct msg *msg)
{
    ASSERT(msg->request);
    ASSERT(!conn->client && !conn->proxy);

    TAILQ_INSERT_TAIL(&conn->omsg_q, msg, s_tqe);
    log_debug(LOG_VERB, "conn %p enqueue outq %d:%d", conn, msg->id, msg->parent_id);

    stats_server_incr(ctx, conn->owner, out_queue);
    stats_server_incr_by(ctx, conn->owner, out_queue_bytes, msg->mlen);
}

void
req_client_dequeue_omsgq(struct context *ctx, struct conn *conn, struct msg *msg)
{
    ASSERT(msg->request);
    ASSERT(conn->client && !conn->proxy);

    uint64_t latency = dn_usec_now() - msg->stime_in_microsec;
    stats_histo_add_latency(ctx, latency);
    TAILQ_REMOVE(&conn->omsg_q, msg, c_tqe);
    log_debug(LOG_VERB, "conn %p dequeue outq %p", conn, msg);
}

void
req_server_dequeue_omsgq(struct context *ctx, struct conn *conn, struct msg *msg)
{
    ASSERT(msg->request);
    ASSERT(!conn->client && !conn->proxy);

    msg_tmo_delete(msg);

    TAILQ_REMOVE(&conn->omsg_q, msg, s_tqe);
    log_debug(LOG_VERB, "conn %p dequeue outq %d:%d", conn, msg->id, msg->parent_id);

    stats_server_decr(ctx, conn->owner, out_queue);
    stats_server_decr_by(ctx, conn->owner, out_queue_bytes, msg->mlen);
}

struct msg *
req_recv_next(struct context *ctx, struct conn *conn, bool alloc)
{
    struct msg *msg;

    ASSERT((conn->client && !conn->proxy) || (conn->dnode_client && !conn->dnode_server));

    if (conn->eof) {
        msg = conn->rmsg;

        //if (conn->dyn_mode) {
        //    if (conn->non_bytes_recv > MAX_CONN_ALLOWABLE_NON_RECV) {
        //        conn->err = EPIPE;
        //        return NULL;
        //    }
        //    conn->eof = 0;
        //    return msg;
        //}

        /* client sent eof before sending the entire request */
        if (msg != NULL) {
            conn->rmsg = NULL;

            ASSERT(msg->peer == NULL);
            ASSERT(msg->request && !msg->done);

            log_error("eof c %d discarding incomplete req %"PRIu64" len "
                      "%"PRIu32"", conn->sd, msg->id, msg->mlen);

            req_put(msg);
        }

        /*
         * TCP half-close enables the client to terminate its half of the
         * connection (i.e. the client no longer sends data), but it still
         * is able to receive data from the proxy. The proxy closes its
         * half (by sending the second FIN) when the client has no
         * outstanding requests
         */
        if (!conn->active(conn)) {
            conn->done = 1;
            log_debug(LOG_INFO, "c %d is done", conn->sd);
        }

        return NULL;
    }

    msg = conn->rmsg;
    if (msg != NULL) {
        ASSERT(msg->request);
        return msg;
    }

    if (!alloc) {
        return NULL;
    }

    msg = req_get(conn);
    if (msg != NULL) {
        conn->rmsg = msg;
    }

    return msg;
}

static bool
req_filter(struct context *ctx, struct conn *conn, struct msg *msg)
{
    ASSERT(conn->client && !conn->proxy);

    if (msg_empty(msg)) {
        ASSERT(conn->rmsg == NULL);
        log_debug(LOG_VERB, "filter empty req %"PRIu64" from c %d", msg->id,
                  conn->sd);
        req_put(msg);
        return true;
    }

    /*
     * Handle "quit\r\n", which is the protocol way of doing a
     * passive close
     */
    if (msg->quit) {
        ASSERT(conn->rmsg == NULL);
        log_debug(LOG_INFO, "filter quit req %"PRIu64" from c %d", msg->id,
                  conn->sd);
        conn->eof = 1;
        conn->recv_ready = 0;
        req_put(msg);
        return true;
    }

    return false;
}

static void
req_forward_error(struct context *ctx, struct conn *conn, struct msg *msg)
{
    rstatus_t status;

    if (log_loggable(LOG_INFO)) {
       log_debug(LOG_INFO, "forward req %"PRIu64" len %"PRIu32" type %d from "
                 "c %d failed: %s", msg->id, msg->mlen, msg->type, conn->sd,
                 strerror(errno));
    }

    msg->done = 1;
    msg->error = 1;
    msg->err = errno;

    /* noreply request don't expect any response */
    if (msg->noreply) {
        req_put(msg);
        return;
    }

    if (req_done(conn, TAILQ_FIRST(&conn->omsg_q))) {
        status = event_add_out(ctx->evb, conn);
        if (status != DN_OK) {
            conn->err = errno;
        }
    }

}

static void
req_forward_stats(struct context *ctx, struct server *server, struct msg *msg)
{
    ASSERT(msg->request);

    if (msg->is_read) {
       stats_server_incr(ctx, server, read_requests);
       stats_server_incr_by(ctx, server, read_request_bytes, msg->mlen);
    } else {
       stats_server_incr(ctx, server, write_requests);
       stats_server_incr_by(ctx, server, write_request_bytes, msg->mlen);
    }
}

void
local_req_forward(struct context *ctx, struct conn *c_conn, struct msg *msg,
                  uint8_t *key, uint32_t keylen)
{
    rstatus_t status;
    struct conn *s_conn;

    if (log_loggable(LOG_VVERB)) {
       loga("local_req_forward entering ............");
    }

    ASSERT((c_conn->client || c_conn->dnode_client) && !c_conn->proxy &&
           !c_conn->dnode_server);

    /* enqueue message (request) into client outq, if response is expected */
    if (!msg->noreply) {
        c_conn->enqueue_outq(ctx, c_conn, msg);
    }

    s_conn = server_pool_conn(ctx, c_conn->owner, key, keylen);
    log_debug(LOG_VERB, "c_conn %p got server conn %p", c_conn, s_conn);
    if (s_conn == NULL) {
        req_forward_error(ctx, c_conn, msg);
        return;
    }
    ASSERT(!s_conn->client && !s_conn->proxy);

    if (log_loggable(LOG_DEBUG)) {
       log_debug(LOG_DEBUG, "forwarding request from client conn '%s' to storage conn '%s'",
                    dn_unresolve_peer_desc(c_conn->sd), dn_unresolve_peer_desc(s_conn->sd));
    }

    if (ctx->dyn_state == NORMAL) {
        /* enqueue the message (request) into server inq */
        if (TAILQ_EMPTY(&s_conn->imsg_q)) {
            status = event_add_out(ctx->evb, s_conn);

            if (status != DN_OK) {
                req_forward_error(ctx, c_conn, msg);
                s_conn->err = errno;
                return;
            }
        }
    } else if (ctx->dyn_state == STANDBY) {  //no reads/writes from peers/clients
        log_debug(LOG_INFO, "Node is in STANDBY state. Drop write/read requests");
        req_forward_error(ctx, c_conn, msg);
        return;
    } else if (ctx->dyn_state == WRITES_ONLY && msg->is_read) {
        //no reads from peers/clients but allow writes from peers/clients
        log_debug(LOG_INFO, "Node is in WRITES_ONLY state. Drop read requests");
        req_forward_error(ctx, c_conn, msg);
        return;
    } else if (ctx->dyn_state == RESUMING) {
        log_debug(LOG_INFO, "Node is in RESUMING state. Still drop read requests and flush out all the queued writes");
        if (msg->is_read) {
            req_forward_error(ctx, c_conn, msg);
            return;
        }

        status = event_add_out(ctx->evb, s_conn);

        if (status != DN_OK) {
            req_forward_error(ctx, c_conn, msg);
            s_conn->err = errno;
            return;
        }
    }

    s_conn->enqueue_inq(ctx, s_conn, msg);
    req_forward_stats(ctx, s_conn->owner, msg);


    if (log_loggable(LOG_VERB)) {
       log_debug(LOG_VERB, "local forward from c %d to s %d req %"PRIu64" len %"PRIu32
                " type %d with key '%.*s'", c_conn->sd, s_conn->sd, msg->id,
                msg->mlen, msg->type, keylen, key);
    }
}


static bool
request_send_to_all_racks(struct msg *msg)
{
    if (!msg->is_read)
        return true;
    if (msg->consistency == LOCAL_QUORUM)
        return true;
    return false;
}

static void
send_rsp_integer(struct context *ctx, struct conn *c_conn, struct msg *msg)
{
    //do nothing
    struct msg *nmsg = msg_get_rsp_integer(true);
    if (!msg->noreply)
        c_conn->enqueue_outq(ctx, c_conn, msg);
    msg->peer = nmsg;
    nmsg->peer = msg;

    msg->done = 1;
    //msg->pre_coalesce(msg);
    rstatus_t status = event_add_out(ctx->evb, c_conn);
    IGNORE_RET_VAL(status);
}

static void
admin_local_req_forward(struct context *ctx, struct conn *c_conn, struct msg *msg,
                        struct rack *rack, uint8_t *key, uint32_t keylen)
{
    struct conn *p_conn;

    ASSERT(c_conn->client || c_conn->dnode_client);

    p_conn = dnode_peer_pool_conn(ctx, c_conn->owner, rack, key, keylen, msg->msg_type);
    if (p_conn == NULL) {
        c_conn->err = EHOSTDOWN;
        req_forward_error(ctx, c_conn, msg);
        return;
    }

    struct server *peer = p_conn->owner;

    if (peer->is_local) {
        send_rsp_integer(ctx, c_conn, msg);
    } else {
        log_debug(LOG_NOTICE, "Need to delete [%.*s] ", keylen, key);
        local_req_forward(ctx, c_conn, msg, key, keylen);
    }
}

void
remote_req_forward(struct context *ctx, struct conn *c_conn, struct msg *msg, 
                        struct rack *rack, uint8_t *key, uint32_t keylen)
{
    struct conn *p_conn;

    ASSERT(c_conn->client || c_conn->dnode_client);

    p_conn = dnode_peer_pool_conn(ctx, c_conn->owner, rack, key, keylen, msg->msg_type);
    if (p_conn == NULL) {
        c_conn->err = EHOSTDOWN;
        req_forward_error(ctx, c_conn, msg);
        return;
    }

    //jeb - check if s_conn is _this_ node, and if so, get conn from server_pool_conn instead
    struct server *peer = p_conn->owner;

    if (peer->is_local) {
        log_debug(LOG_VERB, "c_conn: %p forwarding %d:%d is local", c_conn,
                  msg->id, msg->parent_id);
        local_req_forward(ctx, c_conn, msg, key, keylen);
        return;
    } else {
        log_debug(LOG_VERB, "c_conn: %p forwarding %d:%d to p_conn %p", c_conn,
                  msg->id, msg->parent_id, p_conn);
        dnode_peer_req_forward(ctx, c_conn, p_conn, msg, rack, key, keylen);
    }
}

static void
req_set_read_consistency(struct context *ctx, struct conn *c_conn, struct msg *msg)
{
    // TODO: parse the value and set the consistency appropriately
    // TODO: FOR NOW JUST TOGGLE THE CONSISTENCY
    if (conn_get_read_consistency(c_conn) == LOCAL_ONE)
        conn_set_read_consistency(c_conn, LOCAL_QUORUM);
    else
        conn_set_read_consistency(c_conn, LOCAL_ONE);

    send_rsp_integer(ctx, c_conn, msg);
    
    return;
}

static void
req_set_write_consistency(struct context *ctx, struct conn *c_conn, struct msg *msg)
{
    // TODO: parse the value and set the consistency appropriately
    // TODO: FOR NOW JUST TOGGLE THE CONSISTENCY
    if (conn_get_write_consistency(c_conn) == LOCAL_ONE)
        conn_set_write_consistency(c_conn, LOCAL_QUORUM);
    else
        conn_set_write_consistency(c_conn, LOCAL_ONE);

    send_rsp_integer(ctx, c_conn, msg);
    return;
}

static void
req_set_dyno_consistency(struct context *ctx, struct conn *c_conn, struct msg *msg)
{
    uint32_t keylen = (uint32_t)(msg->key_end - msg->key_start);
    if ((keylen == 4) && !dn_strncasecmp(msg->key_start, "read", 4)) {
        req_set_read_consistency(ctx, c_conn, msg);
        return;
    } else if ((keylen == 5) && !dn_strncasecmp(msg->key_start, "write", 5)) {
        req_set_write_consistency(ctx, c_conn, msg);
        return;
    } else {
        errno = EINVAL;
        if (!msg->noreply)
            c_conn->enqueue_outq(ctx, c_conn, msg);
        req_forward_error(ctx, c_conn, msg);
        return;
    }

}

static void
req_forward(struct context *ctx, struct conn *c_conn, struct msg *msg)
{
    struct server_pool *pool = c_conn->owner;
    uint8_t *key;
    uint32_t keylen;

    ASSERT(c_conn->client && !c_conn->proxy);

    if (msg->is_read)
        stats_pool_incr(ctx, pool, client_read_requests);
    else
        stats_pool_incr(ctx, pool, client_write_requests);

    key = NULL;
    keylen = 0;
    if (msg->type == MSG_REQ_DYNO_CONSISTENCY) {
        req_set_dyno_consistency(ctx, c_conn, msg);
        return;
    }

    // add the message to the dict
    log_debug(LOG_VERB, "conn %p adding message %d:%d", c_conn, msg->id, msg->parent_id);
    dictAdd(c_conn->outstanding_msgs_dict, &msg->id, msg);

    if (!string_empty(&pool->hash_tag)) {
        struct string *tag = &pool->hash_tag;
        uint8_t *tag_start, *tag_end;

        tag_start = dn_strchr(msg->key_start, msg->key_end, tag->data[0]);
        if (tag_start != NULL) {
            tag_end = dn_strchr(tag_start + 1, msg->key_end, tag->data[1]);
            if (tag_end != NULL) {
                key = tag_start + 1;
                keylen = (uint32_t)(tag_end - key);
            }
        }
    }

    if (keylen == 0) {
        key = msg->key_start;
        keylen = (uint32_t)(msg->key_end - msg->key_start);
    }

    // need to capture the initial mbuf location as once we add in the dynomite
    // headers (as mbufs to the src msg), that will bork the request sent to
    // secondary racks
    struct mbuf *orig_mbuf = STAILQ_FIRST(&msg->mhdr);

    if (ctx->admin_opt == 1) {
        if (msg->type == MSG_REQ_REDIS_DEL || msg->type == MSG_REQ_MC_DELETE) {
          struct rack * rack = server_get_rack_by_dc_rack(pool, &pool->rack, &pool->dc);
          admin_local_req_forward(ctx, c_conn, msg, rack, key, keylen);
          return;
        }
    }

    if (msg->is_read) {
        msg->consistency = conn_get_read_consistency(c_conn);
    } else {
        msg->consistency = conn_get_write_consistency(c_conn);
    }

    if (request_send_to_all_racks(msg)) {
        msg->rsp_handler = msg->is_read ? msg_read_local_quorum_rsp_handler:
                                          msg_write_local_quorum_rsp_handler;
        uint32_t dc_cnt = array_n(&pool->datacenters);
        uint32_t dc_index;
        for(dc_index = 0; dc_index < dc_cnt; dc_index++) {
            struct datacenter *dc = array_get(&pool->datacenters, dc_index);
            if (dc == NULL) {
                log_error("Wow, this is very bad, dc is NULL");
                return;
            }

            if (string_compare(dc->name, &pool->dc) == 0) { //send to all local racks
                //log_debug(LOG_DEBUG, "dc name  '%.*s'",
                //            dc->name->len, dc->name->data);
                uint8_t rack_cnt = (uint8_t)array_n(&dc->racks);
                uint8_t rack_index;
                msg->pending_responses = msg->consistency == LOCAL_ONE ? 1 :
                                         rack_cnt;
                msg->quorum_responses = msg->consistency == LOCAL_ONE ? 1 :
                                         (uint8_t)(rack_cnt/2 + 1);
                log_debug(LOG_NOTICE, "same DC racks:%d expect replies %d",
                          rack_cnt, msg->pending_responses);
                for(rack_index = 0; rack_index < rack_cnt; rack_index++) {
                    struct rack *rack = array_get(&dc->racks, rack_index);
                    //log_debug(LOG_DEBUG, "rack name '%.*s'",
                    //            rack->name->len, rack->name->data);
                    struct msg *rack_msg;
                    // clone message even for local node
                    if (string_compare(rack->name, &pool->rack) == 0 ) {
                        rack_msg = msg;
                    } else {
                        rack_msg = msg_get(c_conn, msg->request, msg->redis);
                        if (rack_msg == NULL) {
                            log_debug(LOG_VERB, "whelp, looks like yer screwed "
                                      "now, buddy. no inter-rack messages for "
                                      "you!");
                            continue;
                        }

                        msg_clone(msg, orig_mbuf, rack_msg);
                        log_debug(LOG_VERB, "msg (%d:%d) clone to rack msg (%d:%d)",
                                  msg->id, msg->parent_id, rack_msg->id, rack_msg->parent_id);
                        rack_msg->swallow = true;
                    }

                    if (log_loggable(LOG_DEBUG)) {
                       log_debug(LOG_DEBUG, "forwarding request to conn '%s' on rack '%.*s'",
                               dn_unresolve_peer_desc(c_conn->sd), rack->name->len, rack->name->data);
                    }
                    log_debug(LOG_VERB, "c_conn: %p forwarding (%d:%d)",
                              c_conn, rack_msg->id, rack_msg->parent_id);
                    remote_req_forward(ctx, c_conn, rack_msg, rack, key, keylen);
                }
            } else {
                uint32_t rack_cnt = array_n(&dc->racks);
                if (rack_cnt == 0)
                    continue;

                uint32_t ran_index = (uint32_t)rand() % rack_cnt;
                struct rack *rack = array_get(&dc->racks, ran_index);

                struct msg *rack_msg = msg_get(c_conn, msg->request, msg->redis);
                if (rack_msg == NULL) {
                    log_debug(LOG_VERB, "whelp, looks like yer screwed now, buddy. no inter-rack messages for you!");
                    msg_put(rack_msg);
                    continue;
                }

                msg_clone(msg, orig_mbuf, rack_msg);
                rack_msg->swallow = true;

                if (log_loggable(LOG_DEBUG)) {
                   log_debug(LOG_DEBUG, "forwarding request to conn '%s' on rack '%.*s'",
                           dn_unresolve_peer_desc(c_conn->sd), rack->name->len, rack->name->data);
                }
                remote_req_forward(ctx, c_conn, rack_msg, rack, key, keylen);
            }
        }
    } else { //for read only requests
        msg->rsp_handler = msg_read_one_rsp_handler;
        struct rack * rack = server_get_rack_by_dc_rack(pool, &pool->rack, &pool->dc);
        remote_req_forward(ctx, c_conn, msg, rack, key, keylen);
    }
}


void
req_recv_done(struct context *ctx, struct conn *conn,
              struct msg *msg, struct msg *nmsg)
{
    ASSERT(conn->client && !conn->proxy);
    ASSERT(msg->request);
    ASSERT(msg->owner == conn);
    ASSERT(conn->rmsg == msg);
    ASSERT(nmsg == NULL || nmsg->request);

    stats_histo_add_payloadsize(ctx, msg->mlen);

    /* enqueue next message (request), if any */
    conn->rmsg = nmsg;

    if (req_filter(ctx, conn, msg)) {
        return;
    }

    req_forward(ctx, conn, msg);
}

struct msg *
req_send_next(struct context *ctx, struct conn *conn)
{
    rstatus_t status;
    struct msg *msg, *nmsg; /* current and next message */

    ASSERT((!conn->client && !conn->proxy) || (!conn->dnode_client && !conn->dnode_server));

    if (conn->connecting) {
        if (!conn->dyn_mode && !conn->client) {
           server_connected(ctx, conn);
        } else if (conn->dyn_mode && !conn->dnode_client) {
           dnode_peer_connected(ctx, conn);
        }
    }

    nmsg = TAILQ_FIRST(&conn->imsg_q);
    if (nmsg == NULL) {
        /* nothing to send as the server inq is empty */
        status = event_del_out(ctx->evb, conn);
        if (status != DN_OK) {
            conn->err = errno;
        }

        return NULL;
    }

    msg = conn->smsg;
    if (msg != NULL) {
        ASSERT(msg->request && !msg->done);
        nmsg = TAILQ_NEXT(msg, s_tqe);
    }

    conn->smsg = nmsg;

    if (nmsg == NULL) {
        return NULL;
    }

    ASSERT(nmsg->request && !nmsg->done);

    if (log_loggable(LOG_VVERB)) {
       log_debug(LOG_VVERB, "send next req %"PRIu64" len %"PRIu32" type %d on "
                "s %d", nmsg->id, nmsg->mlen, nmsg->type, conn->sd);
    }

    return nmsg;
}

void
req_send_done(struct context *ctx, struct conn *conn, struct msg *msg)
{
    ASSERT((!conn->client && !conn->proxy) || (!conn->dnode_client && !conn->dnode_server));
    ASSERT(msg != NULL && conn->smsg == NULL);
    ASSERT(msg->request && !msg->done);
    //ASSERT(msg->owner == conn);

    if (log_loggable(LOG_VVERB)) {
       log_debug(LOG_VVERB, "send done req %"PRIu64" len %"PRIu32" type %d on "
                "s %d", msg->id, msg->mlen, msg->type, conn->sd);
    }

    /* dequeue the message (request) from server inq */
    conn->dequeue_inq(ctx, conn, msg);

    /*
     * noreply request instructs the server not to send any response. So,
     * enqueue message (request) in server outq, if response is expected.
     * Otherwise, free the noreply request
     */
    if (!msg->noreply) {
        conn->enqueue_outq(ctx, conn, msg);
    } else {
        if (!conn->dyn_mode && !conn->client && !conn->proxy) { //still enqueue if it is storage conn
            conn->enqueue_outq(ctx, conn, msg);
        } else {
            req_put(msg);
        }
    }

}

static rstatus_t
msg_read_one_rsp_handler(struct msg *req, struct msg *rsp)
{
    if (req->peer)
        log_warn("Received more than one response for local_one. req: %d:%d \
                 prev rsp %d:%d new rsp %d:%d", req->id, req->parent_id,
                 req->peer->id, req->peer->parent_id, rsp->id, rsp->parent_id);
    req->peer = rsp;
    rsp->peer = req;
    return DN_OK;
}


static rstatus_t
msg_read_local_quorum_rsp_handler(struct msg *req, struct msg *rsp)
{
    int i;

    for (i = 0; i < MAX_REPLICAS_PER_DC; i++) {
        if (req->responses[i])
            continue;
        // empty slot.
        req->responses[i] = rsp;
        --req->pending_responses;
        if (!req->quorum_responses || !--req->quorum_responses)
            break;
        log_notice("Received a response %d:%d for req %d:%d need %d more",
                   rsp->id, rsp->parent_id, req->id, req->parent_id,
                   req->pending_responses);
        return DN_EAGAIN;
    }
    int received_responses = i + 1;
    log_notice("Received %d responses for req %d:%d", received_responses,
               req->id, req->parent_id);

    /* I would have done a better job here but we are having only three replicas.
     * Added a Static assert here just in case */
    STATIC_ASSERT(MAX_REPLICAS_PER_DC == 3, "This code should change");
    uint32_t chk0, chk1, chk2;
    int selected_rsp_idx = -1;
    chk0 = msg_payload_crc32(req->responses[0]);
    chk1 = msg_payload_crc32(req->responses[1]);
    if (chk0 == chk1) {
        // return the response now;
        selected_rsp_idx = 0;
        goto rsp_selected;
    } else {
        if (req->pending_responses) {
            log_notice("quorum responses receveid do not match. waiting for "\
                       "pending responses for req %d:%d", req->id, req->parent_id);
            return DN_EAGAIN;
        }
        // no pending responses we received 2 or 3 responses
        selected_rsp_idx = 0;
        if (received_responses > 2) {
            // compare third guy
            chk2 = msg_payload_crc32(req->responses[2]);
            if (chk1 == chk2) {
                selected_rsp_idx = 1;
                goto rsp_selected;
            } else if (chk0 == chk2) {
                selected_rsp_idx = 0;
                goto rsp_selected;
            }
        }
        log_warn("none of the responses match for req %d:%d returning first",
                 req->id, req->parent_id);
        selected_rsp_idx = 0;
        goto rsp_selected;
    }

rsp_selected:
    for (i = 0; i < MAX_REPLICAS_PER_DC; i++)
        if (i !=  selected_rsp_idx)
            rsp_put(req->responses[i]);

    req->peer = req->responses[selected_rsp_idx];
    req->responses[selected_rsp_idx]->peer = req;

    return DN_OK;
}

static rstatus_t
msg_write_local_quorum_rsp_handler(struct msg *req, struct msg *rsp)
{
    // we own the response. We will free it when done.
    // the first guy wins. Others are put to rest
    if (req->peer) {
        log_notice("putting response %d:%d for request %d:%d",
                   rsp->id, rsp->parent_id, req->id, req->parent_id);
        rsp_put(rsp);
    } else {
        req->peer = rsp; 
        rsp->peer = req; 
        log_notice("accept response %d:%d for request %d:%d",
                   rsp->id, rsp->parent_id, req->id, req->parent_id);
    }

    if(--req->quorum_responses) {
        log_notice("msg %d:%d received response %d:%d need %d more",
                   req->id, req->parent_id, rsp->id, rsp->parent_id,
                   req->quorum_responses);
        return DN_EAGAIN;
    }
    log_notice("msg %d received all responses", req->id);
    return DN_OK;
}

/*
 * InfiniBand BMI method.
 *
 * Copyright (C) 2003-4 Pete Wyckoff <pw@osc.edu>
 *
 * See COPYING in top-level directory.
 *
 * $Id: ib.c,v 1.13 2004-09-29 13:47:55 pw Exp $
 */
#include <stdio.h>  /* just for NULL for id-generator.h */
#include <sys/time.h>
#include <src/common/id-generator/id-generator.h>
#include <src/common/quicklist/quicklist.h>
#include <src/io/bmi/bmi-method-support.h>
#include <src/common/gen-locks/gen-locks.h>
#include <vapi.h>
#include <vapi_common.h>
#include "ib.h"

static gen_mutex_t interface_mutex = GEN_MUTEX_INITIALIZER;

/* alloc space for shared variables */
bmi_size_t EAGER_BUF_PAYLOAD __hidden;
VAPI_hca_hndl_t nic_handle __hidden;
VAPI_cq_hndl_t nic_cq __hidden;
int listen_sock __hidden;
list_t connection __hidden;
list_t sendq __hidden;
list_t recvq __hidden;
VAPI_sg_lst_entry_t *sg_tmp_array __hidden;
int sg_max_len __hidden;
int max_outstanding_wr __hidden;

static int send_cts(ib_recv_t *rq);
static void post_sr(const buf_head_t *bh, u_int32_t len);
/* post_rr declared externally */
static void post_sr_ack(const ib_connection_t *c, const buf_head_t *bh);
static void post_rr_ack(const ib_connection_t *c, const buf_head_t *bh);
static void post_sr_rdmaw(ib_send_t *sq, msg_header_cts_t *mh_cts);

static void encourage_send_waiting_buffer(ib_send_t *sq);
static void encourage_send_incoming_ack(ib_send_t *sq);
static void encourage_send_send_completed(ib_send_t *sq);
static void encourage_send_incoming_cts(buf_head_t *bh, u_int32_t byte_len);

static void encourage_recv_incoming(ib_connection_t *c, buf_head_t *bh,
  u_int32_t byte_len);
static void encourage_recv_incoming_cts_ack(ib_recv_t *rq);
static void encourage_recv_to_send_cts(ib_recv_t *rq);

/*
 * Wander through single completion queue, pulling off messages and
 * sticking them on the proper connection queues.  Later you can
 * walk the incomingq looking for things to do to them.  Returns
 * number of new things that arrived.
 */
static int
check_cq(void)
{
    VAPI_wc_desc_t desc;
    int ret = 0;

    for (;;) {
	int vret = VAPI_poll_cq(nic_handle, nic_cq, &desc);
	if (vret < 0) {
	    if (vret == VAPI_CQ_EMPTY)
		break;
	    error_verrno(vret, "%s: VAPI_poll_cq", __func__);
	}

	debug(2, "%s: found something", __func__);
	++ret;
	if (desc.status != VAPI_SUCCESS) {
	    warning("%s: entry id 0x%Lx opcode %s error %s", __func__,
	      desc.id, VAPI_cqe_opcode_sym(desc.opcode),
	      VAPI_wc_status_sym(desc.status));
	    if (desc.opcode == VAPI_CQE_SQ_SEND_DATA) {
		if (desc.id) {
		    ib_connection_t *c = ptr_from_int64(desc.id);
		    if (c->cancelled)
			debug(0, "%s: ignoring error on cancelled conn %p",
			  __func__, c);
		}
	    }
	}

	if (desc.opcode == VAPI_CQE_RQ_SEND_DATA) {
	    /*
	     * Remote side did a send to us.  Filled one of the receive
	     * queue descriptors, either message or ack.
	     */
	    buf_head_t *bh = ptr_from_int64(desc.id);
	    u_int32_t byte_len = desc.byte_len;

	    if (byte_len == 0) {
		/*
		 * Acknowledgment message on qp_ack.
		 */
		int bufnum = desc.imm_data;
		ib_send_t *sq;

		assert(desc.imm_data_valid, "%s: immediate data is not valid",
		  __func__);
		debug(2, "%s: acknowledgment message %s buf %d",
		  __func__, bh->c->peername, bufnum);
		/* maybe this is okay?  --pw, 6 mar 04 */
		assert(bufnum == bh->num, "%s: ack out of sequence, got %d"
		  " in descriptor for buffer %d", __func__, bufnum, bh->num);
		/*
		 * Do not get the sq from the bh that posted this, just in case
		 * it ever becomes okay to do out-of-order.  Instead look up
		 * the bufnum in the static send array.  This sq will actually
		 * be an rq if the ack is of a CTS.
		 */
		sq = bh->c->eager_send_buf_head_contig[bufnum].sq;
		if (unlikely(sq->type == TYPE_RECV))
		    /* ack of a CTS send by the receiver */
		    encourage_recv_incoming_cts_ack((ib_recv_t *)sq);
		else
		    encourage_send_incoming_ack(sq);

	    } else {
		/*
		 * Some other message: eager send, RTS, CTS, BYE.
		 */
		msg_header_t *mh = bh->buf;

		debug(2, "%s: found len %d at %s bufnum %d type %s",
		  __func__, byte_len, bh->c->peername, bh->num,
		  msg_type_name(mh->type));
		if (mh->type == MSG_CTS) {
		    /* incoming CTS messages go to the send engine */
		    debug(2, "%s: found cts message", __func__);
		    encourage_send_incoming_cts(bh, byte_len);
		} else {
		    /* something for the recv side, no known rq yet */
		    debug(2, "%s: found message for receive engine", __func__);
		    encourage_recv_incoming(bh->c, bh, byte_len);
		}
	    }

	} else if (desc.opcode == VAPI_CQE_SQ_RDMA_WRITE) {

	    /* completion event for the rdma write we initiated, used
	     * to signal memory unpin etc. */
	    ib_send_t *sq = ptr_from_int64(desc.id);
	    encourage_send_send_completed(sq);

	} else if (desc.opcode == VAPI_CQE_SQ_SEND_DATA) {

	    /* XXX: debugging post_sr_ack queue filling up */
	    ib_connection_t *c = ptr_from_int64(desc.id);
	    debug(2, "%s: sr ack to %s send completed", __func__, c->peername);

	} else {
	    const char *ops = VAPI_cqe_opcode_sym(desc.opcode);
	    if (!ops)
		ops = "(null)";
	    error("%s: cq entry id 0x%Lx opcode %s (%d) unexpected", __func__,
	      desc.id, ops, desc.opcode);
	}
    }
    return ret;
}

/*
 * Push a send message along its next step.  Called internally only.
 */
static void
encourage_send_waiting_buffer(ib_send_t *sq)
{
    /*
     * Must get buffers both locally and remote to do an eager send
     * or to initiate an RTS.  Maybe pair these two allocations if it
     * happens frequently.
     */
    buf_head_t *bh;
    msg_header_t *mh;

    debug(2, "%s: sq %p", __func__, sq);
    assert(sq->state == SQ_WAITING_BUFFER, "%s: wrong send state %s",
      __func__, sq_state_name(sq->state));

    bh = qlist_try_del_head(&sq->c->eager_send_buf_free);
    if (!bh) {
	debug(2, "%s: sq %p no free send buffers", __func__, sq);
	return;
    }
    sq->bh = bh;
    bh->sq = sq;  /* uplink for completion */

    if (sq->buflist.tot_len <= EAGER_BUF_PAYLOAD) {
	/*
	 * Eager send.
	 */
	mh = bh->buf;
	mh->type = sq->is_unexpected
	  ? MSG_EAGER_SENDUNEXPECTED : MSG_EAGER_SEND;
	mh->bmi_tag = sq->bmi_tag;

	memcpy_from_buflist(&sq->buflist, mh + 1);

	/* get ready to receive the ack */
	post_rr_ack(sq->c, bh);

	/* send the message */
	post_sr(bh, sizeof(*mh) + sq->buflist.tot_len);

	/* wait for ack saying remote has received and recycled his buf */
	sq->state = SQ_WAITING_EAGER_ACK;
	debug(2, "%s: sq %p sent EAGER now %s", __func__, sq,
	  sq_state_name(sq->state));

    } else {
	/*
	 * Request to send, rendez-vous.  Include the mop id in the message
	 * which will be returned to us in the CTS so we can look it up.
	 */
	msg_header_rts_t *mh_rts;

	mh = bh->buf;
	mh->type = MSG_RTS;
	mh->bmi_tag = sq->bmi_tag;
	mh_rts = (void*)((char *) bh->buf + sizeof(*mh));
	mh_rts->mop_id = sq->mop->op_id;
	mh_rts->tot_len = sq->buflist.tot_len;

	/* get ready to receive the ack */
	post_rr_ack(sq->c, bh);

	post_sr(bh, sizeof(*mh) + sizeof(*mh_rts));

	sq->state = SQ_WAITING_RTS_ACK;
	debug(2, "%s: sq %p sent RTS now %s", __func__, sq,
	  sq_state_name(sq->state));
    }
}

/*
 * Push a send message along its next step.  Called in response to an
 * incoming message (including local acks of messages we sent).
 *
 * bh_in and byte_len only needed for CTS reply to check length
 */
static void
encourage_send_incoming_ack(ib_send_t *sq)
{
    debug(2, "%s: sq %p %s", __func__, sq, sq_state_name(sq->state));

    if (sq->state == SQ_WAITING_EAGER_ACK)
	sq->state = SQ_WAITING_USER_TEST;
    else if (sq->state == SQ_WAITING_RTS_ACK)
	sq->state = SQ_WAITING_CTS;
    else
	error("%s: unknown send state %s", __func__, sq_state_name(sq->state));

    qlist_add_tail(&sq->bh->list, &sq->c->eager_send_buf_free);
    debug(2, "%s: sq %p now %s", __func__, sq, sq_state_name(sq->state));
}

static void
encourage_send_send_completed(ib_send_t *sq)
{
    debug(2, "%s: sq %p %s", __func__, sq, sq_state_name(sq->state));
    assert(sq->state == SQ_WAITING_DATA_LOCAL_SEND_COMPLETE,
      "%s: wrong send state %s", __func__, sq_state_name(sq->state));

    /* re-post and ack cts saved above, signals rdma completed */
    post_rr(sq->c, sq->bh);
    post_sr_ack(sq->c, sq->bh);

    ib_mem_deregister(&sq->buflist);
    sq->state = SQ_WAITING_USER_TEST;
    debug(2, "%s: sq %p now %s", __func__, sq, sq_state_name(sq->state));
}


/*
 * Look at the incoming message which is a response to an earlier RTS
 * from us, and start the real data send.
 */
static void
encourage_send_incoming_cts(buf_head_t *bh, u_int32_t byte_len)
{
    msg_header_t *mh;
    msg_header_cts_t *mh_cts;
    ib_send_t *sq;
    u_int32_t want;
    list_t *l;

    mh = bh->buf;
    mh_cts = (void *)(mh + 1);

    /*
     * Look through this CTS message to determine the owning sq.  Works
     * using the mop_id which was sent during the RTS, now returned to us.
     */
    sq = 0;
    qlist_for_each(l, &sendq) {
	ib_send_t *sqt = (ib_send_t *) l;
	debug(8, "%s: looking for op_id 0x%Lx, consider 0x%Lx", __func__,
	  mh_cts->rts_mop_id, sqt->mop->op_id);
	if (sqt->mop->op_id == (bmi_op_id_t) mh_cts->rts_mop_id) {
	    sq = sqt;
	    break;
	}
    }
    if (!sq)
	error("%s: mop_id %Lx in CTS message not found", __func__,
	  mh_cts->rts_mop_id);

    debug(2, "%s: sq %p %s bh %p len %u", __func__,
      sq, sq_state_name(sq->state), bh, byte_len);
    assert(sq->state == SQ_WAITING_CTS,
      "%s: wrong send state %s", __func__, sq_state_name(sq->state));

    /* message; cts content; list of buffers, lengths, and keys */
    want = sizeof(*mh) + sizeof(*mh_cts)
      + mh_cts->buflist_num * MSG_HEADER_CTS_BUFLIST_ENTRY_SIZE;
    assert(byte_len == want,
      "%s: wrong message size for CTS, got %u, want %u", __func__,
      byte_len, want);

    /* save the bh which received the CTS for later acking */
    sq->bh = bh;

    /* start the big tranfser */
    post_sr_rdmaw(sq, mh_cts);

    sq->state = SQ_WAITING_DATA_LOCAL_SEND_COMPLETE;
    debug(2, "%s: sq %p now %s", __func__, sq, sq_state_name(sq->state));
}


/*
 * See if anything was preposted that matches this.
 */
static ib_recv_t *
find_matching_recv(rq_state_t state, const ib_connection_t *c,
  bmi_msg_tag_t bmi_tag)
{
    list_t *l;

    qlist_for_each(l, &recvq) {
	ib_recv_t *rq = qlist_upcast(l);
	if (rq->state == state && rq->c == c && rq->bmi_tag == bmi_tag)
	    return rq;
    }
    return 0;
}

/*
 * Init a new recvq entry from something that arrived on the wire.
 */
static ib_recv_t *
alloc_new_recv(ib_connection_t *c, buf_head_t *bh)
{
    ib_recv_t *rq = Malloc(sizeof(*rq));
    rq->type = TYPE_RECV;
    rq->c = c;
    rq->bh = bh;
    rq->mop = 0;  /* until user posts for it */
    qlist_add_tail(&rq->list, &recvq);
    return rq;
}

/*
 * Called from incoming message processing, except for the case
 * of ack to a CTS, for which we know the rq (see below).
 *
 * Unexpected receive, either no post or explicit sendunexpected.
 */
static void
encourage_recv_incoming(ib_connection_t *c, buf_head_t *bh, u_int32_t byte_len)
{
    msg_header_t *mh = bh->buf;
    ib_recv_t *rq;

    debug(2, "%s: incoming msg type %s", __func__, msg_type_name(mh->type));

    if (mh->type == MSG_EAGER_SEND) {

	rq = find_matching_recv(RQ_WAITING_INCOMING, c, mh->bmi_tag);
	if (rq) {
	    int len = byte_len - sizeof(*mh);
	    if (len > rq->buflist.tot_len)
		error("%s: EAGER received %d too small for buffer "
		  FORMAT_BMI_SIZE_T,
		  __func__, len, rq->buflist.tot_len);

	    memcpy_to_buflist(&rq->buflist,
	      (char *) bh->buf + sizeof(*mh), len);

	    /* re-post */
	    post_rr(c, bh);
	    /* done with buffer, ack to remote */
	    post_sr_ack(c, bh);
	    rq->state = RQ_EAGER_WAITING_USER_TEST;
	    debug(2, "%s: matched rq %p now %s", __func__, rq,
	      rq_state_name(rq->state));

	} else {
	    rq = alloc_new_recv(c, bh);
	    /* return value for when user does post_recv for this one */
	    rq->bmi_tag = mh->bmi_tag;
	    rq->state = RQ_EAGER_WAITING_USER_POST;
	    /* do not repost or ack, keeping bh until user test */
	    debug(2, "%s: new rq %p now %s", __func__, rq,
	      rq_state_name(rq->state));
	}
	rq->actual_len = byte_len - sizeof(*mh);

    } else if (mh->type == MSG_EAGER_SENDUNEXPECTED) {

	rq = alloc_new_recv(c, bh);
	/* return values for when user does testunexpected for this one */
	rq->bmi_tag = mh->bmi_tag;
	rq->state = RQ_EAGER_WAITING_USER_TESTUNEXPECTED;
	rq->actual_len = byte_len - sizeof(*mh);
	/* do not repost or ack, keeping bh until user test */
	debug(2, "%s: new rq %p now %s", __func__, rq,
	  rq_state_name(rq->state));

    } else if (mh->type == MSG_RTS) {
	/*
	 * Sender wants to send a big message, initiates rts/cts protocol.
	 * Has the user posted a matching receive for it yet?
	 */
	msg_header_rts_t *mh_rts = (void *)(mh + 1);

	rq = find_matching_recv(RQ_WAITING_INCOMING, c, mh->bmi_tag);
	if (rq) {
	    if ((int)mh_rts->tot_len > rq->buflist.tot_len) {
		error("%s: RTS received " FORMAT_U_INT64_T
		  " too small for buffer " FORMAT_U_INT64_T,
		  __func__, mh_rts->tot_len, rq->buflist.tot_len);
	    }
	    rq->state = RQ_RTS_WAITING_CTS_BUFFER;
	    debug(2, "%s: matched rq %p MSG_RTS now %s", __func__, rq,
	      rq_state_name(rq->state));
	} else {
	    rq = alloc_new_recv(c, bh);
	    /* return value for when user does post_recv for this one */
	    rq->bmi_tag = mh->bmi_tag;
	    rq->state = RQ_RTS_WAITING_USER_POST;
	    debug(2, "%s: new rq %p MSG_RTS now %s", __func__, rq,
	      rq_state_name(rq->state));
	}
	rq->actual_len = mh_rts->tot_len;
	rq->rts_mop_id = mh_rts->mop_id;

	/* ack his rts for simplicity */
	debug(2, "%s: rq %p ack RTS from %s opid 0x%Lx", __func__,
	  rq, c->peername, rq->rts_mop_id);
	post_rr(c, bh);
	post_sr_ack(c, bh);

	if (rq->state == RQ_RTS_WAITING_CTS_BUFFER)
	    encourage_recv_to_send_cts(rq);

    } else if (mh->type == MSG_BYE) {
	/*
	 * Other side requests connection close.  Do it.
	 */
	ib_close_connection(c);

    } else {
	error("%s: unknown message header type %d", __func__, mh->type);
    }
}

/*
 * Called internally when we notice that we should send a CTS for a
 * message, either just after receiving an RTS, or when the user gets
 * around to posting a receive that matchies an earlier received RTS.
 *
 * Also at test time if this state is found, since sending a CTS requires
 * a local buffer, and we might run out of that.
 */
static void
encourage_recv_to_send_cts(ib_recv_t *rq)
{
    int ret;

    debug(2, "%s: rq %p %s", __func__, rq, rq_state_name(rq->state));
    assert(rq->state == RQ_RTS_WAITING_CTS_BUFFER,
      "%s: wrong state %s", __func__, rq_state_name(rq->state));

    ret = send_cts(rq);
    if (ret == 0)
	rq->state = RQ_RTS_WAITING_DATA;
    /* else keep waiting until we can send that cts */

    debug(2, "%s: rq %p now %s", __func__, rq, rq_state_name(rq->state));
}

/*
 * Data has arrived, we know because we got the ack to the CTS
 * we sent out.  Serves to release remote cts buffer too.
 */
static void
encourage_recv_incoming_cts_ack(ib_recv_t *rq)
{
    debug(2, "%s: rq %p %s", __func__, rq, rq_state_name(rq->state));
    assert(rq->state == RQ_RTS_WAITING_DATA, "%s: CTS ack to rq wrong state %s",
      __func__, rq_state_name(rq->state));

    /* XXX: should be head for cache, but use tail for debugging */
    qlist_add_tail(&rq->bh->list, &rq->c->eager_send_buf_free);
    ib_mem_deregister(&rq->buflist);
    rq->state = RQ_RTS_WAITING_USER_TEST;

    debug(2, "%s: rq %p now %s", __func__, rq, rq_state_name(rq->state));
}

/*
 * Two places need to send a CTS in response to an RTS.  They both
 * call this.  This handles pinning the memory, too.  Don't forget
 * to unpin when done.
 */
static int
send_cts(ib_recv_t *rq)
{
    buf_head_t *bh;
    msg_header_t *mh;
    msg_header_cts_t *mh_cts;
    u_int64_t *bufp;
    u_int32_t *lenp;
    u_int32_t *keyp;
    u_int32_t post_len;
    int i;

    debug(2, "%s: rq %p, offering to recv %s opid 0x%Lx len %Ld",
      __func__, rq, rq->c->peername, rq->rts_mop_id, Ld(rq->buflist.tot_len));

    bh = qlist_try_del_head(&rq->c->eager_send_buf_free);
    if (!bh) {
	debug(2, "%s: no bh available", __func__);
	return 1;
    }
    rq->bh = bh;
    bh->sq = (ib_send_t *) rq;  /* uplink for completion */

    ib_mem_register(&rq->buflist, TYPE_RECV);

    /* expect an ack for this cts */
    post_rr_ack(rq->c, bh);

    mh = bh->buf;
    mh->type = MSG_CTS;
    /* XXX: mh->bmi_tag unused, consider a more primitive union */
    mh_cts = (void *)((char *) bh->buf + sizeof(*mh));
    mh_cts->rts_mop_id = rq->rts_mop_id;
    mh_cts->buflist_num = rq->buflist.num;
    mh_cts->buflist_tot_len = rq->buflist.tot_len;
    /* encode all the buflist entries */
    bufp = (u_int64_t *)(mh_cts + 1);
    lenp = (u_int32_t *)(bufp + rq->buflist.num);
    keyp = (u_int32_t *)(lenp + rq->buflist.num);
    post_len = (char *)(keyp + rq->buflist.num) - (char *)mh;
    if (post_len > EAGER_BUF_SIZE)
	error("%s: too many (%d) recv buflist entries for buf",  __func__,
	  rq->buflist.num);
    for (i=0; i<rq->buflist.num; i++) {
	bufp[i] = int64_from_ptr(rq->buflist.buf.recv[i]);
	lenp[i] = rq->buflist.len[i];
	keyp[i] = rq->buflist.rkey[i];
    }

    post_sr(bh, post_len);
    return 0;
}


/*
 * Simplify VAPI interface to post sends.  Not RDMA, just SEND.
 * Called for an eager send, rts send, or cts send.  Local send
 * completion is ignored.
 */
static void
post_sr(const buf_head_t *bh, u_int32_t len)
{
    VAPI_sg_lst_entry_t sg;
    VAPI_sr_desc_t sr;
    int ret;
    const ib_connection_t *c = bh->c;

    debug(2, "%s: %s bh %d len %u", __func__, c->peername, bh->num, len);
    sg.addr = int64_from_ptr(bh->buf);
    sg.len = len;
    sg.lkey = c->eager_send_lkey;

    memset(&sr, 0, sizeof(sr));
    sr.opcode = VAPI_SEND;
    sr.comp_type = VAPI_UNSIGNALED;  /* == 1 */
    sr.sg_lst_p = &sg;
    sr.sg_lst_len = 1;
    ret = VAPI_post_sr(nic_handle, c->qp, &sr);
    if (ret < 0)
	error_verrno(ret, "%s: VAPI_post_sr", __func__);
}

/*
 * Post one of the eager recv bufs for this connection.
 */
void
post_rr(const ib_connection_t *c, buf_head_t *bh)
{
    VAPI_sg_lst_entry_t sg;
    VAPI_rr_desc_t rr;
    int ret;

    debug(2, "%s: %s bh %d", __func__, c->peername, bh->num);
    sg.addr = int64_from_ptr(bh->buf);
    sg.len = EAGER_BUF_SIZE;
    sg.lkey = c->eager_recv_lkey;

    memset(&rr, 0, sizeof(rr));
    rr.opcode = VAPI_RECEIVE;
    rr.id = int64_from_ptr(bh);
    rr.sg_lst_p = &sg;
    rr.sg_lst_len = 1;
    ret = VAPI_post_rr(nic_handle, c->qp, &rr);
    if (ret < 0)
	error_verrno(ret, "%s: VAPI_post_rr", __func__);
}

/*
 * Explicitly return a credit.  Immediate data says for which local
 * buffer on the sender is this ack.  Buffers are tied together, so
 * we use our local bufnum which is the same as his.
 *
 * Don't want to get a local completion from this, but if we don't do
 * so every once in a while, the NIC will fill up apparently.  So we
 * generate one every N - 100, where N =~ 5000, the number asked for
 * at QP build time.
 */
static void
post_sr_ack(const ib_connection_t *c, const buf_head_t *bh)
{
    VAPI_sr_desc_t sr;
    int ret;
    static int num_sr_ack = 0;

    debug(2, "%s: %s bh %d wr %d/%d", __func__, c->peername, bh->num,
      num_sr_ack, max_outstanding_wr);
    memset(&sr, 0, sizeof(sr));
    sr.opcode = VAPI_SEND_WITH_IMM;
    sr.id = int64_from_ptr(c);  /* for error checking if send fails */
    if (++num_sr_ack + 10 == max_outstanding_wr) {
	num_sr_ack = 0;
	sr.comp_type = VAPI_SIGNALED;
    } else
	sr.comp_type = VAPI_UNSIGNALED;  /* == 1 */
    sr.imm_data = bh->num;
    sr.sg_lst_len = 0;
    ret = VAPI_post_sr(nic_handle, c->qp_ack, &sr);
    if (ret < 0)
	error_verrno(ret, "%s: VAPI_post_sr", __func__);
}

/*
 * Put another receive entry on the list for an ack.  These have no
 * data, so require no local buffers.  Just add a descriptor to the
 * NIC list.  We do keep the .id pointing to the bh which is the originator
 * of the eager (or RTS or whatever) send, just as a consistency check
 * that when the ack comes in, it is for the outgoing message we expected.
 *
 * In the future they could be out-of-order, though, so perhaps that will
 * go away.
 *
 * Could prepost a whole load of these and just replenish them without
 * thinking.
 */
static void
post_rr_ack(const ib_connection_t *c, const buf_head_t *bh)
{
    VAPI_rr_desc_t rr;
    int ret;

    debug(2, "%s: %s bh %d", __func__, c->peername, bh->num);
    memset(&rr, 0, sizeof(rr));
    rr.opcode = VAPI_RECEIVE;
    rr.comp_type = VAPI_SIGNALED;  /* ask to get these, == 0 */
    rr.id = int64_from_ptr(bh);
    ret = VAPI_post_rr(nic_handle, c->qp_ack, &rr);
    if (ret < 0)
	error_verrno(ret, "%s: VAPI_post_rr", __func__);
}

/*
 * Called only in response to receipt of a CTS on the sender.  RDMA write
 * the big data to the other side.  A bit messy since an RDMA write may
 * not scatter to the receiver, but can gather from the sender, and we may
 * have a non-trivial buflist on both sides.
 */
static void
post_sr_rdmaw(ib_send_t *sq, msg_header_cts_t *mh_cts)
{
    VAPI_sr_desc_t sr;
    int done;

    int send_index = 0, recv_index = 0;    /* working entry in buflist */
    int send_offset = 0;  /* byte offset in working send entry */
    u_int64_t *recv_bufp = (u_int64_t *)(mh_cts + 1);
    u_int32_t *recv_lenp = (u_int32_t *)(recv_bufp + mh_cts->buflist_num);
    u_int32_t *recv_rkey = (u_int32_t *)(recv_lenp + mh_cts->buflist_num);

    debug(2, "%s: sq %p totlen %d", __func__, sq, (int) sq->buflist.tot_len);

    ib_mem_register(&sq->buflist, TYPE_SEND);

    /* constant things for every send */
    memset(&sr, 0, sizeof(sr));
    sr.opcode = VAPI_RDMA_WRITE;
    sr.comp_type = VAPI_UNSIGNALED;
    sr.sg_lst_p = sg_tmp_array;

    done = 0;
    while (!done) {
	int ret;
	u_int32_t recv_bytes_needed;

	/*
	 * Driven by recv elements.  Sizes have already been checked
	 * (hopefully).
	 */
	sr.remote_addr = recv_bufp[recv_index];
	sr.r_key = recv_rkey[recv_index];
	sr.sg_lst_len = 0;
	recv_bytes_needed = recv_lenp[recv_index];
	debug(4, "%s: chunk to %s remote addr %Lx rkey %x",
	  __func__, sq->c->peername, sr.remote_addr, sr.r_key);
	while (recv_bytes_needed > 0) {
	    /* consume from send buflist to fill this one receive */
	    u_int32_t send_bytes_offered
	      = sq->buflist.len[send_index] - send_offset;
	    u_int32_t this_bytes = send_bytes_offered;
	    if (this_bytes > recv_bytes_needed)
		this_bytes = recv_bytes_needed;

	    sg_tmp_array[sr.sg_lst_len].addr =
	      int64_from_ptr(sq->buflist.buf.send[send_index])
	      + send_offset;
	    sg_tmp_array[sr.sg_lst_len].len = this_bytes;
	    sg_tmp_array[sr.sg_lst_len].lkey = sq->buflist.lkey[send_index];

	    debug(4, "%s: chunk %d local addr %Lx len %d lkey %x",
	      __func__, sr.sg_lst_len,
	      sg_tmp_array[sr.sg_lst_len].addr,
	      sg_tmp_array[sr.sg_lst_len].len,
	      sg_tmp_array[sr.sg_lst_len].lkey);

	    ++sr.sg_lst_len;
	    if ((int)sr.sg_lst_len > sg_max_len)
		error("%s: send buflist len %d bigger than max %d", __func__,
		  sr.sg_lst_len, sg_max_len);

	    send_offset += this_bytes;
	    if (send_offset == sq->buflist.len[send_index]) {
		++send_index;
		send_offset = 0;
		if (send_index == sq->buflist.num) {
		    done = 1;
		    break;  /* short send */
		}
	    }
	    recv_bytes_needed -= this_bytes;
	}

	/* done with the one we were just working on, is this the last recv? */
	++recv_index;
	if (recv_index == (int)mh_cts->buflist_num)
	    done = 1;

	/* either filled the recv or exhausted the send */
	if (done) {
	    sr.id = int64_from_ptr(sq);    /* used to match in completion */
	    sr.comp_type = VAPI_SIGNALED;  /* completion drives the unpin */
	}
	ret = VAPI_post_sr(nic_handle, sq->c->qp, &sr);
	if (ret < 0)
	    error_verrno(ret, "%s: VAPI_post_sr", __func__);
    }
}

/*
 * Bring up the connection before posting a send or receive on it.
 */
static void
ensure_connected(struct method_addr *remote_map)
{
    ib_method_addr_t *ibmap = remote_map->method_data;

    if (!ibmap->c)
	ib_tcp_client_connect(ibmap, remote_map);
}

/*
 * Used by both send and sendunexpected.
 */
static int
generic_post_send(bmi_op_id_t *id, struct method_addr *remote_map,
  int numbufs, const void *const *buffers, const bmi_size_t *sizes,
  bmi_size_t total_size, bmi_msg_tag_t tag, void *user_ptr,
  bmi_context_id context_id, int is_unexpected)
{
    ib_send_t *sq;
    struct method_op *mop;
    ib_method_addr_t *ibmap;
    int i;
    int ret = 0;

    gen_mutex_lock(&interface_mutex);
    ensure_connected(remote_map);
    ibmap = remote_map->method_data;

    /* alloc and build new sendq structure */
    sq = Malloc(sizeof(*sq));
    sq->type = TYPE_SEND;
    sq->state = SQ_WAITING_BUFFER;

    /*
     * For a single buffer, store it inside the sq directly, else save
     * the pointer to the list the user built when calling a _list
     * function.  This case is indicated by the non-_list functions by
     * a zero in numbufs.
     */
    if (numbufs == 0) {
	sq->buflist_one_buf = *buffers;
	sq->buflist_one_len = *sizes;
	sq->buflist.num = 1;
	sq->buflist.buf.send = &sq->buflist_one_buf;
	sq->buflist.len = &sq->buflist_one_len;
    } else {
	sq->buflist.num = numbufs;
	sq->buflist.buf.send = buffers;
	sq->buflist.len = sizes;
    }
    sq->buflist.tot_len = 0;
    for (i=0; i<sq->buflist.num; i++)
	sq->buflist.tot_len += sizes[i];

    /*
     * This passed-in total length field does not make much sense
     * to me, but I'll at least check it for accuracy.
     */
    if (sq->buflist.tot_len != total_size)
	error("%s: user-provided tot len " FORMAT_BMI_SIZE_T
	  " does not match buffer list tot len " FORMAT_BMI_SIZE_T,
	  __func__, total_size, sq->buflist.tot_len);

    /* unexpected messages must fit inside an eager message */
    if (is_unexpected && sq->buflist.tot_len > EAGER_BUF_PAYLOAD) {
	free(sq);
	ret = -EINVAL;
	goto out;
    }

    sq->bmi_tag = tag;
    sq->c = ibmap->c;
    sq->is_unexpected = is_unexpected;
    qlist_add_tail(&sq->list, &sendq);

    /* generate identifier used by caller to test for message later */
    mop = Malloc(sizeof(*mop));
    id_gen_safe_register(&mop->op_id, mop);
    mop->addr = remote_map;  /* set of function pointers, essentially */
    mop->method_data = sq;
    mop->user_ptr = user_ptr;
    mop->context_id = context_id;
    *id = mop->op_id;
    sq->mop = mop;
    debug(2, "%s: new sq %p", __func__, sq);

    /* and start sending it if possible */
    encourage_send_waiting_buffer(sq);
  out:
    gen_mutex_unlock(&interface_mutex);
    return ret;
}

static int
BMI_ib_post_send(bmi_op_id_t *id, struct method_addr *remote_map,
  const void *buffer, bmi_size_t size,
  enum bmi_buffer_type buffer_flag __unused,
  bmi_msg_tag_t tag, void *user_ptr, bmi_context_id context_id)
{
    debug(2, "%s: len %d tag %d", __func__, (int) size, tag);
    /* references here will not be saved after this func returns */
    return generic_post_send(id, remote_map, 0, &buffer, &size, size,
      tag, user_ptr, context_id, 0);
}

static int
BMI_ib_post_send_list(bmi_op_id_t *id, struct method_addr *remote_map,
  const void *const *buffers, const bmi_size_t *sizes, int list_count,
  bmi_size_t total_size, enum bmi_buffer_type buffer_flag __unused,
  bmi_msg_tag_t tag, void *user_ptr, bmi_context_id context_id)
{
    debug(2, "%s: listlen %d tag %d", __func__, list_count, tag);
    if (list_count < 1)
	error("%s: list count must be positive", __func__);
    return generic_post_send(id, remote_map, list_count, buffers, sizes,
      total_size, tag, user_ptr, context_id, 0);
}

static int
BMI_ib_post_sendunexpected(bmi_op_id_t *id, struct method_addr *remote_map,
  const void *buffer, bmi_size_t size,
  enum bmi_buffer_type buffer_flag __unused,
  bmi_msg_tag_t tag, void *user_ptr, bmi_context_id context_id)
{
    debug(2, "%s: len %d tag %d", __func__, (int) size, tag);
    /* references here will not be saved after this func returns */
    return generic_post_send(id, remote_map, 0, &buffer, &size, size, tag,
      user_ptr, context_id, 1);
}

static int
BMI_ib_post_sendunexpected_list(bmi_op_id_t *id, struct method_addr *remote_map,
  const void *const *buffers, const bmi_size_t *sizes, int list_count,
  bmi_size_t total_size, enum bmi_buffer_type buffer_flag __unused,
  bmi_msg_tag_t tag, void *user_ptr, bmi_context_id context_id)
{
    debug(2, "%s: listlen %d tag %d", __func__, list_count, tag);
    if (list_count < 1)
	error("%s: list count must be positive", __func__);
    /* references here will not be saved after this func returns */
    return generic_post_send(id, remote_map, list_count, buffers, sizes,
      total_size, tag, user_ptr, context_id, 1);
}

/*
 * Used by both recv and recv_list.
 */
static void
generic_post_recv(bmi_op_id_t *id, struct method_addr *remote_map,
  int numbufs, void *const *buffers, const bmi_size_t *sizes,
  bmi_size_t tot_expected_len, bmi_msg_tag_t tag,
  void *user_ptr, bmi_context_id context_id)
{
    ib_recv_t *rq;
    struct method_op *mop;
    ib_method_addr_t *ibmap;
    ib_connection_t *c;
    int i;
    
    gen_mutex_lock(&interface_mutex);
    ensure_connected(remote_map);
    ibmap = remote_map->method_data;
    c = ibmap->c;

    /* poll interface first to save a few steps below */
    check_cq();

    /* check to see if matching recv is in the queue */
    rq = find_matching_recv(RQ_EAGER_WAITING_USER_POST, c, tag);
    if (!rq)
	rq = find_matching_recv(RQ_RTS_WAITING_USER_POST, c, tag);

    if (rq) {
	debug(2, "%s: rq %p matches %s", __func__, rq,
	  rq_state_name(rq->state));
    } else {
	/* alloc and build new recvq structure */
	rq = Malloc(sizeof(*rq));
	rq->type = TYPE_RECV;
	rq->state = RQ_WAITING_INCOMING;
	rq->bmi_tag = tag;
	rq->c = c;
	qlist_add_tail(&rq->list, &recvq);
	debug(2, "%s: new rq %p", __func__, rq);
    }

    if (numbufs == 0) {
	rq->buflist_one_buf = *buffers;
	rq->buflist_one_len = *sizes;
	rq->buflist.num = 1;
	rq->buflist.buf.recv = &rq->buflist_one_buf;
	rq->buflist.len = &rq->buflist_one_len;
    } else {
	rq->buflist.num = numbufs;
	rq->buflist.buf.recv = buffers;
	rq->buflist.len = sizes;
    }
    rq->buflist.tot_len = 0;
    for (i=0; i<rq->buflist.num; i++)
	rq->buflist.tot_len += sizes[i];

    /*
     * This passed-in total length field does not make much sense
     * to me, but I'll at least check it for accuracy.
     */
    if (rq->buflist.tot_len != tot_expected_len)
	error("%s: user-provided tot len " FORMAT_BMI_SIZE_T
	  " does not match buffer list tot len " FORMAT_BMI_SIZE_T,
	  __func__, tot_expected_len, rq->buflist.tot_len);

    /* generate identifier used by caller to test for message later */
    mop = Malloc(sizeof(*mop));
    id_gen_safe_register(&mop->op_id, mop);
    mop->addr = remote_map;  /* set of function pointers, essentially */
    mop->method_data = rq;
    mop->user_ptr = user_ptr;
    mop->context_id = context_id;
    *id = mop->op_id;
    rq->mop = mop;

    /* handle the two "waiting for a local user post" states */
    if (rq->state == RQ_EAGER_WAITING_USER_POST) {

	debug(2, "%s: rq %p state %s finish eager directly", __func__,
	  rq, rq_state_name(rq->state));
	if (rq->actual_len > tot_expected_len) {
	    error("%s: received " FORMAT_BMI_SIZE_T
	      " matches too-small buffer " FORMAT_BMI_SIZE_T,
	      __func__, rq->actual_len, rq->buflist.tot_len);
	}

	memcpy_to_buflist(&rq->buflist,
	  (char *) rq->bh->buf + sizeof(msg_header_t),
	  rq->actual_len);

	/* re-post */
	post_rr(rq->c, rq->bh);
	/* done with buffer, ack to remote */
	post_sr_ack(rq->c, rq->bh);

	/* now just wait for user to test, never do "immediate completion" */
	rq->state = RQ_EAGER_WAITING_USER_TEST;

    } else if (rq->state == RQ_RTS_WAITING_USER_POST) {
	int ret;
	debug(2, "%s: rq %p %s send cts", __func__, rq,
	  rq_state_name(rq->state));
	ret = send_cts(rq);
	if (ret == 0)
	    rq->state = RQ_RTS_WAITING_DATA;
	else
	    rq->state = RQ_RTS_WAITING_CTS_BUFFER;
    }
    gen_mutex_unlock(&interface_mutex);
}

static int
BMI_ib_post_recv(bmi_op_id_t *id, struct method_addr *remote_map,
  void *buffer, bmi_size_t expected_len, bmi_size_t *actual_len __unused,
  enum bmi_buffer_type buffer_flag __unused, bmi_msg_tag_t tag, void *user_ptr,
  bmi_context_id context_id)
{
    debug(2, "%s: expected len %d tag %d", __func__, (int) expected_len, tag);
    generic_post_recv(id, remote_map, 0, &buffer, &expected_len,
      expected_len, tag, user_ptr, context_id);
    return 0;
}

static int
BMI_ib_post_recv_list(bmi_op_id_t *id, struct method_addr *remote_map,
  void *const *buffers, const bmi_size_t *sizes, int list_count,
  bmi_size_t tot_expected_len, bmi_size_t *tot_actual_len __unused,
  enum bmi_buffer_type buffer_flag __unused, bmi_msg_tag_t tag, void *user_ptr,
  bmi_context_id context_id)
{
    debug(2, "%s: tot expected len %d tag %d", __func__,
      (int) tot_expected_len, tag);
    if (list_count < 1)
	error("%s: list count must be positive", __func__);
    generic_post_recv(id, remote_map, list_count, buffers, sizes,
      tot_expected_len, tag, user_ptr, context_id);
    return 0;
}

/*
 * Internal shared helper function.  Return 1 if found something
 * completed.
 */
static int
test_sq(ib_send_t *sq, bmi_op_id_t *outid, bmi_error_code_t *err,
  bmi_size_t *size, void **user_ptr, int complete)
{
    debug(9, "%s: sq %p outid %p err %p size %p user_ptr %p complete %d",
      __func__, sq, outid, err, size, user_ptr, complete);

    if (sq->state == SQ_WAITING_USER_TEST) {
	if (complete) {
	    debug(2, "%s: sq %p completed %Ld to %s", __func__,
	      sq, Ld(sq->buflist.tot_len),
	      ((ib_method_addr_t *) sq->c->remote_map->method_data)
		->hostname);
	    *outid = sq->mop->op_id;
	    *err = 0;
	    *size = sq->buflist.tot_len;
	    if (user_ptr)
		*user_ptr = sq->mop->user_ptr;
	    qlist_del(&sq->list);
	    id_gen_safe_unregister(sq->mop->op_id);
	    free(sq->mop);
	    free(sq);
	    return 1;
	}
    /* this state needs help, push it (ideally would be triggered
     * when the resource is freed... XXX */
    } else if (sq->state == SQ_WAITING_BUFFER) {
	debug(2, "%s: sq %p %s, encouraging", __func__, sq,
	  sq_state_name(sq->state));
	encourage_send_waiting_buffer(sq);
    } else {
	debug(9, "%s: sq %p found, not done, state %s", __func__,
	  sq, sq_state_name(sq->state));
    }
    return 0;
}

/*
 * Internal shared helper function.  Return 1 if found something
 * completed.  Note that rq->mop can be null for unexpected
 * messages.
 */
static int
test_rq(ib_recv_t *rq, bmi_op_id_t *outid, bmi_error_code_t *err,
  bmi_size_t *size, void **user_ptr, int complete)
{
    debug(9, "%s: rq %p outid %p err %p size %p user_ptr %p complete %d",
      __func__, rq, outid, err, size, user_ptr, complete);

    if (rq->state == RQ_EAGER_WAITING_USER_TEST 
      || rq->state == RQ_RTS_WAITING_USER_TEST) {
	if (complete) {
	    debug(2, "%s: rq %p completed %Ld from %s", __func__,
	      rq, Ld(rq->actual_len),
	      ((ib_method_addr_t *) rq->c->remote_map->method_data)
		->hostname);
	    *err = 0;
	    *size = rq->actual_len;
	    if (rq->mop) {
		*outid = rq->mop->op_id;
		if (user_ptr)
		    *user_ptr = rq->mop->user_ptr;
		id_gen_safe_unregister(rq->mop->op_id);
		free(rq->mop);
	    }
	    qlist_del(&rq->list);
	    free(rq);
	    return 1;
	}
    /* this state needs help, push it (ideally would be triggered
     * when the resource is freed... XXX */
    } else if (rq->state == RQ_RTS_WAITING_CTS_BUFFER) {
	debug(2, "%s: rq %p %s, encouraging", __func__, rq,
	  rq_state_name(rq->state));
	encourage_recv_to_send_cts(rq);
    } else {
	debug(9, "%s: rq %p found, not done, state %s", __func__,
	  rq, rq_state_name(rq->state));
    }
    return 0;
}

/*
 * Test one message, send or receive.  Also used to test the send side of
 * messages sent using sendunexpected.
 */
static int
BMI_ib_test(bmi_op_id_t id, int *outcount, bmi_error_code_t *err,
  bmi_size_t *size, void **user_ptr, int max_idle_time __unused,
  bmi_context_id context_id __unused)
{
    struct method_op *mop;
    ib_send_t *sq;
    int n;

    gen_mutex_lock(&interface_mutex);
    check_cq();

    mop = id_gen_safe_lookup(id);
    sq = mop->method_data;
    n = 0;
    if (sq->type == TYPE_SEND) {
	if (test_sq(sq, &id, err, size, user_ptr, 1))
	    n = 1;
    } else {
	/* actually a recv */
	ib_recv_t *rq = mop->method_data;
	if (test_rq(rq, &id, err, size, user_ptr, 1))
	    n = 1;
    }
    *outcount = n;
    gen_mutex_unlock(&interface_mutex);
    return 0;
}

/*
 * Used by testcontext and testunexpected to block if not much is going on
 * since the timeouts at the BMI job layer are too coarse.
 */
static struct timeval last_action = { 0, 0 };

/*
 * Test for multiple completions matching a particular user context.
 */
static int
BMI_ib_testcontext(int incount, bmi_op_id_t *outids, int *outcount,
  bmi_error_code_t *errs, bmi_size_t *sizes, void **user_ptrs,
  int max_idle_time, bmi_context_id context_id)
{
    list_t *l, *lnext;
    int n, complete;
    void **up = 0;

    gen_mutex_lock(&interface_mutex);
    check_cq();

    /*
     * Walk _all_ entries on sq, rq, marking them completed or
     * encouraging them as needed due to resource limitations.
     */
    n = 0;
    for (l=sendq.next; l != &sendq; l=lnext) {
	ib_send_t *sq = qlist_upcast(l);
	lnext = l->next;
	/* test them all, even if can't reap them, just to encourage */
	complete = (sq->mop->context_id == context_id) && (n < incount);
	if (user_ptrs)
	    up = &user_ptrs[n];
	n += test_sq(sq, &outids[n], &errs[n], &sizes[n], up, complete);
    }

    for (l=recvq.next; l != &recvq; l=lnext) {
	ib_recv_t *rq = qlist_upcast(l);
	lnext = l->next;

	/* some receives have no mops:  unexpected */
	complete = rq->mop &&
	  (rq->mop->context_id == context_id) && (n < incount);
	if (user_ptrs)
	    up = &user_ptrs[n];
	n += test_rq(rq, &outids[n], &errs[n], &sizes[n], up, complete);
    }
    *outcount = n;
    if (n > 0) {
	gettimeofday(&last_action, 0);
    } else if (max_idle_time > 0) {
	/*
	 * Block for up to max_idle_time to avoid spinning from BMI.  Instead
	 * of sleeping, watch the accept socket for something new.  No way
	 * to blockingly poll in standard VAPI.
	 */
	struct timeval now;
	gettimeofday(&now, 0);
	now.tv_sec -= last_action.tv_sec;
	if (now.tv_sec == 1) {
	    now.tv_usec -= last_action.tv_usec;
	    if (now.tv_usec < 0)
		--now.tv_sec;
	}
	if (now.tv_sec > 0)  /* spin for 1 sec following any activity */
	    if (ib_tcp_server_block_new_connections(max_idle_time))
		gettimeofday(&last_action, 0);
    }
    gen_mutex_unlock(&interface_mutex);
    return 0;
}

/*
 * Non-blocking test to look for any incoming unexpected messages.
 * This is also where we check for new connections on the TCP socket, since
 * those would show up as unexpected the first time anything is sent.
 * Return 0 for success, or -1 for failure; number of things in *outcount.
 */
static int
BMI_ib_testunexpected(int incount __unused, int *outcount,
  struct method_unexpected_info *ui, int max_idle_time __unused)
{
    int num_action;
    list_t *l;

    gen_mutex_lock(&interface_mutex);

    /* Check CQ, then look for the first unexpected message.  */
    num_action = check_cq();

    *outcount = 0;
    qlist_for_each(l, &recvq) {
	ib_recv_t *rq = qlist_upcast(l);
	if (rq->state == RQ_EAGER_WAITING_USER_TESTUNEXPECTED) {
	    debug(2, "%s: found waiting testunexpected", __func__);
	    ui->error_code = 0;
	    ui->addr = rq->c->remote_map;  /* hand back permanent method_addr */
	    ui->buffer = Malloc(rq->actual_len);
	    ui->size = rq->actual_len;
	    memcpy(ui->buffer, (char *) rq->bh->buf + sizeof(msg_header_t),
	      ui->size);
	    ui->tag = rq->bmi_tag;
	    /* re-post the buffer in which it was sitting, just unexpecteds */
	    post_rr(rq->c, rq->bh);
	    /* freed our eager buffer, ack it */
	    post_sr_ack(rq->c, rq->bh);
	    *outcount = 1;
	    qlist_del(&rq->list);
	    free(rq);
	    goto out;
	}
    }

    num_action += ib_tcp_server_check_new_connections();
    if (num_action)
	gettimeofday(&last_action, 0);

  out:
    gen_mutex_unlock(&interface_mutex);
    return 0;
}

/*
 * Do not care about memory allocation.  Send/recv functions will pin as
 * necessary.
 */
static void *
BMI_ib_memalloc(bmi_size_t size,
  enum bmi_op_type send_recv __unused)
{
    return malloc((size_t) size);
}

static int
BMI_ib_memfree(void *buf, bmi_size_t size __unused,
  enum bmi_op_type send_recv __unused)
{
    free(buf);
    return 0;
}

/*
 * No need to track these internally.  Just search the entire queue.
 */
static int
BMI_ib_open_context(bmi_context_id context_id __unused)
{
    return 0;
}

static void
BMI_ib_close_context(bmi_context_id context_id __unused)
{
}


/* exported method interface */
struct bmi_method_ops bmi_ib_ops = 
{
    .method_name = "bmi_ib",
    .BMI_meth_initialize = BMI_ib_initialize,
    .BMI_meth_finalize = BMI_ib_finalize,
    .BMI_meth_set_info = BMI_ib_set_info,
    .BMI_meth_get_info = BMI_ib_get_info,
    .BMI_meth_memalloc = BMI_ib_memalloc,
    .BMI_meth_memfree = BMI_ib_memfree,
    .BMI_meth_post_send = BMI_ib_post_send,
    .BMI_meth_post_sendunexpected = BMI_ib_post_sendunexpected,
    .BMI_meth_post_recv = BMI_ib_post_recv,
    .BMI_meth_test = BMI_ib_test,
    .BMI_meth_testsome = 0,
    .BMI_meth_testcontext = BMI_ib_testcontext,
    .BMI_meth_testunexpected = BMI_ib_testunexpected,
    .BMI_meth_method_addr_lookup = BMI_ib_method_addr_lookup,
    .BMI_meth_post_send_list = BMI_ib_post_send_list,
    .BMI_meth_post_recv_list = BMI_ib_post_recv_list,
    .BMI_meth_post_sendunexpected_list = BMI_ib_post_sendunexpected_list,
    .BMI_meth_open_context = BMI_ib_open_context,
    .BMI_meth_close_context = BMI_ib_close_context,
};

/* vi: set tags+=/home/pw/src/infiniband/mellanox/include/tags: */

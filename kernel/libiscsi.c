/*
 * iSCSI lib functions
 *
 * Copyright (C) 2006 Red Hat, Inc.  All rights reserved.
 * Copyright (C) 2004 - 2006 Mike Christie
 * Copyright (C) 2004 - 2005 Dmitry Yusupov
 * Copyright (C) 2004 - 2005 Alex Aizman
 * maintained by open-iscsi@googlegroups.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/kfifo.h>
#include <linux/delay.h>
#include <net/tcp.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_tcq.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi.h>
#include <scsi/iscsi_proto.h>
#include <scsi/scsi_transport.h>
#include "scsi_transport_iscsi.h"
#include "libiscsi.h"

struct iscsi_session *
class_to_transport_session(struct iscsi_cls_session *cls_session)
{
	struct Scsi_Host *shost = iscsi_session_to_shost(cls_session);
	return iscsi_hostdata(shost->hostdata);
}
EXPORT_SYMBOL_GPL(class_to_transport_session);

#define INVALID_SN_DELTA	0xffff

int
iscsi_check_assign_cmdsn(struct iscsi_session *session, struct iscsi_nopin *hdr)
{
	uint32_t max_cmdsn = be32_to_cpu(hdr->max_cmdsn);
	uint32_t exp_cmdsn = be32_to_cpu(hdr->exp_cmdsn);

	if (max_cmdsn < exp_cmdsn -1 &&
	    max_cmdsn > exp_cmdsn - INVALID_SN_DELTA)
		return ISCSI_ERR_MAX_CMDSN;
	if (max_cmdsn > session->max_cmdsn ||
	    max_cmdsn < session->max_cmdsn - INVALID_SN_DELTA)
		session->max_cmdsn = max_cmdsn;
	if (exp_cmdsn > session->exp_cmdsn ||
	    exp_cmdsn < session->exp_cmdsn - INVALID_SN_DELTA)
		session->exp_cmdsn = exp_cmdsn;

	return 0;
}
EXPORT_SYMBOL_GPL(iscsi_check_assign_cmdsn);

void iscsi_prep_unsolicit_data_pdu(struct iscsi_cmd_task *ctask,
				   struct iscsi_data *hdr,
				   int transport_data_cnt)
{
	struct iscsi_conn *conn = ctask->conn;

	memset(hdr, 0, sizeof(struct iscsi_data));
	hdr->ttt = cpu_to_be32(ISCSI_RESERVED_TAG);
	hdr->datasn = cpu_to_be32(ctask->unsol_datasn);
	ctask->unsol_datasn++;
	hdr->opcode = ISCSI_OP_SCSI_DATA_OUT;
	memcpy(hdr->lun, ctask->hdr->lun, sizeof(hdr->lun));

	hdr->itt = ctask->hdr->itt;
	hdr->exp_statsn = cpu_to_be32(conn->exp_statsn);

	hdr->offset = cpu_to_be32(ctask->total_length -
				  transport_data_cnt -
				  ctask->unsol_count);

	if (ctask->unsol_count > conn->max_xmit_dlength) {
		hton24(hdr->dlength, conn->max_xmit_dlength);
		ctask->data_count = conn->max_xmit_dlength;
		hdr->flags = 0;
	} else {
		hton24(hdr->dlength, ctask->unsol_count);
		ctask->data_count = ctask->unsol_count;
		hdr->flags = ISCSI_FLAG_CMD_FINAL;
	}
}
EXPORT_SYMBOL_GPL(iscsi_prep_unsolicit_data_pdu);

/**
 * iscsi_prep_scsi_cmd_pdu - prep iscsi scsi cmd pdu
 * @ctask: iscsi cmd task
 *
 * Prep basic iSCSI PDU fields for a scsi cmd pdu. The LLD should set
 * fields like dlength or final based on how much data it sends
 */
static void iscsi_prep_scsi_cmd_pdu(struct iscsi_cmd_task *ctask)
{
	struct iscsi_conn *conn = ctask->conn;
	struct iscsi_session *session = conn->session;
	struct iscsi_cmd *hdr = ctask->hdr;
	struct scsi_cmnd *sc = ctask->sc;

        hdr->opcode = ISCSI_OP_SCSI_CMD;
        hdr->flags = ISCSI_ATTR_SIMPLE;
        int_to_scsilun(sc->device->lun, (struct scsi_lun *)hdr->lun);
        hdr->itt = ctask->itt | (conn->id << ISCSI_CID_SHIFT) |
                         (session->age << ISCSI_AGE_SHIFT);
        hdr->data_length = cpu_to_be32(sc->request_bufflen);
        hdr->cmdsn = cpu_to_be32(session->cmdsn);
        session->cmdsn++;
        hdr->exp_statsn = cpu_to_be32(conn->exp_statsn);
        memcpy(hdr->cdb, sc->cmnd, sc->cmd_len);
        memset(&hdr->cdb[sc->cmd_len], 0, MAX_COMMAND_SIZE - sc->cmd_len);

	if (sc->sc_data_direction == DMA_TO_DEVICE) {
		hdr->flags |= ISCSI_FLAG_CMD_WRITE;
		/*
		 * Write counters:
		 *
		 *	imm_count	bytes to be sent right after
		 *			SCSI PDU Header
		 *
		 *	unsol_count	bytes(as Data-Out) to be sent
		 *			without	R2T ack right after
		 *			immediate data
		 *
		 *	r2t_data_count	bytes to be sent via R2T ack's
		 *
		 *      pad_count       bytes to be sent as zero-padding
		 */
		ctask->imm_count = 0;
		ctask->unsol_count = 0;
		ctask->unsol_datasn = 0;

		if (session->imm_data_en) {
			if (ctask->total_length >= session->first_burst)
				ctask->imm_count = min(session->first_burst,
							conn->max_xmit_dlength);
			else
				ctask->imm_count = min(ctask->total_length,
							conn->max_xmit_dlength);
			hton24(ctask->hdr->dlength, ctask->imm_count);
		} else
			zero_data(ctask->hdr->dlength);

		if (!session->initial_r2t_en)
			ctask->unsol_count = min(session->first_burst,
				ctask->total_length) - ctask->imm_count;
		if (!ctask->unsol_count)
			/* No unsolicit Data-Out's */
			ctask->hdr->flags |= ISCSI_FLAG_CMD_FINAL;
	} else {
		ctask->datasn = 0;
		hdr->flags |= ISCSI_FLAG_CMD_FINAL;
		zero_data(hdr->dlength);

		if (sc->sc_data_direction == DMA_FROM_DEVICE)
			hdr->flags |= ISCSI_FLAG_CMD_READ;
	}

	conn->scsicmd_pdus_cnt++;
}
EXPORT_SYMBOL_GPL(iscsi_prep_scsi_cmd_pdu);

/**
 * iscsi_complete_command - return command back to scsi-ml
 * @session: iscsi session
 * @ctask: iscsi cmd task
 *
 * Must be called with session lock.
 * This function returns the scsi command to scsi-ml and returns
 * the cmd task to the pool of available cmd tasks.
 */
static void iscsi_complete_command(struct iscsi_session *session,
				   struct iscsi_cmd_task *ctask)
{
	struct scsi_cmnd *sc = ctask->sc;

	ctask->sc = NULL;
	list_del_init(&ctask->running);
	__kfifo_put(session->cmdpool.queue, (void*)&ctask, sizeof(void*));
	sc->scsi_done(sc);
}

/**
 * iscsi_cmd_rsp - SCSI Command Response processing
 * @conn: iscsi connection
 * @hdr: iscsi header
 * @ctask: scsi command task
 * @data: cmd data buffer
 * @datalen: len of buffer
 *
 * iscsi_cmd_rsp sets up the scsi_cmnd fields based on the PDU and
 * then completes the command and task.
 **/
static int iscsi_scsi_cmd_rsp(struct iscsi_conn *conn, struct iscsi_hdr *hdr,
			      struct iscsi_cmd_task *ctask, char *data,
			      int datalen)
{
	int rc;
	struct iscsi_cmd_rsp *rhdr = (struct iscsi_cmd_rsp *)hdr;
	struct iscsi_session *session = conn->session;
	struct scsi_cmnd *sc = ctask->sc;

	rc = iscsi_check_assign_cmdsn(session, (struct iscsi_nopin*)rhdr);
	if (rc) {
		sc->result = DID_ERROR << 16;
		goto out;
	}

	conn->exp_statsn = be32_to_cpu(rhdr->statsn) + 1;

	sc->result = (DID_OK << 16) | rhdr->cmd_status;

	if (rhdr->response != ISCSI_STATUS_CMD_COMPLETED) {
		sc->result = DID_ERROR << 16;
		goto out;
	}

	if (rhdr->cmd_status == SAM_STAT_CHECK_CONDITION) {
		int senselen;

		if (datalen < 2) {
invalid_datalen:
			printk(KERN_ERR "iscsi: Got CHECK_CONDITION but "
			       "invalid data buffer size of %d\n", datalen);
			sc->result = DID_BAD_TARGET << 16;
			goto out;
		}

		senselen = (data[0] << 8) | data[1];
		if (datalen < senselen)
			goto invalid_datalen;

		memcpy(sc->sense_buffer, data + 2,
		       min(senselen, SCSI_SENSE_BUFFERSIZE));
		debug_scsi("copied %d bytes of sense\n",
			   min(senselen, SCSI_SENSE_BUFFERSIZE));
	}

	if (sc->sc_data_direction == DMA_TO_DEVICE)
		goto out;

	if (rhdr->flags & ISCSI_FLAG_CMD_UNDERFLOW) {
		int res_count = be32_to_cpu(rhdr->residual_count);

		if (res_count > 0 && res_count <= sc->request_bufflen)
			sc->resid = res_count;
		else
			sc->result = (DID_BAD_TARGET << 16) | rhdr->cmd_status;
	} else if (rhdr->flags & ISCSI_FLAG_CMD_BIDI_UNDERFLOW)
		sc->result = (DID_BAD_TARGET << 16) | rhdr->cmd_status;
	else if (rhdr->flags & ISCSI_FLAG_CMD_OVERFLOW)
		sc->resid = be32_to_cpu(rhdr->residual_count);

out:
	debug_scsi("done [sc %lx res %d itt 0x%x]\n",
		   (long)sc, sc->result, ctask->itt);
	conn->scsirsp_pdus_cnt++;

	iscsi_complete_command(conn->session, ctask);
	return rc;
}

/**
 * __iscsi_complete_pdu - complete pdu
 * @conn: iscsi conn
 * @hdr: iscsi header
 * @data: data buffer
 * @datalen: len of data buffer
 *
 * Completes pdu processing by freeing any resources allocated at
 * queuecommand or send generic. session lock must be held and verify
 * itt must have been called.
 */
int __iscsi_complete_pdu(struct iscsi_conn *conn, struct iscsi_hdr *hdr,
			 char *data, int datalen)
{
	struct iscsi_session *session = conn->session;
	int opcode = hdr->opcode & ISCSI_OPCODE_MASK, rc = 0;
	struct iscsi_cmd_task *ctask;
	struct iscsi_mgmt_task *mtask;
	uint32_t itt;

	if (hdr->itt != cpu_to_be32(ISCSI_RESERVED_TAG))
		itt = hdr->itt & ISCSI_ITT_MASK;
	else
		itt = hdr->itt;

	if (itt < session->cmds_max) {
		ctask = session->cmds[itt];

		debug_scsi("cmdrsp [op 0x%x cid %d itt 0x%x len %d]\n",
			   opcode, conn->id, ctask->itt, datalen);

		switch(opcode) {
		case ISCSI_OP_SCSI_CMD_RSP:
			BUG_ON((void*)ctask != ctask->sc->SCp.ptr);
			rc = iscsi_scsi_cmd_rsp(conn, hdr, ctask, data,
						datalen);
			break;
		case ISCSI_OP_SCSI_DATA_IN:
			BUG_ON((void*)ctask != ctask->sc->SCp.ptr);
			if (hdr->flags & ISCSI_FLAG_DATA_STATUS) {
				conn->scsirsp_pdus_cnt++;
				iscsi_complete_command(session, ctask);
			}
			break;
		case ISCSI_OP_R2T:
			/* LLD handles this for now */
			break;
		default:
			rc = ISCSI_ERR_BAD_OPCODE;
			break;
		}
	} else if (itt >= ISCSI_MGMT_ITT_OFFSET &&
		   itt < ISCSI_MGMT_ITT_OFFSET + session->mgmtpool_max) {
		mtask = session->mgmt_cmds[itt - ISCSI_MGMT_ITT_OFFSET];

		debug_scsi("immrsp [op 0x%x cid %d itt 0x%x len %d]\n",
			   opcode, conn->id, mtask->itt, datalen);

		rc = iscsi_check_assign_cmdsn(session,
					      (struct iscsi_nopin*)hdr);
		if (rc)
			goto done;

		switch(opcode) {
		case ISCSI_OP_LOGOUT_RSP:
			conn->exp_statsn = be32_to_cpu(hdr->statsn) + 1;
			/* fall through */
		case ISCSI_OP_LOGIN_RSP:
		case ISCSI_OP_TEXT_RSP:
			/*
			 * login related PDU's exp_statsn is handled in
			 * userspace
			 */
			rc = iscsi_recv_pdu(conn->cls_conn, hdr, data, datalen);
			list_del(&mtask->running);
			if (conn->login_mtask != mtask)
				__kfifo_put(session->mgmtpool.queue,
					    (void*)&mtask, sizeof(void*));
			break;
		case ISCSI_OP_SCSI_TMFUNC_RSP:
			if (datalen) {
				rc = ISCSI_ERR_PROTO;
				break;
			}

			conn->exp_statsn = be32_to_cpu(hdr->statsn) + 1;
			conn->tmfrsp_pdus_cnt++;
			if (conn->tmabort_state == TMABORT_INITIAL) {
				conn->tmabort_state =
					((struct iscsi_tm_rsp *)hdr)->
					response == ISCSI_TMF_RSP_COMPLETE ?
						TMABORT_SUCCESS:TMABORT_FAILED;
				/* unblock eh_abort() */
				wake_up(&conn->ehwait);
			}
			break;
		case ISCSI_OP_NOOP_IN:
			if (hdr->ttt != ISCSI_RESERVED_TAG) {
				rc = ISCSI_ERR_PROTO;
				break;
			}
			conn->exp_statsn = be32_to_cpu(hdr->statsn) + 1;

			rc = iscsi_recv_pdu(conn->cls_conn, hdr, data, datalen);
			list_del(&mtask->running);
			if (conn->login_mtask != mtask)
				__kfifo_put(session->mgmtpool.queue,
					    (void*)&mtask, sizeof(void*));
			break;
		default:
			rc = ISCSI_ERR_BAD_OPCODE;
			break;
		}
	} else if (itt == ISCSI_RESERVED_TAG) {
		switch(opcode) {
		case ISCSI_OP_NOOP_IN:
			if (!datalen) {
				rc = iscsi_check_assign_cmdsn(session,
						 (struct iscsi_nopin*)hdr);
				if (!rc && hdr->ttt != ISCSI_RESERVED_TAG)
					rc = iscsi_recv_pdu(conn->cls_conn,
							    hdr, NULL, 0);
			} else
				rc = ISCSI_ERR_PROTO;
			break;
		case ISCSI_OP_REJECT:
			/* we need sth like iscsi_reject_rsp()*/
		case ISCSI_OP_ASYNC_EVENT:
			conn->exp_statsn = be32_to_cpu(hdr->statsn) + 1;
			/* we need sth like iscsi_async_event_rsp() */
			rc = ISCSI_ERR_BAD_OPCODE;
			break;
		default:
			rc = ISCSI_ERR_BAD_OPCODE;
			break;
		}
	} else
		rc = ISCSI_ERR_BAD_ITT;

done:
	return rc;
}
EXPORT_SYMBOL_GPL(__iscsi_complete_pdu);

int iscsi_complete_pdu(struct iscsi_conn *conn, struct iscsi_hdr *hdr,
		       char *data, int datalen)
{
	int rc;

	spin_lock(&conn->session->lock);
	rc = __iscsi_complete_pdu(conn, hdr, data, datalen);
	spin_unlock(&conn->session->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(iscsi_complete_pdu);

/* verify itt (itt encoding: age+cid+itt) */
int iscsi_verify_itt(struct iscsi_conn *conn, struct iscsi_hdr *hdr,
		     uint32_t *ret_itt)
{
	struct iscsi_session *session = conn->session;
	struct iscsi_cmd_task *ctask;
	uint32_t itt;

	if (hdr->itt != cpu_to_be32(ISCSI_RESERVED_TAG)) {
		if ((hdr->itt & ISCSI_AGE_MASK) !=
		    (session->age << ISCSI_AGE_SHIFT)) {
			printk(KERN_ERR "iscsi: received itt %x expected "
				"session age (%x)\n", hdr->itt,
				session->age & ISCSI_AGE_MASK);
			return ISCSI_ERR_BAD_ITT;
		}

		if ((hdr->itt & ISCSI_CID_MASK) !=
		    (conn->id << ISCSI_CID_SHIFT)) {
			printk(KERN_ERR "iscsi: received itt %x, expected "
				"CID (%x)\n", hdr->itt, conn->id);
			return ISCSI_ERR_BAD_ITT;
		}
		itt = hdr->itt & ISCSI_ITT_MASK;
	} else
		itt = hdr->itt;

	if (itt < session->cmds_max) {
		ctask = session->cmds[itt];

		if (!ctask->sc) {
			printk(KERN_INFO "iscsi: dropping ctask with "
			       "itt 0x%x\n", ctask->itt);
			/* force drop */
			return ISCSI_ERR_NO_SCSI_CMD;
		}

		if (ctask->sc->SCp.phase != session->age) {
			printk(KERN_ERR "iscsi: ctask's session age %d, "
				"expected %d\n", ctask->sc->SCp.phase,
				session->age);
			return ISCSI_ERR_SESSION_FAILED;
		}
	}

	*ret_itt = itt;
	return 0;
}
EXPORT_SYMBOL_GPL(iscsi_verify_itt);

void iscsi_conn_failure(struct iscsi_conn *conn, enum iscsi_err err)
{
	struct iscsi_session *session = conn->session;
	unsigned long flags;

	spin_lock_irqsave(&session->lock, flags);
	if (session->state == ISCSI_STATE_FAILED) {
		spin_unlock_irqrestore(&session->lock, flags);
		return;
	}

	if (conn->stop_stage == 0)
		session->state = ISCSI_STATE_FAILED;
	spin_unlock_irqrestore(&session->lock, flags);
	set_bit(ISCSI_SUSPEND_BIT, &conn->suspend_tx);
	set_bit(ISCSI_SUSPEND_BIT, &conn->suspend_rx);
	iscsi_conn_error(conn->cls_conn, err);
}
EXPORT_SYMBOL_GPL(iscsi_conn_failure);

/**
 * iscsi_data_xmit - xmit any command into the scheduled connection
 * @conn: iscsi connection
 *
 * Notes:
 *	The function can return -EAGAIN in which case the caller must
 *	re-schedule it again later or recover. '0' return code means
 *	successful xmit.
 **/
static int iscsi_data_xmit(struct iscsi_conn *conn)
{
	struct iscsi_transport *tt;

	if (unlikely(conn->suspend_tx)) {
		debug_scsi("conn %d Tx suspended!\n", conn->id);
		return 0;
	}
	tt = conn->session->tt;

	/*
	 * Transmit in the following order:
	 *
	 * 1) un-finished xmit (ctask or mtask)
	 * 2) immediate control PDUs
	 * 3) write data
	 * 4) SCSI commands
	 * 5) non-immediate control PDUs
	 *
	 * No need to lock around __kfifo_get as long as
	 * there's one producer and one consumer.
	 */

	BUG_ON(conn->ctask && conn->mtask);

	if (conn->ctask) {
		if (tt->xmit_cmd_task(conn, conn->ctask))
			goto again;
		/* done with this in-progress ctask */
		conn->ctask = NULL;
	}
	if (conn->mtask) {
	        if (tt->xmit_mgmt_task(conn, conn->mtask))
		        goto again;
		/* done with this in-progress mtask */
		conn->mtask = NULL;
	}

	/* process immediate first */
        if (unlikely(__kfifo_len(conn->immqueue))) {
	        while (__kfifo_get(conn->immqueue, (void*)&conn->mtask,
			           sizeof(void*))) {
			spin_lock_bh(&conn->session->lock);
			list_add_tail(&conn->mtask->running,
				      &conn->mgmt_run_list);
			spin_unlock_bh(&conn->session->lock);
		        if (tt->xmit_mgmt_task(conn, conn->mtask))
			        goto again;
	        }
		/* done with this mtask */
		conn->mtask = NULL;
	}

	/* process command queue */
	while (__kfifo_get(conn->xmitqueue, (void*)&conn->ctask,
			   sizeof(void*))) {
		/*
		 * iscsi tcp may readd the task to the xmitqueue to send
		 * write data
		 */
		spin_lock_bh(&conn->session->lock);
		if (list_empty(&conn->ctask->running))
			list_add_tail(&conn->ctask->running, &conn->run_list);
		spin_unlock_bh(&conn->session->lock);
		if (tt->xmit_cmd_task(conn, conn->ctask))
			goto again;
	}

	/* done with this ctask */
	conn->ctask = NULL;

	/* process the rest control plane PDUs, if any */
        if (unlikely(__kfifo_len(conn->mgmtqueue))) {
	        while (__kfifo_get(conn->mgmtqueue, (void*)&conn->mtask,
			           sizeof(void*))) {
			spin_lock_bh(&conn->session->lock);
			list_add_tail(&conn->mtask->running,
				      &conn->mgmt_run_list);
			spin_unlock_bh(&conn->session->lock);
		        if (tt->xmit_mgmt_task(conn, conn->mtask))
			        goto again;
	        }
		/* done with this mtask */
		conn->mtask = NULL;
	}

	return 0;

again:
	if (unlikely(conn->suspend_tx))
		return 0;

	return -EAGAIN;
}

static void iscsi_xmitworker(void *data)
{
	struct iscsi_conn *conn = data;

	/*
	 * serialize Xmit worker on a per-connection basis.
	 */
	mutex_lock(&conn->xmitmutex);
	if (iscsi_data_xmit(conn))
		scsi_queue_work(conn->session->host, &conn->xmitwork);
	mutex_unlock(&conn->xmitmutex);
}

enum {
	FAILURE_BAD_HOST = 1,
	FAILURE_SESSION_FAILED,
	FAILURE_SESSION_FREED,
	FAILURE_WINDOW_CLOSED,
	FAILURE_SESSION_TERMINATE,
	FAILURE_SESSION_IN_RECOVERY,
	FAILURE_SESSION_RECOVERY_TIMEOUT,
};

int iscsi_queuecommand(struct scsi_cmnd *sc, void (*done)(struct scsi_cmnd *))
{
	struct Scsi_Host *host;
	int reason = 0;
	struct iscsi_session *session;
	struct iscsi_conn *conn;
	struct iscsi_cmd_task *ctask = NULL;

	sc->scsi_done = done;
	sc->result = 0;

	host = sc->device->host;
	session = iscsi_hostdata(host->hostdata);

	spin_lock(&session->lock);

	/*
	 * ISCSI_STATE_FAILED is a temp. state. The recovery
	 * code will decide what is best to do with command queued
	 * during this time
	 */
	if (session->state != ISCSI_STATE_LOGGED_IN &&
	    session->state != ISCSI_STATE_FAILED) {
		/*
		 * to handle the race between when we set the recovery state
		 * and block the session we requeue here (commands could
		 * be entering our queuecommand while a block is starting
		 * up because the block code is not locked)
		 */
		if (session->state == ISCSI_STATE_IN_RECOVERY) {
			reason = FAILURE_SESSION_IN_RECOVERY;
			goto reject;
		}

		if (session->state == ISCSI_STATE_RECOVERY_FAILED)
			reason = FAILURE_SESSION_RECOVERY_TIMEOUT;
		else if (session->state == ISCSI_STATE_TERMINATE)
			reason = FAILURE_SESSION_TERMINATE;
		else
			reason = FAILURE_SESSION_FREED;
		goto fault;
	}

	/*
	 * Check for iSCSI window and take care of CmdSN wrap-around
	 */
	if ((int)(session->max_cmdsn - session->cmdsn) < 0) {
		reason = FAILURE_WINDOW_CLOSED;
		goto reject;
	}

	conn = session->leadconn;

	__kfifo_get(session->cmdpool.queue, (void*)&ctask, sizeof(void*));
	sc->SCp.phase = session->age;
	sc->SCp.ptr = (char *)ctask;

	ctask->mtask = NULL;
	ctask->conn = conn;
	ctask->sc = sc;
	INIT_LIST_HEAD(&ctask->running);
	ctask->total_length = sc->request_bufflen;
	iscsi_prep_scsi_cmd_pdu(ctask);

	session->tt->init_cmd_task(ctask);

	__kfifo_put(conn->xmitqueue, (void*)&ctask, sizeof(void*));
	debug_scsi(
	       "ctask enq [%s cid %d sc %lx itt 0x%x len %d cmdsn %d win %d]\n",
		sc->sc_data_direction == DMA_TO_DEVICE ? "write" : "read",
		conn->id, (long)sc, ctask->itt, sc->request_bufflen,
		session->cmdsn, session->max_cmdsn - session->exp_cmdsn + 1);
	spin_unlock(&session->lock);

	scsi_queue_work(host, &conn->xmitwork);
	return 0;

reject:
	spin_unlock(&session->lock);
	debug_scsi("cmd 0x%x rejected (%d)\n", sc->cmnd[0], reason);
	return SCSI_MLQUEUE_HOST_BUSY;

fault:
	spin_unlock(&session->lock);
	printk(KERN_ERR "iscsi: cmd 0x%x is not queued (%d)\n",
	       sc->cmnd[0], reason);
	sc->result = (DID_NO_CONNECT << 16);
	sc->resid = sc->request_bufflen;
	sc->scsi_done(sc);
	return 0;
}
EXPORT_SYMBOL_GPL(iscsi_queuecommand);

int iscsi_change_queue_depth(struct scsi_device *sdev, int depth)
{
	if (depth > ISCSI_MAX_CMD_PER_LUN)
		depth = ISCSI_MAX_CMD_PER_LUN;
	scsi_adjust_queue_depth(sdev, scsi_get_tag_type(sdev), depth);
	return sdev->queue_depth;
}
EXPORT_SYMBOL_GPL(iscsi_change_queue_depth);

static int
iscsi_conn_send_generic(struct iscsi_conn *conn, struct iscsi_hdr *hdr,
			char *data, uint32_t data_size)
{
	struct iscsi_session *session = conn->session;
	struct iscsi_nopout *nop = (struct iscsi_nopout *)hdr;
	struct iscsi_mgmt_task *mtask;

	spin_lock_bh(&session->lock);
	if (session->state == ISCSI_STATE_TERMINATE) {
		spin_unlock_bh(&session->lock);
		return -EPERM;
	}
	if (hdr->opcode == (ISCSI_OP_LOGIN | ISCSI_OP_IMMEDIATE) ||
	    hdr->opcode == (ISCSI_OP_TEXT | ISCSI_OP_IMMEDIATE))
		/*
		 * Login and Text are sent serially, in
		 * request-followed-by-response sequence.
		 * Same mtask can be used. Same ITT must be used.
		 * Note that login_mtask is preallocated at conn_create().
		 */
		mtask = conn->login_mtask;
	else {
		BUG_ON(conn->c_stage == ISCSI_CONN_INITIAL_STAGE);
		BUG_ON(conn->c_stage == ISCSI_CONN_STOPPED);

		nop->exp_statsn = cpu_to_be32(conn->exp_statsn);
		if (!__kfifo_get(session->mgmtpool.queue,
				 (void*)&mtask, sizeof(void*))) {
			spin_unlock_bh(&session->lock);
			return -ENOSPC;
		}
	}

	/*
	 * pre-format CmdSN for outgoing PDU.
	 */
	if (hdr->itt != cpu_to_be32(ISCSI_RESERVED_TAG)) {
		hdr->itt = mtask->itt | (conn->id << ISCSI_CID_SHIFT) |
			   (session->age << ISCSI_AGE_SHIFT);
		nop->cmdsn = cpu_to_be32(session->cmdsn);
		if (conn->c_stage == ISCSI_CONN_STARTED &&
		    !(hdr->opcode & ISCSI_OP_IMMEDIATE))
			session->cmdsn++;
	} else
		/* do not advance CmdSN */
		nop->cmdsn = cpu_to_be32(session->cmdsn);

	if (data_size) {
		memcpy(mtask->data, data, data_size);
		mtask->data_count = data_size;
	} else
		mtask->data_count = 0;

	INIT_LIST_HEAD(&mtask->running);
	memcpy(mtask->hdr, hdr, sizeof(struct iscsi_hdr));
	if (session->tt->init_mgmt_task)
		session->tt->init_mgmt_task(conn, mtask, data, data_size);
	spin_unlock_bh(&session->lock);

	debug_scsi("mgmtpdu [op 0x%x hdr->itt 0x%x datalen %d]\n",
		   hdr->opcode, hdr->itt, data_size);

	/*
	 * since send_pdu() could be called at least from two contexts,
	 * we need to serialize __kfifo_put, so we don't have to take
	 * additional lock on fast data-path
	 */
        if (hdr->opcode & ISCSI_OP_IMMEDIATE)
	        __kfifo_put(conn->immqueue, (void*)&mtask, sizeof(void*));
	else
	        __kfifo_put(conn->mgmtqueue, (void*)&mtask, sizeof(void*));

	scsi_queue_work(session->host, &conn->xmitwork);
	return 0;
}

int iscsi_conn_send_pdu(struct iscsi_cls_conn *cls_conn, struct iscsi_hdr *hdr,
			char *data, uint32_t data_size)
{
	struct iscsi_conn *conn = cls_conn->dd_data;
	int rc;

	mutex_lock(&conn->xmitmutex);
	rc = iscsi_conn_send_generic(conn, hdr, data, data_size);
	mutex_unlock(&conn->xmitmutex);

	return rc;
}
EXPORT_SYMBOL_GPL(iscsi_conn_send_pdu);

void iscsi_session_recovery_timedout(struct iscsi_cls_session *cls_session)
{
	struct iscsi_session *session = class_to_transport_session(cls_session);
	struct iscsi_conn *conn = session->leadconn;

	spin_lock_bh(&session->lock);
	if (session->state != ISCSI_STATE_LOGGED_IN) {
		session->state = ISCSI_STATE_RECOVERY_FAILED;
		if (conn)
			wake_up(&conn->ehwait);
	}
	spin_unlock_bh(&session->lock);
}
EXPORT_SYMBOL_GPL(iscsi_session_recovery_timedout);

int iscsi_eh_host_reset(struct scsi_cmnd *sc)
{
	struct Scsi_Host *host = sc->device->host;
	struct iscsi_session *session = iscsi_hostdata(host->hostdata);
	struct iscsi_conn *conn = session->leadconn;
	int fail_session = 0;

	spin_lock_bh(&session->lock);
	if (session->state == ISCSI_STATE_TERMINATE) {
failed:
		debug_scsi("failing host reset: session terminated "
			   "[CID %d age %d]", conn->id, session->age);
		spin_unlock_bh(&session->lock);
		return FAILED;
	}

	if (sc->SCp.phase == session->age) {
		debug_scsi("failing connection CID %d due to SCSI host reset",
			   conn->id);
		fail_session = 1;
	}
	spin_unlock_bh(&session->lock);

	/*
	 * we drop the lock here but the leadconn cannot be destoyed while
	 * we are in the scsi eh
	 */
	if (fail_session)
		iscsi_conn_failure(conn, ISCSI_ERR_CONN_FAILED);

	debug_scsi("iscsi_eh_host_reset wait for relogin\n");
	wait_event_interruptible(conn->ehwait,
				 session->state == ISCSI_STATE_TERMINATE ||
				 session->state == ISCSI_STATE_LOGGED_IN ||
				 session->state == ISCSI_STATE_RECOVERY_FAILED);
	if (signal_pending(current))
		flush_signals(current);

	spin_lock_bh(&session->lock);
	if (session->state == ISCSI_STATE_LOGGED_IN)
		printk(KERN_INFO "iscsi: host reset succeeded\n");
	else
		goto failed;
	spin_unlock_bh(&session->lock);

	return SUCCESS;
}
EXPORT_SYMBOL_GPL(iscsi_eh_host_reset);

static void iscsi_tmabort_timedout(unsigned long data)
{
	struct iscsi_cmd_task *ctask = (struct iscsi_cmd_task *)data;
	struct iscsi_conn *conn = ctask->conn;
	struct iscsi_session *session = conn->session;

	spin_lock(&session->lock);
	if (conn->tmabort_state == TMABORT_INITIAL) {
		conn->tmabort_state = TMABORT_TIMEDOUT;
		debug_scsi("tmabort timedout [sc %p itt 0x%x]\n",
			ctask->sc, ctask->itt);
		/* unblock eh_abort() */
		wake_up(&conn->ehwait);
	}
	spin_unlock(&session->lock);
}

/* must be called with the mutex lock */
static int iscsi_exec_abort_task(struct scsi_cmnd *sc,
				 struct iscsi_cmd_task *ctask)
{
	struct iscsi_conn *conn = ctask->conn;
	struct iscsi_session *session = conn->session;
	struct iscsi_tm *hdr = &conn->tmhdr;
	int rc;

	/*
	 * ctask timed out but session is OK requests must be serialized.
	 */
	memset(hdr, 0, sizeof(struct iscsi_tm));
	hdr->opcode = ISCSI_OP_SCSI_TMFUNC | ISCSI_OP_IMMEDIATE;
	hdr->flags = ISCSI_TM_FUNC_ABORT_TASK;
	hdr->flags |= ISCSI_FLAG_CMD_FINAL;
	memcpy(hdr->lun, ctask->hdr->lun, sizeof(hdr->lun));
	hdr->rtt = ctask->hdr->itt;
	hdr->refcmdsn = ctask->hdr->cmdsn;

	rc = iscsi_conn_send_generic(conn, (struct iscsi_hdr *)hdr,
				     NULL, 0);
	if (rc) {
		iscsi_conn_failure(conn, ISCSI_ERR_CONN_FAILED);
		debug_scsi("abort sent failure [itt 0x%x] %d", ctask->itt, rc);
		return rc;
	}

	debug_scsi("abort sent [itt 0x%x]\n", ctask->itt);

	spin_lock_bh(&session->lock);
	ctask->mtask = (struct iscsi_mgmt_task *)
			session->mgmt_cmds[(hdr->itt & ISCSI_ITT_MASK) -
					ISCSI_MGMT_ITT_OFFSET];

	if (conn->tmabort_state == TMABORT_INITIAL) {
		conn->tmfcmd_pdus_cnt++;
		conn->tmabort_timer.expires = 10*HZ + jiffies;
		conn->tmabort_timer.function = iscsi_tmabort_timedout;
		conn->tmabort_timer.data = (unsigned long)ctask;
		add_timer(&conn->tmabort_timer);
		debug_scsi("abort set timeout [itt 0x%x]", ctask->itt);
	}
	spin_unlock_bh(&session->lock);
	mutex_unlock(&conn->xmitmutex);

	/*
	 * block eh thread until:
	 *
	 * 1) abort response
	 * 2) abort timeout
	 * 3) session is terminated or restarted or userspace has
	 * given up on recovery
	 */
	wait_event_interruptible(conn->ehwait,
				 sc->SCp.phase != session->age ||
				 session->state != ISCSI_STATE_LOGGED_IN ||
				 conn->tmabort_state != TMABORT_INITIAL);
	if (signal_pending(current))
		flush_signals(current);
	del_timer_sync(&conn->tmabort_timer);

	mutex_lock(&conn->xmitmutex);
	return 0;
}

/*
 * xmit mutex and session lock must be held
 */
#define iscsi_remove_task(tasktype)					\
static struct iscsi_##tasktype *					\
iscsi_remove_##tasktype(struct kfifo *fifo, uint32_t itt)		\
{									\
	int i, nr_tasks = __kfifo_len(fifo) / sizeof(void*);		\
	struct iscsi_##tasktype *task;					\
									\
	debug_scsi("searching %d tasks\n", nr_tasks);			\
									\
	for (i = 0; i < nr_tasks; i++) {				\
		__kfifo_get(fifo, (void*)&task, sizeof(void*));		\
		debug_scsi("check task %u\n", task->itt);		\
									\
		if (task->itt == itt) {					\
			debug_scsi("matched task\n");			\
			return task;					\
		}							\
									\
		__kfifo_put(fifo, (void*)&task, sizeof(void*));		\
	}								\
	return NULL;							\
}

iscsi_remove_task(mgmt_task);
iscsi_remove_task(cmd_task);

static int iscsi_ctask_mtask_cleanup(struct iscsi_cmd_task *ctask)
{
	struct iscsi_conn *conn = ctask->conn;
	struct iscsi_session *session = conn->session;

	if (!ctask->mtask)
		return -EINVAL;

	if (!iscsi_remove_mgmt_task(conn->immqueue, ctask->mtask->itt))
		list_del(&ctask->mtask->running);
	__kfifo_put(session->mgmtpool.queue, (void*)&ctask->mtask,
		    sizeof(void*));
	ctask->mtask = NULL;
	return 0;
}

/*
 * session lock and xmitmutex must be held
 */
static void fail_command(struct iscsi_conn *conn, struct iscsi_cmd_task *ctask,
			 int err)
{
	struct scsi_cmnd *sc;

	conn->session->tt->cleanup_cmd_task(conn, ctask);
	iscsi_ctask_mtask_cleanup(ctask);

	sc = ctask->sc;
	if (!sc)
		return;
	sc->result = err;
	sc->resid = sc->request_bufflen;
	iscsi_complete_command(conn->session, ctask);
}

int iscsi_eh_abort(struct scsi_cmnd *sc)
{
	struct iscsi_cmd_task *ctask = (struct iscsi_cmd_task *)sc->SCp.ptr;
	struct iscsi_conn *conn = ctask->conn;
	struct iscsi_session *session = conn->session;
	struct iscsi_cmd_task *pending_ctask;
	int rc;

	conn->eh_abort_cnt++;
	debug_scsi("aborting [sc %p itt 0x%x]\n", sc, ctask->itt);

	mutex_lock(&conn->xmitmutex);
	spin_lock_bh(&session->lock);

	/*
	 * If we are not logged in or we have started a new session
	 * then let the host reset code handle this
	 */
	if (session->state != ISCSI_STATE_LOGGED_IN ||
	    sc->SCp.phase != session->age)
		goto failed;

	/* ctask completed before time out */
	if (!ctask->sc)
		goto success;

	/* what should we do here ? */
	if (conn->ctask == ctask) {
		printk(KERN_INFO "iscsi: sc %p itt 0x%x partially sent. "
		       "Failing abort\n", sc, ctask->itt);
		goto failed;
	}

	/* check for the easy pending cmd abort */
	pending_ctask = iscsi_remove_cmd_task(conn->xmitqueue, ctask->itt);
	if (pending_ctask) {
		/* iscsi_tcp queues write transfers on the xmitqueue */
		if (list_empty(&pending_ctask->running)) {
			debug_scsi("found pending task\n");
			goto success;
		} else
			__kfifo_put(conn->xmitqueue, (void*)&pending_ctask,
				    sizeof(void*));
	}

	conn->tmabort_state = TMABORT_INITIAL;

	spin_unlock_bh(&session->lock);
	rc = iscsi_exec_abort_task(sc, ctask);
	spin_lock_bh(&session->lock);

	iscsi_ctask_mtask_cleanup(ctask);
	if (rc || sc->SCp.phase != session->age ||
	    session->state != ISCSI_STATE_LOGGED_IN)
		goto failed;

	/* ctask completed before tmf abort response */
	if (!ctask->sc) {
		debug_scsi("sc completed while abort in progress\n");
		goto success;
	}

	if (conn->tmabort_state != TMABORT_SUCCESS) {
		spin_unlock_bh(&session->lock);
		iscsi_conn_failure(conn, ISCSI_ERR_CONN_FAILED);
		spin_lock_bh(&session->lock);
		goto failed;
	}

success:
	debug_scsi("abort success [sc %lx itt 0x%x]\n", (long)sc, ctask->itt);
	spin_unlock_bh(&session->lock);

	/*
	 * clean up task if aborted. we have the xmitmutex so grab
	 * the recv lock as a writer
	 */
	write_lock_bh(conn->recv_lock);
	spin_lock(&session->lock);
	fail_command(conn, ctask, DID_ABORT << 16);
	spin_unlock(&session->lock);
	write_unlock_bh(conn->recv_lock);

	mutex_unlock(&conn->xmitmutex);
	return SUCCESS;

failed:
	spin_unlock_bh(&session->lock);
	mutex_unlock(&conn->xmitmutex);

	debug_scsi("abort failed [sc %lx itt 0x%x]\n", (long)sc, ctask->itt);
	return FAILED;
}
EXPORT_SYMBOL_GPL(iscsi_eh_abort);

int
iscsi_pool_init(struct iscsi_queue *q, int max, void ***items, int item_size)
{
	int i;

	*items = kmalloc(max * sizeof(void*), GFP_KERNEL);
	if (*items == NULL)
		return -ENOMEM;

	q->max = max;
	q->pool = kmalloc(max * sizeof(void*), GFP_KERNEL);
	if (q->pool == NULL) {
		kfree(*items);
		return -ENOMEM;
	}

	q->queue = kfifo_init((void*)q->pool, max * sizeof(void*),
			      GFP_KERNEL, NULL);
	if (q->queue == ERR_PTR(-ENOMEM)) {
		kfree(q->pool);
		kfree(*items);
		return -ENOMEM;
	}

	for (i = 0; i < max; i++) {
		q->pool[i] = kmalloc(item_size, GFP_KERNEL);
		if (q->pool[i] == NULL) {
			int j;

			for (j = 0; j < i; j++)
				kfree(q->pool[j]);

			kfifo_free(q->queue);
			kfree(q->pool);
			kfree(*items);
			return -ENOMEM;
		}
		memset(q->pool[i], 0, item_size);
		(*items)[i] = q->pool[i];
		__kfifo_put(q->queue, (void*)&q->pool[i], sizeof(void*));
	}
	return 0;
}
EXPORT_SYMBOL_GPL(iscsi_pool_init);

void iscsi_pool_free(struct iscsi_queue *q, void **items)
{
	int i;

	for (i = 0; i < q->max; i++)
		kfree(items[i]);
	kfree(q->pool);
	kfree(items);
}
EXPORT_SYMBOL_GPL(iscsi_pool_free);

/*
 * iSCSI Session's hostdata organization:
 *
 *    *------------------* <== hostdata_session(host->hostdata)
 *    | ptr to class sess|
 *    |------------------| <== iscsi_hostdata(host->hostdata)
 *    | iscsi_session    |
 *    *------------------*
 */

#define hostdata_privsize(_sz)	(sizeof(unsigned long) + _sz + \
				 _sz % sizeof(unsigned long))

#define hostdata_session(_hostdata) (iscsi_ptr(*(unsigned long *)_hostdata))

/**
 * iscsi_session_setup - create iscsi cls session and host and session
 * @scsit: scsi transport template
 * @iscsit: iscsi transport template
 * @initial_cmdsn: initial CmdSN
 * @hostno: host no allocated
 *
 * This can be used by software iscsi_transports that allocate
 * a session per scsi host.
 **/
struct iscsi_cls_session *
iscsi_session_setup(struct iscsi_transport *iscsit,
		    struct scsi_transport_template *scsit,
		    int cmd_task_size, int mgmt_task_size,
		    uint32_t initial_cmdsn, uint32_t *hostno)
{
	struct Scsi_Host *shost;
	struct iscsi_session *session;
	struct iscsi_cls_session *cls_session;
	int cmd_i;

	shost = scsi_host_alloc(iscsit->host_template,
				hostdata_privsize(sizeof(*session)));
	if (!shost)
		return NULL;

	shost->max_id = 1;
	shost->max_channel = 0;
	shost->max_lun = iscsit->max_lun;
	shost->max_cmd_len = iscsit->max_cmd_len;
	shost->transportt = scsit;
	shost->transportt->create_work_queue = 1;
	*hostno = shost->host_no;

	session = iscsi_hostdata(shost->hostdata);
	memset(session, 0, sizeof(struct iscsi_session));
	session->host = shost;
	session->state = ISCSI_STATE_FREE;
	session->mgmtpool_max = ISCSI_MGMT_CMDS_MAX;
	session->cmds_max = ISCSI_XMIT_CMDS_MAX;
	session->cmdsn = initial_cmdsn;
	session->exp_cmdsn = initial_cmdsn + 1;
	session->max_cmdsn = initial_cmdsn + 1;
	session->max_r2t = 1;
	session->tt = iscsit;

	/* initialize SCSI PDU commands pool */
	if (iscsi_pool_init(&session->cmdpool, session->cmds_max,
			    (void***)&session->cmds,
			    cmd_task_size + sizeof(struct iscsi_cmd_task)))
		goto cmdpool_alloc_fail;

	/* pre-format cmds pool with ITT */
	for (cmd_i = 0; cmd_i < session->cmds_max; cmd_i++) {
		struct iscsi_cmd_task *ctask = session->cmds[cmd_i];

		if (cmd_task_size)
			ctask->dd_data = &ctask[1];
		ctask->itt = cmd_i;
	}

	spin_lock_init(&session->lock);
	INIT_LIST_HEAD(&session->connections);

	/* initialize immediate command pool */
	if (iscsi_pool_init(&session->mgmtpool, session->mgmtpool_max,
			   (void***)&session->mgmt_cmds,
			   mgmt_task_size + sizeof(struct iscsi_mgmt_task)))
		goto mgmtpool_alloc_fail;


	/* pre-format immediate cmds pool with ITT */
	for (cmd_i = 0; cmd_i < session->mgmtpool_max; cmd_i++) {
		struct iscsi_mgmt_task *mtask = session->mgmt_cmds[cmd_i];

		if (mgmt_task_size)
			mtask->dd_data = &mtask[1];
		mtask->itt = ISCSI_MGMT_ITT_OFFSET + cmd_i;
	}

	if (scsi_add_host(shost, NULL))
		goto add_host_fail;

	cls_session = iscsi_create_session(shost, iscsit, 0);
	if (!cls_session)
		goto cls_session_fail;
	*(unsigned long*)shost->hostdata = (unsigned long)cls_session;

	return cls_session;

cls_session_fail:
	scsi_remove_host(shost);
add_host_fail:
	iscsi_pool_free(&session->mgmtpool, (void**)session->mgmt_cmds);
mgmtpool_alloc_fail:
	iscsi_pool_free(&session->cmdpool, (void**)session->cmds);
cmdpool_alloc_fail:
	scsi_host_put(shost);
	return NULL;
}
EXPORT_SYMBOL_GPL(iscsi_session_setup);

/**
 * iscsi_session_teardown - destroy session, host, and cls_session
 * shost: scsi host
 *
 * This can be used by software iscsi_transports that allocate
 * a session per scsi host.
 **/
void iscsi_session_teardown(struct iscsi_cls_session *cls_session)
{
	struct Scsi_Host *shost = iscsi_session_to_shost(cls_session);
	struct iscsi_session *session = iscsi_hostdata(shost->hostdata);

	scsi_remove_host(shost);

	iscsi_pool_free(&session->mgmtpool, (void**)session->mgmt_cmds);
	iscsi_pool_free(&session->cmdpool, (void**)session->cmds);

	iscsi_destroy_session(cls_session);
	scsi_host_put(shost);
}
EXPORT_SYMBOL_GPL(iscsi_session_teardown);

/**
 * iscsi_conn_setup - create iscsi_cls_conn and iscsi_conn
 * @cls_session: iscsi_cls_session
 * @conn_idx: cid
 **/
struct iscsi_cls_conn *
iscsi_conn_setup(struct iscsi_cls_session *cls_session, uint32_t conn_idx)
{
	struct iscsi_session *session = class_to_transport_session(cls_session);
	struct iscsi_conn *conn;
	struct iscsi_cls_conn *cls_conn;
	char *data;

	cls_conn = iscsi_create_conn(cls_session, conn_idx);
	if (!cls_conn)
		return NULL;
	conn = cls_conn->dd_data;
	memset(conn, 0, sizeof(*conn));

	conn->session = session;
	conn->cls_conn = cls_conn;
	conn->c_stage = ISCSI_CONN_INITIAL_STAGE;
	conn->id = conn_idx;
	conn->exp_statsn = 0;
	conn->tmabort_state = TMABORT_INITIAL;
	INIT_LIST_HEAD(&conn->run_list);
	INIT_LIST_HEAD(&conn->mgmt_run_list);

	/* initialize general xmit PDU commands queue */
	conn->xmitqueue = kfifo_alloc(session->cmds_max * sizeof(void*),
					GFP_KERNEL, NULL);
	if (conn->xmitqueue == ERR_PTR(-ENOMEM))
		goto xmitqueue_alloc_fail;

	/* initialize general immediate & non-immediate PDU commands queue */
	conn->immqueue = kfifo_alloc(session->mgmtpool_max * sizeof(void*),
			                GFP_KERNEL, NULL);
	if (conn->immqueue == ERR_PTR(-ENOMEM))
		goto immqueue_alloc_fail;

	conn->mgmtqueue = kfifo_alloc(session->mgmtpool_max * sizeof(void*),
			                GFP_KERNEL, NULL);
	if (conn->mgmtqueue == ERR_PTR(-ENOMEM))
		goto mgmtqueue_alloc_fail;

	INIT_WORK(&conn->xmitwork, iscsi_xmitworker, conn);

	/* allocate login_mtask used for the login/text sequences */
	spin_lock_bh(&session->lock);
	if (!__kfifo_get(session->mgmtpool.queue,
                         (void*)&conn->login_mtask,
			 sizeof(void*))) {
		spin_unlock_bh(&session->lock);
		goto login_mtask_alloc_fail;
	}
	spin_unlock_bh(&session->lock);

	data = kmalloc(DEFAULT_MAX_RECV_DATA_SEGMENT_LENGTH, GFP_KERNEL);
	if (!data)
		goto login_mtask_data_alloc_fail;
	conn->login_mtask->data = data;

	init_timer(&conn->tmabort_timer);
	mutex_init(&conn->xmitmutex);
	init_waitqueue_head(&conn->ehwait);

	return cls_conn;

login_mtask_data_alloc_fail:
	__kfifo_put(session->mgmtpool.queue, (void*)&conn->login_mtask,
		    sizeof(void*));
login_mtask_alloc_fail:
	kfifo_free(conn->mgmtqueue);
mgmtqueue_alloc_fail:
	kfifo_free(conn->immqueue);
immqueue_alloc_fail:
	kfifo_free(conn->xmitqueue);
xmitqueue_alloc_fail:
	iscsi_destroy_conn(cls_conn);
	return NULL;
}
EXPORT_SYMBOL_GPL(iscsi_conn_setup);

/**
 * iscsi_conn_teardown - teardown iscsi connection
 * cls_conn: iscsi class connection
 *
 * TODO: we may need to make this into a two step process
 * like scsi-mls remove + put host
 */
void iscsi_conn_teardown(struct iscsi_cls_conn *cls_conn)
{
	struct iscsi_conn *conn = cls_conn->dd_data;
	struct iscsi_session *session = conn->session;
	unsigned long flags;

	set_bit(ISCSI_SUSPEND_BIT, &conn->suspend_tx);
	mutex_lock(&conn->xmitmutex);
	if (conn->c_stage == ISCSI_CONN_INITIAL_STAGE) {
		if (session->tt->suspend_conn_recv)
			session->tt->suspend_conn_recv(conn);

		session->tt->terminate_conn(conn);
	}

	spin_lock_bh(&session->lock);
	conn->c_stage = ISCSI_CONN_CLEANUP_WAIT;
	if (session->leadconn == conn) {
		/*
		 * leading connection? then give up on recovery.
		 */
		session->state = ISCSI_STATE_TERMINATE;
		wake_up(&conn->ehwait);
	}
	spin_unlock_bh(&session->lock);

	mutex_unlock(&conn->xmitmutex);

	/*
	 * Block until all in-progress commands for this connection
	 * time out or fail.
	 */
	for (;;) {
		spin_lock_irqsave(session->host->host_lock, flags);
		if (!session->host->host_busy) { /* OK for ERL == 0 */
			spin_unlock_irqrestore(session->host->host_lock, flags);
			break;
		}
		spin_unlock_irqrestore(session->host->host_lock, flags);
		msleep_interruptible(500);
		printk(KERN_INFO "iscsi: scsi conn_destroy(): host_busy %d "
		       "host_failed %d\n", session->host->host_busy,
		       session->host->host_failed);
		/*
		 * force eh_abort() to unblock
		 */
		wake_up(&conn->ehwait);
	}

	spin_lock_bh(&session->lock);
	kfree(conn->login_mtask->data);
	__kfifo_put(session->mgmtpool.queue, (void*)&conn->login_mtask,
		    sizeof(void*));
	list_del(&conn->item);
	if (list_empty(&session->connections))
		session->leadconn = NULL;
	if (session->leadconn && session->leadconn == conn)
		session->leadconn = container_of(session->connections.next,
			struct iscsi_conn, item);

	if (session->leadconn == NULL)
		/* no connections exits.. reset sequencing */
		session->cmdsn = session->max_cmdsn = session->exp_cmdsn = 1;
	spin_unlock_bh(&session->lock);

	kfifo_free(conn->xmitqueue);
	kfifo_free(conn->immqueue);
	kfifo_free(conn->mgmtqueue);

	iscsi_destroy_conn(cls_conn);
}
EXPORT_SYMBOL_GPL(iscsi_conn_teardown);

int iscsi_conn_start(struct iscsi_cls_conn *cls_conn)
{
	struct iscsi_conn *conn = cls_conn->dd_data;
	struct iscsi_session *session = conn->session;

	if (session == NULL) {
		printk(KERN_ERR "iscsi: can't start unbound connection\n");
		return -EPERM;
	}

	spin_lock_bh(&session->lock);
	conn->c_stage = ISCSI_CONN_STARTED;
	session->state = ISCSI_STATE_LOGGED_IN;

	switch(conn->stop_stage) {
	case STOP_CONN_RECOVER:
		/*
		 * unblock eh_abort() if it is blocked. re-try all
		 * commands after successful recovery
		 */
		conn->stop_stage = 0;
		conn->tmabort_state = TMABORT_INITIAL;
		session->age++;
		spin_unlock_bh(&session->lock);

		iscsi_unblock_session(session_to_cls(session));
		wake_up(&conn->ehwait);
		return 0;
	case STOP_CONN_TERM:
		conn->stop_stage = 0;
		break;
	default:
		break;
	}
	spin_unlock_bh(&session->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(iscsi_conn_start);

static void
flush_control_queues(struct iscsi_session *session, struct iscsi_conn *conn)
{
	struct iscsi_mgmt_task *mtask, *tmp;

	/* handle pending */
	while (__kfifo_get(conn->immqueue, (void*)&mtask, sizeof(void*)) ||
	       __kfifo_get(conn->mgmtqueue, (void*)&mtask, sizeof(void*))) {
		if (mtask == conn->login_mtask)
			continue;
		debug_scsi("flushing pending mgmt task itt 0x%x\n", mtask->itt);
		__kfifo_put(session->mgmtpool.queue, (void*)&mtask,
			    sizeof(void*));
	}

	/* handle running */
	list_for_each_entry_safe(mtask, tmp, &conn->mgmt_run_list, running) {
		debug_scsi("flushing running mgmt task itt 0x%x\n", mtask->itt);
		list_del(&mtask->running);

		if (mtask == conn->login_mtask)
			continue;
		__kfifo_put(session->mgmtpool.queue, (void*)&mtask,
			   sizeof(void*));
	}

	conn->mtask = NULL;
}

/* Fail commands. Mutex and session lock held and recv side suspended */
static void fail_all_commands(struct iscsi_conn *conn)
{
	struct iscsi_cmd_task *ctask, *tmp;

	/* flush pending */
	while (__kfifo_get(conn->xmitqueue, (void*)&ctask, sizeof(void*))) {
		debug_scsi("failing pending sc %p itt 0x%x\n", ctask->sc,
			   ctask->itt);
		fail_command(conn, ctask, DID_BUS_BUSY << 16);
	}

	/* fail all other running */
	list_for_each_entry_safe(ctask, tmp, &conn->run_list, running) {
		debug_scsi("failing in progress sc %p itt 0x%x\n",
			   ctask->sc, ctask->itt);
		fail_command(conn, ctask, DID_BUS_BUSY << 16);
	}

	conn->ctask = NULL;
}

static void iscsi_start_session_recovery(struct iscsi_session *session,
					 struct iscsi_conn *conn, int flag)
{
	int old_stop_stage;

	spin_lock_bh(&session->lock);
	if (conn->stop_stage == STOP_CONN_TERM) {
		spin_unlock_bh(&session->lock);
		return;
	}

	/*
	 * When this is called for the in_login state, we only want to clean
	 * up the login task and connection. We do not need to block and set
	 * the recovery state again
	 */
	if (flag == STOP_CONN_TERM)
		session->state = ISCSI_STATE_TERMINATE;
	else if (conn->stop_stage != STOP_CONN_RECOVER)
		session->state = ISCSI_STATE_IN_RECOVERY;

	old_stop_stage = conn->stop_stage;
	conn->stop_stage = flag;
	conn->c_stage = ISCSI_CONN_STOPPED;
	set_bit(ISCSI_SUSPEND_BIT, &conn->suspend_tx);
	spin_unlock_bh(&session->lock);

	if (session->tt->suspend_conn_recv)
		session->tt->suspend_conn_recv(conn);

	mutex_lock(&conn->xmitmutex);
	/*
	 * for connection level recovery we should not calculate
	 * header digest. conn->hdr_size used for optimization
	 * in hdr_extract() and will be re-negotiated at
	 * set_param() time.
	 */
	if (flag == STOP_CONN_RECOVER) {
		conn->hdrdgst_en = 0;
		conn->datadgst_en = 0;
		if (session->state == ISCSI_STATE_IN_RECOVERY &&
		    old_stop_stage != STOP_CONN_RECOVER) {
			debug_scsi("blocking session\n");
			iscsi_block_session(session_to_cls(session));
		}
	}

	session->tt->terminate_conn(conn);
	/*
	 * flush queues.
	 */
	spin_lock_bh(&session->lock);
	fail_all_commands(conn);
	flush_control_queues(session, conn);
	spin_unlock_bh(&session->lock);

	mutex_unlock(&conn->xmitmutex);
}

void iscsi_conn_stop(struct iscsi_cls_conn *cls_conn, int flag)
{
	struct iscsi_conn *conn = cls_conn->dd_data;
	struct iscsi_session *session = conn->session;

	switch (flag) {
	case STOP_CONN_RECOVER:
	case STOP_CONN_TERM:
		iscsi_start_session_recovery(session, conn, flag);
		break;
	default:
		printk(KERN_ERR "iscsi: invalid stop flag %d\n", flag);
	}
}
EXPORT_SYMBOL_GPL(iscsi_conn_stop);

int iscsi_conn_bind(struct iscsi_cls_session *cls_session,
		    struct iscsi_cls_conn *cls_conn, int is_leading)
{
	struct iscsi_session *session = class_to_transport_session(cls_session);
	struct iscsi_conn *tmp = ERR_PTR(-EEXIST), *conn = cls_conn->dd_data;

	/* lookup for existing connection */
	spin_lock_bh(&session->lock);
	list_for_each_entry(tmp, &session->connections, item) {
		if (tmp == conn) {
			if (conn->c_stage != ISCSI_CONN_STOPPED ||
			    conn->stop_stage == STOP_CONN_TERM) {
				printk(KERN_ERR "iscsi: can't bind "
				       "non-stopped connection (%d:%d)\n",
				       conn->c_stage, conn->stop_stage);
				spin_unlock_bh(&session->lock);
				return -EIO;
			}
			break;
		}
	}
	if (tmp != conn) {
		/* bind new iSCSI connection to session */
		conn->session = session;
		list_add(&conn->item, &session->connections);
	}
	spin_unlock_bh(&session->lock);

	if (is_leading)
		session->leadconn = conn;

	/*
	 * Unblock xmitworker(), Login Phase will pass through.
	 */
	clear_bit(ISCSI_SUSPEND_BIT, &conn->suspend_rx);
	clear_bit(ISCSI_SUSPEND_BIT, &conn->suspend_tx);
	return 0;
}
EXPORT_SYMBOL_GPL(iscsi_conn_bind);

MODULE_AUTHOR("Mike Christie");
MODULE_DESCRIPTION("iSCSI library functions");
MODULE_LICENSE("GPL");

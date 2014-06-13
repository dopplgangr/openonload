/*
** Copyright 2005-2012  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of version 2 of the GNU General Public License as
** published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

/**************************************************************************\
*//*! \file
** <L5_PRIVATE L5_SOURCE>
** \author  djr
**  \brief  TCP recvmsg() etc.
**   \date  2003/09/02
**    \cop  (c) Level 5 Networks Limited.
** </L5_PRIVATE>
*//*
\**************************************************************************/

/*! \cidoxg_lib_transport_ip */

#include "ip_internal.h"


#define LPF "TCP RECV "

# define CI_MSG_TRUNC   MSG_TRUNC


static int ci_tcp_recvmsg_urg(ci_netif* ni, ci_tcp_state* ts,
                              struct msghdr* msg, int flags);

static int ci_tcp_recvmsg_recv2(const ci_tcp_recvmsg_args*, ci_iovec_ptr* piov,
                                int* bytes_so_far);


/*
 * \todo It looks like it's common with getpeername().
 */
ci_inline void
ci_tcp_recv_fill_msgname(ci_tcp_state* ts, struct sockaddr *name,
                         socklen_t *namelen)
{
#if CI_CFG_TCP_RECVMSG_MSGNAME
  if( name ) {
    struct sockaddr_in* sinp;
    struct sockaddr_in  sin_buf;

    ci_assert(ts);
    ci_assert(namelen);

    if( CI_LIKELY(*namelen >= sizeof(struct sockaddr_in)) ) {
      sinp = (struct sockaddr_in *)name;
      sinp->sin_family = AF_INET;
      sinp->sin_port = TS_TCP(ts)->tcp_dest_be16;
      sinp->sin_addr.s_addr = ts->s.pkt.ip.ip_daddr_be32;
      *namelen = sizeof(struct sockaddr_in);
    }
    else {
      sin_buf.sin_family = AF_INET;
      sin_buf.sin_port = TS_TCP(ts)->tcp_dest_be16;
      sin_buf.sin_addr.s_addr = ts->s.pkt.ip.ip_daddr_be32;
      memcpy(name, &sin_buf, *namelen);
    }
  }
#else
  *namelen = 0;
#endif
}


int ci_tcp_send_wnd_update(ci_netif* ni, ci_tcp_state* ts)
{
  int max_window;

  ci_assert(ci_netif_is_locked(ni));

  max_window = CI_MIN(tcp_rcv_buff(ts), (0xffff << ts->rcv_wscl));
  
  if(CI_UNLIKELY( ! (ts->s.b.state & CI_TCP_STATE_ACCEPT_DATA) ))
    return 0;

  if( SEQ_SUB(ts->rcv_delivered + max_window,
              tcp_rcv_wnd_right_edge_sent(ts))
      >= ci_tcp_ack_trigger_delta(ts) ) {
    ci_ip_pkt_fmt* pkt = ci_netif_pkt_alloc(ni);
    if( pkt ) {
      LOG_TR(log(LNTS_FMT "window update advertised=%d",
                 LNTS_PRI_ARGS(ni, ts), tcp_rcv_wnd_advertised(ts)));
      CITP_STATS_NETIF_INC(ni, wnd_updates_sent);
      ci_tcp_send_ack(ni, ts, pkt);
      /* Update the ack trigger so we won't attempt to send another windows
      ** update for a while.
      */
      ts->ack_trigger += ci_tcp_ack_trigger_delta(ts);
      return 1;
    }
  }
  return 0;
}


/* This is called after we've pulled a certain amount of data from the
** receive queue, and sends a window update if appropriate.
*/
static void ci_tcp_recvmsg_send_wnd_update(ci_netif* ni, ci_tcp_state* ts,
                                           int flags)
{
  /* This means we're being called from within a ci_netif_poll(), so an ACK
  ** is going to be sent real soon anyway.  We also must not call
  ** ci_netif_poll() in this case. */
  if( flags & CI_MSG_NO_WND_UPDATE )  return;

  if( ! (flags & CI_MSG_NETIF_LOCKED) && ! ci_netif_trylock(ni) ) {
    ci_bit_set(&ts->s.s_aflags, CI_SOCK_AFLAG_NEED_ACK_BIT);
    if( ! ci_netif_lock_or_defer_work(ni, &ts->s.b) )
      return;
    ci_bit_clear(&ts->s.s_aflags, CI_SOCK_AFLAG_NEED_ACK_BIT);
  }

  CHECK_TS(ni, ts);

  if( ! (flags & CI_MSG_NO_POLL) )
    /* This is a good opportunity to keep things moving along. */
    ci_netif_poll(ni);

  LOG_TR(log(LNTS_FMT "ack_trigger=%x c/w rcv_delivered=%x "
             "rcv_added=%u buff=%u wnd_rhs=%x current=%u",
             LNTS_PRI_ARGS(ni, ts), ts->ack_trigger, ts->rcv_delivered,
             ts->rcv_added, tcp_rcv_buff(ts), tcp_rcv_wnd_right_edge_sent(ts),
             tcp_rcv_wnd_current(ts)));

  if( ts->s.b.state & CI_TCP_STATE_NOT_CONNECTED )  goto out;

  /* Free-up some receive buffers now we have the netif lock. */
  ci_tcp_rx_reap_rxq_bufs(ni, ts);

  /* RFC1122 silly window avoidance requires that we do not send window
  ** updates of less than an MSS.
  **
  ** The reason we're here is because we think the window should have grown
  ** sufficiently that an update is needed.  However, because the recv code
  ** is asynchronous, the window could have closed down again, so we do
  ** have to check we're not about to advertise a silly window.  We
  ** actually check that the right edge has moved by at least
  ** ci_tcp_ack_trigger_delta() since we last advertised a window.
  */
  if( ! ci_tcp_send_wnd_update(ni, ts) )
    /* Reset [ack_trigger] so it'll fire when we would advertise a window
    ** which is at least tcp_rcv_wnd_advertised() + delta.
    */
    ts->ack_trigger = ts->rcv_delivered
      + ci_tcp_ack_trigger_delta(ts)
      - SEQ_SUB(ts->rcv_delivered + tcp_rcv_buff(ts),
                tcp_rcv_wnd_right_edge_sent(ts));

 out:
  CHECK_TS(ni, ts);

  if (!(flags & CI_MSG_NETIF_LOCKED))
    ci_netif_unlock(ni);
}


/* Copy data from the receive queue to the app's buffer(s).  Returns the
** number of bytes copied.  This function also sends window updates as
** appropriate.
**
** User-level callers must hold the socket lock.  Other, trusted,
** stacks can get away without it as long as they avoid concurrent
** receives (currently assumes use of the netif lock).  Use the flags
** arg and the CI_MSG_*_LOCKED constants to specify which locks are
** already held.
*/
int ci_tcp_recvmsg_get(const ci_tcp_recvmsg_args* a, ci_iovec_ptr* piov)
{
  ci_netif* netif = a->ni;
  ci_tcp_state* ts = a->ts;
  int n, peek_off, total;
  ci_ip_pkt_fmt* pkt;
  int max_bytes;

  ci_assert(netif);
  ci_assert(ts);
  ci_assert(piov);

  /* The socket must be locked, unless the caller really is sure that that
  ** isn't necessary!. */
  ci_assert((a->flags & CI_MSG_SOCK_LOCK_NOT_NEEDED) ||
	    ci_sock_is_locked(netif, &ts->s.b));

  peek_off = 0;
  total = 0;

  /* Maximum number of bytes we have in both recv1 and recv2.
   * In this function, we get data from recv1 only, so the actual amount
   * of received data may be less than max_bytes. */
  max_bytes = tcp_rcv_usr(ts);

  if( max_bytes <= 0 || OO_PP_IS_NULL(ts->recv1_extract))
    return total;       /* Receive queue is empty. */

  pkt = PKT_CHK_NML(netif, ts->recv1_extract, a->flags & CI_MSG_NETIF_LOCKED);
  if( oo_offbuf_is_empty(&pkt->buf) ) {
    if( OO_PP_IS_NULL(pkt->next) )  return total;  /* recv1 is empty. */
    ts->recv1_extract = pkt->next;
    pkt = PKT_CHK_NML(netif, ts->recv1_extract, a->flags&CI_MSG_NETIF_LOCKED);
    ci_assert(oo_offbuf_not_empty(&pkt->buf));
  }

  while( 1 ) {
    PKT_TCP_RX_BUF_ASSERT_VALID(netif, pkt);
    ci_assert(oo_offbuf_not_empty(&pkt->buf));
    ci_assert(oo_offbuf_left(&pkt->buf) > peek_off);

#ifdef  __KERNEL__
    if( a->addr_spc != CI_ADDR_SPC_KERNEL )
      n = ci_ip_copy_pkt_to_user(netif, &piov->io, pkt, peek_off, a->addr_spc);
    else {
      n = oo_offbuf_left(&pkt->buf) - peek_off;
      n = CI_MIN(CI_IOVEC_LEN(&piov->io), (unsigned) n);
      memcpy(CI_IOVEC_BASE(&piov->io), oo_offbuf_ptr(&pkt->buf) + peek_off, n);
      ci_iovec_ptr_advance(piov, n);
      oo_offbuf_advance(&pkt->buf, n);
    }
    if( n < 0 )  return total;
#else
    n = ci_ip_copy_pkt_to_user(netif, &piov->io, pkt, peek_off, a->addr_spc);
#endif

    total += n;
    ci_assert(total <= max_bytes);

    if(CI_LIKELY( ! (a->flags & MSG_PEEK) )) {
      ci_assert(peek_off == 0);
      ts->rcv_delivered += n;
      if( oo_offbuf_left(&pkt->buf) == 0 ) {
        /* We've emptied the current packet. */
        if( CI_UNLIKELY(SEQ_LE(ts->ack_trigger, ts->rcv_delivered)) )
          ci_tcp_recvmsg_send_wnd_update(netif, ts, a->flags);
        if( total == max_bytes || OO_PP_IS_NULL(pkt->next) )
          /* We've emptied the receive queue. */
          return total;
        ts->recv1_extract = pkt->next;
        pkt = PKT_CHK_NML(netif, ts->recv1_extract,
                          a->flags & CI_MSG_NETIF_LOCKED);
        ci_assert(oo_offbuf_not_empty(&pkt->buf));
      }
    }
    else {
      /* copy did an implicit advance of the offbuf which we do not want */
      oo_offbuf_retard(&pkt->buf, n);

      peek_off += n;
      if( oo_offbuf_left(&pkt->buf) - peek_off == 0 ) {
        /* We've emptied the current packet. */
        if( total == max_bytes || OO_PP_IS_NULL(pkt->next) )
          /* We've emptied the receive queue. */
          return total;
        pkt = PKT_CHK_NML(netif, pkt->next, a->flags & CI_MSG_NETIF_LOCKED);
        peek_off = 0;
        ci_assert(oo_offbuf_not_empty(&pkt->buf));
      }
    }

    if( CI_IOVEC_LEN(&piov->io) == 0 ) {
      /* Exit here if we've filled the app's buffer. */
      if( piov->iovlen == 0 )  return total;
      piov->io = *(piov->iov)++;
      --piov->iovlen;
    }
    /* Yes, [piov->io.iov_len] could be zero here.  Just means we'll waste
    ** time going round the loop an extra time and not copy an data.  This
    ** is harmless.  Doing it this way makes the common case faster, and
    ** saves 3 characters.  Which I've just more than wasted in this
    ** comment; darn.
    */
  }
}


static int ci_tcp_recvmsg_spin(ci_netif* ni, ci_tcp_state* ts,
                               ci_uint64 start_frc)
{
  ci_uint64 now_frc;
  ci_uint64 schedule_frc = start_frc;
#ifndef __KERNEL__
  citp_signal_info* si = citp_signal_get_specific_inited();
#endif
  int rc = 0;
  ci_uint64 max_spin = ni->state->spin_cycles;
  int spin_limit_by_so = 0;

  if( ts->s.so.rcvtimeo_msec ) {
    ci_uint64 max_so_spin = (ci_uint64)ts->s.so.rcvtimeo_msec *
        IPTIMER_STATE(ni)->khz;
    if( max_so_spin <= max_spin ) {
      max_spin = max_so_spin;
      spin_limit_by_so = 1;
    }
  }

  now_frc = start_frc;

  do {
    if( ci_netif_may_poll(ni) ) {
      if( ci_netif_need_poll_spinning(ni, now_frc) ) {
        if( ci_netif_trylock(ni) ) {
          ci_netif_poll_n(ni, NI_OPTS(ni).evs_per_poll);
          ci_netif_unlock(ni);
        }
      }
      else if( ! ni->state->is_spinner )
        ni->state->is_spinner = 1;
    }
    if( tcp_rcv_usr(ts) || TCP_RX_DONE(ts) ) {
      rc = 1;
      break;
    }
    ci_frc64(&now_frc);
    rc = OO_SPINLOOP_PAUSE_CHECK_SIGNALS(ni, now_frc, schedule_frc, 
                                         ts->s.so.rcvtimeo_msec,
                                         &ts->s.b, si);
    if( rc != 0 )
      return rc;
  } while( now_frc - start_frc < max_spin );

  ni->state->is_spinner = 0;

  /* Check if we timed out */
  if( spin_limit_by_so && now_frc - start_frc >= max_spin )
    return -EAGAIN;
  return rc;
}


/* This macro returns true if the combination of [flags] and receive
** low-water-mark permit us to return given the amount of data we've
** received already.
**
** We can return if they've not asked to fill their buffer (no MSG_WAITALL)
** provided we've reached the low-water-mark, or if they've specified
** MSG_DONTWAIT or MSG_PEEK.  (On linux at least: MSG_PEEK cancels
** MSG_WAITALL, and MSG_DONTWAIT overrides MSG_WAITALL).
*/
#define FLAGS_AND_LOWAT_PERMIT_FAST_RET_WITH_DATA(ts, bytes, flags)     \
  ((flags & (MSG_DONTWAIT | MSG_PEEK)) ||                               \
   ((~flags & MSG_WAITALL) && (bytes) >= (ts)->s.so.rcvlowat))



int ci_tcp_recvmsg(const ci_tcp_recvmsg_args* a)
{
  int                   rc, have_polled;
  ci_uint64             sleep_seq;
  ci_iovec_ptr          piov;
  ci_tcp_state*         ts = a->ts;
  ci_netif*             ni = a->ni;
  int                   flags = a->flags;
  ci_uint64             start_frc = 0; /* suppress compiler warning */
  unsigned              tcp_recv_spin = 0;
  ci_uint32             timeout = ts->s.so.rcvtimeo_msec;

  ci_assert(a);
  ci_assert(ni);
  ci_assert(ts);
  ci_ss_assert(ni, ts->s.b.state != CI_TCP_LISTEN);
  ci_assert(a->msg);


  /* ?? TODO: MSG_TRUNC */

  /* Grab the per-socket lock so we can access the receive queue. */
  rc = ci_sock_lock(ni, &ts->s.b);
  if(CI_UNLIKELY( rc != 0 )) {
    CI_SET_ERROR(rc, -rc);
    return rc;
  }

  have_polled = 0;
  ci_assert_equal(rc, 0);

  if( (flags & MSG_OOB) |
      (a->msg->msg_iovlen == 0) | (a->msg->msg_iov == NULL) )
    goto slow_path;

  /* [piov] gives keeps track of our position in the apps buffer(s). */
  ci_iovec_ptr_init_nz(&piov, a->msg->msg_iov, a->msg->msg_iovlen);

  LOG_TR(log(LNTS_FMT "recvmsg len=%d flags=%x bytes_in_rxq=%d", 
	     LNTS_PRI_ARGS(ni, ts),
             ci_iovec_ptr_bytes_count(&piov), flags, tcp_rcv_usr(ts)));

#ifndef __KERNEL__
  tcp_recv_spin = 
    oo_per_thread_get()->spinstate & (1 << ONLOAD_SPIN_TCP_RECV);
#endif
  ci_frc64(&start_frc);

 poll_recv_queue:
  rc += ci_tcp_recvmsg_get(a, &piov);

  /* Return immediately if we've filled the app's buffer(s).  This also
  ** deals with the case that the app passed a zero-length buffer.
  */
  if( ci_iovec_ptr_is_empty_proper(&piov) ) {
    if( CI_UNLIKELY(rc == 0) )  goto check_errno;
    goto success_unlock_out;
  }

  if( ! have_polled ) {
    /* We've not yet filled the app's buffer.  But the receive queue may
    ** not be up-to-date, so we need to check that it is, or bring it
    ** up-to-date ourselves.
    */
    have_polled = 1;

    if( ci_netif_may_poll(ni) && ci_netif_need_poll_spinning(ni, start_frc) ) {
      if( ci_netif_trylock(ni) ) {
        ci_uint32 rcv_added_before = ts->rcv_added;
        int any_evs = ci_netif_poll_n(ni, NI_OPTS(ni).evs_per_poll);
        if( ts->rcv_added != rcv_added_before ) {
          /* We've handled some events, but possibly not all.  So if the
           * events we've handled do not satisfy the request, we need to
           * ensure we come back and poll some more.
           */
          have_polled = 0;
        }
        else if( any_evs )
          ci_netif_poll(ni);
	ci_netif_unlock(ni);
	if( ts->rcv_added != rcv_added_before ) {
	  if( (flags & MSG_PEEK) ) {
            ci_iovec_ptr_init_nz(&piov, a->msg->msg_iov, a->msg->msg_iovlen);
            rc = 0;
          }
	  goto poll_recv_queue;
	}
      }
      else {
        /* The netif lock is contended, so the chances are we're up-to-date.
        ** Even if we're not, at least we will be soon.  So we pretend we are
        ** up-to-date, and continue...
        */
      }
    }
  }

  /* We haven't filled the app's buffer, but recv2 might contain more data
  ** before the mark.
  */
  /* \todo For MSG_PEEK, we always will re-copy all data if we did not
   * filled user buffer. */
  if(CI_UNLIKELY( OO_PP_NOT_NULL(ts->recv2.head) ))
    if( ci_tcp_recvmsg_recv2(a, &piov, &rc) )
      goto success_unlock_out;

  /* We've done at least one ci_netif_poll(), so we're up-to-date.  But we
  ** haven't filled the app's buffer.
  */
  ci_assert(!ci_iovec_ptr_is_empty_proper(&piov));

  if( rc && FLAGS_AND_LOWAT_PERMIT_FAST_RET_WITH_DATA(ts, rc, flags) )
    goto success_unlock_out;

  if( TCP_RX_DONE(ts) )  goto rx_done;

  if( rc == 0 && (flags & MSG_DONTWAIT) ) {
    CI_SET_ERROR(rc, EAGAIN);
    goto unlock_out;
  }

  /* Must not delay return if we have any data and are peeking. */
  ci_assert(!(flags & MSG_PEEK) || rc == 0);


  /* Spin (if enabled) until timeout, or something happens, or we get
  ** contention on the netif lock.
  */
  if( tcp_recv_spin ) {
    int rc2;

    if( (rc2 = ci_tcp_recvmsg_spin(ni, ts, start_frc)) ) {
      if( rc2 < 0 ) {
        /* -ERESTARTSYS or -EAGAIN */
        CI_SET_ERROR(rc, -rc2);
        goto unlock_out;
      }
      goto poll_recv_queue;
    }

    tcp_recv_spin = 0;
    if( timeout ) {
      ci_uint32 spin_ms = NI_OPTS(ni).spin_usec >> 10;
      if( spin_ms < timeout )
        timeout -= spin_ms;
      else {
        CI_SET_ERROR(rc, EAGAIN);
        goto rx_done;
      }
    }
  }

  /* Time to block. */

  sleep_seq = ts->s.b.sleep_seq.all;
  ci_rmb();
  if( tcp_rcv_usr(ts) )  goto poll_recv_queue;
  if( TCP_RX_DONE(ts) )  goto rx_done;

  /* ?? TODO: lock recv queue so other thread can't get in in middle of our
  ** receive.  NB. Need to check what happens on Linux if one thread blocks
  ** in receive (w & w/o WAITALL) and another does concurrent non-blocking
  ** receive.
  */


  {
    int rc2;

    /* This function drops the socket lock, and returns unlocked. */
    rc2 = ci_sock_sleep(ni, &ts->s.b, CI_SB_FLAG_WAKE_RX,
                        CI_SLEEP_SOCK_LOCKED | CI_SLEEP_SOCK_RQ,
                        sleep_seq, &timeout);
    if( rc2 == 0 )
      rc2 = ci_sock_lock(ni, &ts->s.b);
    if( rc2 < 0 ) {
      /* If we've received anything at all, we must say how much. */
      if( rc )
        ci_tcp_recv_fill_msgname(ts, (struct sockaddr*) a->msg->msg_name,
                                 &a->msg->msg_namelen);
      else
        CI_SET_ERROR(rc, -rc2);
      goto out;
    }
  }
  ci_assert(have_polled);
  goto poll_recv_queue;


 slow_path:

  if( a->msg->msg_iovlen == 0 )
    goto success_unlock_out;

  if( a->msg->msg_iov == NULL ) {
    CI_SET_ERROR(rc, EFAULT);
    goto unlock_out;
  }

  ci_assert(flags & MSG_OOB);
  rc = ci_tcp_recvmsg_urg(ni, ts, a->msg, flags);

  if( rc >= 0 )  goto success_unlock_out;
  CI_SET_ERROR(rc, -rc);
  goto unlock_out;

 rx_done:
  if( tcp_rcv_usr(ts) && !ci_iovec_ptr_is_empty_proper(&piov) )
    /* Race breaker: rx_errno can get updated asynchronously just after
    ** we've looked at the receive queue.  We need to go back and get that
    ** data.
    */
    goto poll_recv_queue;
  if( rc )  goto success_unlock_out;
 check_errno:
  if (ts->s.so_error) {
    ci_int32 rc1 = ci_get_so_error(&ts->s);
    if (rc1 != 0)
      CI_SET_ERROR(rc, rc1);
  } else if( TCP_RX_ERRNO(ts) ) {
    CI_SET_ERROR(rc, TCP_RX_ERRNO(ts));
  }
  goto unlock_out;

 success_unlock_out:
  ci_tcp_recv_fill_msgname(ts, (struct sockaddr*) a->msg->msg_name,
                           &a->msg->msg_namelen);  /*!\TODO fixme remove cast*/
 unlock_out:
  ci_sock_unlock(ni, &ts->s.b);
 out:
  if(CI_UNLIKELY( ni->state->rxq_low ))
    ci_netif_rxq_low_on_recv(ni, &ts->s, rc);
  return rc;
}

static void move_from_recv2_to_recv1(ci_netif* ni, ci_tcp_state* ts,
                                     ci_ip_pkt_fmt* head,
                                     ci_ip_pkt_fmt* tail, int n)
{
  /* Move the [n] packets from [head] to [tail] inclusive from the
  ** beginning of [recv2] to [recv1].  If [recv2] is emptied, switch back
  ** to using [recv1].
  */
  ci_ip_pkt_queue* recv1 = &ts->recv1;
  ci_ip_pkt_queue* recv2 = &ts->recv2;

  ci_assert(ci_netif_is_locked(ni));
  ci_assert(ci_sock_is_locked(ni, &ts->s.b));
  ci_assert(n > 0);
  ci_assert(recv2->num >= n);
  ci_assert(OO_PP_EQ(recv2->head, OO_PKT_P(head)));
  ci_assert(n < recv2->num || OO_PP_IS_NULL(tail->next));

  if( n ) {
    LOG_URG(log(NTS_FMT "recvmsg: moving %d pkts from recv2 to recv1",
                NTS_PRI_ARGS(ni, ts), n));
    ci_ip_queue_move(ni, recv2, recv1, tail, n);
    /* The extract pointer can only be made -ve when the receive queues are
    ** emptied (and both locks are held).  It can only be -ve here if after
    ** the queue was empty the first packet that arrived contained urgent
    ** data.
    */
    if( OO_PP_IS_NULL(ts->recv1_extract) ) {
      ts->recv1_extract = recv1->head;
    }
    else {
      /*
       * must point to an emptied packet
       * - pull up to the first packet moved from recv2
       */
      ci_assert(oo_offbuf_is_empty(&(PKT_CHK(ni, ts->recv1_extract)->buf)));
      ts->recv1_extract = OO_PKT_P(head);
    }
    
  }

  /* If we've managed to empty recv2, we can switch back to recv1. */
  if( OO_PP_IS_NULL(recv2->head) ) {
    LOG_URG(log(NTS_FMT "recvmsg: switch to recv1", NTS_PRI_ARGS(ni, ts)));
    TS_QUEUE_RX_SET(ts, recv1);
    ci_assert(!(tcp_urg_data(ts) & CI_TCP_URG_PTR_VALID));
  }
}


static int ci_tcp_recvmsg_urg(ci_netif* ni, ci_tcp_state* ts,
                              struct msghdr* msg, int flags)
{
  ci_iovec_ptr piov;
  ci_uint8 oob;
  int can_write;
  int rc = 0;

  ci_netif_lock_id(ni, S_SP(ts));
  CHECK_TS(ni, ts);

  LOG_URG(ci_log(TCP_URG_FMT, TCP_URG_ARGS(ts)));

  ci_assert(msg->msg_iovlen > 0);
  ci_iovec_ptr_init_nz(&piov, msg->msg_iov, msg->msg_iovlen);
  can_write = !ci_iovec_ptr_is_empty_proper(&piov);

  if( ts->s.s_flags & CI_SOCK_FLAG_OOBINLINE ) {
    LOG_URG(ci_log("%s: OOBINLINE is set, rc=-EINVAL", __FUNCTION__));
    rc = -EINVAL;
    goto out;
  }

  /* unconditional poll - ensure up to date */
  ci_netif_poll(ni);

  if( tcp_urg_data(ts) & CI_TCP_URG_COMING ) {
    LOG_URG(log("%s: no OOB byte, rc=-EINVAL", __FUNCTION__));
    rc = -EAGAIN;
    goto out;
  }
  if( ~tcp_urg_data(ts) & CI_TCP_URG_IS_HERE ) {
    LOG_URG(ci_log("%s: OOB byte hasn't arrived, rc=-EAGAIN", __FUNCTION__));
    rc = -EINVAL;
    goto out;
  }

  if (ts->s.b.state == CI_TCP_CLOSED) {
    LOG_URG(ci_log("%s: tcp state is CLOSED, rc=0", __FUNCTION__));
    goto out;
  }



  /* at this point, we have an OOB byte */

  /* read the out-of-band byte */
  oob = tcp_urg_data(ts) & CI_TCP_URG_DATA_MASK;
  msg->msg_flags |= MSG_OOB;

  LOG_URG(ci_log("Reading OOB byte, oob=0x%X, flags=0x%X", oob, flags));

  /* if we are not in peek mode, mark the oob state as read */
  if (~flags & MSG_PEEK)
    tcp_urg_data(ts) &=~ (CI_TCP_URG_IS_HERE | CI_TCP_URG_DATA_MASK);

  /*! Linux appears to treat the MSG_TRUNC flag, in TCP, as a
   *  "PEEK and clear data" flag.
   *  \TODO: review this in the future */
  if( flags & CI_MSG_TRUNC ) {
    rc = can_write;
    goto out;
  }

  if( ! can_write ) {
    msg->msg_flags |= CI_MSG_TRUNC;
    rc = 0;
    goto out;
  }

  /* We passed all the checks, just copy the byte now.
  ** ci_iovec_ptr_is_empty_proper() above has moved us to a non-zero-length
  ** buffer, so we can just copy the byte here.
  */
  *(char*)CI_IOVEC_BASE(&piov.io) = oob;
  rc = 1;

 out:
  CHECK_TS(ni, ts);
  ci_netif_unlock(ni);
  return rc;
}


static void ci_tcp_recvmsg_recv2_peek2(const ci_tcp_recvmsg_args* a,
                                       ci_iovec_ptr* piov, int* bytes_so_far,
                                       int start_skip, int stop_at_mark,
                                       unsigned rd_nxt_seq)
{
  /* 
   * This function is used to peek at data on recv2.  Either to look a data
   ** before the mark, or at data after the OOB byte.
   * 
   * Windows: unlike normal reads, peeks will not read past any OOBB
   */
  ci_tcp_state* ts = a->ts;
  ci_netif* ni = a->ni;
  ci_ip_pkt_queue* recv2 = &ts->recv2;
  ci_ip_pkt_fmt* pkt = PKT_CHK(ni, recv2->head);
  oo_offbuf* buf = &pkt->buf;
  int n, peek_off = start_skip;
#ifdef __KERNEL__
  int rc;
#endif

  ci_assert(oo_offbuf_left(buf) >= start_skip);
  ci_assert(tcp_urg_data(ts) & CI_TCP_URG_PTR_VALID);
  ci_assert(!stop_at_mark || SEQ_LE(rd_nxt_seq, tcp_rcv_up(ts)));

  LOG_URG(log(LNTS_FMT "recv2_peek: so_far=%d skip=%d stop@mark=%d "
              "rd_nxt_seq=%08x rcv_up=%08x", LNTS_PRI_ARGS(ni, ts),
              *bytes_so_far, start_skip, stop_at_mark,
              rd_nxt_seq, tcp_rcv_up(ts)));

  rd_nxt_seq += start_skip;

  while( 1 ) {
    n = oo_offbuf_left(buf) - peek_off;
    n = CI_MIN(n, (int)CI_IOVEC_LEN(&piov->io));
    if( stop_at_mark ) {
      int dist_to_urg = tcp_rcv_up(ts) - rd_nxt_seq;
      if( dist_to_urg == 0 )  
	{
	  ci_log("dist_to_urg == 0");
	  break;
	}
      n = CI_MIN(n, dist_to_urg);
    }

#ifdef __KERNEL__
    rc = copy_to_user(CI_IOVEC_BASE(&piov->io), oo_offbuf_ptr(buf) + peek_off, n);
    ci_assert(rc == 0);
#else
    memcpy(CI_IOVEC_BASE(&piov->io), oo_offbuf_ptr(buf) + peek_off, n);
#endif
    *bytes_so_far += n;
    ci_iovec_ptr_advance(piov, n);
    peek_off += n;
    rd_nxt_seq += n;

    if( CI_IOVEC_LEN(&piov->io) == 0 ) {
      if( piov->iovlen == 0 ) 
	break;
      
      piov->io = *(piov->iov)++;
      --piov->iovlen;
    }
    if( oo_offbuf_left(buf) - peek_off == 0 ) {
      if( OO_PP_IS_NULL(pkt->next) ) 
	break;
      pkt = PKT_CHK(ni, pkt->next);
      buf = &pkt->buf;
      peek_off = 0;
    }
  }
}


static int ci_tcp_recvmsg_recv2_peek(const ci_tcp_recvmsg_args* a,
                                     ci_iovec_ptr* piov, int* bytes_so_far)
{
  ci_tcp_state* ts = a->ts;
  ci_netif* ni = a->ni;
  ci_ip_pkt_queue* recv2 = &ts->recv2;
  ci_ip_pkt_fmt* pkt;
  int skip, stop_at_mark;
  unsigned rd_nxt_seq;

  ci_netif_lock_id(ni, S_SP(ts));

  pkt = PKT_CHK(ni, recv2->head);
  rd_nxt_seq = PKT_RX_BUF_SEQ(pkt);

  /* Double-check for packets added to recv1 after we finished sucking data
  ** from it.
  */
  if( OO_PP_NOT_NULL(ts->recv1_extract) ) {
    ci_ip_pkt_fmt* r1pkt = PKT_CHK(ni, ts->recv1_extract);
    unsigned seq = PKT_RX_BUF_SEQ(r1pkt) + *bytes_so_far;
    /* We think we've read everything in recv1, and [seq] points just
    ** beyond that.  So it ought to match the beginning of recv2.  If it
    ** doesn't, then something else has been added to recv1.
    */
    if( seq != CI_BSWAP_BE32(PKT_TCP_HDR(pkt)->tcp_seq_be32) )
      /* Ooops...more data appended to recv1.  But it arrived after we
      ** started reading, so we can legitimately return without reading
      ** this data.  If we've not read anything yet, we can safely return
      ** to recvmsg() which will try recv1 again.
      */
      goto out;
  }

  /* If we're at the mark, peek the OOB byte (if inline) and data following
  ** it.  Otherwise peek the data up to the mark.
  */
  if( tcp_rcv_up(ts) == rd_nxt_seq ) {
    skip = !(ts->s.s_flags & CI_SOCK_FLAG_OOBINLINE);
    stop_at_mark = 0;
  }
  else {
    skip = 0;
    stop_at_mark = 1;
  }
  ci_tcp_recvmsg_recv2_peek2(a, piov, bytes_so_far, skip,
                             stop_at_mark, rd_nxt_seq);

 out:
  ci_netif_unlock(ni);
  return *bytes_so_far;
}


static int ci_tcp_recvmsg_handle_race(const ci_tcp_recvmsg_args* a,
                                      ci_iovec_ptr* piov, int* bytes_so_far)
{
  /* One or more packets were added to recv1 after we finished looking at
  ** it, but before we looked at recv2.  So we need to go and pick up that
  ** data.
  */
  ci_netif_unlock(a->ni);
  *bytes_so_far += ci_tcp_recvmsg_get(a, piov);
  ci_netif_lock_id(a->ni, S_SP(a->ts));
  /* NB. No more data can have arrived in recv1, because once we start
  ** using recv2 we stick with it until the consumer switches back to
  ** recv1.  Which we haven't.
  */
  return ci_iovec_ptr_is_empty_proper(piov);
}


ci_inline int ci_tcp_recv1_is_empty(ci_netif* ni, ci_tcp_state* ts)
{
  /* NB. The first buffer pointed to by the extract pointer may be empty,
  ** but any subsequent ones must not be.
  */
  ci_ip_pkt_fmt *pkt;
  if( OO_PP_IS_NULL(ts->recv1_extract) )  return 1;
  pkt = PKT_CHK_NNL(ni, ts->recv1_extract);
  return oo_offbuf_is_empty(&pkt->buf) && OO_PP_IS_NULL(pkt->next);
}


static int ci_tcp_recvmsg_recv2(const ci_tcp_recvmsg_args* a,
                                ci_iovec_ptr* piov, int* bytes_so_far)
{
  ci_tcp_state* ts = a->ts;
  ci_netif* ni = a->ni;
  ci_ip_pkt_queue* recv2 = &ts->recv2;
  ci_ip_pkt_fmt* pkt, *head_pkt, *tail_pkt;
  oo_offbuf* buf;
  unsigned rd_nxt_seq, n;
  int must_return_from_recv = 0;

  if( a->flags & MSG_PEEK )
    return ci_tcp_recvmsg_recv2_peek(a, piov, bytes_so_far);

 again:
  LOG_URG(ci_log("%s: again bytes_so_far=%d", 
		 __FUNCTION__, *bytes_so_far));
  
  ci_assert(ci_sock_is_locked(ni, &ts->s.b));
  ci_netif_lock_id(ni, S_SP(ts));
  CHECK_TS(ni, ts);


  /* Double-check for packets added to recv1. */
  if( ! ci_tcp_recv1_is_empty(ni, ts) ) {
    must_return_from_recv = ci_tcp_recvmsg_handle_race(a, piov, bytes_so_far);
    if( must_return_from_recv )  goto unlock_out;
  }

  ci_assert(ci_tcp_recv1_is_empty(ni, ts));

  pkt = PKT_CHK(ni, recv2->head);
  buf = &pkt->buf;
  ci_assert(oo_offbuf_left(buf));

  /* Calculate the sequence number of the first un-read byte in this pkt. */
  rd_nxt_seq = PKT_RX_BUF_SEQ(pkt);

  LOG_URG(log("%s: "NTS_FMT "so_far=%d flags=%x nxt_seq=%08x rcv_up=%08x "
              "urg_data=%03x", __FUNCTION__, NTS_PRI_ARGS(ni, ts),
              *bytes_so_far, a->flags, rd_nxt_seq, tcp_rcv_up(ts),
              tcp_urg_data(ts)));

  ci_assert(tcp_urg_data(ts) & CI_TCP_URG_PTR_VALID);

  if( tcp_rcv_up(ts) == rd_nxt_seq ) {
    /* We are staring at the urgent byte. */
    LOG_URG(ci_log("%s: We're staring at the oob byte and bytes_so_far=%d",
              __FUNCTION__, *bytes_so_far));

    /*
     * windows allows in-band reads to pass the mark - so don't quit here
     */
    if( *bytes_so_far ) {
      /* We've consumed some data, so stop at the mark. */
      LOG_URG(ci_log("%s: We're staring at the oob byte and bytes_so_far=%d",
              __FUNCTION__, *bytes_so_far));
      must_return_from_recv = 1;
      goto unlock_out;
    }
    

    if( ! (ts->s.s_flags & CI_SOCK_FLAG_OOBINLINE) ) {
      /* App is trying to read past the urgent data.  In this case the
      ** urgent data just disappears (just as if it had never been there).
      */
      oo_offbuf_advance(buf, 1);
      ++ts->rcv_delivered;
    }
    /* Now we can move everything onto recv1 and look and recv1.  This
    ** packet might be empty, but recv1 is permitted to have an empty
    ** packet at the start, so we don't have to worry about it.
    */
    /*
     * windows allows MSG_OOB read after in-band reads have passed the mark
     * - so leave as valid
     */
    tcp_urg_data_invalidate(ts);
    move_from_recv2_to_recv1(ni, ts, pkt, PKT_CHK(ni,recv2->tail), recv2->num);
    ci_assert(OO_PP_IS_NULL(recv2->head));
    ci_assert(TS_QUEUE_RX(ts) == &ts->recv1);
    ci_netif_unlock(ni);
    *bytes_so_far += ci_tcp_recvmsg_get(a, piov);
    goto out;
  }

  /* There is some normal data before the urgent data.  Look for any whole
  ** packets that come before the mark.
  */
  head_pkt = pkt;
  n = 0;
  tail_pkt = 0; /* just to suppress compiler warning */
  while( tcp_rcv_up(ts) >= pkt->pf.tcp_rx.end_seq ) {
    tail_pkt = pkt;
    ++n;
    if( OO_PP_IS_NULL(pkt->next) )  break;
    pkt = PKT_CHK(ni, pkt->next);
  }
  if( n ) {
    /* We've got [n] whole packets before the mark.  (This happens when
    ** more urgent data arrives before we've gone past the mark).  We move
    ** them onto recv1.
    */
    move_from_recv2_to_recv1(ni, ts, head_pkt, tail_pkt, n);
    CHECK_TS(ni, ts);
    ci_netif_unlock(ni);
    /* Pull data out of recv1 and return if we fill app's buffer. */
    *bytes_so_far += ci_tcp_recvmsg_get(a, piov);
    must_return_from_recv = ci_iovec_ptr_is_empty_proper(piov);
    if( must_return_from_recv )  goto out;
    /* May need to pull some more from recv2 before the mark.  NB. Can't
    ** just fall through to the code below, because the mark may have moved
    ** forward because we dropped the netif lock.
    */
    if( OO_PP_NOT_NULL(recv2->head) )  goto again;
    goto out;
  }
  else {
    /* The packet at the head of recv2 (if any) contains normal data
    ** followed by urgent data.  So read the normal data.
    */
    int n;
    if( OO_PP_IS_NULL(recv2->head) )  goto unlock_out;
    n = tcp_rcv_up(ts) - rd_nxt_seq;    /* number of normal bytes */
    LOG_URG(ci_log("%s: reading %d bytes from urg segment before OOBB",
		   __FUNCTION__, n));
    ci_assert(n > 0);
    ci_assert_lt(n, oo_offbuf_left(buf));
    n = ci_copy_to_iovec(piov, oo_offbuf_ptr(buf), n);
    *bytes_so_far += n;
    oo_offbuf_advance(buf, n);
    ts->rcv_delivered += n;
    ci_assert(oo_offbuf_left(buf));
    /* We've either filled the app buffer, or read up to the mark, so
    ** recvmsg() can return now.
    */
    must_return_from_recv = 1;

  }

 unlock_out:
  CHECK_TS(ni, ts);
  ci_netif_unlock(ni);
 out:
  /* Must return if we've filled the app buffer. */
  must_return_from_recv |= ci_iovec_ptr_is_empty_proper(piov);

  LOG_URG(ci_log("%s: returning %d bytes_so_far=%d "
		 "ci_iovec_ptr_is_empty_proper=%d",
		 __FUNCTION__, must_return_from_recv,
		 *bytes_so_far,
		 ci_iovec_ptr_is_empty_proper(piov)));
  
  return must_return_from_recv;
}


/*! \cidoxg_end */
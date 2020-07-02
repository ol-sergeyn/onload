/* SPDX-License-Identifier: LGPL-2.1 */
/* X-SPDX-Copyright-Text: (c) Solarflare Communications Inc */

#include "ef_vi_internal.h"

#if CI_HAVE_AF_XDP

#include <ci/efhw/common.h>
#include <linux/if_xdp.h>
#include "logging.h"

/* Currently, AF_XDP requires a system call to start transmitting.
 *
 * There is a limit (undocumented, so we can't rely on it being 16) to the
 * number of packets which will be sent each time. We use the "previous"
 * field to store the last packet known to be sent; if this does not cover
 * all those in the queue, we will try again once a send has completed.
 */
static int efxdp_tx_need_kick(ef_vi* vi)
{
  ef_vi_txq_state* qs = &vi->ep_state->txq;
  return qs->previous != qs->added;
}

static void efxdp_tx_kick(ef_vi* vi)
{
  if( vi->xdp_kick(vi) == 0 ) {
    ef_vi_txq_state* qs = &vi->ep_state->txq;
    qs->previous = qs->added;
  }
}

static int efxdp_ef_vi_transmitv_init(ef_vi* vi, const ef_iovec* iov,
                                      int iov_len, ef_request_id dma_id)
{
  ef_vi_txq* q = &vi->vi_txq;
  ef_vi_txq_state* qs = &vi->ep_state->txq;
  struct xdp_desc* dq = vi->xdp_tx.desc;
  uint32_t* iq = q->ids;
  int i;

  if( iov_len != 1 )
    return -EINVAL; /* Multiple buffers per packet not supported */

  if( qs->added - qs->removed >= q->mask )
    return -EAGAIN;

  i = qs->added++ & q->mask;
  EF_VI_BUG_ON(iq[i] != EF_REQUEST_ID_MASK);
  iq[i] = dma_id;
  dq[i].addr = iov->iov_base;
  dq[i].len = iov->iov_len;
  return 0;
}

static void efxdp_ef_vi_transmit_push(ef_vi* vi)
{
  *vi->xdp_tx.producer = vi->ep_state->txq.added;
  efxdp_tx_kick(vi);
}

static int efxdp_ef_vi_transmit(ef_vi* vi, ef_addr base, int len,
                                ef_request_id dma_id)
{
  ef_iovec iov = { base, len };
  int rc = efxdp_ef_vi_transmitv_init(vi, &iov, 1, dma_id);
  if( rc == 0 ) {
    wmb();
    efxdp_ef_vi_transmit_push(vi);
  }
  return rc;
}

static int efxdp_ef_vi_transmitv(ef_vi* vi, const ef_iovec* iov, int iov_len,
                                 ef_request_id dma_id)
{
  int rc = efxdp_ef_vi_transmitv_init(vi, iov, iov_len, dma_id);
  if( rc == 0 ) {
    wmb();
    efxdp_ef_vi_transmit_push(vi);
  }
  return rc;
}

static int efxdp_ef_vi_transmit_pio(ef_vi* vi, int offset, int len,
                                    ef_request_id dma_id)
{
  return -EOPNOTSUPP;
}

static int efxdp_ef_vi_transmit_copy_pio(ef_vi* vi, int offset,
                                         const void* src_buf, int len,
                                         ef_request_id dma_id)
{
  return -EOPNOTSUPP;
}

static void efxdp_ef_vi_transmit_pio_warm(ef_vi* vi)
{
  /* PIO is unsupported so do nothing */
}

static void efxdp_ef_vi_transmit_copy_pio_warm(ef_vi* vi, int pio_offset,
                                               const void* src_buf, int len)
{
  /* PIO is unsupported so do nothing */
}

static void efxdp_ef_vi_transmitv_ctpio(ef_vi* vi, size_t frame_len,
                                        const struct iovec* iov, int iovcnt,
                                        unsigned threshold)
{
  /* CTPIO is unsupported so do nothing. Fallback will send the packet. */
}

static void efxdp_ef_vi_transmitv_ctpio_copy(ef_vi* vi, size_t frame_len,
                                             const struct iovec* iov, int iovcnt,
                                             unsigned threshold, void* fallback)
{
  // TODO copy to fallback
}

static int efxdp_ef_vi_transmit_alt_select(ef_vi* vi, unsigned alt_id)
{
  return -EOPNOTSUPP;
}

static int efxdp_ef_vi_transmit_alt_select_normal(ef_vi* vi)
{
  return -EOPNOTSUPP;
}

static int efxdp_ef_vi_transmit_alt_stop(ef_vi* vi, unsigned alt_id)
{
  return -EOPNOTSUPP;
}

static int efxdp_ef_vi_transmit_alt_discard(ef_vi* vi, unsigned alt_id)
{
  return -EOPNOTSUPP;
}

static int efxdp_ef_vi_transmit_alt_go(ef_vi* vi, unsigned alt_id)
{
  return -EOPNOTSUPP;
}

static int efxdp_ef_vi_receive_init(ef_vi* vi, ef_addr addr,
                                    ef_request_id dma_id)
{
  ef_vi_rxq* q = &vi->vi_rxq;
  ef_vi_rxq_state* qs = &vi->ep_state->rxq;
  uint64_t* dq = vi->xdp_fr.desc;
  int i;

  if( qs->added - qs->removed >= q->mask )
    return -EAGAIN;

  i = qs->added++ & q->mask;
  EF_VI_BUG_ON(q->ids[i] != EF_REQUEST_ID_MASK);
  q->ids[i] = dma_id;
  dq[i] = addr;
  return 0;
}

static void efxdp_ef_vi_receive_push(ef_vi* vi)
{
  wmb();
  *vi->xdp_fr.producer = vi->ep_state->rxq.added;
}

static void efxdp_ef_eventq_prime(ef_vi* vi)
{
  // TODO
}

static int efxdp_ef_eventq_poll(ef_vi* vi, ef_event* evs, int evs_len)
{
  int n = 0;

  /* rx_buffer_len is power of two */
  EF_VI_ASSERT(((vi->rx_buffer_len -1) & vi->rx_buffer_len) == 0);

  /* Check rx ring, which won't exist on tx-only interfaces */
  if( n < evs_len && ef_vi_receive_capacity(vi) != 0 ) {
    struct ef_vi_xdp_ring* ring = &vi->xdp_rx;
    uint32_t cons = *ring->consumer;
    uint32_t prod = *ring->producer;

    if( cons != prod ) {
      ef_vi_rxq* q = &vi->vi_rxq;
      ef_vi_rxq_state* qs = &vi->ep_state->rxq;
      struct xdp_desc* dq = ring->desc;

      do {
        unsigned desc_i = qs->removed++ & q->mask;

        evs[n].rx.type = EF_EVENT_TYPE_RX;
        evs[n].rx.q_id = 0;
        evs[n].rx.rq_id = q->ids[desc_i];
        q->ids[desc_i] = EF_REQUEST_ID_MASK;  /* Debug only? */

        /* FIXME: handle jumbo, multicast */
        evs[n].rx.flags = EF_EVENT_FLAG_SOP;
        /* In case of AF_XDP offset of the placement of payload from
         * the beginning of the packet buffer may vary. */
        evs[n].rx.ofs = dq[desc_i].addr & (vi->rx_buffer_len -1 );
        evs[n].rx.len = dq[desc_i].len;

        ++n;
        ++cons;
      } while( cons != prod && n != evs_len );

      /* Full memory barrier needed to ensure the descriptors aren't overwritten
       * by incoming packets before the read accesses above */
      ci_mb();
      *ring->consumer = cons;
    }
  }

  /* Check tx completion ring */
  if( n < evs_len ) {
    struct ef_vi_xdp_ring* ring = &vi->xdp_cr;
    uint32_t cons = *ring->consumer;
    uint32_t prod = *ring->producer;

    if( cons != prod ) {
      do {
        if( prod - cons <= EF_VI_TRANSMIT_BATCH )
          cons = prod;
        else
          cons += EF_VI_TRANSMIT_BATCH;

        evs[n].tx.type = EF_EVENT_TYPE_TX;
        evs[n].tx.desc_id = cons;
        evs[n].tx.flags = 0;
        evs[n].tx.q_id = 0;
        ++n;
      } while( cons != prod && n != evs_len );

      /* No memory barrier needed as we aren't accessing the descriptor data.
       * We just recorded the value of 'cons` for later use to access `q->ids`
       * from `ef_vi_transmit_unbundle`. */
      *ring->consumer = cons;

      if( efxdp_tx_need_kick(vi) )
        efxdp_tx_kick(vi);
    }
  }

  return n;
}

static void efxdp_ef_eventq_timer_prime(ef_vi* vi, unsigned v)
{
  // TODO
}

static void efxdp_ef_eventq_timer_run(ef_vi* vi, unsigned v)
{
  // TODO
}

static void efxdp_ef_eventq_timer_clear(ef_vi* vi)
{
  // TODO
}

static void efxdp_ef_eventq_timer_zero(ef_vi* vi)
{
  // TODO
}

void efxdp_vi_init(ef_vi* vi)
{
  vi->ops.transmit               = efxdp_ef_vi_transmit;
  vi->ops.transmitv              = efxdp_ef_vi_transmitv;
  vi->ops.transmitv_init         = efxdp_ef_vi_transmitv_init;
  vi->ops.transmit_push          = efxdp_ef_vi_transmit_push;
  vi->ops.transmit_pio           = efxdp_ef_vi_transmit_pio;
  vi->ops.transmit_copy_pio      = efxdp_ef_vi_transmit_copy_pio;
  vi->ops.transmit_pio_warm      = efxdp_ef_vi_transmit_pio_warm;
  vi->ops.transmit_copy_pio_warm = efxdp_ef_vi_transmit_copy_pio_warm;
  vi->ops.transmitv_ctpio        = efxdp_ef_vi_transmitv_ctpio;
  vi->ops.transmitv_ctpio_copy   = efxdp_ef_vi_transmitv_ctpio_copy;
  vi->ops.transmit_alt_select    = efxdp_ef_vi_transmit_alt_select;
  vi->ops.transmit_alt_select_default = efxdp_ef_vi_transmit_alt_select_normal;
  vi->ops.transmit_alt_stop      = efxdp_ef_vi_transmit_alt_stop;
  vi->ops.transmit_alt_go        = efxdp_ef_vi_transmit_alt_go;
  vi->ops.transmit_alt_discard   = efxdp_ef_vi_transmit_alt_discard;
  vi->ops.receive_init           = efxdp_ef_vi_receive_init;
  vi->ops.receive_push           = efxdp_ef_vi_receive_push;
  vi->ops.eventq_poll            = efxdp_ef_eventq_poll;
  vi->ops.eventq_prime           = efxdp_ef_eventq_prime;
  vi->ops.eventq_timer_prime     = efxdp_ef_eventq_timer_prime;
  vi->ops.eventq_timer_run       = efxdp_ef_eventq_timer_run;
  vi->ops.eventq_timer_clear     = efxdp_ef_eventq_timer_clear;
  vi->ops.eventq_timer_zero      = efxdp_ef_eventq_timer_zero;

  vi->rx_buffer_len = 2048;
  vi->rx_prefix_len = 0;
}
#else
void efxdp_vi_init(ef_vi* vi)
{
}
#endif

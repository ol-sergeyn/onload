/* SPDX-License-Identifier: BSD-2-Clause */
/* X-SPDX-Copyright-Text: (c) Copyright 2021 Xilinx, Inc. */

#include "ef_vi_internal.h"
#ifndef __KERNEL__
#include "driver_access.h"
#endif
#include <etherfabric/internal/efct_uk_api.h>

/* FIXME EFCT: make this variable */
#define EFCT_PKT_STRIDE 2048

#include <stdbool.h>

struct efct_rx_descriptor
{
  uint16_t refcnt;
};

/* pkt_ids are:
 *  bits 0..15 packet index in superbuf
 *  bits 16..25 superbuf index
 *  bits 26..28 rxq (as an index in to vi->efct_rxq, not as a hardware ID)
 *  bits 29..31 unused/zero
 *  [NB: bit 31 is stolen by some users to cache the superbuf's sentinel]
 * This layout is not part of the stable ABI. rxq index is slammed up against
 * superbuf index to allow for dirty tricks where we mmap all superbufs in
 * contiguous virtual address space and thus avoid some arithmetic.
 */

#define PKTS_PER_SUPERBUF_BITS 16

static int pkt_id_to_index_in_superbuf(uint32_t pkt_id)
{
  return pkt_id & ((1u << PKTS_PER_SUPERBUF_BITS) - 1);
}

static int pkt_id_to_global_superbuf_ix(uint32_t pkt_id)
{
  return pkt_id >> PKTS_PER_SUPERBUF_BITS;
}

static int pkt_id_to_local_superbuf_ix(uint32_t pkt_id)
{
  return pkt_id_to_global_superbuf_ix(pkt_id) & (CI_EFCT_MAX_SUPERBUFS - 1);
}

static int pkt_id_to_rxq_ix(uint32_t pkt_id)
{
  return pkt_id_to_global_superbuf_ix(pkt_id) / CI_EFCT_MAX_SUPERBUFS;
}

ef_vi_noinline
static int superbuf_config_refresh(ef_vi* vi, ef_vi_efct_rxq* rxq)
{
#ifdef __KERNEL__
  /* TODO EFCT */
  return -ENOSYS;
#else
  ci_resource_op_t op;
  rxq->config_generation = rxq->shm->config_generation;
  op.op = CI_RSOP_RXQ_REFRESH;
  op.id = efch_make_resource_id(rxq->resource_id);
  op.u.rxq_refresh.superbufs = (uintptr_t)rxq->superbuf;
  op.u.rxq_refresh.current_mappings = (uintptr_t)rxq->current_mappings;
  op.u.rxq_refresh.max_superbufs = CI_EFCT_MAX_SUPERBUFS;
  return ci_resource_op(vi->dh, &op);
#endif
}

static int superbuf_next(ef_vi* vi, ef_vi_efct_rxq* rxq)
{
  struct efab_efct_rxq_uk_shm* shm = rxq->shm;
  uint64_t added_full;
  uint32_t added, removed;
  int sbid;

  /* TODO: track the global superbuf sequence number */
  added_full = OO_ACCESS_ONCE(shm->rxq.added);
  added = (uint32_t)added_full;
  removed = shm->rxq.removed;
  if( added == removed )
    return -EAGAIN;
  ci_rmb();
  sbid = OO_ACCESS_ONCE(shm->rxq.q[removed & (CI_ARRAY_SIZE(shm->rxq.q) - 1)]);
  EF_VI_ASSERT((sbid & CI_EFCT_Q_SUPERBUF_ID_MASK) < CI_EFCT_MAX_SUPERBUFS);
  OO_ACCESS_ONCE(shm->rxq.removed) = removed + 1;
  return sbid;
}

static void superbuf_free(ef_vi* vi, ef_vi_efct_rxq* rxq, int sbid)
{
  struct efab_efct_rxq_uk_shm* shm = rxq->shm;
  uint32_t added, removed;

  added = shm->freeq.added;
  removed = OO_ACCESS_ONCE(shm->freeq.removed);
  /* TODO: need to make this smarter and/or have a much bigger freeq if we
   * allow apps to hold on to superbufs for longer */
  (void)removed;
  EF_VI_ASSERT(added - removed < CI_ARRAY_SIZE(shm->freeq.q));
  shm->freeq.q[added & (CI_ARRAY_SIZE(shm->freeq.q) - 1)] = sbid;
  ci_wmb();
  OO_ACCESS_ONCE(shm->freeq.added) = added + 1;
}

static bool efct_rxq_is_active(const ef_vi* vi, int qid)
{
  return vi->efct_rxq[qid].superbuf_pkts != 0;
}

/* The superbuf descriptor for this packet */
static struct efct_rx_descriptor* efct_rx_desc(ef_vi* vi, uint32_t pkt_id)
{
  ef_vi_rxq* q = &vi->vi_rxq;
  struct efct_rx_descriptor* desc = q->descriptors;
  return desc + pkt_id_to_global_superbuf_ix(pkt_id);
}

/* The header preceding this packet */
static const ci_qword_t* efct_rx_header(const ef_vi* vi, size_t pkt_id)
{
  /* Sneakily rely on vi->efct_rxq[i].superbuf being contiguous.
   * TODO EFCT: kernelspace won't be able to be this cunning */
  return (const ci_qword_t*)
         (vi->efct_rxq[0].superbuf +
          pkt_id_to_global_superbuf_ix(pkt_id) * EFCT_RX_SUPERBUF_BYTES +
          pkt_id_to_index_in_superbuf(pkt_id) * EFCT_PKT_STRIDE);
}

static uint32_t rxq_ptr_to_pkt_id(uint32_t ptr)
{
  /* Masking off the sentinel */
  return ptr & 0x7fffffff;
}

/* The header following the next packet, or null if not available */
static const ci_qword_t* efct_rx_next_header(const ef_vi* vi, int qid)
{
  const ef_vi_rxq_state* qs = &vi->ep_state->rxq;
  uint32_t next = qs->rxq_ptr[qid].next;
  const ci_qword_t* header = efct_rx_header(vi, rxq_ptr_to_pkt_id(next));

  int expect_phase = next >> 31;
  int actual_phase = CI_QWORD_FIELD(*header, EFCT_RX_HEADER_SENTINEL);

  return expect_phase == actual_phase ? header : NULL;
}

/* Check whether a received packet is available */
static bool efct_rx_check_event(const ef_vi* vi)
{
  int i;
  if( ! vi->vi_rxq.mask )
    return false;
  if( vi->vi_flags & EF_VI_EFCT_UNIQUEUE )
    return efct_rxq_is_active(vi, 0) && efct_rx_next_header(vi, 0) != NULL;
  for( i = 0; i < EF_VI_MAX_EFCT_RXQS; ++i )
    if( efct_rxq_is_active(vi, i) && efct_rx_next_header(vi, i) != NULL )
      return true;
  return false;
}

/* tx packet descriptor, stored in the ring until completion */
/* TODO fix the size of this, and update tx_desc_bytes in vi_init.c */
struct efct_tx_descriptor
{
  /* total length including header and padding, in bytes */
  uint16_t len;
};

/* state of a partially-completed tx operation */
struct efct_tx_state
{
  /* next write location within the aperture. NOTE: we assume the aperture is
   * mapped twice, so that each packet can be written contiguously */
  volatile uint64_t* aperture;
  /* up to 7 bytes left over after writing a block in 64-bit chunks */
  union {uint64_t word; uint8_t bytes[8];} tail;
  /* number of left over bytes in 'tail' */
  unsigned tail_len;
};

/* generic tx header */
static uint64_t efct_tx_header(unsigned packet_length, unsigned ct_thresh,
                               unsigned timestamp_flag, unsigned warm_flag,
                               unsigned action)
{
  ci_qword_t qword;

  RANGECHCK(packet_length, EFCT_TX_HEADER_PACKET_LENGTH_WIDTH);
  RANGECHCK(ct_thresh, EFCT_TX_HEADER_CT_THRESH_WIDTH);
  RANGECHCK(timestamp_flag, EFCT_TX_HEADER_TIMESTAMP_FLAG_WIDTH);
  RANGECHCK(warm_flag, EFCT_TX_HEADER_WARM_FLAG_WIDTH);
  RANGECHCK(action, EFCT_TX_HEADER_ACTION_WIDTH);

  CI_POPULATE_QWORD_5(qword,
      EFCT_TX_HEADER_PACKET_LENGTH, packet_length,
      EFCT_TX_HEADER_CT_THRESH, ct_thresh,
      EFCT_TX_HEADER_TIMESTAMP_FLAG, timestamp_flag,
      EFCT_TX_HEADER_WARM_FLAG, warm_flag,
      EFCT_TX_HEADER_ACTION, action);

  return qword.u64[0];
}

/* tx header for standard (non-templated) send */
static uint64_t efct_tx_pkt_header(unsigned length, unsigned ct_thresh,
                                   unsigned timestamp_flag)
{
  return efct_tx_header(length, ct_thresh, timestamp_flag, 0, 0);
}

/* check that we have space to send a packet of this length */
static bool efct_tx_check(ef_vi* vi, int len)
{
  /* We require the txq to be large enough for the maximum number of packets
   * which can be written to the FIFO. Each packet consumes at least 64 bytes.
   */
  BUG_ON((vi->vi_txq.mask + 1) <
         (vi->vi_txq.ct_fifo_bytes + EFCT_TX_HEADER_BYTES) / EFCT_TX_ALIGNMENT);

  return ef_vi_transmit_space_bytes(vi) >= len;
}

/* initialise state for a transmit operation */
static void efct_tx_init(ef_vi* vi, struct efct_tx_state* tx)
{
  unsigned offset = vi->ep_state->txq.ct_added % EFCT_TX_APERTURE;

  BUG_ON(offset % EFCT_TX_ALIGNMENT != 0);
  tx->aperture = (void*)(vi->vi_ctpio_mmap_ptr + offset);
  tx->tail.word = 0;
  tx->tail_len = 0;
}

/* store a left-over byte from the start or end of a block */
static void efct_tx_tail_byte(struct efct_tx_state* tx, uint8_t byte)
{
  BUG_ON(tx->tail_len >= 8);
  tx->tail.bytes[tx->tail_len++] = byte;
}

/* write a 64-bit word to the CTPIO aperture */
static void efct_tx_word(struct efct_tx_state* tx, uint64_t value)
{
  *tx->aperture++ = value;
}

/* write a block of bytes to the CTPIO aperture, dealing with leftovers */
static void efct_tx_block(struct efct_tx_state* tx, char* base, int len)
{
  if( tx->tail_len != 0 ) {
    while( len > 0 && tx->tail_len < 8 ) {
      efct_tx_tail_byte(tx, *base);
      base++;
      len--;
    }

    if( tx->tail_len == 8 ) {
      efct_tx_word(tx, tx->tail.word);
      tx->tail.word = 0;
      tx->tail_len = 0;
    }
  }

  while( len >= 8 ) {
    efct_tx_word(tx, *(uint64_t*)base);
    base += 8;
    len -= 8;
  }

  while( len > 0 ) {
    efct_tx_tail_byte(tx, *base);
    base++;
    len--;
  }
}

/* complete a tx operation, writing leftover bytes and padding as needed */
static void efct_tx_complete(ef_vi* vi, struct efct_tx_state* tx, uint32_t dma_id)
{
  unsigned start, end, len;

  ef_vi_txq* q = &vi->vi_txq;
  ef_vi_txq_state* qs = &vi->ep_state->txq;
  struct efct_tx_descriptor* desc = q->descriptors;
  int i = qs->added & q->mask;

  if( tx->tail_len != 0 )
    efct_tx_word(tx, tx->tail.word);
  while( (uintptr_t)tx->aperture % EFCT_TX_ALIGNMENT != 0 )
    efct_tx_word(tx, 0);

  start = qs->ct_added % EFCT_TX_APERTURE;
  end = ((char*)tx->aperture - vi->vi_ctpio_mmap_ptr);
  len = end - start;

  desc[i].len = len;
  q->ids[i] = dma_id;
  qs->ct_added += len;
  qs->added += 1;
}

/* get a tx completion event, or null if no valid event available */
static ci_qword_t* efct_tx_get_event(const ef_vi* vi, uint32_t evq_ptr)
{
  ci_qword_t* event = (ci_qword_t*)(vi->evq_base + (evq_ptr & vi->evq_mask));

  int expect_phase = (evq_ptr & (vi->evq_mask + 1)) != 0;
  int actual_phase = CI_QWORD_FIELD(*event, EFCT_EVENT_PHASE);

  return actual_phase == expect_phase ? event : NULL;
}

/* check whether a tx completion event is available */
static bool efct_tx_check_event(const ef_vi* vi)
{
  return vi->evq_mask && efct_tx_get_event(vi, vi->ep_state->evq.evq_ptr);
}

/* handle a tx completion event */
static void efct_tx_handle_event(ef_vi* vi, ci_qword_t event, ef_event* ev_out)
{
  ef_vi_txq* q = &vi->vi_txq;
  ef_vi_txq_state* qs = &vi->ep_state->txq;
  struct efct_tx_descriptor* desc = vi->vi_txq.descriptors;

  unsigned seq = CI_QWORD_FIELD(event, EFCT_TX_EVENT_SEQUENCE);
  unsigned seq_mask = (1 << EFCT_TX_EVENT_SEQUENCE_WIDTH) - 1;

  /* Fully inclusive range as both previous and seq are both inclusive */
  while( (qs->previous & seq_mask) != ((seq + 1) & seq_mask) ) {
    BUG_ON(qs->previous == qs->added);
    qs->ct_removed += desc[qs->previous & q->mask].len;
    qs->previous += 1;
  }

  ev_out->tx.type = EF_EVENT_TYPE_TX; /* TODO _WITH_TIMESTAMP */
  ev_out->tx.q_id = CI_QWORD_FIELD(event, EFCT_TX_EVENT_LABEL);
  ev_out->tx.flags = EF_EVENT_FLAG_CTPIO;
  ev_out->tx.desc_id = qs->previous;
}

static int efct_ef_vi_transmit(ef_vi* vi, ef_addr base, int len,
                               ef_request_id dma_id)
{
  /* TODO need to avoid calling this with CTPIO fallback buffers */
  struct efct_tx_state tx;

  if( ! efct_tx_check(vi, len) )
    return -EAGAIN;

  efct_tx_init(vi, &tx);
  /* TODO timestamp flag */
  efct_tx_word(&tx, efct_tx_pkt_header(len, EFCT_TX_CT_DISABLE, 0));
  efct_tx_block(&tx, (void*)(uintptr_t)base, len);
  efct_tx_complete(vi, &tx, dma_id);

  return 0;
}

static int efct_ef_vi_transmitv(ef_vi* vi, const ef_iovec* iov, int iov_len,
                                ef_request_id dma_id)
{
  struct efct_tx_state tx;
  int len = 0, i;

  efct_tx_init(vi, &tx);

  for( i = 0; i < iov_len; ++i )
    len += iov[i].iov_len;

  if( ! efct_tx_check(vi, len) )
    return -EAGAIN;

  /* TODO timestamp flag */
  efct_tx_word(&tx, efct_tx_pkt_header(len, EFCT_TX_CT_DISABLE, 0));

  for( i = 0; i < iov_len; ++i )
    efct_tx_block(&tx, (void*)(uintptr_t)iov[i].iov_base, iov[i].iov_len);

  efct_tx_complete(vi, &tx, dma_id);

  return 0;
}

static void efct_ef_vi_transmit_push(ef_vi* vi)
{
}

static int efct_ef_vi_transmit_pio(ef_vi* vi, int offset, int len,
                                   ef_request_id dma_id)
{
  return -EOPNOTSUPP;
}

static int efct_ef_vi_transmit_copy_pio(ef_vi* vi, int offset,
                                        const void* src_buf, int len,
                                        ef_request_id dma_id)
{
  return -EOPNOTSUPP;
}

static void efct_ef_vi_transmit_pio_warm(ef_vi* vi)
{
}

static void efct_ef_vi_transmit_copy_pio_warm(ef_vi* vi, int pio_offset,
                                              const void* src_buf, int len)
{
}

static void efct_ef_vi_transmitv_ctpio(ef_vi* vi, size_t len,
                                       const struct iovec* iov, int iovcnt,
                                       unsigned threshold)
{
  struct efct_tx_state tx;
  int i;

  /* The caller must check space, as this function can't report failure. */
  /* TODO this function should probably remain compatible with legacy ef_vi,
   * perhaps by doing nothing and deferring transmission to when the fallback
   * buffer is posted. In that case we'd need another API, very similar to
   * this, but without the requirement for a fallback buffer, for best speed.
   */
  BUG_ON(!efct_tx_check(vi, len));
  efct_tx_init(vi, &tx);

  /* TODO timestamp flag */
  /* ef_vi interface takes threshold in bytes, but the efct hardware interface
   * takes multiples of 64.
   */
  threshold >>= 6;
  /* Anything too big to fit in the field width is equivalent in disabling
   * cut through.
   */
  if( threshold > EFCT_TX_CT_DISABLE )
    threshold = EFCT_TX_CT_DISABLE;
  efct_tx_word(&tx, efct_tx_pkt_header(len, threshold, 0));

  for( i = 0; i < iovcnt; ++i )
    efct_tx_block(&tx, iov[i].iov_base, iov[i].iov_len);

  /* Use a valid but bogus dma_id rather than invalid EF_REQUEST_ID_MASK to
   * support tcpdirect, which relies on the correct return value from
   * ef_vi_transmit_unbundle to free its otherwise * unused transmit buffers.
   */
  efct_tx_complete(vi, &tx, 0);

  /* TODO for ef_vi compatibility, we probably need an efct-specific version of
   * ef_vi_transmit_ctpio_fallback to record the correct dma_id.
   */
}

static void efct_ef_vi_transmitv_ctpio_copy(ef_vi* vi, size_t frame_len,
                                            const struct iovec* iov, int iovcnt,
                                            unsigned threshold, void* fallback)
{
  /* Fallback is unnecessary for this architecture */
  efct_ef_vi_transmitv_ctpio(vi, frame_len, iov, iovcnt, threshold);
}

static int efct_ef_vi_transmit_alt_select(ef_vi* vi, unsigned alt_id)
{
  return -EOPNOTSUPP;
}

static int efct_ef_vi_transmit_alt_select_default(ef_vi* vi)
{
  return -EOPNOTSUPP;
}

static int efct_ef_vi_transmit_alt_stop(ef_vi* vi, unsigned alt_id)
{
  return -EOPNOTSUPP;
}

static int efct_ef_vi_transmit_alt_go(ef_vi* vi, unsigned alt_id)
{
  return -EOPNOTSUPP;
}

static int efct_ef_vi_transmit_alt_discard(ef_vi* vi, unsigned alt_id)
{
  return -EOPNOTSUPP;
}

static int efct_ef_vi_receive_init(ef_vi* vi, ef_addr addr,
                                   ef_request_id dma_id)
{
  /* TODO X3 */
  return -ENOSYS;
}

static void efct_ef_vi_receive_push(ef_vi* vi)
{
  /* TODO X3 */
}

static int rx_rollover(ef_vi* vi, int qid)
{
  uint32_t pkt_id;
  uint32_t next;
  uint32_t superbuf_pkts = vi->efct_rxq[qid].superbuf_pkts;
  ef_vi_rxq_state* qs = &vi->ep_state->rxq;
  int rc = superbuf_next(vi, &vi->efct_rxq[qid]);
  if( rc < 0 )
    return rc;
  pkt_id = (qid * CI_EFCT_MAX_SUPERBUFS + (rc & CI_EFCT_Q_SUPERBUF_ID_MASK)) <<
           PKTS_PER_SUPERBUF_BITS;
  next = pkt_id | ((rc >> 15) << 31);
  if( pkt_id_to_index_in_superbuf(qs->rxq_ptr[qid].next) > superbuf_pkts ) {
    /* special case for when we want to ignore the first metadata, e.g. at
     * queue startup */
    qs->rxq_ptr[qid].prev = next;
    qs->rxq_ptr[qid].next = next + 1;
  }
  else {
    qs->rxq_ptr[qid].next = next;
  }
  /* Preload the superbuf's refcount with all the (potential) packets in
   * it - more efficient than incrementing for each rx individually */
  efct_rx_desc(vi, pkt_id)->refcnt = superbuf_pkts;
  return 0;
}

static int efct_poll_rx(ef_vi* vi, int qid, ef_event* evs, int evs_len)
{
  ef_vi_rxq_state* qs = &vi->ep_state->rxq;
  ef_vi_efct_rxq* rxq = &vi->efct_rxq[qid];
  struct efab_efct_rxq_uk_shm* shm = rxq->shm;
  uint32_t superbuf_pkts = rxq->superbuf_pkts;
  int i;

  if( ! efct_rxq_is_active(vi, qid) )
    return 0;

  for( i = 0; i < evs_len; ++i, ++qs->removed ) {
    const ci_qword_t* header;

    if( pkt_id_to_index_in_superbuf(rxq_ptr_to_pkt_id(qs->rxq_ptr[qid].next))
        >= superbuf_pkts ) {
      if( rx_rollover(vi, qid) < 0 ) {
        /* ef_eventq_poll() has historically never been able to fail, so we
         * maintain that policy */
        return i;
      }
    }
    /* We only need to check for new config after a rollover and for the first
     * ev in a poll (in case some other address space did a rollover and we
     * now need to mmap here), but it's just as cheap to test the real thing
     * every time */
    if( shm->config_generation != rxq->config_generation )
      if( superbuf_config_refresh(vi, rxq) < 0 )
        return i;

    header = efct_rx_next_header(vi, qid);
    if( header == NULL )
      break;

    /* For simplicity, require configuration for a fixed data offset.
     * Otherwise, we'd also have to check NEXT_FRAME_LOC in the previous buffer.
     */
    BUG_ON(CI_QWORD_FIELD(*header, EFCT_RX_HEADER_NEXT_FRAME_LOC) != 1);

    evs[i].rx.type = EF_EVENT_TYPE_RX;
    evs[i].rx.q_id = qid;
    evs[i].rx.rq_id = rxq_ptr_to_pkt_id(qs->rxq_ptr[qid].prev);
    evs[i].rx.len = CI_QWORD_FIELD(*header, EFCT_RX_HEADER_PACKET_LENGTH);
    evs[i].rx.flags = EF_EVENT_FLAG_SOP;
    evs[i].rx.ofs = EFCT_RX_HEADER_NEXT_FRAME_LOC_1;
    /* TODO might be nice to provide more of the available metadata */
    /* TODO: handle manual rollover */

    qs->rxq_ptr[qid].prev = qs->rxq_ptr[qid].next++;
  }

  return i;
}

static int efct_poll_tx(ef_vi* vi, ef_event* evs, int evs_len)
{
  ef_eventq_state* evq = &vi->ep_state->evq;
  ci_qword_t* event;
  int i;

  /* Check for overflow. If the previous entry has been overwritten already,
   * then it will have the wrong phase value and will appear invalid */
  BUG_ON(efct_tx_get_event(vi, evq->evq_ptr - sizeof(*event)) == NULL);

  for( i = 0; i < evs_len; ++i, evq->evq_ptr += sizeof(*event) ) {
    event = efct_tx_get_event(vi, evq->evq_ptr);
    if( event == NULL )
      break;

    switch( CI_QWORD_FIELD(*event, EFCT_EVENT_TYPE) ) {
      case EFCT_EVENT_TYPE_TX:
        efct_tx_handle_event(vi, *event, &evs[i]);
        break;
      case EFCT_EVENT_TYPE_CONTROL:
        /* TODO X3 */
        break;
      default:
        ef_log("%s:%d: ERROR: event="CI_QWORD_FMT,
               __FUNCTION__, __LINE__, CI_QWORD_VAL(*event));
        break;
    }
  }

  return i;
}

static int efct_ef_eventq_poll_1rx(ef_vi* vi, ef_event* evs, int evs_len)
{
  return efct_poll_rx(vi, 0, evs, evs_len);
}

static int efct_ef_eventq_poll_1rxtx(ef_vi* vi, ef_event* evs, int evs_len)
{
  int i;

  i = efct_poll_rx(vi, 0, evs, evs_len);
  i += efct_poll_tx(vi, evs + i, evs_len - i);

  return i;
}

static int efct_ef_eventq_poll_generic(ef_vi* vi, ef_event* evs, int evs_len)
{
  int i, n = 0;
  for( i = 0; i < vi->max_efct_rxq; ++i )
    n += efct_poll_rx(vi, i, evs + n, evs_len - n);
  if( vi->vi_txq.mask )
    n += efct_poll_tx(vi, evs + n, evs_len - n);
  return n;
}

static void efct_ef_eventq_prime(ef_vi* vi)
{
  /* TODO X3 */
}

static void efct_ef_eventq_timer_prime(ef_vi* vi, unsigned v)
{
  /* TODO X3 */
}

static void efct_ef_eventq_timer_run(ef_vi* vi, unsigned v)
{
  /* TODO X3 */
}

static void efct_ef_eventq_timer_clear(ef_vi* vi)
{
  /* TODO X3 */
}

static void efct_ef_eventq_timer_zero(ef_vi* vi)
{
  /* TODO X3 */
}

static ssize_t efct_ef_vi_transmit_memcpy(struct ef_vi* vi,
                                          const ef_remote_iovec* dst_iov,
                                          int dst_iov_len,
                                          const ef_remote_iovec* src_iov,
                                          int src_iov_len)
{
  return -EOPNOTSUPP;
}

static int efct_ef_vi_transmit_memcpy_sync(struct ef_vi* vi,
                                           ef_request_id dma_id)
{
  return -EOPNOTSUPP;
}

static void efct_vi_initialise_ops(ef_vi* vi)
{
  vi->ops.transmit               = efct_ef_vi_transmit;
  vi->ops.transmitv              = efct_ef_vi_transmitv;
  vi->ops.transmitv_init         = efct_ef_vi_transmitv;
  vi->ops.transmit_push          = efct_ef_vi_transmit_push;
  vi->ops.transmit_pio           = efct_ef_vi_transmit_pio;
  vi->ops.transmit_copy_pio      = efct_ef_vi_transmit_copy_pio;
  vi->ops.transmit_pio_warm      = efct_ef_vi_transmit_pio_warm;
  vi->ops.transmit_copy_pio_warm = efct_ef_vi_transmit_copy_pio_warm;
  vi->ops.transmitv_ctpio        = efct_ef_vi_transmitv_ctpio;
  vi->ops.transmitv_ctpio_copy   = efct_ef_vi_transmitv_ctpio_copy;
  vi->ops.transmit_alt_select    = efct_ef_vi_transmit_alt_select;
  vi->ops.transmit_alt_select_default = efct_ef_vi_transmit_alt_select_default;
  vi->ops.transmit_alt_stop      = efct_ef_vi_transmit_alt_stop;
  vi->ops.transmit_alt_go        = efct_ef_vi_transmit_alt_go;
  vi->ops.transmit_alt_discard   = efct_ef_vi_transmit_alt_discard;
  vi->ops.receive_init           = efct_ef_vi_receive_init;
  vi->ops.receive_push           = efct_ef_vi_receive_push;
  vi->ops.eventq_prime           = efct_ef_eventq_prime;
  vi->ops.eventq_timer_prime     = efct_ef_eventq_timer_prime;
  vi->ops.eventq_timer_run       = efct_ef_eventq_timer_run;
  vi->ops.eventq_timer_clear     = efct_ef_eventq_timer_clear;
  vi->ops.eventq_timer_zero      = efct_ef_eventq_timer_zero;
  vi->ops.transmit_memcpy        = efct_ef_vi_transmit_memcpy;
  vi->ops.transmit_memcpy_sync   = efct_ef_vi_transmit_memcpy_sync;

  if( vi->vi_flags & EF_VI_EFCT_UNIQUEUE ) {
    vi->max_efct_rxq = 1;
    if( vi->vi_txq.mask == 0 )
      vi->ops.eventq_poll = efct_ef_eventq_poll_1rx;
    else
      vi->ops.eventq_poll = efct_ef_eventq_poll_1rxtx;
  }
  else {
    /* It wouldn't be difficult to specialise this by txable too, but this is
     * the slow, backward-compatible variant so there's not much point */
    vi->ops.eventq_poll = efct_ef_eventq_poll_generic;
    vi->max_efct_rxq = EF_VI_MAX_EFCT_RXQS;
  }
}

void efct_vi_init(ef_vi* vi)
{
  EF_VI_BUILD_ASSERT(sizeof(struct efct_tx_descriptor) ==
                     EFCT_TX_DESCRIPTOR_BYTES);
  EF_VI_BUILD_ASSERT(sizeof(struct efct_rx_descriptor) ==
                     EFCT_RX_DESCRIPTOR_BYTES);

  efct_vi_initialise_ops(vi);
  vi->evq_phase_bits = 1;
}

#ifndef __KERNEL__
int efct_vi_mmap_init(ef_vi* vi)
{
  void* space;
  uint64_t* mappings;
  int i;
  const size_t bytes_per_rxq = CI_EFCT_MAX_SUPERBUFS * EFCT_RX_SUPERBUF_BYTES;
  const size_t mappings_bytes =
    vi->max_efct_rxq * CI_EFCT_MAX_HUGEPAGES * sizeof(mappings[0]);

  mappings = malloc(mappings_bytes);
  if( mappings == NULL )
    return -ENOMEM;

  memset(mappings, 0xff, mappings_bytes);

  /* This is reserving a gigantic amount of virtual address space (with no
   * memory behind it) so we can later on (in efct_vi_attach_rxq()) plonk the
   * actual mmappings for each specific superbuf into a computable place
   * within this space, i.e. so that conversion from {rxq#,superbuf#} to
   * memory address is trivial arithmetic rather than needing various array
   * lookups (c.f. what we need to do in ifdef __KERNEL__, which doesn't have
   * facilities for this kind of address space trickery). */
  space = mmap(NULL, vi->max_efct_rxq * bytes_per_rxq, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_HUGETLB,
               -1, 0);
  if( space == MAP_FAILED ) {
    free(mappings);
    return -ENOMEM;
  }

  for( i = 0; i < vi->max_efct_rxq; ++i ) {
    ef_vi_efct_rxq* rxq = &vi->efct_rxq[i];
    rxq->superbuf = (char*)space + i * bytes_per_rxq;
    rxq->current_mappings = mappings + i * CI_EFCT_MAX_HUGEPAGES;
  }

  /* TODO EFCT: This will eventually move to filter_add: */
  return efct_vi_attach_rxq(vi, 0, 4);
}

void efct_vi_munmap(ef_vi* vi)
{
  munmap((void*)vi->efct_rxq[0].superbuf,
         vi->max_efct_rxq * CI_EFCT_MAX_SUPERBUFS * EFCT_RX_SUPERBUF_BYTES);
  free(vi->efct_rxq[0].current_mappings);
}

int efct_vi_attach_rxq(ef_vi* vi, int qid, unsigned n_superbufs)
{
  int rc;
  ci_resource_alloc_t ra;
  void* p;
  int ix;
  ef_vi_efct_rxq* rxq;

  for( ix = 0; ix < vi->max_efct_rxq; ++ix )
    if( ! efct_rxq_is_active(vi, ix) )
      break;
  if( ix == vi->max_efct_rxq )
    return -ENOSPC;

  memset(&ra, 0, sizeof(ra));
  ef_vi_set_intf_ver(ra.intf_ver, sizeof(ra.intf_ver));
  ra.ra_type = EFRM_RESOURCE_EFCT_RXQ;
  ra.u.rxq.in_qid = qid;
  ra.u.rxq.in_vi_rs_id = efch_make_resource_id(vi->vi_resource_id);
  ra.u.rxq.in_n_hugepages = CI_ROUND_UP(n_superbufs,
                                        CI_EFCT_SUPERBUFS_PER_PAGE);
  ra.u.rxq.in_timestamp_req = true;
  rc = ci_resource_alloc(vi->dh, &ra);
  if( rc < 0 ) {
    LOGVV(ef_log("%s: ci_resource_alloc rxq %d", __FUNCTION__, rc));
    return rc;
  }

  rc = ci_resource_mmap(vi->dh, ra.out_id.index, 0,
                CI_ROUND_UP(sizeof(struct efab_efct_rxq_uk_shm), CI_PAGE_SIZE),
                &p);
  if( rc ) {
    LOGVV(ef_log("%s: ci_resource_mmap rxq %d", __FUNCTION__, rc));
    return rc;
  }

  rxq = &vi->efct_rxq[ix];
  rxq->resource_id = ra.out_id.index;
  rxq->shm = p;
  rxq->config_generation = rxq->shm->config_generation - 1;
  rxq->superbuf_pkts = EFCT_RX_SUPERBUF_BYTES / EFCT_PKT_STRIDE;
  /* This is a totally fake pkt_id, but it makes efct_poll_rx() think that a
   * rollover is needed. We use +1 as a marker that this is the first packet,
   * i.e. ignore the first metadata: */
  vi->ep_state->rxq.rxq_ptr[ix].next = 1 + rxq->superbuf_pkts;

  return 0;
}

/* efct_vi_detach_rxq not yet implemented */
#endif

void efct_vi_rxpkt_get(ef_vi* vi, uint32_t pkt_id, const void** pkt_start)
{
  EF_VI_ASSERT(vi->nic_type.arch == EF_VI_ARCH_EFCT);

  /* assume DP_FRAME_OFFSET_FIXED (correct for initial hardware) */
  *pkt_start = (char*)efct_rx_header(vi, pkt_id) +
               EFCT_RX_HEADER_NEXT_FRAME_LOC_1;
}

void efct_vi_rxpkt_release(ef_vi* vi, uint32_t pkt_id)
{
  EF_VI_ASSERT(efct_rx_desc(vi, pkt_id)->refcnt > 0);

  if( --efct_rx_desc(vi, pkt_id)->refcnt == 0 )
    superbuf_free(vi, &vi->efct_rxq[pkt_id_to_rxq_ix(pkt_id)],
                  pkt_id_to_local_superbuf_ix(pkt_id));
}

int efct_ef_eventq_check_event(const ef_vi* vi)
{
  return efct_tx_check_event(vi) || efct_rx_check_event(vi);
}


/*
 * ngtcp2
 *
 * Copyright (c) 2017 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "ngtcp2_conn.h"

#include <string.h>
#include <assert.h>
#include <math.h>

#include "ngtcp2_ppe.h"
#include "ngtcp2_macro.h"
#include "ngtcp2_log.h"
#include "ngtcp2_cid.h"
#include "ngtcp2_conv.h"
#include "ngtcp2_vec.h"
#include "ngtcp2_addr.h"
#include "ngtcp2_path.h"

/*
 * conn_local_stream returns nonzero if |stream_id| indicates that it
 * is the stream initiated by local endpoint.
 */
static int conn_local_stream(ngtcp2_conn *conn, int64_t stream_id) {
  return (uint8_t)(stream_id & 1) == conn->server;
}

/*
 * bidi_stream returns nonzero if |stream_id| is a bidirectional
 * stream ID.
 */
static int bidi_stream(int64_t stream_id) { return (stream_id & 0x2) == 0; }

static int conn_call_recv_client_initial(ngtcp2_conn *conn,
                                         const ngtcp2_cid *dcid) {
  int rv;

  assert(conn->callbacks.recv_client_initial);

  rv = conn->callbacks.recv_client_initial(conn, dcid, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_handshake_completed(ngtcp2_conn *conn) {
  int rv;

  if (!conn->callbacks.handshake_completed) {
    return 0;
  }

  rv = conn->callbacks.handshake_completed(conn, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_recv_stream_data(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                      int fin, uint64_t offset,
                                      const uint8_t *data, size_t datalen) {
  int rv;

  if (!conn->callbacks.recv_stream_data) {
    return 0;
  }

  rv = conn->callbacks.recv_stream_data(conn, strm->stream_id, fin, offset,
                                        data, datalen, conn->user_data,
                                        strm->stream_user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_recv_crypto_data(ngtcp2_conn *conn,
                                      ngtcp2_crypto_level crypto_level,
                                      uint64_t offset, const uint8_t *data,
                                      size_t datalen) {
  int rv;

  rv = conn->callbacks.recv_crypto_data(conn, crypto_level, offset, data,
                                        datalen, conn->user_data);
  switch (rv) {
  case 0:
  case NGTCP2_ERR_CRYPTO:
  case NGTCP2_ERR_PROTO:
  case NGTCP2_ERR_CALLBACK_FAILURE:
    return rv;
  default:
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
}

static int conn_call_stream_open(ngtcp2_conn *conn, ngtcp2_strm *strm) {
  int rv;

  if (!conn->callbacks.stream_open) {
    return 0;
  }

  rv = conn->callbacks.stream_open(conn, strm->stream_id, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_stream_close(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                  uint16_t app_error_code) {
  int rv;

  if (!conn->callbacks.stream_close) {
    return 0;
  }

  rv = conn->callbacks.stream_close(conn, strm->stream_id, app_error_code,
                                    conn->user_data, strm->stream_user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_stream_reset(ngtcp2_conn *conn, int64_t stream_id,
                                  uint64_t final_size, uint16_t app_error_code,
                                  void *stream_user_data) {
  int rv;

  if (!conn->callbacks.stream_reset) {
    return 0;
  }

  rv = conn->callbacks.stream_reset(conn, stream_id, final_size, app_error_code,
                                    conn->user_data, stream_user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_extend_max_local_streams_bidi(ngtcp2_conn *conn,
                                                   uint64_t max_streams) {
  int rv;

  if (!conn->callbacks.extend_max_local_streams_bidi) {
    return 0;
  }

  rv = conn->callbacks.extend_max_local_streams_bidi(conn, max_streams,
                                                     conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_extend_max_local_streams_uni(ngtcp2_conn *conn,
                                                  uint64_t max_streams) {
  int rv;

  if (!conn->callbacks.extend_max_local_streams_uni) {
    return 0;
  }

  rv = conn->callbacks.extend_max_local_streams_uni(conn, max_streams,
                                                    conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_get_new_connection_id(ngtcp2_conn *conn, ngtcp2_cid *cid,
                                           uint8_t *token, size_t cidlen) {
  int rv;

  assert(conn->callbacks.get_new_connection_id);

  rv = conn->callbacks.get_new_connection_id(conn, cid, token, cidlen,
                                             conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_remove_connection_id(ngtcp2_conn *conn,
                                          const ngtcp2_cid *cid) {
  int rv;

  if (!conn->callbacks.remove_connection_id) {
    return 0;
  }

  rv = conn->callbacks.remove_connection_id(conn, cid, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_path_validation(ngtcp2_conn *conn, const ngtcp2_path *path,
                                     ngtcp2_path_validation_result res) {
  int rv;

  if (!conn->callbacks.path_validation) {
    return 0;
  }

  rv = conn->callbacks.path_validation(conn, path, res, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_select_preferred_addr(ngtcp2_conn *conn,
                                           ngtcp2_addr *dest) {
  int rv;

  assert(conn->callbacks.select_preferred_addr);
  assert(conn->remote.settings.preferred_address_present);

  rv = conn->callbacks.select_preferred_addr(
      conn, dest, &conn->remote.settings.preferred_address, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_extend_max_remote_streams_bidi(ngtcp2_conn *conn,
                                                    uint64_t max_streams) {
  int rv;

  if (!conn->callbacks.extend_max_remote_streams_bidi) {
    return 0;
  }

  rv = conn->callbacks.extend_max_remote_streams_bidi(conn, max_streams,
                                                      conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_extend_max_remote_streams_uni(ngtcp2_conn *conn,
                                                   uint64_t max_streams) {
  int rv;

  if (!conn->callbacks.extend_max_remote_streams_uni) {
    return 0;
  }

  rv = conn->callbacks.extend_max_remote_streams_uni(conn, max_streams,
                                                     conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int conn_call_extend_max_stream_data(ngtcp2_conn *conn,
                                            ngtcp2_strm *strm,
                                            int64_t stream_id,
                                            uint64_t datalen) {
  int rv;

  if (!conn->callbacks.extend_max_stream_data) {
    return 0;
  }

  rv = conn->callbacks.extend_max_stream_data(
      conn, stream_id, datalen, conn->user_data, strm->stream_user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

/*
 * bw_reset resets |bw| to the initial state.
 */
static void bw_reset(ngtcp2_bw *bw) {
  bw->first_ts = 0;
  bw->last_ts = 0;
  bw->datalen = 0;
  bw->value = 0.;
}

static int crypto_offset_less(const ngtcp2_pq_entry *lhs,
                              const ngtcp2_pq_entry *rhs) {
  ngtcp2_crypto_frame_chain *lfrc =
      ngtcp2_struct_of(lhs, ngtcp2_crypto_frame_chain, pe);
  ngtcp2_crypto_frame_chain *rfrc =
      ngtcp2_struct_of(rhs, ngtcp2_crypto_frame_chain, pe);

  return lfrc->fr.offset < rfrc->fr.offset;
}

static int pktns_init(ngtcp2_pktns *pktns, ngtcp2_crypto_level crypto_level,
                      ngtcp2_default_cc *cc, ngtcp2_log *log,
                      const ngtcp2_mem *mem) {
  int rv;

  rv = ngtcp2_gaptr_init(&pktns->rx.pngap, mem);
  if (rv != 0) {
    return rv;
  }

  pktns->tx.last_pkt_num = -1;
  pktns->rx.max_pkt_num = -1;

  rv = ngtcp2_acktr_init(&pktns->acktr, log, mem);
  if (rv != 0) {
    goto fail_acktr_init;
  }

  rv = ngtcp2_strm_init(&pktns->crypto.strm, 0, NGTCP2_STRM_FLAG_NONE, 0, 0,
                        NULL, mem);
  if (rv != 0) {
    goto fail_crypto_init;
  }

  ngtcp2_rtb_init(&pktns->rtb, crypto_level, &pktns->crypto.strm, cc, log, mem);
  ngtcp2_pq_init(&pktns->crypto.tx.frq, crypto_offset_less, mem);

  return 0;

fail_crypto_init:
  ngtcp2_acktr_free(&pktns->acktr);
fail_acktr_init:
  ngtcp2_gaptr_free(&pktns->rx.pngap);

  return rv;
}

static int cycle_less(const ngtcp2_pq_entry *lhs, const ngtcp2_pq_entry *rhs) {
  ngtcp2_strm *ls = ngtcp2_struct_of(lhs, ngtcp2_strm, pe);
  ngtcp2_strm *rs = ngtcp2_struct_of(rhs, ngtcp2_strm, pe);

  if (ls->cycle < rs->cycle) {
    return rs->cycle - ls->cycle <= 1;
  }

  return ls->cycle - rs->cycle > 1;
}

static void delete_buffed_pkts(ngtcp2_pkt_chain *pc, const ngtcp2_mem *mem) {
  ngtcp2_pkt_chain *next;

  for (; pc;) {
    next = pc->next;
    ngtcp2_pkt_chain_del(pc, mem);
    pc = next;
  }
}

static void pktns_free(ngtcp2_pktns *pktns, const ngtcp2_mem *mem) {
  ngtcp2_crypto_frame_chain *frc;

  delete_buffed_pkts(pktns->rx.buffed_pkts, mem);

  ngtcp2_frame_chain_list_del(pktns->tx.frq, mem);

  ngtcp2_vec_del(pktns->crypto.rx.hp, mem);
  ngtcp2_vec_del(pktns->crypto.tx.hp, mem);

  ngtcp2_crypto_km_del(pktns->crypto.rx.ckm, mem);
  ngtcp2_crypto_km_del(pktns->crypto.tx.ckm, mem);

  for (; !ngtcp2_pq_empty(&pktns->crypto.tx.frq);) {
    frc = ngtcp2_struct_of(ngtcp2_pq_top(&pktns->crypto.tx.frq),
                           ngtcp2_crypto_frame_chain, pe);
    ngtcp2_pq_pop(&pktns->crypto.tx.frq);
    ngtcp2_crypto_frame_chain_del(frc, mem);
  }

  ngtcp2_pq_free(&pktns->crypto.tx.frq);
  ngtcp2_rtb_free(&pktns->rtb);
  ngtcp2_strm_free(&pktns->crypto.strm);
  ngtcp2_acktr_free(&pktns->acktr);
  ngtcp2_gaptr_free(&pktns->rx.pngap);
}

static int cid_less(const ngtcp2_ksl_key *lhs, const ngtcp2_ksl_key *rhs) {
  return ngtcp2_cid_less(lhs->ptr, rhs->ptr);
}

static int ts_retired_less(const ngtcp2_pq_entry *lhs,
                           const ngtcp2_pq_entry *rhs) {
  const ngtcp2_scid *a = ngtcp2_struct_of(lhs, ngtcp2_scid, pe);
  const ngtcp2_scid *b = ngtcp2_struct_of(rhs, ngtcp2_scid, pe);

  return a->ts_retired < b->ts_retired;
}

static void rcvry_stat_reset(ngtcp2_rcvry_stat *rcs) {
  memset(rcs, 0, sizeof(*rcs));
  rcs->min_rtt = UINT64_MAX;
}

static void cc_stat_reset(ngtcp2_cc_stat *ccs) {
  memset(ccs, 0, sizeof(*ccs));
  ccs->cwnd = ngtcp2_min(10 * NGTCP2_MAX_DGRAM_SIZE,
                         ngtcp2_max(2 * NGTCP2_MAX_DGRAM_SIZE, 14720));
  ccs->ssthresh = UINT64_MAX;
}

static void delete_scid(ngtcp2_ksl *scids, const ngtcp2_mem *mem) {
  ngtcp2_ksl_it it;

  for (it = ngtcp2_ksl_begin(scids); !ngtcp2_ksl_it_end(&it);
       ngtcp2_ksl_it_next(&it)) {
    ngtcp2_mem_free(mem, ngtcp2_ksl_it_get(&it));
  }
}

static int conn_new(ngtcp2_conn **pconn, const ngtcp2_cid *dcid,
                    const ngtcp2_cid *scid, const ngtcp2_path *path,
                    uint32_t version, const ngtcp2_conn_callbacks *callbacks,
                    const ngtcp2_settings *settings, const ngtcp2_mem *mem,
                    void *user_data, int server) {
  int rv;
  ngtcp2_scid *scident;
  ngtcp2_ksl_key key;

  if (mem == NULL) {
    mem = ngtcp2_mem_default();
  }

  *pconn = ngtcp2_mem_calloc(mem, 1, sizeof(ngtcp2_conn));
  if (*pconn == NULL) {
    rv = NGTCP2_ERR_NOMEM;
    goto fail_conn;
  }

  rv = ngtcp2_ringbuf_init(&(*pconn)->dcid.bound,
                           NGTCP2_MAX_BOUND_DCID_POOL_SIZE, sizeof(ngtcp2_dcid),
                           mem);
  if (rv != 0) {
    goto fail_dcid_bound_init;
  }

  rv = ngtcp2_ringbuf_init(&(*pconn)->dcid.unused, NGTCP2_MAX_DCID_POOL_SIZE,
                           sizeof(ngtcp2_dcid), mem);
  if (rv != 0) {
    goto fail_dcid_unused_init;
  }

  rv =
      ngtcp2_ringbuf_init(&(*pconn)->dcid.retired, NGTCP2_MAX_DCID_RETIRED_SIZE,
                          sizeof(ngtcp2_dcid), mem);
  if (rv != 0) {
    goto fail_dcid_retired_init;
  }

  rv = ngtcp2_ksl_init(&(*pconn)->scid.set, cid_less, sizeof(ngtcp2_cid), mem);
  if (rv != 0) {
    goto fail_scid_set_init;
  }

  ngtcp2_pq_init(&(*pconn)->scid.used, ts_retired_less, mem);

  rv = ngtcp2_map_init(&(*pconn)->strms, mem);
  if (rv != 0) {
    goto fail_strms_init;
  }

  ngtcp2_pq_init(&(*pconn)->tx.strmq, cycle_less, mem);

  rv = ngtcp2_idtr_init(&(*pconn)->remote.bidi.idtr, !server, mem);
  if (rv != 0) {
    goto fail_remote_bidi_idtr_init;
  }

  rv = ngtcp2_idtr_init(&(*pconn)->remote.uni.idtr, !server, mem);
  if (rv != 0) {
    goto fail_remote_uni_idtr_init;
  }

  rv = ngtcp2_ringbuf_init(&(*pconn)->rx.path_challenge, 4,
                           sizeof(ngtcp2_path_challenge_entry), mem);
  if (rv != 0) {
    goto fail_rx_path_challenge_init;
  }

  ngtcp2_log_init(&(*pconn)->log, scid, settings->log_printf,
                  settings->initial_ts, user_data);

  ngtcp2_default_cc_init(&(*pconn)->cc, &(*pconn)->ccs, &(*pconn)->log);

  rv = pktns_init(&(*pconn)->in_pktns, NGTCP2_CRYPTO_LEVEL_INITIAL,
                  &(*pconn)->cc, &(*pconn)->log, mem);
  if (rv != 0) {
    goto fail_in_pktns_init;
  }

  rv = pktns_init(&(*pconn)->hs_pktns, NGTCP2_CRYPTO_LEVEL_HANDSHAKE,
                  &(*pconn)->cc, &(*pconn)->log, mem);
  if (rv != 0) {
    goto fail_hs_pktns_init;
  }

  rv = pktns_init(&(*pconn)->pktns, NGTCP2_CRYPTO_LEVEL_APP, &(*pconn)->cc,
                  &(*pconn)->log, mem);
  if (rv != 0) {
    goto fail_pktns_init;
  }

  scident = ngtcp2_mem_malloc(mem, sizeof(*scident));
  if (scident == NULL) {
    rv = NGTCP2_ERR_NOMEM;
    goto fail_scident;
  }

  ngtcp2_scid_init(scident, 0, scid,
                   settings->stateless_reset_token_present
                       ? settings->stateless_reset_token
                       : NULL);

  rv = ngtcp2_ksl_insert(&(*pconn)->scid.set, NULL,
                         ngtcp2_ksl_key_ptr(&key, &scident->cid), scident);
  if (rv != 0) {
    goto fail_scid_set_insert;
  }

  scident = NULL;

  if (server && settings->preferred_address_present) {
    scident = ngtcp2_mem_malloc(mem, sizeof(*scident));
    if (scid == NULL) {
      rv = NGTCP2_ERR_NOMEM;
      goto fail_scident;
    }

    ngtcp2_scid_init(scident, 1, &settings->preferred_address.cid,
                     settings->preferred_address.stateless_reset_token);

    rv = ngtcp2_ksl_insert(&(*pconn)->scid.set, NULL,
                           ngtcp2_ksl_key_ptr(&key, &scident->cid), scident);
    if (rv != 0) {
      goto fail_scid_set_insert;
    }

    scident = NULL;

    (*pconn)->scid.last_seq = 1;
  }

  ngtcp2_dcid_init(&(*pconn)->dcid.current, 0, dcid, NULL);
  ngtcp2_path_copy(&(*pconn)->dcid.current.ps.path, path);

  (*pconn)->oscid = *scid;
  (*pconn)->callbacks = *callbacks;
  (*pconn)->version = version;
  (*pconn)->mem = mem;
  (*pconn)->user_data = user_data;
  (*pconn)->local.settings = *settings;
  (*pconn)->rx.unsent_max_offset = (*pconn)->rx.max_offset = settings->max_data;

  rcvry_stat_reset(&(*pconn)->rcs);
  cc_stat_reset(&(*pconn)->ccs);

  return 0;

fail_scid_set_insert:
  ngtcp2_mem_free(mem, scident);
fail_scident:
  pktns_free(&(*pconn)->pktns, mem);
fail_pktns_init:
  pktns_free(&(*pconn)->hs_pktns, mem);
fail_hs_pktns_init:
  pktns_free(&(*pconn)->in_pktns, mem);
fail_in_pktns_init:
  ngtcp2_default_cc_free(&(*pconn)->cc);
  ngtcp2_ringbuf_free(&(*pconn)->rx.path_challenge);
fail_rx_path_challenge_init:
  ngtcp2_idtr_free(&(*pconn)->remote.uni.idtr);
fail_remote_uni_idtr_init:
  ngtcp2_idtr_free(&(*pconn)->remote.bidi.idtr);
fail_remote_bidi_idtr_init:
  ngtcp2_map_free(&(*pconn)->strms);
fail_strms_init:
  delete_scid(&(*pconn)->scid.set, mem);
  ngtcp2_ksl_free(&(*pconn)->scid.set);
fail_scid_set_init:
  ngtcp2_ringbuf_free(&(*pconn)->dcid.retired);
fail_dcid_retired_init:
  ngtcp2_ringbuf_free(&(*pconn)->dcid.unused);
fail_dcid_unused_init:
  ngtcp2_ringbuf_free(&(*pconn)->dcid.bound);
fail_dcid_bound_init:
  ngtcp2_mem_free(mem, *pconn);
fail_conn:
  return rv;
}

int ngtcp2_conn_client_new(ngtcp2_conn **pconn, const ngtcp2_cid *dcid,
                           const ngtcp2_cid *scid, const ngtcp2_path *path,
                           uint32_t version,
                           const ngtcp2_conn_callbacks *callbacks,
                           const ngtcp2_settings *settings,
                           const ngtcp2_mem *mem, void *user_data) {
  int rv;
  rv = conn_new(pconn, dcid, scid, path, version, callbacks, settings, mem,
                user_data, 0);
  if (rv != 0) {
    return rv;
  }
  (*pconn)->rcid = *dcid;
  (*pconn)->remote.bidi.unsent_max_streams = (*pconn)->remote.bidi.max_streams =
      settings->max_streams_bidi;

  (*pconn)->remote.uni.unsent_max_streams = (*pconn)->remote.uni.max_streams =
      settings->max_streams_uni;

  (*pconn)->state = NGTCP2_CS_CLIENT_INITIAL;
  (*pconn)->local.bidi.next_stream_id = 0;
  (*pconn)->local.uni.next_stream_id = 2;
  return 0;
}

int ngtcp2_conn_server_new(ngtcp2_conn **pconn, const ngtcp2_cid *dcid,
                           const ngtcp2_cid *scid, const ngtcp2_path *path,
                           uint32_t version,
                           const ngtcp2_conn_callbacks *callbacks,
                           const ngtcp2_settings *settings,
                           const ngtcp2_mem *mem, void *user_data) {
  int rv;
  rv = conn_new(pconn, dcid, scid, path, version, callbacks, settings, mem,
                user_data, 1);
  if (rv != 0) {
    return rv;
  }
  (*pconn)->server = 1;
  (*pconn)->remote.bidi.unsent_max_streams = (*pconn)->remote.bidi.max_streams =
      settings->max_streams_bidi;

  (*pconn)->remote.uni.unsent_max_streams = (*pconn)->remote.uni.max_streams =
      settings->max_streams_uni;

  (*pconn)->state = NGTCP2_CS_SERVER_INITIAL;
  (*pconn)->local.bidi.next_stream_id = 1;
  (*pconn)->local.uni.next_stream_id = 3;
  return 0;
}

/*
 * conn_fc_credits returns the number of bytes allowed to be sent to
 * the given stream.  Both connection and stream level flow control
 * credits are considered.
 */
static size_t conn_fc_credits(ngtcp2_conn *conn, ngtcp2_strm *strm) {
  return ngtcp2_min(strm->tx.max_offset - strm->tx.offset,
                    conn->tx.max_offset - conn->tx.offset);
}

/*
 * conn_enforce_flow_control returns the number of bytes allowed to be
 * sent to the given stream.  |len| might be shorted because of
 * available flow control credits.
 */
static size_t conn_enforce_flow_control(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                        size_t len) {
  size_t fc_credits = conn_fc_credits(conn, strm);
  return ngtcp2_min(len, fc_credits);
}

static int delete_strms_each(ngtcp2_map_entry *ent, void *ptr) {
  const ngtcp2_mem *mem = ptr;
  ngtcp2_strm *s = ngtcp2_struct_of(ent, ngtcp2_strm, me);

  ngtcp2_strm_free(s);
  ngtcp2_mem_free(mem, s);

  return 0;
}

void ngtcp2_conn_del(ngtcp2_conn *conn) {
  if (conn == NULL) {
    return;
  }

  ngtcp2_mem_free(conn->mem, conn->token.begin);
  ngtcp2_mem_free(conn->mem, conn->crypto.decrypt_buf.base);

  ngtcp2_crypto_km_del(conn->crypto.key_update.old_rx_ckm, conn->mem);
  ngtcp2_crypto_km_del(conn->crypto.key_update.new_rx_ckm, conn->mem);
  ngtcp2_crypto_km_del(conn->crypto.key_update.new_tx_ckm, conn->mem);
  ngtcp2_vec_del(conn->early.hp, conn->mem);
  ngtcp2_crypto_km_del(conn->early.ckm, conn->mem);

  pktns_free(&conn->pktns, conn->mem);
  pktns_free(&conn->hs_pktns, conn->mem);
  pktns_free(&conn->in_pktns, conn->mem);

  ngtcp2_default_cc_free(&conn->cc);

  ngtcp2_ringbuf_free(&conn->rx.path_challenge);

  ngtcp2_pv_del(conn->pv);

  ngtcp2_idtr_free(&conn->remote.uni.idtr);
  ngtcp2_idtr_free(&conn->remote.bidi.idtr);
  ngtcp2_pq_free(&conn->tx.strmq);
  ngtcp2_map_each_free(&conn->strms, delete_strms_each, (void *)conn->mem);
  ngtcp2_map_free(&conn->strms);

  ngtcp2_pq_free(&conn->scid.used);
  delete_scid(&conn->scid.set, conn->mem);
  ngtcp2_ksl_free(&conn->scid.set);
  ngtcp2_ringbuf_free(&conn->dcid.retired);
  ngtcp2_ringbuf_free(&conn->dcid.unused);
  ngtcp2_ringbuf_free(&conn->dcid.bound);

  ngtcp2_mem_free(conn->mem, conn);
}

/*
 * conn_ensure_ack_blks makes sure that |(*pfr)->ack.blks| can contain
 * at least |n| ngtcp2_ack_blk.  |*pfr| points to the ngtcp2_frame
 * object.  |*pnum_blks_max| is the number of ngtpc2_ack_blk which
 * |*pfr| can contain.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_ensure_ack_blks(ngtcp2_conn *conn, ngtcp2_frame **pfr,
                                size_t *pnum_blks_max, size_t n) {
  ngtcp2_frame *fr;

  if (n <= *pnum_blks_max) {
    return 0;
  }

  *pnum_blks_max *= 2;
  fr = ngtcp2_mem_realloc(conn->mem, *pfr,
                          sizeof(ngtcp2_ack) +
                              sizeof(ngtcp2_ack_blk) * (*pnum_blks_max - 1));
  if (fr == NULL) {
    return NGTCP2_ERR_NOMEM;
  }

  *pfr = fr;

  return 0;
}

/*
 * conn_compute_ack_delay computes ACK delay for outgoing protected
 * ACK.
 */
static ngtcp2_duration conn_compute_ack_delay(ngtcp2_conn *conn) {
  ngtcp2_duration initial_delay = conn->local.settings.max_ack_delay;

  if (conn->rcs.smoothed_rtt < 1e-9) {
    return initial_delay;
  }

  return ngtcp2_min(initial_delay,
                    (ngtcp2_duration)(conn->rcs.smoothed_rtt / 8));
}

/*
 * conn_create_ack_frame creates ACK frame, and assigns its pointer to
 * |*pfr| if there are any received packets to acknowledge.  If there
 * are no packets to acknowledge, this function returns 0, and |*pfr|
 * is untouched.  The caller is advised to set |*pfr| to NULL before
 * calling this function, and check it after this function returns.
 * If |nodelay| is nonzero, delayed ACK timer is ignored.
 *
 * The memory for ACK frame is dynamically allocated by this function.
 * A caller is responsible to free it.
 *
 * Call ngtcp2_acktr_commit_ack after a created ACK frame is
 * successfully serialized into a packet.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_create_ack_frame(ngtcp2_conn *conn, ngtcp2_frame **pfr,
                                 ngtcp2_acktr *acktr, ngtcp2_tstamp ts,
                                 uint64_t ack_delay,
                                 uint64_t ack_delay_exponent) {
  int64_t last_pkt_num;
  ngtcp2_ack_blk *blk;
  ngtcp2_ksl_it it;
  ngtcp2_acktr_entry *rpkt;
  ngtcp2_frame *fr;
  ngtcp2_ack *ack;
  /* TODO Measure an actual size of ACK bloks to find the best default
     value. */
  size_t num_blks_max = 8;
  size_t blk_idx;
  int rv;

  if (acktr->flags & NGTCP2_ACKTR_FLAG_IMMEDIATE_ACK) {
    ack_delay = 0;
  }

  if (!ngtcp2_acktr_require_active_ack(acktr, ack_delay, ts)) {
    return 0;
  }

  it = ngtcp2_acktr_get(acktr);
  if (ngtcp2_ksl_it_end(&it)) {
    ngtcp2_acktr_commit_ack(acktr);
    return 0;
  }

  fr = ngtcp2_mem_malloc(conn->mem, sizeof(ngtcp2_ack) +
                                        sizeof(ngtcp2_ack_blk) * num_blks_max);
  if (fr == NULL) {
    return NGTCP2_ERR_NOMEM;
  }

  ack = &fr->ack;

  rpkt = ngtcp2_ksl_it_get(&it);
  last_pkt_num = rpkt->pkt_num - (int64_t)(rpkt->len - 1);
  ack->type = NGTCP2_FRAME_ACK;
  ack->largest_ack = rpkt->pkt_num;
  ack->first_ack_blklen = rpkt->len - 1;
  ack->ack_delay_unscaled = ts - rpkt->tstamp;
  ack->ack_delay = ack->ack_delay_unscaled /
                   (NGTCP2_DURATION_TICK / NGTCP2_MICROSECONDS) /
                   (1UL << ack_delay_exponent);
  ack->num_blks = 0;

  ngtcp2_ksl_it_next(&it);

  for (; !ngtcp2_ksl_it_end(&it); ngtcp2_ksl_it_next(&it)) {
    rpkt = ngtcp2_ksl_it_get(&it);

    blk_idx = ack->num_blks++;
    rv = conn_ensure_ack_blks(conn, &fr, &num_blks_max, ack->num_blks);
    if (rv != 0) {
      ngtcp2_mem_free(conn->mem, fr);
      return rv;
    }
    ack = &fr->ack;
    blk = &ack->blks[blk_idx];
    blk->gap = (uint64_t)(last_pkt_num - rpkt->pkt_num - 2);
    blk->blklen = rpkt->len - 1;

    last_pkt_num = rpkt->pkt_num - (int64_t)(rpkt->len - 1);

    if (ack->num_blks == NGTCP2_MAX_ACK_BLKS) {
      break;
    }
  }

  /* TODO Just remove entries which cannot fit into a single ACK frame
     for now. */
  if (!ngtcp2_ksl_it_end(&it)) {
    ngtcp2_acktr_forget(acktr, ngtcp2_ksl_it_get(&it));
  }

  *pfr = fr;

  return 0;
}

/*
 * conn_ppe_write_frame writes |fr| to |ppe|.  If |hd_logged| is not
 * NULL and |*hd_logged| is zero, packet header is logged, and 1 is
 * assigned to |*hd_logged|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOBUF
 *     Buffer is too small.
 */
static int conn_ppe_write_frame_hd_log(ngtcp2_conn *conn, ngtcp2_ppe *ppe,
                                       int *hd_logged, const ngtcp2_pkt_hd *hd,
                                       ngtcp2_frame *fr) {
  int rv;

  rv = ngtcp2_ppe_encode_frame(ppe, fr);
  if (rv != 0) {
    assert(NGTCP2_ERR_NOBUF == rv);
    return rv;
  }

  if (hd_logged && !*hd_logged) {
    *hd_logged = 1;
    ngtcp2_log_tx_pkt_hd(&conn->log, hd);
  }

  ngtcp2_log_tx_fr(&conn->log, hd, fr);

  return 0;
}

/*
 * conn_ppe_write_frame writes |fr| to |ppe|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOBUF
 *     Buffer is too small.
 */
static int conn_ppe_write_frame(ngtcp2_conn *conn, ngtcp2_ppe *ppe,
                                const ngtcp2_pkt_hd *hd, ngtcp2_frame *fr) {
  return conn_ppe_write_frame_hd_log(conn, ppe, NULL, hd, fr);
}

/*
 * conn_on_pkt_sent is called when new retransmittable packet is sent.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory
 */
static int conn_on_pkt_sent(ngtcp2_conn *conn, ngtcp2_rtb *rtb,
                            ngtcp2_rtb_entry *ent) {
  int rv;

  /* This function implements OnPacketSent, but it handles only
     retransmittable packet (non-ACK only packet). */
  rv = ngtcp2_rtb_add(rtb, ent);
  if (rv != 0) {
    return rv;
  }

  if (ent->flags & NGTCP2_RTB_FLAG_CRYPTO_PKT) {
    assert(ngtcp2_pkt_handshake_pkt(&ent->hd));
    conn->rcs.last_hs_tx_pkt_ts = ent->ts;
  }
  if (ent->flags & NGTCP2_RTB_FLAG_ACK_ELICITING) {
    conn->rcs.last_tx_pkt_ts = ent->ts;
  }
  ngtcp2_conn_set_loss_detection_timer(conn);

  return 0;
}

/*
 * pktns_select_pkt_numlen selects shortest packet number encoding for
 * the next packet number based on the largest acknowledged packet
 * number.  It returns the number of bytes to encode the packet
 * number.
 */
static size_t pktns_select_pkt_numlen(ngtcp2_pktns *pktns) {
  int64_t pkt_num = pktns->tx.last_pkt_num + 1;
  ngtcp2_rtb *rtb = &pktns->rtb;
  int64_t n = pkt_num - rtb->largest_acked_tx_pkt_num;

  if (NGTCP2_MAX_PKT_NUM / 2 <= pkt_num) {
    return 4;
  }

  n = n * 2 + 1;

  if (n > 0xffffff) {
    return 4;
  }
  if (n > 0xffff) {
    return 3;
  }
  if (n > 0xff) {
    return 2;
  }
  return 1;
}

/*
 * conn_cwnd_left returns the number of bytes the local endpoint can
 * sent at this time.
 */
static uint64_t conn_cwnd_left(ngtcp2_conn *conn) {
  uint64_t bytes_in_flight = ngtcp2_conn_get_bytes_in_flight(conn);
  uint64_t cwnd =
      conn->pv && (conn->pv->flags & NGTCP2_PV_FLAG_FALLBACK_ON_FAILURE)
          ? NGTCP2_MIN_CWND
          : conn->ccs.cwnd;

  /* We might send more than bytes_in_flight if probe packets are
     involved. */
  if (bytes_in_flight >= cwnd) {
    return 0;
  }
  return cwnd - bytes_in_flight;
}

/*
 * conn_retry_early_payloadlen returns the estimated wire length of
 * the first STREAM frame of 0-RTT packet which should be
 * retransmitted due to Retry frame
 */
static size_t conn_retry_early_payloadlen(ngtcp2_conn *conn) {
  ngtcp2_stream_frame_chain *sfrc;
  ngtcp2_strm *strm;

  for (; !ngtcp2_pq_empty(&conn->tx.strmq);) {
    strm = ngtcp2_conn_tx_strmq_top(conn);
    if (ngtcp2_strm_streamfrq_empty(strm)) {
      ngtcp2_conn_tx_strmq_pop(conn);
      continue;
    }

    sfrc = ngtcp2_strm_streamfrq_top(strm);
    return ngtcp2_vec_len(sfrc->fr.data, sfrc->fr.datacnt) +
           NGTCP2_STREAM_OVERHEAD;
  }

  return 0;
}

/*
 * conn_cryptofrq_top returns the element which sits on top of the
 * queue.  The queue must not be empty.
 */
static ngtcp2_crypto_frame_chain *conn_cryptofrq_top(ngtcp2_conn *conn,
                                                     ngtcp2_pktns *pktns) {
  (void)conn;
  assert(!ngtcp2_pq_empty(&pktns->crypto.tx.frq));
  return ngtcp2_struct_of(ngtcp2_pq_top(&pktns->crypto.tx.frq),
                          ngtcp2_crypto_frame_chain, pe);
}

static int conn_cryptofrq_unacked_pop(ngtcp2_conn *conn, ngtcp2_pktns *pktns,
                                      ngtcp2_crypto_frame_chain **pfrc) {
  ngtcp2_crypto_frame_chain *frc, *nfrc;
  ngtcp2_crypto *fr, *nfr;
  uint64_t offset, end_offset;
  size_t idx, end_idx;
  size_t base_offset, end_base_offset;
  ngtcp2_ksl_it gapit;
  ngtcp2_range gap;
  ngtcp2_rtb *rtb = &pktns->rtb;
  ngtcp2_vec *v;
  int rv;

  *pfrc = NULL;

  for (; !ngtcp2_pq_empty(&pktns->crypto.tx.frq);) {
    frc = ngtcp2_struct_of(ngtcp2_pq_top(&pktns->crypto.tx.frq),
                           ngtcp2_crypto_frame_chain, pe);

    ngtcp2_pq_pop(&pktns->crypto.tx.frq);
    frc->pe.index = NGTCP2_PQ_BAD_INDEX;

    fr = &frc->fr;
    idx = 0;
    offset = fr->offset;
    base_offset = 0;

    gapit =
        ngtcp2_gaptr_get_first_gap_after(&rtb->crypto->tx.acked_offset, offset);
    gap = *(ngtcp2_range *)ngtcp2_ksl_it_key(&gapit).ptr;
    if (gap.begin < offset) {
      gap.begin = offset;
    }

    for (; idx < fr->datacnt && offset < gap.begin; ++idx) {
      v = &fr->data[idx];
      if (offset + v->len > gap.begin) {
        base_offset = gap.begin - offset;
        break;
      }

      offset += v->len;
    }

    if (idx == fr->datacnt) {
      ngtcp2_crypto_frame_chain_del(frc, conn->mem);
      continue;
    }

    assert(gap.begin == offset + base_offset);

    end_idx = idx;
    end_offset = offset;
    end_base_offset = 0;

    for (; end_idx < fr->datacnt; ++end_idx) {
      v = &fr->data[end_idx];
      if (end_offset + v->len > gap.end) {
        end_base_offset = gap.end - end_offset;
        break;
      }

      end_offset += v->len;
    }

    if (fr->offset == offset && base_offset == 0 && fr->datacnt == end_idx) {
      *pfrc = frc;
      return 0;
    }

    if (fr->datacnt == end_idx) {
      memmove(fr->data, fr->data + idx, sizeof(fr->data[0]) * (end_idx - idx));

      assert(fr->data[0].len > base_offset);

      fr->offset = offset + base_offset;
      fr->datacnt = end_idx - idx;
      fr->data[0].base += base_offset;
      fr->data[0].len -= base_offset;

      *pfrc = frc;
      return 0;
    }

    rv = ngtcp2_crypto_frame_chain_new(&nfrc, conn->mem);
    if (rv != 0) {
      ngtcp2_crypto_frame_chain_del(frc, conn->mem);
      return rv;
    }

    nfr = &nfrc->fr;
    memcpy(nfr->data, fr->data + end_idx,
           sizeof(nfr->data[0]) * (fr->datacnt - end_idx));

    assert(nfr->data[0].len > end_base_offset);

    nfr->offset = end_offset + end_base_offset;
    nfr->datacnt = fr->datacnt - end_idx;
    nfr->data[0].base += end_base_offset;
    nfr->data[0].len -= end_base_offset;

    rv = ngtcp2_pq_push(&pktns->crypto.tx.frq, &nfrc->pe);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal(rv));
      ngtcp2_crypto_frame_chain_del(nfrc, conn->mem);
      ngtcp2_crypto_frame_chain_del(frc, conn->mem);
      return rv;
    }

    memmove(fr->data, fr->data + idx, sizeof(fr->data[0]) * (end_idx - idx));

    assert(fr->data[0].len > base_offset);

    fr->offset = offset + base_offset;
    fr->datacnt = end_idx - idx;
    fr->data[0].base += base_offset;
    fr->data[0].len -= base_offset;

    *pfrc = frc;
    return 0;
  }

  return 0;
}
static int conn_cryptofrq_pop(ngtcp2_conn *conn,
                              ngtcp2_crypto_frame_chain **pfrc,
                              ngtcp2_pktns *pktns, size_t left) {
  ngtcp2_crypto *fr, *nfr;
  ngtcp2_crypto_frame_chain *frc, *nfrc;
  int rv;
  ssize_t nsplit;
  size_t nmerged;
  size_t datalen;

  rv = conn_cryptofrq_unacked_pop(conn, pktns, &frc);
  if (rv != 0) {
    return rv;
  }
  if (frc == NULL) {
    *pfrc = NULL;
    return 0;
  }

  fr = &frc->fr;

  datalen = ngtcp2_vec_len(fr->data, fr->datacnt);
  if (datalen > left) {
    if (!ngtcp2_pq_empty(&pktns->crypto.tx.frq)) {
      nfrc = ngtcp2_struct_of(ngtcp2_pq_top(&pktns->crypto.tx.frq),
                              ngtcp2_crypto_frame_chain, pe);
      nfr = &nfrc->fr;

      if (fr->offset + datalen == nfr->offset) {
        nsplit =
            ngtcp2_vec_split(fr->data, &fr->datacnt, nfr->data, &nfr->datacnt,
                             left, NGTCP2_MAX_CRYPTO_DATACNT);
        assert(nsplit);

        if (nsplit > 0) {
          ngtcp2_pq_pop(&pktns->crypto.tx.frq);
          nfr->offset -= (size_t)nsplit;

          rv = ngtcp2_pq_push(&pktns->crypto.tx.frq, &nfrc->pe);
          if (rv != 0) {
            assert(ngtcp2_err_is_fatal(rv));
            ngtcp2_crypto_frame_chain_del(nfrc, conn->mem);
            ngtcp2_crypto_frame_chain_del(frc, conn->mem);
            return rv;
          }

          *pfrc = frc;

          return 0;
        }
      }
    }

    rv = ngtcp2_crypto_frame_chain_new(&nfrc, conn->mem);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal(rv));
      ngtcp2_crypto_frame_chain_del(frc, conn->mem);
      return rv;
    }

    nfr = &nfrc->fr;
    nfr->type = NGTCP2_FRAME_CRYPTO;
    nfr->offset = fr->offset + left;
    nfr->datacnt = 0;

    ngtcp2_vec_split(fr->data, &fr->datacnt, nfr->data, &nfr->datacnt, left,
                     NGTCP2_MAX_CRYPTO_DATACNT);

    rv = ngtcp2_pq_push(&pktns->crypto.tx.frq, &nfrc->pe);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal(rv));
      ngtcp2_crypto_frame_chain_del(nfrc, conn->mem);
      ngtcp2_crypto_frame_chain_del(frc, conn->mem);
      return rv;
    }

    *pfrc = frc;

    return 0;
  }

  if (fr->datacnt == NGTCP2_MAX_CRYPTO_DATACNT) {
    *pfrc = frc;
    return 0;
  }

  left -= datalen;

  for (; left && fr->datacnt < NGTCP2_MAX_CRYPTO_DATACNT &&
         !ngtcp2_pq_empty(&pktns->crypto.tx.frq);) {
    nfrc = ngtcp2_struct_of(ngtcp2_pq_top(&pktns->crypto.tx.frq),
                            ngtcp2_crypto_frame_chain, pe);
    nfr = &nfrc->fr;

    if (nfr->offset != fr->offset + datalen) {
      assert(fr->offset + datalen < nfr->offset);
      break;
    }

    rv = conn_cryptofrq_unacked_pop(conn, pktns, &nfrc);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal(rv));
      ngtcp2_crypto_frame_chain_del(frc, conn->mem);
      return rv;
    }

    nmerged = ngtcp2_vec_merge(fr->data, &fr->datacnt, nfr->data, &nfr->datacnt,
                               left, NGTCP2_MAX_CRYPTO_DATACNT);
    if (nmerged == 0) {
      rv = ngtcp2_pq_push(&pktns->crypto.tx.frq, &nfrc->pe);
      if (rv != 0) {
        assert(ngtcp2_err_is_fatal(rv));
        ngtcp2_crypto_frame_chain_del(nfrc, conn->mem);
        ngtcp2_crypto_frame_chain_del(frc, conn->mem);
        return rv;
      }
      break;
    }

    datalen += nmerged;
    nfr->offset += nmerged;
    left -= nmerged;

    if (nfr->datacnt == 0) {
      ngtcp2_crypto_frame_chain_del(nfrc, conn->mem);
      continue;
    }

    rv = ngtcp2_pq_push(&pktns->crypto.tx.frq, &nfrc->pe);
    if (rv != 0) {
      ngtcp2_crypto_frame_chain_del(nfrc, conn->mem);
      ngtcp2_crypto_frame_chain_del(frc, conn->mem);
      return rv;
    }
  }

  *pfrc = frc;
  return 0;
}

/*
 * conn_verify_dcid verifies that destination connection ID in |hd| is
 * valid for the connection.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_INVALID_ARGUMENT
 *     |dcid| is not known to the local endpoint.
 */
static int conn_verify_dcid(ngtcp2_conn *conn, const ngtcp2_pkt_hd *hd) {
  ngtcp2_ksl_key key;
  ngtcp2_ksl_it it;
  ngtcp2_scid *scid;
  int rv;

  it = ngtcp2_ksl_lower_bound(&conn->scid.set,
                              ngtcp2_ksl_key_ptr(&key, &hd->dcid));
  if (ngtcp2_ksl_it_end(&it)) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  scid = ngtcp2_ksl_it_get(&it);
  if (!ngtcp2_cid_eq(&scid->cid, &hd->dcid)) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  if (!(scid->flags & NGTCP2_SCID_FLAG_USED)) {
    scid->flags |= NGTCP2_SCID_FLAG_USED;

    if (scid->pe.index == NGTCP2_PQ_BAD_INDEX) {
      rv = ngtcp2_pq_push(&conn->scid.used, &scid->pe);
      if (rv != 0) {
        return rv;
      }
    }
  }

  return 0;
}

/*
 * conn_should_pad_pkt returns nonzero if the packet should be padded.
 * |type| is the type of packet.  |left| is the space left in packet
 * buffer.  |early_datalen| is the number of bytes which will be sent
 * in the next, coalesced 0-RTT packet.
 */
static int conn_should_pad_pkt(ngtcp2_conn *conn, uint8_t type, size_t left,
                               size_t early_datalen) {
  size_t min_payloadlen;

  if (conn->server || conn->hs_pktns.crypto.tx.ckm) {
    return 0;
  }

  if (type != NGTCP2_PKT_INITIAL) {
    return 0;
  }

  if (!conn->early.ckm || early_datalen == 0) {
    return 1;
  }
  min_payloadlen = ngtcp2_min(early_datalen, 128);

  return left <
         /* TODO Assuming that pkt_num is encoded in 1 byte. */
         NGTCP2_MIN_LONG_HEADERLEN + conn->dcid.current.cid.datalen +
             conn->oscid.datalen + 1 /* payloadlen bytes - 1 */ +
             min_payloadlen + NGTCP2_MAX_AEAD_OVERHEAD;
}

/*
 * conn_write_handshake_pkt writes handshake packet in the buffer
 * pointed by |dest| whose length is |destlen|.  |type| specifies long
 * packet type.  It should be either NGTCP2_PKT_INITIAL or
 * NGTCP2_PKT_HANDSHAKE_PKT.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 */
static ssize_t conn_write_handshake_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                        size_t destlen, uint8_t type,
                                        size_t early_datalen,
                                        ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_ppe ppe;
  ngtcp2_pkt_hd hd;
  ngtcp2_frame_chain *frq = NULL, **pfrc = &frq;
  ngtcp2_crypto_frame_chain *ncfrc;
  ngtcp2_frame *ackfr = NULL, lfr;
  ssize_t spktlen;
  ngtcp2_crypto_ctx ctx;
  ngtcp2_rtb_entry *rtbent;
  ngtcp2_pktns *pktns;
  size_t left;
  uint8_t flags = NGTCP2_RTB_FLAG_NONE;
  int pkt_empty = 1;
  int padded = 0;
  int hd_logged = 0;

  switch (type) {
  case NGTCP2_PKT_INITIAL:
    if (!conn->in_pktns.crypto.tx.ckm) {
      return 0;
    }
    pktns = &conn->in_pktns;
    ctx.ckm = pktns->crypto.tx.ckm;
    ctx.hp = pktns->crypto.tx.hp;
    ctx.aead_overhead = NGTCP2_INITIAL_AEAD_OVERHEAD;
    ctx.encrypt = conn->callbacks.in_encrypt;
    ctx.hp_mask = conn->callbacks.in_hp_mask;
    break;
  case NGTCP2_PKT_HANDSHAKE:
    if (!conn->hs_pktns.crypto.tx.ckm) {
      return 0;
    }
    pktns = &conn->hs_pktns;
    ctx.ckm = pktns->crypto.tx.ckm;
    ctx.hp = pktns->crypto.tx.hp;
    ctx.aead_overhead = conn->crypto.aead_overhead;
    ctx.encrypt = conn->callbacks.encrypt;
    ctx.hp_mask = conn->callbacks.hp_mask;
    ctx.user_data = conn;
    break;
  default:
    assert(0);
  }

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_LONG_FORM, type,
                     &conn->dcid.current.cid, &conn->oscid,
                     pktns->tx.last_pkt_num + 1, pktns_select_pkt_numlen(pktns),
                     conn->version, 0);

  if (type == NGTCP2_PKT_INITIAL && ngtcp2_buf_len(&conn->token)) {
    hd.token = conn->token.pos;
    hd.tokenlen = ngtcp2_buf_len(&conn->token);
  }

  ctx.user_data = conn;

  if (ngtcp2_pq_empty(&pktns->crypto.tx.frq)) {
    return 0;
  }

  ngtcp2_ppe_init(&ppe, dest, destlen, &ctx);

  rv = ngtcp2_ppe_encode_hd(&ppe, &hd);
  if (rv != 0) {
    assert(NGTCP2_ERR_NOBUF == rv);
    return 0;
  }

  if (!ngtcp2_ppe_ensure_hp_sample(&ppe)) {
    return 0;
  }

  for (; !ngtcp2_pq_empty(&pktns->crypto.tx.frq);) {
    left = ngtcp2_ppe_left(&ppe);
    left = ngtcp2_pkt_crypto_max_datalen(
        conn_cryptofrq_top(conn, pktns)->fr.offset, left, left);

    if (left == (size_t)-1) {
      break;
    }

    rv = conn_cryptofrq_pop(conn, &ncfrc, pktns, left);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal(rv));
      return rv;
    }

    if (ncfrc == NULL) {
      break;
    }

    rv = conn_ppe_write_frame_hd_log(conn, &ppe, &hd_logged, &hd,
                                     &ncfrc->frc.fr);
    if (rv != 0) {
      assert(0);
    }

    *pfrc = &ncfrc->frc;
    pfrc = &(*pfrc)->next;

    pkt_empty = 0;
    flags |= NGTCP2_RTB_FLAG_ACK_ELICITING | NGTCP2_RTB_FLAG_CRYPTO_PKT;
  }

  if (pkt_empty) {
    return 0;
  }

  rv = conn_create_ack_frame(conn, &ackfr, &pktns->acktr, ts, 0 /* ack_delay */,
                             NGTCP2_DEFAULT_ACK_DELAY_EXPONENT);
  if (rv != 0) {
    return rv;
  }

  if (ackfr) {
    rv = conn_ppe_write_frame_hd_log(conn, &ppe, &hd_logged, &hd, ackfr);
    if (rv != 0) {
      assert(NGTCP2_ERR_NOBUF == rv);
    } else {
      ngtcp2_acktr_commit_ack(&pktns->acktr);
      ngtcp2_acktr_add_ack(&pktns->acktr, hd.pkt_num, ackfr->ack.largest_ack);
      pkt_empty = 0;
    }
    ngtcp2_mem_free(conn->mem, ackfr);
    ackfr = NULL;
  }

  /* If we cannot write another packet, then we need to add padding to
     Initial here. */
  if (conn_should_pad_pkt(conn, type, ngtcp2_ppe_left(&ppe), early_datalen)) {
    lfr.type = NGTCP2_FRAME_PADDING;
    lfr.padding.len = ngtcp2_ppe_padding(&ppe);
    if (lfr.padding.len > 0) {
      ngtcp2_log_tx_fr(&conn->log, &hd, &lfr);
    }
    padded = 1;
  } else {
    lfr.type = NGTCP2_FRAME_PADDING;
    lfr.padding.len = ngtcp2_ppe_padding_hp_sample(&ppe);
    if (lfr.padding.len) {
      ngtcp2_log_tx_fr(&conn->log, &hd, &lfr);
    }
  }

  spktlen = ngtcp2_ppe_final(&ppe, NULL);
  if (spktlen < 0) {
    assert(ngtcp2_err_is_fatal((int)spktlen));
    return spktlen;
  }

  if (*pfrc != frq || padded) {
    rv = ngtcp2_rtb_entry_new(&rtbent, &hd, frq, ts, (size_t)spktlen, flags,
                              conn->mem);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal(rv));
      ngtcp2_frame_chain_list_del(frq, conn->mem);
      return rv;
    }

    rv = conn_on_pkt_sent(conn, &pktns->rtb, rtbent);
    if (rv != 0) {
      ngtcp2_rtb_entry_del(rtbent, conn->mem);
      return rv;
    }
  }

  ++pktns->tx.last_pkt_num;

  return spktlen;
}

/*
 * conn_write_handshake_ack_pkt writes unprotected QUIC packet in the
 * buffer pointed by |dest| whose length is |destlen|.  The packet
 * only includes ACK frame if any ack is required.
 *
 * This function might send PADDING only packet if it has no ACK frame
 * to send under certain condition.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static ssize_t conn_write_handshake_ack_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                            size_t destlen, uint8_t type,
                                            int require_padding,
                                            ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_ppe ppe;
  ngtcp2_pkt_hd hd;
  ngtcp2_frame *ackfr, lfr;
  ngtcp2_crypto_ctx ctx;
  ngtcp2_pktns *pktns;
  ngtcp2_rtb_entry *rtbent;
  ssize_t spktlen;
  int force_send;
  int immediate_ack = 0;

  switch (type) {
  case NGTCP2_PKT_INITIAL:
    pktns = &conn->in_pktns;
    ctx.aead_overhead = NGTCP2_INITIAL_AEAD_OVERHEAD;
    ctx.encrypt = conn->callbacks.in_encrypt;
    ctx.hp_mask = conn->callbacks.in_hp_mask;
    immediate_ack = conn->hs_pktns.crypto.tx.ckm != NULL;
    break;
  case NGTCP2_PKT_HANDSHAKE:
    pktns = &conn->hs_pktns;
    ctx.aead_overhead = conn->crypto.aead_overhead;
    ctx.encrypt = conn->callbacks.encrypt;
    ctx.hp_mask = conn->callbacks.hp_mask;
    immediate_ack = conn->pktns.crypto.tx.ckm != NULL;
    break;
  default:
    assert(0);
  }

  if (!pktns->crypto.tx.ckm) {
    return 0;
  }

  force_send = (conn->flags & NGTCP2_CONN_FLAG_FORCE_SEND_HANDSHAKE);

  if (immediate_ack) {
    ngtcp2_acktr_immediate_ack(&pktns->acktr);
  }

  ackfr = NULL;
  rv = conn_create_ack_frame(conn, &ackfr, &pktns->acktr, ts,
                             NGTCP2_HS_ACK_DELAY,
                             NGTCP2_DEFAULT_ACK_DELAY_EXPONENT);
  if (rv != 0) {
    return rv;
  }
  if (!ackfr && !force_send) {
    return 0;
  }

  ngtcp2_pkt_hd_init(&hd, NGTCP2_PKT_FLAG_LONG_FORM, type,
                     &conn->dcid.current.cid, &conn->oscid,
                     pktns->tx.last_pkt_num + 1, pktns_select_pkt_numlen(pktns),
                     conn->version, 0);

  ctx.ckm = pktns->crypto.tx.ckm;
  ctx.hp = pktns->crypto.tx.hp;
  ctx.user_data = conn;

  ngtcp2_ppe_init(&ppe, dest, destlen, &ctx);

  rv = ngtcp2_ppe_encode_hd(&ppe, &hd);
  if (rv != 0) {
    assert(NGTCP2_ERR_NOBUF == rv);
    ngtcp2_mem_free(conn->mem, ackfr);
    return 0;
  }

  if (!ngtcp2_ppe_ensure_hp_sample(&ppe)) {
    ngtcp2_mem_free(conn->mem, ackfr);
    return 0;
  }

  ngtcp2_log_tx_pkt_hd(&conn->log, &hd);

  if (ackfr) {
    rv = conn_ppe_write_frame(conn, &ppe, &hd, ackfr);
    if (rv != 0) {
      assert(NGTCP2_ERR_NOBUF == rv);
    } else {
      ngtcp2_acktr_commit_ack(&pktns->acktr);
      ngtcp2_acktr_add_ack(&pktns->acktr, hd.pkt_num, ackfr->ack.largest_ack);
    }
    ngtcp2_mem_free(conn->mem, ackfr);
    ackfr = NULL;
  }

  if (require_padding || force_send) {
    lfr.type = NGTCP2_FRAME_PADDING;
    lfr.padding.len = ngtcp2_ppe_padding(&ppe);
    if (lfr.padding.len > 0) {
      ngtcp2_log_tx_fr(&conn->log, &hd, &lfr);
    }

    spktlen = ngtcp2_ppe_final(&ppe, NULL);
    if (spktlen < 0) {
      return spktlen;
    }

    rv = ngtcp2_rtb_entry_new(&rtbent, &hd, NULL, ts, (size_t)spktlen,
                              NGTCP2_RTB_FLAG_NONE, conn->mem);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal(rv));
      return rv;
    }

    rv = conn_on_pkt_sent(conn, &pktns->rtb, rtbent);
    if (rv != 0) {
      ngtcp2_rtb_entry_del(rtbent, conn->mem);
      return rv;
    }
  } else {
    lfr.type = NGTCP2_FRAME_PADDING;
    lfr.padding.len = ngtcp2_ppe_padding_hp_sample(&ppe);
    if (lfr.padding.len) {
      ngtcp2_log_tx_fr(&conn->log, &hd, &lfr);
    }

    spktlen = ngtcp2_ppe_final(&ppe, NULL);
    if (spktlen < 0) {
      return spktlen;
    }
  }

  conn->flags &= (uint16_t)~NGTCP2_CONN_FLAG_FORCE_SEND_HANDSHAKE;

  ++pktns->tx.last_pkt_num;

  return spktlen;
}

/*
 * conn_write_handshake_ack_pkts writes packets which contain ACK
 * frame only.  This function writes at most 2 packets for each
 * Initial and Handshake packet.
 */
static ssize_t conn_write_handshake_ack_pkts(ngtcp2_conn *conn, uint8_t *dest,
                                             size_t destlen,
                                             int require_padding,
                                             ngtcp2_tstamp ts) {
  ssize_t res = 0, nwrite = 0;

  if (conn->hs_pktns.crypto.tx.ckm) {
    nwrite =
        conn_write_handshake_ack_pkt(conn, dest, destlen, NGTCP2_PKT_HANDSHAKE,
                                     /* require_padding = */ 0, ts);
    if (nwrite < 0) {
      assert(nwrite != NGTCP2_ERR_NOBUF);
      return nwrite;
    }

    res += nwrite;
    dest += nwrite;
    destlen -= (size_t)nwrite;
  }

  require_padding = require_padding && res == 0;

  if (require_padding) {
    /* PADDING frame counts toward bytes_in_flight, thus destlen is
       constrained to cwnd */
    destlen = ngtcp2_min(destlen, conn_cwnd_left(conn));
  }

  nwrite = conn_write_handshake_ack_pkt(conn, dest, destlen, NGTCP2_PKT_INITIAL,
                                        require_padding, ts);
  if (nwrite < 0) {
    assert(nwrite != NGTCP2_ERR_NOBUF);
    return nwrite;
  }

  return res + nwrite;
}

/*
 * conn_write_client_initial writes Initial packet in the buffer
 * pointed by |dest| whose length is |destlen|.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 */
static ssize_t conn_write_client_initial(ngtcp2_conn *conn, uint8_t *dest,
                                         size_t destlen, size_t early_datalen,
                                         ngtcp2_tstamp ts) {
  int rv;

  rv = conn->callbacks.client_initial(conn, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return conn_write_handshake_pkt(conn, dest, destlen, NGTCP2_PKT_INITIAL,
                                  early_datalen, ts);
}

/*
 * conn_write_handshake_pkts writes Initial and Handshake packets in
 * the buffer pointed by |dest| whose length is |destlen|.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 */
static ssize_t conn_write_handshake_pkts(ngtcp2_conn *conn, uint8_t *dest,
                                         size_t destlen, size_t early_datalen,
                                         ngtcp2_tstamp ts) {
  ssize_t nwrite;
  ssize_t res = 0;

  nwrite = conn_write_handshake_pkt(conn, dest, destlen, NGTCP2_PKT_INITIAL,
                                    early_datalen, ts);
  if (nwrite < 0) {
    assert(nwrite != NGTCP2_ERR_NOBUF);
    return nwrite;
  }

  res += nwrite;
  dest += nwrite;
  destlen -= (size_t)nwrite;

  nwrite = conn_write_handshake_pkt(conn, dest, destlen, NGTCP2_PKT_HANDSHAKE,
                                    0, ts);
  if (nwrite < 0) {
    assert(nwrite != NGTCP2_ERR_NOBUF);
    return nwrite;
  }

  res += nwrite;
  dest += nwrite;
  destlen -= (size_t)nwrite;

  return res;
}

static ssize_t conn_write_protected_ack_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                            size_t destlen, ngtcp2_tstamp ts);

static ssize_t conn_write_server_handshake(ngtcp2_conn *conn, uint8_t *dest,
                                           size_t destlen, ngtcp2_tstamp ts) {
  ssize_t nwrite;
  ssize_t res = 0;

  nwrite = conn_write_handshake_pkts(conn, dest, destlen, 0, ts);
  if (nwrite < 0) {
    assert(nwrite != NGTCP2_ERR_NOBUF);
    return nwrite;
  }

  res += nwrite;
  dest += nwrite;
  destlen -= (size_t)nwrite;

  /* Acknowledge 0-RTT packet here. */
  if (conn->pktns.crypto.tx.ckm) {
    nwrite = conn_write_protected_ack_pkt(conn, dest, destlen, ts);
    if (nwrite < 0) {
      assert(nwrite != NGTCP2_ERR_NOBUF);
      return nwrite;
    }

    res += nwrite;
    dest += nwrite;
    destlen -= (size_t)nwrite;
  }

  return res;
}

/*
 * conn_initial_stream_rx_offset returns the initial maximum offset of
 * data for a stream denoted by |stream_id|.
 */
static uint64_t conn_initial_stream_rx_offset(ngtcp2_conn *conn,
                                              int64_t stream_id) {
  int local_stream = conn_local_stream(conn, stream_id);

  if (bidi_stream(stream_id)) {
    if (local_stream) {
      return conn->local.settings.max_stream_data_bidi_local;
    }
    return conn->local.settings.max_stream_data_bidi_remote;
  }

  if (local_stream) {
    return 0;
  }
  return conn->local.settings.max_stream_data_uni;
}

/*
 * conn_should_send_max_stream_data returns nonzero if MAX_STREAM_DATA
 * frame should be send for |strm|.
 */
static int conn_should_send_max_stream_data(ngtcp2_conn *conn,
                                            ngtcp2_strm *strm) {
  return conn_initial_stream_rx_offset(conn, strm->stream_id) / 2 <
         strm->rx.unsent_max_offset - strm->rx.max_offset;
}

/*
 * conn_should_send_max_data returns nonzero if MAX_DATA frame should
 * be sent.
 */
static int conn_should_send_max_data(ngtcp2_conn *conn) {
  return conn->local.settings.max_data / 2 <
             conn->rx.unsent_max_offset - conn->rx.max_offset ||
         2 * conn->rx.bw.value * conn->rcs.smoothed_rtt >=
             conn->rx.max_offset - conn->rx.offset;
}

/*
 * conn_required_num_new_connection_id returns the number of
 * additional connection ID the local endpoint has to provide to the
 * remote endpoint.
 */
static size_t conn_required_num_new_connection_id(ngtcp2_conn *conn) {
  size_t n = ngtcp2_ksl_len(&conn->scid.set) - ngtcp2_pq_size(&conn->scid.used);

  return n < NGTCP2_MIN_SCID_POOL_SIZE ? NGTCP2_MIN_SCID_POOL_SIZE - n : 0;
}

/*
 * conn_enqueue_new_connection_id generates additional connection IDs
 * and prepares to send them to the remote endpoint.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 */
static int conn_enqueue_new_connection_id(ngtcp2_conn *conn) {
  size_t i, need = conn_required_num_new_connection_id(conn);
  size_t cidlen = conn->oscid.datalen;
  ngtcp2_cid cid;
  uint64_t seq;
  int rv;
  uint8_t token[NGTCP2_STATELESS_RESET_TOKENLEN];
  ngtcp2_frame_chain *nfrc;
  ngtcp2_pktns *pktns = &conn->pktns;
  ngtcp2_scid *scid;
  ngtcp2_ksl_key key;
  ngtcp2_ksl_it it;

  for (i = 0; i < need; ++i) {
    rv = conn_call_get_new_connection_id(conn, &cid, token, cidlen);
    if (rv != 0) {
      return rv;
    }

    if (cid.datalen != cidlen) {
      return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    /* Assert uniqueness */
    it =
        ngtcp2_ksl_lower_bound(&conn->scid.set, ngtcp2_ksl_key_ptr(&key, &cid));
    if (!ngtcp2_ksl_it_end(&it) &&
        ngtcp2_cid_eq(ngtcp2_ksl_it_key(&it).ptr, &cid)) {
      return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    seq = ++conn->scid.last_seq;

    scid = ngtcp2_mem_malloc(conn->mem, sizeof(*scid));
    if (scid == NULL) {
      return NGTCP2_ERR_NOMEM;
    }

    ngtcp2_scid_init(scid, seq, &cid, token);

    rv = ngtcp2_ksl_insert(&conn->scid.set, NULL,
                           ngtcp2_ksl_key_ptr(&key, &scid->cid), scid);
    if (rv != 0) {
      ngtcp2_mem_free(conn->mem, scid);
      return rv;
    }

    rv = ngtcp2_frame_chain_new(&nfrc, conn->mem);
    if (rv != 0) {
      return rv;
    }

    nfrc->fr.type = NGTCP2_FRAME_NEW_CONNECTION_ID;
    nfrc->fr.new_connection_id.seq = seq;
    nfrc->fr.new_connection_id.cid = cid;
    memcpy(nfrc->fr.new_connection_id.stateless_reset_token, token,
           sizeof(token));
    nfrc->next = pktns->tx.frq;
    pktns->tx.frq = nfrc;
  }

  return 0;
}

/*
 * conn_compute_pto computes the current PTO.
 */
static ngtcp2_duration conn_compute_pto(ngtcp2_conn *conn) {
  ngtcp2_rcvry_stat *rcs = &conn->rcs;
  double var = ngtcp2_max(4 * rcs->rttvar, NGTCP2_GRANULARITY);
  ngtcp2_duration max_ack_delay =
      (conn->flags & NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED)
          ? conn->remote.settings.max_ack_delay
          : NGTCP2_DEFAULT_MAX_ACK_DELAY;
  ngtcp2_duration timeout =
      (ngtcp2_duration)(rcs->smoothed_rtt + var + (double)max_ack_delay);
  timeout *= (ngtcp2_duration)(1ULL << rcs->pto_count);

  return timeout;
}

/*
 * conn_remove_retired_connection_id removes the already retired
 * connection ID.  It waits RTT * 2 before actually removing a
 * connection ID after it receives RETIRE_CONNECTION_ID from peer to
 * catch reordered packets.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 */
static int conn_remove_retired_connection_id(ngtcp2_conn *conn,
                                             ngtcp2_tstamp ts) {
  ngtcp2_duration timeout;
  ngtcp2_scid *scid;
  ngtcp2_ksl_key key;
  ngtcp2_dcid *dcid;
  int rv;

  timeout = conn_compute_pto(conn);
  timeout =
      ngtcp2_max(timeout, (ngtcp2_duration)(6ULL * NGTCP2_DEFAULT_INITIAL_RTT));

  for (; !ngtcp2_pq_empty(&conn->scid.used);) {
    scid = ngtcp2_struct_of(ngtcp2_pq_top(&conn->scid.used), ngtcp2_scid, pe);

    if (scid->ts_retired == UINT64_MAX || scid->ts_retired + timeout >= ts) {
      return 0;
    }

    assert(scid->flags & NGTCP2_SCID_FLAG_RETIRED);

    rv = conn_call_remove_connection_id(conn, &scid->cid);
    if (rv != 0) {
      return rv;
    }

    ngtcp2_ksl_remove(&conn->scid.set, NULL,
                      ngtcp2_ksl_key_ptr(&key, &scid->cid));
    ngtcp2_pq_pop(&conn->scid.used);
    ngtcp2_mem_free(conn->mem, scid);
  }

  for (; ngtcp2_ringbuf_len(&conn->dcid.retired);) {
    dcid = ngtcp2_ringbuf_get(&conn->dcid.retired, 0);
    if (dcid->ts_retired + timeout >= ts) {
      break;
    }
    ngtcp2_ringbuf_pop_front(&conn->dcid.retired);
  }

  return 0;
}

/*
 * conn_write_pkt writes a protected packet in the buffer pointed by
 * |dest| whose length if |destlen|.  |type| specifies the type of
 * packet.  It can be NGTCP2_PKT_SHORT or NGTCP2_PKT_0RTT.
 *
 * This function can send new stream data.  In order to send stream
 * data, specify the underlying stream to |data_strm|.  If |fin| is
 * set to nonzero, it signals that the given data is the final portion
 * of the stream.  |datav| vector of length |datavcnt| specify stream
 * data to send.  If no stream data to send, set |strm| to NULL.  The
 * number of bytes sent to the stream is assigned to |*pdatalen|.  If
 * 0 length STREAM data is sent, 0 is assigned to |*pdatalen|.  The
 * caller should initialize |*pdatalen| to -1.
 *
 * If |require_padding| is nonzero, padding bytes are added to occupy
 * the remaining packet payload.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_STREAM_DATA_BLOCKED
 *     Stream data could not be written because of flow control.
 */
static ssize_t conn_write_pkt(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                              ssize_t *pdatalen, uint8_t type,
                              ngtcp2_strm *data_strm, uint8_t fin,
                              const ngtcp2_vec *datav, size_t datavcnt,
                              int require_padding, ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_ppe ppe;
  ngtcp2_pkt_hd hd;
  ngtcp2_frame *ackfr = NULL, lfr;
  ssize_t nwrite;
  ngtcp2_crypto_ctx ctx;
  ngtcp2_frame_chain **pfrc, *nfrc, *frc;
  ngtcp2_stream_frame_chain *nsfrc;
  ngtcp2_crypto_frame_chain *ncfrc;
  ngtcp2_rtb_entry *ent;
  ngtcp2_strm *strm;
  int pkt_empty = 1;
  size_t ndatalen = 0;
  int send_stream = 0;
  int stream_blocked = 0;
  ngtcp2_pktns *pktns = &conn->pktns;
  size_t left;
  int64_t written_stream_id = -1;
  size_t datalen = ngtcp2_vec_len(datav, datavcnt);
  uint8_t rtb_entry_flags = NGTCP2_RTB_FLAG_NONE;
  int hd_logged = 0;
  ngtcp2_path_challenge_entry *pcent;
  uint8_t hd_flags;

  switch (type) {
  case NGTCP2_PKT_SHORT:
    hd_flags =
        (pktns->crypto.tx.ckm->flags & NGTCP2_CRYPTO_KM_FLAG_KEY_PHASE_ONE)
            ? NGTCP2_PKT_FLAG_KEY_PHASE
            : NGTCP2_PKT_FLAG_NONE;
    ctx.ckm = pktns->crypto.tx.ckm;
    ctx.hp = pktns->crypto.tx.hp;
    break;
  case NGTCP2_PKT_0RTT:
    assert(!conn->server);
    if (!conn->early.ckm) {
      return 0;
    }
    hd_flags = NGTCP2_PKT_FLAG_LONG_FORM;
    ctx.ckm = conn->early.ckm;
    ctx.hp = conn->early.hp;
    break;
  default:
    /* Unreachable */
    assert(0);
  }

  ctx.aead_overhead = conn->crypto.aead_overhead;
  ctx.encrypt = conn->callbacks.encrypt;
  ctx.hp_mask = conn->callbacks.hp_mask;
  ctx.user_data = conn;

  if (data_strm) {
    ndatalen = conn_enforce_flow_control(conn, data_strm, datalen);
    /* 0 length STREAM frame is allowed */
    if (ndatalen || datalen == 0) {
      send_stream = 1;
    } else {
      stream_blocked = 1;
    }
  }

  if (conn->oscid.datalen) {
    rv = conn_enqueue_new_connection_id(conn);
    if (rv != 0) {
      return rv;
    }
  }

  /* TODO Take into account stream frames */
  if ((pktns->tx.frq || send_stream ||
       ngtcp2_ringbuf_len(&conn->rx.path_challenge) ||
       conn_should_send_max_data(conn)) &&
      conn->rx.unsent_max_offset > conn->rx.max_offset) {
    rv = ngtcp2_frame_chain_new(&nfrc, conn->mem);
    if (rv != 0) {
      return rv;
    }
    nfrc->fr.type = NGTCP2_FRAME_MAX_DATA;
    nfrc->fr.max_data.max_data = conn->rx.unsent_max_offset;
    nfrc->next = pktns->tx.frq;
    pktns->tx.frq = nfrc;

    conn->rx.max_offset = conn->rx.unsent_max_offset;
  }

  ngtcp2_pkt_hd_init(&hd, hd_flags, type, &conn->dcid.current.cid, &conn->oscid,
                     pktns->tx.last_pkt_num + 1, pktns_select_pkt_numlen(pktns),
                     conn->version, 0);

  ngtcp2_ppe_init(&ppe, dest, destlen, &ctx);

  rv = ngtcp2_ppe_encode_hd(&ppe, &hd);
  if (rv != 0) {
    assert(NGTCP2_ERR_NOBUF == rv);
    return 0;
  }

  if (!ngtcp2_ppe_ensure_hp_sample(&ppe)) {
    return 0;
  }

  for (; ngtcp2_ringbuf_len(&conn->rx.path_challenge);) {
    pcent = ngtcp2_ringbuf_get(&conn->rx.path_challenge, 0);

    /* PATH_RESPONSE is bound to the path that the corresponding
       PATH_CHALLENGE is received. */
    if (!ngtcp2_path_eq(&conn->dcid.current.ps.path, &pcent->ps.path)) {
      break;
    }

    lfr.type = NGTCP2_FRAME_PATH_RESPONSE;
    memcpy(lfr.path_response.data, pcent->data, sizeof(lfr.path_response.data));

    rv = conn_ppe_write_frame_hd_log(conn, &ppe, &hd_logged, &hd, &lfr);
    if (rv != 0) {
      assert(NGTCP2_ERR_NOBUF == rv);
      break;
    }

    ngtcp2_ringbuf_pop_front(&conn->rx.path_challenge);

    pkt_empty = 0;
    rtb_entry_flags |= NGTCP2_RTB_FLAG_ACK_ELICITING;
    /* We don't retransmit PATH_RESPONSE. */
  }

  rv = conn_create_ack_frame(conn, &ackfr, &pktns->acktr, ts,
                             conn_compute_ack_delay(conn),
                             conn->local.settings.ack_delay_exponent);
  if (rv != 0) {
    assert(ngtcp2_err_is_fatal(rv));
    return rv;
  }

  if (ackfr) {
    rv = conn_ppe_write_frame_hd_log(conn, &ppe, &hd_logged, &hd, ackfr);
    if (rv != 0) {
      assert(NGTCP2_ERR_NOBUF == rv);
    } else {
      ngtcp2_acktr_commit_ack(&pktns->acktr);
      ngtcp2_acktr_add_ack(&pktns->acktr, hd.pkt_num, ackfr->ack.largest_ack);
      pkt_empty = 0;
    }
    ngtcp2_mem_free(conn->mem, ackfr);
    ackfr = NULL;
  }

  for (pfrc = &pktns->tx.frq; *pfrc;) {
    switch ((*pfrc)->fr.type) {
    case NGTCP2_FRAME_STOP_SENDING:
      strm = ngtcp2_conn_find_stream(conn, (*pfrc)->fr.stop_sending.stream_id);
      if (strm == NULL || (strm->flags & NGTCP2_STRM_FLAG_SHUT_RD)) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
        continue;
      }
      break;
    case NGTCP2_FRAME_STREAM:
      assert(0);
      break;
    case NGTCP2_FRAME_MAX_STREAMS_BIDI:
      if ((*pfrc)->fr.max_streams.max_streams < conn->remote.bidi.max_streams) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
        continue;
      }
      break;
    case NGTCP2_FRAME_MAX_STREAMS_UNI:
      if ((*pfrc)->fr.max_streams.max_streams < conn->remote.uni.max_streams) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
        continue;
      }
      break;
    case NGTCP2_FRAME_MAX_STREAM_DATA:
      strm =
          ngtcp2_conn_find_stream(conn, (*pfrc)->fr.max_stream_data.stream_id);
      if (strm == NULL || (strm->flags & NGTCP2_STRM_FLAG_SHUT_RD) ||
          (*pfrc)->fr.max_stream_data.max_stream_data < strm->rx.max_offset) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
        continue;
      }
      break;
    case NGTCP2_FRAME_MAX_DATA:
      if ((*pfrc)->fr.max_data.max_data < conn->rx.max_offset) {
        frc = *pfrc;
        *pfrc = (*pfrc)->next;
        ngtcp2_frame_chain_del(frc, conn->mem);
        continue;
      }
      break;
    case NGTCP2_FRAME_CRYPTO:
      assert(0);
      break;
    }

    rv = conn_ppe_write_frame_hd_log(conn, &ppe, &hd_logged, &hd, &(*pfrc)->fr);
    if (rv != 0) {
      assert(NGTCP2_ERR_NOBUF == rv);
      break;
    }

    pkt_empty = 0;
    rtb_entry_flags |= NGTCP2_RTB_FLAG_ACK_ELICITING;
    pfrc = &(*pfrc)->next;
  }

  if (rv != NGTCP2_ERR_NOBUF) {
    for (; !ngtcp2_pq_empty(&pktns->crypto.tx.frq);) {
      left = ngtcp2_ppe_left(&ppe);

      left = ngtcp2_pkt_crypto_max_datalen(
          conn_cryptofrq_top(conn, pktns)->fr.offset, left, left);

      if (left == (size_t)-1) {
        break;
      }

      rv = conn_cryptofrq_pop(conn, &ncfrc, pktns, left);
      if (rv != 0) {
        assert(ngtcp2_err_is_fatal(rv));
        return rv;
      }

      if (ncfrc == NULL) {
        break;
      }

      rv = conn_ppe_write_frame_hd_log(conn, &ppe, &hd_logged, &hd,
                                       &ncfrc->frc.fr);
      if (rv != 0) {
        assert(0);
      }

      *pfrc = &ncfrc->frc;
      pfrc = &(*pfrc)->next;

      pkt_empty = 0;
      rtb_entry_flags |= NGTCP2_RTB_FLAG_ACK_ELICITING;
    }
  }

  /* Write MAX_STREAM_ID after RESET_STREAM so that we can extend stream
     ID space in one packet. */
  if (rv != NGTCP2_ERR_NOBUF && *pfrc == NULL &&
      conn->remote.bidi.unsent_max_streams > conn->remote.bidi.max_streams) {
    rv = conn_call_extend_max_remote_streams_bidi(
        conn, conn->remote.bidi.unsent_max_streams);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal(rv));
      return rv;
    }

    rv = ngtcp2_frame_chain_new(&nfrc, conn->mem);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal(rv));
      return rv;
    }
    nfrc->fr.type = NGTCP2_FRAME_MAX_STREAMS_BIDI;
    nfrc->fr.max_streams.max_streams = conn->remote.bidi.unsent_max_streams;
    *pfrc = nfrc;

    conn->remote.bidi.max_streams = conn->remote.bidi.unsent_max_streams;

    rv = conn_ppe_write_frame_hd_log(conn, &ppe, &hd_logged, &hd, &(*pfrc)->fr);
    if (rv != 0) {
      assert(NGTCP2_ERR_NOBUF == rv);
    } else {
      pkt_empty = 0;
      rtb_entry_flags |= NGTCP2_RTB_FLAG_ACK_ELICITING;
      pfrc = &(*pfrc)->next;
    }
  }

  if (rv != NGTCP2_ERR_NOBUF && *pfrc == NULL) {
    if (conn->remote.uni.unsent_max_streams > conn->remote.uni.max_streams) {
      rv = conn_call_extend_max_remote_streams_uni(
          conn, conn->remote.uni.unsent_max_streams);
      if (rv != 0) {
        assert(ngtcp2_err_is_fatal(rv));
        return rv;
      }

      rv = ngtcp2_frame_chain_new(&nfrc, conn->mem);
      if (rv != 0) {
        assert(ngtcp2_err_is_fatal(rv));
        return rv;
      }
      nfrc->fr.type = NGTCP2_FRAME_MAX_STREAMS_UNI;
      nfrc->fr.max_streams.max_streams = conn->remote.uni.unsent_max_streams;
      *pfrc = nfrc;

      conn->remote.uni.max_streams = conn->remote.uni.unsent_max_streams;

      rv = conn_ppe_write_frame_hd_log(conn, &ppe, &hd_logged, &hd,
                                       &(*pfrc)->fr);
      if (rv != 0) {
        assert(NGTCP2_ERR_NOBUF == rv);
      } else {
        pkt_empty = 0;
        rtb_entry_flags |= NGTCP2_RTB_FLAG_ACK_ELICITING;
        pfrc = &(*pfrc)->next;
      }
    }
  }

  if (rv != NGTCP2_ERR_NOBUF) {
    for (; !ngtcp2_pq_empty(&conn->tx.strmq);) {
      strm = ngtcp2_conn_tx_strmq_top(conn);

      if (!(strm->flags & NGTCP2_STRM_FLAG_SHUT_RD) &&
          strm->rx.max_offset < strm->rx.unsent_max_offset) {
        rv = ngtcp2_frame_chain_new(&nfrc, conn->mem);
        if (rv != 0) {
          assert(ngtcp2_err_is_fatal(rv));
          return rv;
        }
        nfrc->fr.type = NGTCP2_FRAME_MAX_STREAM_DATA;
        nfrc->fr.max_stream_data.stream_id = strm->stream_id;
        nfrc->fr.max_stream_data.max_stream_data = strm->rx.unsent_max_offset;
        ngtcp2_list_insert(nfrc, pfrc);

        rv =
            conn_ppe_write_frame_hd_log(conn, &ppe, &hd_logged, &hd, &nfrc->fr);
        if (rv != 0) {
          assert(NGTCP2_ERR_NOBUF == rv);
          goto tx_strmq_finish;
        }

        pkt_empty = 0;
        rtb_entry_flags |= NGTCP2_RTB_FLAG_ACK_ELICITING;
        pfrc = &(*pfrc)->next;
        strm->rx.max_offset = strm->rx.unsent_max_offset;
      }

      for (;;) {
        if (ngtcp2_strm_streamfrq_empty(strm)) {
          ngtcp2_conn_tx_strmq_pop(conn);
          if (written_stream_id == -1) {
            break;
          }
          goto tx_strmq_finish;
        }

        left = ngtcp2_ppe_left(&ppe);

        left = ngtcp2_pkt_stream_max_datalen(
            strm->stream_id, ngtcp2_strm_streamfrq_top(strm)->fr.offset, left,
            left);

        if (left == (size_t)-1) {
          if (written_stream_id != -1) {
            ngtcp2_conn_tx_strmq_pop(conn);
            ++strm->cycle;
            rv = ngtcp2_conn_tx_strmq_push(conn, strm);
            if (rv != 0) {
              assert(ngtcp2_err_is_fatal(rv));
              return rv;
            }
          }
          goto tx_strmq_finish;
        }

        rv = ngtcp2_strm_streamfrq_pop(strm, &nsfrc, left);
        if (rv != 0) {
          assert(ngtcp2_err_is_fatal(rv));
          return rv;
        }

        if (nsfrc == NULL) {
          goto tx_strmq_finish;
        }

        rv = conn_ppe_write_frame_hd_log(conn, &ppe, &hd_logged, &hd,
                                         &nsfrc->frc.fr);
        if (rv != 0) {
          assert(0);
        }

        *pfrc = &nsfrc->frc;
        pfrc = &(*pfrc)->next;

        written_stream_id = strm->stream_id;

        pkt_empty = 0;
        rtb_entry_flags |= NGTCP2_RTB_FLAG_ACK_ELICITING;
      }
    }
  }

tx_strmq_finish:

  left = ngtcp2_ppe_left(&ppe);

  if (rv != NGTCP2_ERR_NOBUF && send_stream &&
      (written_stream_id == -1 || written_stream_id == data_strm->stream_id) &&
      *pfrc == NULL &&
      (ndatalen = ngtcp2_pkt_stream_max_datalen(data_strm->stream_id,
                                                data_strm->tx.offset, ndatalen,
                                                left)) != (size_t)-1 &&
      (ndatalen || datalen == 0)) {
    rv = ngtcp2_stream_frame_chain_new(&nsfrc, conn->mem);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal(rv));
      return rv;
    }

    nsfrc->fr.type = NGTCP2_FRAME_STREAM;
    nsfrc->fr.flags = 0;
    nsfrc->fr.stream_id = data_strm->stream_id;
    nsfrc->fr.offset = data_strm->tx.offset;
    nsfrc->fr.datacnt =
        ngtcp2_vec_copy(nsfrc->fr.data, &ndatalen, NGTCP2_MAX_STREAM_DATACNT,
                        datav, datavcnt, ndatalen);

    fin = fin && ndatalen == datalen;
    nsfrc->fr.fin = fin;

    rv = conn_ppe_write_frame_hd_log(conn, &ppe, &hd_logged, &hd,
                                     &nsfrc->frc.fr);
    if (rv != 0) {
      assert(0);
    }

    *pfrc = &nsfrc->frc;
    pfrc = &(*pfrc)->next;

    pkt_empty = 0;
    rtb_entry_flags |= NGTCP2_RTB_FLAG_ACK_ELICITING;
  } else {
    send_stream = 0;
  }

  if (pkt_empty) {
    assert(rv == 0 || NGTCP2_ERR_NOBUF == rv);
    if (rv == 0 && stream_blocked) {
      return NGTCP2_ERR_STREAM_DATA_BLOCKED;
    }
    return 0;
  }

  /* TODO Push STREAM frame back to ngtcp2_strm if there is an error
     before ngtcp2_rtb_entry is safely created and added. */
  if ((require_padding ||
       (type == NGTCP2_PKT_0RTT && conn->state == NGTCP2_CS_CLIENT_INITIAL)) &&
      ngtcp2_ppe_left(&ppe)) {
    lfr.type = NGTCP2_FRAME_PADDING;
    lfr.padding.len = ngtcp2_ppe_padding(&ppe);
    ngtcp2_log_tx_fr(&conn->log, &hd, &lfr);
  } else {
    lfr.type = NGTCP2_FRAME_PADDING;
    lfr.padding.len = ngtcp2_ppe_padding_hp_sample(&ppe);
    if (lfr.padding.len) {
      ngtcp2_log_tx_fr(&conn->log, &hd, &lfr);
    }
  }

  nwrite = ngtcp2_ppe_final(&ppe, NULL);
  if (nwrite < 0) {
    assert(ngtcp2_err_is_fatal((int)nwrite));
    return nwrite;
  }

  if (*pfrc != pktns->tx.frq) {
    rv = ngtcp2_rtb_entry_new(&ent, &hd, NULL, ts, (size_t)nwrite,
                              rtb_entry_flags, conn->mem);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal((int)nwrite));
      return rv;
    }

    ent->frc = pktns->tx.frq;
    pktns->tx.frq = *pfrc;
    *pfrc = NULL;

    rv = conn_on_pkt_sent(conn, &pktns->rtb, ent);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal(rv));
      ngtcp2_rtb_entry_del(ent, conn->mem);
      return rv;
    }

    if (send_stream) {
      data_strm->tx.offset += ndatalen;
      conn->tx.offset += ndatalen;

      if (fin) {
        ngtcp2_strm_shutdown(data_strm, NGTCP2_STRM_FLAG_SHUT_WR);
      }
    }
  }

  if (pdatalen && send_stream) {
    *pdatalen = (ssize_t)ndatalen;
  }

  ++pktns->tx.last_pkt_num;

  return nwrite;
}

/*
 * conn_write_single_frame_pkt writes a packet which contains |fr|
 * frame only in the buffer pointed by |dest| whose length if
 * |destlen|.  |type| is a long packet type to send.  If |type| is 0,
 * Short packet is used.  |dcid| is used as a destination connection
 * ID.
 *
 * The packet written by this function will not be retransmitted.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 */
static ssize_t conn_write_single_frame_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                           size_t destlen, uint8_t type,
                                           const ngtcp2_cid *dcid,
                                           ngtcp2_frame *fr, uint8_t rtb_flags,
                                           ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_ppe ppe;
  ngtcp2_pkt_hd hd;
  ngtcp2_frame lfr;
  ssize_t nwrite;
  ngtcp2_crypto_ctx ctx;
  ngtcp2_pktns *pktns;
  uint8_t flags;
  ngtcp2_rtb_entry *rtbent;

  switch (type) {
  case NGTCP2_PKT_INITIAL:
    pktns = &conn->in_pktns;
    ctx.aead_overhead = NGTCP2_INITIAL_AEAD_OVERHEAD;
    ctx.encrypt = conn->callbacks.in_encrypt;
    ctx.hp_mask = conn->callbacks.in_hp_mask;
    flags = NGTCP2_PKT_FLAG_LONG_FORM;
    break;
  case NGTCP2_PKT_HANDSHAKE:
    pktns = &conn->hs_pktns;
    ctx.aead_overhead = conn->crypto.aead_overhead;
    ctx.encrypt = conn->callbacks.encrypt;
    ctx.hp_mask = conn->callbacks.hp_mask;
    flags = NGTCP2_PKT_FLAG_LONG_FORM;
    break;
  case NGTCP2_PKT_SHORT:
    /* 0 means Short packet. */
    pktns = &conn->pktns;
    ctx.aead_overhead = conn->crypto.aead_overhead;
    ctx.encrypt = conn->callbacks.encrypt;
    ctx.hp_mask = conn->callbacks.hp_mask;
    flags = (pktns->crypto.tx.ckm->flags & NGTCP2_CRYPTO_KM_FLAG_KEY_PHASE_ONE)
                ? NGTCP2_PKT_FLAG_KEY_PHASE
                : NGTCP2_PKT_FLAG_NONE;
    break;
  default:
    /* We don't support 0-RTT packet in this function. */
    assert(0);
  }

  ctx.ckm = pktns->crypto.tx.ckm;
  ctx.hp = pktns->crypto.tx.hp;
  ctx.user_data = conn;

  ngtcp2_pkt_hd_init(&hd, flags, type, dcid, &conn->oscid,
                     pktns->tx.last_pkt_num + 1, pktns_select_pkt_numlen(pktns),
                     conn->version, 0);

  ngtcp2_ppe_init(&ppe, dest, destlen, &ctx);

  rv = ngtcp2_ppe_encode_hd(&ppe, &hd);
  if (rv != 0) {
    assert(NGTCP2_ERR_NOBUF == rv);
    return 0;
  }

  if (!ngtcp2_ppe_ensure_hp_sample(&ppe)) {
    return 0;
  }

  ngtcp2_log_tx_pkt_hd(&conn->log, &hd);

  rv = conn_ppe_write_frame(conn, &ppe, &hd, fr);
  if (rv != 0) {
    assert(NGTCP2_ERR_NOBUF == rv);
    return 0;
  }

  lfr.type = NGTCP2_FRAME_PADDING;
  lfr.padding.len = ngtcp2_ppe_padding_hp_sample(&ppe);
  if (lfr.padding.len) {
    ngtcp2_log_tx_fr(&conn->log, &hd, &lfr);
  }

  nwrite = ngtcp2_ppe_final(&ppe, NULL);
  if (nwrite < 0) {
    return nwrite;
  }

  /* Do this when we are sure that there is no error. */
  if (fr->type == NGTCP2_FRAME_ACK) {
    ngtcp2_acktr_commit_ack(&pktns->acktr);
    ngtcp2_acktr_add_ack(&pktns->acktr, hd.pkt_num, fr->ack.largest_ack);
  }

  if (rtb_flags & NGTCP2_RTB_FLAG_ACK_ELICITING) {
    rv = ngtcp2_rtb_entry_new(&rtbent, &hd, NULL, ts, (size_t)nwrite, rtb_flags,
                              conn->mem);
    if (rv != 0) {
      return rv;
    }

    rv = conn_on_pkt_sent(conn, &pktns->rtb, rtbent);
    if (rv != 0) {
      ngtcp2_rtb_entry_del(rtbent, conn->mem);
      return rv;
    }
  }

  ++pktns->tx.last_pkt_num;

  return nwrite;
}

/*
 * conn_write_protected_ack_pkt writes QUIC Short packet which only
 * includes ACK frame in the buffer pointed by |dest| whose length is
 * |destlen|.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static ssize_t conn_write_protected_ack_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                            size_t destlen, ngtcp2_tstamp ts) {
  int rv;
  ssize_t spktlen;
  ngtcp2_frame *ackfr;
  ngtcp2_acktr *acktr = &conn->pktns.acktr;

  ackfr = NULL;
  rv = conn_create_ack_frame(conn, &ackfr, acktr, ts,
                             conn_compute_ack_delay(conn),
                             conn->local.settings.ack_delay_exponent);
  if (rv != 0) {
    return rv;
  }

  if (!ackfr) {
    return 0;
  }

  spktlen = conn_write_single_frame_pkt(conn, dest, destlen, NGTCP2_PKT_SHORT,
                                        &conn->dcid.current.cid, ackfr,
                                        NGTCP2_RTB_FLAG_NONE, ts);
  ngtcp2_mem_free(conn->mem, ackfr);
  if (spktlen < 0) {
    return spktlen;
  }

  return spktlen;
}

/*
 * conn_process_early_rtb makes any pending 0RTT packet Short packet.
 */
static void conn_process_early_rtb(ngtcp2_conn *conn) {
  ngtcp2_rtb_entry *ent;
  ngtcp2_rtb *rtb = &conn->pktns.rtb;
  ngtcp2_ksl_it it;

  for (it = ngtcp2_rtb_head(rtb); !ngtcp2_ksl_it_end(&it);
       ngtcp2_ksl_it_next(&it)) {
    ent = ngtcp2_ksl_it_get(&it);

    if ((ent->hd.flags & NGTCP2_PKT_FLAG_LONG_FORM) == 0 ||
        ent->hd.type != NGTCP2_PKT_0RTT) {
      continue;
    }

    ent->hd.dcid = conn->dcid.current.cid;

    /*  0-RTT packet is retransmitted as a Short packet. */
    ent->hd.flags &= (uint8_t)~NGTCP2_PKT_FLAG_LONG_FORM;
    ent->hd.type = NGTCP2_PKT_SHORT;
  }
}

/*
 * conn_write_probe_ping writes probe packet containing PING frame
 * (and optionally ACK frame) to the buffer pointed by |dest| of
 * length |destlen|.  Probe packet is always Short packet.  This
 * function might return 0 if it cannot write packet (e.g., |destlen|
 * is too small).
 *
 * This function returns the number of bytes written to |dest|, or one
 * of the following negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static ssize_t conn_write_probe_ping(ngtcp2_conn *conn, uint8_t *dest,
                                     size_t destlen, ngtcp2_tstamp ts) {
  ngtcp2_ppe ppe;
  ngtcp2_pkt_hd hd;
  ngtcp2_pktns *pktns = &conn->pktns;
  ngtcp2_crypto_ctx ctx;
  ngtcp2_frame_chain *frc = NULL;
  ngtcp2_rtb_entry *ent;
  ngtcp2_frame *ackfr = NULL, lfr;
  int rv;
  ssize_t nwrite;

  assert(pktns->crypto.tx.ckm);

  ctx.aead_overhead = conn->crypto.aead_overhead;
  ctx.encrypt = conn->callbacks.encrypt;
  ctx.hp_mask = conn->callbacks.hp_mask;
  ctx.ckm = pktns->crypto.tx.ckm;
  ctx.hp = pktns->crypto.tx.hp;
  ctx.user_data = conn;

  ngtcp2_pkt_hd_init(
      &hd,
      (pktns->crypto.tx.ckm->flags & NGTCP2_CRYPTO_KM_FLAG_KEY_PHASE_ONE)
          ? NGTCP2_PKT_FLAG_KEY_PHASE
          : NGTCP2_PKT_FLAG_NONE,
      NGTCP2_PKT_SHORT, &conn->dcid.current.cid, NULL,
      pktns->tx.last_pkt_num + 1, pktns_select_pkt_numlen(pktns), conn->version,
      0);

  ngtcp2_ppe_init(&ppe, dest, destlen, &ctx);

  rv = ngtcp2_ppe_encode_hd(&ppe, &hd);
  if (rv != 0) {
    assert(NGTCP2_ERR_NOBUF == rv);
    return 0;
  }

  if (!ngtcp2_ppe_ensure_hp_sample(&ppe)) {
    return 0;
  }

  ngtcp2_log_tx_pkt_hd(&conn->log, &hd);

  rv = ngtcp2_frame_chain_new(&frc, conn->mem);
  if (rv != 0) {
    return rv;
  }

  frc->fr.type = NGTCP2_FRAME_PING;

  rv = conn_ppe_write_frame(conn, &ppe, &hd, &frc->fr);
  if (rv != 0) {
    assert(NGTCP2_ERR_NOBUF == rv);
    rv = 0;
    goto fail;
  }

  rv = conn_create_ack_frame(conn, &ackfr, &pktns->acktr, ts,
                             conn_compute_ack_delay(conn),
                             conn->local.settings.ack_delay_exponent);
  if (rv != 0) {
    goto fail;
  }

  if (ackfr) {
    rv = conn_ppe_write_frame(conn, &ppe, &hd, ackfr);
    if (rv != 0) {
      assert(NGTCP2_ERR_NOBUF == rv);
    } else {
      ngtcp2_acktr_commit_ack(&pktns->acktr);
      ngtcp2_acktr_add_ack(&pktns->acktr, hd.pkt_num, ackfr->ack.largest_ack);
    }
    ngtcp2_mem_free(conn->mem, ackfr);
    ackfr = NULL;
  }

  lfr.type = NGTCP2_FRAME_PADDING;
  lfr.padding.len = ngtcp2_ppe_padding_hp_sample(&ppe);
  if (lfr.padding.len) {
    ngtcp2_log_tx_fr(&conn->log, &hd, &lfr);
  }

  nwrite = ngtcp2_ppe_final(&ppe, NULL);
  if (nwrite < 0) {
    rv = (int)nwrite;
    goto fail;
  }

  rv = ngtcp2_rtb_entry_new(
      &ent, &hd, frc, ts, (size_t)nwrite,
      NGTCP2_RTB_FLAG_PROBE | NGTCP2_RTB_FLAG_ACK_ELICITING, conn->mem);
  if (rv != 0) {
    goto fail;
  }

  rv = conn_on_pkt_sent(conn, &pktns->rtb, ent);
  if (rv != 0) {
    ngtcp2_rtb_entry_del(ent, conn->mem);
    return rv;
  }

  ++pktns->tx.last_pkt_num;

  return nwrite;

fail:
  ngtcp2_frame_chain_del(frc, conn->mem);

  return rv;
}

/*
 * conn_write_probe_pkt writes a QUIC Short packet as probe packet.
 * The packet is written to the buffer pointed by |dest| of length
 * |destlen|.  This function can send new stream data.  In order to
 * send stream data, specify the underlying stream to |strm|.  If
 * |fin| is set to nonzero, it signals that the given data is the
 * final portion of the stream.  |datav| vector of length |datavcnt|
 * specify stream data to send.  If no stream data to send, set |strm|
 * to NULL.  The number of bytes sent to the stream is assigned to
 * |*pdatalen|.  If 0 length STREAM data is sent, 0 is assigned to
 * |*pdatalen|.  The caller should initialize |*pdatalen| to -1.
 *
 * This function returns the number of bytes written to the buffer
 * pointed by |dest|, or one of the following negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_STREAM_DATA_BLOCKED
 *     Stream data could not be written because of flow control.
 */
static ssize_t conn_write_probe_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                    size_t destlen, ssize_t *pdatalen,
                                    ngtcp2_strm *strm, uint8_t fin,
                                    const ngtcp2_vec *datav, size_t datavcnt,
                                    ngtcp2_tstamp ts) {
  ssize_t nwrite;

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON,
                  "transmit probe pkt left=%zu", conn->rcs.probe_pkt_left);

  /* a probe packet is not blocked by cwnd. */
  nwrite = conn_write_pkt(conn, dest, destlen, pdatalen, NGTCP2_PKT_SHORT, strm,
                          fin, datav, datavcnt, /* require_padding = */ 0, ts);
  if (nwrite == 0 || nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
    nwrite = conn_write_probe_ping(conn, dest, destlen, ts);
  }
  if (nwrite <= 0) {
    return nwrite;
  }

  --conn->rcs.probe_pkt_left;

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON, "probe pkt size=%zd",
                  nwrite);

  return nwrite;
}

/*
 * conn_handshake_remnants_left returns nonzero if there may be
 * handshake packets the local endpoint has to send, including new
 * packets and lost ones.
 */
static int conn_handshake_remnants_left(ngtcp2_conn *conn) {
  return !(conn->flags & NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED) ||
         ngtcp2_rtb_num_ack_eliciting(&conn->in_pktns.rtb) ||
         ngtcp2_rtb_num_ack_eliciting(&conn->hs_pktns.rtb) ||
         !ngtcp2_pq_empty(&conn->in_pktns.crypto.tx.frq) ||
         !ngtcp2_pq_empty(&conn->hs_pktns.crypto.tx.frq);
}

/*
 * conn_retire_dcid retires |dcid|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory
 */
static int conn_retire_dcid(ngtcp2_conn *conn, const ngtcp2_dcid *dcid,
                            ngtcp2_tstamp ts) {
  ngtcp2_pktns *pktns = &conn->pktns;
  ngtcp2_frame_chain *nfrc;
  ngtcp2_ringbuf *rb = &conn->dcid.retired;
  ngtcp2_dcid *dest;
  int rv;

  if (ngtcp2_ringbuf_full(rb)) {
    ngtcp2_ringbuf_pop_front(rb);
  }

  dest = ngtcp2_ringbuf_push_back(rb);
  ngtcp2_dcid_copy(dest, dcid);
  dest->ts_retired = ts;

  rv = ngtcp2_frame_chain_new(&nfrc, conn->mem);
  if (rv != 0) {
    return rv;
  }

  nfrc->fr.type = NGTCP2_FRAME_RETIRE_CONNECTION_ID;
  nfrc->fr.retire_connection_id.seq = dcid->seq;
  nfrc->next = pktns->tx.frq;
  pktns->tx.frq = nfrc;

  return 0;
}

/*
 * conn_stop_pv stops the path validation which is currently running.
 * This function does nothing if no path validation is currently being
 * performed.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory
 */
static int conn_stop_pv(ngtcp2_conn *conn, ngtcp2_tstamp ts) {
  int rv = 0;
  ngtcp2_pv *pv = conn->pv;

  if (pv == NULL) {
    return 0;
  }

  if (pv->flags & NGTCP2_PV_FLAG_RETIRE_DCID_ON_FINISH) {
    rv = conn_retire_dcid(conn, &pv->dcid, ts);
  }

  ngtcp2_pv_del(pv);
  conn->pv = NULL;

  return rv;
}

/*
 * conn_on_path_validation_failed is called when path validation
 * fails.  This function may delete |pv|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 */
static int conn_on_path_validation_failed(ngtcp2_conn *conn, ngtcp2_pv *pv,
                                          ngtcp2_tstamp ts) {
  int rv;

  /* If path validation fails, the bound DCID is no longer
     necessary.  Retire it. */
  pv->flags |= NGTCP2_PV_FLAG_RETIRE_DCID_ON_FINISH;

  if (!(pv->flags & NGTCP2_PV_FLAG_DONT_CARE)) {
    rv = conn_call_path_validation(conn, &pv->dcid.ps.path,
                                   NGTCP2_PATH_VALIDATION_RESULT_FAILURE);
    if (rv != 0) {
      return rv;
    }
  }

  if (pv->flags & NGTCP2_PV_FLAG_FALLBACK_ON_FAILURE) {
    ngtcp2_dcid_copy(&conn->dcid.current, &pv->fallback_dcid);
  }

  return conn_stop_pv(conn, ts);
}

/*
 * conn_write_path_challenge writes a packet which includes
 * PATH_CHALLENGE frame into |dest| of length |destlen|.
 *
 * This function returns the number of bytes written to |dest|, or one
 * of the following negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 */
static ssize_t conn_write_path_challenge(ngtcp2_conn *conn, ngtcp2_path *path,
                                         uint8_t *dest, size_t destlen,
                                         ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_tstamp expiry;
  ngtcp2_pv *pv = conn->pv;
  ngtcp2_frame lfr;

  ngtcp2_pv_ensure_start(pv, ts);

  if (ngtcp2_pv_validation_timed_out(pv, ts)) {
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PTV,
                    "path validation was timed out");
    return conn_on_path_validation_failed(conn, pv, ts);
  }

  ngtcp2_pv_handle_entry_expiry(pv, ts);

  if (ngtcp2_pv_full(pv)) {
    return 0;
  }

  if (path) {
    ngtcp2_path_copy(path, &pv->dcid.ps.path);
  }

  assert(conn->callbacks.rand);
  rv = conn->callbacks.rand(conn, lfr.path_challenge.data,
                            sizeof(lfr.path_challenge.data),
                            NGTCP2_RAND_CTX_PATH_CHALLENGE, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  lfr.type = NGTCP2_FRAME_PATH_CHALLENGE;

  /* TODO reconsider this.  This might get larger pretty quickly than
     validation timeout which is just around 3*PTO. */
  expiry = ts + 6ULL * NGTCP2_DEFAULT_INITIAL_RTT * (1ULL << pv->loss_count);

  ngtcp2_pv_add_entry(pv, lfr.path_challenge.data, expiry);

  return conn_write_single_frame_pkt(conn, dest, destlen, NGTCP2_PKT_SHORT,
                                     &pv->dcid.cid, &lfr,
                                     NGTCP2_RTB_FLAG_ACK_ELICITING, ts);
}

/*
 * conn_bind_dcid stores the DCID to |*pdcid| bound to |path|.  If
 * such DCID is not found, bind the new DCID to |path| and stores it
 * to |*pdcid|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_CONN_ID_BLOCKED
 *     No unbound DCID is available
 * NGTCP2_ERR_NOMEM
 *     Out of memory
 */
static int conn_bind_dcid(ngtcp2_conn *conn, ngtcp2_dcid **pdcid,
                          const ngtcp2_path *path, ngtcp2_tstamp ts) {
  ngtcp2_pv *pv = conn->pv;
  ngtcp2_dcid *dcid, *ndcid;
  size_t i, len;
  int rv;

  assert(!ngtcp2_path_eq(&conn->dcid.current.ps.path, path));
  assert(!pv || !ngtcp2_path_eq(&pv->dcid.ps.path, path));

  len = ngtcp2_ringbuf_len(&conn->dcid.bound);
  for (i = 0; i < len; ++i) {
    dcid = ngtcp2_ringbuf_get(&conn->dcid.bound, i);

    if (ngtcp2_path_eq(&dcid->ps.path, path)) {
      *pdcid = dcid;
      return 0;
    }
  }

  if (ngtcp2_ringbuf_len(&conn->dcid.unused) == 0) {
    return NGTCP2_ERR_CONN_ID_BLOCKED;
  }

  dcid = ngtcp2_ringbuf_get(&conn->dcid.unused, 0);

  if (ngtcp2_ringbuf_full(&conn->dcid.bound)) {
    rv = conn_retire_dcid(conn, ngtcp2_ringbuf_get(&conn->dcid.bound, 0), ts);
    if (rv != 0) {
      return rv;
    }
  }

  ndcid = ngtcp2_ringbuf_push_back(&conn->dcid.bound);

  ngtcp2_dcid_copy(ndcid, dcid);
  ngtcp2_path_copy(&ndcid->ps.path, path);

  ngtcp2_ringbuf_pop_front(&conn->dcid.unused);

  *pdcid = ndcid;

  return 0;
}

/*
 * conn_write_path_response writes a packet which includes
 * PATH_RESPONSE frame into |dest| of length |destlen|.
 *
 * This function returns the number of bytes written to |dest|, or one
 * of the following negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 */
static ssize_t conn_write_path_response(ngtcp2_conn *conn, ngtcp2_path *path,
                                        uint8_t *dest, size_t destlen,
                                        ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_path_challenge_entry *pcent = NULL;
  ngtcp2_dcid *dcid = NULL;
  ngtcp2_frame lfr;
  ssize_t nwrite;

  for (; ngtcp2_ringbuf_len(&conn->rx.path_challenge);) {
    pcent = ngtcp2_ringbuf_get(&conn->rx.path_challenge, 0);

    if (ngtcp2_path_eq(&conn->dcid.current.ps.path, &pcent->ps.path)) {
      dcid = &conn->dcid.current;
      break;
    }

    if (conn->pv && ngtcp2_path_eq(&conn->pv->dcid.ps.path, &pcent->ps.path)) {
      dcid = &conn->pv->dcid;
      break;
    }

    if (!conn->server) {
      /* Client does not expect to respond to path validation against
         unknown path */
      ngtcp2_ringbuf_pop_front(&conn->rx.path_challenge);
      pcent = NULL;
      continue;
    }

    break;
  }

  if (pcent == NULL) {
    return 0;
  }

  lfr.type = NGTCP2_FRAME_PATH_RESPONSE;
  memcpy(lfr.path_response.data, pcent->data, sizeof(lfr.path_response.data));

  if (dcid == NULL) {
    /* client is expected to have |path| in conn->dcid.current or
       conn->pv. */
    assert(conn->server);

    rv = conn_bind_dcid(conn, &dcid, &pcent->ps.path, ts);
    if (rv != 0) {
      if (ngtcp2_err_is_fatal(rv)) {
        return rv;
      }
      return 0;
    }
  }

  if (path) {
    ngtcp2_path_copy(path, &pcent->ps.path);
  }

  nwrite = conn_write_single_frame_pkt(conn, dest, destlen, NGTCP2_PKT_SHORT,
                                       &dcid->cid, &lfr,
                                       NGTCP2_RTB_FLAG_ACK_ELICITING, ts);
  if (nwrite <= 0) {
    return nwrite;
  }

  ngtcp2_ringbuf_pop_front(&conn->rx.path_challenge);

  return nwrite;
}

/*
 * conn_peer_has_unused_cid returns nonzero if the remote endpoint has
 * at least one unused connection ID.
 */
static int conn_peer_has_unused_cid(ngtcp2_conn *conn) {
  return ngtcp2_ksl_len(&conn->scid.set) - ngtcp2_pq_size(&conn->scid.used) > 0;
}

ssize_t ngtcp2_conn_write_pkt(ngtcp2_conn *conn, ngtcp2_path *path,
                              uint8_t *dest, size_t destlen, ngtcp2_tstamp ts) {
  ssize_t nwrite;
  uint64_t cwnd;
  ngtcp2_pktns *pktns = &conn->pktns;
  size_t origlen = destlen;
  int rv;

  conn->log.last_ts = ts;

  if (pktns->tx.last_pkt_num == NGTCP2_MAX_PKT_NUM) {
    return NGTCP2_ERR_PKT_NUM_EXHAUSTED;
  }

  switch (conn->state) {
  case NGTCP2_CS_CLIENT_INITIAL:
  case NGTCP2_CS_CLIENT_WAIT_HANDSHAKE:
  case NGTCP2_CS_CLIENT_TLS_HANDSHAKE_FAILED:
  case NGTCP2_CS_SERVER_INITIAL:
  case NGTCP2_CS_SERVER_WAIT_HANDSHAKE:
  case NGTCP2_CS_SERVER_TLS_HANDSHAKE_FAILED:
    return NGTCP2_ERR_INVALID_STATE;
  case NGTCP2_CS_POST_HANDSHAKE:
    rv = conn_remove_retired_connection_id(conn, ts);
    if (rv != 0) {
      return rv;
    }

    nwrite = conn_write_path_response(conn, path, dest, destlen, ts);
    if (nwrite) {
      return nwrite;
    }

    if (conn->pv && conn_peer_has_unused_cid(conn)) {
      nwrite = conn_write_path_challenge(conn, path, dest, destlen, ts);
      if (nwrite) {
        return nwrite;
      }
    }

    cwnd = conn_cwnd_left(conn);
    destlen = ngtcp2_min(destlen, cwnd);

    if (path) {
      ngtcp2_path_copy(path, &conn->dcid.current.ps.path);
    }

    if (conn_handshake_remnants_left(conn)) {
      nwrite = conn_write_handshake_pkts(conn, dest, destlen, 0, ts);
      if (nwrite) {
        return nwrite;
      }
    }
    nwrite = conn_write_handshake_ack_pkts(conn, dest, origlen,
                                           /* require_padding = */ 1, ts);
    if (nwrite) {
      return nwrite;
    }

    if (conn->rcs.probe_pkt_left) {
      return conn_write_probe_pkt(conn, dest, origlen, NULL, NULL, 0, NULL, 0,
                                  ts);
    }

    nwrite = conn_write_pkt(conn, dest, destlen, NULL, NGTCP2_PKT_SHORT, NULL,
                            0, NULL, 0, /* require_padding = */ 0, ts);
    if (nwrite < 0) {
      assert(nwrite != NGTCP2_ERR_NOBUF);
      return nwrite;
    }
    if (nwrite) {
      return nwrite;
    }
    return conn_write_protected_ack_pkt(conn, dest, origlen, ts);
  case NGTCP2_CS_CLOSING:
    return NGTCP2_ERR_CLOSING;
  case NGTCP2_CS_DRAINING:
    return NGTCP2_ERR_DRAINING;
  default:
    return 0;
  }
}

/*
 * conn_on_version_negotiation is called when Version Negotiation
 * packet is received.  The function decodes the data in the buffer
 * pointed by |payload| whose length is |payloadlen| as Version
 * Negotiation packet payload.  The packet header is given in |hd|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_INVALID_ARGUMENT
 *     Packet payload is badly formatted.
 */
static int conn_on_version_negotiation(ngtcp2_conn *conn,
                                       const ngtcp2_pkt_hd *hd,
                                       const uint8_t *payload,
                                       size_t payloadlen) {
  uint32_t sv[16];
  uint32_t *p;
  int rv = 0;
  size_t nsv;

  if (payloadlen % sizeof(uint32_t)) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  if (payloadlen > sizeof(sv)) {
    p = ngtcp2_mem_malloc(conn->mem, payloadlen);
    if (p == NULL) {
      return NGTCP2_ERR_NOMEM;
    }
  } else {
    p = sv;
  }

  /* TODO Just move to the terminal state for now in order not to send
     CONNECTION_CLOSE frame. */
  conn->state = NGTCP2_CS_DRAINING;

  nsv = ngtcp2_pkt_decode_version_negotiation(p, payload, payloadlen);

  ngtcp2_log_rx_vn(&conn->log, hd, sv, nsv);

  if (conn->callbacks.recv_version_negotiation) {
    rv = conn->callbacks.recv_version_negotiation(conn, hd, sv, nsv,
                                                  conn->user_data);
  }

  if (p != sv) {
    ngtcp2_mem_free(conn->mem, p);
  }

  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

/*
 * conn_resched_frames reschedules frames linked from |*pfrc| for
 * retransmission.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_resched_frames(ngtcp2_conn *conn, ngtcp2_pktns *pktns,
                               ngtcp2_frame_chain **pfrc) {
  ngtcp2_frame_chain **first = pfrc;
  ngtcp2_stream_frame_chain *sfrc;
  ngtcp2_stream *sfr;
  ngtcp2_crypto_frame_chain *cfrc;
  ngtcp2_strm *strm;
  int rv;

  if (*pfrc == NULL) {
    return 0;
  }

  for (; *pfrc;) {
    switch ((*pfrc)->fr.type) {
    case NGTCP2_FRAME_STREAM:
      sfrc = (ngtcp2_stream_frame_chain *)*pfrc;

      *pfrc = sfrc->next;
      sfrc->next = NULL;
      sfr = &sfrc->fr;

      strm = ngtcp2_conn_find_stream(conn, sfr->stream_id);
      if (!strm) {
        ngtcp2_stream_frame_chain_del(sfrc, conn->mem);
        break;
      }
      rv = ngtcp2_strm_streamfrq_push(strm, sfrc);
      if (rv != 0) {
        ngtcp2_stream_frame_chain_del(sfrc, conn->mem);
        return rv;
      }
      if (!ngtcp2_strm_is_tx_queued(strm)) {
        rv = ngtcp2_conn_tx_strmq_push(conn, strm);
        if (rv != 0) {
          return rv;
        }
      }
      break;
    case NGTCP2_FRAME_CRYPTO:
      cfrc = (ngtcp2_crypto_frame_chain *)*pfrc;

      *pfrc = cfrc->next;
      cfrc->next = NULL;

      rv = ngtcp2_pq_push(&pktns->crypto.tx.frq, &cfrc->pe);
      if (rv != 0) {
        assert(ngtcp2_err_is_fatal(rv));
        ngtcp2_crypto_frame_chain_del(cfrc, conn->mem);
        return rv;
      }
      break;
    default:
      pfrc = &(*pfrc)->next;
    }
  }

  *pfrc = pktns->tx.frq;
  pktns->tx.frq = *first;

  return 0;
}

/*
 * conn_on_retry is called when Retry packet is received.  The
 * function decodes the data in the buffer pointed by |payload| whose
 * length is |payloadlen| as Retry packet payload.  The packet header
 * is given in |hd|.  The length of ODCIL is given as |odcil|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_INVALID_ARGUMENT
 *     Packet payload is badly formatted.
 * NGTCP2_ERR_PROTO
 *     ODCID does not match; or Token is empty.
 */
static int conn_on_retry(ngtcp2_conn *conn, const ngtcp2_pkt_hd *hd,
                         size_t odcil, const uint8_t *payload,
                         size_t payloadlen) {
  int rv;
  ngtcp2_pkt_retry retry;
  uint8_t *p;
  ngtcp2_rtb *rtb = &conn->pktns.rtb;
  ngtcp2_rtb *in_rtb = &conn->in_pktns.rtb;
  uint8_t cidbuf[sizeof(retry.odcid.data) * 2 + 1];
  ngtcp2_frame_chain *frc = NULL;

  if (conn->flags & NGTCP2_CONN_FLAG_RECV_RETRY) {
    return 0;
  }

  rv = ngtcp2_pkt_decode_retry(&retry, odcil, payload, payloadlen);
  if (rv != 0) {
    return rv;
  }

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT, "odcid=0x%s",
                  (const char *)ngtcp2_encode_hex(cidbuf, retry.odcid.data,
                                                  retry.odcid.datalen));

  if (!ngtcp2_cid_eq(&conn->dcid.current.cid, &retry.odcid) ||
      retry.tokenlen == 0) {
    return NGTCP2_ERR_PROTO;
  }

  /* DCID must be updated before invoking callback because client
     generates new initial keys there. */
  conn->dcid.current.cid = hd->scid;

  conn->flags |= NGTCP2_CONN_FLAG_RECV_RETRY;

  assert(conn->callbacks.recv_retry);

  rv = conn->callbacks.recv_retry(conn, hd, &retry, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  conn->state = NGTCP2_CS_CLIENT_INITIAL;

  /* Just freeing memory is dangerous because we might free twice. */

  ngtcp2_rtb_remove_all(rtb, &frc);

  rv = conn_resched_frames(conn, &conn->pktns, &frc);
  if (rv != 0) {
    assert(ngtcp2_err_is_fatal(rv));
    ngtcp2_frame_chain_list_del(frc, conn->mem);
    return rv;
  }

  frc = NULL;
  ngtcp2_rtb_remove_all(in_rtb, &frc);

  rv = conn_resched_frames(conn, &conn->in_pktns, &frc);
  if (rv != 0) {
    assert(ngtcp2_err_is_fatal(rv));
    ngtcp2_frame_chain_list_del(frc, conn->mem);
    return rv;
  }

  assert(conn->token.begin == NULL);

  p = ngtcp2_mem_malloc(conn->mem, retry.tokenlen);
  if (p == NULL) {
    return NGTCP2_ERR_NOMEM;
  }
  ngtcp2_buf_init(&conn->token, p, retry.tokenlen);

  ngtcp2_cpymem(conn->token.begin, retry.token, retry.tokenlen);
  conn->token.pos = conn->token.begin;
  conn->token.last = conn->token.pos + retry.tokenlen;

  return 0;
}

int ngtcp2_conn_detect_lost_pkt(ngtcp2_conn *conn, ngtcp2_pktns *pktns,
                                ngtcp2_rcvry_stat *rcs, ngtcp2_tstamp ts) {
  ngtcp2_frame_chain *frc = NULL;
  int rv;
  ngtcp2_duration pto = conn_compute_pto(conn);

  ngtcp2_rtb_detect_lost_pkt(&pktns->rtb, &frc, rcs, pto, ts);

  rv = conn_resched_frames(conn, pktns, &frc);
  if (rv != 0) {
    ngtcp2_frame_chain_list_del(frc, conn->mem);
    return rv;
  }

  return 0;
}

/*
 * conn_recv_ack processes received ACK frame |fr|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory
 * NGTCP2_ERR_ACK_FRAME
 *     ACK frame is malformed.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed.
 */
static int conn_recv_ack(ngtcp2_conn *conn, ngtcp2_pktns *pktns, ngtcp2_ack *fr,
                         ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_frame_chain *frc = NULL;
  ngtcp2_rcvry_stat *rcs = &conn->rcs;
  ssize_t num_acked;

  rv = ngtcp2_pkt_validate_ack(fr);
  if (rv != 0) {
    return rv;
  }

  ngtcp2_acktr_recv_ack(&pktns->acktr, fr);

  num_acked = ngtcp2_rtb_recv_ack(&pktns->rtb, fr, conn, ts);
  if (num_acked < 0) {
    /* TODO assert this */
    assert(ngtcp2_err_is_fatal((int)num_acked));
    ngtcp2_frame_chain_list_del(frc, conn->mem);
    return (int)num_acked;
  }

  if (num_acked == 0) {
    return 0;
  }

  rv = ngtcp2_conn_detect_lost_pkt(conn, pktns, &conn->rcs, ts);
  if (rv != 0) {
    return rv;
  }

  rcs->crypto_count = 0;
  rcs->pto_count = 0;
  rcs->probe_pkt_left = 0;

  ngtcp2_conn_set_loss_detection_timer(conn);

  return 0;
}

/*
 * conn_assign_recved_ack_delay_unscaled assigns
 * fr->ack_delay_unscaled.
 */
static void assign_recved_ack_delay_unscaled(ngtcp2_ack *fr,
                                             uint64_t ack_delay_exponent) {
  fr->ack_delay_unscaled = fr->ack_delay * (1UL << ack_delay_exponent) *
                           (NGTCP2_DURATION_TICK / NGTCP2_MICROSECONDS);
}

/*
 * conn_recv_max_stream_data processes received MAX_STREAM_DATA frame
 * |fr|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_STREAM_STATE
 *     Stream ID indicates that it is a local stream, and the local
 *     endpoint has not initiated it; or stream is peer initiated
 *     unidirectional stream.
 * NGTCP2_ERR_STREAM_LIMIT
 *     Stream ID exceeds allowed limit.
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_recv_max_stream_data(ngtcp2_conn *conn,
                                     const ngtcp2_max_stream_data *fr) {
  ngtcp2_strm *strm;
  ngtcp2_idtr *idtr;
  int local_stream = conn_local_stream(conn, fr->stream_id);
  int bidi = bidi_stream(fr->stream_id);
  int rv;

  if (bidi) {
    if (local_stream) {
      if (conn->local.bidi.next_stream_id <= fr->stream_id) {
        return NGTCP2_ERR_STREAM_STATE;
      }
    } else if (conn->remote.bidi.max_streams <
               ngtcp2_ord_stream_id(fr->stream_id)) {
      return NGTCP2_ERR_STREAM_LIMIT;
    }

    idtr = &conn->remote.bidi.idtr;
  } else {
    if (!local_stream || conn->local.uni.next_stream_id <= fr->stream_id) {
      return NGTCP2_ERR_STREAM_STATE;
    }

    idtr = &conn->remote.uni.idtr;
  }

  strm = ngtcp2_conn_find_stream(conn, fr->stream_id);
  if (strm == NULL) {
    if (local_stream) {
      /* Stream has been closed. */
      return 0;
    }

    rv = ngtcp2_idtr_open(idtr, fr->stream_id);
    if (rv != 0) {
      if (ngtcp2_err_is_fatal(rv)) {
        return rv;
      }
      assert(rv == NGTCP2_ERR_STREAM_IN_USE);
      /* Stream has been closed. */
      return 0;
    }

    strm = ngtcp2_mem_malloc(conn->mem, sizeof(ngtcp2_strm));
    if (strm == NULL) {
      return NGTCP2_ERR_NOMEM;
    }
    rv = ngtcp2_conn_init_stream(conn, strm, fr->stream_id, NULL);
    if (rv != 0) {
      ngtcp2_mem_free(conn->mem, strm);
      return rv;
    }
  }

  if (strm->tx.max_offset < fr->max_stream_data) {
    strm->tx.max_offset = fr->max_stream_data;

    rv = conn_call_extend_max_stream_data(conn, strm, fr->stream_id,
                                          fr->max_stream_data);
    if (rv != 0) {
      return rv;
    }
  }

  return 0;
}

/*
 * conn_recv_max_data processes received MAX_DATA frame |fr|.
 */
static void conn_recv_max_data(ngtcp2_conn *conn, const ngtcp2_max_data *fr) {
  conn->tx.max_offset = ngtcp2_max(conn->tx.max_offset, fr->max_data);
}

/*
 * conn_buffer_pkt buffers |pkt| of length |pktlen|, chaining it from
 * |*ppc|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_buffer_pkt(ngtcp2_conn *conn, ngtcp2_pktns *pktns,
                           const ngtcp2_path *path, const uint8_t *pkt,
                           size_t pktlen, ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_pkt_chain **ppc = &pktns->rx.buffed_pkts, *pc;
  size_t i;
  for (i = 0; *ppc && i < NGTCP2_MAX_NUM_BUFFED_RX_PKTS;
       ppc = &(*ppc)->next, ++i)
    ;

  if (i == NGTCP2_MAX_NUM_BUFFED_RX_PKTS) {
    return 0;
  }

  rv = ngtcp2_pkt_chain_new(&pc, path, pkt, pktlen, ts, conn->mem);
  if (rv != 0) {
    return rv;
  }

  *ppc = pc;

  return 0;
}

/*
 * conn_ensure_decrypt_buffer ensures that conn->crypto.decrypt_buf
 * has at least |n| bytes space.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_ensure_decrypt_buffer(ngtcp2_conn *conn, size_t n) {
  uint8_t *nbuf;
  size_t len;
  ngtcp2_array *decrypt_buf = &conn->crypto.decrypt_buf;

  if (decrypt_buf->len >= n) {
    return 0;
  }

  len = decrypt_buf->len == 0 ? 2048 : decrypt_buf->len * 2;
  for (; len < n; len *= 2)
    ;
  nbuf = ngtcp2_mem_realloc(conn->mem, decrypt_buf->base, len);
  if (nbuf == NULL) {
    return NGTCP2_ERR_NOMEM;
  }
  decrypt_buf->base = nbuf;
  decrypt_buf->len = len;

  return 0;
}

/*
 * conn_decrypt_pkt decrypts the data pointed by |payload| whose
 * length is |payloadlen|, and writes plaintext data to the buffer
 * pointed by |dest| whose capacity is |destlen|.  The buffer pointed
 * by |ad| is the Additional Data, and its length is |adlen|.
 * |pkt_num| is used to create a nonce.  |ckm| is the cryptographic
 * key, and iv to use.  |decrypt| is a callback function which
 * actually decrypts a packet.
 *
 * This function returns the number of bytes written in |dest| if it
 * succeeds, or one of the following negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed.
 * NGTCP2_ERR_TLS_DECRYPT
 *     TLS backend failed to decrypt data.
 */
static ssize_t conn_decrypt_pkt(ngtcp2_conn *conn, uint8_t *dest,
                                size_t destlen, const uint8_t *payload,
                                size_t payloadlen, const uint8_t *ad,
                                size_t adlen, int64_t pkt_num,
                                ngtcp2_crypto_km *ckm, ngtcp2_decrypt decrypt) {
  /* TODO nonce is limited to 64 bytes. */
  uint8_t nonce[64];
  ssize_t nwrite;

  assert(sizeof(nonce) >= ckm->iv.len);

  ngtcp2_crypto_create_nonce(nonce, ckm->iv.base, ckm->iv.len, pkt_num);

  nwrite =
      decrypt(conn, dest, destlen, payload, payloadlen, ckm->key.base,
              ckm->key.len, nonce, ckm->iv.len, ad, adlen, conn->user_data);

  if (nwrite < 0) {
    if (nwrite == NGTCP2_ERR_TLS_DECRYPT) {
      return nwrite;
    }
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return nwrite;
}

/*
 * conn_decrypt_hp decryptes packet header.  The packet number starts
 * at |pkt| + |pkt_num_offset|.  The entire plaintext QUIC packer
 * header will be written to the buffer pointed by |dest| whose
 * capacity is |destlen|.
 *
 * This function returns the number of bytes written to |dest|, or one
 * of the following negative error codes:
 *
 * NGTCP2_ERR_PROTO
 *     Packet is badly formatted
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed; or it does not return
 *     expected result.
 */
static ssize_t conn_decrypt_hp(ngtcp2_conn *conn, ngtcp2_pkt_hd *hd,
                               uint8_t *dest, size_t destlen,
                               const uint8_t *pkt, size_t pktlen,
                               size_t pkt_num_offset, ngtcp2_crypto_km *ckm,
                               const ngtcp2_vec *hp, ngtcp2_hp_mask hp_mask,
                               size_t aead_overhead) {
  ssize_t nwrite;
  size_t sample_offset;
  uint8_t *p = dest;
  uint8_t mask[NGTCP2_HP_SAMPLELEN];
  size_t i;

  assert(hp_mask);
  assert(ckm);
  assert(aead_overhead >= NGTCP2_HP_SAMPLELEN);
  assert(destlen >= pkt_num_offset + 4);

  if (pkt_num_offset + NGTCP2_HP_SAMPLELEN > pktlen) {
    return NGTCP2_ERR_PROTO;
  }

  p = ngtcp2_cpymem(p, pkt, pkt_num_offset);

  sample_offset = pkt_num_offset + 4;

  nwrite = hp_mask(conn, mask, sizeof(mask), hp->base, hp->len,
                   pkt + sample_offset, NGTCP2_HP_SAMPLELEN, conn->user_data);
  if (nwrite < NGTCP2_HP_MASKLEN) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  if (hd->flags & NGTCP2_PKT_FLAG_LONG_FORM) {
    dest[0] = (uint8_t)(dest[0] ^ (mask[0] & 0x0f));
  } else {
    dest[0] = (uint8_t)(dest[0] ^ (mask[0] & 0x1f));
    if (dest[0] & NGTCP2_SHORT_KEY_PHASE_BIT) {
      hd->flags |= NGTCP2_PKT_FLAG_KEY_PHASE;
    }
  }

  hd->pkt_numlen = (size_t)((dest[0] & NGTCP2_PKT_NUMLEN_MASK) + 1);

  for (i = 0; i < hd->pkt_numlen; ++i) {
    *p++ = *(pkt + pkt_num_offset + i) ^ mask[i + 1];
  }

  hd->pkt_num = ngtcp2_get_pkt_num(p - hd->pkt_numlen, hd->pkt_numlen);

  return p - dest;
}

/*
 * conn_emit_pending_crypto_data delivers pending stream data to the
 * application due to packet reordering.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed
 * NGTCP2_ERR_CRYPTO
 *     TLS backend reported error
 */
static int conn_emit_pending_crypto_data(ngtcp2_conn *conn,
                                         ngtcp2_crypto_level crypto_level,
                                         ngtcp2_strm *strm,
                                         uint64_t rx_offset) {
  size_t datalen;
  const uint8_t *data;
  int rv;
  uint64_t offset;

  for (;;) {
    datalen = ngtcp2_rob_data_at(&strm->rx.rob, &data, rx_offset);
    if (datalen == 0) {
      assert(rx_offset == ngtcp2_strm_rx_offset(strm));
      return 0;
    }

    offset = rx_offset;
    rx_offset += datalen;

    rv = conn_call_recv_crypto_data(conn, crypto_level, offset, data, datalen);
    if (rv != 0) {
      return rv;
    }

    ngtcp2_rob_pop(&strm->rx.rob, rx_offset - datalen, datalen);
  }
}

/*
 * conn_recv_connection_close is called when CONNECTION_CLOSE or
 * APPLICATION_CLOSE frame is received.
 */
static void conn_recv_connection_close(ngtcp2_conn *conn) {
  conn->state = NGTCP2_CS_DRAINING;
}

static void conn_recv_path_challenge(ngtcp2_conn *conn, const ngtcp2_path *path,
                                     ngtcp2_path_challenge *fr) {
  ngtcp2_path_challenge_entry *ent;

  ent = ngtcp2_ringbuf_push_front(&conn->rx.path_challenge);
  ngtcp2_path_challenge_entry_init(ent, path, fr->data);
}

static int conn_recv_path_response(ngtcp2_conn *conn, const ngtcp2_path *path,
                                   ngtcp2_path_response *fr, ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_pv *pv = conn->pv, *npv = NULL;
  ngtcp2_duration timeout;

  if (!pv) {
    return 0;
  }

  rv = ngtcp2_pv_validate(pv, path, fr->data);
  if (rv != 0) {
    if (rv == NGTCP2_ERR_PATH_VALIDATION_FAILED) {
      return conn_on_path_validation_failed(conn, pv, ts);
    }
    return 0;
  }

  /* If validation succeeds, we don't have to throw DCID away. */
  pv->flags &= (uint8_t)~NGTCP2_PV_FLAG_RETIRE_DCID_ON_FINISH;

  if (pv->flags & NGTCP2_PV_FLAG_FALLBACK_ON_FAILURE) {
    timeout = conn_compute_pto(conn);
    timeout = ngtcp2_max(timeout,
                         (ngtcp2_duration)(6ULL * NGTCP2_DEFAULT_INITIAL_RTT));

    rv = ngtcp2_pv_new(&npv, &pv->fallback_dcid, timeout,
                       NGTCP2_PV_FLAG_RETIRE_DCID_ON_FINISH, &conn->log,
                       conn->mem);
    if (rv != 0) {
      return rv;
    }
  }

  /* TODO Retire all DCIDs in conn->bound_dcid */

  if (!(pv->flags & NGTCP2_PV_FLAG_DONT_CARE)) {
    if (!(pv->flags & NGTCP2_PV_FLAG_FALLBACK_ON_FAILURE)) {
      rv = conn_retire_dcid(conn, &conn->dcid.current, ts);
      if (rv != 0) {
        goto fail;
      }
      ngtcp2_dcid_copy(&conn->dcid.current, &pv->dcid);
    }

    rv = conn_call_path_validation(conn, &pv->dcid.ps.path,
                                   NGTCP2_PATH_VALIDATION_RESULT_SUCCESS);
    if (rv != 0) {
      goto fail;
    }
  }

  rv = conn_stop_pv(conn, ts);
  if (rv != 0) {
    goto fail;
  }

  conn->pv = npv;

  return 0;

fail:
  ngtcp2_pv_del(npv);

  return rv;
}

/*
 * conn_update_rx_bw updates rx bandwidth.
 */
static void conn_update_rx_bw(ngtcp2_conn *conn, size_t datalen,
                              ngtcp2_tstamp ts) {
  /* Reset bandwidth measurement after 1 second idle time. */
  if (conn->rx.bw.last_ts == 0 || ts - conn->rx.bw.last_ts > NGTCP2_SECONDS) {
    conn->rx.bw.first_ts = ts;
    conn->rx.bw.last_ts = ts;
    conn->rx.bw.datalen = datalen;
    conn->rx.bw.value = 0.;
    return;
  }

  conn->rx.bw.last_ts = ts;
  conn->rx.bw.datalen += datalen;

  if (ts - conn->rx.bw.first_ts >= 25 * NGTCP2_MILLISECONDS) {
    conn->rx.bw.value =
        (double)conn->rx.bw.datalen / (double)(ts - conn->rx.bw.first_ts);

    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON, "rx_bw=%.02fBs",
                    conn->rx.bw.value * NGTCP2_DURATION_TICK);
  }
}

/*
 * pkt_num_bits returns the number of bits available when packet
 * number is encoded in |pkt_numlen| bytes.
 */
static size_t pkt_num_bits(size_t pkt_numlen) {
  switch (pkt_numlen) {
  case 1:
    return 8;
  case 2:
    return 16;
  case 3:
    return 24;
  case 4:
    return 32;
  default:
    assert(0);
  }
}

/*
 * pktns_pkt_num_is_duplicate returns nonzero if |pkt_num| is
 * duplicated packet number.
 */
static int pktns_pkt_num_is_duplicate(ngtcp2_pktns *pktns, int64_t pkt_num) {
  return ngtcp2_gaptr_is_pushed(&pktns->rx.pngap, (uint64_t)pkt_num, 1);
}

/*
 * pktns_commit_recv_pkt_num marks packet number |pkt_num| as
 * received.
 */
static int pktns_commit_recv_pkt_num(ngtcp2_pktns *pktns, int64_t pkt_num) {
  int rv;
  ngtcp2_ksl_it it;
  ngtcp2_ksl_key key;
  ngtcp2_range range;

  if (pktns->rx.max_pkt_num + 1 != pkt_num) {
    ngtcp2_acktr_immediate_ack(&pktns->acktr);
  }
  if (pktns->rx.max_pkt_num < pkt_num) {
    pktns->rx.max_pkt_num = pkt_num;
  }

  rv = ngtcp2_gaptr_push(&pktns->rx.pngap, (uint64_t)pkt_num, 1);
  if (rv != 0) {
    return rv;
  }

  if (ngtcp2_ksl_len(&pktns->rx.pngap.gap) > 256) {
    it = ngtcp2_ksl_begin(&pktns->rx.pngap.gap);
    range = *(ngtcp2_range *)ngtcp2_ksl_it_key(&it).ptr;
    ngtcp2_ksl_remove(&pktns->rx.pngap.gap, NULL,
                      ngtcp2_ksl_key_ptr(&key, &range));
  }

  return 0;
}

/*
 * conn_discard_initial_key discards Initial packet protection keys.
 */
static void conn_discard_initial_key(ngtcp2_conn *conn) {
  ngtcp2_pktns *pktns = &conn->in_pktns;

  if (conn->flags & NGTCP2_CONN_FLAG_INITIAL_KEY_DISCARDED) {
    return;
  }

  conn->flags |= NGTCP2_CONN_FLAG_INITIAL_KEY_DISCARDED;

  ngtcp2_crypto_km_del(pktns->crypto.tx.ckm, conn->mem);
  ngtcp2_crypto_km_del(pktns->crypto.rx.ckm, conn->mem);

  pktns->crypto.tx.ckm = NULL;
  pktns->crypto.rx.ckm = NULL;

  ngtcp2_rtb_clear(&pktns->rtb);
  ngtcp2_acktr_commit_ack(&pktns->acktr);
}

static int conn_recv_crypto(ngtcp2_conn *conn, ngtcp2_crypto_level crypto_level,
                            ngtcp2_strm *strm, const ngtcp2_crypto *fr);

static ssize_t conn_recv_pkt(ngtcp2_conn *conn, const ngtcp2_path *path,
                             const uint8_t *pkt, size_t pktlen,
                             ngtcp2_tstamp ts);

/*
 * conn_recv_handshake_pkt processes received packet |pkt| whose
 * length is |pktlen| during handshake period.  The buffer pointed by
 * |pkt| might contain multiple packets.  This function only processes
 * one packet.
 *
 * This function returns the number of bytes it reads if it succeeds,
 * or one of the following negative error codes:
 *
 * NGTCP2_ERR_RECV_VERSION_NEGOTIATION
 *     Version Negotiation packet is received.
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_DISCARD_PKT
 *     Packet was discarded because plain text header was malformed;
 *     or its payload could not be decrypted.
 * NGTCP2_ERR_FRAME_FORMAT
 *     Frame is badly formatted
 * NGTCP2_ERR_ACK_FRAME
 *     ACK frame is malformed.
 * NGTCP2_ERR_CRYPTO
 *     TLS stack reported error.
 * NGTCP2_ERR_PROTO
 *     Generic QUIC protocol error.
 *
 * In addition to the above error codes, error codes returned from
 * conn_recv_pkt are also returned.
 */
static ssize_t conn_recv_handshake_pkt(ngtcp2_conn *conn,
                                       const ngtcp2_path *path,
                                       const uint8_t *pkt, size_t pktlen,
                                       ngtcp2_tstamp ts) {
  ssize_t nread;
  ngtcp2_pkt_hd hd;
  ngtcp2_max_frame mfr;
  ngtcp2_frame *fr = &mfr.fr;
  int rv;
  int require_ack = 0;
  size_t hdpktlen;
  const uint8_t *payload;
  size_t payloadlen;
  ssize_t nwrite;
  uint8_t plain_hdpkt[1500];
  ngtcp2_crypto_km *ckm;
  const ngtcp2_vec *hp;
  ngtcp2_hp_mask hp_mask;
  ngtcp2_decrypt decrypt;
  size_t aead_overhead;
  ngtcp2_pktns *pktns;
  ngtcp2_strm *crypto;
  size_t odcil;
  ngtcp2_crypto_level crypto_level;
  int invalid_reserved_bits = 0;

  if (pktlen == 0) {
    return 0;
  }

  if (!(pkt[0] & NGTCP2_HEADER_FORM_BIT)) {
    if (conn->state == NGTCP2_CS_SERVER_INITIAL) {
      /* Ignore Short packet unless server's first Handshake packet
         has been transmitted. */
      return (ssize_t)pktlen;
    }

    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON,
                    "buffering Short packet len=%zu", pktlen);

    /* TODO It would be nicer not to buffer Short packet if we have
       1RTT key. */
    rv = conn_buffer_pkt(conn, &conn->hs_pktns, path, pkt, pktlen, ts);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal(rv));
      return rv;
    }
    return (ssize_t)pktlen;
  }

  nread = ngtcp2_pkt_decode_hd_long(&hd, pkt, pktlen);
  if (nread < 0) {
    return NGTCP2_ERR_DISCARD_PKT;
  }

  switch (hd.type) {
  case NGTCP2_PKT_VERSION_NEGOTIATION:
    hdpktlen = (size_t)nread;

    ngtcp2_log_rx_pkt_hd(&conn->log, &hd);

    if (conn->server) {
      return NGTCP2_ERR_DISCARD_PKT;
    }

    /* Receiving Version Negotiation packet after getting Handshake
       packet from server is invalid. */
    if (conn->flags & NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED) {
      return NGTCP2_ERR_DISCARD_PKT;
    }

    /* TODO Do not change state here? */
    rv = conn_verify_dcid(conn, &hd);
    if (rv != 0) {
      if (ngtcp2_err_is_fatal(rv)) {
        return rv;
      }
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "packet was ignored because of mismatched DCID");
      return NGTCP2_ERR_DISCARD_PKT;
    }

    if (!ngtcp2_cid_eq(&conn->dcid.current.cid, &hd.scid)) {
      /* Just discard invalid Version Negotiation packet */
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "packet was ignored because of mismatched SCID");
      return NGTCP2_ERR_DISCARD_PKT;
    }
    rv = conn_on_version_negotiation(conn, &hd, pkt + hdpktlen,
                                     pktlen - hdpktlen);
    if (rv != 0) {
      if (ngtcp2_err_is_fatal(rv)) {
        return rv;
      }
      return NGTCP2_ERR_DISCARD_PKT;
    }
    return NGTCP2_ERR_RECV_VERSION_NEGOTIATION;
  case NGTCP2_PKT_RETRY:
    hdpktlen = (size_t)nread;

    ngtcp2_log_rx_pkt_hd(&conn->log, &hd);

    if (conn->server) {
      return NGTCP2_ERR_DISCARD_PKT;
    }

    /* Receiving Retry packet after getting Initial packet from server
       is invalid. */
    if (conn->flags & NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED) {
      return NGTCP2_ERR_DISCARD_PKT;
    }

    odcil = pkt[0] & 0x0f;
    if (odcil) {
      odcil += 3;
    }
    rv = conn_on_retry(conn, &hd, odcil, pkt + hdpktlen, pktlen - hdpktlen);
    if (rv != 0) {
      if (ngtcp2_err_is_fatal(rv)) {
        return rv;
      }
      return NGTCP2_ERR_DISCARD_PKT;
    }
    return (ssize_t)pktlen;
  }

  if (pktlen < (size_t)nread + hd.len) {
    return NGTCP2_ERR_DISCARD_PKT;
  }

  pktlen = (size_t)nread + hd.len;

  if (conn->version != hd.version) {
    return NGTCP2_ERR_DISCARD_PKT;
  }

  /* Quoted from spec: if subsequent packets of those types include a
     different Source Connection ID, they MUST be discarded. */
  if ((conn->flags & NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED) &&
      !ngtcp2_cid_eq(&conn->dcid.current.cid, &hd.scid)) {
    ngtcp2_log_rx_pkt_hd(&conn->log, &hd);
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "packet was ignored because of mismatched SCID");
    return NGTCP2_ERR_DISCARD_PKT;
  }

  switch (hd.type) {
  case NGTCP2_PKT_0RTT:
    if (!conn->server) {
      return NGTCP2_ERR_DISCARD_PKT;
    }
    if (conn->flags & NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED) {
      if (conn->early.ckm) {
        ssize_t nread2;
        /* TODO Avoid to parse header twice. */
        nread2 =
            conn_recv_pkt(conn, &conn->dcid.current.ps.path, pkt, pktlen, ts);
        if (nread2 < 0) {
          return nread2;
        }
      }

      /* Discard 0-RTT packet if we don't have a key to decrypt it. */
      return (ssize_t)pktlen;
    }

    /* Buffer re-ordered 0-RTT packet. */
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON,
                    "buffering 0-RTT packet len=%zu", pktlen);

    rv = conn_buffer_pkt(conn, &conn->in_pktns, path, pkt, pktlen, ts);
    if (rv != 0) {
      assert(ngtcp2_err_is_fatal(rv));
      return rv;
    }

    return (ssize_t)pktlen;
  case NGTCP2_PKT_INITIAL:
    if (conn->flags & NGTCP2_CONN_FLAG_INITIAL_KEY_DISCARDED) {
      ngtcp2_log_info(
          &conn->log, NGTCP2_LOG_EVENT_PKT,
          "Initial packet is discarded because keys have been discarded");
      return (ssize_t)pktlen;
    }

    if (conn->server) {
      if ((conn->flags & NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED) == 0) {
        rv = conn_call_recv_client_initial(conn, &hd.dcid);
        if (rv != 0) {
          return rv;
        }
      }
    } else if (hd.tokenlen != 0) {
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "packet was ignored because token is not empty");
      return NGTCP2_ERR_DISCARD_PKT;
    }

    pktns = &conn->in_pktns;
    hp_mask = conn->callbacks.in_hp_mask;
    decrypt = conn->callbacks.in_decrypt;
    aead_overhead = NGTCP2_INITIAL_AEAD_OVERHEAD;
    crypto = &conn->in_pktns.crypto.strm;
    crypto_level = NGTCP2_CRYPTO_LEVEL_INITIAL;

    break;
  case NGTCP2_PKT_HANDSHAKE:
    if (!conn->hs_pktns.crypto.rx.ckm) {
      if (conn->server) {
        ngtcp2_log_info(
            &conn->log, NGTCP2_LOG_EVENT_PKT,
            "Handshake packet at this point is unexpected and discarded");
        return (ssize_t)pktlen;
      }
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON,
                      "buffering Handshake packet len=%zu", pktlen);

      rv = conn_buffer_pkt(conn, &conn->in_pktns, path, pkt, pktlen, ts);
      if (rv != 0) {
        assert(ngtcp2_err_is_fatal(rv));
        return rv;
      }
      return (ssize_t)pktlen;
    }

    pktns = &conn->hs_pktns;
    hp_mask = conn->callbacks.hp_mask;
    decrypt = conn->callbacks.decrypt;
    aead_overhead = conn->crypto.aead_overhead;
    crypto = &conn->hs_pktns.crypto.strm;
    crypto_level = NGTCP2_CRYPTO_LEVEL_HANDSHAKE;

    break;
  default:
    /* unknown packet type */
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "packet was ignored because of unknown packet type");
    return (ssize_t)pktlen;
  }

  ckm = pktns->crypto.rx.ckm;
  hp = pktns->crypto.rx.hp;

  assert(ckm);
  assert(hp_mask);
  assert(decrypt);

  nwrite =
      conn_decrypt_hp(conn, &hd, plain_hdpkt, sizeof(plain_hdpkt), pkt, pktlen,
                      (size_t)nread, ckm, hp, hp_mask, aead_overhead);
  if (nwrite < 0) {
    if (ngtcp2_err_is_fatal((int)nwrite)) {
      return nwrite;
    }
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "could not decrypt packet number");
    return NGTCP2_ERR_DISCARD_PKT;
  }

  hdpktlen = (size_t)nwrite;
  payload = pkt + hdpktlen;
  payloadlen = hd.len - hd.pkt_numlen;

  hd.pkt_num = ngtcp2_pkt_adjust_pkt_num(pktns->rx.max_pkt_num, hd.pkt_num,
                                         pkt_num_bits(hd.pkt_numlen));

  ngtcp2_log_rx_pkt_hd(&conn->log, &hd);

  rv = ngtcp2_pkt_verify_reserved_bits(plain_hdpkt[0]);
  if (rv != 0) {
    invalid_reserved_bits = 1;

    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "packet has incorrect reserved bits");

    /* Will return error after decrypting payload */
  }

  if (pktns_pkt_num_is_duplicate(pktns, hd.pkt_num)) {
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "packet was discarded because of duplicated packet number");
    return NGTCP2_ERR_DISCARD_PKT;
  }

  rv = conn_ensure_decrypt_buffer(conn, payloadlen);
  if (rv != 0) {
    return rv;
  }

  nwrite = conn_decrypt_pkt(conn, conn->crypto.decrypt_buf.base, payloadlen,
                            payload, payloadlen, plain_hdpkt, hdpktlen,
                            hd.pkt_num, ckm, decrypt);
  if (nwrite < 0) {
    if (ngtcp2_err_is_fatal((int)nwrite)) {
      return nwrite;
    }
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "could not decrypt packet payload");
    return NGTCP2_ERR_DISCARD_PKT;
  }

  if (invalid_reserved_bits) {
    return NGTCP2_ERR_PROTO;
  }

  payload = conn->crypto.decrypt_buf.base;
  payloadlen = (size_t)nwrite;

  switch (hd.type) {
  case NGTCP2_PKT_INITIAL:
    if (!conn->server || ((conn->flags & NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED) &&
                          !ngtcp2_cid_eq(&conn->rcid, &hd.dcid))) {
      rv = conn_verify_dcid(conn, &hd);
      if (rv != 0) {
        if (ngtcp2_err_is_fatal(rv)) {
          return rv;
        }
        ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                        "packet was ignored because of mismatched DCID");
        return NGTCP2_ERR_DISCARD_PKT;
      }
    }
    break;
  case NGTCP2_PKT_HANDSHAKE:
    rv = conn_verify_dcid(conn, &hd);
    if (rv != 0) {
      if (ngtcp2_err_is_fatal(rv)) {
        return rv;
      }
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "packet was ignored because of mismatched DCID");
      return NGTCP2_ERR_DISCARD_PKT;
    }
    break;
  default:
    assert(0);
  }

  if (payloadlen == 0) {
    /* QUIC packet must contain at least one frame */
    return NGTCP2_ERR_DISCARD_PKT;
  }

  if (hd.type == NGTCP2_PKT_INITIAL &&
      !(conn->flags & NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED)) {
    conn->flags |= NGTCP2_CONN_FLAG_CONN_ID_NEGOTIATED;
    if (conn->server) {
      conn->rcid = hd.dcid;
    } else {
      conn->dcid.current.cid = hd.scid;
    }
    conn->odcid = hd.scid;
  }

  for (; payloadlen;) {
    nread = ngtcp2_pkt_decode_frame(fr, payload, payloadlen);
    if (nread < 0) {
      return (int)nread;
    }

    payload += nread;
    payloadlen -= (size_t)nread;

    if (fr->type == NGTCP2_FRAME_ACK) {
      assign_recved_ack_delay_unscaled(&fr->ack,
                                       NGTCP2_DEFAULT_ACK_DELAY_EXPONENT);
    }

    ngtcp2_log_rx_fr(&conn->log, &hd, fr);

    switch (fr->type) {
    case NGTCP2_FRAME_ACK:
    case NGTCP2_FRAME_ACK_ECN:
      rv = conn_recv_ack(conn, pktns, &fr->ack, ts);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGTCP2_FRAME_PADDING:
      break;
    case NGTCP2_FRAME_CRYPTO:
      rv = conn_recv_crypto(conn, crypto_level, crypto, &fr->crypto);
      if (rv != 0) {
        return rv;
      }
      require_ack = 1;
      break;
    case NGTCP2_FRAME_CONNECTION_CLOSE:
      conn_recv_connection_close(conn);
      break;
    case NGTCP2_FRAME_CONNECTION_CLOSE_APP:
      if (fr->type != NGTCP2_PKT_HANDSHAKE) {
        return NGTCP2_ERR_PROTO;
      }
      conn_recv_connection_close(conn);
      break;
    default:
      return NGTCP2_ERR_PROTO;
    }
  }

  if (conn->server) {
    switch (hd.type) {
    case NGTCP2_PKT_INITIAL:
      if (ngtcp2_rob_first_gap_offset(&crypto->rx.rob) == 0) {
        return NGTCP2_ERR_PROTO;
      }
      break;
    case NGTCP2_PKT_HANDSHAKE:
      if (conn->server && hd.type == NGTCP2_PKT_HANDSHAKE) {
        /* Successful processing of Handshake packet from client verifies
           source address. */
        conn->flags |= NGTCP2_CONN_FLAG_SADDR_VERIFIED;
      }
      break;
    }
  }

  rv = pktns_commit_recv_pkt_num(pktns, hd.pkt_num);
  if (rv != 0) {
    return rv;
  }

  if (require_ack && ++pktns->acktr.rx_npkt >= NGTCP2_NUM_IMMEDIATE_ACK_PKT) {
    ngtcp2_acktr_immediate_ack(&pktns->acktr);
  }

  rv = ngtcp2_conn_sched_ack(conn, &pktns->acktr, hd.pkt_num, require_ack, ts);
  if (rv != 0) {
    return rv;
  }

  return conn->state == NGTCP2_CS_DRAINING ? NGTCP2_ERR_DRAINING
                                           : (ssize_t)pktlen;
}

/*
 * conn_recv_handshake_cpkt processes compound packet during
 * handshake.  The buffer pointed by |pkt| might contain multiple
 * packets.  The Short packet must be the last one because it does not
 * have payload length field.
 *
 * This function returns the same error code returned by
 * conn_recv_handshake_pkt.
 */
static int conn_recv_handshake_cpkt(ngtcp2_conn *conn, const ngtcp2_path *path,
                                    const uint8_t *pkt, size_t pktlen,
                                    ngtcp2_tstamp ts) {
  ssize_t nread;
  size_t origlen = pktlen;

  while (pktlen) {
    nread = conn_recv_handshake_pkt(conn, path, pkt, pktlen, ts);
    if (nread < 0) {
      if (ngtcp2_err_is_fatal((int)nread)) {
        return (int)nread;
      }
      if (nread == NGTCP2_ERR_DISCARD_PKT) {
        return 0;
      }
      if (nread != NGTCP2_ERR_CRYPTO && (pkt[0] & NGTCP2_HEADER_FORM_BIT) &&
          ngtcp2_pkt_get_type_long(pkt[0]) == NGTCP2_PKT_INITIAL) {
        return 0;
      }
      return (int)nread;
    }

    assert(pktlen >= (size_t)nread);
    pkt += nread;
    pktlen -= (size_t)nread;

    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "read packet %zd left %zu", nread, pktlen);
  }

  conn->hs_recved += origlen;

  return 0;
}

int ngtcp2_conn_init_stream(ngtcp2_conn *conn, ngtcp2_strm *strm,
                            int64_t stream_id, void *stream_user_data) {
  int rv;
  uint64_t max_rx_offset;
  uint64_t max_tx_offset;
  int local_stream = conn_local_stream(conn, stream_id);

  if (bidi_stream(stream_id)) {
    if (local_stream) {
      max_rx_offset = conn->local.settings.max_stream_data_bidi_local;
      max_tx_offset = conn->remote.settings.max_stream_data_bidi_remote;
    } else {
      max_rx_offset = conn->local.settings.max_stream_data_bidi_remote;
      max_tx_offset = conn->remote.settings.max_stream_data_bidi_local;
    }
  } else if (local_stream) {
    max_rx_offset = 0;
    max_tx_offset = conn->remote.settings.max_stream_data_uni;
  } else {
    max_rx_offset = conn->local.settings.max_stream_data_uni;
    max_tx_offset = 0;
  }

  rv = ngtcp2_strm_init(strm, stream_id, NGTCP2_STRM_FLAG_NONE, max_rx_offset,
                        max_tx_offset, stream_user_data, conn->mem);
  if (rv != 0) {
    return rv;
  }

  rv = ngtcp2_map_insert(&conn->strms, &strm->me);
  if (rv != 0) {
    assert(rv != NGTCP2_ERR_INVALID_ARGUMENT);
    goto fail;
  }

  if (!conn_local_stream(conn, stream_id)) {
    rv = conn_call_stream_open(conn, strm);
    if (rv != 0) {
      goto fail;
    }
  }

  return 0;

fail:
  ngtcp2_strm_free(strm);
  return rv;
}

/*
 * conn_emit_pending_stream_data passes buffered ordered stream data
 * to the application.  |rx_offset| is the first offset to deliver to
 * the application.  This function assumes that the data up to
 * |rx_offset| has been delivered already.  This function only passes
 * the ordered data without any gap.  If there is a gap, it stops
 * providing the data to the application, and returns.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed.
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_emit_pending_stream_data(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                         uint64_t rx_offset) {
  size_t datalen;
  const uint8_t *data;
  int rv;
  uint64_t offset;

  for (;;) {
    datalen = ngtcp2_rob_data_at(&strm->rx.rob, &data, rx_offset);
    if (datalen == 0) {
      assert(rx_offset == ngtcp2_strm_rx_offset(strm));
      return 0;
    }

    offset = rx_offset;
    rx_offset += datalen;

    rv = conn_call_recv_stream_data(conn, strm,
                                    (strm->flags & NGTCP2_STRM_FLAG_SHUT_RD) &&
                                        rx_offset == strm->rx.last_offset,
                                    offset, data, datalen);
    if (rv != 0) {
      return rv;
    }

    ngtcp2_rob_pop(&strm->rx.rob, rx_offset - datalen, datalen);
  }
}

/*
 * conn_recv_crypto is called when CRYPTO frame |fr| is received.
 * |rx_offset_base| is the offset in the entire TLS handshake stream.
 * fr->offset specifies the offset in each encryption level.
 * |max_rx_offset| is, if it is nonzero, the maximum offset in the
 * entire TLS handshake stream that |fr| can carry.  |crypto_level| is
 * the encryption level where this data is received.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_PROTO
 *     CRYPTO frame has invalid offset.
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CRYPTO
 *     TLS stack reported error.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 */
static int conn_recv_crypto(ngtcp2_conn *conn, ngtcp2_crypto_level crypto_level,
                            ngtcp2_strm *crypto, const ngtcp2_crypto *fr) {
  uint64_t fr_end_offset;
  uint64_t rx_offset;
  int rv;

  if (fr->datacnt == 0) {
    return 0;
  }

  fr_end_offset = fr->offset + fr->data[0].len;

  if (NGTCP2_MAX_VARINT < fr_end_offset) {
    return NGTCP2_ERR_PROTO;
  }

  rx_offset = ngtcp2_strm_rx_offset(crypto);

  if (fr_end_offset <= rx_offset) {
    return 0;
  }

  crypto->rx.last_offset = ngtcp2_max(crypto->rx.last_offset, fr_end_offset);

  /* TODO Before dispatching incoming data to TLS stack, make sure
     that previous data in previous encryption level has been
     completely sent to TLS stack.  Usually, if data is left, it is an
     error because key is generated after consuming all data in the
     previous encryption level. */
  if (fr->offset <= rx_offset) {
    size_t ncut = rx_offset - fr->offset;
    const uint8_t *data = fr->data[0].base + ncut;
    size_t datalen = fr->data[0].len - ncut;
    uint64_t offset = rx_offset;

    rx_offset += datalen;
    rv = ngtcp2_rob_remove_prefix(&crypto->rx.rob, rx_offset);
    if (rv != 0) {
      return rv;
    }

    rv = conn_call_recv_crypto_data(conn, crypto_level, offset, data, datalen);
    if (rv != 0) {
      return rv;
    }

    rv = conn_emit_pending_crypto_data(conn, crypto_level, crypto, rx_offset);
    if (rv != 0) {
      return rv;
    }
  } else if (fr_end_offset - rx_offset > NGTCP2_MAX_REORDERED_CRYPTO_DATA) {
    return NGTCP2_ERR_CRYPTO_BUFFER_EXCEEDED;
  } else {
    rv = ngtcp2_strm_recv_reordering(crypto, fr->data[0].base, fr->data[0].len,
                                     fr->offset);
    if (rv != 0) {
      return rv;
    }
  }

  return 0;
}

/*
 * conn_max_data_violated returns nonzero if receiving |datalen|
 * violates connection flow control on local endpoint.
 */
static int conn_max_data_violated(ngtcp2_conn *conn, size_t datalen) {
  return conn->rx.max_offset - conn->rx.offset < datalen;
}

/*
 * conn_recv_stream is called when STREAM frame |fr| is received.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_STREAM_STATE
 *     STREAM frame is received for a local stream which is not
 *     initiated; or STREAM frame is received for a local
 *     unidirectional stream
 * NGTCP2_ERR_STREAM_LIMIT
 *     STREAM frame has remote stream ID which is strictly greater
 *     than the allowed limit.
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_FLOW_CONTROL
 *     Flow control limit is violated; or the end offset of stream
 *     data is beyond the NGTCP2_MAX_VARINT.
 * NGTCP2_ERR_FINAL_SIZE
 *     STREAM frame has strictly larger end offset than it is
 *     permitted.
 */
static int conn_recv_stream(ngtcp2_conn *conn, const ngtcp2_stream *fr) {
  int rv;
  ngtcp2_strm *strm;
  ngtcp2_idtr *idtr;
  uint64_t rx_offset, fr_end_offset;
  int local_stream;
  int bidi;
  size_t datalen = ngtcp2_vec_len(fr->data, fr->datacnt);

  local_stream = conn_local_stream(conn, fr->stream_id);
  bidi = bidi_stream(fr->stream_id);

  if (bidi) {
    if (local_stream) {
      if (conn->local.bidi.next_stream_id <= fr->stream_id) {
        return NGTCP2_ERR_STREAM_STATE;
      }
    } else if (conn->remote.bidi.max_streams <
               ngtcp2_ord_stream_id(fr->stream_id)) {
      return NGTCP2_ERR_STREAM_LIMIT;
    }

    idtr = &conn->remote.bidi.idtr;
  } else {
    if (local_stream) {
      return NGTCP2_ERR_STREAM_STATE;
    }
    if (conn->remote.uni.max_streams < ngtcp2_ord_stream_id(fr->stream_id)) {
      return NGTCP2_ERR_STREAM_LIMIT;
    }

    idtr = &conn->remote.uni.idtr;
  }

  if (NGTCP2_MAX_VARINT - datalen < fr->offset) {
    return NGTCP2_ERR_FLOW_CONTROL;
  }

  strm = ngtcp2_conn_find_stream(conn, fr->stream_id);
  if (strm == NULL) {
    if (local_stream) {
      /* TODO The stream has been closed.  This should be responded
         with RESET_STREAM, or simply ignored. */
      return 0;
    }

    rv = ngtcp2_idtr_open(idtr, fr->stream_id);
    if (rv != 0) {
      if (ngtcp2_err_is_fatal(rv)) {
        return rv;
      }
      assert(rv == NGTCP2_ERR_STREAM_IN_USE);
      /* TODO The stream has been closed.  This should be responded
         with RESET_STREAM, or simply ignored. */
      return 0;
    }

    strm = ngtcp2_mem_malloc(conn->mem, sizeof(ngtcp2_strm));
    if (strm == NULL) {
      return NGTCP2_ERR_NOMEM;
    }
    /* TODO Perhaps, call new_stream callback? */
    rv = ngtcp2_conn_init_stream(conn, strm, fr->stream_id, NULL);
    if (rv != 0) {
      ngtcp2_mem_free(conn->mem, strm);
      return rv;
    }
    if (!bidi) {
      ngtcp2_strm_shutdown(strm, NGTCP2_STRM_FLAG_SHUT_WR);
    }
  }

  fr_end_offset = fr->offset + datalen;

  if (strm->rx.max_offset < fr_end_offset) {
    return NGTCP2_ERR_FLOW_CONTROL;
  }

  if (strm->rx.last_offset < fr_end_offset) {
    size_t len = fr_end_offset - strm->rx.last_offset;

    if (conn_max_data_violated(conn, len)) {
      return NGTCP2_ERR_FLOW_CONTROL;
    }

    conn->rx.offset += len;

    if (strm->flags & NGTCP2_STRM_FLAG_STOP_SENDING) {
      ngtcp2_conn_extend_max_offset(conn, len);
    }
  }

  rx_offset = ngtcp2_strm_rx_offset(strm);

  if (fr->fin) {
    if (strm->flags & NGTCP2_STRM_FLAG_SHUT_RD) {
      if (strm->rx.last_offset != fr_end_offset) {
        return NGTCP2_ERR_FINAL_SIZE;
      }

      if (strm->flags &
          (NGTCP2_STRM_FLAG_STOP_SENDING | NGTCP2_STRM_FLAG_RECV_RST)) {
        return 0;
      }

      if (rx_offset == fr_end_offset) {
        return 0;
      }
    } else if (strm->rx.last_offset > fr_end_offset) {
      return NGTCP2_ERR_FINAL_SIZE;
    } else {
      strm->rx.last_offset = fr_end_offset;

      ngtcp2_strm_shutdown(strm, NGTCP2_STRM_FLAG_SHUT_RD);

      if (strm->flags & NGTCP2_STRM_FLAG_STOP_SENDING) {
        return ngtcp2_conn_close_stream_if_shut_rdwr(conn, strm,
                                                     strm->app_error_code);
      }

      if (fr_end_offset == rx_offset) {
        rv = conn_call_recv_stream_data(conn, strm, 1, rx_offset, NULL, 0);
        if (rv != 0) {
          return rv;
        }
        return ngtcp2_conn_close_stream_if_shut_rdwr(conn, strm,
                                                     NGTCP2_NO_ERROR);
      }
    }
  } else {
    if ((strm->flags & NGTCP2_STRM_FLAG_SHUT_RD) &&
        strm->rx.last_offset < fr_end_offset) {
      return NGTCP2_ERR_FINAL_SIZE;
    }

    strm->rx.last_offset = ngtcp2_max(strm->rx.last_offset, fr_end_offset);

    if (fr_end_offset <= rx_offset) {
      return 0;
    }

    if (strm->flags &
        (NGTCP2_STRM_FLAG_STOP_SENDING | NGTCP2_STRM_FLAG_RECV_RST)) {
      return 0;
    }
  }

  if (fr->offset <= rx_offset) {
    size_t ncut = rx_offset - fr->offset;
    uint64_t offset = rx_offset;
    const uint8_t *data;
    int fin;

    if (fr->datacnt) {
      data = fr->data[0].base + ncut;
      datalen -= ncut;

      rx_offset += datalen;
      rv = ngtcp2_rob_remove_prefix(&strm->rx.rob, rx_offset);
      if (rv != 0) {
        return rv;
      }
    } else {
      data = NULL;
      datalen = 0;
    }

    fin = (strm->flags & NGTCP2_STRM_FLAG_SHUT_RD) &&
          rx_offset == strm->rx.last_offset;

    if (fin || datalen) {
      rv = conn_call_recv_stream_data(conn, strm, fin, offset, data, datalen);
      if (rv != 0) {
        return rv;
      }

      rv = conn_emit_pending_stream_data(conn, strm, rx_offset);
      if (rv != 0) {
        return rv;
      }
    }
  } else if (fr->datacnt) {
    rv = ngtcp2_strm_recv_reordering(strm, fr->data[0].base, fr->data[0].len,
                                     fr->offset);
    if (rv != 0) {
      return rv;
    }
  }
  return ngtcp2_conn_close_stream_if_shut_rdwr(conn, strm, NGTCP2_NO_ERROR);
}

/*
 * conn_reset_stream adds RESET_STREAM frame to the transmission
 * queue.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_reset_stream(ngtcp2_conn *conn, ngtcp2_strm *strm,
                             uint16_t app_error_code) {
  int rv;
  ngtcp2_frame_chain *frc;
  ngtcp2_pktns *pktns = &conn->pktns;

  rv = ngtcp2_frame_chain_new(&frc, conn->mem);
  if (rv != 0) {
    return rv;
  }

  frc->fr.type = NGTCP2_FRAME_RESET_STREAM;
  frc->fr.reset_stream.stream_id = strm->stream_id;
  frc->fr.reset_stream.app_error_code = app_error_code;
  frc->fr.reset_stream.final_size = strm->tx.offset;

  /* TODO This prepends RESET_STREAM to pktns->tx.frq. */
  frc->next = pktns->tx.frq;
  pktns->tx.frq = frc;

  return 0;
}

/*
 * conn_stop_sending adds STOP_SENDING frame to the transmission
 * queue.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_stop_sending(ngtcp2_conn *conn, ngtcp2_strm *strm,
                             uint16_t app_error_code) {
  int rv;
  ngtcp2_frame_chain *frc;
  ngtcp2_pktns *pktns = &conn->pktns;

  rv = ngtcp2_frame_chain_new(&frc, conn->mem);
  if (rv != 0) {
    return rv;
  }

  frc->fr.type = NGTCP2_FRAME_STOP_SENDING;
  frc->fr.stop_sending.stream_id = strm->stream_id;
  frc->fr.stop_sending.app_error_code = app_error_code;

  /* TODO This prepends STOP_SENDING to pktns->tx.frq. */
  frc->next = pktns->tx.frq;
  pktns->tx.frq = frc;

  return 0;
}

/*
 * handle_max_remote_streams_extension extends
 * |*punsent_max_remote_streams| if a condition allows it.
 */
static void
handle_max_remote_streams_extension(uint64_t *punsent_max_remote_streams) {
  if (*punsent_max_remote_streams < NGTCP2_MAX_STREAMS) {
    ++(*punsent_max_remote_streams);
  }
}

/*
 * conn_recv_reset_stream is called when RESET_STREAM |fr| is
 * received.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_STREAM_STATE
 *     RESET_STREAM frame is received to the local stream which is not
 *     initiated.
 * NGTCP2_ERR_STREAM_LIMIT
 *     RESET_STREAM frame has remote stream ID which is strictly
 *     greater than the allowed limit.
 * NGTCP2_ERR_PROTO
 *     RESET_STREAM frame is received to the local unidirectional
 *     stream
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_FLOW_CONTROL
 *     Flow control limit is violated; or the final size is beyond the
 *     NGTCP2_MAX_VARINT.
 * NGTCP2_ERR_FINAL_SIZE
 *     The final offset is strictly larger than it is permitted.
 */
static int conn_recv_reset_stream(ngtcp2_conn *conn,
                                  const ngtcp2_reset_stream *fr) {
  ngtcp2_strm *strm;
  int local_stream = conn_local_stream(conn, fr->stream_id);
  int bidi = bidi_stream(fr->stream_id);
  uint64_t datalen;
  ngtcp2_idtr *idtr;
  int rv;

  /* TODO share this piece of code */
  if (bidi) {
    if (local_stream) {
      if (conn->local.bidi.next_stream_id <= fr->stream_id) {
        return NGTCP2_ERR_STREAM_STATE;
      }
    } else if (conn->remote.bidi.max_streams <
               ngtcp2_ord_stream_id(fr->stream_id)) {
      return NGTCP2_ERR_STREAM_LIMIT;
    }

    idtr = &conn->remote.bidi.idtr;
  } else {
    if (local_stream) {
      return NGTCP2_ERR_PROTO;
    }
    if (conn->remote.uni.max_streams < ngtcp2_ord_stream_id(fr->stream_id)) {
      return NGTCP2_ERR_STREAM_LIMIT;
    }

    idtr = &conn->remote.uni.idtr;
  }

  if (NGTCP2_MAX_VARINT < fr->final_size) {
    return NGTCP2_ERR_FLOW_CONTROL;
  }

  strm = ngtcp2_conn_find_stream(conn, fr->stream_id);
  if (strm == NULL) {
    if (local_stream) {
      return 0;
    }

    if (conn_initial_stream_rx_offset(conn, fr->stream_id) < fr->final_size ||
        conn_max_data_violated(conn, fr->final_size)) {
      return NGTCP2_ERR_FLOW_CONTROL;
    }
    rv = ngtcp2_idtr_open(idtr, fr->stream_id);
    if (rv != 0) {
      if (ngtcp2_err_is_fatal(rv)) {
        return rv;
      }
      assert(rv == NGTCP2_ERR_STREAM_IN_USE);
      return 0;
    }

    /* Stream is reset before we create ngtcp2_strm object. */
    conn->rx.offset += fr->final_size;
    ngtcp2_conn_extend_max_offset(conn, fr->final_size);

    rv = conn_call_stream_reset(conn, fr->stream_id, fr->final_size,
                                fr->app_error_code, NULL);
    if (rv != 0) {
      return rv;
    }

    /* There will be no activity in this stream because we got
       RESET_STREAM and don't write stream data any further.  This
       effectively allows another new stream for peer. */
    if (bidi) {
      handle_max_remote_streams_extension(
          &conn->remote.bidi.unsent_max_streams);
    } else {
      handle_max_remote_streams_extension(&conn->remote.uni.unsent_max_streams);
    }

    return 0;
  }

  if ((strm->flags & NGTCP2_STRM_FLAG_SHUT_RD)) {
    if (strm->rx.last_offset != fr->final_size) {
      return NGTCP2_ERR_FINAL_SIZE;
    }
  } else if (strm->rx.last_offset > fr->final_size) {
    return NGTCP2_ERR_FINAL_SIZE;
  }

  datalen = fr->final_size - strm->rx.last_offset;

  if (strm->rx.max_offset < fr->final_size ||
      conn_max_data_violated(conn, datalen)) {
    return NGTCP2_ERR_FLOW_CONTROL;
  }

  if (!(strm->flags & NGTCP2_STRM_FLAG_RECV_RST)) {
    rv = conn_call_stream_reset(conn, fr->stream_id, fr->final_size,
                                fr->app_error_code, strm->stream_user_data);
    if (rv != 0) {
      return rv;
    }
  }

  conn->rx.offset += datalen;
  ngtcp2_conn_extend_max_offset(conn, datalen);

  strm->rx.last_offset = fr->final_size;
  strm->flags |= NGTCP2_STRM_FLAG_SHUT_RD | NGTCP2_STRM_FLAG_RECV_RST;

  return ngtcp2_conn_close_stream_if_shut_rdwr(conn, strm, fr->app_error_code);
}

/*
 * conn_recv_stop_sending is called when STOP_SENDING |fr| is received.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_STREAM_STATE
 *     STOP_SENDING frame is received for a local stream which is not
 *     initiated; or STOP_SENDING frame is received for a local
 *     unidirectional stream.
 * NGTCP2_ERR_STREAM_LIMIT
 *     STOP_SENDING frame has remote stream ID which is strictly
 *     greater than the allowed limit.
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 */
static int conn_recv_stop_sending(ngtcp2_conn *conn,
                                  const ngtcp2_stop_sending *fr) {
  int rv;
  ngtcp2_strm *strm;
  ngtcp2_idtr *idtr;
  int local_stream = conn_local_stream(conn, fr->stream_id);
  int bidi = bidi_stream(fr->stream_id);

  if (bidi) {
    if (local_stream) {
      if (conn->local.bidi.next_stream_id <= fr->stream_id) {
        return NGTCP2_ERR_STREAM_STATE;
      }
    } else if (conn->remote.bidi.max_streams <
               ngtcp2_ord_stream_id(fr->stream_id)) {
      return NGTCP2_ERR_STREAM_LIMIT;
    }

    idtr = &conn->remote.bidi.idtr;
  } else {
    if (!local_stream || conn->local.uni.next_stream_id <= fr->stream_id) {
      return NGTCP2_ERR_STREAM_STATE;
    }

    idtr = &conn->remote.uni.idtr;
  }

  strm = ngtcp2_conn_find_stream(conn, fr->stream_id);
  if (strm == NULL) {
    if (local_stream) {
      return 0;
    }
    rv = ngtcp2_idtr_open(idtr, fr->stream_id);
    if (rv != 0) {
      if (ngtcp2_err_is_fatal(rv)) {
        return rv;
      }
      assert(rv == NGTCP2_ERR_STREAM_IN_USE);
      return 0;
    }

    /* Frame is received reset before we create ngtcp2_strm
       object. */
    strm = ngtcp2_mem_malloc(conn->mem, sizeof(ngtcp2_strm));
    if (strm == NULL) {
      return NGTCP2_ERR_NOMEM;
    }
    rv = ngtcp2_conn_init_stream(conn, strm, fr->stream_id, NULL);
    if (rv != 0) {
      ngtcp2_mem_free(conn->mem, strm);
      return rv;
    }
  }

  /* No RESET_STREAM is required if we have sent FIN and all data have
     been acknowledged. */
  if ((strm->flags & NGTCP2_STRM_FLAG_SHUT_WR) &&
      ngtcp2_strm_is_all_tx_data_acked(strm)) {
    return 0;
  }

  rv = conn_reset_stream(conn, strm, fr->app_error_code);
  if (rv != 0) {
    return rv;
  }

  strm->flags |= NGTCP2_STRM_FLAG_SHUT_WR | NGTCP2_STRM_FLAG_SENT_RST;

  ngtcp2_strm_streamfrq_clear(strm);

  return ngtcp2_conn_close_stream_if_shut_rdwr(conn, strm, fr->app_error_code);
}

/*
 * conn_on_stateless_reset decodes Stateless Reset from the buffer
 * pointed by |payload| whose length is |payloadlen|.  |payload|
 * should start after first byte of packet.
 *
 * If Stateless Reset is decoded, and the Stateless Reset Token is
 * validated, the connection is closed.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_INVALID_ARGUMENT
 *     Could not decode Stateless Reset; or Stateless Reset Token does
 *     not match; or No stateless reset token is available.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed.
 */
static int conn_on_stateless_reset(ngtcp2_conn *conn, const uint8_t *payload,
                                   size_t payloadlen) {
  int rv = 1;
  ngtcp2_pkt_stateless_reset sr;
  size_t i, len;
  ngtcp2_dcid *dcid;

  rv = ngtcp2_pkt_decode_stateless_reset(&sr, payload, payloadlen);
  if (rv != 0) {
    return rv;
  }

  if (ngtcp2_verify_stateless_retry_token(conn->dcid.current.token,
                                          sr.stateless_reset_token) != 0 &&
      (!conn->pv || !(conn->pv->flags & NGTCP2_PV_FLAG_FALLBACK_ON_FAILURE) ||
       ngtcp2_verify_stateless_retry_token(conn->pv->fallback_dcid.token,
                                           sr.stateless_reset_token) != 0)) {
    len = ngtcp2_ringbuf_len(&conn->dcid.bound);
    for (i = 0; i < len; ++i) {
      dcid = ngtcp2_ringbuf_get(&conn->dcid.bound, i);
      if (ngtcp2_verify_stateless_retry_token(dcid->token,
                                              sr.stateless_reset_token) == 0) {
        break;
      }
    }

    if (i == len) {
      return NGTCP2_ERR_INVALID_ARGUMENT;
    }
  }

  conn->state = NGTCP2_CS_DRAINING;

  ngtcp2_log_rx_sr(&conn->log, &sr);

  if (!conn->callbacks.recv_stateless_reset) {
    return 0;
  }

  rv = conn->callbacks.recv_stateless_reset(conn, &sr, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

/*
 * conn_recv_delayed_handshake_pkt processes the received Handshake
 * packet which is received after handshake completed.  This function
 * does the minimal job, and its purpose is send acknowledgement of
 * this packet to the peer.  We assume that hd->type is one of
 * Initial, or Handshake.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed.
 * NGTCP2_ERR_FRAME_ENCODING
 *     Frame is badly formatted; or frame type is unknown.
 * NGTCP2_ERR_NOMEM
 *     Out of memory
 * NGTCP2_ERR_DISCARD_PKT
 *     Packet was discarded.
 * NGTCP2_ERR_ACK_FRAME
 *     ACK frame is malformed.
 * NGTCP2_ERR_PROTO
 *     APPLICATION_CLOSE frame is included in Initial packet.
 */
static int conn_recv_delayed_handshake_pkt(ngtcp2_conn *conn,
                                           const ngtcp2_pkt_hd *hd,
                                           const uint8_t *payload,
                                           size_t payloadlen,
                                           ngtcp2_tstamp ts) {
  ssize_t nread;
  ngtcp2_max_frame mfr;
  ngtcp2_frame *fr = &mfr.fr;
  int rv;
  int require_ack = 0;
  ngtcp2_pktns *pktns;

  switch (hd->type) {
  case NGTCP2_PKT_HANDSHAKE:
    pktns = &conn->hs_pktns;
    break;
  default:
    assert(0);
  }

  if (payloadlen == 0) {
    /* QUIC packet must contain at least one frame */
    return NGTCP2_ERR_DISCARD_PKT;
  }

  for (; payloadlen;) {
    nread = ngtcp2_pkt_decode_frame(fr, payload, payloadlen);
    if (nread < 0) {
      return (int)nread;
    }

    payload += nread;
    payloadlen -= (size_t)nread;

    if (fr->type == NGTCP2_FRAME_ACK) {
      assign_recved_ack_delay_unscaled(&fr->ack,
                                       NGTCP2_DEFAULT_ACK_DELAY_EXPONENT);
    }

    ngtcp2_log_rx_fr(&conn->log, hd, fr);

    switch (fr->type) {
    case NGTCP2_FRAME_ACK:
    case NGTCP2_FRAME_ACK_ECN:
      rv = conn_recv_ack(conn, pktns, &fr->ack, ts);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGTCP2_FRAME_PADDING:
      break;
    case NGTCP2_FRAME_CONNECTION_CLOSE:
    case NGTCP2_FRAME_CONNECTION_CLOSE_APP:
      if (hd->type != NGTCP2_PKT_HANDSHAKE) {
        break;
      }
      conn_recv_connection_close(conn);
      break;
    case NGTCP2_FRAME_CRYPTO:
      require_ack = 1;
      break;
    default:
      return NGTCP2_ERR_PROTO;
    }
  }

  rv = pktns_commit_recv_pkt_num(pktns, hd->pkt_num);
  if (rv != 0) {
    return rv;
  }

  if (require_ack && ++pktns->acktr.rx_npkt >= NGTCP2_NUM_IMMEDIATE_ACK_PKT) {
    ngtcp2_acktr_immediate_ack(&pktns->acktr);
  }

  return ngtcp2_conn_sched_ack(conn, &pktns->acktr, hd->pkt_num, require_ack,
                               ts);
}

/*
 * conn_recv_max_streams processes the incoming MAX_STREAMS frame
 * |fr|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed.
 */
static int conn_recv_max_streams(ngtcp2_conn *conn,
                                 const ngtcp2_max_streams *fr) {
  uint64_t n;

  if (fr->max_streams > NGTCP2_MAX_STREAMS) {
    return NGTCP2_ERR_STREAM_LIMIT;
  }

  n = ngtcp2_min(fr->max_streams, NGTCP2_MAX_STREAMS);

  if (fr->type == NGTCP2_FRAME_MAX_STREAMS_BIDI) {
    if (conn->local.bidi.max_streams < n) {
      conn->local.bidi.max_streams = n;
      return conn_call_extend_max_local_streams_bidi(conn, n);
    }
    return 0;
  }

  if (conn->local.uni.max_streams < n) {
    conn->local.uni.max_streams = n;
    return conn_call_extend_max_local_streams_uni(conn, n);
  }
  return 0;
}

/*
 * conn_recv_new_connection_id processes the incoming
 * NEW_CONNECTION_ID frame |fr|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_PROTO
 *     |fr| has the duplicated sequence number with different CID or
 *     token; or DCID is zero-length.
 */
static int conn_recv_new_connection_id(ngtcp2_conn *conn,
                                       const ngtcp2_new_connection_id *fr) {
  size_t i, len;
  ngtcp2_dcid *dcid;
  ngtcp2_pv *pv = conn->pv;
  int rv;

  if (conn->dcid.current.cid.datalen == 0) {
    return NGTCP2_ERR_PROTO;
  }

  rv = ngtcp2_dcid_verify_uniqueness(&conn->dcid.current, fr->seq, &fr->cid,
                                     fr->stateless_reset_token);
  if (rv != 0) {
    return rv;
  }
  if (ngtcp2_cid_eq(&conn->dcid.current.cid, &fr->cid)) {
    return 0;
  }

  if (pv) {
    rv = ngtcp2_dcid_verify_uniqueness(&pv->dcid, fr->seq, &fr->cid,
                                       fr->stateless_reset_token);
    if (rv != 0) {
      return rv;
    }
    if (ngtcp2_cid_eq(&pv->dcid.cid, &fr->cid)) {
      return 0;
    }
  }

  len = ngtcp2_ringbuf_len(&conn->dcid.bound);

  for (i = 0; i < len; ++i) {
    dcid = ngtcp2_ringbuf_get(&conn->dcid.bound, i);
    rv = ngtcp2_dcid_verify_uniqueness(dcid, fr->seq, &fr->cid,
                                       fr->stateless_reset_token);
    if (rv != 0) {
      return NGTCP2_ERR_PROTO;
    }
    if (ngtcp2_cid_eq(&dcid->cid, &fr->cid)) {
      return 0;
    }
  }

  len = ngtcp2_ringbuf_len(&conn->dcid.unused);

  for (i = 0; i < len; ++i) {
    dcid = ngtcp2_ringbuf_get(&conn->dcid.unused, i);
    rv = ngtcp2_dcid_verify_uniqueness(dcid, fr->seq, &fr->cid,
                                       fr->stateless_reset_token);
    if (rv != 0) {
      return NGTCP2_ERR_PROTO;
    }
    if (ngtcp2_cid_eq(&dcid->cid, &fr->cid)) {
      return 0;
    }
  }

  if (len >= NGTCP2_MAX_DCID_POOL_SIZE) {
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON, "too many connection ID");
    return 0;
  }

  dcid = ngtcp2_ringbuf_push_back(&conn->dcid.unused);
  ngtcp2_dcid_init(dcid, fr->seq, &fr->cid, fr->stateless_reset_token);

  return 0;
}

/*
 * conn_recv_retire_connection_id processes the incoming
 * RETIRE_CONNECTION_ID frame |fr|.  |hd| is a packet header which
 * |fr| is included.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_PROTO
 *     SCID is zero-length.
 */
static int conn_recv_retire_connection_id(ngtcp2_conn *conn,
                                          const ngtcp2_pkt_hd *hd,
                                          const ngtcp2_retire_connection_id *fr,
                                          ngtcp2_tstamp ts) {
  ngtcp2_ksl_it it;
  ngtcp2_scid *scid;

  if (conn->oscid.datalen == 0) {
    return NGTCP2_ERR_PROTO;
  }

  for (it = ngtcp2_ksl_begin(&conn->scid.set); !ngtcp2_ksl_it_end(&it);
       ngtcp2_ksl_it_next(&it)) {
    scid = ngtcp2_ksl_it_get(&it);
    if (scid->seq == fr->seq) {
      if (ngtcp2_cid_eq(&scid->cid, &hd->dcid)) {
        return NGTCP2_ERR_PROTO;
      }

      scid->flags |= NGTCP2_SCID_FLAG_RETIRED;

      if (scid->pe.index != NGTCP2_PQ_BAD_INDEX) {
        ngtcp2_pq_remove(&conn->scid.used, &scid->pe);
        scid->pe.index = NGTCP2_PQ_BAD_INDEX;
      }

      scid->ts_retired = ts;

      return ngtcp2_pq_push(&conn->scid.used, &scid->pe);
    }
  }

  return 0;
}

/*
 * conn_key_phase_changed returns nonzero if |hd| indicates that the
 * key phase has unexpected value.
 */
static int conn_key_phase_changed(ngtcp2_conn *conn, const ngtcp2_pkt_hd *hd) {
  ngtcp2_pktns *pktns = &conn->pktns;

  return !(pktns->crypto.rx.ckm->flags & NGTCP2_CRYPTO_KM_FLAG_KEY_PHASE_ONE) ^
         !(hd->flags & NGTCP2_PKT_FLAG_KEY_PHASE);
}

/*
 * conn_prepare_key_update installs new updated keys.
 */
static int conn_prepare_key_update(ngtcp2_conn *conn) {
  int rv;

  if (conn->crypto.key_update.new_rx_ckm ||
      conn->crypto.key_update.new_tx_ckm) {
    assert(conn->crypto.key_update.new_rx_ckm);
    assert(conn->crypto.key_update.new_tx_ckm);
    return 0;
  }

  assert(conn->callbacks.update_key);

  /* application is supposed to call ngtcp2_conn_update_tx_key and
   * ngtcp2_conn_update_rx_key during execution of callback.
   */
  rv = conn->callbacks.update_key(conn, conn->user_data);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  assert(conn->crypto.key_update.new_rx_ckm);
  assert(conn->crypto.key_update.new_tx_ckm);

  return 0;
}

/*
 * conn_commit_key_update rotates keys.  The current key moves to old
 * key, and new key moves to the current key.
 */
static void conn_commit_key_update(ngtcp2_conn *conn, int64_t pkt_num) {
  ngtcp2_pktns *pktns = &conn->pktns;

  assert(conn->crypto.key_update.new_rx_ckm);
  assert(conn->crypto.key_update.new_tx_ckm);

  ngtcp2_crypto_km_del(conn->crypto.key_update.old_rx_ckm, conn->mem);
  conn->crypto.key_update.old_rx_ckm = pktns->crypto.rx.ckm;

  pktns->crypto.rx.ckm = conn->crypto.key_update.new_rx_ckm;
  conn->crypto.key_update.new_rx_ckm = NULL;
  pktns->crypto.rx.ckm->pkt_num = pkt_num;

  ngtcp2_crypto_km_del(pktns->crypto.tx.ckm, conn->mem);
  pktns->crypto.tx.ckm = conn->crypto.key_update.new_tx_ckm;
  conn->crypto.key_update.new_tx_ckm = NULL;
  pktns->crypto.tx.ckm->pkt_num = pktns->tx.last_pkt_num + 1;
}

/*
 * conn_path_validation_in_progress returns nonzero if path validation
 * against |path| is underway.
 */
static int conn_path_validation_in_progress(ngtcp2_conn *conn,
                                            const ngtcp2_path *path) {
  ngtcp2_pv *pv = conn->pv;

  return pv && !(pv->flags & NGTCP2_PV_FLAG_DONT_CARE) &&
         ngtcp2_path_eq(&pv->dcid.ps.path, path);
}

/*
 * conn_reset_congestion_state resets congestion state.
 */
static void conn_reset_congestion_state(ngtcp2_conn *conn) {
  uint64_t bytes_in_flight;

  bw_reset(&conn->rx.bw);
  rcvry_stat_reset(&conn->rcs);
  /* Keep bytes_in_flight because we have to take care of packets
     in flight. */
  bytes_in_flight = conn->ccs.bytes_in_flight;
  cc_stat_reset(&conn->ccs);
  conn->ccs.bytes_in_flight = bytes_in_flight;
}

/*
 * conn_recv_non_probing_pkt_on_new_path is called when non-probing
 * packet is received via new path.  It starts path validation against
 * the new path.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_CONN_ID_BLOCKED
 *     No DCID is available
 * NGTCP2_ERR_NOMEM
 *     Out of memory
 */
static int conn_recv_non_probing_pkt_on_new_path(ngtcp2_conn *conn,
                                                 const ngtcp2_path *path,
                                                 ngtcp2_tstamp ts) {

  ngtcp2_dcid *dcid, *last_dcid;
  ngtcp2_ringbuf *rb;
  ngtcp2_pv *pv;
  size_t i, len;
  int rv;
  ngtcp2_duration timeout;

  assert(conn->server);

  if (conn->pv && (conn->pv->flags & NGTCP2_PV_FLAG_FALLBACK_ON_FAILURE) &&
      ngtcp2_path_eq(&conn->pv->fallback_dcid.ps.path, path)) {
    /* If new path equals fallback path, that means connection
       migrated back to the original path.  Fallback path is
       considered to be validated. */
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PTV,
                    "path is migrated back to the original path");
    ngtcp2_dcid_copy(&conn->dcid.current, &conn->pv->fallback_dcid);
    rv = conn_stop_pv(conn, ts);
    if (rv != 0) {
      return rv;
    }
    return 0;
  }

  len = ngtcp2_ringbuf_len(&conn->dcid.bound);

  for (i = 0; i < len; ++i) {
    dcid = ngtcp2_ringbuf_get(&conn->dcid.bound, i);
    if (ngtcp2_path_eq(&dcid->ps.path, path)) {
      rb = &conn->dcid.bound;
      break;
    }
  }

  if (i == len) {
    if (ngtcp2_ringbuf_len(&conn->dcid.unused) == 0) {
      return NGTCP2_ERR_CONN_ID_BLOCKED;
    }

    dcid = ngtcp2_ringbuf_get(&conn->dcid.unused, 0);
    rb = &conn->dcid.unused;
  }

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON,
                  "remote address has changed");

  conn_reset_congestion_state(conn);

  timeout = conn_compute_pto(conn);
  timeout =
      ngtcp2_max(timeout, (ngtcp2_duration)(6ULL * NGTCP2_DEFAULT_INITIAL_RTT));

  rv = ngtcp2_pv_new(&pv, dcid, timeout,
                     NGTCP2_PV_FLAG_FALLBACK_ON_FAILURE |
                         NGTCP2_PV_FLAG_RETIRE_DCID_ON_FINISH,
                     &conn->log, conn->mem);
  if (rv != 0) {
    return rv;
  }

  ngtcp2_path_copy(&pv->dcid.ps.path, path);
  if (conn->pv && (conn->pv->flags & NGTCP2_PV_FLAG_FALLBACK_ON_FAILURE)) {
    ngtcp2_dcid_copy(&pv->fallback_dcid, &conn->pv->fallback_dcid);
  } else {
    ngtcp2_dcid_copy(&pv->fallback_dcid, &conn->dcid.current);
  }
  ngtcp2_dcid_copy(&conn->dcid.current, &pv->dcid);

  if (conn->pv) {
    ngtcp2_log_info(
        &conn->log, NGTCP2_LOG_EVENT_PTV,
        "path migration is aborted because new migration has started");
    rv = conn_stop_pv(conn, ts);
    if (rv != 0) {
      ngtcp2_pv_del(pv);
      return rv;
    }
  }

  conn->pv = pv;

  if (rb == &conn->dcid.unused) {
    ngtcp2_ringbuf_pop_front(&conn->dcid.unused);
    return 0;
  }

  assert(rb == &conn->dcid.bound);

  if (i == 0) {
    ngtcp2_ringbuf_pop_front(&conn->dcid.bound);
  } else if (i == len - 1) {
    ngtcp2_ringbuf_pop_back(&conn->dcid.bound);
  } else {
    assert(i < len);

    last_dcid = ngtcp2_ringbuf_get(&conn->dcid.bound, len - 1);
    ngtcp2_dcid_copy(dcid, last_dcid);
    ngtcp2_ringbuf_pop_back(&conn->dcid.bound);
  }

  return 0;
}

/*
 * conn_recv_pkt processes a packet contained in the buffer pointed by
 * |pkt| of length |pktlen|.  |pkt| may contain multiple QUIC packets.
 * This function only processes the first packet.
 *
 * This function returns the number of bytes processed if it succeeds,
 * or one of the following negative error codes:
 *
 * NGTCP2_ERR_DISCARD_PKT
 *     Packet was discarded because plain text header was malformed;
 *     or its payload could not be decrypted.
 * NGTCP2_ERR_PROTO
 *     Packet is badly formatted; or 0RTT packet contains other than
 *     PADDING or STREAM frames; or other QUIC protocol violation is
 *     found.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_FRAME_ENCODING
 *     Frame is badly formatted; or frame type is unknown.
 * NGTCP2_ERR_ACK_FRAME
 *     ACK frame is malformed.
 * NGTCP2_ERR_STREAM_STATE
 *     Frame is received to the local stream which is not initiated.
 * NGTCP2_ERR_STREAM_LIMIT
 *     Frame has remote stream ID which is strictly greater than the
 *     allowed limit.
 * NGTCP2_ERR_FLOW_CONTROL
 *     Flow control limit is violated.
 * NGTCP2_ERR_FINAL_SIZE
 *     Frame has strictly larger end offset than it is permitted.
 */
static ssize_t conn_recv_pkt(ngtcp2_conn *conn, const ngtcp2_path *path,
                             const uint8_t *pkt, size_t pktlen,
                             ngtcp2_tstamp ts) {
  ngtcp2_pkt_hd hd;
  int rv = 0;
  size_t hdpktlen;
  const uint8_t *payload;
  size_t payloadlen;
  ssize_t nread, nwrite;
  ngtcp2_max_frame mfr;
  ngtcp2_frame *fr = &mfr.fr;
  int require_ack = 0;
  ngtcp2_crypto_km *ckm;
  const ngtcp2_vec *hp;
  uint8_t plain_hdpkt[1500];
  ngtcp2_hp_mask hp_mask;
  ngtcp2_decrypt decrypt;
  size_t aead_overhead;
  ngtcp2_pktns *pktns;
  int non_probing_pkt = 0;
  int key_phase_bit_changed = 0;
  int force_decrypt_failure = 0;
  int invalid_reserved_bits = 0;

  if (pkt[0] & NGTCP2_HEADER_FORM_BIT) {
    nread = ngtcp2_pkt_decode_hd_long(&hd, pkt, pktlen);
    if (nread < 0) {
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "could not decode long header");
      return NGTCP2_ERR_DISCARD_PKT;
    }

    if (pktlen < (size_t)nread + hd.len || conn->version != hd.version) {
      return NGTCP2_ERR_DISCARD_PKT;
    }

    pktlen = (size_t)nread + hd.len;

    /* Quoted from spec: if subsequent packets of those types include
       a different Source Connection ID, they MUST be discarded. */
    if (!ngtcp2_cid_eq(&conn->odcid, &hd.scid)) {
      ngtcp2_log_rx_pkt_hd(&conn->log, &hd);
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "packet was ignored because of mismatched SCID");
      return NGTCP2_ERR_DISCARD_PKT;
    }

    switch (hd.type) {
    case NGTCP2_PKT_INITIAL:
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "delayed Initial packet was discarded");
      return (ssize_t)pktlen;
    case NGTCP2_PKT_HANDSHAKE:
      pktns = &conn->hs_pktns;
      ckm = pktns->crypto.rx.ckm;
      hp = pktns->crypto.rx.hp;
      hp_mask = conn->callbacks.hp_mask;
      decrypt = conn->callbacks.decrypt;
      aead_overhead = conn->crypto.aead_overhead;
      break;
    case NGTCP2_PKT_0RTT:
      if (!conn->server || !conn->early.ckm) {
        return NGTCP2_ERR_DISCARD_PKT;
      }

      pktns = &conn->pktns;
      ckm = conn->early.ckm;
      hp = conn->early.hp;
      hp_mask = conn->callbacks.hp_mask;
      decrypt = conn->callbacks.decrypt;
      aead_overhead = conn->crypto.aead_overhead;
      break;
    default:
      ngtcp2_log_rx_pkt_hd(&conn->log, &hd);
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "packet type 0x%02x was ignored", hd.type);
      return (ssize_t)pktlen;
    }
  } else {
    nread = ngtcp2_pkt_decode_hd_short(&hd, pkt, pktlen, conn->oscid.datalen);
    if (nread < 0) {
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "could not decode short header");
      return NGTCP2_ERR_DISCARD_PKT;
    }

    pktns = &conn->pktns;
    ckm = pktns->crypto.rx.ckm;
    hp = pktns->crypto.rx.hp;
    hp_mask = conn->callbacks.hp_mask;
    decrypt = conn->callbacks.decrypt;
    aead_overhead = conn->crypto.aead_overhead;
  }

  nwrite =
      conn_decrypt_hp(conn, &hd, plain_hdpkt, sizeof(plain_hdpkt), pkt, pktlen,
                      (size_t)nread, ckm, hp, hp_mask, aead_overhead);
  if (nwrite < 0) {
    if (ngtcp2_err_is_fatal((int)nwrite)) {
      return nwrite;
    }
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "could not decrypt packet number");
    return NGTCP2_ERR_DISCARD_PKT;
  }

  hdpktlen = (size_t)nwrite;
  payload = pkt + hdpktlen;
  payloadlen = pktlen - hdpktlen;

  hd.pkt_num = ngtcp2_pkt_adjust_pkt_num(pktns->rx.max_pkt_num, hd.pkt_num,
                                         pkt_num_bits(hd.pkt_numlen));

  ngtcp2_log_rx_pkt_hd(&conn->log, &hd);

  rv = ngtcp2_pkt_verify_reserved_bits(plain_hdpkt[0]);
  if (rv != 0) {
    invalid_reserved_bits = 1;

    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "packet has incorrect reserved bits");

    /* Will return error after decrypting payload */
  }

  if (pktns_pkt_num_is_duplicate(pktns, hd.pkt_num)) {
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "packet was discarded because of duplicated packet number");
    return NGTCP2_ERR_DISCARD_PKT;
  }

  if (hd.type == NGTCP2_PKT_SHORT) {
    key_phase_bit_changed = conn_key_phase_changed(conn, &hd);
  }

  rv = conn_ensure_decrypt_buffer(conn, payloadlen);
  if (rv != 0) {
    return rv;
  }

  if (key_phase_bit_changed) {
    assert(hd.type == NGTCP2_PKT_SHORT);

    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT, "unexpected KEY_PHASE");

    if (ckm->pkt_num > hd.pkt_num) {
      if (conn->crypto.key_update.old_rx_ckm) {
        ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                        "decrypting with old key");
        ckm = conn->crypto.key_update.old_rx_ckm;
      } else {
        force_decrypt_failure = 1;
      }
    } else if (pktns->rx.max_pkt_num < hd.pkt_num) {
      assert(ckm->pkt_num < hd.pkt_num);
      if (!conn->crypto.key_update.new_rx_ckm) {
        ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT, "preparing new key");
        rv = conn_prepare_key_update(conn);
        if (rv != 0) {
          return rv;
        }
      }
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "decrypting with new key");
      ckm = conn->crypto.key_update.new_rx_ckm;
    } else {
      force_decrypt_failure = 1;
    }
  }

  nwrite = conn_decrypt_pkt(conn, conn->crypto.decrypt_buf.base, payloadlen,
                            payload, payloadlen, plain_hdpkt, hdpktlen,
                            hd.pkt_num, ckm, decrypt);

  if (force_decrypt_failure) {
    nwrite = NGTCP2_ERR_TLS_DECRYPT;
  }

  if (nwrite < 0) {
    if (ngtcp2_err_is_fatal((int)nwrite)) {
      return nwrite;
    }

    assert(NGTCP2_ERR_TLS_DECRYPT == nwrite);

    if (hd.flags & NGTCP2_PKT_FLAG_LONG_FORM) {
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "could not decrypt packet payload");
      return NGTCP2_ERR_DISCARD_PKT;
    }

    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "could not decrypt packet payload");
    return NGTCP2_ERR_DISCARD_PKT;
  }

  if (invalid_reserved_bits) {
    return NGTCP2_ERR_PROTO;
  }

  payload = conn->crypto.decrypt_buf.base;
  payloadlen = (size_t)nwrite;

  if (payloadlen == 0) {
    /* QUIC packet must contain at least one frame */
    return NGTCP2_ERR_DISCARD_PKT;
  }

  if (hd.flags & NGTCP2_PKT_FLAG_LONG_FORM) {
    switch (hd.type) {
    case NGTCP2_PKT_HANDSHAKE:
      rv = conn_verify_dcid(conn, &hd);
      if (rv != 0) {
        if (ngtcp2_err_is_fatal(rv)) {
          return rv;
        }
        ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                        "packet was ignored because of mismatched DCID");
        return NGTCP2_ERR_DISCARD_PKT;
      }

      /* TODO find a way when to ignore incoming handshake packet */
      rv = conn_recv_delayed_handshake_pkt(conn, &hd, payload, payloadlen, ts);
      if (rv < 0) {
        if (ngtcp2_err_is_fatal(rv)) {
          return rv;
        }
        return (ssize_t)rv;
      }
      return (ssize_t)pktlen;
    case NGTCP2_PKT_0RTT:
      if (!ngtcp2_cid_eq(&conn->rcid, &hd.dcid)) {
        rv = conn_verify_dcid(conn, &hd);
        if (rv != 0) {
          if (ngtcp2_err_is_fatal(rv)) {
            return rv;
          }
          ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                          "packet was ignored because of mismatched DCID");
          return NGTCP2_ERR_DISCARD_PKT;
        }
      }
      break;
    }
  } else {
    rv = conn_verify_dcid(conn, &hd);
    if (rv != 0) {
      if (ngtcp2_err_is_fatal(rv)) {
        return rv;
      }
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                      "packet was ignored because of mismatched DCID");
      return NGTCP2_ERR_DISCARD_PKT;
    }
    conn->flags |= NGTCP2_CONN_FLAG_RECV_PROTECTED_PKT;
  }

  for (; payloadlen;) {
    nread = ngtcp2_pkt_decode_frame(fr, payload, payloadlen);
    if (nread < 0) {
      return (int)nread;
    }

    payload += nread;
    payloadlen -= (size_t)nread;

    if (fr->type == NGTCP2_FRAME_ACK) {
      if ((hd.flags & NGTCP2_PKT_FLAG_LONG_FORM) &&
          hd.type == NGTCP2_PKT_0RTT) {
        return NGTCP2_ERR_PROTO;
      }
      assign_recved_ack_delay_unscaled(
          &fr->ack, conn->remote.settings.ack_delay_exponent);
    }

    ngtcp2_log_rx_fr(&conn->log, &hd, fr);

    if (hd.type == NGTCP2_PKT_0RTT) {
      switch (fr->type) {
      case NGTCP2_FRAME_PADDING:
      case NGTCP2_FRAME_PING:
      case NGTCP2_FRAME_RESET_STREAM:
      case NGTCP2_FRAME_STOP_SENDING:
      case NGTCP2_FRAME_STREAM:
      case NGTCP2_FRAME_MAX_DATA:
      case NGTCP2_FRAME_MAX_STREAM_DATA:
      case NGTCP2_FRAME_MAX_STREAMS_BIDI:
      case NGTCP2_FRAME_MAX_STREAMS_UNI:
      case NGTCP2_FRAME_DATA_BLOCKED:
      case NGTCP2_FRAME_STREAM_DATA_BLOCKED:
      case NGTCP2_FRAME_STREAMS_BLOCKED_BIDI:
      case NGTCP2_FRAME_STREAMS_BLOCKED_UNI:
      case NGTCP2_FRAME_NEW_CONNECTION_ID:
      case NGTCP2_FRAME_PATH_CHALLENGE:
        break;
      default:
        return NGTCP2_ERR_PROTO;
      }
    }

    switch (fr->type) {
    case NGTCP2_FRAME_ACK:
    case NGTCP2_FRAME_ACK_ECN:
    case NGTCP2_FRAME_PADDING:
    case NGTCP2_FRAME_CONNECTION_CLOSE:
    case NGTCP2_FRAME_CONNECTION_CLOSE_APP:
      break;
    default:
      require_ack = 1;
    }

    switch (fr->type) {
    case NGTCP2_FRAME_ACK:
    case NGTCP2_FRAME_ACK_ECN:
      rv = conn_recv_ack(conn, pktns, &fr->ack, ts);
      if (rv != 0) {
        return rv;
      }
      non_probing_pkt = 1;
      break;
    case NGTCP2_FRAME_STREAM:
      rv = conn_recv_stream(conn, &fr->stream);
      if (rv != 0) {
        return rv;
      }
      non_probing_pkt = 1;
      conn_update_rx_bw(
          conn, ngtcp2_vec_len(fr->stream.data, fr->stream.datacnt), ts);
      break;
    case NGTCP2_FRAME_CRYPTO:
      rv = conn_recv_crypto(conn, NGTCP2_CRYPTO_LEVEL_APP, &pktns->crypto.strm,
                            &fr->crypto);
      if (rv != 0) {
        return rv;
      }
      non_probing_pkt = 1;
      break;
    case NGTCP2_FRAME_RESET_STREAM:
      rv = conn_recv_reset_stream(conn, &fr->reset_stream);
      if (rv != 0) {
        return rv;
      }
      non_probing_pkt = 1;
      break;
    case NGTCP2_FRAME_STOP_SENDING:
      rv = conn_recv_stop_sending(conn, &fr->stop_sending);
      if (rv != 0) {
        return rv;
      }
      non_probing_pkt = 1;
      break;
    case NGTCP2_FRAME_MAX_STREAM_DATA:
      rv = conn_recv_max_stream_data(conn, &fr->max_stream_data);
      if (rv != 0) {
        return rv;
      }
      non_probing_pkt = 1;
      break;
    case NGTCP2_FRAME_MAX_DATA:
      conn_recv_max_data(conn, &fr->max_data);
      non_probing_pkt = 1;
      break;
    case NGTCP2_FRAME_MAX_STREAMS_BIDI:
    case NGTCP2_FRAME_MAX_STREAMS_UNI:
      rv = conn_recv_max_streams(conn, &fr->max_streams);
      if (rv != 0) {
        return rv;
      }
      non_probing_pkt = 1;
      break;
    case NGTCP2_FRAME_CONNECTION_CLOSE:
    case NGTCP2_FRAME_CONNECTION_CLOSE_APP:
      conn_recv_connection_close(conn);
      break;
    case NGTCP2_FRAME_PING:
      non_probing_pkt = 1;
      break;
    case NGTCP2_FRAME_PATH_CHALLENGE:
      conn_recv_path_challenge(conn, path, &fr->path_challenge);
      break;
    case NGTCP2_FRAME_PATH_RESPONSE:
      rv = conn_recv_path_response(conn, path, &fr->path_response, ts);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGTCP2_FRAME_NEW_CONNECTION_ID:
      rv = conn_recv_new_connection_id(conn, &fr->new_connection_id);
      if (rv != 0) {
        return rv;
      }
      break;
    case NGTCP2_FRAME_RETIRE_CONNECTION_ID:
      rv = conn_recv_retire_connection_id(conn, &hd, &fr->retire_connection_id,
                                          ts);
      if (rv != 0) {
        return rv;
      }
      non_probing_pkt = 1;
      break;
    case NGTCP2_FRAME_DATA_BLOCKED:
    case NGTCP2_FRAME_STREAMS_BLOCKED_BIDI:
    case NGTCP2_FRAME_STREAMS_BLOCKED_UNI:
    case NGTCP2_FRAME_NEW_TOKEN:
      /* TODO Not implemented yet */
      non_probing_pkt = 1;
      break;
    }
  }

  if (conn->server && hd.type == NGTCP2_PKT_SHORT && non_probing_pkt &&
      pktns->rx.max_pkt_num < hd.pkt_num &&
      !ngtcp2_path_eq(&conn->dcid.current.ps.path, path) &&
      !conn_path_validation_in_progress(conn, path)) {
    rv = conn_recv_non_probing_pkt_on_new_path(conn, path, ts);
    if (rv != 0) {
      if (ngtcp2_err_is_fatal(rv)) {
        return rv;
      }

      /* DCID is not available.  Just continue. */
      assert(NGTCP2_ERR_CONN_ID_BLOCKED == rv);
    }
  }

  if (hd.type == NGTCP2_PKT_SHORT) {
    if (ckm == conn->crypto.key_update.new_rx_ckm) {
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON, "commit new key");
      conn_commit_key_update(conn, hd.pkt_num);
    } else {
      if (ckm == pktns->crypto.rx.ckm &&
          (conn->flags & NGTCP2_CONN_FLAG_WAIT_FOR_REMOTE_KEY_UPDATE)) {
        ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON,
                        "key synchronization completed");
        conn->flags &= (uint16_t)~NGTCP2_CONN_FLAG_WAIT_FOR_REMOTE_KEY_UPDATE;
      }
      if (ckm->pkt_num > hd.pkt_num) {
        ckm->pkt_num = hd.pkt_num;
      }
    }
  }

  rv = pktns_commit_recv_pkt_num(pktns, hd.pkt_num);
  if (rv != 0) {
    return rv;
  }

  if (require_ack && ++pktns->acktr.rx_npkt >= NGTCP2_NUM_IMMEDIATE_ACK_PKT) {
    ngtcp2_acktr_immediate_ack(&pktns->acktr);
  }

  rv = ngtcp2_conn_sched_ack(conn, &pktns->acktr, hd.pkt_num, require_ack, ts);
  if (rv != 0) {
    return rv;
  }

  return (ssize_t)pktlen;
}

/*
 * conn_process_buffered_protected_pkt processes buffered 0RTT or
 * Short packets.
 *
 * This function returns 0 if it succeeds, or the same negative error
 * codes from conn_recv_pkt.
 */
static int conn_process_buffered_protected_pkt(ngtcp2_conn *conn,
                                               ngtcp2_pktns *pktns,
                                               ngtcp2_tstamp ts) {
  ssize_t nread;
  ngtcp2_pkt_chain **ppc, *next;
  int rv;

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON,
                  "processing buffered protected packet");

  for (ppc = &pktns->rx.buffed_pkts; *ppc;) {
    next = (*ppc)->next;
    nread = conn_recv_pkt(conn, &(*ppc)->path.path, (*ppc)->pkt, (*ppc)->pktlen,
                          ts);
    if (nread < 0 && !ngtcp2_err_is_fatal((int)nread)) {
      rv = conn_on_stateless_reset(conn, (*ppc)->pkt, (*ppc)->pktlen);
      if (rv == 0) {
        ngtcp2_pkt_chain_del(*ppc, conn->mem);
        *ppc = next;
        return 0;
      }
    }

    ngtcp2_pkt_chain_del(*ppc, conn->mem);
    *ppc = next;
    if (nread < 0) {
      if (nread == NGTCP2_ERR_DISCARD_PKT) {
        continue;
      }
      return (int)nread;
    }
  }

  return 0;
}

/*
 * conn_process_buffered_handshake_pkt processes buffered Handshake
 * packets.
 *
 * This function returns 0 if it succeeds, or the same negative error
 * codes from conn_recv_handshake_pkt.
 */
static int conn_process_buffered_handshake_pkt(ngtcp2_conn *conn,
                                               ngtcp2_tstamp ts) {
  ngtcp2_pktns *pktns = &conn->in_pktns;
  ssize_t nread;
  ngtcp2_pkt_chain **ppc, *next;

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON,
                  "processing buffered handshake packet");

  for (ppc = &pktns->rx.buffed_pkts; *ppc;) {
    next = (*ppc)->next;
    nread = conn_recv_handshake_pkt(conn, &(*ppc)->path.path, (*ppc)->pkt,
                                    (*ppc)->pktlen, ts);
    ngtcp2_pkt_chain_del(*ppc, conn->mem);
    *ppc = next;
    if (nread < 0) {
      if (nread == NGTCP2_ERR_DISCARD_PKT) {
        continue;
      }
      return (int)nread;
    }
  }

  return 0;
}

/*
 * conn_handshake_completed is called once cryptographic handshake has
 * completed.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User callback failed.
 */
static int conn_handshake_completed(ngtcp2_conn *conn) {
  int rv;

  conn->flags |= NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED_HANDLED;

  rv = conn_call_handshake_completed(conn);
  if (rv != 0) {
    return rv;
  }

  if (conn->local.bidi.max_streams > 0) {
    rv = conn_call_extend_max_local_streams_bidi(conn,
                                                 conn->local.bidi.max_streams);
    if (rv != 0) {
      return rv;
    }
  }
  if (conn->local.uni.max_streams > 0) {
    rv = conn_call_extend_max_local_streams_uni(conn,
                                                conn->local.uni.max_streams);
    if (rv != 0) {
      return rv;
    }
  }

  return 0;
}

/*
 * conn_recv_cpkt processes compound packet after handshake.  The
 * buffer pointed by |pkt| might contain multiple packets.  The Short
 * packet must be the last one because it does not have payload length
 * field.
 *
 * This function returns 0 if it succeeds, or the same negative error
 * codes from conn_recv_pkt except for NGTCP2_ERR_DISCARD_PKT.
 */
static int conn_recv_cpkt(ngtcp2_conn *conn, const ngtcp2_path *path,
                          const uint8_t *pkt, size_t pktlen, ngtcp2_tstamp ts) {
  ssize_t nread;
  int rv;
  const uint8_t *origpkt = pkt;
  size_t origpktlen = pktlen;

  while (pktlen) {
    nread = conn_recv_pkt(conn, path, pkt, pktlen, ts);
    if (nread < 0) {
      if (ngtcp2_err_is_fatal((int)nread)) {
        return (int)nread;
      }
      rv = conn_on_stateless_reset(conn, origpkt, origpktlen);
      if (rv == 0) {
        return 0;
      }
      if (nread == NGTCP2_ERR_DISCARD_PKT) {
        return 0;
      }
      return (int)nread;
    }

    assert(pktlen >= (size_t)nread);
    pkt += nread;
    pktlen -= (size_t)nread;

    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_PKT,
                    "read packet %zd left %zu", nread, pktlen);
  }

  return 0;
}

/*
 * conn_is_retired_path returns nonzero if |path| is inclued in
 * retired path list.
 */
static int conn_is_retired_path(ngtcp2_conn *conn, const ngtcp2_path *path) {
  size_t i, len = ngtcp2_ringbuf_len(&conn->dcid.retired);
  ngtcp2_dcid *dcid;

  for (i = 0; i < len; ++i) {
    dcid = ngtcp2_ringbuf_get(&conn->dcid.retired, i);
    if (ngtcp2_path_eq(&dcid->ps.path, path)) {
      return 1;
    }
  }

  return 0;
}

int ngtcp2_conn_read_pkt(ngtcp2_conn *conn, const ngtcp2_path *path,
                         const uint8_t *pkt, size_t pktlen, ngtcp2_tstamp ts) {
  int rv = 0;

  conn->log.last_ts = ts;

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON, "recv packet len=%zu",
                  pktlen);

  if (pktlen == 0) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  /* client does not expect a packet from unknown path. */
  if (!conn->server && !ngtcp2_path_eq(&conn->dcid.current.ps.path, path) &&
      (!conn->pv || !ngtcp2_path_eq(&conn->pv->dcid.ps.path, path)) &&
      !conn_is_retired_path(conn, path)) {
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON,
                    "ignore packet from unknown path");
    return 0;
  }

  switch (conn->state) {
  case NGTCP2_CS_CLIENT_INITIAL:
  case NGTCP2_CS_CLIENT_WAIT_HANDSHAKE:
  case NGTCP2_CS_CLIENT_TLS_HANDSHAKE_FAILED:
  case NGTCP2_CS_SERVER_INITIAL:
  case NGTCP2_CS_SERVER_WAIT_HANDSHAKE:
  case NGTCP2_CS_SERVER_TLS_HANDSHAKE_FAILED:
    return NGTCP2_ERR_INVALID_STATE;
  case NGTCP2_CS_CLOSING:
    return NGTCP2_ERR_CLOSING;
  case NGTCP2_CS_DRAINING:
    return NGTCP2_ERR_DRAINING;
  case NGTCP2_CS_POST_HANDSHAKE:
    rv = conn_recv_cpkt(conn, path, pkt, pktlen, ts);
    if (rv != 0) {
      break;
    }
    if (conn->state == NGTCP2_CS_DRAINING) {
      return NGTCP2_ERR_DRAINING;
    }
    break;
  }

  return rv;
}

/*
 * conn_check_pkt_num_exhausted returns nonzero if packet number is
 * exhausted in at least one of packet number space.
 */
static int conn_check_pkt_num_exhausted(ngtcp2_conn *conn) {
  return conn->in_pktns.tx.last_pkt_num == NGTCP2_MAX_PKT_NUM ||
         conn->hs_pktns.tx.last_pkt_num == NGTCP2_MAX_PKT_NUM ||
         conn->pktns.tx.last_pkt_num == NGTCP2_MAX_PKT_NUM;
}

/*
 * conn_server_hs_tx_left returns the maximum number of bytes that
 * server is allowed to send during handshake.
 */
static size_t conn_server_hs_tx_left(ngtcp2_conn *conn) {
  if (conn->flags & NGTCP2_CONN_FLAG_SADDR_VERIFIED) {
    return SIZE_MAX;
  }
  /* From QUIC spec: Prior to validating the client address, servers
     MUST NOT send more than three times as many bytes as the number
     of bytes they have received. */
  return conn->hs_recved * 3 - conn->hs_sent;
}

int ngtcp2_conn_read_handshake(ngtcp2_conn *conn, const ngtcp2_path *path,
                               const uint8_t *pkt, size_t pktlen,
                               ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_pktns *hs_pktns = &conn->hs_pktns;

  conn->log.last_ts = ts;

  if (pktlen > 0) {
    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_CON, "recv packet len=%zu",
                    pktlen);
  }

  switch (conn->state) {
  case NGTCP2_CS_CLIENT_INITIAL:
    /* TODO Better to log something when we ignore input */
    return 0;
  case NGTCP2_CS_CLIENT_WAIT_HANDSHAKE:
    rv = conn_recv_handshake_cpkt(conn, path, pkt, pktlen, ts);
    if (rv < 0) {
      return rv;
    }

    if (conn->state == NGTCP2_CS_CLIENT_INITIAL) {
      /* Retry packet was received */
      return 0;
    }

    if (hs_pktns->crypto.rx.ckm) {
      rv = conn_process_buffered_handshake_pkt(conn, ts);
      if (rv != 0) {
        return rv;
      }
    }

    return 0;
  case NGTCP2_CS_SERVER_INITIAL:
    rv = conn_recv_handshake_cpkt(conn, path, pkt, pktlen, ts);
    if (rv < 0) {
      return rv;
    }

    if (ngtcp2_rob_first_gap_offset(&conn->in_pktns.crypto.strm.rx.rob) == 0) {
      return 0;
    }

    /* Process re-ordered 0-RTT packets which were arrived before
       Initial packet. */
    if (conn->early.ckm) {
      rv = conn_process_buffered_protected_pkt(conn, &conn->in_pktns, ts);
      if (rv != 0) {
        return rv;
      }
    } else {
      delete_buffed_pkts(conn->in_pktns.rx.buffed_pkts, conn->mem);
      conn->in_pktns.rx.buffed_pkts = NULL;
    }

    return 0;
  case NGTCP2_CS_SERVER_WAIT_HANDSHAKE:
    rv = conn_recv_handshake_cpkt(conn, path, pkt, pktlen, ts);
    if (rv < 0) {
      return rv;
    }

    if (hs_pktns->crypto.rx.ckm) {
      rv = conn_process_buffered_handshake_pkt(conn, ts);
      if (rv != 0) {
        return rv;
      }
    }

    if (conn->hs_pktns.rx.max_pkt_num != -1) {
      conn_discard_initial_key(conn);
    }

    if (!(conn->flags & NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED)) {
      return 0;
    }

    if (!(conn->flags & NGTCP2_CONN_FLAG_TRANSPORT_PARAM_RECVED)) {
      return NGTCP2_ERR_REQUIRED_TRANSPORT_PARAM;
    }

    rv = conn_handshake_completed(conn);
    if (rv != 0) {
      return rv;
    }
    conn->state = NGTCP2_CS_POST_HANDSHAKE;

    rv = conn_process_buffered_protected_pkt(conn, &conn->hs_pktns, ts);
    if (rv != 0) {
      return rv;
    }

    conn->hs_pktns.acktr.flags |= NGTCP2_ACKTR_FLAG_PENDING_FINISHED_ACK;

    return 0;
  case NGTCP2_CS_CLOSING:
    return NGTCP2_ERR_CLOSING;
  case NGTCP2_CS_DRAINING:
    return NGTCP2_ERR_DRAINING;
  default:
    return 0;
  }
}

/*
 * conn_select_preferred_addr asks a client application to select a
 * server address from preferred addresses received from server.  If a
 * client chooses the address, path validation will start.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 * NGTCP2_ERR_CALLBACK_FAILURE
 *     User-defined callback function failed.
 */
static int conn_select_preferred_addr(ngtcp2_conn *conn) {
  uint8_t buf[128];
  ngtcp2_addr addr;
  int rv;
  ngtcp2_duration timeout;
  ngtcp2_pv *pv;
  ngtcp2_dcid dcid;

  ngtcp2_addr_init(&addr, buf, 0, NULL);

  rv = conn_call_select_preferred_addr(conn, &addr);
  if (rv != 0) {
    return rv;
  }

  if (addr.addrlen == 0 ||
      ngtcp2_addr_eq(&conn->dcid.current.ps.path.remote, &addr)) {
    return 0;
  }

  ngtcp2_dcid_init(
      &dcid, 1, &conn->remote.settings.preferred_address.cid,
      conn->remote.settings.preferred_address.stateless_reset_token);

  assert(conn->pv == NULL);

  timeout = conn_compute_pto(conn);
  timeout =
      ngtcp2_max(timeout, (ngtcp2_duration)(6ULL * NGTCP2_DEFAULT_INITIAL_RTT));

  rv = ngtcp2_pv_new(&pv, &dcid, timeout, NGTCP2_PV_FLAG_RETIRE_DCID_ON_FINISH,
                     &conn->log, conn->mem);
  if (rv != 0) {
    /* TODO Call ngtcp2_dcid_free here if it is introduced */
    return rv;
  }

  conn->pv = pv;

  ngtcp2_addr_copy(&pv->dcid.ps.path.local, &conn->dcid.current.ps.path.local);
  ngtcp2_addr_copy(&pv->dcid.ps.path.remote, &addr);

  conn_reset_congestion_state(conn);

  return 0;
}

/*
 * conn_retransmit_retry_early retransmits 0RTT packet after Retry is
 * received from server.
 */
static ssize_t conn_retransmit_retry_early(ngtcp2_conn *conn, uint8_t *dest,
                                           size_t destlen, ngtcp2_tstamp ts) {
  return conn_write_pkt(conn, dest, destlen, NULL, NGTCP2_PKT_0RTT, NULL, 0,
                        NULL, 0, /* require_padding = */ 0, ts);
}

/*
 * conn_write_handshake writes QUIC handshake packets to the buffer
 * pointed by |dest| of length |destlen|.  |early_datalen| specifies
 * the expected length of early data to send.  Specify 0 to
 * |early_datalen| if there is no early data.
 *
 * This function returns the number of bytes written to the buffer, or
 * one of the following negative error codes:
 *
 * NGTCP2_ERR_PKT_NUM_EXHAUSTED
 *     Packet number is exhausted.
 * NGTCP2_ERR_NOMEM
 *     Out of memory
 * NGTCP2_ERR_REQUIRED_TRANSPORT_PARAM
 *     Required transport parameter is missing.
 * NGTCP2_CS_CLOSING
 *     Connection is in closing state.
 * NGTCP2_CS_DRAINING
 *     Connection is in draining state.
 *
 * In addition to the above negative error codes, the same error codes
 * from conn_recv_pkt may also be returned.
 */
static ssize_t conn_write_handshake(ngtcp2_conn *conn, uint8_t *dest,
                                    size_t destlen, size_t early_datalen,
                                    ngtcp2_tstamp ts) {
  int rv;
  ssize_t res = 0, nwrite = 0, early_spktlen = 0;
  uint64_t cwnd;
  size_t origlen = destlen;
  size_t server_hs_tx_left;
  ngtcp2_rcvry_stat *rcs = &conn->rcs;
  size_t pending_early_datalen;

  conn->log.last_ts = ts;

  if (conn_check_pkt_num_exhausted(conn)) {
    return NGTCP2_ERR_PKT_NUM_EXHAUSTED;
  }

  cwnd = conn_cwnd_left(conn);
  destlen = ngtcp2_min(destlen, cwnd);

  switch (conn->state) {
  case NGTCP2_CS_CLIENT_INITIAL:
    pending_early_datalen = conn_retry_early_payloadlen(conn);
    if (pending_early_datalen) {
      early_datalen = pending_early_datalen;
    }

    if (!(conn->flags & NGTCP2_CONN_FLAG_RECV_RETRY)) {
      nwrite =
          conn_write_client_initial(conn, dest, destlen, early_datalen, ts);
      if (nwrite <= 0) {
        return nwrite;
      }
    } else {
      nwrite = conn_write_handshake_pkt(conn, dest, destlen, NGTCP2_PKT_INITIAL,
                                        early_datalen, ts);
      if (nwrite < 0) {
        return nwrite;
      }
    }

    if (pending_early_datalen) {
      early_spktlen = conn_retransmit_retry_early(conn, dest + nwrite,
                                                  destlen - (size_t)nwrite, ts);

      if (early_spktlen < 0) {
        assert(ngtcp2_err_is_fatal((int)early_spktlen));
        return early_spktlen;
      }
    }

    conn->state = NGTCP2_CS_CLIENT_WAIT_HANDSHAKE;

    return nwrite + early_spktlen;
  case NGTCP2_CS_CLIENT_WAIT_HANDSHAKE:
    if (!(conn->flags & NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED_HANDLED)) {
      pending_early_datalen = conn_retry_early_payloadlen(conn);
      if (pending_early_datalen) {
        early_datalen = pending_early_datalen;
      }
    }

    nwrite = conn_write_handshake_pkts(conn, dest, destlen, early_datalen, ts);
    if (nwrite < 0) {
      return nwrite;
    }

    if (conn->hs_pktns.tx.last_pkt_num != -1) {
      conn_discard_initial_key(conn);
    }

    res += nwrite;
    dest += nwrite;
    destlen -= (size_t)nwrite;

    if (!(conn->flags & NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED)) {
      nwrite = conn_retransmit_retry_early(conn, dest, destlen, ts);
      if (nwrite < 0) {
        return nwrite;
      }

      res += nwrite;

      if (res == 0) {
        /* This might send PADDING only Initial packet if client has
           nothing to send and does not have client handshake traffic
           key to prevent server from deadlocking. */
        nwrite = conn_write_handshake_ack_pkts(conn, dest, destlen,
                                               /* require_padding = */ 1, ts);
        if (nwrite < 0) {
          return nwrite;
        }
        res = nwrite;
      }
      if (res) {
        conn->flags &= (uint16_t)~NGTCP2_CONN_FLAG_FORCE_SEND_HANDSHAKE;
      }
      return res;
    }

    if (!(conn->flags & NGTCP2_CONN_FLAG_TRANSPORT_PARAM_RECVED)) {
      return NGTCP2_ERR_REQUIRED_TRANSPORT_PARAM;
    }

    rv = conn_handshake_completed(conn);
    if (rv != 0) {
      return (ssize_t)rv;
    }

    conn->state = NGTCP2_CS_POST_HANDSHAKE;

    if (conn->remote.settings.stateless_reset_token_present) {
      memcpy(conn->dcid.current.token,
             conn->remote.settings.stateless_reset_token,
             sizeof(conn->dcid.current.token));
    }

    conn_process_early_rtb(conn);

    rv = conn_process_buffered_protected_pkt(conn, &conn->hs_pktns, ts);
    if (rv != 0) {
      return (ssize_t)rv;
    }

    if (conn->remote.settings.preferred_address_present) {
      /* TODO Starting path validation against preferred address must
         be done after dropping Handshake key which is impossible at
         draft-18. */
      /* TODO And client has to send NEW_CONNECTION_ID before starting
         path validation */
      rv = conn_select_preferred_addr(conn);
      if (rv != 0) {
        return (ssize_t)rv;
      }
    }

    return res;
  case NGTCP2_CS_SERVER_INITIAL:
    nwrite = conn_write_server_handshake(conn, dest, destlen, ts);
    if (nwrite < 0) {
      return nwrite;
    }

    if (nwrite) {
      conn->state = NGTCP2_CS_SERVER_WAIT_HANDSHAKE;
      conn->hs_sent += (size_t)nwrite;
    }

    return nwrite;
  case NGTCP2_CS_SERVER_WAIT_HANDSHAKE:
    if (!(conn->flags & NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED)) {
      server_hs_tx_left = conn_server_hs_tx_left(conn);
      if (server_hs_tx_left == 0) {
        if (rcs->loss_detection_timer) {
          ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_RCV,
                          "loss detection timer canceled");
          rcs->loss_detection_timer = 0;
        }
        return 0;
      }

      destlen = ngtcp2_min(destlen, server_hs_tx_left);
      nwrite = conn_write_server_handshake(conn, dest, destlen, ts);
      if (nwrite < 0) {
        return nwrite;
      }

      /* TODO Write 1RTT ACK packet if we have received 0RTT packet */

      res += nwrite;
      dest += nwrite;
      destlen -= (size_t)nwrite;

      nwrite = conn_write_handshake_ack_pkts(
          conn, dest,
          res == 0 ? ngtcp2_min(origlen, server_hs_tx_left) : destlen,
          /* require_padding = */ res == 0, ts);
      if (nwrite < 0) {
        return nwrite;
      }

      res += nwrite;
      conn->hs_sent += (size_t)res;
      return res;
    }

    nwrite = conn_write_handshake_ack_pkts(
        conn, dest, origlen, /* require_padding = */ res == 0, ts);
    if (nwrite < 0) {
      return nwrite;
    }

    res += nwrite;

    if (!(conn->flags & NGTCP2_CONN_FLAG_TRANSPORT_PARAM_RECVED)) {
      return NGTCP2_ERR_REQUIRED_TRANSPORT_PARAM;
    }

    rv = conn_handshake_completed(conn);
    if (rv != 0) {
      return (ssize_t)rv;
    }
    conn->state = NGTCP2_CS_POST_HANDSHAKE;

    rv = conn_process_buffered_protected_pkt(conn, &conn->hs_pktns, ts);
    if (rv != 0) {
      return (ssize_t)rv;
    }

    conn->hs_pktns.acktr.flags |= NGTCP2_ACKTR_FLAG_PENDING_FINISHED_ACK;

    return res;
  case NGTCP2_CS_CLOSING:
    return NGTCP2_ERR_CLOSING;
  case NGTCP2_CS_DRAINING:
    return NGTCP2_ERR_DRAINING;
  default:
    return 0;
  }
}

ssize_t ngtcp2_conn_write_handshake(ngtcp2_conn *conn, uint8_t *dest,
                                    size_t destlen, ngtcp2_tstamp ts) {
  return conn_write_handshake(conn, dest, destlen, 0, ts);
}

ssize_t ngtcp2_conn_client_write_handshake(ngtcp2_conn *conn, uint8_t *dest,
                                           size_t destlen, ssize_t *pdatalen,
                                           int64_t stream_id, uint8_t fin,
                                           const ngtcp2_vec *datav,
                                           size_t datavcnt, ngtcp2_tstamp ts) {
  ngtcp2_strm *strm = NULL;
  int send_stream = 0;
  ssize_t spktlen, early_spktlen;
  uint64_t cwnd;
  int require_padding;
  int was_client_initial;
  size_t datalen = ngtcp2_vec_len(datav, datavcnt);
  size_t early_datalen = 0;

  assert(!conn->server);

  if (pdatalen) {
    *pdatalen = -1;
  }

  /* conn->early.ckm might be created in the first call of
     conn_handshake().  Check it later. */
  if (stream_id != -1 &&
      !(conn->flags & NGTCP2_CONN_FLAG_EARLY_DATA_REJECTED)) {
    strm = ngtcp2_conn_find_stream(conn, stream_id);
    if (strm == NULL) {
      return NGTCP2_ERR_STREAM_NOT_FOUND;
    }

    if (strm->flags & NGTCP2_STRM_FLAG_SHUT_WR) {
      return NGTCP2_ERR_STREAM_SHUT_WR;
    }

    send_stream = conn_retry_early_payloadlen(conn) == 0 &&
                  /* 0 length STREAM frame is allowed */
                  (datalen == 0 ||
                   (datalen > 0 && (strm->tx.max_offset - strm->tx.offset) &&
                    (conn->tx.max_offset - conn->tx.offset)));
    if (send_stream) {
      early_datalen =
          ngtcp2_min(datalen, strm->tx.max_offset - strm->tx.offset);
      early_datalen =
          ngtcp2_min(early_datalen, conn->tx.max_offset - conn->tx.offset) +
          NGTCP2_STREAM_OVERHEAD;
    }
  }

  was_client_initial = conn->state == NGTCP2_CS_CLIENT_INITIAL;
  spktlen = conn_write_handshake(conn, dest, destlen, early_datalen, ts);

  if (spktlen < 0) {
    return spktlen;
  }

  if (conn->pktns.crypto.tx.ckm || !conn->early.ckm || !send_stream) {
    return spktlen;
  }

  /* If spktlen > 0, we are making a compound packet.  If Initial
     packet is written, we have to pad bytes to 0-RTT packet. */

  require_padding = spktlen && was_client_initial;

  cwnd = conn_cwnd_left(conn);

  dest += spktlen;
  destlen -= (size_t)spktlen;
  destlen = ngtcp2_min(destlen, cwnd);

  early_spktlen =
      conn_write_pkt(conn, dest, destlen, pdatalen, NGTCP2_PKT_0RTT, strm, fin,
                     datav, datavcnt, require_padding, ts);

  if (early_spktlen < 0) {
    if (early_spktlen == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
      return spktlen;
    }
    return early_spktlen;
  }

  return spktlen + early_spktlen;
}

void ngtcp2_conn_handshake_completed(ngtcp2_conn *conn) {
  conn->flags |= NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED;
}

int ngtcp2_conn_get_handshake_completed(ngtcp2_conn *conn) {
  return (conn->flags & NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED) &&
         (conn->flags & NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED_HANDLED);
}

int ngtcp2_conn_sched_ack(ngtcp2_conn *conn, ngtcp2_acktr *acktr,
                          int64_t pkt_num, int active_ack, ngtcp2_tstamp ts) {
  int rv;
  (void)conn;

  rv = ngtcp2_acktr_add(acktr, pkt_num, active_ack, ts);
  if (rv != 0) {
    assert(rv != NGTCP2_ERR_INVALID_ARGUMENT);
    return rv;
  }

  return 0;
}

int ngtcp2_accept(ngtcp2_pkt_hd *dest, const uint8_t *pkt, size_t pktlen) {
  ssize_t nread;
  ngtcp2_pkt_hd hd, *p;

  if (dest) {
    p = dest;
  } else {
    p = &hd;
  }

  if (pktlen == 0 || (pkt[0] & NGTCP2_HEADER_FORM_BIT) == 0) {
    return -1;
  }

  nread = ngtcp2_pkt_decode_hd_long(p, pkt, pktlen);
  if (nread < 0) {
    return -1;
  }

  switch (p->type) {
  case NGTCP2_PKT_INITIAL:
    if (pktlen < NGTCP2_MIN_INITIAL_PKTLEN) {
      return -1;
    }
    break;
  case NGTCP2_PKT_0RTT:
    /* 0-RTT packet may arrive before Initial packet due to
       re-ordering. */
    break;
  default:
    return -1;
  }

  switch (p->version) {
  case NGTCP2_PROTO_VER:
    break;
  default:
    return 1;
  }

  return 0;
}

void ngtcp2_conn_set_aead_overhead(ngtcp2_conn *conn, size_t aead_overhead) {
  conn->crypto.aead_overhead = aead_overhead;
}

int ngtcp2_conn_install_initial_tx_keys(ngtcp2_conn *conn, const uint8_t *key,
                                        size_t keylen, const uint8_t *iv,
                                        size_t ivlen, const uint8_t *pn,
                                        size_t pnlen) {
  ngtcp2_pktns *pktns = &conn->in_pktns;
  int rv;

  if (pktns->crypto.tx.hp) {
    ngtcp2_vec_del(pktns->crypto.tx.hp, conn->mem);
    pktns->crypto.tx.hp = NULL;
  }
  if (pktns->crypto.tx.ckm) {
    ngtcp2_crypto_km_del(pktns->crypto.tx.ckm, conn->mem);
    pktns->crypto.tx.ckm = NULL;
  }

  rv = ngtcp2_crypto_km_new(&pktns->crypto.tx.ckm, key, keylen, iv, ivlen,
                            conn->mem);
  if (rv != 0) {
    return rv;
  }

  return ngtcp2_vec_new(&pktns->crypto.tx.hp, pn, pnlen, conn->mem);
}

int ngtcp2_conn_install_initial_rx_keys(ngtcp2_conn *conn, const uint8_t *key,
                                        size_t keylen, const uint8_t *iv,
                                        size_t ivlen, const uint8_t *pn,
                                        size_t pnlen) {
  ngtcp2_pktns *pktns = &conn->in_pktns;
  int rv;

  if (pktns->crypto.rx.hp) {
    ngtcp2_vec_del(pktns->crypto.rx.hp, conn->mem);
    pktns->crypto.rx.hp = NULL;
  }
  if (pktns->crypto.rx.ckm) {
    ngtcp2_crypto_km_del(pktns->crypto.rx.ckm, conn->mem);
    pktns->crypto.rx.ckm = NULL;
  }

  rv = ngtcp2_crypto_km_new(&pktns->crypto.rx.ckm, key, keylen, iv, ivlen,
                            conn->mem);
  if (rv != 0) {
    return rv;
  }

  return ngtcp2_vec_new(&pktns->crypto.rx.hp, pn, pnlen, conn->mem);
}

int ngtcp2_conn_install_handshake_tx_keys(ngtcp2_conn *conn, const uint8_t *key,
                                          size_t keylen, const uint8_t *iv,
                                          size_t ivlen, const uint8_t *pn,
                                          size_t pnlen) {
  ngtcp2_pktns *pktns = &conn->hs_pktns;
  int rv;

  assert(!pktns->crypto.tx.hp && !pktns->crypto.tx.ckm);

  rv = ngtcp2_crypto_km_new(&pktns->crypto.tx.ckm, key, keylen, iv, ivlen,
                            conn->mem);
  if (rv != 0) {
    return rv;
  }

  return ngtcp2_vec_new(&pktns->crypto.tx.hp, pn, pnlen, conn->mem);
}

int ngtcp2_conn_install_handshake_rx_keys(ngtcp2_conn *conn, const uint8_t *key,
                                          size_t keylen, const uint8_t *iv,
                                          size_t ivlen, const uint8_t *pn,
                                          size_t pnlen) {
  ngtcp2_pktns *pktns = &conn->hs_pktns;
  int rv;

  assert(!pktns->crypto.rx.hp && !pktns->crypto.rx.ckm);

  rv = ngtcp2_crypto_km_new(&pktns->crypto.rx.ckm, key, keylen, iv, ivlen,
                            conn->mem);
  if (rv != 0) {
    return rv;
  }

  return ngtcp2_vec_new(&pktns->crypto.rx.hp, pn, pnlen, conn->mem);
}

int ngtcp2_conn_install_early_keys(ngtcp2_conn *conn, const uint8_t *key,
                                   size_t keylen, const uint8_t *iv,
                                   size_t ivlen, const uint8_t *pn,
                                   size_t pnlen) {
  int rv;

  assert(!conn->early.hp && !conn->early.ckm);

  rv =
      ngtcp2_crypto_km_new(&conn->early.ckm, key, keylen, iv, ivlen, conn->mem);
  if (rv != 0) {
    return rv;
  }

  return ngtcp2_vec_new(&conn->early.hp, pn, pnlen, conn->mem);
}

int ngtcp2_conn_install_tx_keys(ngtcp2_conn *conn, const uint8_t *key,
                                size_t keylen, const uint8_t *iv, size_t ivlen,
                                const uint8_t *pn, size_t pnlen) {
  ngtcp2_pktns *pktns = &conn->pktns;
  int rv;

  assert(!pktns->crypto.tx.hp && !pktns->crypto.tx.ckm);

  rv = ngtcp2_crypto_km_new(&pktns->crypto.tx.ckm, key, keylen, iv, ivlen,
                            conn->mem);
  if (rv != 0) {
    return rv;
  }

  return ngtcp2_vec_new(&pktns->crypto.tx.hp, pn, pnlen, conn->mem);
}

int ngtcp2_conn_install_rx_keys(ngtcp2_conn *conn, const uint8_t *key,
                                size_t keylen, const uint8_t *iv, size_t ivlen,
                                const uint8_t *pn, size_t pnlen) {
  ngtcp2_pktns *pktns = &conn->pktns;
  int rv;

  assert(!pktns->crypto.rx.hp && !pktns->crypto.rx.ckm);

  rv = ngtcp2_crypto_km_new(&pktns->crypto.rx.ckm, key, keylen, iv, ivlen,
                            conn->mem);
  if (rv != 0) {
    return rv;
  }

  return ngtcp2_vec_new(&pktns->crypto.rx.hp, pn, pnlen, conn->mem);
}

int ngtcp2_conn_update_tx_key(ngtcp2_conn *conn, const uint8_t *key,
                              size_t keylen, const uint8_t *iv, size_t ivlen) {
  ngtcp2_pktns *pktns = &conn->pktns;
  int rv;

  if ((conn->flags & NGTCP2_CONN_FLAG_WAIT_FOR_REMOTE_KEY_UPDATE) ||
      conn->crypto.key_update.new_tx_ckm) {
    return NGTCP2_ERR_INVALID_STATE;
  }

  rv = ngtcp2_crypto_km_new(&conn->crypto.key_update.new_tx_ckm, key, keylen,
                            iv, ivlen, conn->mem);
  if (rv != 0) {
    return rv;
  }

  if (!(pktns->crypto.tx.ckm->flags & NGTCP2_CRYPTO_KM_FLAG_KEY_PHASE_ONE)) {
    conn->crypto.key_update.new_tx_ckm->flags |=
        NGTCP2_CRYPTO_KM_FLAG_KEY_PHASE_ONE;
  }

  return 0;
}

int ngtcp2_conn_update_rx_key(ngtcp2_conn *conn, const uint8_t *key,
                              size_t keylen, const uint8_t *iv, size_t ivlen) {
  ngtcp2_pktns *pktns = &conn->pktns;
  int rv;

  if ((conn->flags & NGTCP2_CONN_FLAG_WAIT_FOR_REMOTE_KEY_UPDATE) ||
      conn->crypto.key_update.new_rx_ckm) {
    return NGTCP2_ERR_INVALID_STATE;
  }

  rv = ngtcp2_crypto_km_new(&conn->crypto.key_update.new_rx_ckm, key, keylen,
                            iv, ivlen, conn->mem);
  if (rv != 0) {
    return rv;
  }

  if (!(pktns->crypto.rx.ckm->flags & NGTCP2_CRYPTO_KM_FLAG_KEY_PHASE_ONE)) {
    conn->crypto.key_update.new_rx_ckm->flags |=
        NGTCP2_CRYPTO_KM_FLAG_KEY_PHASE_ONE;
  }

  return 0;
}

int ngtcp2_conn_initiate_key_update(ngtcp2_conn *conn) {
  if ((conn->flags & NGTCP2_CONN_FLAG_WAIT_FOR_REMOTE_KEY_UPDATE) ||
      !conn->crypto.key_update.new_tx_ckm ||
      !conn->crypto.key_update.new_rx_ckm) {
    return NGTCP2_ERR_INVALID_STATE;
  }

  conn_commit_key_update(conn, NGTCP2_MAX_PKT_NUM);

  conn->flags |= NGTCP2_CONN_FLAG_WAIT_FOR_REMOTE_KEY_UPDATE;

  return 0;
}

ngtcp2_tstamp ngtcp2_conn_loss_detection_expiry(ngtcp2_conn *conn) {
  ngtcp2_tstamp ts = UINT64_MAX;

  if (conn->pv) {
    ts = ngtcp2_pv_next_expiry(conn->pv);
  }
  if (conn->rcs.loss_detection_timer) {
    ts = ngtcp2_min(ts, conn->rcs.loss_detection_timer);
  }
  return ts;
}

ngtcp2_tstamp ngtcp2_conn_ack_delay_expiry(ngtcp2_conn *conn) {
  ngtcp2_acktr *in_acktr = &conn->in_pktns.acktr;
  ngtcp2_acktr *hs_acktr = &conn->hs_pktns.acktr;
  ngtcp2_acktr *acktr = &conn->pktns.acktr;
  ngtcp2_tstamp ts = UINT64_MAX, t;

  if (in_acktr->first_unacked_ts != UINT64_MAX) {
    t = in_acktr->first_unacked_ts + NGTCP2_HS_ACK_DELAY;
    ts = ngtcp2_min(ts, t);
  }
  if (hs_acktr->first_unacked_ts != UINT64_MAX) {
    t = hs_acktr->first_unacked_ts + NGTCP2_HS_ACK_DELAY;
    ts = ngtcp2_min(ts, t);
  }
  if (acktr->first_unacked_ts != UINT64_MAX) {
    t = acktr->first_unacked_ts + conn_compute_ack_delay(conn);
    ts = ngtcp2_min(ts, t);
  }
  return ts;
}

ngtcp2_tstamp ngtcp2_conn_get_expiry(ngtcp2_conn *conn) {
  ngtcp2_tstamp t1 = ngtcp2_conn_loss_detection_expiry(conn);
  ngtcp2_tstamp t2 = ngtcp2_conn_ack_delay_expiry(conn);
  return ngtcp2_min(t1, t2);
}

/*
 * settings_copy_from_transport_params translates
 * ngtcp2_transport_params to ngtcp2_settings.
 */
static void
settings_copy_from_transport_params(ngtcp2_settings *dest,
                                    const ngtcp2_transport_params *src) {
  dest->max_stream_data_bidi_local = src->initial_max_stream_data_bidi_local;
  dest->max_stream_data_bidi_remote = src->initial_max_stream_data_bidi_remote;
  dest->max_stream_data_uni = src->initial_max_stream_data_uni;
  dest->max_data = src->initial_max_data;
  dest->max_streams_bidi = src->initial_max_streams_bidi;
  dest->max_streams_uni = src->initial_max_streams_uni;
  dest->idle_timeout = src->idle_timeout;
  dest->max_packet_size = src->max_packet_size;
  dest->stateless_reset_token_present = src->stateless_reset_token_present;
  if (src->stateless_reset_token_present) {
    memcpy(dest->stateless_reset_token, src->stateless_reset_token,
           sizeof(dest->stateless_reset_token));
  } else {
    memset(dest->stateless_reset_token, 0, sizeof(dest->stateless_reset_token));
  }
  dest->ack_delay_exponent = src->ack_delay_exponent;
  dest->disable_migration = src->disable_migration;
  dest->max_ack_delay = src->max_ack_delay;
  dest->preferred_address_present = src->preferred_address_present;
  if (src->preferred_address_present) {
    dest->preferred_address = src->preferred_address;
  }
}

/*
 * transport_params_copy_from_settings translates ngtcp2_settings to
 * ngtcp2_transport_params.
 */
static void transport_params_copy_from_settings(ngtcp2_transport_params *dest,
                                                const ngtcp2_settings *src) {
  dest->initial_max_stream_data_bidi_local = src->max_stream_data_bidi_local;
  dest->initial_max_stream_data_bidi_remote = src->max_stream_data_bidi_remote;
  dest->initial_max_stream_data_uni = src->max_stream_data_uni;
  dest->initial_max_data = src->max_data;
  dest->initial_max_streams_bidi = src->max_streams_bidi;
  dest->initial_max_streams_uni = src->max_streams_uni;
  dest->idle_timeout = src->idle_timeout;
  dest->max_packet_size = src->max_packet_size;
  dest->stateless_reset_token_present = src->stateless_reset_token_present;
  if (src->stateless_reset_token_present) {
    memcpy(dest->stateless_reset_token, src->stateless_reset_token,
           sizeof(dest->stateless_reset_token));
  } else {
    memset(dest->stateless_reset_token, 0, sizeof(dest->stateless_reset_token));
  }
  dest->ack_delay_exponent = src->ack_delay_exponent;
  dest->disable_migration = src->disable_migration;
  dest->max_ack_delay = src->max_ack_delay;
  dest->preferred_address_present = src->preferred_address_present;
  if (src->preferred_address_present) {
    dest->preferred_address = src->preferred_address;
  }
}

/*
 * conn_client_validate_transport_params validates |params| as client.
 * |params| must be sent with Encrypted Extensions.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_TRANSPORT_PARAM
 *     Transport parameters are invalid.
 */
static int
conn_client_validate_transport_params(ngtcp2_conn *conn,
                                      const ngtcp2_transport_params *params) {
  if (conn->flags & NGTCP2_CONN_FLAG_RECV_RETRY) {
    if (!params->original_connection_id_present) {
      return NGTCP2_ERR_TRANSPORT_PARAM;
    }
    if (!ngtcp2_cid_eq(&conn->rcid, &params->original_connection_id)) {
      return NGTCP2_ERR_TRANSPORT_PARAM;
    }
  }

  return 0;
}

static void conn_sync_stream_id_limit(ngtcp2_conn *conn) {
  conn->local.bidi.max_streams = conn->remote.settings.max_streams_bidi;
  conn->local.uni.max_streams = conn->remote.settings.max_streams_uni;
}

int ngtcp2_conn_set_remote_transport_params(
    ngtcp2_conn *conn, const ngtcp2_transport_params *params) {
  int rv;

  if (!conn->server) {
    rv = conn_client_validate_transport_params(conn, params);
    if (rv != 0) {
      return rv;
    }
  }

  ngtcp2_log_remote_tp(&conn->log,
                       conn->server
                           ? NGTCP2_TRANSPORT_PARAMS_TYPE_CLIENT_HELLO
                           : NGTCP2_TRANSPORT_PARAMS_TYPE_ENCRYPTED_EXTENSIONS,
                       params);

  settings_copy_from_transport_params(&conn->remote.settings, params);
  conn_sync_stream_id_limit(conn);

  conn->tx.max_offset = conn->remote.settings.max_data;

  conn->flags |= NGTCP2_CONN_FLAG_TRANSPORT_PARAM_RECVED;

  return 0;
}

void ngtcp2_conn_set_early_remote_transport_params(
    ngtcp2_conn *conn, const ngtcp2_transport_params *params) {
  settings_copy_from_transport_params(&conn->remote.settings, params);
  conn_sync_stream_id_limit(conn);

  conn->tx.max_offset = conn->remote.settings.max_data;
}

void ngtcp2_conn_get_local_transport_params(ngtcp2_conn *conn,
                                            ngtcp2_transport_params *params) {
  transport_params_copy_from_settings(params, &conn->local.settings);
  if (conn->server && (conn->flags & NGTCP2_CONN_FLAG_OCID_PRESENT)) {
    ngtcp2_cid_init(&params->original_connection_id, conn->ocid.data,
                    conn->ocid.datalen);
    params->original_connection_id_present = 1;
  } else {
    params->original_connection_id_present = 0;
  }
}

int ngtcp2_conn_open_bidi_stream(ngtcp2_conn *conn, int64_t *pstream_id,
                                 void *stream_user_data) {
  int rv;
  ngtcp2_strm *strm;

  if (ngtcp2_ord_stream_id(conn->local.bidi.next_stream_id) >
      conn->local.bidi.max_streams) {
    return NGTCP2_ERR_STREAM_ID_BLOCKED;
  }

  strm = ngtcp2_mem_malloc(conn->mem, sizeof(ngtcp2_strm));
  if (strm == NULL) {
    return NGTCP2_ERR_NOMEM;
  }

  rv = ngtcp2_conn_init_stream(conn, strm, conn->local.bidi.next_stream_id,
                               stream_user_data);
  if (rv != 0) {
    ngtcp2_mem_free(conn->mem, strm);
    return rv;
  }

  *pstream_id = conn->local.bidi.next_stream_id;
  conn->local.bidi.next_stream_id += 4;

  return 0;
}

int ngtcp2_conn_open_uni_stream(ngtcp2_conn *conn, int64_t *pstream_id,
                                void *stream_user_data) {
  int rv;
  ngtcp2_strm *strm;

  if (ngtcp2_ord_stream_id(conn->local.uni.next_stream_id) >
      conn->local.uni.max_streams) {
    return NGTCP2_ERR_STREAM_ID_BLOCKED;
  }

  strm = ngtcp2_mem_malloc(conn->mem, sizeof(ngtcp2_strm));
  if (strm == NULL) {
    return NGTCP2_ERR_NOMEM;
  }

  rv = ngtcp2_conn_init_stream(conn, strm, conn->local.uni.next_stream_id,
                               stream_user_data);
  if (rv != 0) {
    ngtcp2_mem_free(conn->mem, strm);
    return rv;
  }
  ngtcp2_strm_shutdown(strm, NGTCP2_STRM_FLAG_SHUT_RD);

  *pstream_id = conn->local.uni.next_stream_id;
  conn->local.uni.next_stream_id += 4;

  return 0;
}

ngtcp2_strm *ngtcp2_conn_find_stream(ngtcp2_conn *conn, int64_t stream_id) {
  ngtcp2_map_entry *me;

  me = ngtcp2_map_find(&conn->strms, (uint64_t)stream_id);
  if (me == NULL) {
    return NULL;
  }

  return ngtcp2_struct_of(me, ngtcp2_strm, me);
}

ssize_t ngtcp2_conn_write_stream(ngtcp2_conn *conn, ngtcp2_path *path,
                                 uint8_t *dest, size_t destlen,
                                 ssize_t *pdatalen, int64_t stream_id,
                                 uint8_t fin, const uint8_t *data,
                                 size_t datalen, ngtcp2_tstamp ts) {
  ngtcp2_vec datav;

  datav.len = datalen;
  datav.base = (uint8_t *)data;

  return ngtcp2_conn_writev_stream(conn, path, dest, destlen, pdatalen,
                                   stream_id, fin, &datav, 1, ts);
}

ssize_t ngtcp2_conn_writev_stream(ngtcp2_conn *conn, ngtcp2_path *path,
                                  uint8_t *dest, size_t destlen,
                                  ssize_t *pdatalen, int64_t stream_id,
                                  uint8_t fin, const ngtcp2_vec *datav,
                                  size_t datavcnt, ngtcp2_tstamp ts) {
  ngtcp2_strm *strm;
  ssize_t nwrite;
  uint64_t cwnd;
  ngtcp2_pktns *pktns = &conn->pktns;
  size_t origlen = destlen;
  size_t server_hs_tx_left;
  ngtcp2_rcvry_stat *rcs = &conn->rcs;
  int rv;

  conn->log.last_ts = ts;

  if (pdatalen) {
    *pdatalen = -1;
  }

  switch (conn->state) {
  case NGTCP2_CS_CLOSING:
    return NGTCP2_ERR_CLOSING;
  case NGTCP2_CS_DRAINING:
    return NGTCP2_ERR_DRAINING;
  }

  if (conn_check_pkt_num_exhausted(conn)) {
    return NGTCP2_ERR_PKT_NUM_EXHAUSTED;
  }

  rv = conn_remove_retired_connection_id(conn, ts);
  if (rv != 0) {
    return rv;
  }

  strm = ngtcp2_conn_find_stream(conn, stream_id);
  if (strm == NULL) {
    return NGTCP2_ERR_STREAM_NOT_FOUND;
  }

  if (strm->flags & NGTCP2_STRM_FLAG_SHUT_WR) {
    return NGTCP2_ERR_STREAM_SHUT_WR;
  }

  nwrite = conn_write_path_response(conn, path, dest, destlen, ts);
  if (nwrite) {
    return nwrite;
  }

  if (conn->pv && conn_peer_has_unused_cid(conn)) {
    nwrite = conn_write_path_challenge(conn, path, dest, destlen, ts);
    if (nwrite) {
      return nwrite;
    }
  }

  cwnd = conn_cwnd_left(conn);
  destlen = ngtcp2_min(destlen, cwnd);

  if (conn->server) {
    server_hs_tx_left = conn_server_hs_tx_left(conn);
    if (server_hs_tx_left == 0) {
      if (rcs->loss_detection_timer) {
        ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_RCV,
                        "loss detection timer canceled");
        rcs->loss_detection_timer = 0;
      }
      return 0;
    }
    destlen = ngtcp2_min(destlen, server_hs_tx_left);
  }

  if (path) {
    ngtcp2_path_copy(path, &conn->dcid.current.ps.path);
  }

  if (conn_handshake_remnants_left(conn)) {
    nwrite = conn_write_handshake_pkts(conn, dest, destlen, 0, ts);
    if (nwrite) {
      return nwrite;
    }
  }
  nwrite = conn_write_handshake_ack_pkts(conn, dest, origlen,
                                         /* require_padding = */ 1, ts);
  if (nwrite) {
    return nwrite;
  }

  if (pktns->crypto.tx.ckm) {
    if (conn->rcs.probe_pkt_left) {
      return conn_write_probe_pkt(conn, dest, origlen, pdatalen, strm, fin,
                                  datav, datavcnt, ts);
    }

    nwrite =
        conn_write_pkt(conn, dest, destlen, pdatalen, NGTCP2_PKT_SHORT, strm,
                       fin, datav, datavcnt, /* require_padding = */ 0, ts);
    if (nwrite < 0) {
      assert(nwrite != NGTCP2_ERR_NOBUF);
      return nwrite;
    }
    if (nwrite == 0) {
      return conn_write_protected_ack_pkt(conn, dest, origlen, ts);
    }
    return nwrite;
  }

  /* Send STREAM frame in 0-RTT packet. */
  if (conn->server || !conn->early.ckm) {
    return NGTCP2_ERR_NOKEY;
  }

  if (conn->flags & NGTCP2_CONN_FLAG_EARLY_DATA_REJECTED) {
    return NGTCP2_ERR_EARLY_DATA_REJECTED;
  }

  return conn_write_pkt(conn, dest, destlen, pdatalen, NGTCP2_PKT_0RTT, strm,
                        fin, datav, datavcnt, /* require_padding = */ 0, ts);
}

ssize_t ngtcp2_conn_write_connection_close(ngtcp2_conn *conn, ngtcp2_path *path,
                                           uint8_t *dest, size_t destlen,
                                           uint16_t error_code,
                                           ngtcp2_tstamp ts) {
  ssize_t nwrite;
  ngtcp2_frame fr;
  uint8_t pkt_type;

  conn->log.last_ts = ts;

  if (conn_check_pkt_num_exhausted(conn)) {
    return NGTCP2_ERR_PKT_NUM_EXHAUSTED;
  }

  switch (conn->state) {
  case NGTCP2_CS_CLOSING:
  case NGTCP2_CS_DRAINING:
    return NGTCP2_ERR_INVALID_STATE;
  }

  if (path) {
    ngtcp2_path_copy(path, &conn->dcid.current.ps.path);
  }

  fr.type = NGTCP2_FRAME_CONNECTION_CLOSE;
  fr.connection_close.error_code = error_code;
  fr.connection_close.frame_type = 0;
  fr.connection_close.reasonlen = 0;
  fr.connection_close.reason = NULL;

  if (conn->state == NGTCP2_CS_POST_HANDSHAKE) {
    pkt_type = NGTCP2_PKT_SHORT;
  } else if (conn->hs_pktns.crypto.tx.ckm) {
    pkt_type = NGTCP2_PKT_HANDSHAKE;
  } else {
    assert(conn->in_pktns.crypto.tx.ckm);
    pkt_type = NGTCP2_PKT_INITIAL;
  }

  nwrite = conn_write_single_frame_pkt(conn, dest, destlen, pkt_type,
                                       &conn->dcid.current.cid, &fr,
                                       NGTCP2_RTB_FLAG_NONE, ts);

  if (nwrite > 0) {
    conn->state = NGTCP2_CS_CLOSING;
  }

  return nwrite;
}

ssize_t ngtcp2_conn_write_application_close(ngtcp2_conn *conn,
                                            ngtcp2_path *path, uint8_t *dest,
                                            size_t destlen,
                                            uint16_t app_error_code,
                                            ngtcp2_tstamp ts) {
  ssize_t nwrite;
  ngtcp2_frame fr;

  conn->log.last_ts = ts;

  if (conn_check_pkt_num_exhausted(conn)) {
    return NGTCP2_ERR_PKT_NUM_EXHAUSTED;
  }

  switch (conn->state) {
  case NGTCP2_CS_POST_HANDSHAKE:
    break;
  default:
    return NGTCP2_ERR_INVALID_STATE;
  }

  if (path) {
    ngtcp2_path_copy(path, &conn->dcid.current.ps.path);
  }

  fr.type = NGTCP2_FRAME_CONNECTION_CLOSE_APP;
  fr.connection_close.error_code = app_error_code;
  fr.connection_close.frame_type = 0;
  fr.connection_close.reasonlen = 0;
  fr.connection_close.reason = NULL;

  nwrite = conn_write_single_frame_pkt(conn, dest, destlen, NGTCP2_PKT_SHORT,
                                       &conn->dcid.current.cid, &fr,
                                       NGTCP2_RTB_FLAG_NONE, ts);

  if (nwrite > 0) {
    conn->state = NGTCP2_CS_CLOSING;
  }

  return nwrite;
}

int ngtcp2_conn_is_in_closing_period(ngtcp2_conn *conn) {
  return conn->state == NGTCP2_CS_CLOSING;
}

int ngtcp2_conn_is_in_draining_period(ngtcp2_conn *conn) {
  return conn->state == NGTCP2_CS_DRAINING;
}

int ngtcp2_conn_close_stream(ngtcp2_conn *conn, ngtcp2_strm *strm,
                             uint16_t app_error_code) {
  int rv;

  if (!strm->app_error_code) {
    app_error_code = strm->app_error_code;
  }

  rv = ngtcp2_map_remove(&conn->strms, strm->me.key);
  if (rv != 0) {
    assert(rv != NGTCP2_ERR_INVALID_ARGUMENT);
    return rv;
  }

  rv = conn_call_stream_close(conn, strm, app_error_code);
  if (rv != 0) {
    goto fin;
  }

  if (!conn_local_stream(conn, strm->stream_id)) {
    if (bidi_stream(strm->stream_id)) {
      handle_max_remote_streams_extension(
          &conn->remote.bidi.unsent_max_streams);
    } else {
      handle_max_remote_streams_extension(&conn->remote.uni.unsent_max_streams);
    }
  }

  if (ngtcp2_strm_is_tx_queued(strm)) {
    ngtcp2_pq_remove(&conn->tx.strmq, &strm->pe);
  }

fin:
  ngtcp2_strm_free(strm);
  ngtcp2_mem_free(conn->mem, strm);

  return rv;
}

int ngtcp2_conn_close_stream_if_shut_rdwr(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                          uint16_t app_error_code) {
  if ((strm->flags & NGTCP2_STRM_FLAG_SHUT_RDWR) ==
          NGTCP2_STRM_FLAG_SHUT_RDWR &&
      ((strm->flags & NGTCP2_STRM_FLAG_RECV_RST) ||
       ngtcp2_rob_first_gap_offset(&strm->rx.rob) == strm->rx.last_offset) &&
      (((strm->flags & NGTCP2_STRM_FLAG_SENT_RST) &&
        (strm->flags & NGTCP2_STRM_FLAG_RST_ACKED)) ||
       (!(strm->flags & NGTCP2_STRM_FLAG_SENT_RST) &&
        ngtcp2_strm_is_all_tx_data_acked(strm)))) {
    return ngtcp2_conn_close_stream(conn, strm, app_error_code);
  }
  return 0;
}

/*
 * conn_shutdown_stream_write closes send stream with error code
 * |app_error_code|.  RESET_STREAM frame is scheduled.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_shutdown_stream_write(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                      uint16_t app_error_code) {
  if (strm->flags & NGTCP2_STRM_FLAG_SENT_RST) {
    return 0;
  }

  /* Set this flag so that we don't accidentally send DATA to this
     stream. */
  strm->flags |= NGTCP2_STRM_FLAG_SHUT_WR | NGTCP2_STRM_FLAG_SENT_RST;
  strm->app_error_code = app_error_code;

  ngtcp2_strm_streamfrq_clear(strm);

  return conn_reset_stream(conn, strm, app_error_code);
}

/*
 * conn_shutdown_stream_read closes read stream with error code
 * |app_error_code|.  STOP_SENDING frame is scheduled.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_shutdown_stream_read(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                     uint16_t app_error_code) {
  if (strm->flags &
      (NGTCP2_STRM_FLAG_SHUT_RD | NGTCP2_STRM_FLAG_STOP_SENDING)) {
    return 0;
  }

  strm->flags |= NGTCP2_STRM_FLAG_STOP_SENDING;
  strm->app_error_code = app_error_code;

  return conn_stop_sending(conn, strm, app_error_code);
}

int ngtcp2_conn_shutdown_stream(ngtcp2_conn *conn, int64_t stream_id,
                                uint16_t app_error_code) {
  int rv;
  ngtcp2_strm *strm;

  strm = ngtcp2_conn_find_stream(conn, stream_id);
  if (strm == NULL) {
    return NGTCP2_ERR_STREAM_NOT_FOUND;
  }

  rv = conn_shutdown_stream_read(conn, strm, app_error_code);
  if (rv != 0) {
    return rv;
  }

  rv = conn_shutdown_stream_write(conn, strm, app_error_code);
  if (rv != 0) {
    return rv;
  }

  return 0;
}

int ngtcp2_conn_shutdown_stream_write(ngtcp2_conn *conn, int64_t stream_id,
                                      uint16_t app_error_code) {
  ngtcp2_strm *strm;

  strm = ngtcp2_conn_find_stream(conn, stream_id);
  if (strm == NULL) {
    return NGTCP2_ERR_STREAM_NOT_FOUND;
  }

  return conn_shutdown_stream_write(conn, strm, app_error_code);
}

int ngtcp2_conn_shutdown_stream_read(ngtcp2_conn *conn, int64_t stream_id,
                                     uint16_t app_error_code) {
  ngtcp2_strm *strm;

  strm = ngtcp2_conn_find_stream(conn, stream_id);
  if (strm == NULL) {
    return NGTCP2_ERR_STREAM_NOT_FOUND;
  }

  return conn_shutdown_stream_read(conn, strm, app_error_code);
}

/*
 * conn_extend_max_stream_offset extends stream level flow control
 * window by |datalen| of the stream denoted by |strm|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_extend_max_stream_offset(ngtcp2_conn *conn, ngtcp2_strm *strm,
                                         size_t datalen) {
  ngtcp2_strm *top;

  if (strm->rx.unsent_max_offset <= NGTCP2_MAX_VARINT - datalen) {
    strm->rx.unsent_max_offset += datalen;
  }

  if (!(strm->flags &
        (NGTCP2_STRM_FLAG_SHUT_RD | NGTCP2_STRM_FLAG_STOP_SENDING)) &&
      !ngtcp2_strm_is_tx_queued(strm) &&
      conn_should_send_max_stream_data(conn, strm)) {
    if (!ngtcp2_pq_empty(&conn->tx.strmq)) {
      top = ngtcp2_conn_tx_strmq_top(conn);
      strm->cycle = top->cycle;
    }
    return ngtcp2_conn_tx_strmq_push(conn, strm);
  }

  return 0;
}

int ngtcp2_conn_extend_max_stream_offset(ngtcp2_conn *conn, int64_t stream_id,
                                         size_t datalen) {
  ngtcp2_strm *strm;

  strm = ngtcp2_conn_find_stream(conn, stream_id);
  if (strm == NULL) {
    return NGTCP2_ERR_STREAM_NOT_FOUND;
  }

  return conn_extend_max_stream_offset(conn, strm, datalen);
}

void ngtcp2_conn_extend_max_offset(ngtcp2_conn *conn, size_t datalen) {
  if (NGTCP2_MAX_VARINT < (uint64_t)datalen ||
      conn->rx.unsent_max_offset > NGTCP2_MAX_VARINT - datalen) {
    conn->rx.unsent_max_offset = NGTCP2_MAX_VARINT;
    return;
  }

  conn->rx.unsent_max_offset += datalen;
}

size_t ngtcp2_conn_get_bytes_in_flight(ngtcp2_conn *conn) {
  return conn->ccs.bytes_in_flight;
}

const ngtcp2_cid *ngtcp2_conn_get_dcid(ngtcp2_conn *conn) {
  return &conn->dcid.current.cid;
}

uint32_t ngtcp2_conn_get_negotiated_version(ngtcp2_conn *conn) {
  return conn->version;
}

int ngtcp2_conn_early_data_rejected(ngtcp2_conn *conn) {
  ngtcp2_pktns *pktns = &conn->pktns;
  ngtcp2_rtb *rtb = &conn->pktns.rtb;
  ngtcp2_frame_chain *frc = NULL;
  int rv;

  conn->flags |= NGTCP2_CONN_FLAG_EARLY_DATA_REJECTED;

  ngtcp2_rtb_remove_all(rtb, &frc);

  rv = conn_resched_frames(conn, pktns, &frc);
  if (rv != 0) {
    assert(ngtcp2_err_is_fatal(rv));
    ngtcp2_frame_chain_list_del(frc, conn->mem);
    return rv;
  }

  return rv;
}

void ngtcp2_conn_update_rtt(ngtcp2_conn *conn, uint64_t rtt,
                            uint64_t ack_delay) {
  ngtcp2_rcvry_stat *rcs = &conn->rcs;

  rcs->latest_rtt = rtt;

  if (rcs->smoothed_rtt < 1e-9) {
    rcs->min_rtt = rtt;
    rcs->smoothed_rtt = (double)rtt;
    rcs->rttvar = (double)rtt / 2;
    return;
  }

  rcs->min_rtt = ngtcp2_min(rcs->min_rtt, rtt);
  if (conn->flags & NGTCP2_CONN_FLAG_HANDSHAKE_COMPLETED) {
    ack_delay = ngtcp2_min(ack_delay, conn->remote.settings.max_ack_delay);
  } else {
    ack_delay = ngtcp2_min(ack_delay, NGTCP2_DEFAULT_MAX_ACK_DELAY);
  }
  if (rtt > rcs->min_rtt + ack_delay) {
    rtt -= ack_delay;
  }

  rcs->rttvar = rcs->rttvar * 3 / 4 + fabs(rcs->smoothed_rtt - (double)rtt) / 4;
  rcs->smoothed_rtt = rcs->smoothed_rtt * 7 / 8 + (double)rtt / 8;

  ngtcp2_log_info(
      &conn->log, NGTCP2_LOG_EVENT_RCV,
      "latest_rtt=%" PRIu64 " min_rtt=%" PRIu64
      " smoothed_rtt=%.3f rttvar=%.3f ack_delay=%" PRIu64,
      rcs->latest_rtt / NGTCP2_MILLISECONDS, rcs->min_rtt / NGTCP2_MILLISECONDS,
      rcs->smoothed_rtt / NGTCP2_MILLISECONDS,
      rcs->rttvar / NGTCP2_MILLISECONDS, ack_delay / NGTCP2_MILLISECONDS);
}

void ngtcp2_conn_get_rcvry_stat(ngtcp2_conn *conn, ngtcp2_rcvry_stat *rcs) {
  *rcs = conn->rcs;
}

static ngtcp2_pktns *conn_get_earliest_loss_time_pktns(ngtcp2_conn *conn) {
  ngtcp2_pktns *in_pktns = &conn->in_pktns;
  ngtcp2_pktns *hs_pktns = &conn->hs_pktns;
  ngtcp2_pktns *pktns = &conn->pktns;
  ngtcp2_pktns *res = in_pktns;

  if (res->rtb.loss_time == 0 ||
      (hs_pktns->rtb.loss_time &&
       hs_pktns->rtb.loss_time < res->rtb.loss_time)) {
    res = hs_pktns;
  }
  if (res->rtb.loss_time == 0 ||
      (pktns->rtb.loss_time && pktns->rtb.loss_time < res->rtb.loss_time)) {
    res = pktns;
  }

  return res;
}

void ngtcp2_conn_set_loss_detection_timer(ngtcp2_conn *conn) {
  ngtcp2_rcvry_stat *rcs = &conn->rcs;
  ngtcp2_duration timeout;
  ngtcp2_pktns *in_pktns = &conn->in_pktns;
  ngtcp2_pktns *hs_pktns = &conn->hs_pktns;
  ngtcp2_pktns *pktns = &conn->pktns;
  ngtcp2_pktns *loss_pktns = conn_get_earliest_loss_time_pktns(conn);

  if (loss_pktns->rtb.loss_time) {
    rcs->loss_detection_timer = loss_pktns->rtb.loss_time;

    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_RCV,
                    "loss_detection_timer=%" PRIu64 " nonzero crypto loss time",
                    rcs->loss_detection_timer);
    return;
  }

  if (ngtcp2_rtb_num_ack_eliciting(&in_pktns->rtb) ||
      ngtcp2_rtb_num_ack_eliciting(&hs_pktns->rtb) ||
      (!conn->server && !conn->pktns.crypto.tx.ckm)) {
    if (rcs->smoothed_rtt < 1e-09) {
      timeout = 2 * NGTCP2_DEFAULT_INITIAL_RTT;
    } else {
      timeout = (ngtcp2_duration)(2 * rcs->smoothed_rtt);
    }

    timeout = ngtcp2_max(timeout, NGTCP2_GRANULARITY);
    timeout *= 1ULL << rcs->crypto_count;

    rcs->loss_detection_timer = rcs->last_hs_tx_pkt_ts + timeout;

    ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_RCV,
                    "loss_detection_timer=%" PRIu64
                    " last_hs_tx_pkt_ts=%" PRIu64 " timeout=%" PRIu64,
                    rcs->loss_detection_timer, rcs->last_hs_tx_pkt_ts,
                    timeout / NGTCP2_MILLISECONDS);
    return;
  }

  if (ngtcp2_rtb_num_ack_eliciting(&pktns->rtb) == 0) {
    if (rcs->loss_detection_timer) {
      ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_RCV,
                      "loss detection timer canceled");
      rcs->loss_detection_timer = 0;
    }
    return;
  }

  rcs->loss_detection_timer = rcs->last_tx_pkt_ts + conn_compute_pto(conn);
}

/*
 * conn_handshake_pkt_lost is called when handshake packets which
 * belong to |pktns| are lost.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
static int conn_on_crypto_timeout(ngtcp2_conn *conn, ngtcp2_pktns *pktns) {
  ngtcp2_frame_chain *frc = NULL;
  int rv;

  rv = ngtcp2_rtb_on_crypto_timeout(&pktns->rtb, &frc);
  if (rv != 0) {
    assert(ngtcp2_err_is_fatal(rv));
    ngtcp2_frame_chain_list_del(frc, conn->mem);
    return rv;
  }

  rv = conn_resched_frames(conn, pktns, &frc);
  if (rv != 0) {
    ngtcp2_frame_chain_list_del(frc, conn->mem);
    return rv;
  }

  return 0;
}

int ngtcp2_conn_on_loss_detection_timer(ngtcp2_conn *conn, ngtcp2_tstamp ts) {
  ngtcp2_rcvry_stat *rcs = &conn->rcs;
  int rv;
  ngtcp2_pktns *in_pktns = &conn->in_pktns;
  ngtcp2_pktns *hs_pktns = &conn->hs_pktns;
  ngtcp2_pktns *loss_pktns = conn_get_earliest_loss_time_pktns(conn);

  conn->log.last_ts = ts;

  if (!rcs->loss_detection_timer) {
    return 0;
  }

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_RCV,
                  "loss detection timer fired");

  if (loss_pktns->rtb.loss_time) {
    rv = ngtcp2_conn_detect_lost_pkt(conn, loss_pktns, rcs, ts);
    if (rv != 0) {
      return rv;
    }
  } else if (ngtcp2_rtb_num_ack_eliciting(&in_pktns->rtb) ||
             ngtcp2_rtb_num_ack_eliciting(&hs_pktns->rtb)) {
    rv = conn_on_crypto_timeout(conn, in_pktns);
    if (rv != 0) {
      return rv;
    }
    rv = conn_on_crypto_timeout(conn, hs_pktns);
    if (rv != 0) {
      return rv;
    }
    if (!conn->server && !conn->hs_pktns.crypto.tx.ckm) {
      conn->flags |= NGTCP2_CONN_FLAG_FORCE_SEND_HANDSHAKE;
    }
    ++rcs->crypto_count;
  } else if (!conn->server && !conn->pktns.crypto.tx.ckm) {
    conn->flags |= NGTCP2_CONN_FLAG_FORCE_SEND_HANDSHAKE;
    ++rcs->crypto_count;
  } else {
    rcs->probe_pkt_left = 2;
    ++rcs->pto_count;
  }

  ngtcp2_log_info(&conn->log, NGTCP2_LOG_EVENT_RCV,
                  "crypto_count=%zu pto_count=%zu", rcs->crypto_count,
                  rcs->pto_count);

  ngtcp2_conn_set_loss_detection_timer(conn);

  return 0;
}

int ngtcp2_conn_submit_crypto_data(ngtcp2_conn *conn,
                                   ngtcp2_crypto_level crypto_level,
                                   const uint8_t *data, const size_t datalen) {
  ngtcp2_pktns *pktns;
  ngtcp2_crypto_frame_chain *frc;
  ngtcp2_crypto *fr;
  int rv;

  if (datalen == 0) {
    return 0;
  }

  switch (crypto_level) {
  case NGTCP2_CRYPTO_LEVEL_INITIAL:
    pktns = &conn->in_pktns;
    break;
  case NGTCP2_CRYPTO_LEVEL_HANDSHAKE:
    pktns = &conn->hs_pktns;
    break;
  case NGTCP2_CRYPTO_LEVEL_APP:
    pktns = &conn->pktns;
    break;
  default:
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  rv = ngtcp2_crypto_frame_chain_new(&frc, conn->mem);
  if (rv != 0) {
    return rv;
  }

  fr = &frc->fr;

  fr->type = NGTCP2_FRAME_CRYPTO;
  fr->offset = pktns->crypto.tx.offset;
  fr->datacnt = 1;
  fr->data[0].len = datalen;
  fr->data[0].base = (uint8_t *)data;

  rv = ngtcp2_pq_push(&pktns->crypto.tx.frq, &frc->pe);
  if (rv != 0) {
    ngtcp2_crypto_frame_chain_del(frc, conn->mem);
    return rv;
  }

  pktns->crypto.strm.tx.offset += datalen;
  pktns->crypto.tx.offset += datalen;

  return 0;
}

void ngtcp2_conn_set_retry_ocid(ngtcp2_conn *conn, const ngtcp2_cid *ocid) {
  assert(conn->server);

  conn->flags |= NGTCP2_CONN_FLAG_OCID_PRESENT;
  conn->ocid = *ocid;
}

ngtcp2_strm *ngtcp2_conn_tx_strmq_top(ngtcp2_conn *conn) {
  assert(!ngtcp2_pq_empty(&conn->tx.strmq));
  return ngtcp2_struct_of(ngtcp2_pq_top(&conn->tx.strmq), ngtcp2_strm, pe);
}

void ngtcp2_conn_tx_strmq_pop(ngtcp2_conn *conn) {
  ngtcp2_strm *strm = ngtcp2_conn_tx_strmq_top(conn);
  assert(strm);
  ngtcp2_pq_pop(&conn->tx.strmq);
  strm->pe.index = NGTCP2_PQ_BAD_INDEX;
}

int ngtcp2_conn_tx_strmq_push(ngtcp2_conn *conn, ngtcp2_strm *strm) {
  return ngtcp2_pq_push(&conn->tx.strmq, &strm->pe);
}

size_t ngtcp2_conn_get_num_scid(ngtcp2_conn *conn) {
  return ngtcp2_ksl_len(&conn->scid.set);
}

size_t ngtcp2_conn_get_scid(ngtcp2_conn *conn, ngtcp2_cid *dest) {
  ngtcp2_ksl_it it;
  ngtcp2_scid *scid;

  for (it = ngtcp2_ksl_begin(&conn->scid.set); !ngtcp2_ksl_it_end(&it);
       ngtcp2_ksl_it_next(&it)) {
    scid = ngtcp2_ksl_it_get(&it);
    *dest++ = scid->cid;
  }

  return ngtcp2_ksl_len(&conn->scid.set);
}

void ngtcp2_conn_set_local_addr(ngtcp2_conn *conn, const ngtcp2_addr *addr) {
  ngtcp2_addr *dest = &conn->dcid.current.ps.path.local;

  assert(addr->addrlen <= sizeof(conn->dcid.current.ps.local_addrbuf));
  ngtcp2_addr_copy(dest, addr);
}

void ngtcp2_conn_set_remote_addr(ngtcp2_conn *conn, const ngtcp2_addr *addr) {
  ngtcp2_addr *dest = &conn->dcid.current.ps.path.remote;

  assert(addr->addrlen <= sizeof(conn->dcid.current.ps.remote_addrbuf));
  ngtcp2_addr_copy(dest, addr);
}

const ngtcp2_addr *ngtcp2_conn_get_remote_addr(ngtcp2_conn *conn) {
  return &conn->dcid.current.ps.path.remote;
}

int ngtcp2_conn_initiate_migration(ngtcp2_conn *conn, const ngtcp2_path *path,
                                   ngtcp2_tstamp ts) {
  int rv;
  ngtcp2_dcid *dcid;

  assert(!conn->server);

  conn->log.last_ts = ts;

  if (conn->remote.settings.disable_migration) {
    return NGTCP2_ERR_INVALID_STATE;
  }
  if (ngtcp2_ringbuf_len(&conn->dcid.unused) == 0) {
    return NGTCP2_ERR_CONN_ID_BLOCKED;
  }

  if (ngtcp2_path_eq(&conn->dcid.current.ps.path, path)) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  dcid = ngtcp2_ringbuf_get(&conn->dcid.unused, 0);

  rv = conn_stop_pv(conn, ts);
  if (rv != 0) {
    return rv;
  }

  rv = conn_retire_dcid(conn, &conn->dcid.current, ts);
  if (rv != 0) {
    return rv;
  }

  ngtcp2_dcid_copy(&conn->dcid.current, dcid);
  ngtcp2_path_copy(&conn->dcid.current.ps.path, path);
  ngtcp2_ringbuf_pop_front(&conn->dcid.unused);

  conn_reset_congestion_state(conn);

  return 0;
}

uint64_t ngtcp2_conn_get_max_local_streams_uni(ngtcp2_conn *conn) {
  return conn->local.uni.max_streams;
}

uint64_t ngtcp2_conn_get_max_data_left(ngtcp2_conn *conn) {
  return conn->tx.max_offset - conn->tx.offset;
}

ngtcp2_duration ngtcp2_conn_get_idle_timeout(ngtcp2_conn *conn) {
  ngtcp2_duration trpto;

  if (conn->local.settings.idle_timeout == 0) {
    return UINT64_MAX;
  }

  trpto = 3 * conn_compute_pto(conn);

  return ngtcp2_max(conn->local.settings.idle_timeout * NGTCP2_MILLISECONDS,
                    trpto);
}

ngtcp2_duration ngtcp2_conn_get_pto(ngtcp2_conn *conn) {
  return conn_compute_pto(conn);
}

void ngtcp2_path_challenge_entry_init(ngtcp2_path_challenge_entry *pcent,
                                      const ngtcp2_path *path,
                                      const uint8_t *data) {
  ngtcp2_path_storage_init2(&pcent->ps, path);
  memcpy(pcent->data, data, sizeof(pcent->data));
}

void ngtcp2_settings_default(ngtcp2_settings *settings) {
  memset(settings, 0, sizeof(*settings));
  settings->max_packet_size = NGTCP2_MAX_PKT_SIZE;
  settings->ack_delay_exponent = NGTCP2_DEFAULT_ACK_DELAY_EXPONENT;
  settings->max_ack_delay = NGTCP2_DEFAULT_MAX_ACK_DELAY;
}

#ifndef ERPC_RPC_H
#define ERPC_RPC_H

#include <algorithm>
#include <random>
#include <vector>
#include "common.h"
#include "msg_buffer.h"
#include "nexus.h"
#include "ops.h"
#include "pkthdr.h"
#include "session.h"
#include "transport.h"
#include "transport_impl/ib_transport.h"
#include "util/buffer.h"
#include "util/huge_alloc.h"
#include "util/mt_list.h"
#include "util/rand.h"
#include "util/tsc.h"

namespace ERpc {

/**
 * @brief Rpc object created by foreground threads, and possibly shared with
 * background threads.
 *
 * Non-const functions that are not thread-safe should be marked in the
 * documentation.
 *
 * @tparam TTr The unreliable transport
 */
template <class TTr>
class Rpc {
 public:
  /// Max request or response *data* size, i.e., excluding packet headers
  static constexpr size_t kMaxMsgSize =
      HugeAlloc::kMaxClassSize -
      ((HugeAlloc::kMaxClassSize / TTr::kMaxDataPerPkt) * sizeof(pkthdr_t));
  static_assert((1ull << kMsgSizeBits) >= kMaxMsgSize, "");
  static_assert((1ull << kPktNumBits) * TTr::kMaxDataPerPkt >= kMaxMsgSize, "");

  /// Duration of a packet loss detection epoch in milliseconds
  static constexpr size_t kPktLossEpochMs = 50;

  /// Packet loss timeout for an RPC request in milliseconds
  static constexpr size_t kPktLossTimeoutMs = 500;

  /// Initial capacity of the hugepage allocator
  static constexpr size_t kInitialHugeAllocSize = (128 * MB(1));

  //
  // Constructor/destructor (rpc.cc)
  //

  /**
   * @brief Construct the Rpc object from a foreground thread
   * @throw runtime_error if construction fails
   */
  Rpc(Nexus<TTr> *nexus, void *context, uint8_t rpc_id, sm_handler_t sm_handler,
      uint8_t phy_port = 0, size_t numa_node = 0);

  /// Destroy the Rpc from a foreground thread
  ~Rpc();

  //
  // MsgBuffer management
  //

  /**
   * @brief Create a hugepage-backed MsgBuffer for the eRPC user.
   *
   * The returned MsgBuffer's \p buf is surrounded by packet headers that the
   * user must not modify. This function does not fill in these message headers,
   * though it sets the magic field in the zeroth header.
   *
   * @param max_data_size Maximum non-header bytes in the returned MsgBuffer
   *
   * @return \p The allocated MsgBuffer. The MsgBuffer is invalid (i.e., its
   * \p buf is NULL) if we ran out of memory.
   *
   * @throw runtime_error if \p size is too large for the allocator, or if
   * hugepage reservation failure is catastrophic. An exception is *not* thrown
   * if allocation fails simply because we ran out of memory.
   */
  inline MsgBuffer alloc_msg_buffer(size_t max_data_size) {
    size_t max_num_pkts;

    // Avoid division for small packets
    if (small_rpc_likely(max_data_size <= TTr::kMaxDataPerPkt)) {
      max_num_pkts = 1;
    } else {
      max_num_pkts =
          (max_data_size + (TTr::kMaxDataPerPkt - 1)) / TTr::kMaxDataPerPkt;
    }

    lock_cond(&huge_alloc_lock);
    Buffer buffer =
        huge_alloc->alloc(max_data_size + (max_num_pkts * sizeof(pkthdr_t)));
    unlock_cond(&huge_alloc_lock);

    if (unlikely(buffer.buf == nullptr)) {
      MsgBuffer msg_buffer;
      msg_buffer.buf = nullptr;
      return msg_buffer;
    }

    MsgBuffer msg_buffer(buffer, max_data_size, max_num_pkts);
    return msg_buffer;
  }

  /// Resize a MsgBuffer to a smaller size than its max allocation.
  /// This does not modify the MsgBuffer's packet headers.
  static inline void resize_msg_buffer(MsgBuffer *msg_buffer,
                                       size_t new_data_size) {
    assert(msg_buffer != nullptr);
    assert(msg_buffer->buf != nullptr && msg_buffer->check_magic());
    assert(new_data_size <= msg_buffer->max_data_size);

    size_t new_num_pkts;

    // Avoid division for small packets
    if (small_rpc_likely(new_data_size <= TTr::kMaxDataPerPkt)) {
      new_num_pkts = 1;
    } else {
      new_num_pkts =
          (new_data_size + (TTr::kMaxDataPerPkt - 1)) / TTr::kMaxDataPerPkt;
    }

    msg_buffer->resize(new_data_size, new_num_pkts);
  }

  /// Free a MsgBuffer created by \p alloc_msg_buffer()
  inline void free_msg_buffer(MsgBuffer msg_buffer) {
    assert(msg_buffer.is_dynamic() && msg_buffer.check_magic());

    lock_cond(&huge_alloc_lock);
    huge_alloc->free_buf(msg_buffer.buffer);
    unlock_cond(&huge_alloc_lock);
  }

  /// Return the total amount of memory allocated to the user
  inline size_t get_stat_user_alloc_tot() {
    lock_cond(&huge_alloc_lock);
    size_t ret = huge_alloc->get_stat_user_alloc_tot();
    unlock_cond(&huge_alloc_lock);
    return ret;
  }

  /**
   * @brief Free the session slot's TX MsgBuffer if it is dynamic, and NULL-ify
   * it in any case. This does not fully validate the MsgBuffer, since we don't
   * want to conditionally bury only initialized sslots.
   *
   * This is thread-safe, as \p free_msg_buffer() is thread-safe.
   */
  inline void bury_sslot_tx_msgbuf(SSlot *sslot) {
    assert(sslot != nullptr);

    // The TX MsgBuffer used dynamic allocation if its buffer.buf is non-NULL.
    // Its buf can be non-NULL even when dynamic allocation is not used.
    MsgBuffer *tx_msgbuf = sslot->tx_msgbuf;
    if (small_rpc_unlikely(tx_msgbuf != nullptr && tx_msgbuf->is_dynamic())) {
      // This check is OK, as dynamic sslots must be initialized
      assert(tx_msgbuf->buf != nullptr && tx_msgbuf->check_magic());
      free_msg_buffer(*tx_msgbuf);
      // Need not nullify tx_msgbuf->buffer.buf: we'll just nullify tx_msgbuf
    }

    sslot->tx_msgbuf = nullptr;
  }

  /**
   * @brief Bury a session slot's TX MsgBuffer without freeing possibly
   * dynamically allocated memory.
   *
   * This is used for burying the TX MsgBuffer used for holding requests at
   * the client.
   */
  static inline void bury_sslot_tx_msgbuf_nofree(SSlot *sslot) {
    assert(sslot != nullptr);
    sslot->tx_msgbuf = nullptr;
  }

  /**
   * @brief Free the session slot's RX MsgBuffer if it is dynamic, and NULL-ify
   * it in any case. This does not fully validate the MsgBuffer, since we don't
   * want to conditinally bury only initialized sslots.
   *
   * This is thread-safe, as \p free_msg_buffer() is thread-safe.
   */
  inline void bury_sslot_rx_msgbuf(SSlot *sslot) {
    assert(sslot != nullptr);

    // The RX MsgBuffer used dynamic allocation if its buffer.buf is non-NULL.
    // Its buf can be non-NULL even when dynamic allocation is not used.
    MsgBuffer &rx_msgbuf = sslot->rx_msgbuf;
    if (small_rpc_unlikely(rx_msgbuf.is_dynamic())) {
      // This check is OK, as dynamic sslots must be initialized
      assert(rx_msgbuf.buf != nullptr && rx_msgbuf.check_magic());
      free_msg_buffer(rx_msgbuf);
      rx_msgbuf.buffer.buf = nullptr;  // Mark invalid for future
    }

    rx_msgbuf.buf = nullptr;
  }

  /**
   * @brief Bury a session slot's RX MsgBuffer without freeing possibly
   * dynamically allocated memory. This is used for burying fake RX MsgBuffers.
   */
  static inline void bury_sslot_rx_msgbuf_nofree(SSlot *sslot) {
    assert(sslot != nullptr);
    assert(!sslot->rx_msgbuf.is_dynamic());  // It's fake
    sslot->rx_msgbuf.buf = nullptr;
  }

  //
  // Session management API (rpc_sm_api.cc)
  //

 public:
  /**
   * @brief Create a session and initiate session connection. This function
   * can only be called from the creator thread.
   *
   * @return The local session number (>= 0) of the session if creation succeeds
   * and the connect request is sent, negative errno otherwise.
   *
   * A callback of type \p kConnected or \p kConnectFailed will be invoked if
   * this call is successful.
   */
  int create_session(const char *_rem_hostname, uint8_t rem_rpc_id,
                     uint8_t rem_phy_port = 0) {
    return create_session_st(_rem_hostname, rem_rpc_id, rem_phy_port);
  }

  /**
   * @brief Disconnect and destroy a client session. The session should not
   * be used by the application after this function is called. This functio
   * can only be called from the creator thread.
   *
   * @param session_num A session number returned from a successful
   * create_session()
   *
   * @return 0 if (a) the session disconnect packet was sent, and the disconnect
   * callback will be invoked later. Negative errno if the session cannot be
   * disconnected.
   */
  int destroy_session(int session_num) {
    return destroy_session_st(session_num);
  }

  /// Return the number of active server or client sessions. This function
  /// can be called only from the creator thread.
  size_t num_active_sessions() { return num_active_sessions_st(); }

 private:
  int create_session_st(const char *_rem_hostname, uint8_t rem_rpc_id,
                        uint8_t rem_phy_port = 0);
  int destroy_session_st(int session_num);
  size_t num_active_sessions_st();

  //
  // Session management helper functions
  //
 private:
  // rpc_sm_helpers.cc

  /// Process all session management packets in the hook's RX list and free
  /// them. The handlers for individual request/response types should not free
  /// packets.
  void handle_sm_st();

  /// Free a session's resources and mark it as NULL in the session vector.
  /// Only the MsgBuffers allocated by the Rpc layer are freed. The user is
  /// responsible for freeing user-allocated MsgBuffers.
  void bury_session_st(Session *session);

  /// Allocate a session management request packet and enqueue it to the
  /// session management thread. The allocated packet will be freed by the
  /// session management thread on transmission.
  void enqueue_sm_req_st(Session *session, SmPktType pkt_type,
                         size_t gen_data = 0);

  /// Allocate a response-copy of the session manegement packet and enqueue it
  /// to the session management thread. The allocated packet will be freed by
  /// the session management thread on transmission.
  void enqueue_sm_resp_st(typename Nexus<TTr>::SmWorkItem *req_wi,
                          SmErrType err_type);

  // rpc_connect_handlers.cc
  void handle_connect_req_st(typename Nexus<TTr>::SmWorkItem *wi);
  void handle_connect_resp_st(SmPkt *pkt);

  // rpc_disconnect_handlers.cc
  void handle_disconnect_req_st(typename Nexus<TTr>::SmWorkItem *wi);
  void handle_disconnect_resp_st(SmPkt *pkt);

  //
  // Rpc datapath (rpc_enqueue_request.cc and rpc_enqueue_response.cc)
  //

 public:
  /**
   * @brief Try to enqueue a request for transmission.
   *
   * If a session slot is available for this session, the request will be
   * enqueued. If this call succeeds, eRPC owns \p msg_buffer until the request
   * completes (i.e., the continuation is invoked).
   *
   * @param session_num The session number to send the request on
   * @param req_type The type of the request
   * @param msg_buffer The user-created MsgBuffer containing the request payload
   * but not packet headers
   * @param cont_func The continuation function for this request
   * @tag The tag for this request
   *
   * @return 0 on success (i.e., if the request was queued). Negative errno is
   * returned if the request was not queued.
   */
  int enqueue_request(int session_num, uint8_t req_type, MsgBuffer *msg_buffer,
                      erpc_cont_func_t cont_func, size_t tag);

  /// Enqueue a response for transmission at the server
  void enqueue_response(ReqHandle *req_handle);

  /// From a continuation, bury the response MsgBuffer and free up the sslot
  inline void release_respone(RespHandle *resp_handle) {
    assert(resp_handle != nullptr);
    SSlot *sslot = static_cast<SSlot *>(resp_handle);

    // The request MsgBuffer (tx_msgbuf) was buried before calling continuation
    assert(sslot->tx_msgbuf == nullptr);

    // Bury the response, which may be dynamic
    assert(sslot->rx_msgbuf.buf != nullptr && sslot->rx_msgbuf.check_magic());
    bury_sslot_rx_msgbuf(sslot);

    Session *session = sslot->session;
    assert(session != nullptr && session->is_client());
    lock_cond(&session->lock);
    session->sslot_free_vec.push_back(sslot->index);
    unlock_cond(&session->lock);
  }

  //
  // Event loop
  //

 public:
  /// Run one iteration of the event loop
  inline void run_event_loop_one() { run_event_loop_one_st(); }

  /// Run the event loop forever
  inline void run_event_loop() { run_event_loop_st(); }

  /// Run the event loop for \p timeout_ms milliseconds
  inline void run_event_loop_timeout(size_t timeout_ms) {
    run_event_loop_timeout_st(timeout_ms);
  }

 private:
  void run_event_loop_one_st();
  void run_event_loop_st();
  void run_event_loop_timeout_st(size_t timeout_ms);

  //
  // TX (rpc_tx.cc)
  //

 private:
  /// Enqueue a packet for transmission, possibly deferring transmission.
  /// This handles fault injection for dropping packets.
  inline void enqueue_pkt_tx_burst_st(Transport::RoutingInfo *routing_info,
                                      MsgBuffer *tx_msgbuf, size_t offset,
                                      size_t data_bytes) {
    assert(in_creator());
    assert(routing_info != nullptr && tx_msgbuf != nullptr);
    assert(tx_batch_i < TTr::kPostlist);

    Transport::tx_burst_item_t &item = tx_burst_arr[tx_batch_i];
    item.routing_info = routing_info;
    item.msg_buffer = tx_msgbuf;
    item.offset = offset;
    item.data_bytes = data_bytes;

    if (kFaultInjection) {
      // Fault injection is enabled, so we need to set item.drop
      if (faults.drop_tx_local && faults.drop_tx_local_countdown == 0) {
        erpc_dprintf(
            "eRPC Rpc %u: Dropping packet %s.\n", rpc_id,
            tx_msgbuf->get_pkthdr_str(offset / TTr::kMaxDataPerPkt).c_str());
        item.drop = true;
        faults.drop_tx_local = false;
      } else if (faults.drop_tx_local) {
        assert(faults.drop_tx_local_countdown > 0);
        faults.drop_tx_local_countdown--;
        item.drop = false;
      } else {
        item.drop = false;
      }
    } else {
      assert(!item.drop);
    }

    tx_msgbuf->pkts_queued++;  // Update queueing progress
    tx_batch_i++;

    dpath_dprintf(
        "eRPC Rpc %u: Sending packet %s.\n", rpc_id,
        tx_msgbuf->get_pkthdr_str(offset / TTr::kMaxDataPerPkt).c_str());

    if (tx_batch_i == TTr::kPostlist) {
      transport->tx_burst(tx_burst_arr, TTr::kPostlist);
      tx_batch_i = 0;
    }
  }

  /// Transmit a header-only packet right now
  inline void tx_burst_now_st(Transport::RoutingInfo *routing_info,
                              MsgBuffer *tx_msgbuf) {
    assert(in_creator());
    assert(routing_info != nullptr && tx_msgbuf != nullptr);
    assert(tx_batch_i < TTr::kPostlist);
    assert(tx_msgbuf->is_expl_cr() || tx_msgbuf->is_req_for_resp());

    Transport::tx_burst_item_t &item = tx_burst_arr[tx_batch_i];
    item.routing_info = routing_info;
    item.msg_buffer = tx_msgbuf;
    item.offset = 0;
    item.data_bytes = 0;

    // This is a fake MsgBuffer, so no need to update queueing progress
    tx_batch_i++;

    dpath_dprintf("eRPC Rpc %u: Sending packet %s.\n", rpc_id,
                  tx_msgbuf->get_pkthdr_str().c_str());

    transport->tx_burst(tx_burst_arr, tx_batch_i);
    tx_batch_i = 0;
  }

  /// Try to transmit packets for sslots in the datapath TX queue. SSlots for
  /// which all packets can be sent are removed from the queue.
  void process_req_txq_st();

  /**
   * @brief Try to transmit a single-packet request
   *
   * @param sslot The session slot to send the request for
   * @param req_msgbuf A valid single-packet request MsgBuffer that still needs
   * the packet to be queued
   */
  void process_req_txq_small_one_st(SSlot *sslot, MsgBuffer *req_msgbuf);

  /**
   * @brief Try to transmit a multi-packet request
   *
   * @param sslot The session slot to send the request for
   * @param req_msgbuf A valid multi-packet request MsgBuffer that still needs
   * one or more packets to be queued
   */
  void process_req_txq_large_one_st(SSlot *sslot, MsgBuffer *req_msgbuf);

  void process_bg_resp_txq_st();

  //
  // rpc_rx.cc
  //

 public:
  /// Sanity-check a slot's request MsgBuffer on receiving a response packet.
  /// It should be valid, dynamic, and the request number/type should match.
  static void debug_check_req_msgbuf_on_resp(SSlot *sslot, uint64_t req_num,
                                             uint8_t req_type);

  /// Check an RX MsgBuffer submitted to a background thread. It should be
  /// valid, dynamic, and the \p is_req field should match. This holds for
  /// both background request handlers and continuations.
  static void debug_check_bg_rx_msgbuf(
      SSlot *sslot, typename Nexus<TTr>::BgWorkItemType wi_type);

  inline void bump_credits(Session *session) {
    assert(session->is_client());
    assert(session->credits < Session::kSessionCredits);
    session->credits++;
  }

 private:
  /**
   * @brief Process received packets and post RECVs. The ring buffers received
   * from `rx_burst` must not be used after new RECVs are posted.
   *
   * Although none of the polled RX ring buffers can be overwritten by the
   * NIC until we send at least one response/CR packet back, we do not control
   * the order or time at which these packets are sent, due to constraints like
   * session credits and packet pacing.
   */
  void process_comps_st();

  /**
   * @brief Process a request or response packet received for a small message.
   * This packet cannot be a credit return.
   *
   * @param sslot The session slot used for the request or response packet
   * @param pkt The received packet. The zeroth byte of this packet is the
   * eRPC packet header.
   */
  void process_comps_small_msg_one_st(SSlot *sslot, const uint8_t *pkt);

  /**
   * @brief Process a request or response packet received for a large message.
   * This packet cannot be a credit return.
   *
   * @param sslot The session slot for the request or response packet
   * @param pkt The received packet. The zeroth byte of this packet is the
   * eRPC packet header.
   */
  void process_comps_large_msg_one_st(SSlot *sslot, const uint8_t *pkt);

  /// Submit a work item to a background thread
  void submit_background_st(SSlot *sslot,
                            typename Nexus<TTr>::BgWorkItemType wi_type);

  //
  // Fault injection
  //
 public:
  /**
   * @brief Inject a fault that always fails server routing info resolution at
   * all client sessions of this Rpc
   *
   * @throw runtime_error if the caller cannot inject faults
   */
  void fault_inject_resolve_server_rinfo_st();

  /**
   * @brief Inject a fault that forcefully resets the remote ENet peer for
   * a client session, emulating failure of the server. An ENet disconnect event
   * will be generated locally when ENet detects the remote peer failure.
   *
   * This will also affect sessions in other Rpc objects on this machine that
   * are connected to the same host as the session for \p session_num.
   *
   * @throw runtime_error if the caller cannot inject faults
   */
  void fault_inject_reset_remote_epeer_st(int session_num);

  /**
   * @brief Inject a fault that drops a packet transmitted by this Rpc. We do
   * not control which session the drop will affect.
   *
   * @param pkt_countdown Packets to transmit locally before dropping one
   * @throw runtime_error if the caller cannot inject faults
   */
  void fault_inject_drop_tx_local_st(size_t pkt_countdown);

  /**
   * @brief Inject a fault that drops a packet transmitted by the server
   * Rpc for this client session. This can affect any session of the server Rpc.
   *
   * @param session_num The client session to send the fault for
   * @param pkt_countdown Packets to transmit at the server before dropping one
   * @throw runtime_error if the caller cannot inject faults
   */
  void fault_inject_drop_tx_remote_st(int session_num, size_t pkt_countdown);

 private:
  /**
   * @brief Check if the caller can inject faults
   * @throw runtime_error if the caller cannot inject faults
   */
  void fault_inject_check_ok() const;

  //
  // Packet loss handling (rpc_pkt_loss.cc)
  //

  /// Scan all outstanding requests for suspected packet loss
  void pkt_loss_scan_reqs_st();

  //
  // Misc public functions
  //

 public:
  /// Return the maximum *data* size that can be sent in one packet
  static inline constexpr size_t get_max_data_per_pkt() {
    return TTr::kMaxDataPerPkt;
  }

  /// Return the maximum message *data* size that can be sent
  static inline size_t get_max_msg_size() { return kMaxMsgSize; }

  /// Return the ID of this Rpc object
  inline uint8_t get_rpc_id() const { return rpc_id; }

  /// Return true iff the caller is running in a background thread
  inline bool in_background() const { return !in_creator(); }

  /// Return the tiny thread ID of the caller
  inline size_t get_tiny_tid() const { return tls_registry->get_tiny_tid(); }

  //
  // Misc private functions
  //

 private:
  /// Return true iff we're currently running in this Rpc's creator.
  inline bool in_creator() const { return get_tiny_tid() == creator_tiny_tid; }

  /// Return true iff a user-provide session number is in the session vector
  inline bool is_usr_session_num_in_range(int session_num) const {
    return session_num >= 0 &&
           static_cast<size_t>(session_num) < session_vec.size();
  }

  /// Lock the mutex if the Rpc is accessible from multiple threads
  inline void lock_cond(std::mutex *mutex) {
    if (small_rpc_unlikely(multi_threaded)) {
      mutex->lock();
    }
  }

  /// Unlock the mutex if the Rpc is accessible from multiple threads
  inline void unlock_cond(std::mutex *mutex) {
    if (small_rpc_unlikely(multi_threaded)) {
      mutex->unlock();
    }
  }

  // rpc_send_cr.cc

  /**
   * @brief Send a credit return immediately (i.e., no burst queueing)
   *
   * @param session The session to send the credit return on
   * @param req_pkthdr The packet header of the request packet that triggered
   * this credit return
   */
  void send_credit_return_now_st(Session *session, const pkthdr_t *req_pkthdr);

  // rpc_send_rfr.cc

  /**
   * @brief Send a request-for-response immediately (i.e. no burst queueing)
   *
   * @param sslot The session slot to send the request-for-response for
   * @param req_pkthdr The packet header of the response packet that triggered
   * this request-for-response
   */
  void send_req_for_resp_now_st(SSlot *sslot, const pkthdr_t *resp_pkthdr);

 private:
  // Constructor args
  Nexus<TTr> *nexus;
  void *context;  ///< The application context
  const uint8_t rpc_id;
  const sm_handler_t sm_handler;
  const uint8_t phy_port;  ///< Zero-based physical port specified by app
  const size_t numa_node;

  // Derived consts
  const bool multi_threaded;  ///< True iff there are background threads
  const size_t pkt_loss_epoch_cycles;  ///< Packet loss epoch in TSC cycles

  /// A copy of the request/response handlers from the Nexus. We could use
  /// a pointer instead, but an array is faster.
  const std::array<ReqFunc, kMaxReqTypes> req_func_arr;

  // Rpc metadata
  size_t creator_tiny_tid;  ///< Tiny thread ID of the creator thread
  typename Nexus<TTr>::Hook nexus_hook;  ///< A hook shared with the Nexus
  TlsRegistry *tls_registry;  ///< Pointer to the Nexus's thread-local registry

  // Sessions

  /// The append-only list of session pointers, indexed by session number.
  /// Disconnected sessions are denoted by null pointers. This grows as sessions
  /// are repeatedly connected and disconnected, but 8 bytes per session is OK.
  std::vector<Session *> session_vec;
  /// The next request number prefix for each session request window slot
  size_t req_num_arr[Session::kSessionReqWindow] = {0};
  std::mutex req_num_arr_lock;

  // Transport
  TTr *transport = nullptr;  ///< The unreliable transport
  Transport::tx_burst_item_t tx_burst_arr[TTr::kPostlist];  ///< Tx baych info
  size_t tx_batch_i = 0;  ///< The batch index for \p tx_burst_arr
  MsgBuffer rx_msg_buffer_arr[TTr::kPostlist];  /// Batch info for \p rx_burst
  uint8_t *rx_ring[TTr::kRecvQueueDepth];       ///< The transport's RX ring
  size_t rx_ring_head = 0;  ///< Current unused RX ring buffer

  // Allocator
  HugeAlloc *huge_alloc = nullptr;  ///< This thread's hugepage allocator
  std::mutex huge_alloc_lock;       ///< A lock to guard the huge allocator

  // Request and response queues. We don't have a response TX queue because
  // response TX is driven by request-for-response packets from clients.
  std::vector<SSlot *> req_txq;  ///< Request sslots that need TX
  std::mutex req_txq_lock;       ///< Conditional lock for the request TX queue

  std::vector<SSlot *> bg_resp_txq;  ///< Responses from background req handlers
  std::mutex bg_resp_txq_lock;  ///< Unconditional lock for bg response TX queue

  // Packet loss
  size_t prev_epoch_ts;  ///< Timestamp of the previous epoch

  // Misc
  bool in_event_loop;  ///< Track event loop reentrance (w/ kDatapathChecks)
  SlowRand slow_rand;  ///< A slow random generator for "real" randomness
  FastRand fast_rand;  ///< A fast random generator

  /// All the faults that can be injected into ERpc for testing
  struct {
    /// Fail server routing info resolution at client. This is used to test the
    /// case where a client fails to resolve routing info sent by the server.
    bool resolve_server_rinfo = false;

    bool drop_tx_local = false;      ///< Drop a local TX packet
    size_t drop_tx_local_countdown;  ///< Packets to TX before dropping one
  } faults;

  // Datapath stats
  struct {
    size_t ev_loop_calls = 0;
  } dpath_stats;
};

// Instantiate required Rpc classes so they get compiled for the linker
template class Rpc<IBTransport>;

}  // End ERpc

#endif  // ERPC_RPC_H

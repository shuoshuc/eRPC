/**
 * @file acs_server.cc
 *
 * @brief ACS capacity testing server.
 *
 */

#include "acs_server.h"
#include <signal.h>
#include <cstring>
#include "util/autorun_helpers.h"

static constexpr size_t kAppEvLoopMs = 1000;  // Duration of event loop in msec.
static constexpr bool kAppVerbose = false;

// Experiment control flags
static constexpr bool kAppClientMemsetReq = false;   // Fill entire request
static constexpr bool kAppServerMemsetResp = false;  // Fill entire response
static constexpr bool kAppClientCheckResp = false;   // Check entire response

void app_cont_func(void *, void *);  // Forward declaration

// Send a request using this MsgBuffer
void send_req(AppContext *c, size_t msgbuf_idx) {
  erpc::MsgBuffer &req_msgbuf = c->req_msgbuf[msgbuf_idx];
  assert(req_msgbuf.get_data_size() == FLAGS_req_size);

  if (kAppVerbose) {
    printf("large_rpc_tput: Thread %zu sending request using msgbuf_idx %zu.\n",
           c->thread_id_, msgbuf_idx);
  }

  c->req_ts[msgbuf_idx] = erpc::rdtsc();
  c->rpc_->enqueue_request(c->session_num_vec_[0], kAppReqType, &req_msgbuf,
                           &c->resp_msgbuf[msgbuf_idx], app_cont_func,
                           reinterpret_cast<void *>(msgbuf_idx));

  c->stat_tx_bytes_tot += FLAGS_req_size;
  c->stat_tx_query_tot += 1;
}

void req_handler(erpc::ReqHandle *req_handle, void *_context) {
  auto *c = static_cast<AppContext *>(_context);
  const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
  uint8_t resp_byte = req_msgbuf->buf_[0];

  // Use dynamic response
  erpc::MsgBuffer &resp_msgbuf = req_handle->dyn_resp_msgbuf_;
  resp_msgbuf = c->rpc_->alloc_msg_buffer_or_die(FLAGS_resp_size);

  // Touch the response
  if (kAppServerMemsetResp) {
    memset(resp_msgbuf.buf_, resp_byte, FLAGS_resp_size);
  } else {
    resp_msgbuf.buf_[0] = resp_byte;
  }

  c->stat_rx_bytes_tot += FLAGS_req_size;
  c->stat_rx_query_tot += 1;

  c->rpc_->enqueue_response(req_handle, &resp_msgbuf);

  c->stat_tx_bytes_tot += FLAGS_resp_size;
  c->stat_tx_query_tot += 1;
}

void app_cont_func(void *_context, void *_tag) {
  auto *c = static_cast<AppContext *>(_context);
  auto msgbuf_idx = reinterpret_cast<size_t>(_tag);

  const erpc::MsgBuffer &resp_msgbuf = c->resp_msgbuf[msgbuf_idx];
  if (kAppVerbose) {
    printf("large_rpc_tput: Received response for msgbuf %zu.\n", msgbuf_idx);
  }

  // Measure latency. 1 us granularity is sufficient for large RPC latency.
  double usec = erpc::to_usec(erpc::rdtsc() - c->req_ts[msgbuf_idx],
                              c->rpc_->get_freq_ghz());
  c->lat_vec.push_back(usec);

  // Check the response
  erpc::rt_assert(resp_msgbuf.get_data_size() == FLAGS_resp_size,
                  "Invalid response size");

  if (kAppClientCheckResp) {
    bool match = true;
    // Check all response cachelines (checking every byte is slow)
    for (size_t i = 0; i < FLAGS_resp_size; i += 64) {
      if (resp_msgbuf.buf_[i] != kAppDataByte) match = false;
    }
    erpc::rt_assert(match, "Invalid resp data");
  } else {
    erpc::rt_assert(resp_msgbuf.buf_[0] == kAppDataByte, "Invalid resp data");
  }

  c->stat_rx_bytes_tot += FLAGS_resp_size;

  // Create a new request clocking this response, and put in request queue
  if (kAppClientMemsetReq) {
    memset(c->req_msgbuf[msgbuf_idx].buf_, kAppDataByte, FLAGS_req_size);
  } else {
    c->req_msgbuf[msgbuf_idx].buf_[0] = kAppDataByte;
  }

  send_req(c, msgbuf_idx);
}

// The function executed by each thread in the cluster
void thread_func(size_t thread_id, app_stats_t *app_stats, erpc::Nexus *nexus) {
  AppContext c;
  c.thread_id_ = thread_id;
  c.app_stats = app_stats;
  if (thread_id == 0)
    c.tmp_stat_ = new TmpStat(app_stats_t::get_template_str());

  std::vector<size_t> port_vec = flags_get_numa_ports(FLAGS_numa_node);
  erpc::rt_assert(port_vec.size() > 0);
  uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

  erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(&c),
                                  static_cast<uint8_t>(thread_id),
                                  basic_sm_handler, phy_port);
  rpc.retry_connect_on_invalid_rpc_id_ = true;

  c.rpc_ = &rpc;

  if (c.session_num_vec_.size() > 0) {
    printf("large_rpc_tput: Thread %zu: All sessions connected.\n", thread_id);
  } else {
    printf("large_rpc_tput: Thread %zu: No sessions created.\n", thread_id);
  }

  // All threads allocate MsgBuffers, but they may not send requests
  alloc_req_resp_msg_buffers(&c);

  size_t console_ref_tsc = erpc::rdtsc();

  c.tput_t0.reset();
  for (size_t i = 0; i < FLAGS_test_ms; i += kAppEvLoopMs) {
    rpc.run_event_loop(kAppEvLoopMs);
    if (unlikely(ctrl_c_pressed == 1)) break;
  }

  erpc::TimingWheel *wheel = rpc.get_wheel();
  if (wheel != nullptr && !wheel->record_vec_.empty()) {
    const size_t num_to_print = 200;
    const size_t tot_entries = wheel->record_vec_.size();
    const size_t base_entry = tot_entries * .9;

    printf("Printing up to 200 entries toward the end of wheel record\n");
    size_t num_printed = 0;

    for (size_t i = base_entry; i < tot_entries; i++) {
      auto &rec = wheel->record_vec_.at(i);
      printf("wheel: %s\n",
             rec.to_string(console_ref_tsc, rpc.get_freq_ghz()).c_str());

      if (num_printed++ == num_to_print) break;
    }
  }

  // We don't disconnect sessions
}

int main(int argc, char **argv) {
  signal(SIGINT, ctrl_c_handler);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  erpc::Nexus nexus(FLAGS_erpc_local_uri, FLAGS_numa_node, 0);
  nexus.register_req_func(kAppReqType, req_handler);

  size_t num_threads = FLAGS_num_server_threads;
  std::vector<std::thread> threads(num_threads);
  auto *app_stats = new app_stats_t[num_threads];

  for (size_t i = 0; i < num_threads; i++) {
    threads[i] = std::thread(thread_func, i, app_stats, &nexus);
    erpc::bind_to_core(threads[i], FLAGS_numa_node, i);
  }

  for (auto &thread : threads) thread.join();
  delete[] app_stats;
}

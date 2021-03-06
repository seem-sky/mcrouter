/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "ProxyDestination.h"

#include <folly/Conv.h>
#include <folly/Memory.h>

#include "mcrouter/config-impl.h"
#include "mcrouter/config.h"
#include "mcrouter/lib/fbi/asox_timer.h"
#include "mcrouter/lib/fbi/nstring.h"
#include "mcrouter/lib/fbi/timer.h"
#include "mcrouter/lib/fbi/util.h"
#include "mcrouter/lib/fibers/Fiber.h"
#include "mcrouter/lib/network/AsyncMcClient.h"
#include "mcrouter/lib/network/ThreadLocalSSLContextProvider.h"
#include "mcrouter/pclient.h"
#include "mcrouter/ProxyClientCommon.h"
#include "mcrouter/routes/DestinationRoute.h"
#include "mcrouter/stats.h"
#include "mcrouter/TkoTracker.h"

namespace facebook { namespace memcache { namespace mcrouter {

namespace {

constexpr double kProbeExponentialFactor = 1.5;
constexpr double kProbeJitterMin = 0.05;
constexpr double kProbeJitterMax = 0.5;
constexpr double kProbeJitterDelta = kProbeJitterMax - kProbeJitterMin;

static_assert(kProbeJitterMax >= kProbeJitterMin,
              "ProbeJitterMax should be greater or equal tham ProbeJitterMin");

void on_probe_timer(const asox_timer_t timer, void* arg) {
  ProxyDestination* pdstn = (ProxyDestination*)arg;
  pdstn->on_timer(timer);
}

stat_name_t getStatName(ProxyDestinationState st) {
  switch (st) {
    case ProxyDestinationState::kNew:
      return num_servers_new_stat;
    case ProxyDestinationState::kUp:
      return num_servers_up_stat;
    case ProxyDestinationState::kClosed:
      return num_servers_closed_stat;
    case ProxyDestinationState::kDown:
      return num_servers_down_stat;
    default:
      CHECK(false);
      return num_stats; // shouldnt reach here
  }
}

}  // anonymous namespace

void ProxyDestination::schedule_next_probe() {
  FBI_ASSERT(proxy->magic == proxy_magic);
  FBI_ASSERT(!proxy->opts.disable_tko_tracking);

  int delay_ms = probe_delay_next_ms;
  if (probe_delay_next_ms < 2) {
    // int(1 * 1.5) == 1, so advance it to 2 first
    probe_delay_next_ms = 2;
  } else {
    probe_delay_next_ms *= kProbeExponentialFactor;
  }
  if (probe_delay_next_ms > proxy->opts.probe_delay_max_ms) {
    probe_delay_next_ms = proxy->opts.probe_delay_max_ms;
  }

  // Calculate random jitter
  double r = (double)rand() / (double)RAND_MAX;
  double tmo_jitter_pct = r * kProbeJitterDelta + kProbeJitterMin;
  uint64_t delay_us = (double)delay_ms * 1000 * (1.0 + tmo_jitter_pct);
  FBI_ASSERT(delay_us > 0);

  timeval_t delay;
  delay.tv_sec = (delay_us / 1000000);
  delay.tv_usec = (delay_us % 1000000);

  FBI_ASSERT(probe_timer == nullptr);
  probe_timer = asox_add_timer(proxy->eventBase->getLibeventBase(), delay,
                               on_probe_timer, this);
}

void ProxyDestination::on_timer(const asox_timer_t timer) {
  // This assert checks for use-after-free
  FBI_ASSERT(proxy->magic == proxy_magic);
  FBI_ASSERT(timer == probe_timer);
  asox_remove_timer(timer);
  probe_timer = nullptr;
  if (sending_probes) {
    // Note that the previous probe might still be in flight
    if (!probe_req) {
      auto mutReq = createMcMsgRef();
      mutReq->op = mc_op_version;
      probe_req = folly::make_unique<McRequest>(std::move(mutReq));
      ++probesSent_;
      auto selfPtr = selfPtr_;
      proxy->fiberManager.addTask([selfPtr]() mutable {
        auto pdstn = selfPtr.lock();
        if (pdstn == nullptr) {
          return;
        }
        pdstn->proxy->destinationMap->markAsActive(*pdstn);
        // will reconnect if connection was closed
        McReply reply = pdstn->getAsyncMcClient().sendSync(
          *pdstn->probe_req,
          McOperation<mc_op_version>(),
          pdstn->shortestTimeout);
        pdstn->handle_tko(reply, true);
        pdstn->probe_req.reset();
      });
    }
    schedule_next_probe();
  }
}

void ProxyDestination::start_sending_probes() {
  FBI_ASSERT(!sending_probes);
  sending_probes = true;
  probe_delay_next_ms = proxy->opts.probe_delay_initial_ms;
  schedule_next_probe();
}

void ProxyDestination::stop_sending_probes() {
  probesSent_ = 0;
  sending_probes = false;
  if (probe_timer) {
    asox_remove_timer(probe_timer);
    probe_timer = nullptr;
  }
}

void ProxyDestination::unmark_tko(const McReply& reply) {
  FBI_ASSERT(!proxy->opts.disable_tko_tracking);
  shared->tko.recordSuccess(this);
  if (sending_probes) {
    onTkoEvent(TkoLogEvent::UnMarkTko, reply.result());
    stop_sending_probes();
  }
}

void ProxyDestination::handle_tko(const McReply& reply, bool is_probe_req) {
  if (resetting || proxy->opts.disable_tko_tracking) {
    return;
  }

  bool responsible = false;
  if (reply.isError()) {
    if (reply.isHardTkoError()) {
      responsible = shared->tko.recordHardFailure(this);
      if (responsible) {
        onTkoEvent(TkoLogEvent::MarkHardTko, reply.result());
      }
    } else if (reply.isSoftTkoError()) {
      responsible = shared->tko.recordSoftFailure(this);
      if (responsible) {
        onTkoEvent(TkoLogEvent::MarkSoftTko, reply.result());
      }
    }
  } else if (!sending_probes || is_probe_req) {
    /* If we're sending probes, only a probe request should be considered
       successful to avoid outstanding requests from unmarking the box */
    unmark_tko(reply);
  }
  if (responsible) {
    start_sending_probes();
  }
}

void ProxyDestination::onReply(const McReply& reply,
                               DestinationRequestCtx& destreqCtx) {
  FBI_ASSERT(proxy->magic == proxy_magic);

  handle_tko(reply, false);

  stats_.results[reply.result()]++;
  destreqCtx.endTime = nowUs();

  int64_t latency = destreqCtx.endTime - destreqCtx.startTime;
  stats_.avgLatency.insertSample(latency);
}

void ProxyDestination::on_up() {
  FBI_ASSERT(proxy->magic == proxy_magic);
  FBI_ASSERT(stats_.state != ProxyDestinationState::kUp);

  setState(ProxyDestinationState::kUp);

  VLOG(1) << "server " << pdstnKey << " up (" <<
      stat_get_uint64(proxy->stats, num_servers_up_stat) << " of " <<
      stat_get_uint64(proxy->stats, num_servers_stat) << ")";
}

void ProxyDestination::on_down() {
  FBI_ASSERT(proxy->magic == proxy_magic);

  if (resetting) {
    VLOG(1) << "server " << pdstnKey << " inactive (" <<
        stat_get_uint64(proxy->stats, num_servers_up_stat) << " of " <<
        stat_get_uint64(proxy->stats, num_servers_stat) << ")";
    setState(ProxyDestinationState::kClosed);
  } else {
    VLOG(1) << "server " << pdstnKey << " down (" <<
        stat_get_uint64(proxy->stats, num_servers_up_stat) << " of " <<
        stat_get_uint64(proxy->stats, num_servers_stat) << ")";
    setState(ProxyDestinationState::kDown);
    handle_tko(McReply(mc_res_connect_error),
               /* is_probe_req= */ false);
  }
}

size_t ProxyDestination::getPendingRequestCount() const {
  return client_ ? client_->getPendingRequestCount() : 0;
}

size_t ProxyDestination::getInflightRequestCount() const {
  return client_ ? client_->getInflightRequestCount() : 0;
}

std::pair<uint64_t, uint64_t> ProxyDestination::getBatchingStat() const {
  return client_ ? client_->getBatchingStat() : std::make_pair(0UL, 0UL);
}

std::shared_ptr<ProxyDestination> ProxyDestination::create(
    proxy_t* proxy,
    const ProxyClientCommon& ro,
    std::string pdstnKey) {

  auto ptr = std::shared_ptr<ProxyDestination>(
    new ProxyDestination(proxy, ro, std::move(pdstnKey)));
  ptr->selfPtr_ = ptr;
  return ptr;
}

ProxyDestination::~ProxyDestination() {
  shared->removeDestination(this);
  if (proxy->destinationMap) {
    proxy->destinationMap->removeDestination(*this);
  }

  if (client_) {
    client_->setStatusCallbacks(nullptr, nullptr);
    client_->closeNow();
  }

  if (sending_probes) {
    onTkoEvent(TkoLogEvent::RemoveFromConfig, mc_res_ok);
    stop_sending_probes();
  }

  stat_decr(proxy->stats, getStatName(stats_.state), 1);
  magic = kDeadBeef;
}

ProxyDestination::ProxyDestination(proxy_t* proxy_,
                                   const ProxyClientCommon& ro_,
                                   std::string pdstnKey_)
  : proxy(proxy_),
    accessPoint(ro_.ap),
    destinationKey(ro_.destination_key),
    shortestTimeout(ro_.server_timeout),
    pdstnKey(std::move(pdstnKey_)),
    proxy_magic(proxy->magic),
    use_ssl(ro_.useSsl),
    qos(ro_.qos),
    stats_(proxy_->opts),
    poolName_(ro_.pool.getName()) {

  static uint64_t next_magic = 0x12345678900000LL;
  magic = __sync_fetch_and_add(&next_magic, 1);
  stat_incr(proxy->stats, num_servers_new_stat, 1);
}

ProxyDestinationState ProxyDestination::state() const {
  if (shared->tko.isTko()) {
    return ProxyDestinationState::kTko;
  }
  return stats_.state;
}

const ProxyDestinationStats& ProxyDestination::stats() const {
  return stats_;
}

bool ProxyDestination::may_send() {
  FBI_ASSERT(proxy->magic == proxy_magic);

  return !shared->tko.isTko();
}

void ProxyDestination::resetInactive() {
  FBI_ASSERT(proxy->magic == proxy_magic);

  // No need to reset non-existing client.
  if (client_) {
    resetting = 1;
    client_->closeNow();
    client_.reset();
    resetting = 0;
  }
}

void ProxyDestination::initializeAsyncMcClient() {
  CHECK(proxy->eventBase);
  assert(!client_);

  ConnectionOptions options(accessPoint);
  auto& opts = proxy->opts;
  options.noNetwork = opts.no_network;
  options.tcpKeepAliveCount = opts.keepalive_cnt;
  options.tcpKeepAliveIdle = opts.keepalive_idle_s;
  options.tcpKeepAliveInterval = opts.keepalive_interval_s;
  options.writeTimeout = shortestTimeout;
  if (proxy->opts.enable_qos) {
    options.enableQoS = true;
    options.qos = qos;
  }

  if (use_ssl) {
    checkLogic(!opts.pem_cert_path.empty() &&
               !opts.pem_key_path.empty() &&
               !opts.pem_ca_path.empty(),
               "Some of ssl key paths are not set!");
    options.sslContextProvider = [&opts] {
      return getSSLContext(opts.pem_cert_path, opts.pem_key_path,
                           opts.pem_ca_path);
    };
  }

  client_ = folly::make_unique<AsyncMcClient>(*proxy->eventBase,
                                              std::move(options));

  client_->setStatusCallbacks(
    [this] () mutable {
      on_up();
    },
    [this] (const folly::AsyncSocketException&) mutable {
      on_down();
    });

  if (opts.target_max_inflight_requests > 0) {
    client_->setThrottle(opts.target_max_inflight_requests,
                         opts.target_max_pending_requests);
  }
}

AsyncMcClient& ProxyDestination::getAsyncMcClient() {
  if (!client_) {
    initializeAsyncMcClient();
  }
  return *client_;
}

void ProxyDestination::onTkoEvent(TkoLogEvent event, mc_res_t result) const {
  auto logUtil = [this, result](folly::StringPiece eventStr) {
    VLOG(1) << shared->key << " (" << poolName_ << ") "
            << eventStr << ". Total hard TKOs: "
            << shared->tko.globalTkos().hardTkos << "; soft TKOs: "
            << shared->tko.globalTkos().softTkos << ". Reply: "
            << mc_res_to_string(result);
  };

  switch (event) {
    case TkoLogEvent::MarkHardTko:
      logUtil("marked hard TKO");
      break;
    case TkoLogEvent::MarkSoftTko:
      logUtil("marked soft TKO");
      break;
    case TkoLogEvent::UnMarkTko:
      logUtil("unmarked TKO");
      break;
    case TkoLogEvent::RemoveFromConfig:
      logUtil("was TKO, removed from config");
      break;
  }

  TkoLog tkoLog(accessPoint, shared->tko.globalTkos());
  tkoLog.event = event;
  tkoLog.isHardTko = shared->tko.isHardTko();
  tkoLog.isSoftTko = shared->tko.isSoftTko();
  tkoLog.avgLatency = stats_.avgLatency.value();
  tkoLog.probesSent = probesSent_;
  tkoLog.poolName = poolName_;
  tkoLog.result = result;

  logTkoEvent(proxy, tkoLog);
}

void ProxyDestination::setState(ProxyDestinationState new_st) {
  auto old_st = stats_.state;

  if (old_st != new_st) {
    auto old_name = getStatName(old_st);
    auto new_name = getStatName(new_st);
    stat_decr(proxy->stats, old_name, 1);
    stat_incr(proxy->stats, new_name, 1);
    stats_.state = new_st;
  }
}

ProxyDestinationStats::ProxyDestinationStats(const McrouterOptions& opts)
  : avgLatency(1.0 / opts.latency_window_size) {
}

void ProxyDestination::updateShortestTimeout(
    std::chrono::milliseconds timeout) {
  if (!timeout.count()) {
    return;
  }
  if (shortestTimeout.count() == 0 || shortestTimeout > timeout) {
    shortestTimeout = timeout;
    if (client_) {
      client_->updateWriteTimeout(shortestTimeout);
    }
  }
}

}}}  // facebook::memcache::mcrouter

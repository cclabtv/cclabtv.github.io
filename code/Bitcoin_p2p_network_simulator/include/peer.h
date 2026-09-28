/**
 * @file peer.h
 * @brief One node: the addresses it knows and how it exchanges them.
 *
 * A node owns two bucket tables, the set of open connections, and the state
 * kept per connection.
 *
 * Every hash used to place an address into a bucket is salted with the node's
 * own identifier, so two nodes given the same address place it differently.
 * That is what stops anyone choosing which slot an address lands in.
 *
 * @note No include guard. This header is included by core.h and by peer.cpp,
 *       and nothing includes both, so the omission is currently harmless.
 */

#include <algorithm>
#include <cstring>
#include <iostream>
#include <cmath>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <array>
#include <unordered_set>
#include "crypto.h"
#include "events.h"
#include "net.h"
#include "parser.h"
#include "rrandom.h"
#include "structures.h"
#include <serialization.hpp>

/** Tried buckets one AS can reach. Bounds how much of the tried table a single network owns. */
static constexpr uint8_t ADDRMAN_TRIED_BUCKETS_PER_GROUP{8};
/** Over how many buckets entries with new addresses originating from a single group are spread */
static constexpr uint8_t ADDRMAN_NEW_BUCKETS_PER_SOURCE_GROUP{64};

/** The soft limit of the address processing token bucket (the regular MAX_ADDR_RATE_PER_SECOND
 *  based increments won't go above this, but the MAX_ADDR_TO_SEND increment following GETADDR
 *  is exempt from this limit). */
static constexpr uint16_t MAX_ADDR_PROCESSING_TOKEN_BUCKET = 1000;
/** Token refill rate per second. */
static constexpr double MAX_ADDR_RATE_PER_SECOND{0.1};
/** Mean seconds between feeler cycles. The actual wait is exponential with this mean. */
static constexpr uint8_t FEELER_INTERVAL{120};
/** The maximum number of address records permitted in an ADDR message. */
static constexpr size_t MAX_ADDR_TO_SEND{1000};
/** Average delay between peer address broadcasts */
static constexpr auto AVG_ADDRESS_BROADCAST_INTERVAL{30};
/** Period over which the relay destination for a given address rotates. */
static constexpr auto ROTATE_ADDR_RELAY_DEST_INTERVAL{86400};
/** SipHash domain separator for relay-destination selection, taken from Core. */
static constexpr uint64_t RANDOMIZER_ID_ADDRESS_RELAY = 4371869568948661136UL;

/** Seed identities available. A node queries at most this many per session. */
static constexpr uint8_t NUM_DNS_SEEDS{8};
static constexpr uint32_t ADDRS_PER_SEED{32}; // Core: nMaxIPs = 32

/** Addresses per generation of RollingKnown. Capacity is three of these. */
static constexpr size_t GEN_SIZE = 2500;

/**
 * @brief A bounded set of addresses already known to one peer.
 *
 * Three generations of sorted vectors. Membership is a binary search across
 * all three; insertion goes into the current generation and rotates to the
 * next once it fills, discarding the oldest. Exact, so an address is never
 * wrongly reported as present.
 */
struct RollingKnown
{
  std::array<std::vector<uint32_t>, 3> gens; //!< Oldest is discarded when the newest fills.
  int current = 0;                           //!< Generation currently accepting inserts.

  /** @return True if @p x is in any generation. */
  bool contains(uint32_t x) const
  {
    for (auto &g : gens)
      if (std::binary_search(g.begin(), g.end(), x))
        return true;
    return false;
  }
  /** @brief Insert @p x, rotating and discarding the oldest generation when full. */
  void insert(uint32_t x)
  {
    if (contains(x))
      return;
    auto &g = gens[current];
    g.insert(std::lower_bound(g.begin(), g.end(), x), x);
    if (g.size() >= GEN_SIZE)
    {
      current = (current + 1) % 3;
      gens[current].clear();
    }
  }
  /** @brief Forget everything. Used before a self-announcement so it is not suppressed. */
  void clear()
  {
    for (auto &g : gens)
      g.clear();
    current = 0;
  }
};

/**
 * @brief Relay state for one open connection.
 *
 * One of these exists per entry in map_connection, created at handshake and
 * destroyed at teardown. Losing it is how stale messages get dropped.
 */
struct addInfo_C
{
  RollingKnown m_addr_known;            //!< Addresses this peer already knows, so we never echo one back.
  std::vector<Addr_msg> m_addr_to_send; //!< Queue awaiting the next flush, capped at MAX_ADDR_TO_SEND.
  bool m_getaddr_sent;                  //!< True while our own GETADDR is outstanding; suppresses relay of the reply.
  double m_addr_token_bucket;           //!< Remaining ADDR budget, refilled at MAX_ADDR_RATE_PER_SECOND.
  uint64_t m_addr_token_timestamp;      //!< Round of the last refill.
  uint64_t m_next_local_addr_send;      //!< Next self-announcement on this connection.
  /**
   * Next scheduled flush.
   *
   * @note Not read. Flushes are armed on demand by schedule_flush(), so this
   *       field is written and serialized but controls nothing.
   */
  uint64_t m_next_addr_send;
  bool is_inbound;                //!< True if the remote dialled us. GETADDR is answered only on inbound.
  bool m_flush_scheduled = false; //!< Ensures at most one flush is pending per connection.
};

/**
 * @brief What is known about one address.
 *
 * Held in map_info keyed by address. The bucket tables store addresses only,
 * so every occupied slot refers back to a record here.
 */
struct addInfo
{
  uint32_t ip;                   //!< The address this record describes.
  uint16_t asmap;                //!< Its AS, cached at creation.
  uint32_t source;               //!< Address that told us about it, used as the new-bucket source group.
  uint32_t first_seen;           //!< Round we first learned it. Measurement only, no behaviour depends on it.
  uint32_t n_time;               //!< Best known timestamp, refreshed by add() and connected().
  uint32_t m_last_success;       //!< Round of the last successful connection, 0 if never.
  uint32_t m_last_try;           //!< Round of the last dial attempt, successful or not.
  uint16_t n_attempts;           //!< Consecutive failures. Reset by good(), drives get_chance().
  uint8_t n_ref_count;           //!< New-table slots holding this address. At 0 the record is deleted.
  bool in_tried;                 //!< True once promoted. A tried entry has n_ref_count 0.
  uint32_t m_last_count_attempt; //!< Guards double counting a failure within one under-connected window.
  uint32_t n_random_pos{0};      //!< Index into v_randoms, kept current by swap_random() for O(1) deletion.
};

/**
 * @brief One node.
 *
 * Inherits Structures privately for the configured constants, such as the
 * bucket geometry and the thresholds is_terrible() applies.
 */
class Peer : Structures
{
private:
  /*Total number of peers in vvNew map*/
  uint64_t n_new;
  /*Total number of peers in vvTried map*/
  uint64_t n_tried;

  NetSet reachable_nets; // networks I can dial out to   (Core: g_reachable_nets)
  NetSet listening_nets; // networks I accept inbound on  (subset of reachable)
  RRandom rrandom;
  /*The birth time for the peer*/
  uint64_t n_time;
  /*The IP of the peer*/
  uint32_t ip;
  /*Unique identifier that persists across IP changes*/
  uint32_t unique_id;
  /*The AS that Peer belongd to it*/
  uint16_t asmap;
  uint32_t source;                        //!< Address that seeded this node, used as the source group for its DNS entries.
  Net *net;                               //!< Shared synthetic internet. Not owned.
  uint64_t out_of_connection_attempt = 0; //!< Round of the last VERACKE. Written and serialized, never read.
  /**
   * Round of the last good() call.
   *
   * attempt() counts a failure only when the address has not already been
   * counted since this time, so a burst of retries while under-connected does
   * not inflate n_attempts.
   */
  uint64_t m_last_good = 1;

  uint64_t m_last_seed_request; //!< Round of the last DNS query, for the 11s / 5min gate.
  uint8_t m_seeds_queried;      //!< Seeds used this session, capped at NUM_DNS_SEEDS.

  /*The current time, in seconds*/
  uint64_t *round;
  /**
   * Every address known, in permuted order.
   *
   * @warning Positions are not stable. get_addresses() permutes in place and
   *          _delete() swaps with the back, so nothing may index into this by
   *          position. addInfo::n_random_pos is the only valid handle.
   */
  std::vector<uint32_t> v_randoms;
  /*Map of IP addresses to their address information (source, timing, attempts,
   * and table status)*/
  std::unordered_map<uint32_t, addInfo> map_info;

  /*Bucket tables, flat like Core: index = bucket * BUCKET_SIZE + position.
   * 0 is the empty sentinel (add() rejects address 0). Flat storage costs
   * (buckets * BUCKET_SIZE * 4) bytes regardless of occupancy, but that is far
   * cheaper than a nested hash map once the tables fill.*/
  std::vector<uint32_t> vvNew;
  std::vector<uint32_t> vvTried;
  /*Occupied slots per bucket, plus the list of non-empty buckets and each
   * bucket's index within it. select() draws uniformly over NON-EMPTY buckets,
   * so that list has to be maintained in O(1) rather than scanned.*/
  std::vector<uint8_t> new_fill, tried_fill;
  std::vector<uint16_t> new_nonempty, tried_nonempty;
  std::vector<int32_t> new_nonempty_at, tried_nonempty_at;

  /*All the nodes we tried to move them from vvNew to vvTried, but the poisition
   * on vvTried was occupied*/
  std::set<uint32_t> tried_collisions;
  /*Outbound peers connections*/
  std::vector<uint32_t> outbound_connections;
  /*ASmap for the outbound peers connections*/
  std::unordered_map<uint16_t, bool> outbound_asmap;
  /*Inbound peers connections*/
  std::vector<uint32_t> inbound_connections;
  /**
   * Relay state per open connection, inbound and outbound alike.
   *
   * This map is authoritative: the message path looks a peer up here with
   * find() before acting, so a message that arrives after teardown is
   * dropped rather than resurrecting a ghost connection.
   */
  std::unordered_map<uint32_t, addInfo_C> map_connection;
  /*Vector for the peers that we will return them as a response for the call of
   * get_add()*/
  std::vector<Addr_msg> addrs_response_cache;

  /**
   * @note Length of the previous session, set at dormancy.
   */
  uint64_t previous_stay;

public:
  mutable std::mutex peer_mutex; //!< Held by the worker that owns this peer for the round.
  uint64_t next_feeler;          //!< Round of the next feeler cycle.
  /*The round that the peer should update its cache*/
  uint64_t cache_entry_expiration;
  uint32_t tel_feeler_success = 0; //!< Daily counter, drained by Core::collect_daily_telemetry().
  uint32_t tel_feeler_fail = 0;    //!< Daily counter.
  uint32_t tel_getaddr_served = 0; //!< Daily counter.
  uint32_t tel_addr_dropped = 0;   //!< Daily counter of addresses refused by the token bucket.

  /**
   * @brief Build a node and seed its address manager from DNS.
   *
   * The seed addresses are added with a 3 to 7 day timestamp penalty, then
   * get_addresses() primes the advertisement cache. m_seeds_queried starts at
   * 1 because this construction consumed one seed.
   *
   * @param _reachable_nets Networks this node can dial out to.
   * @param _listening_nets Networks it accepts inbound on. Empty accepts none.
   * @param _round          Pointer to the shared clock owned by Core.
   * @param _dns_seed       Initial addresses from a seeder.
   * @param _unique_id      Identity and hash salt, stable across IP changes.
   */
  Peer(NetSet _reachable_nets, NetSet _listening_nets, Network_Config &_nc, RRandom _rrandom, uint32_t _ip,
       uint16_t _asmap, uint32_t _source, Net *_net, uint64_t *_round,
       std::vector<Addr_msg> _dns_seed, uint32_t _unique_id);
  uint32_t get_unique_id() const;                                                 //!< @return Identity, stable across IP changes. Also the hash salt.
  uint64_t get_n_time() const;                                                    //!< @return Round this node joined, or last reactivated.
  uint16_t get_asmap() const;                                                     //!< @return Own AS.
  uint32_t get_ip() const;                                                        //!< @return Current address.
  Network my_net() const { return net_of(ip); }                                   //!< @return Network of the current address.
  bool can_reach(Network n) const { return net_has(reachable_nets, n); }          //!< @return Whether outbound dials to @p n are possible.
  bool accepts_inbound_on(Network n) const { return net_has(listening_nets, n); } //!< @return Whether inbound on @p n is accepted.
  bool is_listening() const { return listening_nets != 0; }                       //!< @return Whether inbound connections are accepted on any network.

  uint64_t get_map_info_size() const;                         //!< @return Total addresses known, new plus tried.
  std::unordered_map<uint32_t, addInfo> get_map_info() const; //!< @return A copy of every address record. Measurement only, and expensive.
  uint64_t get_n_new() const;                                 //!< @return Entries in the new table.
  uint64_t get_n_tried() const;                               //!< @return Entries in the tried table.

  /**
   * @brief New-table bucket for @p _ip learned from @p _source.
   *
   * Two-level hash, both levels salted with unique_id. The first picks a
   * selector within ADDRMAN_NEW_BUCKETS_PER_SOURCE_GROUP, the second maps
   * source group and selector onto a bucket, so one source group reaches at
   * most 64 of the buckets no matter how many addresses it supplies.
   */
  uint16_t get_new_bucket(uint32_t _ip, uint32_t _source) const;

  /**
   * @brief Slot within a bucket.
   * @param _is_tried Selects the table, and salts the hash so an address does
   *                  not land in the same position in both.
   */
  uint8_t get_position(uint32_t _ip, uint32_t _bucket, bool _is_tried) const;

  /**
   * @brief Tried-table bucket for @p _ip.
   *
   * Same two-level construction as get_new_bucket(), keyed on the address and
   * its own AS rather than on a source, bounding one AS to at most
   * ADDRMAN_TRIED_BUCKETS_PER_GROUP buckets.
   */
  uint8_t get_tried_bucket(uint32_t _ip) const;

  /** @brief Read one slot. @return The address, or -1 if the slot is empty. */
  int64_t get_entry(bool _use_tried, size_t _bucket, size_t _position);

  /*Write a slot and keep fill counts / non-empty bucket list in sync.
  ip == 0 clears the slot.*/
  void slot_set(bool _use_tried, size_t _bucket, size_t _position, uint32_t _ip);
  void slot_clear(bool _use_tried, size_t _bucket, size_t _position); //!< @brief Empty a slot. Equivalent to slot_set() with address 0.
  void init_tables();                                                 //!< @brief Allocate both tables and their occupancy indices. Called at construction and before a load.

  void set_ip(uint32_t new_ip);       //!< @brief Repoint to a new address after a rotation. The address manager is untouched.
  void set_asmap(uint16_t new_asmap); //!< @brief Update the cached own-AS after a rotation.
  void set_n_time(uint64_t _n_time);  //!< @brief Restart the session clock, on join or reactivation.
  void set_previous_stay();           //!< @brief Record the session just ended as round minus n_time.

  void clear_outbound_asmap();       //!< @brief Forget which ASes are occupied by outbound peers.
  void clear_outbound_connections(); //!< @brief Drop the outbound list without emitting teardown messages.
  void clear_inbound_connections();  //!< @brief Drop the inbound list without emitting teardown messages.
  void clear_map_connection();       //!< @brief Discard all per-connection relay state.

  /**
   * @brief Sample addresses for an ADDR response.
   *
   * Partial Fisher-Yates over v_randoms, so it draws only what it needs
   * rather than shuffling the whole set. The permutation is applied in place.
   *
   * @param max_addresses Hard cap, 0 for none.
   * @param max_pct       Percentage of the known set, 0 for none. Both caps apply.
   * @param _filtered     Skip addresses failing is_terrible().
   */
  std::vector<Addr_msg> get_addresses(size_t max_addresses, size_t max_pct,
                                      bool _filtered);

  /**
   * @brief Rebuild the advertisement cache and reschedule the next rebuild.
   *
   * Refills addrs_response_cache under a 1000 address and 23 percent cap,
   * filtered. Every request between rebuilds is answered from this snapshot.
   *
   * @return The next CACHE_ENTRY_EXPIRATION event, and the cache contents as
   *         a measurement row.
   */
  std::pair<RoundAction, cache> get_addresses();

  /**
   * @brief Probability weight used to reject stale candidates in select().
   * @return 1.0 for a fresh address, scaled by 0.01 if tried within ten
   *         minutes and by 0.66 per failed attempt up to eight.
   */
  double get_chance(uint32_t ip) const;

  /**
   * @brief Whether an address should be hidden and become evictable.
   * @return True if unknown, timestamped in the future, older than
   *         ADDRMAN_HORIZON, never connected after ADDRMAN_RETRIES tries, or
   *         failing ADDRMAN_MAX_FAILURES times recently. Always false within
   *         a minute of the last attempt.
   */
  bool is_terrible(uint32_t _ip) const;

  /**
   * @brief Promote an address into the tried table.
   *
   * Clears every new-table reference, then takes the tried slot. An incumbent
   * is demoted back into the new table rather than dropped. A slot pointing
   * at an already deleted record is reclaimed.
   */
  void make_tried(uint32_t _ip);

  /**
   * @brief Pick a pending collision to test.
   * @return The incumbent occupying the contested tried slot, the address a
   *         feeler should probe, or 0 if there is nothing to test.
   */
  uint32_t select_tried_collision();

  /**
   * @brief Retire pending collisions.
   *
   * A challenger wins and is promoted when the incumbent has not connected
   * recently, or when the collision has stood longer than
   * ADDRMAN_TEST_WINDOW. Called at the head of every process_message().
   */
  void resolve_collisions();

  /**
   * @brief Run one feeler cycle and chain the next.
   * @return A FEELER to a collision incumbent if one is pending, otherwise to
   *         a new-table address, plus the next RESOLVE_FEELER. The FEELER is
   *         omitted when select() finds nothing.
   */
  std::vector<RoundAction> resolve_feeler();

  /**
   * @brief Close every connection.
   * @param dormant True when the node is leaving, which also clears the
   *                connection maps and records the session length.
   * @return One teardown message per peer on the other side.
   */
  std::vector<RoundAction> disconnect(bool dormant);

  /** @brief Insert a fresh record into map_info and v_randoms. @return Pointer into map_info. */
  addInfo *create(uint32_t _ip, uint32_t _source, uint64_t t);

  /**
   * @brief Record a dial attempt.
   * @param f_count_failure Whether the attempt counts against the address.
   *                        Pass false while under-connected so retry bursts
   *                        do not inflate n_attempts.
   */
  void attempt(uint32_t _ip, bool f_count_failure);

  /** @brief Drop one new-table reference, deleting the record when the last one goes. */
  void clear_new(uint16_t bucket, uint8_t position);

  /** @brief Erase a record, removing it from v_randoms in O(1) via n_random_pos. */
  void _delete(addInfo *_addr_delete);

  /** @brief Swap two v_randoms entries, keeping both n_random_pos values correct. */
  void swap_random(size_t i, size_t j);

  /**
   * @brief Record one address.
   *
   * Refreshes the timestamp of a known address, then refuses the insert when
   * there is no new information, when the address is already tried, at the
   * reference ceiling, or by a stochastic test that makes each additional
   * reference 2^n times harder to gain. An occupied slot is overwritten only
   * if its incumbent is terrible or is better referenced than the newcomer.
   *
   * @param time_penalty Seconds to age the timestamp by. Zero when the source
   *                     is announcing itself.
   * @return True if a slot was written.
   */
  bool add(Addr_msg _addr, uint32_t _source, uint64_t time_penalty = 0);

  /**
   * @brief Draw a connection candidate.
   *
   * Picks a table, then a non-empty bucket uniformly, then scans for an
   * occupied slot and accepts it with probability get_chance(). Up to 100
   * tries, with the acceptance threshold relaxed by 1.2 each round so a
   * sparse table still yields something.
   *
   * @param new_only Restrict to the new table, as feelers do.
   * @return The address, or -1 if nothing was drawn.
   */
  int64_t select(bool new_only);

  /**
   * @brief Try to open one outbound connection.
   * @return A VERSION to a candidate, or a DNS_REQUEST when starved and seeds
   *         remain, or an OUT_OF_CONNECTIONS retry. Candidates in an AS
   *         already held by an outbound peer are skipped.
   */
  RoundAction connect();

  /** @brief Refresh an address timestamp on connection, if older than twenty minutes. */
  void connected(uint32_t _ip);

  /**
   * @brief Mark an address as having connected successfully.
   * @param _test_before_evict When the tried slot is taken, register a
   *                           collision for a feeler to test instead of
   *                           evicting the incumbent outright.
   * @return True if the address was promoted now.
   */
  bool good(uint32_t _ip, bool _test_before_evict);

  /**
   * @brief Handle an incoming message.
   *
   * Covers the handshake, address requests and replies, probe outcomes and
   * teardowns. A request is answered on inbound connections only. A reply is
   * metered by the connection's token bucket, and every accepted address is
   * marked known so it is never sent back to its source.
   *
   * @return Messages to schedule in reply.
   */
  std::vector<RoundAction> process_message(uint32_t peer_ip, ActionType msg_type,
                                           std::vector<Addr_msg> data_addresses);

  /**
   * @brief Flush one connection's pending queue as an ADDR.
   * @return The ADDR, or nothing if the queue is empty or the connection is gone.
   */
  std::vector<RoundAction> send_message(uint32_t peer_ip, ActionType msg_type);

  /**
   * @brief Forward an address to at most two peers.
   *
   * Destinations are the connections with the highest hash value, excluding
   * the one the address came from. The hash includes a daily epoch, so the
   * destinations chosen for a given address change every
   * ROTATE_ADDR_RELAY_DEST_INTERVAL.
   *
   * @param reachable False sends to one peer instead of two.
   */
  void relay_address(uint32_t origin, Addr_msg addr, bool reachable, std::vector<RoundAction> &out);

  /**
   * @brief Arm a flush if one is not already pending.
   *
   * Schedules ADDR_TO_SEND after an exponential delay with mean
   * AVG_ADDRESS_BROADCAST_INTERVAL. The m_flush_scheduled guard keeps at most
   * one pending flush per connection, so nothing is scheduled while the queue
   * is empty.
   */
  void schedule_flush(uint32_t peer_ip, std::vector<RoundAction> &out);

  /**
   * @brief Announce our own address on one connection and chain the next.
   *
   * Clears m_addr_known first so the announcement is not suppressed as
   * already known. Nodes accepting no inbound connections announce too, which
   * is how their addresses become known at all.
   */
  std::vector<RoundAction> local_addr_announce(uint32_t peer_ip);

  /** @brief Queue an address for a peer, skipping ones it knows. A full queue replaces a random entry. */
  void push_address(uint32_t peer_ip, Addr_msg addr);

  uint64_t get_previous_stay() const; //!< @return Length of the previous session. See previous_stay.
  void reset_seed_state();            //!< @brief Clear the seed budget, so a returning node may query DNS again.

  /**
   * @brief Restore from a serialized buffer.
   * @note m_last_seed_request, m_seeds_queried and m_flush_scheduled are not
   *       persisted, and out_of_connection_attempt narrows to uint16. All
   *       reset benignly.
   */
  void deserialize(const std::vector<uint8_t> &data, Network_Config &_nc,
                   Net *_net, uint64_t *_round);
  std::vector<uint8_t> serialize(); //!< @return This node's full state, tables included.
};

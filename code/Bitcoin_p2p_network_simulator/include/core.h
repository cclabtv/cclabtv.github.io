/**
 * @file core.h
 * @brief Event scheduler and node lifecycle manager.
 *
 * Core owns the live node set, the arrival and departure schedules, the pools
 * of departed nodes eligible to return, and the queue that drives every
 * message. Time advances in seconds.
 *
 * One step is
 *   insert_peers() -> remove_peers() -> change_peer_ip() ->
 *   spark_next_round() -> increment_round()
 *
 * @note Output depends on both the seed and the worker count. The worker count
 *       is read once from the SIM_THREADS environment variable, so two runs
 *       agree exactly only when both are pinned to the same value.
 */

#ifndef CORE_H
#define CORE_H

#include "peer.h"
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <queue>
#include <thread>
#include <filesystem>

/** Upper bound on the address space searched when drawing a fresh IP. */
static constexpr uint32_t MAX_TRY_IPS{ADDR_MASK};

/** @brief A departed node whose state is held on disk rather than in memory. */
struct DormantCold
{
  uint32_t ip;            //!< Address the peer held when it went dormant.
  uint64_t previous_stay; //!< Length of its last session, in seconds.
};

/**
 * @brief One seed reply, prepared before the workers are dispatched.
 *
 * The sample is drawn on the calling thread so the generator is consumed in a
 * fixed order however the work is later divided.
 */
struct DnsResult
{
  uint32_t source;             //!< Seed ip.
  uint64_t penalty;            //!< Timestamp ageing applied, 3 to 7 days.
  std::vector<uint32_t> addrs; //!< Sampled listening addresses.
};

/**
 * @brief Scheduling, node turnover, and persistence.
 *
 * Inherits Parser privately for its output helpers. That surface is an
 * implementation detail and is not part of this interface.
 */
class Core : Parser
{
private:
  std::atomic<bool> stop_requested{false}; //!< Set by request_stop() for graceful Ctrl+C.

  mutable std::mutex cache_mutex;
  mutable std::mutex edge_mutex;

  /**
   * Current time, in seconds.
   *
   * Starts at 7 days + 1 rather than 0 because several sites age a timestamp
   * backwards by subtracting a penalty, and the largest penalty in the model
   * is the 3-to-7-day ageing applied to DNS seed replies. Beginning the clock
   * past that worst case guarantees every such subtraction stays positive on
   * unsigned arithmetic.
   *
   * @warning Lowering this start value reintroduces the wrap on:
   *   - core.cpp, DNS reply ageing: `round - penalty`, unguarded.
   *   - peer.cpp, Peer::add() refresh path: `_addr.timestamp - time_penalty`,
   *     guarded only by a std::max against 0.
   */
  uint64_t round{(7 * 24 * 60 * 60) + 1};
  uint64_t next_nodes_insertion_check_time{1}; //!< Next time at which arrivals are drawn.

  Net net;                            //!< Synthetic internet: AS ranges over the address space.
  RRandom rrandom;                    //!< Seeded RNG. All draws go through here.
  Network_Config nc;                  //!< The configuration values in use.
  std::vector<uint32_t> dns_seed_ips; //!< Addresses of the seeds, reserved so no node can be assigned one.
  /**
   * Addresses currently in use.
   *
   * Holds both reachable and unreachable peers. Used by the rejection loop
   * that draws a fresh address, so that a live address is never reissued.
   * Entries are erased on departure, so the `== false` branch at the call
   * sites is defensive rather than reachable in normal operation.
   */
  std::unordered_map<uint32_t, bool> address_in_use;
  /**
   * Every live address, in iterable form.
   *
   * Kept alongside the map because get_dns() must sample from the live set,
   * which a hash map cannot do efficiently. Both classes are present, so
   * get_dns() filters by is_listening() at sample time. reachable_count is
   * the only class-filtered member. The cost is a linear erase on every
   * departure and every address rotation.
   */
  std::vector<uint32_t> live_addresses;
  std::unordered_map<uint32_t, uint32_t> id_to_ip; //!< Unique id to current address; repointed on IP change.
  std::unordered_map<uint16_t, uint32_t> asmap;    //!< Live peer count per AS.

  CalendarQueue scheduled_actions{&round}; //!< Pending events, bucketed by due time.

  /** Arrival times drawn ahead of the current time. */
  std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>>
      scheduled_inserting;
  /** Departure times, keyed by unique id rather than address. */
  std::priority_queue<ChurnAction, std::vector<ChurnAction>,
                      std::greater<ChurnAction>>
      scheduled_churn;
  /** Address-rotation times for unreachable peers. */
  std::priority_queue<ChurnAction, std::vector<ChurnAction>, std::greater<ChurnAction>> scheduled_ip_change;

  std::vector<std::shared_ptr<Peer>> dormant_peers; //!< Departed nodes held in memory, eligible to return.
  std::vector<DormantCold> dormant_cold;            //!< Departed peers spilled to disk.

  /**
   * @brief Draw a seed response.
   *
   * Only nodes accepting inbound connections are eligible, so a node that
   * accepts none is never handed out by a seed.
   *
   * @param _max_peers Maximum addresses to return, clamped to the number available.
   * @return Up to @p _max_peers addresses, sampled without replacement.
   */
  std::vector<uint32_t> get_dns(uint32_t _max_peers);

  uint8_t debug;

  std::vector<cache> v_caches; //!< Buffered GETADDR cache snapshots.
  std::vector<edge> v_edges;   //!< Buffered connection create/teardown events.
  std::vector<node> v_nodes;   //!< Buffered per-node records, written at departure.
  std::vector<info> v_infos;   //!< Buffered address-manager dumps.
  std::vector<ip_change_record> v_ip_changes;
  std::vector<telemetry> v_telemetry;
  uint64_t back{0}; //!< Index of the next backup_N directory.
  uint32_t next_unique_id{1};

  // --- persistent worker pool ---
  unsigned int m_num_threads{0}; //!< Worker count, from SIM_THREADS or hardware_concurrency().
  bool m_pool_started{false};
  std::vector<std::thread> m_pool;
  std::function<void(size_t)> m_task;
  std::mutex m_pool_mtx;
  std::condition_variable m_pool_cv;
  std::condition_variable m_pool_done_cv;
  uint64_t m_pool_gen{0}; //!< Generation counter; workers wake when it advances.
  size_t m_pool_remaining{0};
  bool m_pool_stop{false};

  /**
   * @brief Create the worker pool, once.
   *
   * The count comes from SIM_THREADS if set, otherwise from
   * hardware_concurrency(). Share 0 runs on the calling thread, so only
   * m_num_threads - 1 threads are actually created.
   */
  void start_pool();

  /**
   * @brief Run @p task once per worker share and block until all finish.
   *
   * Share 0 executes on the calling thread. With a single-threaded pool the
   * task is invoked directly and no synchronization occurs.
   *
   * @param task Callable receiving the share index in [0, m_num_threads).
   */
  void run_pool(const std::function<void(size_t)> &task);

public:
  /**
   * Live peers, keyed by current address.
   *
   * Keyed by IP, not `unique_id`, because the address is the only way a peer
   * can be reached: `RoundAction` carries sender and reciver addresses, so the message
   * path resolves targets with `network.find(action.reciver)`. Queues that must
   * survive an IP rotation are keyed by `unique_id` and resolved via `id_to_ip`.
   */
  std::unordered_map<uint32_t, std::shared_ptr<Peer>> network;
  size_t reachable_count = 0; //!< Live listening peers; kept in sync at every network add and remove.
  size_t acc_operations = 0;  //!< Events processed since the last status line.

  /**
   * @brief Construct the engine and seed the initial network.
   * @param _net   Synthetic internet defining AS membership.
   * @param _seed  Seed for the random number generator.
   * @param _nc    Run configuration.
   * @param _debug Verbosity level.
   */
  Core(Net _net, int _seed, Network_Config _nc, uint8_t _debug);
  ~Core();

  /**
   * @brief Whether a departing peer is retained for possible return.
   * @param time_online Length of the session just ended, in seconds.
   * @return Currently always true; every departing peer enters a dormant pool.
   */
  bool should_go_dormant(uint64_t time_online);

  uint64_t get_round();   //!< @return The current time, in seconds.
  void increment_round(); //!< Advance the clock by one second.

  /**
   * @brief Draw an exponential session length by peer class.
   *
   * Unreachable peers use nat_session_days. Reachable peers are split by
   * public_core_fraction into a long-lived backbone drawing public_core_days
   * and an ordinary population drawing public_session_days.
   *
   * @param reachable True for a listening peer.
   * @return Session length in seconds, at least 1.
   */
  uint32_t session_duration(bool reachable);

  /**
   * @brief Admit every arrival now due and schedule the next batch.
   *
   * A due arrival is satisfied by reactivating a dormant peer with
   * probability 0.6 when either dormant pool is non-empty, otherwise by
   * minting a new peer. Arrivals are drawn as a Poisson count over
   * join_node_time seconds.
   *
   * @return Pair of (nodes admitted now, arrivals newly scheduled).
   */
  std::pair<uint32_t, uint32_t> insert_peers();

  /** @brief Mint a peer with a fresh identity, address and address manager. */
  void insert_new_peer();

  /**
   * @brief Return a peer from a dormant pool to the live network.
   *
   * The peer keeps its identity and address tables, so returning peers
   * reinject aged address information. Cold entries are faulted in from disk
   * by load_cold().
   *
   * @return True if a peer was reactivated; false if none was eligible.
   */
  bool reactivate_dormant_peer();

  /**
   * @brief Write a departed node's state to disk and drop it from memory.
   * @param peer Peer to spill.
   */
  void spill_cold(const std::shared_ptr<Peer> &peer);

  /**
   * @brief Read a spilled peer back and delete its on-disk copy.
   * @param ip Address the peer held when it was spilled.
   * @return The restored peer, or nullptr if its file was missing or empty.
   * @note Destructive: the file is removed whether or not the load succeeds.
   */
  std::shared_ptr<Peer> load_cold(uint32_t ip);

  size_t dormant_ram_count() const { return dormant_peers.size(); } //!< @return Departed nodes held in memory.
  size_t dormant_cold_count() const { return dormant_cold.size(); } //!< @return Dormant peers spilled to disk.

  /**
   * @brief Change the address of every node whose change is now due.
   * @return Number of rotations performed.
   */
  uint32_t change_peer_ip();

  /**
   * @brief Rotate one peer onto a new address.
   * @param old_ip Address the peer currently holds.
   */
  void change_peer_ip(uint32_t old_ip);

  /**
   * @brief Remove every node whose session ends at or before now.
   * @return Number of departures processed.
   */
  uint32_t remove_peers();

  /**
   * @brief Tear down one peer: close its connections, log it, retire it.
   * @param peer_ip Address of the departing peer.
   */
  void remove_peer(uint32_t peer_ip);

  /**
   * @brief Execute every event now due.
   *
   * Events are taken from the calendar queue, partitioned by target peer so
   * that no two workers touch the same address manager, and dispatched over
   * the workers. Seed samples are drawn beforehand so the generator is
   * consumed in a fixed order. Events generated by handlers accumulate
   * per thread and are merged single-threaded afterwards.
   *
   * @warning The due bucket must be consumed with CalendarQueue::take_due()
   *          alone. Splitting that into a read followed by a separate clear
   *          corrupts the queue's element count and every subsequent save.
   */
  void spark_next_round();

  /** @brief Sample daily counters: feeler outcomes, GETADDR served, addresses dropped. */
  void collect_daily_telemetry();

  /** @brief Flush any output buffer that has grown past 10 MB. */
  void maybe_save_data();

  void request_stop();      //!< Ask the loop to stop and save at the next boundary.
  bool should_stop() const; //!< @return Whether a stop has been requested.

  /**
   * @brief Serialize scheduler state: clock, queues, dormant pools, counters.
   * @return Byte buffer suitable for save_binary().
   * @note Peer state is serialized separately, one file per peer.
   */
  std::vector<uint8_t> serialize_state();

  /** @brief Flush every output buffer regardless of size. */
  void save_all_data();

  /**
   * @brief Write a resumable snapshot, every 30 days.
   *
   * Produces backup_N/state/core_state.bin, one file per live and dormant
   * peer, and a copy of the run configuration, which together are what a
   * resume expects.
   */
  void backup();

  /**
   * @brief Restore a previously saved run.
   * @param state_path Directory holding core_state.bin and the peer files.
   * @note Detects an in-place resume, where source and destination coincide,
   *       to avoid dropping the cold dormant pool.
   */
  void deserialize_state(const std::string &state_path);
};
#endif // CORE_H

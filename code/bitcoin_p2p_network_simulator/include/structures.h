/**
 * @file structures.h
 * @brief Configuration values, grouped by what they control.
 *
 * Network_Config holds every settable value. Structures holds uppercase copies
 * of the subset a node needs, and is inherited so those names are reachable
 * unqualified. Both are laid out in the same groups and the same order:
 * address tables, connection limits, table collisions, address ageing, address
 * broadcasting, reachability, session lengths, arrivals, resource limits.
 *
 * @note Field order is also the constructor's initialization order. Keep the
 *       two in step or the compiler warns.
 */

#ifndef STRUCTUERS_H
#define STRUCTUERS_H

#include <cstdint>
#include <array>
#include "events.h"

/** @brief How nodes are distributed over one network and how they connect on it. */
struct NetProfile
{
  double share = 0.0;  //!< Weight for choosing this as a node's own network. Zero excludes it.
  double listen = 0.0; //!< Probability of accepting inbound connections on it.
  double reach = 0.0;  //!< Probability of being able to open outbound connections to it.
};

/** @brief Every configurable value, grouped by what it controls. */
struct Network_Config
{
  // --- Randomness ---
  int seed; //!< Seed for the random number generator.

  // --- Address tables ---
  int max_new_buckets_slots;   //!< Number of buckets in the new table.
  int max_tried_buckets_slots; //!< Number of buckets in the tried table. Values above 256 are truncated by the bucket hash.
  int bucket_size;             //!< Slots per bucket, in both tables.
  int addrman_new_buckets_per_address; //!< How many new-table slots one address may occupy at once.

  // --- Connection limits ---
  int max_out_bound_connections; //!< Outbound connections a node opens at most.
  int max_in_bound_connections;  //!< Inbound connections a node accepts at most.

  // --- Table collisions ---
  int addrman_set_tried_collision_size; //!< Unresolved collisions held at once.
  int addrman_replacement; //!< Age below which the occupying address keeps its slot, in seconds.
  int addrman_test_window; //!< Age above which an unresolved collision is decided against the occupant, in seconds.

  // --- Address ageing ---
  int addrman_horizon;      //!< Age above which an address is considered stale, in seconds.
  int addrman_retries;      //!< Attempts allowed before an address that never connected is discarded.
  int addrman_max_failures; //!< Consecutive failures allowed before an address is discarded.
  int addrman_min_fail;     //!< Age the last success must exceed before failures are counted, in seconds.

  // --- Address broadcasting ---
  int avg_local_address_broadcast_interval; //!< Mean interval between announcements of a node's own address, in seconds.
  int time_to_update_advertsisng_cache;     //!< Fixed part of the advertised-address cache lifetime, in seconds.
  int avg_time_to_update_advertsisng_cache; //!< Random part added to that lifetime, in seconds.

  // --- Reachability ---
  double prob_reachable;                    //!< Probability that a node accepts inbound connections.
  std::array<NetProfile, NET_MAX> networks; //!< One profile per network. Index with a Network value.

  // --- Session lengths ---
  double public_session_days;  //!< Mean session length of a node accepting inbound, in days.
  double public_core_fraction; //!< Fraction of those nodes drawn from the long-lived group instead.
  double public_core_days;     //!< Mean session length of that long-lived group, in days.
  double nat_session_days;     //!< Mean session length of a node not accepting inbound, in days.

  // --- Arrivals ---
  double join_node_number; //!< Mean number of nodes arriving per window.
  double join_node_time;   //!< Length of that arrival window, in seconds.

  // --- Resource limits ---
  uint32_t dormant_cap;       //!< Non-zero writes departed nodes to disk instead of holding them in memory.
  uint32_t cache_sample_rate; //!< Record cache contents for one node in this many. 1 records every node.

  /** @brief Apply the default value of every field. */
  Network_Config()
      : seed(0),

        max_new_buckets_slots(1024), max_tried_buckets_slots(256),
        bucket_size(64), addrman_new_buckets_per_address(8),

        max_out_bound_connections(8), max_in_bound_connections(117),

        addrman_set_tried_collision_size(10), addrman_replacement(14400),
        addrman_test_window(2400),

        addrman_horizon(2592000), addrman_retries(3), addrman_max_failures(10),
        addrman_min_fail(604800),

        avg_local_address_broadcast_interval(86400),
        time_to_update_advertsisng_cache(75600),
        avg_time_to_update_advertsisng_cache(21600),

        prob_reachable(0.26),

        public_session_days(5.6), public_core_fraction(0.04),
        public_core_days(365.0), nat_session_days(20.0),

        join_node_number(20.83), join_node_time(900.0),

        dormant_cap(2000), cache_sample_rate(1)
  {
    networks[NET_IPV4] = {1.0, prob_reachable, 1.0};
  }
};

/**
 * @brief The subset of Network_Config a node reads, in the same groups.
 *
 * Inherited rather than held, so the names below are reachable unqualified.
 * The constructor copies each value once. Values only the surrounding engine
 * needs are not copied up.
 */
class Structures
{
public:
  // --- Address tables ---
  uint32_t MAX_NEW_BUCKETS_SLOTS;          //!< Number of buckets in the new table.
  uint32_t MAX_TRIED_BUCKETS_SLOTS;        //!< Number of buckets in the tried table.
  int BUCKET_SIZE;                         //!< Slots per bucket, in both tables.
  uint32_t ADDRMAN_NEW_BUCKETS_PER_ADDRESS; //!< How many new-table slots one address may occupy at once.

  // --- Connection limits ---
  uint32_t MAX_OUT_BOUND_CONNECTIONS; //!< Outbound connections a node opens at most.
  uint32_t MAX_IN_BOUND_CONNECTIONS;  //!< Inbound connections a node accepts at most.

  // --- Table collisions ---
  uint32_t ADDRMAN_SET_TRIED_COLLISION_SIZE; //!< Unresolved collisions held at once.
  uint32_t ADDRMAN_REPLACEMENT; //!< Age below which the occupying address keeps its slot, in seconds.
  uint32_t ADDRMAN_TEST_WINDOW; //!< Age above which an unresolved collision is decided against the occupant, in seconds.

  // --- Address ageing ---
  uint32_t ADDRMAN_HORIZON;      //!< Age above which an address is considered stale, in seconds.
  uint32_t ADDRMAN_RETRIES;      //!< Attempts allowed before an address that never connected is discarded.
  uint32_t ADDRMAN_MAX_FAILURES; //!< Consecutive failures allowed before an address is discarded.
  uint32_t ADDRMAN_MIN_FAIL;     //!< Age the last success must exceed before failures are counted, in seconds.

  // --- Address broadcasting ---
  uint32_t AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL; //!< Mean interval between announcements of a node's own address, in seconds.
  uint32_t TIME_TO_UPDATE_ADVERTSISNG_CACHE;     //!< Fixed part of the advertised-address cache lifetime, in seconds.
  uint32_t AVG_TIME_TO_UPDATE_ADVERTSISNG_CACHE; //!< Random part added to that lifetime, in seconds.

public:
  /** @brief Copy the needed subset out of @p _nc. Takes it by value. */
  Structures(Network_Config _nc);
};
#endif // STRUCTUERS_H

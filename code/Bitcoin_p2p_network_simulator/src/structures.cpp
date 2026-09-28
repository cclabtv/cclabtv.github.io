#include "structures.h"

// Initialization order must match the declaration order in structures.h.
Structures::Structures(Network_Config _nc)
    : MAX_NEW_BUCKETS_SLOTS(_nc.max_new_buckets_slots),
      MAX_TRIED_BUCKETS_SLOTS(_nc.max_tried_buckets_slots),
      BUCKET_SIZE(_nc.bucket_size),
      ADDRMAN_NEW_BUCKETS_PER_ADDRESS(_nc.addrman_new_buckets_per_address),

      MAX_OUT_BOUND_CONNECTIONS(_nc.max_out_bound_connections),
      MAX_IN_BOUND_CONNECTIONS(_nc.max_in_bound_connections),

      ADDRMAN_SET_TRIED_COLLISION_SIZE(_nc.addrman_set_tried_collision_size),
      ADDRMAN_REPLACEMENT(_nc.addrman_replacement),
      ADDRMAN_TEST_WINDOW(_nc.addrman_test_window),

      ADDRMAN_HORIZON(_nc.addrman_horizon),
      ADDRMAN_RETRIES(_nc.addrman_retries),
      ADDRMAN_MAX_FAILURES(_nc.addrman_max_failures),
      ADDRMAN_MIN_FAIL(_nc.addrman_min_fail),

      AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL(
          _nc.avg_local_address_broadcast_interval),
      TIME_TO_UPDATE_ADVERTSISNG_CACHE(_nc.time_to_update_advertsisng_cache),
      AVG_TIME_TO_UPDATE_ADVERTSISNG_CACHE(
          _nc.avg_time_to_update_advertsisng_cache)
{}

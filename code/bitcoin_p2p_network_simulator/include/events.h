/**
 * @file events.h
 * @brief Event records, the queue that holds them, and the address encoding.
 */

#ifndef EVENTS_H
#define EVENTS_H

#include <cinttypes>
#include <queue>
#include <vector>
#include <unordered_set>

/**
 * @brief The kinds of event that can be scheduled.
 *
 * @warning Declaration order is part of the serialized format. The value is
 *          written as a bare ordinal and read back with a cast, so inserting,
 *          reordering or removing an entry changes the meaning of data already
 *          written. Append at the end only.
 */
enum ActionType
{
  /**
   * Nothing to schedule. The value a default-constructed RoundAction carries.
   *
   * Returned when a node is already at its outbound limit, and dropped by the
   * caller instead of being queued. Every other return schedules something, so
   * this is what ends a chain of retries.
   */
  NONE,
  ERR,          //!< Fatal condition: all data is written out and the process exits.
  DNS_REQUEST,  //!< Self-addressed. Adds a batch of addresses obtained from a seed.
  PING,         //!< Reserved, never sent or handled. Holds ordinal 3, see the warning above.
  PONG,         //!< Reserved, never sent or handled. Holds ordinal 4.
  VERSION,      //!< Connection request. Refused when the target accepts no inbound on that network.
  NOVERSION,    //!< Connection refused. The sender records a failed attempt and retries.
  VERACKE,      //!< Connection accepted. The sender registers the peer and requests addresses.
  NOVERACKE,    //!< Acceptance declined: the sender filled up, took that group, or is already connected.
  GETADDR,      //!< Address request. Answered on inbound connections only.
  ADDR,         //!< Batch of addresses. Metered by the receiver's token bucket.
  ADDR_TO_SEND, //!< Flush of one connection's pending queue. Armed on demand, never self-rescheduling.
  CACHE_ENTRY_EXPIRATION, //!< Self-addressed. Rebuilds the advertised-address cache and chains the next.
  OUT_OF_CONNECTIONS,     //!< Self-addressed. Opens one more outbound connection, or backs off and retries.
  RESOLVE_FEELER,         //!< Self-addressed. Runs one probe cycle and chains the next.
  FEELER,                 //!< Short-lived probe of an address, used to test it before evicting another.
  FEELER_S,               //!< Probe succeeded. The target is promoted.
  FEELER_F,               //!< Probe failed. The target records a failed attempt.
  REMOVE_OUTBOUND_CONNECTION, //!< Asks the receiver to drop the sender from its outbound side.
  REMOVE_INBOUND_CONNECTION,  //!< Asks the receiver to drop the sender from its inbound side.
  LOCAL_ADDR_ANNOUNCE //!< Announcement of a node's own address on one connection. Chains the next.
};

/**
 * @brief The network an address belongs to, encoded in its top bits.
 *
 * @note Only NET_IPV4 carries a non-zero share by default. The rest are
 *       defined and handled but unpopulated until configured.
 */
enum Network : uint8_t
{
  NET_UNROUTABLE = 0, //!< What net_of(0) yields, which is why 0 serves as the empty-slot marker.
  NET_IPV4,
  NET_IPV6, //!< Shares a grouping with IPv4: the group function ignores the network bits for both.
  NET_ONION,
  NET_I2P,
  NET_CJDNS,
  NET_INTERNAL,
  NET_MAX,
};
static_assert(NET_MAX <= 8, "NetSet is a uint8_t bitmask");

using NetSet = uint8_t; // bit i <=> Network i
/** @return A one-bit mask for @p n. */
constexpr NetSet net_bit(Network n) { return static_cast<NetSet>(1u << n); }
/** @return Whether @p s contains @p n. */
constexpr bool net_has(NetSet s, Network n) { return (s & net_bit(n)) != 0; }
/** @return @p s with @p n added. */
constexpr NetSet net_add(NetSet s, Network n) { return static_cast<NetSet>(s | net_bit(n)); }

// Address layout: top 3 bits select the network, low 29 bits are the address
constexpr int NET_SHIFT = 29;
constexpr uint32_t ADDR_MASK = (1u << NET_SHIFT) - 1; // 2^29 per network
/** @return The network encoded in @p a. Address 0 is NET_UNROUTABLE. */
constexpr Network net_of(uint32_t a) { return static_cast<Network>(a >> NET_SHIFT); }
/**
 * @brief Pack a network and an offset into one address.
 * @note Onion, I2P, CJDNS and Internal addresses each collapse to a single
 *       group. IPv4 and IPv6 are grouped by their low 29 bits instead.
 */
constexpr uint32_t make_addr(Network n, uint32_t off)
{
  return (static_cast<uint32_t>(n) << NET_SHIFT) | (off & ADDR_MASK);
}

/** @brief One address together with the time it was last seen. */
struct Addr_msg
{
  uint64_t timestamp; //!< Best known time the address was seen, in seconds, already aged by any penalty.
  uint32_t addr;      //!< The address itself.
  Addr_msg(uint64_t _timestamp, uint32_t _addr)
      : timestamp(_timestamp), addr(_addr) {}
};

/**
 * @brief One scheduled event.
 *
 * Both endpoints are addresses rather than identifiers, because an address is
 * the only name one node holds for another. An event whose sender equals its
 * receiver is a timer rather than a message.
 */
struct RoundAction
{
  uint64_t execute_time;      //!< Time the event comes due, in seconds.
  uint32_t sender;            //!< Originating address.
  uint32_t reciver;           //!< Target address. Equal to sender for a timer.
  ActionType action;          //!< What to do.
  std::vector<Addr_msg> args; //!< Payload, used by ADDR only.
  /** @brief Empty event. Its NONE action means "nothing to schedule". */
  RoundAction() : execute_time(0), sender(0), reciver(0), action(NONE) {}

  RoundAction(uint64_t _execute_time, uint32_t _sender, uint32_t _reciver,
              ActionType _action)
  {
    execute_time = _execute_time;
    sender = _sender;
    reciver = _reciver;
    action = _action;
  }
  RoundAction(uint64_t _execute_time, uint32_t _sender, uint32_t _reciver,
              ActionType _action, std::vector<Addr_msg> _args)
  {
    execute_time = _execute_time;
    sender = _sender;
    reciver = _reciver;
    action = _action;
    args = _args;
  }
  /**
   * @brief Reversed comparison, ordering earliest first under a max-heap.
   * @note Not used. Nothing orders these.
   */
  bool operator<(const RoundAction &other) const
  {
    return execute_time > other.execute_time;
  }
};

/**
 * @brief Timing wheel holding every pending event.
 *
 * A ring of W buckets indexed by time modulo W, so inserting an event and
 * taking a whole time step are both constant time and need no comparisons.
 * Events further out than the horizon are parked in @ref overflow and moved
 * into the ring by reflow() once they come within range.
 */
struct CalendarQueue
{
  static constexpr uint64_t W = 1u << 20; //!< Horizon: 1048576 seconds, 12.1 days.
  std::vector<std::vector<RoundAction>> wheel; //!< W buckets, one per time step modulo W.
  std::vector<RoundAction> overflow;           //!< Events beyond the horizon, awaiting reflow().
  const uint64_t *now;                         //!< Borrowed pointer to the current time. Not owned.
  size_t count = 0;                            //!< Total events held.

  explicit CalendarQueue(const uint64_t *round_ptr) : wheel(W), now(round_ptr) {}

  /** @brief Insert @p a, clamping a time at or before now to now + 1. */
  void push(RoundAction a)
  {
    uint64_t t = std::max<uint64_t>(a.execute_time, *now + 1);
    if (t - *now >= W)
      overflow.push_back(std::move(a));
    else
      wheel[t & (W - 1)].push_back(std::move(a));
    ++count;
  }
  /**
   * @brief Reference to one time step's bucket, leaving @ref count untouched.
   * @warning Reading here, moving the bucket out, then calling clear_due()
   *          makes clear_due subtract the size of an already emptied bucket,
   *          so @ref count grows without bound. Use take_due() instead.
   */
  std::vector<RoundAction> &due(uint64_t r) { return wheel[r & (W - 1)]; }
  /**
   * @brief Take one time step's events, moving the bucket out and decrementing
   *        @ref count in a single step.
   *
   * Taking an empty bucket is a no-op.
   */
  std::vector<RoundAction> take_due(uint64_t r)
  {
    auto &b = wheel[r & (W - 1)];
    count -= b.size();
    return std::move(b);
  }

  /** @brief Discard one time step's events. @warning See due(). */
  void clear_due(uint64_t r)
  {
    count -= wheel[r & (W - 1)].size();
    wheel[r & (W - 1)].clear();
  }
  /** @brief Drop everything, ring and overflow alike. */
  void clear()
  {
    for (auto &b : wheel)
      b.clear();
    overflow.clear();
    count = 0;
  }
  /** @brief Visit every pending event, ring first then overflow. */
  template <class F>
  void for_each(F f) const
  {
    for (const auto &b : wheel)
      for (const auto &a : b)
        f(a);
    for (const auto &a : overflow)
      f(a);
  }

  /**
   * @brief Move overflow events that now fall inside the horizon into the ring.
   *
   * Returns immediately when the overflow is empty.
   */
  void reflow()
  {
    if (overflow.empty())
      return;
    std::vector<RoundAction> still;
    still.reserve(overflow.size());
    for (auto &a : overflow)
    {
      uint64_t t = std::max<uint64_t>(a.execute_time, *now + 1);
      if (t - *now >= W)
        still.push_back(std::move(a));
      else
        wheel[t & (W - 1)].push_back(std::move(a));
    }
    overflow.swap(still);
  }
  size_t size() const { return count; }     //!< @return Events pending, ring plus overflow.
  bool empty() const { return count == 0; } //!< @return Whether nothing is pending.
};

/**
 * @brief A scheduled departure or address change.
 *
 * Keyed by identifier rather than address, so the entry stays valid when the
 * node's address changes. The identifier is resolved to an address when the
 * entry comes due.
 */
struct ChurnAction
{
  uint64_t execute_time; //!< Time the node leaves, or changes address, in seconds.
  uint32_t node_id;      //!< The node's identifier.
  ChurnAction(uint64_t _execute_time, uint32_t _node_id)
  {
    execute_time = _execute_time;
    node_id = _node_id;
  }
  /** @brief Ordering that puts the earliest entry first under std::greater. */
  bool operator>(const ChurnAction &other) const
  {
    return execute_time > other.execute_time;
  }
};
#endif // EVENTS_H

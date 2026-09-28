#include "peer.h"

Peer ::Peer(NetSet _reachable_nets, NetSet _listening_nets, Network_Config &_nc, RRandom _rrandom,
            uint32_t _ip, uint16_t _asmap, uint32_t _source, Net *_net,
            uint64_t *_round, std::vector<Addr_msg> _dns_seed,
            uint32_t _unique_id)
    : Structures(_nc), reachable_nets(_reachable_nets), listening_nets(_listening_nets), rrandom(_rrandom), ip(_ip),
      unique_id(_unique_id), asmap(_asmap), source(_source), net(_net),
      round(_round)
{
  n_time = *round;
  cache_entry_expiration = *round + TIME_TO_UPDATE_ADVERTSISNG_CACHE +
                           rrandom.uniform_int<uint32_t>(
                               0, AVG_TIME_TO_UPDATE_ADVERTSISNG_CACHE - 1);
  next_feeler = *round + rrandom.exponential_distribution(FEELER_INTERVAL);
  n_new = 0;
  n_tried = 0;
  init_tables();

  uint32_t day = 24 * 60 * 60;
  for (auto &seed_ip : _dns_seed)
    add(seed_ip, _source, rrandom.uniform_int<uint32_t>(3 * day, 7 * day));
  m_last_seed_request = *round;
  m_seeds_queried = 1;
  get_addresses();
}
uint32_t Peer::get_unique_id() const { return unique_id; }
uint64_t Peer::get_n_time() const { return n_time; }
uint16_t Peer::get_asmap() const { return asmap; }
uint32_t Peer::get_ip() const { return ip; }

uint64_t Peer::get_map_info_size() const { return map_info.size(); }
std::unordered_map<uint32_t, addInfo> Peer::get_map_info() const
{
  return map_info;
}
uint64_t Peer::get_n_new() const { return n_new; }
uint64_t Peer::get_n_tried() const { return n_tried; }
std::vector<RoundAction> Peer::disconnect(bool dormant)
{
  std::vector<RoundAction> v_RA;
  for (auto &ip_in : inbound_connections)
    v_RA.push_back({*round + 1, get_ip(), ip_in, REMOVE_OUTBOUND_CONNECTION});

  for (auto &ip_out : outbound_connections)
    v_RA.push_back({*round + 1, get_ip(), ip_out, REMOVE_INBOUND_CONNECTION});
  if (dormant)
  {
    tried_collisions.clear();
    outbound_asmap.clear();
    outbound_connections.clear();
    inbound_connections.clear();
    map_connection.clear();
    addrs_response_cache.clear();
    set_previous_stay();
  }
  return v_RA;
}
addInfo *Peer::create(uint32_t _ip, uint32_t _source, uint64_t t)
{

  addInfo ai = {_ip, static_cast<uint16_t>(net->get_group(_ip)),
                _source, static_cast<uint32_t>(*round),
                static_cast<uint32_t>(t), 0,
                0, 0,
                0, false,
                0};

  ai.n_random_pos = static_cast<uint32_t>(v_randoms.size());
  map_info[_ip] = ai;
  v_randoms.push_back(_ip);
  n_new++;
  return &map_info[_ip];

  return nullptr;
}
uint16_t Peer::get_new_bucket(uint32_t _ip, uint32_t _source) const
{
  uint16_t as_source = net->get_group(_source);
  uint16_t as_ip = net->get_group(_ip);

  // hash1: salt + addr group + source group
  std::vector<uint8_t> d1 = {
      static_cast<uint8_t>(unique_id & 0xFF),
      static_cast<uint8_t>((unique_id >> 8) & 0xFF),
      static_cast<uint8_t>((unique_id >> 16) & 0xFF),
      static_cast<uint8_t>((unique_id >> 24) & 0xFF),
      static_cast<uint8_t>(as_ip & 0xFF),
      static_cast<uint8_t>((as_ip >> 8) & 0xFF),
      static_cast<uint8_t>(as_source & 0xFF),
      static_cast<uint8_t>((as_source >> 8) & 0xFF),
  };
  uint8_t selector = static_cast<uint8_t>(
      simple_hash(d1) % ADDRMAN_NEW_BUCKETS_PER_SOURCE_GROUP);

  // hash2: salt + source group + selector -> one source group reaches
  // at most 64 of the 1024 new buckets
  std::vector<uint8_t> d2 = {
      static_cast<uint8_t>(unique_id & 0xFF),
      static_cast<uint8_t>((unique_id >> 8) & 0xFF),
      static_cast<uint8_t>((unique_id >> 16) & 0xFF),
      static_cast<uint8_t>((unique_id >> 24) & 0xFF),
      static_cast<uint8_t>(as_source & 0xFF),
      static_cast<uint8_t>((as_source >> 8) & 0xFF),
      selector,
  };
  return static_cast<uint16_t>(simple_hash(d2) % MAX_NEW_BUCKETS_SLOTS);
}

uint8_t Peer::get_position(uint32_t _ip, uint32_t _bucket,
                           bool _is_tried) const
{
  std::vector<uint8_t> data = {
      static_cast<uint8_t>(unique_id & 0xFF),
      static_cast<uint8_t>((unique_id >> 8) & 0xFF),
      static_cast<uint8_t>((unique_id >> 16) & 0xFF),
      static_cast<uint8_t>((unique_id >> 24) & 0xFF),
      _is_tried ? static_cast<uint8_t>('K') : static_cast<uint8_t>('N'),
      static_cast<uint8_t>(_ip & 0xFF),
      static_cast<uint8_t>((_ip >> 8) & 0xFF),
      static_cast<uint8_t>((_ip >> 16) & 0xFF),
      static_cast<uint8_t>((_ip >> 24) & 0xFF),
      static_cast<uint8_t>(_bucket & 0xFF),
      static_cast<uint8_t>((_bucket >> 8) & 0xFF),
  };
  uint64_t hash = simple_hash(data) % BUCKET_SIZE;
  return hash;
}
uint8_t Peer::get_tried_bucket(uint32_t _ip) const
{
  uint16_t as_ip = net->get_group(_ip);

  // hash1: salt + full address
  std::vector<uint8_t> d1 = {
      static_cast<uint8_t>(unique_id & 0xFF),
      static_cast<uint8_t>((unique_id >> 8) & 0xFF),
      static_cast<uint8_t>((unique_id >> 16) & 0xFF),
      static_cast<uint8_t>((unique_id >> 24) & 0xFF),
      static_cast<uint8_t>(_ip & 0xFF),
      static_cast<uint8_t>((_ip >> 8) & 0xFF),
      static_cast<uint8_t>((_ip >> 16) & 0xFF),
      static_cast<uint8_t>((_ip >> 24) & 0xFF),
  };
  uint8_t selector =
      static_cast<uint8_t>(simple_hash(d1) % ADDRMAN_TRIED_BUCKETS_PER_GROUP);

  // hash2: salt + group + selector -> one AS reaches at most 8 tried buckets
  std::vector<uint8_t> d2 = {
      static_cast<uint8_t>(unique_id & 0xFF),
      static_cast<uint8_t>((unique_id >> 8) & 0xFF),
      static_cast<uint8_t>((unique_id >> 16) & 0xFF),
      static_cast<uint8_t>((unique_id >> 24) & 0xFF),
      static_cast<uint8_t>(as_ip & 0xFF),
      static_cast<uint8_t>((as_ip >> 8) & 0xFF),
      selector,
  };
  return static_cast<uint8_t>(simple_hash(d2) % MAX_TRIED_BUCKETS_SLOTS);
}

void Peer::init_tables()
{
  vvNew.assign(static_cast<size_t>(MAX_NEW_BUCKETS_SLOTS) * BUCKET_SIZE, 0);
  vvTried.assign(static_cast<size_t>(MAX_TRIED_BUCKETS_SLOTS) * BUCKET_SIZE, 0);
  new_fill.assign(MAX_NEW_BUCKETS_SLOTS, 0);
  tried_fill.assign(MAX_TRIED_BUCKETS_SLOTS, 0);
  new_nonempty.clear();
  tried_nonempty.clear();
  new_nonempty_at.assign(MAX_NEW_BUCKETS_SLOTS, -1);
  tried_nonempty_at.assign(MAX_TRIED_BUCKETS_SLOTS, -1);
}

void Peer::slot_set(bool _use_tried, size_t _bucket, size_t _position,
                    uint32_t _ip)
{
  auto &tbl = _use_tried ? vvTried : vvNew;
  auto &fill = _use_tried ? tried_fill : new_fill;
  auto &list = _use_tried ? tried_nonempty : new_nonempty;
  auto &at = _use_tried ? tried_nonempty_at : new_nonempty_at;

  uint32_t &cell = tbl[_bucket * BUCKET_SIZE + _position];
  if (cell == 0 && _ip != 0)
  {
    if (fill[_bucket]++ == 0)
    {
      at[_bucket] = static_cast<int32_t>(list.size());
      list.push_back(static_cast<uint16_t>(_bucket));
    }
  }
  else if (cell != 0 && _ip == 0)
  {
    if (--fill[_bucket] == 0)
    {
      const int32_t i = at[_bucket];
      list[i] = list.back();
      at[list[i]] = i; // no-op when the bucket removed was the last element
      list.pop_back();
      at[_bucket] = -1;
    }
  }
  cell = _ip;
}

void Peer::slot_clear(bool _use_tried, size_t _bucket, size_t _position)
{
  slot_set(_use_tried, _bucket, _position, 0);
}

int64_t Peer::get_entry(bool _use_tried, size_t _bucket, size_t _position)
{
  const auto &tbl = _use_tried ? vvTried : vvNew;
  const uint32_t v = tbl[_bucket * BUCKET_SIZE + _position];
  return v == 0 ? -1 : static_cast<int64_t>(v);
}

std::vector<Addr_msg> Peer::get_addresses(size_t max_addresses, size_t max_pct,
                                          bool _filtered)
{
  size_t n_nodes = v_randoms.size();
  if (max_pct != 0)
  {
    max_pct = std::min(max_pct, size_t{100});
    n_nodes = max_pct * n_nodes / 100;
  }
  if (max_addresses != 0)

    n_nodes = std::min(n_nodes, max_addresses);

  std::vector<Addr_msg> addresses;
  addresses.reserve(n_nodes);
  for (size_t n = 0; n < v_randoms.size(); ++n)
  {
    if (addresses.size() >= n_nodes)
      break;
    const size_t rnd_pos = rrandom.uniform_int<size_t>(n, v_randoms.size() - 1);
    swap_random(n, rnd_pos);
    const uint32_t _addr = v_randoms[n];
    if (_filtered && is_terrible(_addr))
      continue;
    auto it = map_info.find(_addr);
    if (it == map_info.end())
      continue;
    addresses.push_back({it->second.n_time, _addr});
  }

  return addresses;
}
std::pair<RoundAction, cache> Peer::get_addresses()
{
  addrs_response_cache = get_addresses(1000, 23, true);
  cache_entry_expiration = *round + TIME_TO_UPDATE_ADVERTSISNG_CACHE +
                           rrandom.uniform_int<uint32_t>(
                               0, AVG_TIME_TO_UPDATE_ADVERTSISNG_CACHE - 1);
  std::vector<std::pair<uint32_t, uint64_t>> v_add;
  v_add.reserve(addrs_response_cache.size());
  for (auto &addr : addrs_response_cache)
    v_add.push_back({addr.addr, addr.timestamp});
  return {{cache_entry_expiration, get_ip(), get_ip(), CACHE_ENTRY_EXPIRATION},
          {*round, get_ip(), static_cast<uint32_t>(map_info.size()), std::move(v_add)}};
}
double Peer::get_chance(uint32_t _ip) const
{
  auto it_peer = map_info.find(_ip);
  if (it_peer == map_info.end())
    return 0; // peer doesnt exist
  auto peer = it_peer->second;

  if (*round < peer.m_last_try)
    return 0;
  double fChance = 1.0;

  if (*round - peer.m_last_try < 60 * 10)
    fChance *= 0.01;
  fChance *= pow(0.66, std::min<int>(peer.n_attempts, 8));
  return fChance;
}
void Peer::set_ip(uint32_t new_ip) { ip = new_ip; }
void Peer::set_asmap(uint16_t new_asmap) { asmap = new_asmap; }

bool Peer::is_terrible(uint32_t _ip) const
{
  auto it_terrible = map_info.find(_ip);
  if (it_terrible == map_info.end())
    return true;
  const addInfo &peer = it_terrible->second;
  if (*round - peer.m_last_try <=
      60)
  { // never remove things tried in the last minute
    return false;
  }

  if (peer.n_time > *round + (10 * 60))
  { // came in a flying DeLorean
    return true;
  }

  if (*round - peer.n_time > ADDRMAN_HORIZON)
  { // not seen in recent history
    return true;
  }

  if (peer.m_last_success == 0 &&
      peer.n_attempts >= ADDRMAN_RETRIES)
  { // tried N times and never a success
    return true;
  }

  if (*round - peer.m_last_success > ADDRMAN_MIN_FAIL &&
      peer.n_attempts >=
          ADDRMAN_MAX_FAILURES)
  { // N successive failures in the last week
    return true;
  }

  return false;
}
void Peer::clear_new(uint16_t _bucket, uint8_t _position)
{
  const uint32_t _ip = vvNew[static_cast<size_t>(_bucket) * BUCKET_SIZE + _position];
  if (_ip == 0)
    return; // empty slot: nothing referenced
  auto it = map_info.find(_ip);
  if (it == map_info.end())
    return; // stale slot pointing at a deleted entry
  addInfo &ai = it->second;
  if (ai.n_ref_count > 0)
    ai.n_ref_count--;
  if (ai.n_ref_count == 0)
    _delete(&ai);
}

void Peer::_delete(addInfo *_addr_delete)
{
  const uint32_t del_ip = _addr_delete->ip;
  auto it = map_info.find(del_ip);
  if (it != map_info.end())
  {
    // O(1) removal, mirroring Core's AddrManImpl::Delete(): read the recorded
    // position, swap the entry with the last one, then pop. The guard keeps the
    // old behaviour (silent no-op) if the index is ever stale.
    const size_t pos = it->second.n_random_pos;
    if (pos < v_randoms.size() && v_randoms[pos] == del_ip)
    {
      swap_random(pos, v_randoms.size() - 1);
      v_randoms.pop_back();
      n_new--;
    }
    map_info.erase(it);
  }
}
void Peer::swap_random(size_t i, size_t j)
{
  if (i == j)
    return;
  const uint32_t a = v_randoms[i], b = v_randoms[j];
  std::swap(v_randoms[i], v_randoms[j]);
  auto ia = map_info.find(a);
  if (ia != map_info.end())
    ia->second.n_random_pos = static_cast<uint32_t>(j);
  auto ib = map_info.find(b);
  if (ib != map_info.end())
    ib->second.n_random_pos = static_cast<uint32_t>(i);
}

void Peer::make_tried(uint32_t _ip)
{
  // remove the entry from all new buckets
  for (uint16_t i = 0; i < MAX_NEW_BUCKETS_SLOTS; i++)
  {
    const int pos{get_position(_ip, i, false)};
    if (get_entry(false, i, pos) == _ip)
    {
      slot_clear(false, i, pos);
      map_info[_ip].n_ref_count--;
      if (map_info[_ip].n_ref_count == 0)
        break;
    }
  }
  n_new--;
  if (map_info[_ip].n_ref_count != 0)
  {
    std::cout << "make_tried: inconsistent refcount "
              << (int)map_info[_ip].n_ref_count << " for " << _ip << std::endl;
    map_info[_ip].n_ref_count = 0;
  }

  uint8_t tried_bucket = get_tried_bucket(_ip);
  uint8_t tried_bucket_pos = get_position(_ip, tried_bucket, true);
  int64_t old_position_ip = get_entry(true, tried_bucket, tried_bucket_pos);
  if (old_position_ip != -1)
  {
    if (map_info.find(old_position_ip) != map_info.end())
    {
      // evict the incumbent back into the new table
      auto &old_peer = map_info[old_position_ip];
      old_peer.in_tried = false;
      slot_clear(true, tried_bucket, tried_bucket_pos);
      n_tried--;
      uint16_t new_bucket = get_new_bucket(old_peer.ip, old_peer.source);
      uint8_t new_bucket_pos = get_position(old_peer.ip, new_bucket, false);
      clear_new(new_bucket, new_bucket_pos);

      old_peer.n_ref_count = 1;
      slot_set(false, new_bucket, new_bucket_pos, old_peer.ip);
      n_new++;
    }
    else
    {
      // stale slot pointing at a deleted entry: reclaim it instead of
      // orphaning the address we're promoting
      slot_clear(true, tried_bucket, tried_bucket_pos);
      n_tried--;
    }
  }

  slot_set(true, tried_bucket, tried_bucket_pos, _ip);
  n_tried++;
  map_info[_ip].in_tried = true;
}
bool Peer::add(Addr_msg _addr, uint32_t _source, uint64_t time_penalty)
{
  if (_addr.addr == 0)
    return false;
  if (_addr.addr == this->get_ip())
    return false;

  if (_addr.addr == _source)
    time_penalty = 0;
  addInfo *p_info;
  bool insert = false;
  if (map_info.find(_addr.addr) != map_info.end())
  {
    // periodically update n_time
    p_info = &map_info[_addr.addr];
    const bool currently_online{*round - _addr.timestamp < 24 * 60 * 60};
    const auto update_interval{currently_online ? 60 * 60 : 24 * 60 * 60};
    if (_addr.timestamp > update_interval + time_penalty &&
        p_info->n_time < _addr.timestamp - update_interval - time_penalty)
      p_info->n_time = std::max(0ul, _addr.timestamp - time_penalty);

    // do not update if no new information is present
    if (_addr.timestamp <= p_info->n_time)
      return false;
    // do not update if the entry was already in the "tried" table
    if (p_info->in_tried)
      return false;

    // do not update if the max reference count is reached
    if (p_info->n_ref_count == ADDRMAN_NEW_BUCKETS_PER_ADDRESS)
      return false;

    // stochastic test: previous nRefCount == N: 2^N times harder to increase it
    if (p_info->n_ref_count > 0)
    {
      const int n_factor{1 << p_info->n_ref_count};
      if (rrandom.uniform_int<int>(0, n_factor - 1) != 0)
        return false;
    }
  }
  else
  {

    p_info = create(_addr.addr, _source, _addr.timestamp);
    p_info->n_time =
        (_addr.timestamp > time_penalty) ? (_addr.timestamp - time_penalty) : 0;
    if (p_info == nullptr)
      return false;
  }
  uint16_t bucket = get_new_bucket(_addr.addr, _source);
  uint8_t position = get_position(_addr.addr, bucket, false);
  insert = get_entry(false, bucket, position) == -1;
  if (get_entry(false, bucket, position) != _addr.addr)
  {
    if (!insert)
    {
      auto it_existing = map_info.find(
          vvNew[static_cast<size_t>(bucket) * BUCKET_SIZE + position]);
      if (it_existing == map_info.end())
      {
        insert = true;
      }
      else
      {
        const addInfo &info_existing = it_existing->second;
        if (is_terrible(info_existing.ip) ||
            (info_existing.n_ref_count > 1 && p_info->n_ref_count == 0))
          // Overwrite the existing new table entry.
          insert = true;
      }
    }
    if (insert)
    {
      clear_new(bucket, position);
      p_info->n_ref_count++;
      slot_set(false, bucket, position, _addr.addr);
    }
    else
    {
      if (p_info->n_ref_count == 0)
        _delete(p_info);
    }
  }

  return insert;
}
int64_t Peer::select(bool new_only)
{
  if (outbound_connections.size() == v_randoms.size())
    return -1;
  if (v_randoms.empty())
    return -1;
  size_t new_count = n_new;
  size_t tried_count = n_tried;
  if (new_only && new_count == 0)
    return -1;
  if (new_count + tried_count == 0)
    return -1;

  // Decide if we are going to search the new or tried table
  // If either option is viable, use a 50% chance to choose
  bool search_tried;
  if (new_only || tried_count == 0)
    search_tried = false;
  else if (new_count == 0)
    search_tried = true;
  else
    search_tried = rrandom.flip();

  double chance_factor = 1.0;
  for (int i = 0; i < 100; i++)
  {
    // Uniform over NON-EMPTY buckets == Core's rejection sampling conditioned
    // on occupancy, but O(entries) instead of O(64 * empty-misses) when sparse.
    int bucket = -1;
    if (search_tried)
    {
      if (tried_nonempty.empty())
        return -1;
      bucket = tried_nonempty[rrandom.uniform_int<size_t>(
          0, tried_nonempty.size() - 1)];
    }
    else
    {
      if (new_nonempty.empty())
        return -1;
      bucket =
          new_nonempty[rrandom.uniform_int<size_t>(0, new_nonempty.size() - 1)];
    }
    int initial_position = rrandom.uniform_int<uint32_t>(0, BUCKET_SIZE - 1);
    int j, position;
    int64_t node_ip = -1;
    for (j = 0; j < BUCKET_SIZE; j++)
    {
      position = (initial_position + j) % BUCKET_SIZE;
      node_ip = get_entry(search_tried, bucket, position);
      if (node_ip != -1)
      {
        break;
      }
    }
    if (node_ip == -1)
      continue;
    if (j == BUCKET_SIZE)
      continue;
    if (map_info.find(node_ip) == map_info.end())
      continue;
    if (rrandom.uniform_int<uint32_t>(0, (1 << 30) - 1) < chance_factor * get_chance(node_ip) * (1 << 30))
    {
      return node_ip;
    }
    chance_factor *= 1.2;
  }
  return -1;
}
void Peer::attempt(uint32_t _ip, bool f_count_failure)
{
  if (map_info.find(_ip) == map_info.end())
    return;
  auto &peer = map_info[_ip];
  peer.m_last_try = *round;
  if (f_count_failure && peer.m_last_count_attempt < m_last_good)
  {
    peer.m_last_count_attempt = *round;
    peer.n_attempts++;
  }
}
RoundAction Peer::connect()
{
  for (int i = 0; i < 100; i++)
  {
    if (outbound_connections.size() == MAX_OUT_BOUND_CONNECTIONS)
      return {};
    int64_t ip_to_connect = select(false);
    if (ip_to_connect == -1 || ip_to_connect == 0)
      continue;
    if (map_info.find(ip_to_connect) == map_info.end())
      continue;
    if (outbound_asmap.find(net->get_group(ip_to_connect)) !=
        outbound_asmap.end())
      continue;
    if (map_connection.find(ip_to_connect) != map_connection.end())
      continue;

    // only consider very recently tried nodes after 30 failed attempts
    if (*round - map_info[ip_to_connect].m_last_try < 10 * 60 && i < 30)
      continue;
    RoundAction RA = {*round + 1, get_ip(),
                      static_cast<uint32_t>(ip_to_connect), VERSION};
    return RA;
  }
  // No viable candidate found.
  if (outbound_connections.size() < 2 && m_seeds_queried < NUM_DNS_SEEDS)
  {
    uint64_t delay = (v_randoms.size() < 1000) ? 11 : 5 * 60;
    if (*round - m_last_seed_request >= delay)
    {
      m_last_seed_request = *round;
      m_seeds_queried++;
      return {*round + 1, get_ip(), get_ip(), DNS_REQUEST};
    }
  }
  // Back off and retry with fresh randomness; poll faster while still seeding
  uint64_t retry =
      (outbound_connections.size() < 2 && m_seeds_queried < NUM_DNS_SEEDS) ? 11 : 60;
  return {*round + retry, get_ip(), get_ip(), OUT_OF_CONNECTIONS};
}
void Peer::connected(uint32_t peer_ip)
{
  auto peer_it = map_info.find(peer_ip);
  if (peer_it == map_info.end())
    return;
  auto &peer = peer_it->second;
  if (*round - peer.n_time > 20 * 60)
    peer.n_time = *round;
}

std::vector<RoundAction>
Peer::process_message(uint32_t peer_ip, ActionType msg_type,
                      std::vector<Addr_msg> data_addresses)
{
  resolve_collisions();
  if (msg_type == VERSION)
  {
    if (inbound_connections.size() < MAX_IN_BOUND_CONNECTIONS &&
        map_connection.find(peer_ip) == map_connection.end())
    {
      std::vector<RoundAction> v_RA;
      uint64_t _m_next_local_addr_send =
          *round + rrandom.exponential_distribution(
                       AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL);
      uint64_t _m_next_addr_send = *round + rrandom.exponential_distribution(
                                                AVG_ADDRESS_BROADCAST_INTERVAL);
      map_connection[peer_ip] = {{}, {}, false, 1.0, *round, 0, 0, true};
      map_connection[peer_ip].m_next_local_addr_send = _m_next_local_addr_send;
      map_connection[peer_ip].m_next_addr_send = _m_next_addr_send;
      inbound_connections.push_back(peer_ip);
      v_RA.push_back({std::max(map_connection[peer_ip].m_next_local_addr_send,
                               *round + 1),
                      get_ip(), peer_ip, LOCAL_ADDR_ANNOUNCE});

      v_RA.push_back({*round + 1, get_ip(), peer_ip, VERACKE});

      return v_RA;
    }
    else
    {
      RoundAction RA = {*round + 1, get_ip(), peer_ip, NOVERSION};
      return {RA};
    }
  }
  if (msg_type == NOVERSION)
  {
    auto peer_it = map_info.find(peer_ip);
    if (peer_it != map_info.end())
    {
      attempt(peer_ip, outbound_asmap.size() >= 2);
    }
    if (outbound_connections.size() < 2 && m_seeds_queried < NUM_DNS_SEEDS)
    {
      // 11s between seeds while addrman is small, 5min once it's big
      uint64_t delay = (v_randoms.size() < 1000) ? 11 : 5 * 60;
      if (*round - m_last_seed_request >= delay)
      {
        m_last_seed_request = *round;
        m_seeds_queried++;
        return {{*round + 1, get_ip(), get_ip(), DNS_REQUEST}};
      }
    }
    RoundAction RA = connect();
    return {RA};
  }
  if (msg_type == VERACKE)
  {

    out_of_connection_attempt = *round;
    if (outbound_connections.size() < MAX_OUT_BOUND_CONNECTIONS &&
        outbound_asmap.find(net->get_group(peer_ip)) == outbound_asmap.end() &&
        map_connection.find(peer_ip) == map_connection.end())
    {
      std::vector<RoundAction> v_RA;
      outbound_connections.push_back(peer_ip);
      outbound_asmap[net->get_group(peer_ip)] = true;
      good(peer_ip, true);
      map_connection[peer_ip] = {{}, {}, false, 1.0, *round, 0, 0, false};
      map_connection[peer_ip].m_addr_token_bucket += MAX_ADDR_PROCESSING_TOKEN_BUCKET;
      map_connection[peer_ip].m_getaddr_sent = true;
      map_connection[peer_ip].m_addr_to_send.clear();
      map_connection[peer_ip].m_next_addr_send =
          *round +
          rrandom.exponential_distribution(AVG_ADDRESS_BROADCAST_INTERVAL);
      outbound_asmap[net->get_group(peer_ip)] = true;
      v_RA.push_back({std::max(map_connection[peer_ip].m_next_local_addr_send,
                               *round + 1),
                      get_ip(), peer_ip, LOCAL_ADDR_ANNOUNCE});

      v_RA.push_back({*round + 1, get_ip(), peer_ip, GETADDR});
      connected(peer_ip);

      if (outbound_connections.size() < MAX_OUT_BOUND_CONNECTIONS)
        v_RA.push_back({*round + 1, get_ip(), get_ip(), OUT_OF_CONNECTIONS});

      return v_RA;
    }
    else
    {
      RoundAction RA = {*round + 1, get_ip(), peer_ip, NOVERACKE};
      return {RA};
    }
  }
  if (msg_type == NOVERACKE)
  {
    inbound_connections.erase(std::remove(inbound_connections.begin(),
                                          inbound_connections.end(), peer_ip),
                              inbound_connections.end());
    map_connection.erase(peer_ip);
  }

  if (msg_type == GETADDR)
  {
    auto it_c = map_connection.find(peer_ip);
    if (it_c == map_connection.end() || !it_c->second.is_inbound)
      return {};
    tel_getaddr_served++;
    std::vector<RoundAction> v_RA;
    for (auto &addr : addrs_response_cache)
      push_address(peer_ip, addr);
    schedule_flush(peer_ip, v_RA);
    return v_RA;
  }

  if (msg_type == ADDR)
  {
    std::vector<RoundAction> v_RA;
    if (data_addresses.size() > MAX_ADDR_PROCESSING_TOKEN_BUCKET)
      return {};
    auto it_c = map_connection.find(peer_ip);
    if (it_c == map_connection.end())
      return {}; // connection closed: drop the stale message
    addInfo_C &conn = it_c->second;

    if (conn.m_addr_token_bucket < MAX_ADDR_PROCESSING_TOKEN_BUCKET)
    {
      const auto time_diff =
          std::max<uint64_t>(*round - conn.m_addr_token_timestamp, 0);
      const double increment = (time_diff)*MAX_ADDR_RATE_PER_SECOND;
      conn.m_addr_token_bucket =
          std::min<double>(conn.m_addr_token_bucket + increment,
                           MAX_ADDR_PROCESSING_TOKEN_BUCKET);
    }
    conn.m_addr_token_timestamp = *round;
    auto vAddr = data_addresses;
    // std::shuffle(vAddr.begin(), vAddr.end(), std::mt19937_64(rrandom.uniform_int<uint64_t>(0, UINT64_MAX)));
    for (auto &addr : vAddr)
    {
      if (conn.m_addr_token_bucket < 1.0)
      {
        tel_addr_dropped++;
        continue;
      }
      else
      {
        conn.m_addr_token_bucket -= 1.0;
      }
      // Mark the received address known-for-this-peer (Core's AddAddressKnown
      // in ProcessAddrs), so a later relay from another peer never echoes it
      // back to the sender. Send-side dedup alone missed this case.
      conn.m_addr_known.insert(addr.addr);
      bool _reachable = can_reach(net_of(addr.addr));
      if (addr.timestamp > *round - 60 * 10 && !conn.m_getaddr_sent &&
          vAddr.size() <= 10 /*addr.IsRoutable()*/)
      {
        relay_address(peer_ip, addr, _reachable, v_RA);
      }
      if (_reachable)
        add(addr, peer_ip, 2 * 60 * 60);
    }
    if (vAddr.size() < 1000)
      conn.m_getaddr_sent = false;
    return v_RA;
  }
  if (msg_type == FEELER_S)
  {
    tel_feeler_success++;
    good(peer_ip, true);
  }
  if (msg_type == FEELER_F)
  {
    tel_feeler_fail++;
    attempt(peer_ip, outbound_asmap.size() >= 2);
  }
  if (msg_type == REMOVE_OUTBOUND_CONNECTION)
  {
    auto it_v = std::find(outbound_connections.begin(),
                          outbound_connections.end(), peer_ip);
    auto it_c = map_connection.find(peer_ip);

    if (it_v != outbound_connections.end() && it_c != map_connection.end())
    {
      outbound_connections.erase(it_v);
      map_connection.erase(it_c);
      outbound_asmap.erase(net->get_group(peer_ip));
      return {{*round + 1, get_ip(), get_ip(), OUT_OF_CONNECTIONS}};
    }

    std::cout << "STALE REMOVE_OUTBOUND_CONNECTION " << get_ip() << " <- "
              << peer_ip
              << " vec:" << (it_v != outbound_connections.end())
              << " map:" << (it_c != map_connection.end()) << std::endl;

    // Dangling vector remnant (map entry gone): reclaim the slot so the node
    // can re-connect; the OUT_OF_CONNECTIONS chain tops it back up.
    if (it_v != outbound_connections.end())
    {
      outbound_connections.erase(it_v);
      outbound_asmap.erase(net->get_group(peer_ip));
      return {{*round + 1, get_ip(), get_ip(), OUT_OF_CONNECTIONS}};
    }

    return {};
  }
  if (msg_type == REMOVE_INBOUND_CONNECTION)
  {
    auto it_v = std::find(inbound_connections.begin(),
                          inbound_connections.end(), peer_ip);
    auto it_c = map_connection.find(peer_ip);

    // Normal teardown.
    if (it_v != inbound_connections.end() && it_c != map_connection.end())
    {
      inbound_connections.erase(it_v);
      map_connection.erase(it_c);
    }
    else
    {
      std::cout << "STALE REMOVE_INBOUND_CONNECTION " << get_ip() << " <- "
                << peer_ip
                << " vec:" << (it_v != inbound_connections.end())
                << " map:" << (it_c != map_connection.end()) << std::endl;
      if (it_v != inbound_connections.end())
        inbound_connections.erase(it_v);
    }
  }
  return {};
}
std::vector<RoundAction> Peer::send_message(uint32_t peer_ip,
                                            ActionType msg_type)
{
  std::vector<RoundAction> v_AT;

  if (msg_type == ADDR_TO_SEND)
  {
    auto it_c = map_connection.find(peer_ip);
    if (it_c == map_connection.end())
      return v_AT; // connection closed: pending flush dies
    addInfo_C &conn = it_c->second;
    conn.m_flush_scheduled = false;

    if (!conn.m_addr_to_send.empty())
    {
      for (auto &addr : conn.m_addr_to_send)
        conn.m_addr_known.insert(addr.addr);
      v_AT.push_back({*round + 1, get_ip(), peer_ip, ADDR,
                      std::move(conn.m_addr_to_send)});
      conn.m_addr_to_send.clear();
    }
  }

  return v_AT;
}
void Peer::relay_address(uint32_t origin, Addr_msg addr, bool reachable, std::vector<RoundAction> &out)
{
  std::vector<uint32_t> combined;
  combined.insert(combined.end(), inbound_connections.begin(),
                  inbound_connections.end());
  combined.insert(combined.end(), outbound_connections.begin(),
                  outbound_connections.end());
  combined.erase(std::remove(combined.begin(), combined.end(), origin),
                 combined.end());
  if (combined.empty())
    return;

  const uint64_t hash_addr{CServiceHash(0, 0)(addr.addr)};

  auto current_time{*round};
  const uint64_t time_addr{(static_cast<uint64_t>(current_time) + hash_addr) / ROTATE_ADDR_RELAY_DEST_INTERVAL};
  SipHasher hasher(RANDOMIZER_ID_ADDRESS_RELAY, unique_id);
  hasher.write(hash_addr);
  hasher.write(time_addr);

  unsigned int nRelayNodes = std::min(
      (reachable || (hasher.finalize() & 1)) ? 2u : 1u,
      (unsigned int)combined.size());

  std::array<std::pair<uint64_t, uint32_t>, 2> best{{{0, 0}, {0, 0}}};
  for (auto &id : combined)
  {
    uint64_t hashKey = SipHasher(hasher).write(id).finalize();
    for (unsigned int i = 0; i < nRelayNodes; i++)
    {
      if (hashKey > best[i].first)
      {
        std::copy(best.begin() + i, best.begin() + nRelayNodes - 1, best.begin() + i + 1);
        best[i] = std::make_pair(hashKey, id);
        break;
      }
    }
  }
  for (unsigned int i = 0; i < nRelayNodes && best[i].first != 0; i++)
  {
    push_address(best[i].second, addr);
    schedule_flush(best[i].second, out);
  }
}
void Peer::schedule_flush(uint32_t peer_ip, std::vector<RoundAction> &out)
{
  auto it_c = map_connection.find(peer_ip);
  if (it_c == map_connection.end())
    return;
  addInfo_C &conn = it_c->second;
  if (conn.m_flush_scheduled || conn.m_addr_to_send.empty())
    return;
  conn.m_flush_scheduled = true;
  out.push_back({*round + rrandom.exponential_distribution(
                              AVG_ADDRESS_BROADCAST_INTERVAL),
                 get_ip(), peer_ip, ADDR_TO_SEND});
}
std::vector<RoundAction> Peer::local_addr_announce(uint32_t peer_ip)
{
  std::vector<RoundAction> v_RA;
  auto it_c = map_connection.find(peer_ip);
  if (it_c == map_connection.end())
    return v_RA; // connection closed or NAT: chain dies
  addInfo_C &conn = it_c->second;

  // If we've sent before, clear the filter so our self-announcement goes out
  if (conn.m_next_local_addr_send != 0)
    conn.m_addr_known.clear();
  conn.m_next_local_addr_send =
      *round + rrandom.exponential_distribution(
                   AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL);
  push_address(peer_ip, {*round, get_ip()});
  schedule_flush(peer_ip, v_RA);

  // daily self-chain, same idiom as CACHE_ENTRY_EXPIRATION / RESOLVE_FEELER
  v_RA.push_back(
      {conn.m_next_local_addr_send, get_ip(), peer_ip, LOCAL_ADDR_ANNOUNCE});
  return v_RA;
}

void Peer::push_address(uint32_t peer_ip, Addr_msg addr)
{
  auto it_c = map_connection.find(peer_ip);
  if (it_c == map_connection.end())
    return;
  addInfo_C &conn = it_c->second;

  if (!conn.m_addr_known.contains(addr.addr))
  {
    if (conn.m_addr_to_send.size() >= MAX_ADDR_TO_SEND)
    {
      conn.m_addr_to_send[rrandom.uniform_int<size_t>(
          0, conn.m_addr_to_send.size() - 1)] = addr;
    }
    else
    {
      conn.m_addr_to_send.push_back(addr);
    }
  }
}

bool Peer::good(uint32_t _ip, bool _test_before_evict)
{
  m_last_good = *round;

  auto it = map_info.find(_ip);
  if (it == map_info.end())
    return false;
  auto &peer = it->second;

  // update info
  peer.m_last_success = *round;
  peer.m_last_try = *round;
  peer.n_attempts = 0;

  if (peer.in_tried)
    return false;
  if (peer.n_ref_count == 0)
    return false;

  uint8_t tried_bucket = get_tried_bucket(_ip);
  uint8_t tried_bucket_pos = get_position(_ip, tried_bucket, true);
  if (_test_before_evict &&
      get_entry(true, tried_bucket, tried_bucket_pos) != -1)
  {
    if (tried_collisions.size() < ADDRMAN_SET_TRIED_COLLISION_SIZE)
    {
      tried_collisions.insert(_ip);
    }
    return false;
  }
  else
  {
    make_tried(_ip);
    return true;
  }
}
void Peer::resolve_collisions()
{
  for (std::set<uint32_t>::iterator it = tried_collisions.begin();
       it != tried_collisions.end();)
  {
    uint32_t id_new = *it;

    bool erase_collision = false;
    if (map_info.find(id_new) == map_info.end())
    {
      erase_collision = true;
    }
    else
    {
      addInfo &peer_new = map_info[id_new];
      uint16_t tried_bucket = get_tried_bucket(peer_new.ip);
      uint8_t tried_bucket_pos = get_position(peer_new.ip, tried_bucket, true);

      if (get_entry(true, tried_bucket, tried_bucket_pos) != -1)
      {
        // Get the to-be-evicted address that is being tested

        uint32_t id_old = static_cast<uint32_t>(
            get_entry(true, tried_bucket, tried_bucket_pos));
        addInfo *peer_old = nullptr;
        if (id_old == 0 || map_info.find(id_old) == map_info.end())
        {
          erase_collision = true;
        }
        else
        {
          peer_old = &map_info[id_old];
          const auto current_time{*round};

          // Has successfully connected in last X hours
          if (current_time - peer_old->m_last_success < ADDRMAN_REPLACEMENT)
          {
            erase_collision = true;
          }
          else if (current_time - peer_old->m_last_try <
                   ADDRMAN_REPLACEMENT) // attempted to connect and failed in
                                        // last X hours
          {
            // Give address at least 60 seconds to successfully connect
            if (current_time - peer_old->m_last_try > 60)
            {

              // Replaces an existing address already in the tried table with
              // the new address
              good(id_new, false);
              erase_collision = true;
            }
          }
          else if (current_time - peer_new.m_last_success >
                   ADDRMAN_TEST_WINDOW)
          {
            // If the collision hasn't resolved in some reasonable amount of
            // time, just evict the old entry -- we must not be able to connect
            // to it for some reason.
            good(id_new, false);
            erase_collision = true;
          }
        }
      }
      else
      {
        // Collision is not actually a collision anymore
        good(id_new, false);
        erase_collision = true;
      }
    }
    if (erase_collision)
    {
      tried_collisions.erase(it++);
    }
    else
    {
      it++;
    }
  }
}
std::vector<RoundAction> Peer::resolve_feeler()
{
  std::vector<RoundAction> v_RA;
  int64_t addr = select_tried_collision();
  if (addr == 0)
  {
    // No tried table collisions. Select a new table address for our feeler.
    addr = select(true);
  }
  else if (map_connection.find(static_cast<uint32_t>(addr)) != map_connection.end())
  {
    // If test-before-evict logic would have us connect to a
    // peer that we're already connected to, just mark that
    // address as Good(). We won't be able to initiate the
    // connection anyway, so this avoids inadvertently evicting
    // a currently-connected peer.
    good(static_cast<uint32_t>(addr), true);
    // Select a new table address for our feeler instead.
    addr = select(true);
  }
  if (addr > 0) // -1: nothing to probe this cycle.
    v_RA.push_back({*round + 1, get_ip(), static_cast<uint32_t>(addr), FEELER});

  next_feeler = *round + rrandom.exponential_distribution(FEELER_INTERVAL);
  v_RA.push_back({next_feeler, get_ip(), get_ip(), RESOLVE_FEELER});

  return v_RA;
}

uint32_t Peer::select_tried_collision()
{
  if (tried_collisions.size() == 0)
    return 0;

  std::set<uint32_t>::iterator it = tried_collisions.begin();
  std::advance(it,
               rrandom.uniform_int<uint32_t>(0, tried_collisions.size() - 1));
  uint32_t ip_new = *it;

  if (map_info.find(ip_new) == map_info.end())
  {
    tried_collisions.erase(it);
    return 0;
  }

  uint16_t tried_bucket = get_tried_bucket(ip_new);
  uint8_t tried_bucket_pos = get_position(ip_new, tried_bucket, true);
  int64_t ip_old = get_entry(true, tried_bucket, tried_bucket_pos);

  if (ip_old == -1 || ip_old == 0)
    return 0;

  return ip_old;
}

void Peer::set_n_time(uint64_t _n_time) { n_time = _n_time; }

void Peer::set_previous_stay() { previous_stay = *round - n_time; }

void Peer::clear_outbound_asmap()
{
  outbound_asmap.clear();
}
void Peer::clear_outbound_connections()
{
  outbound_connections.clear();
}
void Peer::clear_inbound_connections()
{
  inbound_connections.clear();
}
void Peer::clear_map_connection()
{
  map_connection.clear();
}

uint64_t Peer::get_previous_stay() const { return previous_stay; }
void Peer::reset_seed_state()
{
  m_seeds_queried = 0;
  m_last_seed_request = *round;
}

void Peer::deserialize(const std::vector<uint8_t> &data, Network_Config &_nc,
                       Net *_net, uint64_t *_round)
{
  size_t offset = 0;

  auto read_compact = [&data, &offset]<typename T>() -> T
  {
    uint64_t result = 0;
    int shift = 0;

    while (offset < data.size())
    {
      uint8_t byte = data[offset++];
      result |= static_cast<uint64_t>(byte & 0x7F) << shift;

      if ((byte & 0x80) == 0)
      {
        break;
      }
      shift += 7;
    }
    return static_cast<T>(result);
  };

  auto read_raw = [&data, &offset](void *dest, size_t size)
  {
    std::memcpy(dest, &data[offset], size);
    offset += size;
  };

  // Initialize base class and pointers
  static_cast<Structures &>(*this) = Structures(_nc);
  net = _net;
  round = _round;

  // === Peer basic info ===
  ip = read_compact.template operator()<uint32_t>();
  unique_id = read_compact.template operator()<uint32_t>();
  asmap = read_compact.template operator()<uint16_t>();
  source = read_compact.template operator()<uint32_t>();
  n_time = read_compact.template operator()<uint64_t>();
  n_new = read_compact.template operator()<uint64_t>();
  n_tried = read_compact.template operator()<uint64_t>();
  read_raw(&reachable_nets, sizeof(reachable_nets));
  read_raw(&listening_nets, sizeof(listening_nets));
  m_last_good = read_compact.template operator()<uint64_t>();
  out_of_connection_attempt = read_compact.template operator()<uint64_t>();
  next_feeler = read_compact.template operator()<uint64_t>();
  cache_entry_expiration = read_compact.template operator()<uint64_t>();
  previous_stay = read_compact.template operator()<uint64_t>();

  // === vvTried ===
  init_tables();

  size_t vvTried_size = read_compact.template operator()<size_t>();
  for (size_t i = 0; i < vvTried_size; ++i)
  {
    uint8_t bucket_id = read_compact.template operator()<uint8_t>();
    size_t positions_size = read_compact.template operator()<size_t>();
    for (size_t j = 0; j < positions_size; ++j)
    {
      uint8_t pos = read_compact.template operator()<uint8_t>();
      uint32_t ip_val = read_compact.template operator()<uint32_t>();
      slot_set(true, bucket_id, pos, ip_val);
    }
  }

  // === vvNew ===
  size_t vvNew_size = read_compact.template operator()<size_t>();
  for (size_t i = 0; i < vvNew_size; ++i)
  {
    uint16_t bucket_id = read_compact.template operator()<uint16_t>();
    size_t positions_size = read_compact.template operator()<size_t>();
    for (size_t j = 0; j < positions_size; ++j)
    {
      uint8_t pos = read_compact.template operator()<uint8_t>();
      uint32_t ip_val = read_compact.template operator()<uint32_t>();
      slot_set(false, bucket_id, pos, ip_val);
    }
  }

  // === map_info ===
  map_info.clear();
  size_t map_info_size = read_compact.template operator()<size_t>();
  for (size_t i = 0; i < map_info_size; ++i)
  {
    addInfo ai;
    ai.ip = read_compact.template operator()<uint32_t>();
    ai.asmap = read_compact.template operator()<uint16_t>();
    ai.source = read_compact.template operator()<uint32_t>();
    ai.first_seen = read_compact.template operator()<uint32_t>();
    ai.n_time = read_compact.template operator()<uint32_t>();
    ai.m_last_success = read_compact.template operator()<uint32_t>();
    ai.m_last_try = read_compact.template operator()<uint32_t>();
    ai.n_attempts = read_compact.template operator()<uint16_t>();
    ai.n_ref_count = read_compact.template operator()<uint8_t>();
    read_raw(&ai.in_tried, sizeof(ai.in_tried));
    ai.m_last_count_attempt = read_compact.template operator()<uint32_t>();
    map_info[ai.ip] = ai;
  }

  // === v_randoms ===
  v_randoms.clear();
  size_t v_randoms_size = read_compact.template operator()<size_t>();
  for (size_t i = 0; i < v_randoms_size; ++i)
  {
    v_randoms.push_back(read_compact.template operator()<uint32_t>());
  }
  for (size_t i = 0; i < v_randoms.size(); ++i)
    if (auto it_r = map_info.find(v_randoms[i]); it_r != map_info.end())
      it_r->second.n_random_pos = static_cast<uint32_t>(i);

  // === outbound_connections ===
  outbound_connections.clear();
  size_t outbound_size = read_compact.template operator()<size_t>();
  for (size_t i = 0; i < outbound_size; ++i)
  {
    outbound_connections.push_back(
        read_compact.template operator()<uint32_t>());
  }

  // === inbound_connections ===
  inbound_connections.clear();
  size_t inbound_size = read_compact.template operator()<size_t>();
  for (size_t i = 0; i < inbound_size; ++i)
  {
    inbound_connections.push_back(read_compact.template operator()<uint32_t>());
  }

  // === tried_collisions ===
  tried_collisions.clear();
  size_t collisions_size = read_compact.template operator()<size_t>();
  for (size_t i = 0; i < collisions_size; ++i)
  {
    tried_collisions.insert(read_compact.template operator()<uint32_t>());
  }

  // === outbound_asmap ===
  outbound_asmap.clear();
  size_t outbound_asmap_size = read_compact.template operator()<size_t>();
  for (size_t i = 0; i < outbound_asmap_size; ++i)
  {
    uint16_t as_id = read_compact.template operator()<uint16_t>();
    bool val;
    read_raw(&val, sizeof(val));
    outbound_asmap[as_id] = val;
  }

  // === addrs_response_cache ===
  addrs_response_cache.clear();
  size_t cache_size = read_compact.template operator()<size_t>();
  for (size_t i = 0; i < cache_size; ++i)
  {
    uint64_t timestamp = read_compact.template operator()<uint64_t>();
    uint32_t addr = read_compact.template operator()<uint32_t>();
    addrs_response_cache.push_back({timestamp, addr});
  }

  // === map_connection ===
  map_connection.clear();
  size_t conn_size = read_compact.template operator()<size_t>();
  for (size_t i = 0; i < conn_size; ++i)
  {
    uint32_t conn_ip = read_compact.template operator()<uint32_t>();
    addInfo_C c;
    read_raw(&c.m_getaddr_sent, sizeof(c.m_getaddr_sent));
    read_raw(&c.m_addr_token_bucket, sizeof(c.m_addr_token_bucket));
    c.m_addr_token_timestamp = read_compact.template operator()<uint64_t>();
    c.m_next_local_addr_send = read_compact.template operator()<uint64_t>();
    c.m_next_addr_send = read_compact.template operator()<uint64_t>();
    read_raw(&c.is_inbound, sizeof(c.is_inbound));

    // m_addr_known
    size_t known_size = read_compact.template operator()<size_t>();
    for (size_t j = 0; j < known_size; ++j)
    {
      c.m_addr_known.insert(read_compact.template operator()<uint32_t>());
    }

    // m_addr_to_send
    size_t to_send_size = read_compact.template operator()<size_t>();
    for (size_t j = 0; j < to_send_size; ++j)
    {
      uint64_t timestamp = read_compact.template operator()<uint64_t>();
      uint32_t addr = read_compact.template operator()<uint32_t>();
      c.m_addr_to_send.push_back({timestamp, addr});
    }

    map_connection[conn_ip] = c;
  }

  // === RRandom state ===
  size_t rng_size = read_compact.template operator()<size_t>();
  std::vector<uint64_t> rng_state;
  for (size_t i = 0; i < rng_size; ++i)
  {
    rng_state.push_back(read_compact.template operator()<uint64_t>());
  }
  rrandom.set_state(rng_state);
}

std::vector<uint8_t> Peer::serialize()
{
  std::vector<uint8_t> buffer;

  auto append_compact = [&buffer](auto value)
  {
    auto encoded = serialization::make_compact(value);
    buffer.insert(buffer.end(), encoded.begin(), encoded.end());
  };

  auto append_raw = [&buffer](const void *data, size_t size)
  {
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
    buffer.insert(buffer.end(), bytes, bytes + size);
  };

  // === Peer basic info ===
  append_compact(ip);
  append_compact(unique_id);
  append_compact(asmap);
  append_compact(source);
  append_compact(n_time);
  append_compact(n_new);
  append_compact(n_tried);
  append_raw(&reachable_nets, sizeof(reachable_nets));
  append_raw(&listening_nets, sizeof(listening_nets));
  append_compact(m_last_good);
  append_compact(out_of_connection_attempt);
  append_compact(next_feeler);
  append_compact(cache_entry_expiration);
  append_compact(previous_stay);

  // === vvTried ===
  // Format: total_buckets, then for each bucket: bucket_id, num_positions,
  // (pos, ip) pairs
  append_compact(tried_nonempty.size());
  for (const uint16_t b : tried_nonempty)
  {
    append_compact(static_cast<uint8_t>(b));            // bucket id
    append_compact(static_cast<size_t>(tried_fill[b])); // occupied slots
    for (size_t pos = 0; pos < static_cast<size_t>(BUCKET_SIZE); ++pos)
    {
      const uint32_t ip_val = vvTried[static_cast<size_t>(b) * BUCKET_SIZE + pos];
      if (ip_val == 0)
        continue;
      append_compact(static_cast<uint8_t>(pos)); // position
      append_compact(ip_val);                    // ip
    }
  }

  // === vvNew ===
  // Format: total_buckets, then for each bucket: bucket_id, num_positions,
  // (pos, ip) pairs
  append_compact(new_nonempty.size());
  for (const uint16_t b : new_nonempty)
  {
    append_compact(b);                                // bucket id (uint16_t)
    append_compact(static_cast<size_t>(new_fill[b])); // occupied slots
    for (size_t pos = 0; pos < static_cast<size_t>(BUCKET_SIZE); ++pos)
    {
      const uint32_t ip_val = vvNew[static_cast<size_t>(b) * BUCKET_SIZE + pos];
      if (ip_val == 0)
        continue;
      append_compact(static_cast<uint8_t>(pos)); // position
      append_compact(ip_val);                    // ip
    }
  }

  // === map_info ===
  append_compact(map_info.size());
  for (const auto &info_pair : map_info)
  {
    const addInfo &ai = info_pair.second;
    append_compact(ai.ip);
    append_compact(ai.asmap);
    append_compact(ai.source);
    append_compact(ai.first_seen);
    append_compact(ai.n_time);
    append_compact(ai.m_last_success);
    append_compact(ai.m_last_try);
    append_compact(ai.n_attempts);
    append_compact(ai.n_ref_count);
    append_raw(&ai.in_tried, sizeof(ai.in_tried));
    append_compact(ai.m_last_count_attempt);
  }

  // === v_randoms ===
  append_compact(v_randoms.size());
  for (const auto &ip_val : v_randoms)
  {
    append_compact(ip_val);
  }

  // === outbound_connections ===
  append_compact(outbound_connections.size());
  for (const auto &ip_val : outbound_connections)
  {
    append_compact(ip_val);
  }

  // === inbound_connections ===
  append_compact(inbound_connections.size());
  for (const auto &ip_val : inbound_connections)
  {
    append_compact(ip_val);
  }

  // === tried_collisions ===
  append_compact(tried_collisions.size());
  for (const auto &ip_val : tried_collisions)
  {
    append_compact(ip_val);
  }

  // === outbound_asmap ===
  append_compact(outbound_asmap.size());
  for (const auto &as_pair : outbound_asmap)
  {
    append_compact(as_pair.first);
    append_raw(&as_pair.second, sizeof(as_pair.second));
  }

  // === addrs_response_cache ===
  append_compact(addrs_response_cache.size());
  for (const auto &addr : addrs_response_cache)
  {
    append_compact(addr.timestamp);
    append_compact(addr.addr);
  }

  // === map_connection ===
  append_compact(map_connection.size());
  for (const auto &conn_pair : map_connection)
  {
    append_compact(conn_pair.first); // ip
    const addInfo_C &c = conn_pair.second;
    append_raw(&c.m_getaddr_sent, sizeof(c.m_getaddr_sent));
    append_raw(&c.m_addr_token_bucket, sizeof(c.m_addr_token_bucket));
    append_compact(c.m_addr_token_timestamp);
    append_compact(c.m_next_local_addr_send);
    append_compact(c.m_next_addr_send);
    append_raw(&c.is_inbound, sizeof(c.is_inbound));

    // m_addr_known (flattened across generations)
    size_t known_total = 0;
    for (const auto &g : c.m_addr_known.gens)
      known_total += g.size();
    append_compact(known_total);
    for (const auto &g : c.m_addr_known.gens)
      for (const auto &known_ip : g)
        append_compact(known_ip);

    // m_addr_to_send (vector of Addr_msg)
    append_compact(c.m_addr_to_send.size());
    for (const auto &addr : c.m_addr_to_send)
    {
      append_compact(addr.timestamp);
      append_compact(addr.addr);
    }
  }
  // === RRandom state ===
  std::vector<uint64_t> rng_state = rrandom.get_state();
  append_compact(rng_state.size());
  for (const auto &val : rng_state)
  {
    append_compact(val);
  }

  return buffer;
}

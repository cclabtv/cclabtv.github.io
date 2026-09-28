#include <core.h>
static constexpr int NAT_TAU_SEC = (int)(3.06 * 86400);
Core::Core(Net _net, int _seed, Network_Config _nc, uint8_t _debug)
    : net(_net), rrandom(_seed), nc(_nc), debug(_debug)
{
  // 8 DNS seed identities spanning exactly 6 ASes (measured from mainnet).
  std::set<uint16_t> seed_ases;
  while (seed_ases.size() < 6)
    seed_ases.insert(net.get_as_map(rrandom.uniform_int<uint32_t>(1, MAX_TRY_IPS - 1)));

  std::vector<uint16_t> as_list(seed_ases.begin(), seed_ases.end());
  for (int i = 0; i < NUM_DNS_SEEDS; i++)
  {
    bucket b = net.get_bucket_by_id(as_list[i % 6]); // seeds 6,7 share the first two ASes
    dns_seed_ips.push_back(
        make_addr(NET_IPV4, rrandom.uniform_int<uint32_t>(b.min, b.max)));
  }
}
Core::~Core()
{
  {
    std::unique_lock<std::mutex> lk(m_pool_mtx);
    m_pool_stop = true;
  }
  m_pool_cv.notify_all();
  for (auto &t : m_pool)
    if (t.joinable())
      t.join();
}

bool Core::should_go_dormant(uint64_t /*time_online*/)
{
  return true;
}

std::vector<uint32_t> Core::get_dns(uint32_t _max_peers)
{
  // Real seeders only publish nodes that answered a connection, so a
  // behind-NAT peer must never appear in a seed response.
  std::vector<uint32_t> listening;
  listening.reserve(live_addresses.size());
  for (const auto &ip : live_addresses)
  {
    auto it = network.find(ip);
    if (it != network.end() && it->second->is_listening())
      listening.push_back(ip);
  }
  _max_peers = std::min(static_cast<uint32_t>(listening.size()), _max_peers);
  return rrandom.random_subset(listening, _max_peers);
}

uint64_t Core::get_round() { return round; }

uint32_t Core::session_duration(bool reachable)
{
  double mean_days;
  if (!reachable)
    mean_days = nc.nat_session_days; // transient
  else if (rrandom.uniform_double() < nc.public_core_fraction)
    mean_days = nc.public_core_days; // long-lived backbone
  else
    mean_days = nc.public_session_days; // typical public
  int mean_sec = std::max(1, (int)(mean_days * 86400.0));
  return std::max<uint32_t>(1, (uint32_t)rrandom.exponential_distribution(mean_sec));
}

void Core::increment_round() { round++; }

std::pair<uint32_t, uint32_t> Core::insert_peers()
{
  int n_insert = 0;
  int n_scheduled = 0;

  while (!scheduled_inserting.empty())
  {
    auto &action = scheduled_inserting.top();

    if (action <= get_round())
    {
      scheduled_inserting.pop();
      n_insert++;
    }
    else
    {
      break;
    }
  }

  for (int i = 0; i < n_insert; i++)
  {
    double prob_returning = 0.6;
    if ((!dormant_peers.empty() || !dormant_cold.empty()) && rrandom.uniform_double() < prob_returning)
    {
      if (!reactivate_dormant_peer())
        insert_new_peer();
    }
    else
      insert_new_peer();
  }

  if (get_round() >= next_nodes_insertion_check_time)
  {
    int k = rrandom.poisson_distribution(nc.join_node_number);
    auto scheduled_time =
        rrandom.m_s_uniform_distribution(k, nc.join_node_time);
    n_scheduled = scheduled_time.size();

    for (auto &t : scheduled_time)
      scheduled_inserting.push(get_round() + t);

    next_nodes_insertion_check_time = get_round() + nc.join_node_time;
  }
  return {n_insert, n_scheduled};
}
bool Core::reactivate_dormant_peer()
{
  size_t n_hot = dormant_peers.size();
  size_t n_total = n_hot + dormant_cold.size();
  if (n_total == 0)
    return false;
  size_t selected = rrandom.uniform_int<size_t>(0, n_total - 1);

  std::shared_ptr<Peer> peer;
  if (selected < n_hot)
  {
    peer = dormant_peers[selected];
    dormant_peers.erase(dormant_peers.begin() + selected);
  }
  else
  {
    size_t j = selected - n_hot;
    peer = load_cold(dormant_cold[j].ip); // read + delete file
    dormant_cold.erase(dormant_cold.begin() + j);
    if (!peer)
      return false;
  }
  peer->set_n_time(round);
  peer->reset_seed_state();

  uint32_t peer_ip = peer->get_ip();
  uint16_t peer_as = peer->get_asmap();

  network[peer_ip] = peer;
  if (peer->is_listening())
    ++reachable_count;
  address_in_use[peer_ip] = true;
  uint32_t uid = peer->get_unique_id();
  id_to_ip[uid] = peer_ip;
  live_addresses.push_back(peer_ip);
  asmap[peer_as]++;

  uint64_t new_duration = session_duration(peer->is_listening());

  // Schedule churn and actions
  scheduled_churn.push({round + new_duration, uid});
  scheduled_actions.push({round + 1, peer_ip, peer_ip, OUT_OF_CONNECTIONS});
  scheduled_actions.push(
      {peer->cache_entry_expiration, peer_ip, peer_ip, CACHE_ENTRY_EXPIRATION});
  scheduled_actions.push({peer->next_feeler, peer_ip, peer_ip, RESOLVE_FEELER});
  if (debug >= 1)
  {
    std::cout << "Reactivated dormant peer: " << peer_ip << std::endl;
  }
  if (!peer->is_listening()) // NAT peers keep rotating their IP after returning
    scheduled_ip_change.push({round + rrandom.exponential_distribution(NAT_TAU_SEC), peer_ip});
  return true;
}
void Core::spill_cold(const std::shared_ptr<Peer> &peer)
{
  save_binary(std::to_string(peer->get_ip()), peer->serialize(), "dormant_cold");
  dormant_cold.push_back({peer->get_ip(), peer->get_previous_stay()});
}

std::shared_ptr<Peer> Core::load_cold(uint32_t ip)
{
  std::vector<uint8_t> data =
      load_binary(std::to_string(ip), direction + "/dormant_cold");
  if (data.empty())
  {
    std::error_code ec;
    std::filesystem::remove(
        direction + "/dormant_cold/" + std::to_string(ip) + ".bin", ec);
    return nullptr;
  }
  auto peer = std::make_shared<Peer>(0, 0, nc, rrandom, ip, 0, 0, &net, &round,
                                     std::vector<Addr_msg>{}, 0);
  peer->deserialize(data, nc, &net, &round);
  std::filesystem::remove(direction + "/dormant_cold/" + std::to_string(ip) + ".bin");
  return peer;
}

uint32_t Core::change_peer_ip()
{
  std::vector<uint32_t> ips_to_changed;
  while (!scheduled_ip_change.empty())
  {
    auto &peer = scheduled_ip_change.top();
    if (peer.execute_time <= get_round())
    {
      ips_to_changed.push_back(peer.node_id);
      scheduled_ip_change.pop();
    }
    else
    {
      break;
    }
  }
  for (auto &p : ips_to_changed)
  {
    if (network.find(p) != network.end()) // peer could be delete and the information is to stall
      change_peer_ip(p);
  }
  return ips_to_changed.size();
}
void Core::change_peer_ip(uint32_t peer_ip)
{
  bool issue = false;
  auto it_network = network.find(peer_ip);
  auto it_in_use = address_in_use.find(peer_ip);
  auto it_live_nodes = std::find(live_addresses.begin(), live_addresses.end(), peer_ip);
  if (it_network == network.end() || it_in_use == address_in_use.end() ||
      it_live_nodes == live_addresses.end() ||
      asmap.find(net.get_group(peer_ip)) == asmap.end())
  {
    issue = true;
  }
  if (!issue)
  {
    if (debug >= 1)
    {
      std::cout << "change ip peer :" << peer_ip << std::endl;
    }

    auto peer = it_network->second;
    // unique_id is the stable identity across an IP change: the churn entry is
    // keyed by uid and stays untouched, so we must NOT rebuild scheduled_churn
    // We only repoint id_to_ip below so churn resolves to the new address.
    uint32_t uid = peer->get_unique_id();
    uint16_t peer_as = peer->get_asmap();

    std::vector<RoundAction> v_RA;
    // Disconnect from the network and make the edges free
    v_RA = peer->disconnect(false);
    for (auto &addr : v_RA)
      scheduled_actions.push(addr);
    peer->clear_outbound_asmap();
    peer->clear_outbound_connections();
    peer->clear_inbound_connections();
    peer->clear_map_connection();
    // Log the old address-lifetime: the crawler sees old_ip vanish here, with
    // this addrman snapshot as its last observed state.
    v_nodes.push_back({uid, peer_ip, peer->is_listening(),
                       peer->get_n_time(), round, peer_as,
                       peer->get_map_info_size(), peer->get_n_new(),
                       peer->get_n_tried()});
    info inf;
    inf.source = peer_ip;
    for (auto &it_peer_info : peer->get_map_info())
    {
      const auto &ai = it_peer_info.second;
      inf.peers.push_back({ai.first_seen, ai.ip, ai.in_tried, ai.n_attempts,
                           ai.m_last_success, ai.n_ref_count});
    }
    v_infos.push_back(inf);

    network.erase(it_network);
    address_in_use.erase(it_in_use);
    live_addresses.erase(it_live_nodes);

    asmap[peer_as]--;

    uint32_t new_ip = peer_ip;
    const Network pnet = peer->my_net();
    const bool routable_as = (pnet == NET_IPV4);
    bool same_asmap = (!routable_as) || (rrandom.uniform_double() >= 0.05);
    bool is_not_reserved = false;
    uint16_t attempts = 0;
    do
    {
      uint32_t offset;
      if (same_asmap && routable_as)
      {
        bucket b = net.get_bucket_by_id(peer_as);
        offset = rrandom.uniform_int<uint32_t>(b.min, b.max);
      }
      else
      {
        offset = rrandom.uniform_int<uint32_t>(1, MAX_TRY_IPS - 1);
      }
      new_ip = make_addr(pnet, offset);
      uint16_t new_asmap = net.get_group(new_ip);
      bool asmap_ok = same_asmap ? (new_asmap == peer_as) : (new_asmap != peer_as);
      attempts++;
      is_not_reserved =
          new_ip != 0 && new_ip != peer_ip &&
          std::find(dns_seed_ips.begin(), dns_seed_ips.end(), new_ip) == dns_seed_ips.end() &&
          (address_in_use.find(new_ip) == address_in_use.end() || address_in_use[new_ip] == false) &&
          (network.find(new_ip) == network.end()) &&
          std::none_of(dormant_peers.begin(), dormant_peers.end(),
                       [new_ip](const auto &p)
                       { return p->get_ip() == new_ip; }) &&
          std::none_of(dormant_cold.begin(), dormant_cold.end(),
                       [new_ip](const DormantCold &c)
                       { return c.ip == new_ip; }) &&
          asmap_ok;
    } while (!is_not_reserved && attempts <= 1000);
    if (is_not_reserved)
    {
      peer->set_ip(new_ip);
      peer->set_asmap(net.get_group(new_ip));
    }
    else
    {
      new_ip = peer_ip;
    }
    uint16_t new_as = net.get_group(new_ip);
    peer->set_n_time(round);
    address_in_use[new_ip] = true;
    live_addresses.push_back(new_ip);
    network[new_ip] = peer;
    asmap[new_as]++;
    id_to_ip[uid] = new_ip;
    v_ip_changes.push_back({round, peer_ip, new_ip, peer_as, new_as, uid});

    scheduled_actions.push({round + 1, new_ip, new_ip, OUT_OF_CONNECTIONS});
    scheduled_actions.push({network[new_ip]->cache_entry_expiration, new_ip,
                            new_ip, CACHE_ENTRY_EXPIRATION});
    scheduled_actions.push(
        {network[new_ip]->next_feeler, new_ip, new_ip, RESOLVE_FEELER});
    scheduled_ip_change.push({round + rrandom.exponential_distribution(NAT_TAU_SEC), new_ip});
  }
}
static Network draw_network(const std::array<NetProfile, NET_MAX> &p, RRandom &r)
{
  double total = 0.0;
  for (int n = 1; n < NET_MAX; ++n)
    total += p[n].share;
  double u = r.uniform_double() * total, acc = 0.0;
  for (int n = 1; n < NET_MAX; ++n)
    if (acc += p[n].share, u < acc)
      return static_cast<Network>(n);
  return NET_IPV4;
}
void Core::insert_new_peer()
{

  uint32_t peer_ip;
  std::vector<uint32_t> t_uniform_v_seed;
  std::vector<Addr_msg> uniform_v_seed;
  bool is_not_reserved = false;
  Network my_net = draw_network(nc.networks, rrandom);
  do
  {
    peer_ip = make_addr(my_net, rrandom.uniform_int<uint32_t>(1, MAX_TRY_IPS - 1));
    is_not_reserved =
        std::find(dns_seed_ips.begin(), dns_seed_ips.end(), peer_ip) == dns_seed_ips.end() &&
        (address_in_use.find(peer_ip) == address_in_use.end() || address_in_use[peer_ip] == false) &&
        (network.find(peer_ip) == network.end()) &&
        std::none_of(dormant_peers.begin(), dormant_peers.end(),
                     [peer_ip](const auto &p)
                     { return p->get_ip() == peer_ip; }) &&
        std::none_of(dormant_cold.begin(), dormant_cold.end(),
                     [peer_ip](const DormantCold &c)
                     { return c.ip == peer_ip; });
  } while (!is_not_reserved);

  NetSet reach = net_bit(my_net), listen = 0; // own network always reachable
  for (int n = 1; n < NET_MAX; ++n)
  {
    if (nc.networks[n].share <= 0.0 || n == my_net)
      continue;
    if (rrandom.uniform_double() < nc.networks[n].reach)
      reach = net_add(reach, static_cast<Network>(n));
  }
  if (rrandom.uniform_double() < nc.networks[my_net].listen)
    listen = net_add(listen, my_net);

  uint32_t seed_source =
      dns_seed_ips[rrandom.uniform_int<size_t>(0, NUM_DNS_SEEDS - 1)];
  t_uniform_v_seed = get_dns(ADDRS_PER_SEED);
  for (auto &_ip : t_uniform_v_seed)
    uniform_v_seed.push_back({round, _ip});
  uint32_t duration = session_duration(listen != 0);
  uint16_t as = net.get_group(peer_ip);
  address_in_use[peer_ip] = true;
  live_addresses.push_back(peer_ip);
  asmap[as]++;
  network[peer_ip] = std::make_shared<Peer>(reach, listen, nc, rrandom, peer_ip, as,
                                            seed_source, &net, &round, uniform_v_seed,
                                            next_unique_id++);
  if (listen != 0)
    ++reachable_count;
  uint32_t uid = network[peer_ip]->get_unique_id();
  id_to_ip[uid] = peer_ip;
  scheduled_churn.push({round + duration, uid});
  scheduled_actions.push({round + 1, peer_ip, peer_ip, OUT_OF_CONNECTIONS});
  scheduled_actions.push({network[peer_ip]->cache_entry_expiration, peer_ip,
                          peer_ip, CACHE_ENTRY_EXPIRATION});
  scheduled_actions.push(
      {network[peer_ip]->next_feeler, peer_ip, peer_ip, RESOLVE_FEELER});
  if (listen == 0) // NAT/non-listening peers rotate IP (dynamic residential)
    scheduled_ip_change.push({round + rrandom.exponential_distribution(NAT_TAU_SEC), peer_ip});
}

uint32_t Core::remove_peers()
{
  std::vector<uint32_t> peers_to_removed;
  while (!scheduled_churn.empty())
  {
    auto &peer = scheduled_churn.top();
    if (peer.execute_time <= get_round())
    {
      peers_to_removed.push_back(peer.node_id);
      scheduled_churn.pop();
    }
    else
    {
      break;
    }
  }
  for (auto &uid : peers_to_removed)
  {
    auto it = id_to_ip.find(uid);
    if (it == id_to_ip.end())
      continue;
    uint32_t ip = it->second;
    id_to_ip.erase(it);
    remove_peer(ip);
  }

  return peers_to_removed.size();
}
void Core::remove_peer(uint32_t peer_ip)
{
  bool issue = false;
  auto it_network = network.find(peer_ip);
  auto it_in_use = address_in_use.find(peer_ip);
  auto it_live_nodes = std::find(live_addresses.begin(), live_addresses.end(), peer_ip);
  if (it_network == network.end() || it_in_use == address_in_use.end() ||
      it_live_nodes == live_addresses.end() ||
      asmap.find(net.get_group(peer_ip)) == asmap.end())
  {
    issue = true;
  }
  if (!issue)
  {
    if (debug >= 1)
    {
      std::cout << "remove peer :" << peer_ip << std::endl;
    }
    auto peer = it_network->second;
    uint16_t peer_as = peer->get_asmap();
    std::vector<RoundAction> v_RA;
    if (should_go_dormant(round - peer->get_n_time()))
    {
      v_RA = peer->disconnect(true);
      dormant_peers.push_back(peer);
      if (nc.dormant_cap > 0)
      {
        spill_cold(dormant_peers.front());
        dormant_peers.erase(dormant_peers.begin());
      }
    }
    else
    {
      v_RA = peer->disconnect(false);
    }
    v_nodes.push_back({peer->get_unique_id(), peer_ip, peer->is_listening(),
                       peer->get_n_time(), round, peer->get_asmap(),
                       peer->get_map_info_size(), peer->get_n_new(),
                       peer->get_n_tried()});
    info inf;
    inf.source = peer_ip;
    for (auto &it_peer_info : peer->get_map_info())
    {
      const auto &ai = it_peer_info.second;
      inf.peers.push_back({ai.first_seen, ai.ip, ai.in_tried, ai.n_attempts,
                           ai.m_last_success, ai.n_ref_count});
    }
    v_infos.push_back(inf);

    if (peer->is_listening())
      --reachable_count;
    network.erase(it_network);
    address_in_use.erase(it_in_use);
    live_addresses.erase(it_live_nodes);
    asmap[peer_as]--;
    for (auto &addr : v_RA)
      scheduled_actions.push(addr);
  }
  else
  {
    std::cout << "Warning: Peer " << peer_ip
              << " scheduled for removal but not found" << std::endl;
    save_all_data();

    exit(0);
  }
}

void Core::start_pool()
{
  const char *env = std::getenv("SIM_THREADS");
  int n = env ? atoi(env) : static_cast<int>(std::thread::hardware_concurrency());
  m_num_threads = static_cast<unsigned int>(std::max(1, n));
  std::cout << "Worker threads: " << m_num_threads << std::endl;

  for (unsigned int t = 1; t < m_num_threads; ++t) // t=0 runs on the main thread
  {
    m_pool.emplace_back([this, t]()
                        {
      uint64_t local_gen = 0;
      for (;;)
      {
        std::unique_lock<std::mutex> lk(m_pool_mtx);
        m_pool_cv.wait(lk, [&] { return m_pool_stop || m_pool_gen != local_gen; });
        if (m_pool_stop)
          return;
        local_gen = m_pool_gen;
        lk.unlock();

        m_task(t);

        lk.lock();
        if (--m_pool_remaining == 0)
          m_pool_done_cv.notify_one();
      } });
  }
}

void Core::run_pool(const std::function<void(size_t)> &task)
{
  if (m_num_threads <= 1)
  {
    task(0);
    return;
  }
  {
    std::unique_lock<std::mutex> lk(m_pool_mtx);
    m_task = task;
    m_pool_remaining = m_num_threads;
    ++m_pool_gen;
  }
  m_pool_cv.notify_all();

  task(0); // main thread handles share 0

  std::unique_lock<std::mutex> lk(m_pool_mtx);
  if (--m_pool_remaining == 0)
    m_pool_done_cv.notify_one();
  m_pool_done_cv.wait(lk, [&]
                      { return m_pool_remaining == 0; });
}

void Core::spark_next_round()
{
  if (!m_pool_started)
  {
    start_pool();
    m_pool_started = true;
  }
  unsigned int num_threads = m_num_threads;
  scheduled_actions.reflow();
  std::vector<RoundAction> current_actions = scheduled_actions.take_due(round);
  acc_operations += current_actions.size();

  std::unordered_map<uint32_t, DnsResult> dns_results;
  for (auto &action : current_actions)
  {
    if (action.action == DNS_REQUEST)
    {
      // Each request gets its own shuffle, advancing rrandom deterministically
      uint64_t day = 24 * 60 * 60;
      uint32_t source =
          dns_seed_ips[rrandom.uniform_int<size_t>(0, NUM_DNS_SEEDS - 1)];
      uint64_t penalty = rrandom.uniform_int<uint64_t>(3 * day, 7 * day);
      dns_results[action.reciver] = {source, penalty, get_dns(ADDRS_PER_SEED)};
    }
  }
  if (get_round() % 1000 == 0)
  {
    std::cout << " Total operation : " << acc_operations << std::endl;
    acc_operations = 0;
  }

  // Partition actions by target peer so no two threads lock the same peer
  auto get_target_peer = [](const RoundAction &action) -> uint32_t
  {
    switch (action.action)
    {
    case OUT_OF_CONNECTIONS:
    case RESOLVE_FEELER:
    case LOCAL_ADDR_ANNOUNCE:
    case ADDR_TO_SEND:
      return action.sender;
    default:
      return action.reciver;
    }
  };

  // Group action indices by target peer, preserving priority queue order
  // Partition by target peer via a sorted (peer, action_index) array instead of
  // a per-round unordered_map: byte-identical grouping (peers ascending, each
  // peer's actions in index order, round-robin over threads).
  std::vector<std::pair<uint32_t, size_t>> pa;
  pa.reserve(current_actions.size());
  for (size_t i = 0; i < current_actions.size(); ++i)
    pa.emplace_back(get_target_peer(current_actions[i]), i);
  std::sort(pa.begin(), pa.end());

  std::vector<std::vector<size_t>> thread_work(num_threads);
  size_t group = 0;
  for (size_t i = 0; i < pa.size();)
  {
    size_t tid = group % num_threads;
    const uint32_t peer = pa[i].first;
    while (i < pa.size() && pa[i].first == peer)
      thread_work[tid].push_back(pa[i++].second);
    ++group;
  }

  std::vector<std::vector<RoundAction>> thread_results(num_threads);

  auto worker = [&](size_t thread_id)
  {
    std::vector<RoundAction> &local_actions = thread_results[thread_id];

    for (size_t i : thread_work[thread_id])
    {
      auto &action = current_actions[i];

      if (action.action == ERR)
      {
        save_all_data();
        exit(0);
      }
      if (action.action == OUT_OF_CONNECTIONS)
      {
        if (debug >= 1)

          std::cout << "OUT_OF_CONNECTIONS Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;

        auto peer_it = network.find(action.sender);
        if (peer_it != network.end())
        {
          auto &peer = peer_it->second;
          RoundAction RT;
          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            RT = peer->connect();
          }
          if (RT.action != NONE)
          {
            local_actions.push_back(RT);
          }
        }
      }
      if (action.action == VERSION)
      {
        if (debug >= 1)

          std::cout << "VERSION Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;

        auto peer_it = network.find(action.reciver);
        if (peer_it != network.end())
        {
          auto &peer = peer_it->second;
          bool refuse;
          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            refuse = !peer->accepts_inbound_on(net_of(action.reciver));
          }
          if (refuse)
          {
            local_actions.push_back(
                {round + 1, action.reciver, action.sender, NOVERSION});
            continue;
          }
          std::vector<RoundAction> v_RA;
          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            v_RA = peer->process_message(action.sender, action.action, {});
          }

          for (auto &rt : v_RA)
            local_actions.push_back(rt);
        }
        else
        {
          local_actions.push_back(
              {round + 1, action.reciver, action.sender, NOVERSION});
        }
      }
      if (action.action == NOVERSION)
      {
        if (debug >= 1)

          std::cout << "NOVERSION Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;

        auto peer_it = network.find(action.reciver);
        if (peer_it != network.end())
        {
          auto &peer = peer_it->second;
          std::vector<RoundAction> v_RA;
          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            v_RA = peer->process_message(action.sender, action.action, {});
          }

          for (auto &rt : v_RA)
            local_actions.push_back(rt);
        }
      }
      if (action.action == VERACKE)
      {
        if (debug >= 1)

          std::cout << "VERACKE Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;

        auto peer_it = network.find(action.reciver);
        if (peer_it != network.end())
        {
          auto &peer = peer_it->second;
          bool accepted_connection = false;
          std::vector<RoundAction> v_RA;
          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            v_RA = peer->process_message(action.sender, action.action, {});
          }
          for (auto &rt : v_RA)
          {
            if (rt.action == GETADDR)
              accepted_connection = true;
            local_actions.push_back(rt);
          }

          if (accepted_connection)
          {
            {
              std::lock_guard<std::mutex> lock(edge_mutex);
              v_edges.push_back({round, action.reciver, action.sender, true});
            }
          }
        }
      }
      if (action.action == NOVERACKE)
      {
        if (debug >= 1)
          std::cout << "NOVERACKE Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;

        auto peer_it = network.find(action.reciver);
        if (peer_it != network.end())
        {
          auto &peer = peer_it->second;
          std::vector<RoundAction> v_RA;
          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            v_RA = peer->process_message(action.sender, action.action, {});
          }
          for (auto &rt : v_RA)
            local_actions.push_back(rt);
        }
      }
      if (action.action == GETADDR)
      {
        if (debug >= 1)

          std::cout << "GETADDR Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;

        auto peer_it = network.find(action.reciver);
        if (peer_it != network.end())
        {
          auto &peer = peer_it->second;
          std::vector<RoundAction> v_RA;
          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            v_RA = peer->process_message(action.sender, action.action, {});
          }

          for (auto &rt : v_RA)
            local_actions.push_back(rt);
        }
        else
        {
        }
      }
      if (action.action == ADDR)
      {
        if (debug >= 1)

          std::cout << "ADDR Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;

        auto peer_it = network.find(action.reciver);
        if (peer_it != network.end())
        {
          auto &peer = peer_it->second;
          std::vector<RoundAction> v_RA;
          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            v_RA = peer->process_message(action.sender, action.action,
                                         action.args);
          }

          for (auto &rt : v_RA)
            local_actions.push_back(rt);
        }
      }
      if (action.action == LOCAL_ADDR_ANNOUNCE)
      {
        auto peer_it = network.find(action.sender);
        if (peer_it != network.end())
        {
          auto &peer = peer_it->second;
          std::vector<RoundAction> v_RA;
          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            v_RA = peer->local_addr_announce(action.reciver);
          }
          for (auto &rt : v_RA)
            local_actions.push_back(rt);
        }
      }

      if (action.action == CACHE_ENTRY_EXPIRATION)
      {
        if (debug >= 1)

          std::cout << "CACHE_ENTRY_EXPIRATION Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;

        auto peer_it = network.find(action.reciver);
        if (peer_it != network.end())
        {
          auto &peer = peer_it->second;
          std::pair<RoundAction, cache> result;
          RoundAction RA;
          cache c;
          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            result = peer->get_addresses();
          }

          local_actions.push_back(result.first);

          if (nc.cache_sample_rate <= 1 ||
              peer->get_unique_id() % nc.cache_sample_rate == 0)
          {
            std::lock_guard<std::mutex> lock(cache_mutex);
            v_caches.push_back(std::move(result.second));
          }
        }
      }
      if (action.action == RESOLVE_FEELER)
      {
        if (debug >= 1)
          std::cout << "RESOLVE_FEELER Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;

        auto peer_it = network.find(action.sender);
        if (peer_it != network.end())
        {
          auto &peer = peer_it->second;
          std::vector<RoundAction> v_RA;
          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            v_RA = peer->resolve_feeler();
          }
          for (auto &rt : v_RA)
            local_actions.push_back(rt);
        }
      }
      if (action.action == FEELER)
      {
        if (debug >= 1)

          std::cout << "FEELER Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;

        auto peer_it = network.find(action.reciver);
        if (peer_it == network.end())
        {
          local_actions.push_back(
              {round + 1, action.reciver, action.sender, FEELER_F});
        }
        else
        {
          auto &peer = peer_it->second;
          bool b_nat;
          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            b_nat = !peer->accepts_inbound_on(net_of(action.reciver));
          }
          if (b_nat)
          {
            local_actions.push_back(
                {round + 1, action.reciver, action.sender, FEELER_F});
          }
          else
          {
            local_actions.push_back(
                {round + 1, action.reciver, action.sender, FEELER_S});
            // Reachable - feeler succeeded
          }
        }
      }

      if (action.action == FEELER_F)
      {
        if (debug >= 1)

          std::cout << "FEELER_F Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;

        auto peer_it = network.find(action.reciver);
        if (peer_it != network.end())
        {
          auto &peer = peer_it->second;

          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            peer->process_message(action.sender, FEELER_F, {});
          }
        }
      }
      if (action.action == FEELER_S)
      {
        if (debug >= 1)

          std::cout << "FEELER_S Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;

        auto peer_it = network.find(action.reciver);
        if (peer_it != network.end())
        {
          auto &peer = peer_it->second;
          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            peer->process_message(action.sender, FEELER_S, {});
          }
        }
      }

      if (action.action == ADDR_TO_SEND)
      {
        if (debug >= 1)

          std::cout << "ADDR_TO_SEND Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;

        auto peer_it = network.find(action.sender);
        if (peer_it != network.end())
        {
          std::vector<RoundAction> v_RA;
          auto &peer = peer_it->second;

          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            v_RA = peer->send_message(action.reciver, ADDR_TO_SEND);
          }
          for (auto &rt : v_RA)
            local_actions.push_back(rt);
        }
      }
      if (action.action == DNS_REQUEST)
      {
        if (debug >= 1)

          std::cout << "DNS_REQUEST Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;

        auto peer_it = network.find(action.reciver);
        if (peer_it != network.end())
        {
          auto &peer = peer_it->second;
          auto dns_it = dns_results.find(action.reciver);
          if (dns_it != dns_results.end())
          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            uint64_t aged = round - dns_it->second.penalty;
            for (auto &ip : dns_it->second.addrs)
              peer->add({aged, ip}, dns_it->second.source);
            local_actions.push_back({round + 1, action.reciver, action.reciver, {OUT_OF_CONNECTIONS}});
          }
          else
          {
            std::cout << "DNS_REQUEST : issue in allocate thread" << std::endl;
            save_all_data();

            exit(0);
          }
        }
      }
      if (action.action == REMOVE_OUTBOUND_CONNECTION)
      {
        if (debug >= 1)
        {
          std::cout << "REMOVE_OUTBOUND_CONNECTION Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;
        }
        auto peer_it = network.find(action.reciver);
        if (peer_it != network.end())
        {
          auto &peer = peer_it->second;
          std::vector<RoundAction> v_RA;

          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            v_RA = peer->process_message(action.sender,
                                         REMOVE_OUTBOUND_CONNECTION, {});
          }

          for (auto &rt : v_RA)
            local_actions.push_back(rt);

          {
            std::lock_guard<std::mutex> lock(edge_mutex);
            v_edges.push_back({round, action.reciver, action.sender, false});
          }
        }
      }
      if (action.action == REMOVE_INBOUND_CONNECTION)
      {
        if (debug >= 1)
        {
          std::cout << "REMOVE_INBOUND_CONNECTION Sender : " << action.sender
                    << " Reciver: " << action.reciver << std::endl;
        }
        auto peer_it = network.find(action.reciver);
        if (peer_it != network.end())
        {
          auto &peer = peer_it->second;
          std::vector<RoundAction> v_RA;

          {
            std::lock_guard<std::mutex> lock(peer->peer_mutex);
            v_RA = peer->process_message(action.sender,
                                         REMOVE_INBOUND_CONNECTION, {});
          }
          {
            std::lock_guard<std::mutex> lock(edge_mutex);
            v_edges.push_back({round, action.sender, action.reciver, false});
          }
        }
      }
    }
  };

  run_pool(worker);

  for (auto &local_actions : thread_results)
    for (auto &action : local_actions)
      scheduled_actions.push(std::move(action));
}
void Core::collect_daily_telemetry()
{
  for (auto &[ip, peer] : network)
  {
    if (peer->tel_feeler_success || peer->tel_feeler_fail ||
        peer->tel_getaddr_served || peer->tel_addr_dropped)
      v_telemetry.push_back({round, ip, peer->tel_feeler_success,
                             peer->tel_feeler_fail, peer->tel_getaddr_served,
                             peer->tel_addr_dropped});
    peer->tel_feeler_success = 0;
    peer->tel_feeler_fail = 0;
    peer->tel_getaddr_served = 0;
    peer->tel_addr_dropped = 0;
  }
  if (v_telemetry.size() * sizeof(telemetry) > 10 * 1024 * 1024)
  {
    save_telemetry(v_telemetry);
    v_telemetry.clear();
  }
}

void Core::request_stop() { stop_requested.store(true); }

bool Core::should_stop() const { return stop_requested.load(); }
std::vector<uint8_t> Core::serialize_state()
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

  // === Basic state ===
  append_compact(round);
  append_compact(next_nodes_insertion_check_time);
  append_compact(next_unique_id);

  // === RRandom state ===
  std::vector<uint64_t> rng_state = rrandom.get_state();
  append_compact(rng_state.size());
  for (const auto &val : rng_state)
  {
    append_compact(val);
  }

  // === live_addresses ===
  append_compact(live_addresses.size());
  for (const auto &ip : live_addresses)
  {
    append_compact(ip);
  }

  // === address_in_use ===
  append_compact(address_in_use.size());
  for (const auto &pair : address_in_use)
  {
    append_compact(pair.first);
    append_raw(&pair.second, sizeof(pair.second));
  }

  // === asmap ===
  append_compact(asmap.size());
  for (const auto &pair : asmap)
  {
    append_compact(pair.first);
    append_compact(pair.second);
  }

  // === scheduled_actions ===
  append_compact(scheduled_actions.size());
  scheduled_actions.for_each([&](const RoundAction &action)
                             {
    append_compact(action.execute_time);
    append_compact(action.sender);
    append_compact(action.reciver);
    append_compact(static_cast<uint8_t>(action.action));
    append_compact(action.args.size());
    for (const auto &arg : action.args) {
      append_compact(arg.timestamp);
      append_compact(arg.addr);
    } });

  // === scheduled_inserting ===
  std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>>
      inserting_copy = scheduled_inserting;
  append_compact(inserting_copy.size());
  while (!inserting_copy.empty())
  {
    append_compact(inserting_copy.top());
    inserting_copy.pop();
  }

  // === scheduled_ip_change ===
  std::priority_queue<ChurnAction, std::vector<ChurnAction>,
                      std::greater<ChurnAction>>
      ip_change_copy = scheduled_ip_change;
  append_compact(ip_change_copy.size());
  while (!ip_change_copy.empty())
  {
    const ChurnAction &ip_change = ip_change_copy.top();
    append_compact(ip_change.execute_time);
    append_compact(ip_change.node_id);
    ip_change_copy.pop();
  }
  // === scheduled_churn ===
  std::priority_queue<ChurnAction, std::vector<ChurnAction>,
                      std::greater<ChurnAction>>
      churn_copy = scheduled_churn;
  append_compact(churn_copy.size());
  while (!churn_copy.empty())
  {
    const ChurnAction &churn = churn_copy.top();
    append_compact(churn.execute_time);
    append_compact(churn.node_id);
    churn_copy.pop();
  }

  // === network peer IPs ===
  append_compact(network.size());
  for (const auto &pair : network)
  {
    append_compact(pair.first);
  }

  // === dormant peer IPs ===
  append_compact(dormant_peers.size());
  for (const auto &peer : dormant_peers)
  {
    append_compact(peer->get_ip());
  }

  // === dormant cold (disk-backed) index: ip + previous_stay ===
  append_compact(dormant_cold.size());
  for (const auto &c : dormant_cold)
  {
    append_compact(c.ip);
    append_compact(c.previous_stay);
  }

  return buffer;
}

void Core::save_all_data()
{
  constexpr size_t MB_10 = 10 * 1024 * 1024;
  std::vector<uint8_t> b_1 = serialize_state();
  save_binary("core_state", b_1, "state");
  if (!v_caches.empty())
  {
    save_cache(v_caches);
    v_caches.clear();
  }
  if (!v_edges.empty())
  {
    save_edges(v_edges);
    v_edges.clear();
  }
  if (!v_ip_changes.empty())
  {
    save_ip_changes(v_ip_changes);
    v_ip_changes.clear();
  }
  if (!v_telemetry.empty())
  {
    save_telemetry(v_telemetry);
    v_telemetry.clear();
  }

  for (auto &it_peer : network)
  {
    auto &peer = it_peer.second;
    v_nodes.push_back({peer->get_unique_id(), peer->get_ip(), peer->is_listening(),
                       peer->get_n_time(), round, peer->get_asmap(),
                       peer->get_map_info_size(), peer->get_n_new(),
                       peer->get_n_tried()});
    if (v_nodes.size() * sizeof(node) > MB_10)
    {
      save_nodes(v_nodes);
      v_nodes.clear();
    }
    info i;
    i.source = peer->get_ip();
    for (auto &it_peer_info : peer->get_map_info())
    {
      const auto &ai = it_peer_info.second;
      i.peers.push_back({ai.first_seen, ai.ip, ai.in_tried, ai.n_attempts,
                         ai.m_last_success, ai.n_ref_count});
    }
    v_infos.push_back(i);
    if (v_infos.size() > 50)
    {
      save_infos(v_infos);
      v_infos.clear();
    }
    std::vector<uint8_t> buffer = peer->serialize();
    save_binary(std::to_string(peer->get_ip()), buffer, "state/netwok_peers");
  }

  for (auto &d_peer : dormant_peers)
  {
    auto &peer = d_peer;
    std::vector<uint8_t> buffer = peer->serialize();
    save_binary(std::to_string(peer->get_ip()), buffer, "dormant_peers");
  }
  save_nodes(v_nodes);
  v_nodes.clear();
  save_infos(v_infos);
  v_infos.clear();
}

void Core::maybe_save_data()
{
  constexpr size_t MB_10 = 10 * 1024 * 1024;

  size_t caches_size = 0;
  for (const auto &c : v_caches)
    caches_size += sizeof(c.round) + sizeof(c.source) + sizeof(c.addrman_size) +
                   c.addresses.size() * (sizeof(uint32_t) + sizeof(uint64_t));
  if (caches_size > MB_10)
  {
    save_cache(v_caches);
    v_caches.clear();
  }

  if (v_edges.size() * sizeof(edge) > MB_10)
  {
    save_edges(v_edges);
    v_edges.clear();
  }

  if (v_nodes.size() * sizeof(node) > MB_10)
  {
    save_nodes(v_nodes);
    v_nodes.clear();
  }

  size_t infos_size = 0;
  for (const auto &inf : v_infos)
    infos_size += sizeof(inf.source) +
                  inf.peers.size() * sizeof(std::pair<uint64_t, uint32_t>);
  if (infos_size > MB_10)
  {
    save_infos(v_infos);
    v_infos.clear();
  }
  if (v_ip_changes.size() > 1000)
  {
    save_ip_changes(v_ip_changes);
    v_ip_changes.clear();
  }
}
void Core::backup()
{
  if (round % (24 * 3600 * 30) == 0)
  {
    std::vector<uint8_t> b_ = serialize_state();
    save_binary("core_state", b_, "backup_" + std::to_string(back) + "/state");

    for (auto &it_peer : network)
    {
      auto &peer = it_peer.second;
      std::vector<uint8_t> buffer = peer->serialize();
      save_binary(std::to_string(peer->get_ip()), buffer,
                  "backup_" + std::to_string(back) + "/state/netwok_peers");
    }

    for (auto d_peer : dormant_peers)
    {
      auto &peer = d_peer;
      std::vector<uint8_t> buffer = peer->serialize();
      save_binary(std::to_string(peer->get_ip()), buffer,
                  "backup_" + std::to_string(back) + "/dormant_peers");
    }
    std::filesystem::create_directories(
        direction + "/backup_" + std::to_string(back) + "/dormant_cold");
    for (const auto &c : dormant_cold)
    {
      std::string src = direction + "/dormant_cold/" + std::to_string(c.ip) + ".bin";
      std::string dst = direction + "/backup_" + std::to_string(back) +
                        "/dormant_cold/" + std::to_string(c.ip) + ".bin";
      std::error_code ec;
      std::filesystem::create_hard_link(src, dst, ec);
      if (ec)
        std::filesystem::copy_file(src, dst,
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    std::error_code cec;
    std::filesystem::copy_file(
        direction + "/simulator.conf",
        direction + "/backup_" + std::to_string(back) + "/simulator.conf",
        std::filesystem::copy_options::overwrite_existing, cec);
    if (cec)
      std::cerr << "Warning: backup_" << back << " has no simulator.conf ("
                << cec.message() << ") -- it will NOT be resumable."
                << std::endl;

    back++;
  }
}
void Core::deserialize_state(const std::string &state_path)
{
  std::string file_path = state_path + "/state/core_state.bin";
  std::ifstream file(file_path, std::ios::binary);

  if (!file.is_open())
  {
    std::cerr << "Error: Unable to open core state file: " << file_path
              << std::endl;
    return;
  }

  // === Basic state ===
  round = serialization::read_compact<uint64_t>(file);
  next_nodes_insertion_check_time = serialization::read_compact<uint64_t>(file);
  next_unique_id = serialization::read_compact<uint32_t>(file);

  // === RRandom state ===
  size_t rng_size = serialization::read_compact<size_t>(file);
  std::vector<uint64_t> rng_state;
  for (size_t i = 0; i < rng_size; ++i)
  {
    rng_state.push_back(serialization::read_compact<uint64_t>(file));
  }
  rrandom.set_state(rng_state);

  // === live_addresses ===
  live_addresses.clear();
  size_t live_size = serialization::read_compact<size_t>(file);
  for (size_t i = 0; i < live_size; ++i)
  {
    live_addresses.push_back(serialization::read_compact<uint32_t>(file));
  }

  // === address_in_use ===
  address_in_use.clear();
  size_t is_live_size = serialization::read_compact<size_t>(file);
  for (size_t i = 0; i < is_live_size; ++i)
  {
    uint32_t ip = serialization::read_compact<uint32_t>(file);
    bool val;
    serialization::read_bytes(file, &val, sizeof(val));
    address_in_use[ip] = val;
  }

  // === asmap ===
  asmap.clear();
  size_t asmap_size = serialization::read_compact<size_t>(file);
  for (size_t i = 0; i < asmap_size; ++i)
  {
    uint16_t as_id = serialization::read_compact<uint16_t>(file);
    uint32_t count = serialization::read_compact<uint32_t>(file);
    asmap[as_id] = count;
  }

  // === scheduled_actions ===
  scheduled_actions.clear();
  size_t actions_size = serialization::read_compact<size_t>(file);
  for (size_t i = 0; i < actions_size; ++i)
  {
    RoundAction action;
    action.execute_time = serialization::read_compact<uint64_t>(file);
    action.sender = serialization::read_compact<uint32_t>(file);
    action.reciver = serialization::read_compact<uint32_t>(file);
    action.action =
        static_cast<ActionType>(serialization::read_compact<uint8_t>(file));
    size_t args_size = serialization::read_compact<size_t>(file);
    for (size_t j = 0; j < args_size; ++j)
    {
      uint64_t ts = serialization::read_compact<uint64_t>(file);
      uint32_t addr = serialization::read_compact<uint32_t>(file);
      action.args.push_back({ts, addr});
    }
    scheduled_actions.push(action);
  }

  // === scheduled_inserting ===
  while (!scheduled_inserting.empty())
    scheduled_inserting.pop();
  size_t inserting_size = serialization::read_compact<size_t>(file);
  for (size_t i = 0; i < inserting_size; ++i)
  {
    scheduled_inserting.push(serialization::read_compact<uint64_t>(file));
  }

  // === scheduled_ip_change ===
  while (!scheduled_ip_change.empty())
    scheduled_ip_change.pop();
  size_t ip_change = serialization::read_compact<size_t>(file);
  for (size_t i = 0; i < ip_change; ++i)
  {
    uint64_t exec_time = serialization::read_compact<uint64_t>(file);
    uint32_t node_id = serialization::read_compact<uint32_t>(file);
    scheduled_ip_change.push({exec_time, node_id});
  }
  // === scheduled_churn ===
  while (!scheduled_churn.empty())
    scheduled_churn.pop();
  size_t churn_size = serialization::read_compact<size_t>(file);
  for (size_t i = 0; i < churn_size; ++i)
  {
    uint64_t exec_time = serialization::read_compact<uint64_t>(file);
    uint32_t node_id = serialization::read_compact<uint32_t>(file);
    scheduled_churn.push({exec_time, node_id});
  }

  // === network peer IPs ===
  std::vector<uint32_t> network_ips;
  size_t network_size = serialization::read_compact<size_t>(file);
  for (size_t i = 0; i < network_size; ++i)
  {
    network_ips.push_back(serialization::read_compact<uint32_t>(file));
  }

  // === dormant peer IPs ===
  std::vector<uint32_t> dormant_ips;
  size_t dormant_size = serialization::read_compact<size_t>(file);
  for (size_t i = 0; i < dormant_size; ++i)
  {
    dormant_ips.push_back(serialization::read_compact<uint32_t>(file));
  }

  // === dormant cold (disk-backed) index ===
  std::vector<DormantCold> cold_index;
  size_t cold_size = serialization::read_compact<size_t>(file);
  for (size_t i = 0; i < cold_size; ++i)
  {
    uint32_t cip = serialization::read_compact<uint32_t>(file);
    uint64_t cstay = serialization::read_compact<uint64_t>(file);
    cold_index.push_back({cip, cstay});
  }

  file.close();

  // Load network peers
  network.clear();
  reachable_count = 0;
  for (uint32_t peer_ip : network_ips)
  {
    std::vector<uint8_t> peer_data = load_binary(
        std::to_string(peer_ip), state_path + "/state/netwok_peers");
    if (!peer_data.empty())
    {
      auto peer = std::make_shared<Peer>(0, 0, nc, rrandom, peer_ip, 0, 0, &net,
                                         &round, std::vector<Addr_msg>{}, 0);
      peer->deserialize(peer_data, nc, &net, &round);
      network[peer_ip] = peer;
      if (peer->is_listening())
        ++reachable_count;
    }
  }
  id_to_ip.clear();
  for (auto &[ip, peer] : network)
    id_to_ip[peer->get_unique_id()] = ip;

  // Load dormant peers (hot tier)
  dormant_peers.clear();
  for (uint32_t peer_ip : dormant_ips)
  {
    std::vector<uint8_t> peer_data =
        load_binary(std::to_string(peer_ip), state_path + "/dormant_peers");
    if (!peer_data.empty())
    {
      auto peer = std::make_shared<Peer>(0, 0, nc, rrandom, peer_ip, 0, 0, &net,
                                         &round, std::vector<Addr_msg>{}, 0);
      peer->deserialize(peer_data, nc, &net, &round);
      dormant_peers.push_back(peer);
    }
  }

  dormant_cold.clear();
  std::filesystem::create_directories(direction + "/dormant_cold");
  for (const auto &c : cold_index)
  {
    std::string src = state_path + "/dormant_cold/" + std::to_string(c.ip) + ".bin";
    std::string dst = direction + "/dormant_cold/" + std::to_string(c.ip) + ".bin";
    std::error_code ec;
    std::error_code eq_ec;
    if (std::filesystem::exists(dst) &&
        std::filesystem::equivalent(src, dst, eq_ec) && !eq_ec)
    {
      dormant_cold.push_back(c);
      continue;
    }
    std::filesystem::create_hard_link(src, dst, ec);
    if (ec)
      std::filesystem::copy_file(src, dst,
                                 std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec)
      dormant_cold.push_back(c);
    else
      std::cerr << "Warning: missing cold dormant file: " << src << std::endl;
  }

  std::cout << "Loaded core state from: " << file_path << std::endl;
  std::cout << "  - Round: " << round << std::endl;
  std::cout << "  - Network peers: " << network.size() << std::endl;
  std::cout << "  - Dormant peers (RAM):  " << dormant_peers.size() << std::endl;
  std::cout << "  - Dormant peers (disk): " << dormant_cold.size() << std::endl;
}

#include "parser.h"

Parser::Parser()
{
  static const std::string run_dir = []
  {
    const auto ts = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return "data/" + std::to_string(ts);
  }();
  direction = run_dir;
  std::error_code ec;
  fs::create_directories(direction, ec); // creates "data/" too
  if (ec)
  {
    std::cerr << "Fatal: cannot create run directory " << direction << ": "
              << ec.message() << std::endl;
    std::exit(1);
  }
  static bool announced = false;
  if (!announced)
  {
    std::cout << "Created timestamped directory: " << direction << std::endl;
    announced = true;
  }
}

bool Parser::file_exists(const std::string &filename)
{
  return std::filesystem::exists(filename);
}

void Parser::save_cache(std::vector<cache> &data)
{
  if (data.empty())
    return;
  std::string folder_path = direction + "/caches";
  if (!fs::exists(folder_path))
    fs::create_directories(folder_path);
  std::unordered_map<uint32_t, std::string> by_source;
  for (auto &c : data)
  {
    std::string &text = by_source[c.source];
    text += std::to_string(c.round) + "," + std::to_string(c.addrman_size) + "\n";
    for (auto &addr : c.addresses)
      text += "," + std::to_string(addr.first) + "," +
              std::to_string(addr.second) + "\n";
  }
  for (auto &[source, text] : by_source)
  {
    std::string file_path = folder_path + "/" + std::to_string(source);
    std::ofstream file(file_path,
                       fs::exists(file_path) ? std::ios::app : std::ios::out);
    if (file.is_open())
      file << text;
    else
      std::cerr << "Error: Unable to open file " << file_path << std::endl;
  }
}

void Parser::save_edges(std::vector<edge> &data)
{
  if (data.empty())
    return;
  std::string folder_path = direction + "/edges";
  if (!fs::exists(folder_path))
    fs::create_directories(folder_path);
  std::unordered_map<uint32_t, std::string> by_source;
  for (auto &c : data)
    by_source[c.source] += std::to_string(c.round) + "," +
                           std::to_string(c.destination) + "," +
                           std::to_string(c.exist) + "\n";
  for (auto &[source, text] : by_source)
  {
    std::string file_path = folder_path + "/" + std::to_string(source);
    std::ofstream file(file_path,
                       fs::exists(file_path) ? std::ios::app : std::ios::out);
    if (file.is_open())
      file << text;
    else
      std::cerr << "Error: Unable to open file " << file_path << std::endl;
  }
}

void Parser::save_nodes(std::vector<node> &data)
{
  if (data.empty())
    return;
  std::string file_path = direction + "/nodes";
  std::ofstream file(file_path,
                     fs::exists(file_path) ? std::ios::app : std::ios::out);
  if (!file.is_open())
  {
    std::cerr << "Error: Unable to open file " << file_path << std::endl;
    return;
  }
  for (auto &c : data)
    file << c.to_compact_string();
}

void Parser::save_infos(std::vector<info> &data)
{
  if (data.empty())
    return;
  std::string folder_path = direction + "/map_info";
  if (!fs::exists(folder_path))
    fs::create_directories(folder_path);
  std::unordered_map<uint32_t, std::string> by_source;
  for (auto &c : data)
    by_source[c.source] += c.to_compact_string();
  for (auto &[source, text] : by_source)
  {
    std::string file_path = folder_path + "/" + std::to_string(source);
    std::ofstream file(file_path,
                       fs::exists(file_path) ? std::ios::app : std::ios::out);
    if (file.is_open())
      file << text;
    else
      std::cerr << "Error: Unable to open file " << file_path << std::endl;
  }
}

void Parser::save_ip_changes(std::vector<ip_change_record> &data)
{
  std::string folder_path = direction + "/ip_changes";
  if (!fs::exists(folder_path))
  {
    fs::create_directories(folder_path);
  }
  std::string file_path = folder_path + "/ip_changes.csv";
  std::ios_base::openmode mode =
      fs::exists(file_path) ? std::ios::app : std::ios::out;
  std::ofstream file(file_path, mode);
  if (file.is_open())
  {
    for (auto &c : data)
    {
      file << c.round << "," << c.old_ip << "," << c.new_ip << ","
           << c.old_as << "," << c.new_as << "," << c.unique_id << "\n";
    }
    file.close();
  }
}

void Parser::save_compressed(std::string &filepath, std::string &data)
{
  std::ofstream out(filepath);
  out << data;
  out.close();
}
std::vector<std::vector<uint32_t>>
Parser::parse_delimited_data(const std::string &raw_data)
{
  std::vector<std::vector<uint32_t>> result;
  std::stringstream line_stream(raw_data);
  std::string line, field;

  while (std::getline(line_stream, line, '\n'))
  {
    if (line.empty())
      continue;

    std::vector<uint32_t> row;
    std::stringstream field_stream(line);

    while (std::getline(field_stream, field, ','))
      row.push_back(std::stoull(field));

    result.push_back(row);
  }
  return result;
}
std::string Parser::load_data(const std::string &filepath)
{
  std::ifstream file(filepath);
  if (!file)
  {
    std::cerr << "Could not open file for reading: " << filepath << "\n";
    return "";
  }
  std::string file_content;
  std::stringstream buffer;
  buffer << file.rdbuf();
  file_content = buffer.str();
  return file_content;
}
Network_Config Parser::load_config(int argc, char *argv[], const std::string &conf_path)
{
  Network_Config config;
  try
  {
    // Grouped in the same order as the fields in structures.h.
    po::options_description desc("Network Configuration Options");
    desc.add_options()("help,h", "Display this help message")(

        // --- Randomness ---
        "main.seed", po::value<int>(&config.seed)->default_value(0),
        "Seed for the random number generator")(

        // --- Address tables ---
        "peer.max_new_buckets_slots",
        po::value<int>(&config.max_new_buckets_slots)->default_value(1024),
        "Number of buckets in the new table")(
        "peer.max_tried_buckets_slots",
        po::value<int>(&config.max_tried_buckets_slots)->default_value(256),
        "Number of buckets in the tried table")(
        "peer.bucket_size",
        po::value<int>(&config.bucket_size)->default_value(64),
        "Slots per bucket, in both tables")(
        "peer.addrman_new_buckets_per_address",
        po::value<int>(&config.addrman_new_buckets_per_address)
            ->default_value(8),
        "How many new-table slots one address may occupy at once")(

        // --- Connection limits ---
        "peer.max_out_bound_connections",
        po::value<int>(&config.max_out_bound_connections)->default_value(8),
        "Outbound connections a node opens at most")(
        "peer.max_in_bound_connections",
        po::value<int>(&config.max_in_bound_connections)->default_value(117),
        "Inbound connections a node accepts at most")(

        // --- Table collisions ---
        "peer.addrman_set_tried_collision_size",
        po::value<int>(&config.addrman_set_tried_collision_size)
            ->default_value(10),
        "Unresolved collisions held at once")(
        "peer.addrman_replacement",
        po::value<int>(&config.addrman_replacement)->default_value(14400),
        "Age below which the occupying address keeps its slot (seconds)")(
        "peer.addrman_test_window",
        po::value<int>(&config.addrman_test_window)->default_value(2400),
        "Age above which an unresolved collision is decided against the "
        "occupant (seconds)")(

        // --- Address ageing ---
        "peer.addrman_horizon",
        po::value<int>(&config.addrman_horizon)->default_value(2592000),
        "Age above which an address is considered stale (seconds)")(
        "peer.addrman_retries",
        po::value<int>(&config.addrman_retries)->default_value(3),
        "Attempts allowed before an address that never connected is "
        "discarded")(
        "peer.addrman_max_failures",
        po::value<int>(&config.addrman_max_failures)->default_value(10),
        "Consecutive failures allowed before an address is discarded")(
        "peer.addrman_min_fail",
        po::value<int>(&config.addrman_min_fail)->default_value(604800),
        "Age the last success must exceed before failures are counted "
        "(seconds)")(

        // --- Address broadcasting ---
        "peer.avg_local_address_broadcast_interval",
        po::value<int>(&config.avg_local_address_broadcast_interval)
            ->default_value(86400),
        "Mean interval between announcements of a node's own address "
        "(seconds)")(
        "peer.time_to_update_advertsisng_cache",
        po::value<int>(&config.time_to_update_advertsisng_cache)
            ->default_value(75600),
        "Fixed part of the advertised-address cache lifetime (seconds)")(
        "peer.avg_time_to_update_advertsisng_cache",
        po::value<int>(&config.avg_time_to_update_advertsisng_cache)
            ->default_value(21600),
        "Random part added to that lifetime (seconds)")(

        // --- Reachability ---
        "core.prob_reachable",
        po::value<double>(&config.prob_reachable)->default_value(0.26),
        "Probability that a node accepts inbound connections")(

        // --- Session lengths ---
        "core.public_session_days",
        po::value<double>(&config.public_session_days)->default_value(5.6),
        "Mean session length of a node accepting inbound (days)")(
        "core.public_core_fraction",
        po::value<double>(&config.public_core_fraction)->default_value(0.04),
        "Fraction of those nodes drawn from the long-lived group instead")(
        "core.public_core_days",
        po::value<double>(&config.public_core_days)->default_value(365.0),
        "Mean session length of that long-lived group (days)")(
        "core.nat_session_days",
        po::value<double>(&config.nat_session_days)->default_value(20.0),
        "Mean session length of a node not accepting inbound (days)")(

        // --- Arrivals ---
        "core.join_node_number",
        po::value<double>(&config.join_node_number)->default_value(20.83),
        "Mean number of nodes arriving per window")(
        "core.join_node_time",
        po::value<double>(&config.join_node_time)->default_value(900.0),
        "Length of that arrival window (seconds)")(

        // --- Resource limits ---
        "core.dormant_cap",
        po::value<uint32_t>(&config.dormant_cap)->default_value(2000),
        "Non-zero writes departed nodes to disk instead of holding them in "
        "memory")(
        "core.cache_sample_rate",
        po::value<uint32_t>(&config.cache_sample_rate)->default_value(1),
        "Record cache contents for one node in this many; 1 records every "
        "node");

    po::variables_map vm;

    // Parse command-line arguments first (highest priority)
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    // Check if help was requested
    if (vm.count("help"))
    {
      std::cout << desc << std::endl;
      std::cout << "\nUsage examples:" << std::endl;
      std::cout << "  " << argv[0]
                << " --main.seed=42 --peer.max_out_bound_connections=16"
                << std::endl;
      std::cout << "  " << argv[0] << " --help" << std::endl;
      std::cout << "\nConfiguration priority:" << std::endl;
      std::cout << "  1. Command-line arguments (highest)" << std::endl;
      std::cout << "  2. simulator.conf file" << std::endl;
      std::cout << "  3. Default values (lowest)" << std::endl;
      exit(0);
    }

    // Try to load config file
    std::ifstream config_file(conf_path);
    if (config_file.is_open())
    {
      // Parse config file but don't override command-line args.
      // allow_unregistered = true so a conf written before a knob was retired
      // still loads: the stale key is ignored instead of aborting the run.
      // The cost is that a misspelled key is also ignored rather than fatal.
      po::store(po::parse_config_file(config_file, desc, true), vm);
      config_file.close();
      std::cout << "Network configuration loaded from: " << conf_path
                << std::endl;
    }
    else if (conf_path != "simulator.conf")
    {
      std::cerr << "Configuration error: " << conf_path
                << " not found.\nRefusing to resume with default settings — the "
                   "run's config is part of its identity.\n";
      std::exit(1);
    }

    else
    {
      std::cout << "Config file not found. Creating simulator.conf with "
                   "default values..."
                << std::endl;

      // Create config file with default values and comments
      std::ofstream new_config("simulator.conf");
      if (new_config.is_open())
      {
        new_config << "# Network Configuration File\n";
        new_config << "# Automatically generated\n\n";

        new_config << "[main]\n";
        new_config << "# --- Randomness ---\n";
        new_config << "# Seed for the random number generator\n";
        new_config << "seed = 0\n\n";

        new_config << "[peer]\n";
        new_config << "# --- Address tables ---\n";
        new_config << "# Number of buckets in the new table\n";
        new_config << "max_new_buckets_slots = 1024\n";
        new_config << "# Number of buckets in the tried table\n";
        new_config << "max_tried_buckets_slots = 256\n";
        new_config << "# Slots per bucket, in both tables\n";
        new_config << "bucket_size = 64\n";
        new_config << "# How many new-table slots one address may occupy at "
                      "once\n";
        new_config << "addrman_new_buckets_per_address = 8\n\n";

        new_config << "# --- Connection limits ---\n";
        new_config << "# Outbound connections a node opens at most\n";
        new_config << "max_out_bound_connections = 8\n";
        new_config << "# Inbound connections a node accepts at most\n";
        new_config << "max_in_bound_connections = 117\n\n";

        new_config << "# --- Table collisions ---\n";
        new_config << "# Unresolved collisions held at once\n";
        new_config << "addrman_set_tried_collision_size = 10\n";
        new_config << "# Age below which the occupying address keeps its slot "
                      "(seconds)\n";
        new_config << "addrman_replacement = 14400\n";
        new_config << "# Age above which an unresolved collision is decided "
                      "against the occupant (seconds)\n";
        new_config << "addrman_test_window = 2400\n\n";

        new_config << "# --- Address ageing ---\n";
        new_config << "# Age above which an address is considered stale "
                      "(seconds)\n";
        new_config << "addrman_horizon = 2592000\n";
        new_config << "# Attempts allowed before an address that never "
                      "connected is discarded\n";
        new_config << "addrman_retries = 3\n";
        new_config << "# Consecutive failures allowed before an address is "
                      "discarded\n";
        new_config << "addrman_max_failures = 10\n";
        new_config << "# Age the last success must exceed before failures are "
                      "counted (seconds)\n";
        new_config << "addrman_min_fail = 604800\n\n";

        new_config << "# --- Address broadcasting ---\n";
        new_config << "# Mean interval between announcements of a node's own "
                      "address (seconds)\n";
        new_config << "avg_local_address_broadcast_interval = 86400\n";
        new_config << "# Fixed part of the advertised-address cache lifetime "
                      "(seconds)\n";
        new_config << "time_to_update_advertsisng_cache = 75600\n";
        new_config << "# Random part added to that lifetime (seconds)\n";
        new_config << "avg_time_to_update_advertsisng_cache = 21600\n\n";

        new_config << "[core]\n";
        new_config << "# --- Reachability ---\n";
        new_config << "# Probability that a node accepts inbound connections\n";
        new_config << "prob_reachable = 0.26\n\n";

        new_config << "# --- Session lengths ---\n";
        new_config << "# Mean session length of a node accepting inbound "
                      "(days)\n";
        new_config << "public_session_days = 5.6\n";
        new_config << "# Fraction of those nodes drawn from the long-lived "
                      "group instead\n";
        new_config << "public_core_fraction = 0.04\n";
        new_config << "# Mean session length of that long-lived group (days)\n";
        new_config << "public_core_days = 365\n";
        new_config << "# Mean session length of a node not accepting inbound "
                      "(days)\n";
        new_config << "nat_session_days = 20\n\n";

        new_config << "# --- Arrivals ---\n";
        new_config << "# Mean number of nodes arriving per window\n";
        new_config << "join_node_number = 20.83\n";
        new_config << "# Length of that arrival window (seconds)\n";
        new_config << "join_node_time = 900\n\n";

        new_config << "# --- Resource limits ---\n";
        new_config << "# Non-zero writes departed nodes to disk instead of "
                      "holding them in memory\n";
        new_config << "dormant_cap = 2000\n";
        new_config << "# Record cache contents for one node in this many; 1 "
                      "records every node\n";
        new_config << "cache_sample_rate = 1\n";

        new_config.close();
        std::cout << "Created simulator.conf with default values." << std::endl;
      }
      else
      {
        std::cerr << "Warning: Could not create simulator.conf" << std::endl;
      }
    }

    // Apply all configurations
    po::notify(vm);

    std::cout << "Configuration loaded successfully." << std::endl;
  }
  catch (const po::error &e)
  {
    std::cerr << "Configuration error: " << e.what() << "\n"
              << "Refusing to run with silently-defaulted settings. Fix "
                 "simulator.conf, or delete it to regenerate defaults.\n";
    std::exit(1);
  }
  catch (const std::exception &e)
  {
    std::cerr << "Error loading config: " << e.what() << "\n"
              << "Refusing to run with silently-defaulted settings.\n";
    std::exit(1);
  }

  config.networks[NET_IPV4] = {1.0, config.prob_reachable, 1.0};

  return config;
}
void Parser::save_runtime_config(const Network_Config &config)
{
  std::string filepath = direction + "/simulator.conf";
  std::ofstream config_file(filepath);
  if (!config_file.is_open())
  {
    std::cerr << "Warning: Could not create config file at " << filepath
              << std::endl;
    return;
  }

  config_file << "# Network Configuration File\n";
  config_file << "# This file contains the ACTUAL runtime parameters used\n";
  config_file << "# (including command-line argument overrides)\n\n";

  config_file << "[main]\n";
  config_file << "# --- Randomness ---\n";
  config_file << "# Seed for the random number generator\n";
  config_file << "seed = " << config.seed << "\n\n";

  config_file << "[peer]\n";
  config_file << "# --- Address tables ---\n";
  config_file << "# Number of buckets in the new table\n";
  config_file << "max_new_buckets_slots = " << config.max_new_buckets_slots
              << "\n";
  config_file << "# Number of buckets in the tried table\n";
  config_file << "max_tried_buckets_slots = " << config.max_tried_buckets_slots
              << "\n";
  config_file << "# Slots per bucket, in both tables\n";
  config_file << "bucket_size = " << config.bucket_size << "\n";
  config_file << "# How many new-table slots one address may occupy at once\n";
  config_file << "addrman_new_buckets_per_address = "
              << config.addrman_new_buckets_per_address << "\n\n";

  config_file << "# --- Connection limits ---\n";
  config_file << "# Outbound connections a node opens at most\n";
  config_file << "max_out_bound_connections = "
              << config.max_out_bound_connections << "\n";
  config_file << "# Inbound connections a node accepts at most\n";
  config_file << "max_in_bound_connections = "
              << config.max_in_bound_connections << "\n\n";

  config_file << "# --- Table collisions ---\n";
  config_file << "# Unresolved collisions held at once\n";
  config_file << "addrman_set_tried_collision_size = "
              << config.addrman_set_tried_collision_size << "\n";
  config_file << "# Age below which the occupying address keeps its slot "
                 "(seconds)\n";
  config_file << "addrman_replacement = " << config.addrman_replacement << "\n";
  config_file << "# Age above which an unresolved collision is decided against "
                 "the occupant (seconds)\n";
  config_file << "addrman_test_window = " << config.addrman_test_window
              << "\n\n";

  config_file << "# --- Address ageing ---\n";
  config_file << "# Age above which an address is considered stale (seconds)\n";
  config_file << "addrman_horizon = " << config.addrman_horizon << "\n";
  config_file << "# Attempts allowed before an address that never connected is "
                 "discarded\n";
  config_file << "addrman_retries = " << config.addrman_retries << "\n";
  config_file << "# Consecutive failures allowed before an address is "
                 "discarded\n";
  config_file << "addrman_max_failures = " << config.addrman_max_failures
              << "\n";
  config_file << "# Age the last success must exceed before failures are "
                 "counted (seconds)\n";
  config_file << "addrman_min_fail = " << config.addrman_min_fail << "\n\n";

  config_file << "# --- Address broadcasting ---\n";
  config_file << "# Mean interval between announcements of a node's own "
                 "address (seconds)\n";
  config_file << "avg_local_address_broadcast_interval = "
              << config.avg_local_address_broadcast_interval << "\n";
  config_file << "# Fixed part of the advertised-address cache lifetime "
                 "(seconds)\n";
  config_file << "time_to_update_advertsisng_cache = "
              << config.time_to_update_advertsisng_cache << "\n";
  config_file << "# Random part added to that lifetime (seconds)\n";
  config_file << "avg_time_to_update_advertsisng_cache = "
              << config.avg_time_to_update_advertsisng_cache << "\n\n";

  config_file << std::setprecision(15);

  config_file << "[core]\n";
  config_file << "# --- Reachability ---\n";
  config_file << "# Probability that a node accepts inbound connections\n";
  config_file << "prob_reachable = " << config.prob_reachable << "\n\n";

  config_file << "# --- Session lengths ---\n";
  config_file << "# Mean session length of a node accepting inbound (days)\n";
  config_file << "public_session_days = " << config.public_session_days << "\n";
  config_file << "# Fraction of those nodes drawn from the long-lived group "
                 "instead\n";
  config_file << "public_core_fraction = " << config.public_core_fraction
              << "\n";
  config_file << "# Mean session length of that long-lived group (days)\n";
  config_file << "public_core_days = " << config.public_core_days << "\n";
  config_file << "# Mean session length of a node not accepting inbound "
                 "(days)\n";
  config_file << "nat_session_days = " << config.nat_session_days << "\n\n";

  config_file << "# --- Arrivals ---\n";
  config_file << "# Mean number of nodes arriving per window\n";
  config_file << "join_node_number = " << config.join_node_number << "\n";
  config_file << "# Length of that arrival window (seconds)\n";
  config_file << "join_node_time = " << config.join_node_time << "\n\n";

  config_file << "# --- Resource limits ---\n";
  config_file << "# Non-zero writes departed nodes to disk instead of holding "
                 "them in memory\n";
  config_file << "dormant_cap = " << config.dormant_cap << "\n";
  config_file << "# Record cache contents for one node in this many; 1 records "
                 "every node\n";
  config_file << "cache_sample_rate = " << config.cache_sample_rate << "\n";

  config_file.close();
  std::cout << "Saved runtime configuration to: " << filepath << std::endl;
}
void Parser::save_peer_data(uint32_t ip, const std::string &map_info_str,
                            const std::string &vvNew_str,
                            const std::string &vvTried_str,
                            const std::string &base_path)
{
  // Create directories
  fs::create_directories(base_path + "/" + std::to_string(ip));

  std::string map_info_dir =
      base_path + "/" + std::to_string(ip) + "/map_infos";
  std::string vvNew_dir = base_path + "/" + std::to_string(ip) + "/vvNew_infos";
  std::string vvTried_dir =
      base_path + "/" + std::to_string(ip) + "/vvTried_infos";

  // Save compressed files
  save_compressed(map_info_dir, const_cast<std::string &>(map_info_str));
  save_compressed(vvNew_dir, const_cast<std::string &>(vvNew_str));
  save_compressed(vvTried_dir, const_cast<std::string &>(vvTried_str));
}
void Parser::save_telemetry(std::vector<telemetry> &data)
{
  if (data.empty())
    return;
  std::string file_path = direction + "/telemetry.csv";
  bool exists = fs::exists(file_path);
  std::ofstream file(file_path, exists ? std::ios::app : std::ios::out);
  if (!file.is_open())
  {
    std::cerr << "Error: Unable to open file " << file_path << std::endl;
    return;
  }
  if (!exists)
    file << "round,ip,feeler_success,feeler_fail,getaddr_served,addr_dropped\n";
  for (const auto &t : data)
    file << t.round << "," << t.ip << "," << t.feeler_success << ","
         << t.feeler_fail << "," << t.getaddr_served << "," << t.addr_dropped
         << "\n";
}

std::vector<uint8_t> Parser::load_binary(const std::string &file_name,
                                         const std::string &base_path)
{
  std::string file_path = base_path + "/" + file_name + ".bin";
  std::ifstream file(file_path, std::ios::binary | std::ios::ate);

  if (!file.is_open())
  {
    std::cerr << "Error: Unable to open file for reading: " << file_path
              << std::endl;
    return {};
  }

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::vector<uint8_t> buffer(size);
  if (!file.read(reinterpret_cast<char *>(buffer.data()), size))
  {
    std::cerr << "Error: Failed to read file: " << file_path << std::endl;
    return {};
  }

  return buffer;
}

void Parser::save_binary(const std::string file_name,
                         const std::vector<uint8_t> &data,
                         const std::string &base_path)
{
  std::string folder_path = direction + "/" + base_path;
  if (!fs::exists(folder_path))
  {
    fs::create_directories(folder_path);
  }

  std::string file_path = folder_path + "/" + file_name + ".bin";
  std::ofstream file(file_path, std::ios::binary);
  if (file.is_open())
  {
    file.write(reinterpret_cast<const char *>(data.data()), data.size());
    file.close();
  }
  else
  {
    std::cerr << "Error: Unable to open file for writing: " << file_path
              << std::endl;
  }
}

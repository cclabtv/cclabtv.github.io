#ifndef DATA_PROCESSOR_H
#define DATA_PROCESSOR_H

#include <string>
#include <vector>
#include <queue>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <cstdint>
#include "structures.h"
#include "graph.h"
#include "file_manager.h"
#include "parsing_utils.h"
#include "cache_reader.h"

struct DailyCacheStats
{
    double avg_ips_count;
    uint64_t total_ips;
    uint32_t num_rounds;
    uint64_t union_size;
    double avg_age_seconds = 0.0;
    double pct_fresh_1d = 0.0;
    double pct_fresh_7d = 0.0;
};

struct TableQuality
{
    uint64_t entries = 0;
    uint64_t tried = 0;
    double mean_attempts = 0.0;
    double pct_ever_success = 0.0;
    double mean_ref_count = 0.0;
};

class DataProcessor
{
public:
    DataProcessor(
        const std::vector<std::string> &data_paths,
        const std::string &output_path,
        uint64_t computation_interval,
        uint64_t random_seed,
        const std::string &caches_path = "");

    void process();

private:
    std::vector<std::string> data_paths_;
    std::string output_path_;
    uint64_t computation_interval_;
    uint64_t random_seed_;
    std::string caches_path_;

    std::unordered_map<uint32_t, DailyCacheStats> daily_cache_stats_;

    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> daily_ip_freq_;

    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> daily_cache_ips_;

    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> daily_network_nodes_;

    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> daily_all_network_nodes_;

    std::ofstream degree_csv_;
    uint32_t snapshot_days_written_ = 0;

    std::vector<std::string> nodes_files;
    std::vector<std::string> edges_folders;
    std::vector<std::string> info_folders;
    std::vector<std::string> map_info_folders;
    std::string output_folder;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, Node>> nodes_meta_data_map;

    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> node_ip_source_dirs_;
    std::unordered_map<uint32_t, uint32_t> max_degree_;
    std::unordered_map<uint32_t, uint32_t> outbound_edge_count_;
    std::unordered_map<uint32_t, uint32_t> inbound_edge_count_;
    static constexpr size_t BATCH_SIZE = 1000;

    void initialize_paths();

    std::vector<Node> load_nodes();
    std::vector<Edge> load_edges();

    void process_graph_metrics(std::vector<Node> &nodes, std::vector<Edge> &edges);

    void process_local_views(const std::vector<Node> &nodes);

    void compute_and_cache_metrics(
        Graph &graph,
        const std::vector<uint32_t> &nodes_in_graph,
        uint32_t &cached_gcc,
        uint32_t &cached_num_components,
        uint32_t &cached_diameter,
        double &cached_assortativity,
        bool force_computation,
        std::vector<uint32_t> *non_gcc_nodes = nullptr);

    std::vector<MapInfoEntry> load_map_info_entries(uint32_t node_ip);

    std::pair<double, double> calculate_network_presence(
        const std::vector<uint32_t> &map_info_addresses,
        const std::vector<uint32_t> &nodes_in_network);

    static TableQuality summarize_table(const std::vector<MapInfoEntry> &entries);

    void load_cache_data();
    void stream_cache_file(const std::string &filepath, uint32_t source_ip,
                           bool source_crawlable);
    DailyCacheStats get_cache_stats_for_day(uint32_t day) const;
    void save_daily_cache_metrics();
    void save_daily_ip_frequency();
    void write_daily_snapshot(uint32_t day, Graph &graph);

    std::unordered_map<uint32_t, bool> ip_class_;
    void process_lifecycle_metrics(const std::vector<Node> &nodes);
    void process_telemetry();
    void process_ip_changes();
    std::unordered_map<uint32_t, std::unordered_set<uint64_t>> ip_rotation_events_;
    void load_ip_rotation_events();

    void build_session_index();
    void save_crawler_outputs();
};

#endif

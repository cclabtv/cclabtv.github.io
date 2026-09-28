#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include "eigenvalue_calculator.h"

static std::vector<uint32_t> parse_snapshot_nodes(const std::string &filepath)
{
    std::vector<uint32_t> nodes;
    std::ifstream file(filepath);
    if (!file.is_open())
        return nodes;

    std::string line;
    std::getline(file, line);
    while (std::getline(file, line))
    {
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (!line.empty())
            nodes.push_back(static_cast<uint32_t>(std::stoul(line)));
    }
    return nodes;
}

static std::vector<std::pair<uint32_t, uint32_t>> parse_snapshot_edges(const std::string &filepath)
{
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    std::ifstream file(filepath);
    if (!file.is_open())
        return edges;

    std::string line;
    std::getline(file, line);
    while (std::getline(file, line))
    {
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        size_t comma = line.find(',');
        if (comma == std::string::npos)
            continue;
        uint32_t src = static_cast<uint32_t>(std::stoul(line.substr(0, comma)));
        uint32_t dst = static_cast<uint32_t>(std::stoul(line.substr(comma + 1)));
        edges.push_back({src, dst});
    }
    return edges;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0]
                  << " <results_folder> [output_file] [num_threads] [p]" << std::endl;
        std::cerr << "  results_folder : path to results directory containing snapshots/ subfolder" << std::endl;
        std::cerr << "  output_file    : output CSV (default: <results_folder>/eigenvalue_metrics.csv)" << std::endl;
        std::cerr << "  num_threads    : worker threads (default: hardware concurrency)" << std::endl;
        std::cerr << "  p              : upper day index, 0-based (default: last day index)" << std::endl;
        std::cerr << "                   order: days[p] -> days[0] -> days[p-1] -> days[1] -> ... -> days[p/2]" << std::endl;
        return EXIT_FAILURE;
    }

    std::string results_folder = argv[1];
    if (results_folder.back() != '/')
        results_folder += '/';

    const std::string snapshots_dir = results_folder + "snapshots/";
    const std::string output_file   = argc >= 3 ? argv[2] : results_folder + "eigenvalue_metrics.csv";

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    if (argc >= 4)        num_threads = static_cast<unsigned int>(std::stoul(argv[3]));

    if (!std::filesystem::exists(snapshots_dir))
    {
        std::cerr << "Error: snapshots directory not found: " << snapshots_dir << std::endl;
        return EXIT_FAILURE;
    }

    std::vector<uint32_t> days;
    const std::string nodes_suffix = "_nodes.csv";
    for (const auto &entry : std::filesystem::directory_iterator(snapshots_dir))
    {
        const std::string fname = entry.path().filename().string();
        if (fname.size() > nodes_suffix.size() &&
            fname.substr(fname.size() - nodes_suffix.size()) == nodes_suffix)
        {
            try
            {
                std::string day_str = fname.substr(0, fname.size() - nodes_suffix.size());
                days.push_back(static_cast<uint32_t>(std::stoul(day_str)));
            }
            catch (...) {}
        }
    }
    std::sort(days.begin(), days.end());

    if (days.empty())
    {
        std::cerr << "No snapshot files found in " << snapshots_dir << std::endl;
        return EXIT_FAILURE;
    }

    size_t p = days.size() - 1;
    if (argc >= 5)
    {
        size_t arg_p = std::stoul(argv[4]);
        if (arg_p >= days.size())
        {
            std::cerr << "Warning: p=" << arg_p
                      << " exceeds last index " << (days.size() - 1)
                      << ", clamping." << std::endl;
            arg_p = days.size() - 1;
        }
        p = arg_p;
    }

    const size_t total_to_compute = p + 1;
    num_threads = std::min(num_threads, static_cast<unsigned int>(total_to_compute));

    std::cout << "=== Eigenvalue Calculator ===" << std::endl;
    std::cout << "Snapshots dir:  " << snapshots_dir    << std::endl;
    std::cout << "Output file:    " << output_file       << std::endl;
    std::cout << "Total days:     " << days.size()       << std::endl;
    std::cout << "p (upper idx):  " << p                 << "  (day value: " << days[p] << ")" << std::endl;
    std::cout << "Computing:      " << total_to_compute  << " days (indices 0.." << p << ")" << std::endl;
    std::cout << "Threads:        " << num_threads        << std::endl;
    std::cout << "Order:          day " << days[0] << " -> day " << days[1] << " -> ... -> day " << days[p] << std::endl;
    std::cout << "=============================" << std::endl << std::endl;

    std::ofstream out(output_file);
    if (!out.is_open())
    {
        std::cerr << "Error: Failed to create output file: " << output_file << std::endl;
        return EXIT_FAILURE;
    }
    out << "day_index,node_count,edge_count,lambda1,lambda2,spectral_gap,eigenvalue_ratio\n";
    out.flush();

    std::queue<uint32_t> work_queue;
    std::mutex           queue_mutex;
    std::mutex           file_mutex;
    std::mutex           print_mutex;
    std::atomic<size_t>  completed{0};

    for (size_t i = 0; i <= p; ++i)
        work_queue.push(days[i]);

    auto worker = [&]()
    {
        while (true)
        {
            uint32_t day;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (work_queue.empty()) return;
                day = work_queue.front();
                work_queue.pop();
            }

            const std::string day_str = std::to_string(day);
            auto nodes = parse_snapshot_nodes(snapshots_dir + day_str + "_nodes.csv");
            auto edges = parse_snapshot_edges(snapshots_dir + day_str + "_edges.csv");

            EigenvalueResult result = EigenvalueCalculator::compute(nodes, edges);

            {
                std::lock_guard<std::mutex> lock(file_mutex);
                out << day             << ","
                    << result.node_count << ","
                    << result.edge_count << ","
                    << result.lambda1    << ","
                    << result.lambda2    << ","
                    << result.spectral_gap     << ","
                    << result.eigenvalue_ratio << "\n";
                out.flush();
            }

            size_t done = ++completed;
            {
                std::lock_guard<std::mutex> lock(print_mutex);
                std::cout << "  [" << done << "/" << total_to_compute << "]"
                          << "  day=" << day
                          << "  nodes=" << result.node_count
                          << "  edges=" << result.edge_count
                          << "  gap="   << result.spectral_gap
                          << std::endl;
            }
        }
    };

    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (unsigned int i = 0; i < num_threads; ++i)
        threads.emplace_back(worker);

    for (auto &t : threads)
        t.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);

    std::cout << std::endl;
    std::cout << "=============================" << std::endl;
    std::cout << "Completed " << total_to_compute << " days in " << duration.count() << " seconds" << std::endl;
    std::cout << "Results saved to: " << output_file << std::endl;
    std::cout << "=============================" << std::endl;

    return EXIT_SUCCESS;
}

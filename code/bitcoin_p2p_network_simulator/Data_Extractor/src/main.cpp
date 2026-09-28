#include <iostream>
#include <string>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include "data_processor.h"
#include "cache_reader.h"

std::vector<std::string> find_data_paths()
{
    std::string data_dir = "../data";
    std::vector<std::string> paths;
    if (std::filesystem::exists(data_dir))
    {
        for (const auto &entry : std::filesystem::directory_iterator(data_dir))
        {
            if (entry.is_directory())
            {
                paths.push_back(entry.path().filename().string());
            }
        }
        std::sort(paths.begin(), paths.end());
    }
    return paths;
}

std::string find_caches_path(const std::vector<std::string> &data_paths)
{
    for (const auto &data_path : data_paths)
    {
        std::string caches_dir = "../data/" + data_path + "/caches";
        if (std::filesystem::exists(caches_dir))
        {
            return caches_dir;
        }
    }
    return "";
}

int main(int argc, char *argv[])
{
    std::vector<std::string> data_paths;
    if (argc >= 2)
    {
        data_paths.push_back(argv[1]);
    }
    else
    {
        data_paths = find_data_paths();
        if (data_paths.empty())
        {
            std::cerr << "No data folder found in ../data/" << std::endl;
            return EXIT_FAILURE;
        }
    }

    const std::string output_path = argc >= 3 ? argv[2] : "results/";
    const uint64_t computation_interval = argc >= 4 ? std::stoull(argv[3]) : 3 * 3600;
    const uint64_t random_seed = argc >= 5 ? std::stoull(argv[4]) : 0;

    std::string caches_path = argc >= 6 ? argv[5] : "";
    if (caches_path.empty())
    {
        caches_path = find_caches_path(data_paths);
    }

    std::cout << "=== Graph Analysis Tool ===" << std::endl;
    std::cout << "Data paths:" << std::endl;
    for (const auto &p : data_paths)
        std::cout << "  " << p << std::endl;
    std::cout << "Output path: " << output_path << std::endl;
    std::cout << "Computation interval: " << computation_interval << std::endl;
    std::cout << "Random seed: " << random_seed << std::endl;
    if (!caches_path.empty())
    {
        std::cout << "Caches path: " << caches_path << std::endl;
    }
    std::cout << "===========================" << std::endl
              << std::endl;

    try
    {
        auto start_time = std::chrono::high_resolution_clock::now();

        DataProcessor processor(data_paths, output_path, computation_interval, random_seed, caches_path);
        processor.process();

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);

        std::cout << std::endl;
        std::cout << "===========================" << std::endl;
        std::cout << "Processing completed successfully!" << std::endl;
        std::cout << "Time elapsed: " << duration.count() << " seconds" << std::endl;
        std::cout << "Results saved to: " << output_path << data_paths[0] << "/" << std::endl;
        std::cout << "===========================" << std::endl;

        return EXIT_SUCCESS;
    }
    catch (const std::exception &e)
    {
        std::cerr << std::endl;
        std::cerr << "===========================" << std::endl;
        std::cerr << "ERROR: " << e.what() << std::endl;
        std::cerr << "===========================" << std::endl;
        return EXIT_FAILURE;
    }
}

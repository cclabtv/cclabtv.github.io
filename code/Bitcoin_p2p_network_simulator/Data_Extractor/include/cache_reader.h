#ifndef CACHE_READER_H
#define CACHE_READER_H

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <filesystem>

namespace cache
{

    struct RoundData
    {
        uint32_t round;
        uint32_t map_info_size;
        std::vector<uint32_t> ips;
        std::vector<uint64_t> ages;
    };

    using CacheData = std::unordered_map<uint32_t, std::unordered_map<std::string, std::vector<RoundData>>>;

    class CacheReader
    {
    public:
        CacheData data;

        bool read_file(const std::string &filepath)
        {
            std::ifstream file(filepath);
            if (!file.is_open())
            {
                return false;
            }

            std::string provider = std::filesystem::path(filepath).filename().string();
            std::string line;
            uint32_t current_round = 0;
            uint32_t current_map_size = 0;
            std::vector<uint32_t> current_ips;
            std::vector<uint64_t> current_ages;

            while (std::getline(file, line))
            {
                if (line.empty())
                    continue;

                size_t comma_pos = line.find(',');
                if (comma_pos == std::string::npos)
                    continue;

                std::string first_col = line.substr(0, comma_pos);
                std::string second_col = line.substr(comma_pos + 1);

                if (!first_col.empty())
                {
                    if (current_round != 0 && !current_ips.empty())
                    {
                        uint32_t key = current_round % 86400;
                        data[key][provider].push_back(RoundData{current_round, current_map_size, std::move(current_ips), std::move(current_ages)});
                        current_ips.clear();
                        current_ages.clear();
                    }

                    current_round = static_cast<uint32_t>(std::stoul(first_col));
                    current_map_size = static_cast<uint32_t>(std::stoul(second_col));
                }
                else
                {

                    if (!second_col.empty())
                    {
                        size_t age_pos = second_col.find(',');
                        current_ips.push_back(static_cast<uint32_t>(std::stoul(second_col)));
                        if (age_pos != std::string::npos)
                            current_ages.push_back(std::stoull(second_col.substr(age_pos + 1)));
                        else
                            current_ages.push_back(0);
                    }
                }
            }

            if (current_round != 0 && !current_ips.empty())
            {
                uint32_t key = current_round % 86400;
                data[key][provider].push_back(RoundData{current_round, current_map_size, std::move(current_ips), std::move(current_ages)});
            }

            return true;
        }

        bool read_directory(const std::string &dir_path)
        {
            if (!std::filesystem::exists(dir_path))
            {
                return false;
            }

            for (const auto &entry : std::filesystem::directory_iterator(dir_path))
            {
                if (entry.is_regular_file())
                {
                    std::string filename = entry.path().filename().string();
                    read_file(entry.path().string());
                }
            }
            return true;
        }

        std::vector<uint32_t> get_ips_for_round(uint32_t round_key) const
        {
            std::vector<uint32_t> all_ips;
            auto it = data.find(round_key);
            if (it != data.end())
            {
                for (const auto &[provider, rounds] : it->second)
                {
                    for (const auto &rd : rounds)
                    {
                        all_ips.insert(all_ips.end(), rd.ips.begin(), rd.ips.end());
                    }
                }
            }
            return all_ips;
        }

        std::vector<std::string> get_providers_for_round(uint32_t round_key) const
        {
            std::vector<std::string> providers;
            auto it = data.find(round_key);
            if (it != data.end())
            {
                for (const auto &[provider, _] : it->second)
                {
                    providers.push_back(provider);
                }
            }
            return providers;
        }

        bool save_daily_avg_ips_to_csv(const std::string &output_path) const
        {
            std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> daily_stats;
            std::unordered_map<uint32_t, std::unordered_set<uint32_t>> daily_unique_ips;

            for (const auto &[round_key, providers] : data)
            {
                for (const auto &[provider, rounds] : providers)
                {
                    for (const auto &rd : rounds)
                    {
                        uint32_t day = rd.round / 86400;
                        daily_stats[day].first += rd.ips.size();
                        daily_stats[day].second += 1;

                        for (uint32_t ip : rd.ips)
                        {
                            daily_unique_ips[day].insert(ip);
                        }
                    }
                }
            }

            std::ofstream out(output_path);
            if (!out.is_open())
            {
                return false;
            }

            out << "day_index,avg_ips_per_crawl_round,total_ip_entries,crawl_round_count,unique_cached_ip_count\n";

            std::vector<uint32_t> days;
            for (const auto &[day, _] : daily_stats)
            {
                days.push_back(day);
            }
            std::sort(days.begin(), days.end());

            for (uint32_t day : days)
            {
                const auto &stats = daily_stats[day];
                double avg = static_cast<double>(stats.first) / stats.second;
                size_t union_size = daily_unique_ips[day].size();
                out << day << "," << avg << "," << stats.first << "," << stats.second << "," << union_size << "\n";
            }

            return true;
        }

        bool save_daily_ip_frequency_to_csv(const std::string &output_path) const
        {

            std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> daily_ip_freq;

            for (const auto &[round_key, providers] : data)
            {
                for (const auto &[provider, rounds] : providers)
                {
                    for (const auto &rd : rounds)
                    {
                        uint32_t day = rd.round / 86400;
                        for (uint32_t ip : rd.ips)
                        {
                            daily_ip_freq[day][ip]++;
                        }
                    }
                }
            }

            std::ofstream out(output_path);
            if (!out.is_open())
            {
                return false;
            }

            out << "day_index,ip,occurrence_count\n";

            std::vector<uint32_t> days;
            for (const auto &[day, _] : daily_ip_freq)
            {
                days.push_back(day);
            }
            std::sort(days.begin(), days.end());

            for (uint32_t day : days)
            {

                std::vector<std::pair<uint32_t, uint32_t>> ip_counts;
                for (const auto &[ip, count] : daily_ip_freq[day])
                {
                    ip_counts.emplace_back(ip, count);
                }
                std::sort(ip_counts.begin(), ip_counts.end(),
                          [](const auto &a, const auto &b)
                          { return a.second > b.second; });

                for (const auto &[ip, count] : ip_counts)
                {
                    out << day << "," << ip << "," << count << "\n";
                }
            }

            return true;
        }
    };

}

#endif

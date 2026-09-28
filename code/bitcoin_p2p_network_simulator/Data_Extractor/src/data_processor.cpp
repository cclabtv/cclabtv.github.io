#include "data_processor.h"
#include <sstream>
#include <map>
#include <cstdlib>
#include <cstring>
#include <zlib.h>
DataProcessor::DataProcessor(
    const std::vector<std::string> &data_paths,
    const std::string &output_path,
    uint64_t computation_interval,
    uint64_t random_seed,
    const std::string &caches_path) : data_paths_(data_paths),
                                      output_path_(output_path),
                                      computation_interval_(computation_interval),
                                      random_seed_(random_seed),
                                      caches_path_(caches_path)
{
    initialize_paths();
}

namespace
{
    struct CacheDayAcc
    {
        uint64_t total_ips = 0;
        uint32_t num_rounds = 0;
        uint64_t with_age = 0;
        double age_sum = 0.0;
        uint64_t fresh_1d = 0;
        uint64_t fresh_7d = 0;
    };
    std::unordered_map<uint32_t, CacheDayAcc> g_cache_day_acc;

    struct SessionIvl
    {
        uint64_t start;
        uint64_t end;
        bool reachable;
    };
    struct CrawlWindow
    {
        uint32_t start_day;
        uint32_t end_day;
        std::string label;
    };
    std::unordered_map<uint32_t, std::vector<SessionIvl>> g_sessions;
    uint64_t g_last_event = 0;
    std::vector<CrawlWindow> g_windows;
    uint32_t g_full_size = 1000;
    bool g_archive = true;

    std::vector<std::unordered_map<uint32_t, uint32_t>> g_freq_a;

    std::vector<std::unordered_map<uint32_t, uint32_t>> g_freq_b;
    std::vector<uint64_t> g_samples;
    std::vector<uint64_t> g_sources_b;

    std::vector<std::map<uint32_t, std::unordered_set<uint32_t>>> g_day_union;

    std::map<uint32_t, std::vector<float>> g_jaccard_day;

    std::vector<std::map<uint32_t, uint64_t>> g_size_hist;
    gzFile g_crawl_gz = nullptr;

    bool reachable_at(uint32_t ip, uint64_t round)
    {
        auto it = g_sessions.find(ip);
        if (it == g_sessions.end())
            return false;
        for (const auto &s : it->second)
            if (s.start <= round && round <= s.end)
                return s.reachable;
        return false;
    }

    bool active_in_window(uint32_t ip, const CrawlWindow &w)
    {
        auto it = g_sessions.find(ip);
        if (it == g_sessions.end())
            return false;
        uint64_t ws = static_cast<uint64_t>(w.start_day) * 86400;
        uint64_t we = static_cast<uint64_t>(w.end_day) * 86400;
        for (const auto &s : it->second)
            if (s.reachable && s.start < we && s.end >= ws)
                return true;
        return false;
    }

    int window_of_day(uint32_t day)
    {
        for (size_t i = 0; i < g_windows.size(); ++i)
            if (day >= g_windows[i].start_day && day < g_windows[i].end_day)
                return static_cast<int>(i);
        return -1;
    }

    std::string dotted_addr(uint32_t ip)
    {
        return std::to_string((ip >> 24) & 255) + "." + std::to_string((ip >> 16) & 255) + "." +
               std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255) + ":8333";
    }
}

namespace
{
    std::vector<std::unordered_set<uint32_t>> g_cur_union;
    std::vector<uint32_t> g_prev_dump;

    void flush_crawl_source()
    {
        for (size_t w = 0; w < g_cur_union.size(); ++w)
        {
            if (g_cur_union[w].empty())
                continue;
            g_sources_b[w]++;
            for (uint32_t addr : g_cur_union[w])
                g_freq_b[w][addr]++;
            g_cur_union[w].clear();
        }
        g_prev_dump.clear();
    }
}

void DataProcessor::stream_cache_file(const std::string &filepath, uint32_t source_ip,
                                      bool source_crawlable)
{
    std::ifstream file(filepath);
    if (!file.is_open())
        return;

    std::string line;
    uint64_t current_round = 0;
    uint32_t current_day = 0;
    uint32_t round_ip_count = 0;
    bool dump_crawl = false;
    int dump_win = -1;
    std::vector<uint32_t> dump_ips;
    std::string gz_line;

    auto close_round = [&]()
    {
        if (current_round != 0 && round_ip_count > 0)
        {
            auto &acc = g_cache_day_acc[current_day];
            acc.total_ips += round_ip_count;
            acc.num_rounds += 1;
        }
        round_ip_count = 0;

        if (dump_crawl && !dump_ips.empty())
        {
            std::sort(dump_ips.begin(), dump_ips.end());
            g_size_hist[dump_win][static_cast<uint32_t>(dump_ips.size())]++;

            if (g_archive && g_crawl_gz)
            {
                gz_line.clear();
                gz_line += std::to_string(current_round);
                gz_line += ',';
                gz_line += std::to_string(current_day);
                gz_line += ',';
                gz_line += std::to_string(source_ip);
                gz_line += ',';
                gz_line += std::to_string(dump_ips.size());
                gz_line += ',';
                for (size_t i = 0; i < dump_ips.size(); ++i)
                {
                    if (i)
                        gz_line += ';';
                    gz_line += std::to_string(dump_ips[i]);
                }
                gz_line += '\n';
                gzwrite(g_crawl_gz, gz_line.data(), static_cast<unsigned>(gz_line.size()));
            }

            if (g_full_size == 0 || dump_ips.size() >= g_full_size)
            {
                g_samples[dump_win]++;
                for (uint32_t addr : dump_ips)
                {
                    g_freq_a[dump_win][addr]++;
                    g_cur_union[dump_win].insert(addr);
                }
                if (!g_prev_dump.empty() && g_prev_dump != dump_ips)
                {
                    size_t inter = 0, i = 0, j = 0;
                    while (i < g_prev_dump.size() && j < dump_ips.size())
                    {
                        if (g_prev_dump[i] < dump_ips[j])
                            i++;
                        else if (g_prev_dump[i] > dump_ips[j])
                            j++;
                        else
                        {
                            inter++;
                            i++;
                            j++;
                        }
                    }
                    size_t uni = g_prev_dump.size() + dump_ips.size() - inter;
                    if (uni > 0)
                        g_jaccard_day[current_day].push_back(
                            static_cast<float>(inter) / static_cast<float>(uni));
                }
                if (g_prev_dump != dump_ips)
                    g_prev_dump = dump_ips;
                auto &du = g_day_union[dump_win][current_day];
                du.insert(dump_ips.begin(), dump_ips.end());
            }
        }
        dump_ips.clear();
        dump_crawl = false;
        dump_win = -1;
    };

    while (std::getline(file, line))
    {
        if (line.empty())
            continue;

        size_t comma_pos = line.find(',');
        if (comma_pos == std::string::npos)
            continue;

        if (comma_pos > 0)
        {
            close_round();
            current_round = std::strtoull(line.c_str(), nullptr, 10);
            current_day = static_cast<uint32_t>(current_round / 86400);
            if (source_crawlable && current_round != 0)
            {
                dump_win = window_of_day(current_day);
                dump_crawl = dump_win >= 0 && reachable_at(source_ip, current_round);
                if (dump_crawl)
                    dump_ips.reserve(1024);
            }
        }
        else
        {
            const char *s = line.c_str() + 1;
            char *end = nullptr;
            uint32_t ip = static_cast<uint32_t>(std::strtoul(s, &end, 10));
            if (end == s)
                continue;
            round_ip_count++;
            daily_cache_ips_[current_day].insert(ip);
            daily_ip_freq_[current_day][ip]++;
            if (dump_crawl)
                dump_ips.push_back(ip);

            if (*end == ',')
            {
                uint64_t nt = std::strtoull(end + 1, nullptr, 10);
                if (nt > 0 && nt <= current_round)
                {
                    auto &acc = g_cache_day_acc[current_day];
                    double age = static_cast<double>(current_round - nt);
                    acc.with_age++;
                    acc.age_sum += age;
                    if (age <= 86400.0)
                        acc.fresh_1d++;
                    if (age <= 7.0 * 86400.0)
                        acc.fresh_7d++;
                }
            }
        }
    }
    close_round();
}

void DataProcessor::build_session_index()
{
    g_sessions.clear();
    g_windows.clear();
    g_last_event = 0;
    uint64_t first_event = UINT64_MAX;

    for (const auto &[ip, by_join] : nodes_meta_data_map)
    {
        auto &ivls = g_sessions[ip];
        for (const auto &[join, node] : by_join)
        {
            ivls.push_back({node.n_time, node.l_time, node.reachable});
            g_last_event = std::max(g_last_event, node.l_time);
            first_event = std::min(first_event, node.n_time);
        }
        std::sort(ivls.begin(), ivls.end(),
                  [](const SessionIvl &a, const SessionIvl &b)
                  { return a.start < b.start; });
    }
    if (g_sessions.empty())
        return;

    uint32_t first_day = static_cast<uint32_t>(first_event / 86400) + 1;
    uint32_t end_day = static_cast<uint32_t>(g_last_event / 86400);
    if (end_day <= first_day)
        return;
    if (end_day - first_day >= 92)
    {
        g_windows.push_back({end_day - 92, end_day - 61, "m1"});
        g_windows.push_back({end_day - 61, end_day - 30, "m2"});
        g_windows.push_back({end_day - 30, end_day, "m3"});
    }
    else
    {
        g_windows.push_back({first_day, end_day, "w0"});
    }

    size_t n = g_windows.size();
    g_freq_a.assign(n, {});
    g_freq_b.assign(n, {});
    g_samples.assign(n, 0);
    g_sources_b.assign(n, 0);
    g_day_union.assign(n, {});
    g_size_hist.assign(n, {});
    g_cur_union.assign(n, {});
    g_jaccard_day.clear();

    std::cout << "Virtual crawler windows (days):";
    for (const auto &w : g_windows)
        std::cout << " " << w.label << "=[" << w.start_day << "," << w.end_day << ")";
    std::cout << std::endl;
}

void DataProcessor::load_cache_data()
{
    g_cache_day_acc.clear();

    if (const char *e = std::getenv("CRAWL_FULL_SIZE"))
        g_full_size = static_cast<uint32_t>(std::strtoul(e, nullptr, 10));
    if (const char *e = std::getenv("CRAWL_ARCHIVE"))
        g_archive = std::strtoul(e, nullptr, 10) != 0;

    std::vector<std::string> dirs;
    if (!caches_path_.empty())
        dirs.push_back(caches_path_);
    for (const auto &data_path : data_paths_)
    {
        std::string auto_dir = "../data/" + data_path + "/caches";
        if (auto_dir != caches_path_ && std::filesystem::exists(auto_dir))
            dirs.push_back(auto_dir);
    }

    struct CacheFile
    {
        std::string path;
        uint64_t bytes;
        uint32_t source_ip;
        uint32_t dir_idx;
    };
    std::vector<CacheFile> cache_files;
    uint64_t total_bytes = 0;
    uint32_t dir_idx = 0;
    for (const auto &dir : dirs)
    {
        if (!std::filesystem::exists(dir))
        {
            std::cerr << "Warning: caches directory missing: " << dir << std::endl;
            dir_idx++;
            continue;
        }
        for (const auto &entry : std::filesystem::directory_iterator(dir))
        {
            if (!entry.is_regular_file())
                continue;
            const std::string fname = entry.path().filename().string();
            char *end = nullptr;
            uint32_t src = static_cast<uint32_t>(std::strtoul(fname.c_str(), &end, 10));
            if (end == fname.c_str() || *end != '\0')
                continue;
            uint64_t sz = entry.file_size();
            cache_files.push_back({entry.path().string(), sz, src, dir_idx});
            total_bytes += sz;
        }
        dir_idx++;
    }
    std::sort(cache_files.begin(), cache_files.end(),
              [](const CacheFile &a, const CacheFile &b)
              {
                  if (a.source_ip != b.source_ip)
                      return a.source_ip < b.source_ip;
                  return a.dir_idx < b.dir_idx;
              });

    const bool crawler_on = !g_windows.empty();
    std::cout << "Streaming " << cache_files.size() << " cache files ("
              << (total_bytes / (1024.0 * 1024.0 * 1024.0)) << " GB)..." << std::endl;
    if (crawler_on)
        std::cout << "Virtual crawler: full-size threshold=" << g_full_size
                  << (g_full_size == 0 ? " (all sizes)" : "")
                  << ", archive=" << (g_archive ? "on" : "off") << std::endl;

    if (crawler_on && g_archive)
    {
        std::string gz_path = output_folder + "synthetic_crawl.csv.gz";
        g_crawl_gz = gzopen(gz_path.c_str(), "wb");
        if (g_crawl_gz)
        {
            const char *hdr = "round,day,source,size,addrs\n";
            gzwrite(g_crawl_gz, hdr, static_cast<unsigned>(strlen(hdr)));
        }
        else
            std::cerr << "Warning: cannot create " << gz_path << " — archive disabled" << std::endl;
    }

    size_t files = 0;
    uint64_t bytes_done = 0;
    uint32_t cur_source = 0;
    bool have_source = false;
    bool cur_crawlable = false;
    const size_t report_every = std::max<size_t>(1, cache_files.size() / 20);
    for (const auto &cf : cache_files)
    {
        if (!have_source || cf.source_ip != cur_source)
        {
            if (have_source)
                flush_crawl_source();
            cur_source = cf.source_ip;
            have_source = true;
            cur_crawlable = false;
            if (crawler_on)
            {
                auto it = g_sessions.find(cur_source);
                if (it != g_sessions.end())
                    for (const auto &s : it->second)
                        if (s.reachable)
                        {
                            cur_crawlable = true;
                            break;
                        }
            }
        }
        stream_cache_file(cf.path, cf.source_ip, cur_crawlable);
        files++;
        bytes_done += cf.bytes;
        if (files % report_every == 0 || files == cache_files.size())
            std::cout << "  caches: " << files << "/" << cache_files.size()
                      << " files, " << (bytes_done / (1024.0 * 1024.0 * 1024.0))
                      << " GB read" << std::endl;
    }
    if (have_source)
        flush_crawl_source();
    if (g_crawl_gz)
    {
        gzclose(g_crawl_gz);
        g_crawl_gz = nullptr;
    }
    if (g_cache_day_acc.empty())
        return;

    for (const auto &[day, acc] : g_cache_day_acc)
    {
        DailyCacheStats dcs;
        dcs.total_ips = acc.total_ips;
        dcs.num_rounds = acc.num_rounds;
        dcs.avg_ips_count = acc.num_rounds > 0 ? static_cast<double>(acc.total_ips) / acc.num_rounds : 0.0;
        dcs.union_size = daily_cache_ips_[day].size();
        if (acc.with_age > 0)
        {
            dcs.avg_age_seconds = acc.age_sum / acc.with_age;
            dcs.pct_fresh_1d = 100.0 * static_cast<double>(acc.fresh_1d) / acc.with_age;
            dcs.pct_fresh_7d = 100.0 * static_cast<double>(acc.fresh_7d) / acc.with_age;
        }
        daily_cache_stats_[day] = dcs;
    }
    g_cache_day_acc.clear();

    std::cout << "Streamed " << files << " cache files covering "
              << daily_cache_stats_.size() << " days" << std::endl;
}

void DataProcessor::save_daily_ip_frequency()
{
    if (daily_ip_freq_.empty())
        return;
    std::ofstream out(output_folder + "daily_ip_frequency.csv");
    if (!out.is_open())
    {
        std::cerr << "Warning: Failed to create daily_ip_frequency.csv" << std::endl;
        return;
    }
    out << "day_index,ip,occurrence_count\n";
    std::vector<uint32_t> days;
    days.reserve(daily_ip_freq_.size());
    for (const auto &[day, _] : daily_ip_freq_)
        days.push_back(day);
    std::sort(days.begin(), days.end());
    for (uint32_t day : days)
    {
        std::vector<std::pair<uint32_t, uint32_t>> ip_counts(
            daily_ip_freq_[day].begin(), daily_ip_freq_[day].end());
        std::sort(ip_counts.begin(), ip_counts.end(),
                  [](const auto &a, const auto &b)
                  { return a.second > b.second; });
        for (const auto &[ip, count] : ip_counts)
            out << day << "," << ip << "," << count << "\n";
    }
    std::cout << "Cache file saved: daily_ip_frequency.csv" << std::endl;
}

void DataProcessor::save_crawler_outputs()
{
    if (g_windows.empty())
        return;

    std::ofstream wf(output_folder + "crawl_windows.csv");
    wf << "label,start_day,end_day,fullsize_samples,contributing_sources\n";
    for (size_t w = 0; w < g_windows.size(); ++w)
        wf << g_windows[w].label << "," << g_windows[w].start_day << ","
           << g_windows[w].end_day << "," << g_samples[w] << "," << g_sources_b[w] << "\n";

    uint64_t total_samples = 0;
    for (uint64_t s : g_samples)
        total_samples += s;
    if (total_samples == 0)
    {
        std::cout << "Virtual crawler: 0 samples passed the full-size filter (threshold "
                  << g_full_size << ") — caches in this run are smaller than the "
                  << "threshold. Rerun with CRAWL_FULL_SIZE=0 (or a lower value) "
                  << "to include them." << std::endl;
        return;
    }

    auto write_freq = [&](const std::unordered_map<uint32_t, uint32_t> &freq,
                          const std::string &fname, const char *col,
                          const CrawlWindow *active_filter)
    {
        std::vector<std::pair<uint32_t, uint32_t>> rows;
        rows.reserve(freq.size());
        for (const auto &[addr, count] : freq)
        {
            if (active_filter && !active_in_window(addr, *active_filter))
                continue;
            rows.push_back({addr, count});
        }
        std::sort(rows.begin(), rows.end(),
                  [](const auto &a, const auto &b)
                  { return a.second > b.second; });
        std::ofstream out(output_folder + fname);
        out << "," << col << "\n";
        for (const auto &[addr, count] : rows)
            out << dotted_addr(addr) << "," << count << "\n";
    };

    for (size_t w = 0; w < g_windows.size(); ++w)
    {
        const auto &win = g_windows[w];
        write_freq(g_freq_a[w], "crawl_freq_" + win.label + "_A_all.csv", "freq_a", nullptr);
        write_freq(g_freq_a[w], "crawl_freq_" + win.label + "_A_active.csv", "freq_a", &win);
        write_freq(g_freq_b[w], "crawl_freq_" + win.label + "_B_all.csv", "freq_b", nullptr);
        write_freq(g_freq_b[w], "crawl_freq_" + win.label + "_B_active.csv", "freq_b", &win);
    }

    {
        std::ofstream out(output_folder + "crawl_response_sizes.csv");
        out << "window,size,count\n";
        for (size_t w = 0; w < g_windows.size(); ++w)
            for (const auto &[size, count] : g_size_hist[w])
                out << g_windows[w].label << "," << size << "," << count << "\n";
    }

    {
        std::ofstream out(output_folder + "crawl_discovery_growth.csv");
        out << "window,day,daily_union,cumulative_union\n";
        for (size_t w = 0; w < g_windows.size(); ++w)
        {
            std::unordered_set<uint32_t> cumulative;
            for (const auto &[day, addrs] : g_day_union[w])
            {
                cumulative.insert(addrs.begin(), addrs.end());
                out << g_windows[w].label << "," << day << "," << addrs.size()
                    << "," << cumulative.size() << "\n";
            }
        }
    }

    {
        std::ofstream out(output_folder + "crawl_jaccard_daily.csv");
        out << "day,pairs,mean,p25,median,p75\n";
        for (auto &[day, vals] : g_jaccard_day)
        {
            std::sort(vals.begin(), vals.end());
            double mean = 0;
            for (float v : vals)
                mean += v;
            mean /= vals.size();
            auto q = [&](double p)
            { return vals[std::min(vals.size() - 1,
                                   static_cast<size_t>(p * vals.size()))]; };
            out << day << "," << vals.size() << "," << mean << "," << q(0.25)
                << "," << q(0.5) << "," << q(0.75) << "\n";
        }
    }

    for (size_t w = 0; w < g_windows.size(); ++w)
        std::cout << "Virtual crawler [" << g_windows[w].label << "]: "
                  << g_samples[w] << " full-size samples from " << g_sources_b[w]
                  << " sources, " << g_freq_a[w].size() << " distinct addresses"
                  << std::endl;
    std::cout << "Saved crawl_windows.csv, crawl_freq_*.csv, crawl_response_sizes.csv, "
              << "crawl_discovery_growth.csv, crawl_jaccard_daily.csv"
              << (g_archive ? ", synthetic_crawl.csv.gz" : "") << std::endl;
}

DailyCacheStats DataProcessor::get_cache_stats_for_day(uint32_t day) const
{
    auto it = daily_cache_stats_.find(day);
    if (it != daily_cache_stats_.end())
    {
        return it->second;
    }
    return DailyCacheStats{0.0, 0, 0, 0};
}
void DataProcessor::initialize_paths()
{
    for (const auto &data_path : data_paths_)
    {
        std::string input_folder_ = "../data/" + data_path + "/";
        nodes_files.push_back(input_folder_ + "nodes");
        edges_folders.push_back(input_folder_ + "edges/");
        info_folders.push_back(input_folder_ + "info/");
        map_info_folders.push_back(input_folder_ + "map_info/");
    }

    output_folder = output_path_ + data_paths_[0] + "/";
}

std::vector<MapInfoEntry> DataProcessor::load_map_info_entries(uint32_t node_ip)
{
    std::vector<MapInfoEntry> entries;
    std::string node_str = std::to_string(node_ip);

    auto dir_it = node_ip_source_dirs_.find(node_ip);
    if (dir_it == node_ip_source_dirs_.end())
        return entries;

    for (uint32_t dir_idx : dir_it->second)
    {
        if (dir_idx >= map_info_folders.size())
            continue;
        std::string file_path = map_info_folders[dir_idx] + node_str;

        try
        {
            std::string data = load_data(file_path);
            std::istringstream stream(data);
            std::string line;

            while (std::getline(stream, line))
            {
                if (line.empty())
                    continue;

                std::vector<uint64_t> cols;
                std::stringstream field_stream(line);
                std::string field;
                while (std::getline(field_stream, field, ','))
                {
                    if (field.empty())
                        continue;
                    try
                    {
                        cols.push_back(std::stoull(field));
                    }
                    catch (const std::exception &)
                    {
                        cols.clear();
                        break;
                    }
                }
                if (cols.size() < 2)
                    continue;

                MapInfoEntry e;
                e.first_seen = cols[0];
                e.ip = static_cast<uint32_t>(cols[1]);
                if (cols.size() >= 6)
                {
                    e.in_tried = cols[2] != 0;
                    e.n_attempts = static_cast<uint16_t>(cols[3]);
                    e.m_last_success = cols[4];
                    e.n_ref_count = static_cast<uint32_t>(cols[5]);
                }
                entries.push_back(e);
            }
        }
        catch (const std::exception &e)
        {
        }
    }

    return entries;
}

TableQuality DataProcessor::summarize_table(const std::vector<MapInfoEntry> &entries)
{
    TableQuality q;
    q.entries = entries.size();
    if (entries.empty())
        return q;
    uint64_t attempts = 0, succeeded = 0, refs = 0;
    for (const auto &e : entries)
    {
        if (e.in_tried)
            q.tried++;
        attempts += e.n_attempts;
        refs += e.n_ref_count;
        if (e.m_last_success > 0)
            succeeded++;
    }
    q.mean_attempts = static_cast<double>(attempts) / entries.size();
    q.pct_ever_success = 100.0 * static_cast<double>(succeeded) / entries.size();
    q.mean_ref_count = static_cast<double>(refs) / entries.size();
    return q;
}

std::pair<double, double> DataProcessor::calculate_network_presence(
    const std::vector<uint32_t> &map_info_addresses,
    const std::vector<uint32_t> &nodes_in_network)
{
    if (map_info_addresses.empty())
    {
        return {0.0, 0.0};
    }

    std::unordered_set<uint32_t> network_set(nodes_in_network.begin(), nodes_in_network.end());

    uint32_t in_network = 0;
    uint32_t not_in_network = 0;

    for (const auto &addr : map_info_addresses)
    {
        if (network_set.find(addr) != network_set.end())
        {
            in_network++;
        }
        else
        {
            not_in_network++;
        }
    }

    double total = static_cast<double>(map_info_addresses.size());
    double pct_in_network = (static_cast<double>(in_network) / total) * 100.0;
    double pct_not_in_network = (static_cast<double>(not_in_network) / total) * 100.0;

    return {pct_in_network, pct_not_in_network};
}

void DataProcessor::process()
{
    std::cout << "Initializing output directory..." << std::endl;
    if (!create_directory(output_folder))
    {
        throw std::runtime_error("Failed to create output directory: " + output_folder);
    }

    for (const auto &entry : std::filesystem::directory_iterator(output_folder))
    {
        if (entry.path().extension() == ".csv" || entry.path().extension() == ".gz")
            std::filesystem::remove(entry.path());
    }
    std::cout << "Output directory ready: " << output_folder << std::endl
              << std::endl;

    std::cout << "Loading nodes..." << std::endl;
    std::vector<Node> nodes = load_nodes();
    std::cout << "Loaded " << nodes.size() << " nodes" << std::endl;

    std::cout << "Building session index for virtual crawler..." << std::endl;
    build_session_index();

    std::cout << "Streaming caches..." << std::endl;
    load_cache_data();

    std::cout << "Loading edges..." << std::endl;
    std::vector<Edge> edges = load_edges();
    std::cout << "Loaded " << edges.size() << " edges" << std::endl;

    std::cout << "Processing graph metrics..." << std::endl;
    process_graph_metrics(nodes, edges);
    std::vector<Edge>().swap(edges);

    std::cout << "Processing lifecycle (churn/sessions)..." << std::endl;
    load_ip_rotation_events();
    process_lifecycle_metrics(nodes);

    std::cout << "Processing telemetry..." << std::endl;
    process_telemetry();

    std::cout << "Processing ip changes..." << std::endl;
    process_ip_changes();

    std::cout << "Processing local views..." << std::endl;

    if (!daily_cache_stats_.empty())
    {
        std::cout << "Saving cache analysis files..." << std::endl;

        save_daily_cache_metrics();

        save_daily_ip_frequency();
    }

    save_crawler_outputs();

    if (snapshot_days_written_ > 0)
        std::cout << "Wrote " << snapshot_days_written_
                  << " daily snapshots + degree histograms (incremental)" << std::endl;

    std::cout << "Processing complete!" << std::endl;
}

std::vector<Node> DataProcessor::load_nodes()
{

    struct PairHash
    {
        size_t operator()(const std::pair<uint32_t, uint64_t> &p) const
        {
            size_t h1 = std::hash<uint32_t>{}(p.first);
            size_t h2 = std::hash<uint64_t>{}(p.second);
            return h1 ^ (h2 * 2654435761ULL + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
        }
    };
    std::unordered_map<std::pair<uint32_t, uint64_t>, Node, PairHash> best;

    for (size_t file_idx = 0; file_idx < nodes_files.size(); ++file_idx)
    {
        std::string data = load_data(nodes_files[file_idx]);
        std::vector<Node> parsed = ParsingUtils::parse_nodes(data);
        std::cout << "  nodes: " << nodes_files[file_idx] << " -> "
                  << parsed.size() << " session rows" << std::endl;
        for (auto &n : parsed)
        {
            n.source_index = static_cast<uint32_t>(file_idx);

            node_ip_source_dirs_[n.ip].insert(static_cast<uint32_t>(file_idx));
            auto key = std::make_pair(n.unique_id, n.n_time);
            auto it = best.find(key);
            if (it == best.end() || n.l_time > it->second.l_time)
                best[key] = n;
        }
    }

    std::vector<Node> _nodes;
    _nodes.reserve(best.size());
    for (auto &[k, n] : best)
    {
        _nodes.push_back(n);
        nodes_meta_data_map[n.ip][n.n_time] = n;
        ip_class_[n.ip] = n.reachable;
    }
    return _nodes;
}
std::vector<Edge> DataProcessor::load_edges()
{
    std::vector<Edge> _edges;
    for (const auto &edges_folder : edges_folders)
    {
        std::vector<std::string> edges_files = load_files_in_path(edges_folder);
        std::cout << "  edges: " << edges_files.size() << " files in "
                  << edges_folder << std::endl;
        const size_t report_every = std::max<size_t>(1, edges_files.size() / 10);
        size_t done = 0;
        for (const auto &file : edges_files)
        {
            uint32_t source_ip;
            try
            {
                source_ip = static_cast<std::uint32_t>(std::stoul(file));
            }
            catch (const std::exception &)
            {
                continue;
            }
            std::string data = load_data(edges_folder + file);
            std::vector<Edge> parsed = ParsingUtils::parse_edges(source_ip, data);
            _edges.insert(_edges.end(), parsed.begin(), parsed.end());
            done++;
            if (done % report_every == 0 || done == edges_files.size())
                std::cout << "  edges: " << done << "/" << edges_files.size()
                          << " files, " << _edges.size() << " events" << std::endl;
        }
    }
    return _edges;
}
void DataProcessor::process_graph_metrics(std::vector<Node> &nodes, std::vector<Edge> &edges)
{
    Graph graph;
    std::vector<std::vector<double>> metrics_data;
    std::vector<std::vector<double>> node_metadata;
    std::vector<std::vector<double>> non_gcc_data;

    uint32_t cached_gcc = 0;
    uint32_t cached_num_components = 0;
    uint32_t cached_diameter = 0;
    double cached_assortativity = 0.0;
    bool first_iteration = true;
    uint64_t last_computed_slot = UINT64_MAX;

    constexpr uint64_t SECONDS_PER_DAY = 86400;
    uint64_t previous_day = 0;
    std::unordered_set<uint32_t> previous_day_nodes;
    uint64_t daily_intersection = 0;

    std::stable_sort(edges.begin(), edges.end(),
                     [](const Edge &a, const Edge &b)
                     { return a.round < b.round; });
    std::vector<uint32_t> join_order(nodes.size()), leave_order(nodes.size());
    for (uint32_t i = 0; i < nodes.size(); i++)
        join_order[i] = leave_order[i] = i;
    std::stable_sort(join_order.begin(), join_order.end(),
                     [&](uint32_t a, uint32_t b)
                     { return nodes[a].n_time < nodes[b].n_time; });
    std::stable_sort(leave_order.begin(), leave_order.end(),
                     [&](uint32_t a, uint32_t b)
                     { return nodes[a].l_time < nodes[b].l_time; });

    const size_t NN = nodes.size(), NE = edges.size();
    size_t ji = 0, li = 0, ei = 0;

    uint64_t last_element = 1;
    if (NN > 0)
        last_element = std::max(last_element, nodes[leave_order.back()].l_time);
    if (NE > 0)
        last_element = std::max(last_element, edges.back().round);

    create_directory(output_folder + "snapshots/");
    degree_csv_.open(output_folder + "degree_dist_daily.csv");
    degree_csv_ << "day_index,degree,count_public,count_nat\n";

    while (ji < NN || li < NN || ei < NE)
    {
        uint64_t current_time = UINT64_MAX;
        if (ji < NN)
            current_time = std::min(current_time, nodes[join_order[ji]].n_time);
        if (li < NN)
            current_time = std::min(current_time, nodes[leave_order[li]].l_time);
        if (ei < NE)
            current_time = std::min(current_time, edges[ei].round);

        while (ji < NN && nodes[join_order[ji]].n_time == current_time)
        {
            graph.add_node(nodes[join_order[ji]].ip);
            ji++;
        }

        size_t e_end = ei;
        while (e_end < NE && edges[e_end].round == current_time)
            e_end++;
        for (size_t k = ei; k < e_end; k++)
        {
            if (!edges[k].active)
                continue;
            if (graph.add_edge(edges[k].source, edges[k].destination))
            {
                outbound_edge_count_[edges[k].source]++;
                inbound_edge_count_[edges[k].destination]++;
                uint32_t deg_src = graph.get_node_degree(edges[k].source);
                uint32_t deg_dst = graph.get_node_degree(edges[k].destination);
                max_degree_[edges[k].source] = std::max(max_degree_[edges[k].source], deg_src);
                max_degree_[edges[k].destination] = std::max(max_degree_[edges[k].destination], deg_dst);
            }
        }
        for (size_t k = ei; k < e_end; k++)
        {
            if (!edges[k].active)
                graph.remove_edge(edges[k].source, edges[k].destination);
        }
        ei = e_end;

        size_t l_end = li;
        while (l_end < NN && nodes[leave_order[l_end]].l_time == current_time)
            l_end++;

        double avg_leaving_map_info = 0.0;
        if (l_end > li)
        {
            uint64_t total_map_info = 0;
            for (size_t k = li; k < l_end; k++)
                total_map_info += nodes[leave_order[k]].map_info;
            avg_leaving_map_info = static_cast<double>(total_map_info) / (l_end - li);
        }

        std::vector<uint32_t> nodes_in_graph = graph.get_all_nodes();

        uint32_t event_day = static_cast<uint32_t>(current_time / SECONDS_PER_DAY);
        daily_all_network_nodes_[event_day].insert(nodes_in_graph.begin(), nodes_in_graph.end());

        for (size_t k = li; k < l_end; k++)
        {
            const Node &node = nodes[leave_order[k]];

            std::vector<MapInfoEntry> table_entries = load_map_info_entries(node.ip);
            std::vector<uint32_t> map_info_addresses;
            map_info_addresses.reserve(table_entries.size());
            for (const auto &e : table_entries)
                map_info_addresses.push_back(e.ip);
            auto [pct_in_network, pct_not_in_network] = calculate_network_presence(map_info_addresses, nodes_in_graph);
            TableQuality tq = summarize_table(table_entries);

            uint32_t leaving_ip = node.ip;
            int64_t degree = graph.remove_node(leaving_ip);
            if (degree >= 0)
            {
                node_metadata.push_back({static_cast<double>(node.unique_id),
                                         static_cast<double>(node.ip),
                                         static_cast<double>(degree),
                                         static_cast<double>(node.n_time),
                                         pct_in_network,
                                         pct_not_in_network,
                                         static_cast<double>(max_degree_[node.ip]),
                                         static_cast<double>(outbound_edge_count_[node.ip]),
                                         static_cast<double>(inbound_edge_count_[node.ip]),
                                         tq.mean_attempts,
                                         tq.pct_ever_success,
                                         tq.mean_ref_count});
            }
        }
        li = l_end;

        nodes_in_graph = graph.get_all_nodes();
        uint32_t gcc, diameter;

        uint64_t current_day = current_time / SECONDS_PER_DAY;
        if (current_day != previous_day && previous_day != 0)
        {

            daily_intersection = 0;
            for (const auto &node : nodes_in_graph)
            {
                if (previous_day_nodes.find(node) != previous_day_nodes.end())
                {
                    daily_intersection++;
                }
            }

            daily_network_nodes_[static_cast<uint32_t>(previous_day)].insert(
                previous_day_nodes.begin(), previous_day_nodes.end());

            write_daily_snapshot(static_cast<uint32_t>(previous_day), graph);

            previous_day_nodes.clear();
            previous_day_nodes.insert(nodes_in_graph.begin(), nodes_in_graph.end());
        }
        else if (previous_day == 0)
        {

            previous_day_nodes.insert(nodes_in_graph.begin(), nodes_in_graph.end());
        }
        previous_day = current_day;

        uint64_t current_slot = current_time / computation_interval_;
        bool should_compute = first_iteration ||
                              current_slot > last_computed_slot ||
                              (ji >= NN && li >= NN && ei >= NE);
        if (should_compute)
            last_computed_slot = current_slot;

        std::vector<uint32_t> non_gcc_nodes;
        compute_and_cache_metrics(
            graph,
            nodes_in_graph,
            cached_gcc,
            cached_num_components,
            cached_diameter,
            cached_assortativity,
            should_compute,
            should_compute ? &non_gcc_nodes : nullptr);

        gcc = cached_gcc;
        diameter = cached_diameter;

        if (should_compute)
        {
            for (uint32_t node_ip : non_gcc_nodes)
            {
                non_gcc_data.push_back({static_cast<double>(current_time),
                                        static_cast<double>(node_ip)});
            }
        }

        if (non_gcc_data.size() >= BATCH_SIZE)
        {
            write_csv(
                output_folder + "non_gcc_nodes.csv",
                non_gcc_data,
                {"timestamp", "node_ip"});
            non_gcc_data.clear();
        }

        if (metrics_data.size() >= BATCH_SIZE)
        {
            std::cout << (static_cast<double>(current_time) / last_element) * 100 << "%" << std::endl;
            write_csv(
                output_folder + "metrics.csv",
                metrics_data,
                {"timestamp", "edge_count", "node_count", "gcc_size", "num_components", "est_diameter", "avg_degree", "daily_node_overlap", "avg_departing_addr_map_size", "dns_cache_avg_ips_per_round", "dns_cache_total_ip_entries", "dns_cache_round_count", "dns_cache_unique_ip_count", "assortativity"});
            metrics_data.clear();
        }

        if (node_metadata.size() >= BATCH_SIZE)
        {
            std::cout << (static_cast<double>(current_time) / last_element) * 100 << "%" << std::endl;
            std::vector<std::vector<double>> node_meta_data_to_save;
            for (auto &i : node_metadata)
            {
                auto __n = nodes_meta_data_map[static_cast<uint32_t>(i[1])][static_cast<uint64_t>(i[3])];
                node_meta_data_to_save.push_back({
                    static_cast<double>(__n.unique_id),
                    static_cast<double>(__n.ip),
                    static_cast<double>(__n.reachable),
                    static_cast<double>(__n.asmap),
                    i[2],
                    static_cast<double>(__n.n_time),
                    static_cast<double>(__n.l_time),
                    static_cast<double>(__n.map_info),
                    static_cast<double>(__n.vvNew),
                    static_cast<double>(__n.vvTried),
                    i[4],
                    i[5],
                    i[6],
                    i[7],
                    i[8],
                    i[9],
                    i[10],
                    i[11]
                });
            }
            write_csv(
                output_folder + "node_meta_data.csv",
                node_meta_data_to_save,
                {"node_id", "ip", "is_reachable", "asn", "degree_at_departure", "join_timestamp", "leave_timestamp", "addr_map_size", "addrman_new_size", "addrman_tried_size", "pct_addr_map_in_network", "pct_addr_map_outside_network", "max_degree", "outbound_edges", "inbound_edges", "mean_attempts", "pct_ever_success", "mean_ref_count"});
            node_metadata.clear();
        }

        double avg_degree = nodes_in_graph.empty() ? 0.0 : (2.0 * graph.get_edge_count()) / nodes_in_graph.size();

        DailyCacheStats cache_stats = get_cache_stats_for_day(static_cast<uint32_t>(current_day));

        metrics_data.push_back({static_cast<double>(current_time),
                                static_cast<double>(graph.get_edge_count()),
                                static_cast<double>(nodes_in_graph.size()),
                                static_cast<double>(gcc),
                                static_cast<double>(cached_num_components),
                                static_cast<double>(diameter),
                                avg_degree,
                                static_cast<double>(daily_intersection),
                                avg_leaving_map_info,
                                cache_stats.avg_ips_count,
                                static_cast<double>(cache_stats.total_ips),
                                static_cast<double>(cache_stats.num_rounds),
                                static_cast<double>(cache_stats.union_size),
                                cached_assortativity});

        first_iteration = false;
    }

    if (previous_day != 0 && !previous_day_nodes.empty())
    {
        daily_network_nodes_[static_cast<uint32_t>(previous_day)].insert(
            previous_day_nodes.begin(), previous_day_nodes.end());
        write_daily_snapshot(static_cast<uint32_t>(previous_day), graph);
    }
    degree_csv_.close();

    if (!non_gcc_data.empty())
    {
        write_csv(
            output_folder + "non_gcc_nodes.csv",
            non_gcc_data,
            {"timestamp", "node_ip"});
    }

    if (!metrics_data.empty())
    {
        write_csv(
            output_folder + "metrics.csv",
            metrics_data, {"timestamp", "edge_count", "node_count", "gcc_size", "num_components", "est_diameter", "avg_degree", "daily_node_overlap", "avg_departing_addr_map_size", "dns_cache_avg_ips_per_round", "dns_cache_total_ip_entries", "dns_cache_round_count", "dns_cache_unique_ip_count", "assortativity"});
    }

    if (!node_metadata.empty())
    {
        std::vector<std::vector<double>> node_meta_data_to_save;
        for (auto &i : node_metadata)
        {
            auto __n = nodes_meta_data_map[static_cast<uint32_t>(i[1])][static_cast<uint64_t>(i[3])];
            node_meta_data_to_save.push_back({
                static_cast<double>(__n.unique_id),
                static_cast<double>(__n.ip),
                static_cast<double>(__n.reachable),
                static_cast<double>(__n.asmap),
                i[2],
                static_cast<double>(__n.n_time),
                static_cast<double>(__n.l_time),
                static_cast<double>(__n.map_info),
                static_cast<double>(__n.vvNew),
                static_cast<double>(__n.vvTried),
                i[4],
                i[5],
                i[6],
                i[7],
                i[8],
                i[9],
                i[10],
                i[11]
            });
        }
        write_csv(
            output_folder + "node_meta_data.csv",
            node_meta_data_to_save,
            {"node_id", "ip", "is_reachable", "asn", "degree_at_departure", "join_timestamp", "leave_timestamp", "addr_map_size", "addrman_new_size", "addrman_tried_size", "pct_addr_map_in_network", "pct_addr_map_outside_network", "max_degree", "outbound_edges", "inbound_edges", "mean_attempts", "pct_ever_success", "mean_ref_count"});
        node_metadata.clear();
    }
}

void DataProcessor::compute_and_cache_metrics(
    Graph &graph,
    const std::vector<uint32_t> &nodes_in_graph,
    uint32_t &cached_gcc,
    uint32_t &cached_num_components,
    uint32_t &cached_diameter,
    double &cached_assortativity,
    bool force_computation,
    std::vector<uint32_t> *non_gcc_nodes)
{
    if (!force_computation)
    {
        return;
    }

    auto components = graph.get_all_components();
    cached_num_components = static_cast<uint32_t>(components.size());
    cached_gcc = components.empty() ? 0 : static_cast<uint32_t>(components[0].size());
    cached_assortativity = graph.get_assortativity();

    if (non_gcc_nodes != nullptr)
    {
        non_gcc_nodes->clear();
        for (size_t i = 1; i < components.size(); i++)
        {
            for (uint32_t node : components[i])
            {
                non_gcc_nodes->push_back(node);
            }
        }
    }

    if (!nodes_in_graph.empty())
    {
        size_t sample_size = std::min(250UL, static_cast<size_t>(graph.get_node_count()));
        std::vector<uint32_t> sampled_nodes = graph.random_subset(sample_size, random_seed_);

        cached_diameter = 0;
        for (const auto &node : sampled_nodes)
        {
            uint32_t node_copy = node;
            cached_diameter = std::max(
                cached_diameter,
                graph.max_distance_from(node_copy));
        }
    }
    else
    {
        cached_diameter = 0;
    }
}

void DataProcessor::process_local_views(const std::vector<Node> &nodes)
{
    std::vector<std::vector<uint32_t>> info_data;
    for (auto &node : nodes)
    {
        std::string data = load_data(info_folders[0] + std::to_string(node.ip));
        Info parsed = ParsingUtils::parse_info(data, node.ip);
        info_data.push_back({node.ip, static_cast<uint32_t>(parsed.known_peers.size())});
        if (info_data.size() >= BATCH_SIZE)
        {
            write_csv(
                output_folder + "local_views.csv",
                info_data,
                {"peer_ip", "known_peer_count"});
            info_data.clear();
        }
    }
    if (!info_data.empty())
    {
        write_csv(
            output_folder + "local_views.csv",
            info_data,
            {"peer_ip", "known_peer_count"});
    }
}

void DataProcessor::save_daily_cache_metrics()
{
    if (daily_cache_stats_.empty())
    {
        return;
    }

    std::ofstream out(output_folder + "daily_cache_metrics.csv");
    if (!out.is_open())
    {
        std::cerr << "Warning: Failed to create daily_cache_metrics.csv" << std::endl;
        return;
    }

    out << "day_index,avg_ips_per_crawl_round,total_ip_entries,crawl_round_count,unique_cached_ip_count,"
        << "network_size_eod,cached_ips_in_network_eod,cached_ips_outside_network_eod,pct_cached_in_network_eod,pct_cached_outside_network_eod,"
        << "network_size_daily_union,cached_ips_in_network_daily_union,cached_ips_outside_network_daily_union,pct_cached_in_network_daily_union,pct_cached_outside_network_daily_union,"
        << "avg_addr_age_seconds,pct_addr_fresh_1d,pct_addr_fresh_7d\n";

    std::vector<uint32_t> days;
    for (const auto &[day, _] : daily_cache_stats_)
    {
        days.push_back(day);
    }
    std::sort(days.begin(), days.end());

    std::vector<uint32_t> network_days;
    for (const auto &[day, _] : daily_network_nodes_)
    {
        network_days.push_back(day);
    }
    std::sort(network_days.begin(), network_days.end());

    for (uint32_t day : days)
    {
        const auto &cache_stats = daily_cache_stats_[day];
        const auto &cache_ips = daily_cache_ips_[day];

        const std::unordered_set<uint32_t> *network_nodes = nullptr;

        auto it = daily_network_nodes_.find(day);
        if (it != daily_network_nodes_.end())
        {
            network_nodes = &(it->second);
        }
        else
        {

            for (uint32_t net_day : network_days)
            {
                if (net_day > day)
                {
                    network_nodes = &(daily_network_nodes_[net_day]);
                    break;
                }
            }
        }

        const std::unordered_set<uint32_t> *all_network_nodes = nullptr;
        auto all_it = daily_all_network_nodes_.find(day);
        if (all_it != daily_all_network_nodes_.end())
        {
            all_network_nodes = &(all_it->second);
        }

        uint64_t network_nodes_eod = (network_nodes != nullptr) ? network_nodes->size() : 0;
        uint64_t network_nodes_all_size = (all_network_nodes != nullptr) ? all_network_nodes->size() : 0;

        uint64_t in_network = 0;
        uint64_t not_in_network = 0;
        double pct_in = 0.0;
        double pct_not = 0.0;

        uint64_t in_network_all = 0;
        uint64_t not_in_network_all = 0;
        double pct_in_all = 0.0;
        double pct_not_all = 0.0;

        if (!cache_ips.empty())
        {
            for (uint32_t ip : cache_ips)
            {

                if (network_nodes != nullptr && network_nodes->find(ip) != network_nodes->end())
                {
                    in_network++;
                }
                else
                {
                    not_in_network++;
                }

                if (all_network_nodes != nullptr && all_network_nodes->find(ip) != all_network_nodes->end())
                {
                    in_network_all++;
                }
                else
                {
                    not_in_network_all++;
                }
            }

            double total = static_cast<double>(cache_ips.size());
            if (network_nodes != nullptr)
            {
                pct_in = (static_cast<double>(in_network) / total) * 100.0;
                pct_not = (static_cast<double>(not_in_network) / total) * 100.0;
            }
            else
            {
                not_in_network = cache_ips.size();
                pct_not = 100.0;
            }
            if (all_network_nodes != nullptr)
            {
                pct_in_all = (static_cast<double>(in_network_all) / total) * 100.0;
                pct_not_all = (static_cast<double>(not_in_network_all) / total) * 100.0;
            }
            else
            {
                not_in_network_all = cache_ips.size();
                pct_not_all = 100.0;
            }
        }

        out << day << ","
            << cache_stats.avg_ips_count << ","
            << cache_stats.total_ips << ","
            << cache_stats.num_rounds << ","
            << cache_stats.union_size << ","
            << network_nodes_eod << ","
            << in_network << ","
            << not_in_network << ","
            << pct_in << ","
            << pct_not << ","
            << network_nodes_all_size << ","
            << in_network_all << ","
            << not_in_network_all << ","
            << pct_in_all << ","
            << pct_not_all << ","
            << cache_stats.avg_age_seconds << ","
            << cache_stats.pct_fresh_1d << ","
            << cache_stats.pct_fresh_7d << "\n";
    }

    std::cout << "Saved daily_cache_metrics.csv with network comparison" << std::endl;
}

void DataProcessor::process_lifecycle_metrics(const std::vector<Node> &nodes)
{
    if (nodes.empty())
        return;
    constexpr uint64_t DAY = 86400;

    uint64_t last_event = 0;
    for (const auto &n : nodes)
        last_event = std::max(last_event, n.l_time);

    struct DayAcc
    {
        uint64_t joins = 0, joins_pub = 0;
        uint64_t leaves = 0, leaves_pub = 0;
    };
    std::map<uint32_t, DayAcc> days;

    std::unordered_map<uint32_t, std::vector<const Node *>> by_uid;
    for (const auto &n : nodes)
        by_uid[n.unique_id].push_back(&n);

    std::vector<Node> sessions;
    std::vector<uint32_t> rotations;
    sessions.reserve(by_uid.size());
    rotations.reserve(by_uid.size());
    for (auto &[uid, frags] : by_uid)
    {
        std::sort(frags.begin(), frags.end(),
                  [](const Node *a, const Node *b)
                  { return a->n_time < b->n_time; });
        auto rot = ip_rotation_events_.find(uid);
        const bool has_rot = (rot != ip_rotation_events_.end());
        size_t i = 0;
        while (i < frags.size())
        {
            Node s = *frags[i];
            size_t j = i;
            while (j + 1 < frags.size() && has_rot &&
                   rot->second.count(frags[j]->l_time) &&
                   frags[j + 1]->n_time == frags[j]->l_time)
                ++j;

            s.l_time = frags[j]->l_time;
            s.map_info = frags[j]->map_info;
            s.vvNew = frags[j]->vvNew;
            s.vvTried = frags[j]->vvTried;
            sessions.push_back(s);
            rotations.push_back(static_cast<uint32_t>(j - i));
            i = j + 1;
        }
    }
    if (sessions.size() != nodes.size())
        std::cout << "  stitched " << nodes.size() << " address-lifetime rows -> "
                  << sessions.size() << " sessions" << std::endl;

    std::ofstream sess(output_folder + "session_lengths.csv");
    sess << "node_id,ip,is_reachable,join_timestamp,leave_timestamp,session_seconds,censored,ip_rotations\n";

    for (size_t k = 0; k < sessions.size(); ++k)
    {
        const Node &n = sessions[k];
        bool censored = n.l_time >= last_event;
        uint32_t jd = static_cast<uint32_t>(n.n_time / DAY);
        auto &ja = days[jd];
        ja.joins++;
        if (n.reachable)
            ja.joins_pub++;
        if (!censored)
        {
            uint32_t ld = static_cast<uint32_t>(n.l_time / DAY);
            auto &la = days[ld];
            la.leaves++;
            if (n.reachable)
                la.leaves_pub++;
        }
        sess << n.unique_id << "," << n.ip << "," << (n.reachable ? 1 : 0) << ","
             << n.n_time << "," << n.l_time << "," << (n.l_time - n.n_time) << ","
             << (censored ? 1 : 0) << "," << rotations[k] << "\n";
    }

    std::ofstream out(output_folder + "churn_daily.csv");
    out << "day_index,joins,joins_public,joins_nat,leaves,leaves_public,leaves_nat,"
        << "live_eod,live_public_eod,live_nat_eod,churn_pct\n";

    int64_t live = 0, live_pub = 0;
    int64_t live_prev = 0;
    for (const auto &[day, a] : days)
    {
        live += static_cast<int64_t>(a.joins) - static_cast<int64_t>(a.leaves);
        live_pub += static_cast<int64_t>(a.joins_pub) - static_cast<int64_t>(a.leaves_pub);
        double churn = live_prev > 0 ? 100.0 * static_cast<double>(a.leaves) / live_prev : 0.0;
        out << day << "," << a.joins << "," << a.joins_pub << "," << (a.joins - a.joins_pub) << ","
            << a.leaves << "," << a.leaves_pub << "," << (a.leaves - a.leaves_pub) << ","
            << live << "," << live_pub << "," << (live - live_pub) << ","
            << churn << "\n";
        live_prev = live;
    }
    std::cout << "Saved churn_daily.csv + session_lengths.csv (" << days.size() << " days)" << std::endl;
}

void DataProcessor::process_telemetry()
{
    constexpr uint64_t DAY = 86400;
    struct TAcc
    {
        uint64_t rows = 0, fs = 0, ff = 0, ga = 0, ad = 0;
    };
    std::map<uint32_t, TAcc> days;

    for (const auto &data_path : data_paths_)
    {
        std::string path = "../data/" + data_path + "/telemetry.csv";
        std::ifstream file(path);
        if (!file.is_open())
            continue;
        std::string line;
        std::getline(file, line);
        while (std::getline(file, line))
        {
            if (line.empty())
                continue;
            std::vector<uint64_t> cols;
            std::stringstream fs_(line);
            std::string field;
            bool bad = false;
            while (std::getline(fs_, field, ','))
            {
                try
                {
                    cols.push_back(std::stoull(field));
                }
                catch (const std::exception &)
                {
                    bad = true;
                    break;
                }
            }
            if (bad || cols.size() != 6)
                continue;
            auto &a = days[static_cast<uint32_t>(cols[0] / DAY)];
            a.rows++;
            a.fs += cols[2];
            a.ff += cols[3];
            a.ga += cols[4];
            a.ad += cols[5];
        }
    }
    if (days.empty())
        return;

    std::ofstream out(output_folder + "telemetry_daily.csv");
    out << "day_index,reporting_nodes,feeler_success,feeler_fail,feeler_success_rate_pct,"
        << "getaddr_served,addr_dropped\n";
    for (const auto &[day, a] : days)
    {
        double rate = (a.fs + a.ff) > 0 ? 100.0 * static_cast<double>(a.fs) / (a.fs + a.ff) : 0.0;
        out << day << "," << a.rows << "," << a.fs << "," << a.ff << "," << rate << ","
            << a.ga << "," << a.ad << "\n";
    }
    std::cout << "Saved telemetry_daily.csv (" << days.size() << " days)" << std::endl;
}

void DataProcessor::load_ip_rotation_events()
{
    ip_rotation_events_.clear();
    size_t rows = 0;
    for (const auto &data_path : data_paths_)
    {
        std::string path = "../data/" + data_path + "/ip_changes/ip_changes.csv";
        std::ifstream file(path);
        if (!file.is_open())
            continue;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty())
                continue;

            std::vector<uint64_t> cols;
            std::stringstream fs_(line);
            std::string field;
            bool bad = false;
            while (std::getline(fs_, field, ','))
            {
                try
                {
                    cols.push_back(std::stoull(field));
                }
                catch (const std::exception &)
                {
                    bad = true;
                    break;
                }
            }
            if (bad || cols.size() != 6)
                continue;

            ip_rotation_events_[static_cast<uint32_t>(cols[5])].insert(cols[0]);
            ++rows;
        }
    }
    if (rows)
        std::cout << "  ip rotations: " << rows << " across "
                  << ip_rotation_events_.size() << " nodes" << std::endl;
}

void DataProcessor::process_ip_changes()
{
    constexpr uint64_t DAY = 86400;
    struct IAcc
    {
        uint64_t changes = 0, cross_as = 0;
    };
    std::map<uint32_t, IAcc> days;

    for (const auto &data_path : data_paths_)
    {
        std::string path = "../data/" + data_path + "/ip_changes/ip_changes.csv";
        std::ifstream file(path);
        if (!file.is_open())
            continue;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty())
                continue;

            std::vector<uint64_t> cols;
            std::stringstream fs_(line);
            std::string field;
            bool bad = false;
            while (std::getline(fs_, field, ','))
            {
                try
                {
                    cols.push_back(std::stoull(field));
                }
                catch (const std::exception &)
                {
                    bad = true;
                    break;
                }
            }
            if (bad || cols.size() != 6)
                continue;
            auto &a = days[static_cast<uint32_t>(cols[0] / DAY)];
            a.changes++;
            if (cols[3] != cols[4])
                a.cross_as++;
        }
    }
    if (days.empty())
        return;

    std::ofstream out(output_folder + "ip_changes_daily.csv");
    out << "day_index,ip_changes,cross_as_changes\n";
    for (const auto &[day, a] : days)
        out << day << "," << a.changes << "," << a.cross_as << "\n";
    std::cout << "Saved ip_changes_daily.csv (" << days.size() << " days)" << std::endl;
}

void DataProcessor::write_daily_snapshot(uint32_t day, Graph &graph)
{
    std::vector<uint32_t> nodes = graph.get_all_nodes();
    std::vector<std::pair<uint32_t, uint32_t>> edges = graph.get_all_edges();
    std::string day_str = std::to_string(day);
    std::string snapshots_dir = output_folder + "snapshots/";

    std::ofstream nodes_out(snapshots_dir + day_str + "_nodes.csv");
    if (nodes_out.is_open())
    {
        nodes_out << "node_ip\n";
        for (uint32_t node : nodes)
            nodes_out << node << "\n";
    }

    std::ofstream edges_out(snapshots_dir + day_str + "_edges.csv");
    if (edges_out.is_open())
    {
        edges_out << "source,destination\n";
        for (const auto &[src, dst] : edges)
            edges_out << src << "," << dst << "\n";
    }

    if (degree_csv_.is_open())
    {
        std::unordered_map<uint32_t, uint32_t> degree;
        for (uint32_t n : nodes)
            degree[n] = 0;
        for (const auto &[src, dst] : edges)
        {
            degree[src]++;
            degree[dst]++;
        }
        std::map<uint32_t, std::pair<uint64_t, uint64_t>> hist;
        for (const auto &[ip, deg] : degree)
        {
            auto cls = ip_class_.find(ip);
            bool pub = cls != ip_class_.end() && cls->second;
            if (pub)
                hist[deg].first++;
            else
                hist[deg].second++;
        }
        for (const auto &[deg, counts] : hist)
            degree_csv_ << day << "," << deg << ","
                        << counts.first << "," << counts.second << "\n";
    }
    snapshot_days_written_++;
}

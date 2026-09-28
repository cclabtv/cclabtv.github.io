#include "parsing_utils.h"
#include <iostream>
std::vector<std::vector<uint64_t>> ParsingUtils::parse_delimited_data(const std::string &raw_data)
{
    std::vector<std::vector<uint64_t>> result;
    std::stringstream line_stream(raw_data);
    std::string line, field;

    while (std::getline(line_stream, line, '\n'))
    {
        if (line.empty())
            continue;

        std::vector<uint64_t> row;
        std::stringstream field_stream(line);

        bool bad = false;
        while (std::getline(field_stream, field, ','))
        {
            try
            {
                row.push_back(std::stoull(field));
            }
            catch (const std::exception &)
            {
                bad = true;
                break;
            }
        }
        if (!bad && !row.empty())
            result.push_back(row);
    }
    return result;
}

std::vector<Node> ParsingUtils::parse_nodes(const std::string &raw_data)
{
    std::vector<std::vector<uint64_t>> parsed_rows = parse_delimited_data(raw_data);
    std::vector<Node> nodes;
    nodes.reserve(parsed_rows.size());

    for (const auto &row : parsed_rows)
    {
        if (row.size() != 9)
            continue;

        Node node;
        node.unique_id = static_cast<uint32_t>(row[0]);
        node.ip = static_cast<uint32_t>(row[1]);
        node.reachable = static_cast<uint32_t>(row[2]);
        node.n_time = static_cast<uint64_t>(row[3]);
        node.l_time = static_cast<uint64_t>(row[4]);
        node.asmap = static_cast<uint32_t>(row[5]);
        node.map_info = static_cast<uint32_t>(row[6]);
        node.vvNew = static_cast<uint32_t>(row[7]);
        node.vvTried = static_cast<uint32_t>(row[8]);

        nodes.push_back(node);
    }

    return nodes;
}

std::vector<Edge> ParsingUtils::parse_edges(const uint32_t _source, const std::string &raw_data)
{
    std::vector<std::vector<uint64_t>> parsed_rows = parse_delimited_data(raw_data);
    std::vector<Edge> edges;
    edges.reserve(parsed_rows.size());

    for (const auto &row : parsed_rows)
    {
        if (row.size() != 3)
            continue;

        Edge edge;
        edge.round = static_cast<uint32_t>(row[0]);
        edge.source = _source;
        edge.destination = static_cast<uint32_t>(row[1]);
        edge.active = static_cast<bool>(row[2]);
        edges.push_back(edge);

    }

    return edges;
}

std::vector<CacheEntry> ParsingUtils::parse_cache(const std::string &raw_data)
{
    std::vector<std::vector<uint64_t>> parsed_rows = parse_delimited_data(raw_data);
    std::vector<CacheEntry> cache_entries;
    cache_entries.reserve(parsed_rows.size());

    for (const auto &row : parsed_rows)
    {
        if (row.size() != 3)
            continue;

        CacheEntry entry;
        entry.timestamp = static_cast<uint32_t>(row[0]);
        entry.source_id = static_cast<uint32_t>(row[1]);
        entry.destination_id = static_cast<uint32_t>(row[2]);

        cache_entries.push_back(entry);
    }

    return cache_entries;
}

Info ParsingUtils::parse_info(const std::string &raw_data, const uint32_t peer)
{
    std::vector<std::vector<uint64_t>> parsed_rows = parse_delimited_data(raw_data);
    Info info_entries;
    info_entries.peer = peer;
    info_entries.known_peers.reserve(parsed_rows.size());

    for (const auto &row : parsed_rows)
    {
        if (row.size() != 1)
            continue;
        info_entries.known_peers.push_back(static_cast<uint32_t>(row[0]));
    }
    return info_entries;
}

#ifndef STRUCTURES_H
#define STRUCTURES_H

#include <cstdint>
#include <vector>
#include <utility>

struct Node
{
    uint32_t unique_id;
    uint32_t ip;
    bool reachable;
    uint64_t n_time;
    uint64_t l_time;
    uint16_t asmap;
    uint64_t map_info;
    uint64_t vvNew;
    uint64_t vvTried;
    uint32_t source_index = 0;
};

struct Edge
{
    uint64_t round;
    uint32_t source;
    uint32_t destination;
    bool active;
};

struct CacheEntry
{
    uint32_t timestamp;
    uint32_t source_id;
    uint32_t destination_id;
};
struct Info
{
    uint32_t peer;
    std::vector<uint32_t> known_peers;
};

struct MapInfoEntry
{
    uint64_t first_seen = 0;
    uint32_t ip = 0;
    bool in_tried = false;
    uint16_t n_attempts = 0;
    uint64_t m_last_success = 0;
    uint32_t n_ref_count = 0;
};

struct TelemetryRow
{
    uint64_t round = 0;
    uint32_t ip = 0;
    uint32_t feeler_success = 0;
    uint32_t feeler_fail = 0;
    uint32_t getaddr_served = 0;
    uint32_t addr_dropped = 0;
};
struct TimelineEvent
{
    std::vector<uint32_t> joining_nodes;
    std::vector<Node> leaving_nodes;
    std::vector<std::pair<uint32_t, uint32_t>> new_edges;
    std::vector<std::pair<uint32_t, uint32_t>> removed_edges;
};

struct GraphSnapshot
{
    std::vector<uint32_t> nodes;
    std::vector<std::pair<uint32_t, uint32_t>> edges;
};

#endif

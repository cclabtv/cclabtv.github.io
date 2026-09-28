#ifndef PARSING_UTILS_H
#define PARSING_UTILS_H

#include <vector>
#include <cstdint>
#include <sstream>
#include <string>
#include "structures.h"

class ParsingUtils
{
private:
    static std::vector<std::vector<uint64_t>> parse_delimited_data(const std::string &raw_data);

public:
    static std::vector<Node> parse_nodes(const std::string &raw_data);
    static std::vector<Edge> parse_edges(const uint32_t source, const std::string &raw_data);
    static std::vector<CacheEntry> parse_cache(const std::string &raw_data);
    static Info parse_info(const std::string &raw_data, const uint32_t peer);
};

#endif

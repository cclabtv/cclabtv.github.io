#ifndef GRAPH_H
#define GRAPH_H

#include <iostream>
#include <unordered_map>
#include <vector>
#include <queue>
#include <algorithm>
#include <set>
#include <random>
typedef std::pair<uint32_t, uint32_t> pii;
class Graph
{
private:
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, bool>> adj_list;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, int>> lap_matrix;
    std::unordered_map<uint32_t, uint32_t> nodes_degree;
    std::vector<uint32_t> nodes;
    uint32_t number_nodes{0};
    uint32_t number_edges{0};

public:
    Graph();
    bool add_edge(uint32_t &u, uint32_t &v);
    bool remove_edge(uint32_t &u, uint32_t &v);
    uint64_t get_edge_count();

    void add_node(uint32_t &node);
    int64_t remove_node(uint32_t &node);
    uint64_t get_node_count();

    std::vector<uint32_t> get_all_nodes();
    std::vector<std::pair<uint32_t, uint32_t>> get_all_edges();

    void increase_node_degree(uint32_t &node);
    bool decrease_node_degree(uint32_t &node);
    uint32_t get_node_degree(uint32_t &node);

    std::vector<uint32_t> get_neighbors(uint32_t &node);

    std::unordered_map<uint32_t, std::unordered_map<uint32_t, bool>> get_giant_component();
    std::vector<std::vector<uint32_t>> get_all_components();
    uint32_t max_distance_from(uint32_t &source);

    std::vector<uint32_t> random_subset(uint32_t _size, uint64_t _random_seed);

    double get_assortativity();
};
#endif

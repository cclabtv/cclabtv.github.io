#include <graph.h>

Graph::Graph() {}

bool Graph::add_edge(uint32_t &u, uint32_t &v)
{

    if (adj_list.find(u) == adj_list.end() || adj_list.find(v) == adj_list.end())
        return false;

    if (adj_list[u].find(v) != adj_list[u].end())
        return false;

    adj_list[u][v] = true;
    adj_list[v][u] = true;
    increase_node_degree(u);
    increase_node_degree(v);
    number_edges++;
    return true;
}

bool Graph::remove_edge(uint32_t &u, uint32_t &v)
{
    if (adj_list.find(u) == adj_list.end() || adj_list.find(v) == adj_list.end())
        return false;

    if (adj_list[u].find(v) == adj_list[u].end() ||
        adj_list[v].find(u) == adj_list[v].end())
        return false;

    adj_list[u].erase(v);
    adj_list[v].erase(u);

    bool u_decreased = decrease_node_degree(u);
    bool v_decreased = decrease_node_degree(v);

    if (!u_decreased || !v_decreased)
    {
        adj_list[u][v] = true;
        adj_list[v][u] = true;

        if (u_decreased)
            nodes_degree[u]++;
        if (v_decreased)
            nodes_degree[v]++;

        return false;
    }

    number_edges--;
    return true;
}
uint64_t Graph::get_edge_count()
{
    return number_edges;
}
void Graph::add_node(uint32_t &node)
{
    if (adj_list.find(node) == adj_list.end())
    {
        adj_list[node] = std::unordered_map<uint32_t, bool>();
        number_nodes++;
        nodes.push_back(node);
    }
}
int64_t Graph::remove_node(uint32_t &node)
{
    if (adj_list.find(node) == adj_list.end())
        return -1;

    uint32_t node_deg = get_node_degree(node);

    std::vector<uint32_t> neighbors = get_neighbors(node);

    for (auto &neighbor : neighbors)
    {
        if (!remove_edge(node, neighbor))
        {
            std::cerr << "Error: Failed to remove edge between "
                      << node << " and " << neighbor << std::endl;
            return -1;
        }
    }

    adj_list.erase(node);

    auto nodes_it = std::find(nodes.begin(), nodes.end(), node);
    if (nodes_it != nodes.end())
        nodes.erase(nodes_it);

    nodes_degree.erase(node);

    number_nodes--;

    return node_deg;
}
uint64_t Graph::get_node_count()
{
    return number_nodes;
}
std::vector<uint32_t> Graph::get_all_nodes()
{
    return nodes;
}
std::vector<std::pair<uint32_t, uint32_t>> Graph::get_all_edges()
{
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    edges.reserve(number_edges);
    for (const auto &[u, neighbors] : adj_list)
    {
        for (const auto &[v, exists] : neighbors)
        {
            if (exists && u < v)
            {
                edges.push_back({u, v});
            }
        }
    }
    return edges;
}
void Graph::increase_node_degree(uint32_t &node)
{
    if (nodes_degree.find(node) == nodes_degree.end())
        nodes_degree[node] = 0;
    nodes_degree[node]++;
}
bool Graph::decrease_node_degree(uint32_t &node)
{
    if (nodes_degree.find(node) != nodes_degree.end())
        if (nodes_degree[node] > 0)
        {
            nodes_degree[node]--;
            return true;
        }
    return false;
}
uint32_t Graph::get_node_degree(uint32_t &node)
{
    if (nodes_degree.find(node) != nodes_degree.end())
        return nodes_degree[node];
    return 0;
}

std::vector<uint32_t> Graph::get_neighbors(uint32_t &node)
{
    std::vector<uint32_t> neighbors;
    if (adj_list.find(node) == adj_list.end())
        return neighbors;

    for (const auto &neighbor : adj_list[node])
        if (neighbor.second)
            neighbors.push_back(neighbor.first);
    return neighbors;
}

std::unordered_map<uint32_t, std::unordered_map<uint32_t, bool>> Graph::get_giant_component()
{
    std::unordered_map<uint32_t, bool> visited;
    for (const auto &pair : adj_list)
        visited[pair.first] = false;
    std::vector<uint32_t> max_component;
    for (const auto &pair : adj_list)
    {
        uint32_t node = pair.first;
        if (!visited[node])
        {
            std::queue<uint32_t> q;
            q.push(node);
            visited[node] = true;
            std::vector<uint32_t> current_component;
            current_component.push_back(node);
            while (!q.empty())
            {
                uint32_t current_node = q.front();
                q.pop();
                for (const auto &neighbor_pair : adj_list[current_node])
                {
                    uint32_t neighbor = neighbor_pair.first;
                    bool edge_exists = neighbor_pair.second;
                    if (edge_exists && !visited[neighbor])
                    {
                        visited[neighbor] = true;
                        q.push(neighbor);
                        current_component.push_back(neighbor);
                    }
                }
            }
            if (current_component.size() > max_component.size())
                max_component = move(current_component);
        }
    }
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, bool>> giant;
    giant.reserve(max_component.size());
    for (const auto &node : max_component)
        for (const auto &neighbor : adj_list[node])
            if (neighbor.second)
                giant[node][neighbor.first] = true;
    return giant;
}

std::vector<std::vector<uint32_t>> Graph::get_all_components()
{
    std::unordered_map<uint32_t, bool> visited;
    for (const auto &pair : adj_list)
        visited[pair.first] = false;

    std::vector<std::vector<uint32_t>> components;
    for (const auto &pair : adj_list)
    {
        uint32_t node = pair.first;
        if (!visited[node])
        {
            std::queue<uint32_t> q;
            q.push(node);
            visited[node] = true;
            std::vector<uint32_t> component;
            component.push_back(node);
            while (!q.empty())
            {
                uint32_t current_node = q.front();
                q.pop();
                for (const auto &neighbor_pair : adj_list[current_node])
                {
                    uint32_t neighbor = neighbor_pair.first;
                    bool edge_exists = neighbor_pair.second;
                    if (edge_exists && !visited[neighbor])
                    {
                        visited[neighbor] = true;
                        q.push(neighbor);
                        component.push_back(neighbor);
                    }
                }
            }
            components.push_back(std::move(component));
        }
    }

    std::sort(components.begin(), components.end(),
              [](const std::vector<uint32_t> &a, const std::vector<uint32_t> &b)
              {
                  return a.size() > b.size();
              });

    return components;
}

uint32_t Graph::max_distance_from(uint32_t &source)
{
    std::unordered_map<uint32_t, uint32_t> distance;
    std::queue<uint32_t> q;

    for (const auto &node : adj_list)
        distance[node.first] = UINT32_MAX;

    distance[source] = 0;
    q.push(source);
    uint32_t max_distance = 0;

    while (!q.empty())
    {
        uint32_t current = q.front();
        q.pop();

        for (const auto &neighbor_pair : adj_list[current])
        {
            uint32_t neighbor = neighbor_pair.first;
            if (neighbor_pair.second && distance[neighbor] == UINT32_MAX)
            {
                distance[neighbor] = distance[current] + 1;
                max_distance = std::max(max_distance, distance[neighbor]);
                q.push(neighbor);
            }
        }
    }

    return max_distance;
}
std::vector<uint32_t> Graph::random_subset(uint32_t _size, uint64_t _random_seed)
{
    if (nodes.size() <= _size)
        return nodes;

    std::vector<uint32_t> subset;
    subset.reserve(_size);

    std::mt19937 rng(_random_seed);
    std::sample(nodes.begin(), nodes.end(), std::back_inserter(subset), _size, rng);

    return subset;
}

double Graph::get_assortativity()
{
    if (number_edges == 0)
        return 0.0;

    double M = static_cast<double>(number_edges);
    double sum_jk = 0.0;
    double sum_jk_plus = 0.0;
    double sum_jk_sq = 0.0;

    for (const auto &[u, neighbors] : adj_list)
    {
        double deg_u = nodes_degree.count(u) ? static_cast<double>(nodes_degree[u]) : 0.0;
        for (const auto &[v, exists] : neighbors)
        {
            if (exists && u < v)
            {
                double deg_v = nodes_degree.count(v) ? static_cast<double>(nodes_degree[v]) : 0.0;
                sum_jk += deg_u * deg_v;
                sum_jk_plus += 0.5 * (deg_u + deg_v);
                sum_jk_sq += 0.5 * (deg_u * deg_u + deg_v * deg_v);
            }
        }
    }

    double mean_half = sum_jk_plus / M;
    double numerator = (sum_jk / M) - mean_half * mean_half;
    double denominator = (sum_jk_sq / M) - mean_half * mean_half;

    if (denominator == 0.0)
        return 0.0;
    return numerator / denominator;
}

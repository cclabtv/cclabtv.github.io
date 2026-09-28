#ifndef EIGENVALUE_CALCULATOR_H
#define EIGENVALUE_CALCULATOR_H

#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <Eigen/Dense>
#include <Eigen/Eigenvalues>

struct EigenvalueResult
{
    double lambda1;
    double lambda2;
    double spectral_gap;
    double eigenvalue_ratio;
    size_t node_count;
    size_t edge_count;
};

class EigenvalueCalculator
{
public:
    static EigenvalueResult compute(
        const std::vector<uint32_t> &nodes,
        const std::vector<std::pair<uint32_t, uint32_t>> &edges)
    {
        EigenvalueResult result{0.0, 0.0, 0.0, 0.0, nodes.size(), edges.size()};

        if (nodes.size() < 2)
        {
            return result;
        }

        std::unordered_map<uint32_t, size_t> node_to_index;
        node_to_index.reserve(nodes.size());
        std::vector<uint32_t> sorted_nodes(nodes);
        std::sort(sorted_nodes.begin(), sorted_nodes.end());

        for (size_t i = 0; i < sorted_nodes.size(); ++i)
        {
            node_to_index[sorted_nodes[i]] = i;
        }

        size_t n = sorted_nodes.size();

        Eigen::MatrixXd adj_matrix = Eigen::MatrixXd::Zero(n, n);

        for (const auto &edge : edges)
        {
            auto it_u = node_to_index.find(edge.first);
            auto it_v = node_to_index.find(edge.second);
            if (it_u != node_to_index.end() && it_v != node_to_index.end())
            {
                adj_matrix(it_u->second, it_v->second) = 1.0;
                adj_matrix(it_v->second, it_u->second) = 1.0;
            }
        }

        Eigen::VectorXd degrees = adj_matrix.rowwise().sum();

        Eigen::VectorXd d_inv_sqrt = Eigen::VectorXd::Zero(n);
        for (size_t i = 0; i < n; ++i)
        {
            if (degrees(i) > 0.0)
                d_inv_sqrt(i) = 1.0 / std::sqrt(degrees(i));
        }

        Eigen::MatrixXd laplacian = Eigen::MatrixXd::Identity(n, n)
            - d_inv_sqrt.asDiagonal() * adj_matrix * d_inv_sqrt.asDiagonal();

        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(laplacian);

        if (solver.info() != Eigen::Success)
        {
            std::cerr << "Warning: Eigenvalue computation failed for graph with "
                      << n << " nodes" << std::endl;
            return result;
        }

        Eigen::VectorXd eigenvalues = solver.eigenvalues();

        result.lambda1 = eigenvalues(0);
        result.lambda2 = eigenvalues(1);
        result.spectral_gap = result.lambda2 - result.lambda1;
        result.eigenvalue_ratio = (eigenvalues(n - 1) > 1e-10)
                                      ? result.lambda2 / eigenvalues(n - 1)
                                      : 0.0;

        return result;
    }
};

#endif

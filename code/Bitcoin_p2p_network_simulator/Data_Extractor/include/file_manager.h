#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include <iostream>
#include <string>
#include <vector>
#include <zlib.h>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cmath>

std::string load_data(const std::string &filepath);

std::vector<std::string> load_files_in_path(const std::string &files_path);

bool create_directory(const std::string &path);

template <typename T>
bool write_csv(const std::string &filepath,
               const std::vector<std::vector<T>> &data,
               const std::vector<std::string> &header)
{
    std::ofstream file(filepath, std::ios::out | std::ios::app);

    if (!file.is_open())
    {
        std::cerr << "Error: Could not open file for writing: " << filepath << std::endl;
        return false;
    }

    file.seekp(0, std::ios::end);
    bool is_empty = (file.tellp() == 0);

    if (is_empty && !header.empty())
    {
        for (size_t i = 0; i < header.size(); ++i)
        {
            file << header[i];
            if (i < header.size() - 1)
                file << ",";
        }
        file << "\n";
    }

    for (const auto &row : data)
    {
        for (size_t i = 0; i < row.size(); ++i)
        {
            double val = static_cast<double>(row[i]);
            if (val == std::floor(val) && std::isfinite(val))
                file << std::fixed << std::setprecision(0) << val;
            else
                file << std::defaultfloat << std::setprecision(6) << val;
            if (i < row.size() - 1)
                file << ",";
        }
        file << "\n";
    }

    file.close();
    return true;
}

#endif

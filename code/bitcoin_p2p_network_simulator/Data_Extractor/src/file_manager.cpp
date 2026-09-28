#include "file_manager.h"

std::string load_data(const std::string &filepath)
{

    std::ifstream file(filepath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Could not open file for reading: " << filepath << "\n";
        return "";
    }

    file.seekg(0, std::ios::end);
    std::streampos size = file.tellg();
    if (size <= 0)
        return "";
    std::string file_content(static_cast<size_t>(size), '\0');
    file.seekg(0, std::ios::beg);
    file.read(file_content.data(), size);
    if (file.gcount() != size)
        file_content.resize(static_cast<size_t>(file.gcount()));
    return file_content;
}

std::vector<std::string> load_files_in_path(const std::string &files_path)
{
    std::vector<std::string> files;
    if (!std::filesystem::exists(files_path))
    {
        std::cerr << "Warning: directory missing, skipped: " << files_path << "\n";
        return files;
    }
    try
    {
        for (const auto &entry : std::filesystem::directory_iterator(files_path))
        {
            if (!entry.is_regular_file())
                continue;
            std::string file_name = entry.path().filename().string();
            if (file_name == "header")
                continue;
            files.push_back(file_name);
        }
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        std::cerr << "Warning: cannot list " << files_path << ": " << e.what() << "\n";
    }
    std::sort(files.begin(), files.end());
    return files;
}
bool create_directory(const std::string &path)
{
    try
    {
        if (std::filesystem::exists(path))
        {
            return true;
        }

        if (std::filesystem::create_directories(path))
        {
            std::cout << "Created directory: " << path << std::endl;
            return true;
        }
        else
        {
            std::cerr << "Failed to create directory: " << path << std::endl;
            return false;
        }
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        std::cerr << "Error creating directory " << path << ": " << e.what() << std::endl;
        return false;
    }
}

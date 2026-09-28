#ifndef VARINT_HPP
#define VARINT_HPP

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace serialization {

// Encode any integer type to compact variable-length format
template <typename T> std::vector<uint8_t> make_compact(T value) {
  static_assert(std::is_integral<T>::value, "Type must be integral");

  uint64_t v = static_cast<uint64_t>(value);
  std::vector<uint8_t> result;

  while (v >= 0x80) {
    result.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  result.push_back(static_cast<uint8_t>(v & 0x7F));

  return result;
}

// Read compact variable-length integer from file
template <typename T> T read_compact(std::ifstream &file) {
  static_assert(std::is_integral<T>::value, "Type must be integral");

  uint64_t result = 0;
  int shift = 0;
  uint8_t byte;

  while (file.read(reinterpret_cast<char *>(&byte), 1)) {
    result |= static_cast<uint64_t>(byte & 0x7F) << shift;

    if ((byte & 0x80) == 0) {
      break;
    }

    shift += 7;
    if (shift >= 64) {
      throw std::overflow_error("VarInt overflow during read");
    }
  }

  if (!file && !file.eof()) {
    throw std::runtime_error("Failed to read from file");
  }

  return static_cast<T>(result);
}

// Helper: Write compact to file directly
template <typename T> void write_compact(std::ofstream &file, T value) {
  auto encoded = make_compact(value);
  file.write(reinterpret_cast<const char *>(encoded.data()), encoded.size());
}

// Helper: Write raw bytes
inline void write_bytes(std::ofstream &file, const void *data, size_t size) {
  file.write(reinterpret_cast<const char *>(data), size);
}

// Helper: Read raw bytes
inline void read_bytes(std::ifstream &file, void *data, size_t size) {
  file.read(reinterpret_cast<char *>(data), size);
  if (!file) {
    throw std::runtime_error("Failed to read bytes from file");
  }
}

// Helper: Write string with length prefix
inline void write_string(std::ofstream &file, const std::string &str) {
  write_compact(file, str.size());
  file.write(str.data(), str.size());
}

// Helper: Read string with length prefix
inline std::string read_string(std::ifstream &file) {
  size_t length = read_compact<size_t>(file);
  std::string result(length, '\0');
  file.read(&result[0], length);
  if (!file) {
    throw std::runtime_error("Failed to read string from file");
  }
  return result;
}

// Helper: Write vector with length prefix
template <typename T>
void write_vector(std::ofstream &file, const std::vector<T> &vec) {
  write_compact(file, vec.size());
  for (const auto &item : vec) {
    if constexpr (std::is_integral<T>::value) {
      write_compact(file, item);
    } else {
      write_bytes(file, &item, sizeof(T));
    }
  }
}

// Helper: Read vector with length prefix
template <typename T> std::vector<T> read_vector(std::ifstream &file) {
  size_t size = read_compact<size_t>(file);
  std::vector<T> result;
  result.reserve(size);

  for (size_t i = 0; i < size; ++i) {
    if constexpr (std::is_integral<T>::value) {
      result.push_back(read_compact<T>(file));
    } else {
      T item;
      read_bytes(file, &item, sizeof(T));
      result.push_back(item);
    }
  }

  return result;
}

} // namespace serialization

#endif
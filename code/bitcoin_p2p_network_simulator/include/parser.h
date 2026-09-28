/**
 * @file parser.h
 * @brief Configuration loading and the on-disk output format.
 *
 * Every record type written to disk is declared here, and the writer for each
 * is a method of Parser. All output lands under one directory chosen once per
 * process:
 *
 *   <dir>/nodes                  one line per node that has left
 *   <dir>/edges/<address>        connections opened and closed, per source
 *   <dir>/caches/<address>       cache contents over time, per source
 *   <dir>/map_info/<address>     known-address dumps, per source
 *   <dir>/ip_changes/ip_changes.csv
 *   <dir>/telemetry.csv
 *   <dir>/<sub>/<name>.bin       binary state
 *
 * @warning The column order of each record below is the file format. Reordering
 *          a field, or swapping two of the same type, changes the meaning of
 *          every file already written without any error being raised. Separate
 *          tools read these files and must be changed in step.
 */

#ifndef PARSER
#define PARSER
#include <boost/program_options.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <zlib.h>

#include "structures.h"

namespace fs = std::filesystem;
namespace po = boost::program_options;

enum Data_type
{
  d_node,
  d_edge,
  d_cache,
  d_info
};

/**
 * @brief One connection opening or closing.
 *
 * Written to edges/<source>, one line per record, as
 * `round,destination,exist`. The source is the file name and is not repeated
 * on each line.
 */
struct edge
{
  uint64_t round;       //!< Time of the change, in seconds.
  uint32_t source;      //!< Address the connection was opened from.
  uint32_t destination; //!< Address it was opened to.
  bool exist;           //!< True when opened, false when closed.
  edge(uint64_t _round, uint32_t _source, uint32_t _destination, bool _exist)
      : round(_round), source(_source), destination(_destination),
        exist(_exist) {}
};

struct AddInfoFull
{
  uint32_t ip;
  uint16_t asmap;
  uint32_t source;
  uint64_t first_seen;
  uint64_t n_time;
  uint64_t m_last_success;
  uint64_t m_last_try;
  uint16_t n_attempts;
  uint8_t n_ref_count;
  bool in_tried;
  uint64_t m_last_count_attempt;
  uint64_t n_new;
  uint64_t n_tried;
};
// enum Network
// {
//   NET_UNROUTABLE = 0,
//   NET_IPV4,
//   NET_IPV6,
//   NET_ONION,
//   NET_I2P,
//   NET_CJDNS,
//   NET_INTERNAL,
//   NET_MAX,
// };

/**
 * @brief One node, recorded when it leaves.
 *
 * Written to the `nodes` file, one line per record, in the field order below.
 *
 * @warning vvNew precedes vvTried. The two are the same type and adjacent, so
 *          supplying them the other way round produces a file that parses
 *          cleanly with the two table sizes exchanged.
 */
struct node
{
  uint32_t unique_id; //!< Identifier, stable across address changes.
  uint32_t ip;        //!< Address held at the time of the record.
  bool is_reachable;  //!< Whether it accepted inbound connections.
  uint64_t n_time;    //!< Time it joined, in seconds.
  uint64_t l_time;    //!< Time it left, in seconds.
  uint16_t asmap;     //!< Group of its address.
  uint64_t map_info;  //!< Addresses known in total.
  uint64_t vvNew;     //!< Of those, how many sit in the new table.
  uint64_t vvTried;   //!< Of those, how many sit in the tried table.
  node(uint32_t _unique_id, uint32_t _ip, bool _is_reachable, uint64_t _n_time,
       uint64_t _l_time, uint16_t _asmap, uint64_t _map_info, uint64_t _vvNew,
       uint64_t _vvTried)
      : unique_id(_unique_id), ip(_ip), is_reachable(_is_reachable), n_time(_n_time),
        l_time(_l_time), asmap(_asmap), map_info(_map_info), vvNew(_vvNew),
        vvTried(_vvTried) {}
  /** @return One comma-separated line, newline included. */
  std::string to_compact_string() const
  {
    return std::to_string(unique_id) + "," + std::to_string(ip) + "," +
           std::to_string(is_reachable) + "," + std::to_string(n_time) + "," +
           std::to_string(l_time) + "," + std::to_string(asmap) + "," +
           std::to_string(map_info) + "," + std::to_string(vvNew) + "," +
           std::to_string(vvTried) + "\n";
  }
};

/**
 * @brief The addresses one node was offering at one moment.
 *
 * Written to caches/<source> as a two-line-kind format: a header line
 * `round,addrman_size`, then one line per address prefixed with a comma,
 * `,address,timestamp`. The leading comma is what distinguishes an address
 * line from the next header.
 */
struct cache
{
  uint64_t round;                                       //!< Time the snapshot was taken, in seconds.
  uint32_t source;                                      //!< Node the snapshot belongs to. Becomes the file name.
  uint32_t addrman_size;                                //!< Addresses that node knew in total at the time.
  std::vector<std::pair<uint32_t, uint64_t>> addresses; // {ip, n_time}
  cache() {}
  cache(uint64_t _round, uint32_t _source, uint32_t _addrman_size,
        std::vector<std::pair<uint32_t, uint64_t>> _addresses)
      : round(_round), source(_source), addrman_size(_addrman_size),
        addresses(std::move(_addresses)) {}
};

/** @brief One known address, as recorded in a dump. */
struct info_entry
{
  uint64_t first_seen;     //!< Time it was first learned, in seconds.
  uint32_t ip;             //!< The address.
  bool in_tried;           //!< Whether it had been promoted to the tried table.
  uint16_t n_attempts;     //!< Failed attempts standing against it.
  uint64_t m_last_success; //!< Time of the last success, 0 if never.
  uint8_t n_ref_count;     //!< New-table slots holding it.
};

/**
 * @brief Every address one node knew, at one moment.
 *
 * Written to map_info/<source>, one line per entry. The owner is the file name
 * and is not repeated on each line.
 */
struct info
{
  uint32_t source;               //!< Node the dump belongs to. Becomes the file name.
  std::vector<info_entry> peers; //!< One entry per known address.
  /** @return One comma-separated line per entry, newlines included. */
  std::string to_compact_string() const
  {
    std::string result = "";
    for (const auto &p : peers)
      result += std::to_string(p.first_seen) + "," + std::to_string(p.ip) + "," +
                std::to_string(p.in_tried ? 1 : 0) + "," +
                std::to_string(p.n_attempts) + "," +
                std::to_string(p.m_last_success) + "," +
                std::to_string(static_cast<uint32_t>(p.n_ref_count)) + "\n";
    return result;
  }
};

/**
 * @brief Counters for one node over one period.
 *
 * Written to telemetry.csv, which is the only output file carrying a header
 * row. Counters are reset each time they are recorded.
 */
struct telemetry
{
  uint64_t round; //!< Time the counters were read, in seconds.
  uint32_t ip;    //!< Node they belong to.
  uint32_t feeler_success, feeler_fail, getaddr_served, addr_dropped;
};

/**
 * @brief One node changing address.
 *
 * Written to ip_changes/ip_changes.csv in the field order below. Unlike
 * telemetry.csv this file carries no header row.
 */
struct ip_change_record
{
  uint64_t round;     //!< Time of the change, in seconds.
  uint32_t old_ip;    //!< Address given up.
  uint32_t new_ip;    //!< Address taken.
  uint16_t old_as;    //!< Group of the old address.
  uint16_t new_as;    //!< Group of the new address.
  uint32_t unique_id; //!< Identifier, unchanged by the move, linking the two.
};

/**
 * @brief Loads configuration and writes every output file.
 *
 * @note Every writer below appends, so calling one repeatedly extends the
 *       existing files rather than replacing them, and each returns without
 *       creating anything when handed an empty vector.
 */
class Parser
{
private:
public:
  /**
   * Directory all output is written under.
   *
   * Chosen once per process from the wall-clock time, so two instances share
   * one directory rather than splitting the output across two.
   */
  std::string direction;

  /** @brief Choose the output directory if not already chosen, and create it. */
  Parser();

  bool file_exists(const std::string &path); //!< @note Unused. @return Whether @p path exists.

  void save_infos(std::vector<info> &data);                  //!< @brief Append address dumps, one file per owner.
  void save_cache(std::vector<cache> &data);                 //!< @brief Append cache snapshots, one file per owner.
  void save_edges(std::vector<edge> &data);                  //!< @brief Append connection changes, one file per source.
  void save_nodes(std::vector<node> &data);                  //!< @brief Append node records to a single file.
  void save_ip_changes(std::vector<ip_change_record> &data); //!< @brief Append address changes to a single file.

  /**
   * @brief Write @p data to @p file_path.
   * @note Despite the name, writes plain text. No compression is applied.
   * @note Reachable only through save_peer_data(), which nothing calls.
   */
  void save_compressed(std::string &file_path, std::string &data);

  std::string load_data(const std::string &filepath); //!< @note Unused. @return File contents, empty on failure.

  /**
   * @brief Split comma-separated text into rows of integers.
   * @note Unused. Values are parsed as 64-bit and stored as 32-bit, so
   *       anything larger is truncated, and malformed input throws.
   */
  std::vector<std::vector<uint32_t>>
  parse_delimited_data(const std::string &raw_data);

  /**
   * @brief Read the configuration.
   *
   * Command-line arguments take precedence over the file, and the file over
   * the built-in defaults. When @p conf_path is missing: the default path is
   * created with the default values, any other path is an error and the
   * process exits, since a named file that cannot be found would otherwise be
   * silently replaced by defaults.
   *
   * Unrecognised keys in the file are ignored rather than rejected, so a file
   * naming a setting that no longer exists still loads. A misspelled key is
   * therefore also ignored.
   *
   * @return The configuration, with the per-network profile synchronised to
   *         the reachability value.
   */
  Network_Config load_config(int argc, char *argv[], const std::string &conf_path = "simulator.conf");

  /** @brief Write the configuration in use into the output directory. */
  void save_runtime_config(const Network_Config &config);

  /**
   * @brief Write one node's three tables as text under @p base_path.
   * @note Unused.
   */
  void save_peer_data(uint32_t ip, const std::string &map_info_str,
                      const std::string &vvNew_str,
                      const std::string &vvTried_str,
                      const std::string &base_path);

  /** @brief Append counters, creating the file with a header row if absent. */
  void save_telemetry(std::vector<telemetry> &data);

  /**
   * @brief Read a binary file.
   * @param file_name Name without the .bin suffix.
   * @param base_path Directory holding it, used as given.
   * @return Its contents, empty if it could not be opened or read.
   * @warning @p base_path is used as given here but is taken relative to the
   *          output directory by save_binary(). A path that works for one will
   *          not work for the other.
   */
  std::vector<uint8_t> load_binary(const std::string &file_name,
                                   const std::string &base_path);

  /**
   * @brief Write a binary file, creating directories as needed.
   * @param file_name Name without the .bin suffix.
   * @param base_path Directory, relative to the output directory.
   * @warning See load_binary() for the difference in how the two treat
   *          @p base_path.
   */
  void save_binary(const std::string file_name,
                   const std::vector<uint8_t> &data,
                   const std::string &base_path);
};

#endif // PARSER

/**
 * @file net.h
 * @brief Partition of the address space into groups.
 *
 * The address space is divided once, at construction, into contiguous ranges
 * of power-law-distributed size. Every address belongs to exactly one range,
 * and the range index is the group used wherever addresses must be spread
 * across independent operators.
 */

#ifndef NET_H
#define NET_H

#include <iostream>
#include <random>
#include <vector>
#include "events.h"

/** @brief One contiguous range of addresses and the group it belongs to. */
struct bucket
{
  uint32_t min;   //!< First address offset in the range, inclusive.
  uint32_t max;   //!< Last address offset in the range, inclusive.
  uint32_t as_id; //!< Group index. Equal to the range's position in the partition.
};

/**
 * @brief The address-space partition, built once and then read-only.
 *
 * Ranges are stored in ascending order of @ref bucket::min and never change
 * after construction, so lookups are a binary search.
 */
class Net
{
private:
  std::vector<bucket> mapping; //!< The ranges, ascending and non-overlapping.

  /**
   * @brief Build the partition.
   *
   * Draws one weight per range from a power law with exponent @p _alpha, then
   * scales the weights to integer lengths summing to @p _total_span. Any
   * rounding shortfall is added to the last range, so the ranges together
   * cover offsets 0 to @p _total_span - 1 with no gaps.
   *
   * @param _n_as       Number of ranges to create.
   * @param _alpha      Power-law exponent. Lower values make sizes more unequal.
   * @param _total_span Number of offsets to cover.
   * @param _seed       Seed for the size draw.
   *
   * @note A weight small enough to scale to length zero would produce a range
   *       with max one below min, which matches no address and is unsafe to
   *       draw from. This does not arise at the sizes in use, where the
   *       smallest range still spans several hundred thousand offsets.
   */
  void generate_powerlaw_ranges(size_t _n_as, double _alpha, size_t _total_span,
                                int _seed);

public:
  /** @brief Build the partition. See generate_powerlaw_ranges() for the parameters. */
  Net(size_t _n_as, double _alpha, size_t _total_span, int _seed);
  ~Net();

  /**
   * @brief Group of an address, as used to keep connections spread out.
   *
   * Addresses on a network that has no internal structure to exploit collapse
   * to a single group each, well above every range index. Everything else is
   * grouped by its offset, so the same offset on two such networks yields the
   * same group.
   *
   * @param addr Address, network bits included.
   * @return The group index.
   * @warning An offset outside every range makes get_as_map() return -1, which
   *          this casts to 65535. That value is a group like any other to the
   *          callers, so an ungrouped address silently shares a group with
   *          every other ungrouped address. Only the single highest offset is
   *          affected, since the ranges stop one short of the full span.
   */
  uint16_t get_group(uint32_t addr);

  /**
   * @brief Index of the range containing an offset.
   * @param ip Address offset, without network bits.
   * @return The range index, or -1 if no range contains @p ip or the partition
   *         is empty.
   */
  int get_as_map(uint32_t ip);

  /**
   * @brief The range at a given index.
   * @param id Index into the partition.
   * @return A copy of that range.
   * @warning Not bounds-checked. Values get_group() returns for networks that
   *          collapse to one group, and its 65535 failure value, are all
   *          outside the partition and must not be passed here.
   */
  bucket get_bucket_by_id(int id);

  /**
   * @brief The whole partition as text.
   * @return One line per range, "index,min,max".
   */
  std::string get_mapping_string();
};

#endif // NET_H

/**
 * @file rrandom.h
 * @brief A single seeded random source, wrapping one generator.
 *
 * Every draw goes through one instance, so a given seed reproduces a given
 * sequence exactly. Adding, removing or reordering draws changes every value
 * that follows, even where the distributions are unchanged.
 */

#ifndef RRANDOM_H
#define RRANDOM_H
#include <algorithm>
#include <math.h>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <utility>
#include <vector>
/** @note Unused :| , and defined for every file that includes this one. */
#define PI std::acos(-1.0)

/** @brief Seeded source of random values. */
class RRandom
{
private:
  std::mt19937_64 engine_;

public:
  RRandom();               //!< @brief Seed with 0.
  RRandom(int _seed);      //!< @brief Seed with @p _seed.
  bool flip();             //!< @return True or false with equal probability.
  double uniform_double(); //!< @return A value in [0, 1).

  /**
   * @brief A random selection from a vector.
   *
   * @param v      Values to choose among. Not modified.
   * @param _count How many to return.
   * @return @p _count values in random order, or all of them shuffled when
   *         @p v holds fewer. The result is never longer than @p v, so callers
   *         needing an exact size must check.
   *
   * @note Copies @p v and shuffles the copy in full, so the cost is the size
   *       of the input rather than the size of the result.
   */
  template <typename T>
  std::vector<T> random_subset(const std::vector<T> &v, size_t _count)
  {
    std::vector<T> result = v;
    if (result.size() >= _count)
    {
      std::shuffle(result.begin(), result.end(), engine_);
      result.resize(_count);
    }
    else
    {
      std::shuffle(result.begin(), result.end(), engine_);
    }
    return result;
  }

  /**
   * @brief A value from an inclusive range, each equally likely.
   *
   * @param min One end of the range, inclusive.
   * @param max The other end, inclusive.
   * @return A value between the two, whichever order they arrive in.
   */
  template <typename U>
  U uniform_int(U min, U max)
  {
    if (max < min)
      std::swap(min, max);

    std::uniform_int_distribution<U> dist(min, max);
    return dist(engine_);
  }

  /**
   * @brief A value from an exponential distribution.
   *
   * @param mean The average of the values produced. The rate handed to the
   *             distribution is its reciprocal, so 120 yields values
   *             averaging 120.
   * @return The value, rounded to the nearest integer.
   *
   * @note Rounding makes 0 a possible result, more often as @p mean falls:
   *       about 39 percent of draws at a mean of 1. Callers needing a
   *       positive value must impose that themselves.
   */
  int exponential_distribution(int mean);

  /**
   * @brief A count from a Poisson distribution.
   * @param lambda The mean count. Fractional values are accepted.
   */
  int poisson_distribution(double lambda);

  /**
   * @brief @p k independent offsets spread uniformly over a window.
   * @param k How many offsets to produce.
   * @param t Window width. Offsets fall in [0, t - 1].
   * @return The offsets, in the order drawn and not sorted. Values may repeat.
   */
  std::vector<uint64_t> m_s_uniform_distribution(uint k, uint t);

  /**
   * @brief Capture the generator's position so it can be restored later.
   * @return The generator's internal words, 313 of them.
   */
  std::vector<uint64_t> get_state() const;

  /**
   * @brief Restore a position captured by get_state().
   * @param state Exactly what get_state() returned.
   * @note Anything else leaves the generator in whatever position the partial
   *       read produced, without reporting a failure.
   */
  void set_state(const std::vector<uint64_t> &state);
};

#endif // RRANDOM_H

#include "rrandom.h"
RRandom::RRandom() : engine_(0) {}
RRandom::RRandom(int _seed) : engine_(_seed) {}

bool RRandom::flip()
{
  std::uniform_int_distribution<int> dist(0, 1);
  return dist(engine_);
}
double RRandom::uniform_double()
{
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  return dist(engine_);
}
int RRandom::exponential_distribution(int mean)
{
  std::exponential_distribution<double> dist(1.0 / static_cast<double>(mean));
  return static_cast<int>(std::round(dist(engine_)));
}
int RRandom::poisson_distribution(double lambda)
{
  std::poisson_distribution<int> dist(lambda);
  return dist(engine_);
}

std::vector<uint64_t> RRandom::m_s_uniform_distribution(uint k, uint t)
{
  std::vector<uint64_t> event_times;
  for (uint i = 0; i < k; i++)
    event_times.push_back(uniform_int<uint64_t>(0, t - 1));

  return event_times;
}
std::vector<uint64_t> RRandom::get_state() const
{
  std::vector<uint64_t> state;
  std::stringstream ss;
  ss << engine_;

  uint64_t val;
  while (ss >> val)
  {
    state.push_back(val);
  }
  return state;
}

void RRandom::set_state(const std::vector<uint64_t> &state)
{
  std::stringstream ss;
  for (size_t i = 0; i < state.size(); ++i)
  {
    if (i > 0)
      ss << ' ';
    ss << state[i];
  }
  ss >> engine_;
}

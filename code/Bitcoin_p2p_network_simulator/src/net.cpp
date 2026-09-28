#include "net.h"

Net::Net(size_t _n_as, double _alpha, size_t _total_span, int _seed)
{
  generate_powerlaw_ranges(_n_as, _alpha, _total_span, _seed);
}

Net::~Net() {}

void Net::generate_powerlaw_ranges(size_t _n_as, double _alpha,
                                   size_t _total_span, int _seed)
{
  std::mt19937_64 gen(_seed);
  std::uniform_real_distribution<> uni{0.0, 1.0};
  std::vector<double> raw(_n_as);

  for (size_t i = 0; i < _n_as; ++i)
  {
    double u = uni(gen);
    raw[i] = std::pow(1.0 - u, -1.0 / _alpha);
  }

  // Scale to integer lengths summing to _total_span
  double sum_raw = std::accumulate(raw.begin(), raw.end(), 0.0);
  std::vector<uint32_t> lengths(_n_as);
  uint32_t acc = 0;

  for (size_t i = 0; i < _n_as; ++i)
  {
    lengths[i] = uint32_t(raw[i] / sum_raw * _total_span);
    acc += lengths[i];
  }

  if (acc < _total_span)
  {
    lengths[_n_as - 1] += (_total_span - acc);
  }

  mapping.reserve(_n_as);
  uint32_t start = 0;

  for (size_t i = 0; i < _n_as; ++i)
  {
    uint32_t len = lengths[i];
    uint32_t end = start + len - 1;
    mapping.push_back({start, end, uint32_t(i)});
    start += len;

    if (start >= _total_span)
      break;
  }

  std::cout << _n_as << " asmap was generated " << std::endl;
}
static constexpr uint16_t GROUP_BASE = 1000; // above every AS id
uint16_t Net::get_group(uint32_t addr)
{
  const Network n = net_of(addr);
  if (n == NET_ONION || n == NET_I2P || n == NET_CJDNS || n == NET_INTERNAL)
    return static_cast<uint16_t>(GROUP_BASE + n);
  return static_cast<uint16_t>(get_as_map(addr & ADDR_MASK));
}

int Net::get_as_map(uint32_t ip)
{
  if (mapping.empty())
    return -1;

  int left = 0;
  int right = int(mapping.size()) - 1;

  while (left <= right)
  {
    int mid = left + (right - left) / 2;
    const bucket &b = mapping[mid];

    if (ip < b.min)
      right = mid - 1;
    else if (ip > b.max)
      left = mid + 1;
    else
      return b.as_id;
  }
  return -1;
}

bucket Net::get_bucket_by_id(int id) { return mapping[id]; }
std::string Net::get_mapping_string()
{
  std::string mapping_str = "";
  for (auto &b : mapping)
  {
    mapping_str += std::to_string(b.as_id) + "," + std::to_string(b.min) + "," +
                   std::to_string(b.max) + "\n";
  }
  return mapping_str;
}
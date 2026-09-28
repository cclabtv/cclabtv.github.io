#include "core.h"
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <format>

std::atomic<bool> g_shutdown_requested{false};
Core *g_core = nullptr;

void signal_handler(int)
{
  std::cout << "\nCtrl+C received. Stopping gracefully..." << std::endl;
  g_shutdown_requested.store(true);
  if (g_core)
  {
    g_core->request_stop();
  }
}

int main(int argc, char *argv[])
{
  std::signal(SIGINT, signal_handler);

  auto start = std::chrono::high_resolution_clock::now();

  std::string resume_path;
  bool resuming = false;
  // Check if first argument is a resume path
  if (argc > 1)
  {
    std::string first_arg = argv[1];
    // Check if it's a directory containing state folder
    if (std::filesystem::exists(first_arg + "/state/core_state.bin"))
    {
      resume_path = first_arg;
      resuming = true;
      std::cout << "Resuming simulation from: " << resume_path << std::endl;
    }
  }
  std::vector<char *> args;
  args.push_back(argv[0]);
  for (int i = 1; i < argc; i++)
    if (!(resuming && resume_path == argv[i]))
      args.push_back(argv[i]);

  Parser P;
  Network_Config nc = P.load_config(
      (int)args.size(), args.data(),
      resuming ? resume_path + "/simulator.conf" : "simulator.conf");
  P.save_runtime_config(nc);
  int seed{nc.seed};

  RRandom _random(seed);
  Net net(500, 2.1, MAX_TRY_IPS, seed);
  Core core(net, seed, nc, 0);
  g_core = &core;

  if (resuming)
  {
    core.deserialize_state(resume_path);
  }

  uint32_t acc_inserted = 0, acc_scheduled = 0, acc_removed = 0;

  while (!core.should_stop())
  {
    auto [inserted_number, scheduled_number] = core.insert_peers();
    auto total_removed = core.remove_peers();
    core.change_peer_ip();
    acc_inserted += inserted_number;
    acc_scheduled += scheduled_number;
    acc_removed += total_removed;
    if (core.get_round() % 1000 == 0)
    {
      auto now = std::chrono::system_clock::now();
      auto tt = std::chrono::system_clock::to_time_t(now);

      std::tm local = *std::localtime(&tt);
      std::cout << std::put_time(&local, "%F %T")
                << " Round: " << core.get_round()
                << " Size: " << core.network.size()
                << " Pub: " << core.reachable_count
                << " NAT: " << (core.network.size() - core.reachable_count)
                << " New peers: " << acc_inserted
                << " Scheduled: " << acc_scheduled
                << " Total removed: " << acc_removed;
      acc_inserted = acc_scheduled = acc_removed = 0;
    }

    core.spark_next_round();
    core.increment_round();
    if (core.get_round() % 86400 == 0)
      core.collect_daily_telemetry();
    core.maybe_save_data();
    core.backup();
  }
  std::cout << "Saving remaining data..." << std::endl;
  core.save_all_data();

  std::cout << "Shutdown complete. Exiting." << std::endl;
  auto end = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
  std::cout << "Execution time: " << duration.count() << " ms" << std::endl;
}

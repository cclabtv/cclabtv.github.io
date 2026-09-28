# Bitcoin P2P Network Simulator

A discrete-event simulator of Bitcoin Core's peer-discovery layer (the address
manager, `AddrMan`, plus address gossip). It reproduces the **emergent topology**
of an unstructured Bitcoin-like network, at tens of thousands of nodes over
months of simulated time, and measures degree, assortativity, spectral structure
and address visibility. One round is one simulated second.

It is not a block- or transaction-propagation simulator. There is no consensus,
no mining, and no latency model below one second.

---

## What it models

- **Address manager**, a faithful port of Core's `AddrManImpl`: new (1024x64) and
  tried (256x64) tables, two-level per-node-salted bucket hashing, `is_terrible`,
  test-before-evict, feelers, the 1/2^n stochastic accept rule.
- **Address gossip**: VERSION/VERACK handshake, GETADDR/ADDR, the ADDR token
  bucket, 2-peer SipHash relay, trickle, self-announcement, DNS-seed bootstrap.
- **Reachability (routable-NAT model)**: unreachable nodes self-announce and
  relay like listening nodes but refuse inbound dials; DNS seeds never hand them out.
- **Churn**: Poisson arrivals, class-dependent exponential session lengths,
  dormant nodes that return with their tables, IP rotation for unreachable nodes.

---

## Dependencies

```
# Debian/Ubuntu
sudo apt install g++ make libboost-program-options-dev zlib1g-dev   # simulator
sudo apt install libeigen3-dev                                      # Data_Extractor
sudo apt install doxygen graphviz                                   # API docs (optional)
```

C++17 compiler required.

---

## Build, test, run

```
make                 # release build -> ./main
make test            # build + run the test suite (~3500 checks; non-zero exit on failure)
make debug           # ./main_debug  (-g -O0 -DDEBUG)
make asan            # ./main_asan   (ASan + UBSan)
make clean

./main               # fresh run: writes data/<unix_ts>/, reads ./simulator.conf
./main data/<ts>     # resume from a saved state (reads data/<ts>/simulator.conf)
SIM_THREADS=8 ./main # pin the worker-pool size
```

### Reproducibility contract (read this)

**A run is defined by three things: `simulator.conf`, the seed, and `SIM_THREADS`.**
Determinism is `f(seed, thread count)`. Change any of the three and you get a
_different sample path_, not a bug. `SIM_THREADS` unset falls back to
`hardware_concurrency()`, which is machine-dependent, so **set it explicitly** for
any result you intend to reproduce elsewhere. The seed lives in `simulator.conf`
(`[main] seed`).

---

## Configuration and stabilization

All parameters live in `simulator.conf`, grouped by what they control (address
tables, connection limits, table collisions, address ageing, address
broadcasting, reachability, session lengths, arrivals, resource limits). A
missing file is regenerated with defaults; an explicitly named missing file is a
fatal error (a run's config is part of its identity). Unknown keys in the file
are ignored, so a config written before a knob was retired still loads.

The live node count is **not enforced**; it self-stabilizes by Little's law:

```
N* = arrival_rate x E[session]
```

The shipped config targets **N\* ~ 40,000**:

| knob                   | value   | role                                                   |
| ---------------------- | ------- | ------------------------------------------------------ |
| `join_node_number`     | 20.83   | mean arrivals per window                               |
| `join_node_time`       | 900     | window length (s) -> 96 windows/day -> ~2000 nodes/day |
| `public_session_days`  | 5.6     | mean session, typical listening node                   |
| `public_core_fraction` | 0.04    | share of listening nodes drawn as long-lived backbone  |
| `public_core_days`     | 365     | mean session, backbone                                 |
| `nat_session_days`     | 20      | mean session, unreachable node                         |
| `prob_reachable`       | 0.26    | probability a node accepts inbound                     |
| `addrman_horizon`      | 2592000 | 30 days, Core's ADDRMAN_HORIZON                        |
| `dormant_cap`          | 2000    | non-zero spills departed nodes to disk                 |

Both peer classes are tuned to the **same ~20-day mean session** (unreachable = 20;
reachable = 0.96x5.6 + 0.04x365 ~ 20), so `N*` is `2000/day x 20 = 40,000`
**independent of the reachable/unreachable split**. Consequences:

- To retarget size, change `join_node_number` (arrivals scale linearly).
- To change the reachable share without moving size, change `prob_reachable`
  only, and keep both class means near 20 days.
- After any config change, expect a **~30 sim-day burn-in** for the addrman
  tables to fill before metrics are meaningful. `backup()` writes a resumable
  snapshot every 30 sim-days.

---

## Output layout

Each run writes to `data/<unix_ts>/`:

```
data/<ts>/
  simulator.conf              the exact config this run used
  nodes                       one line per node, written when it leaves
  edges/<address>             connections opened/closed, per source
  caches/<address>            advertised-address snapshots over time (the virtual crawler)
  map_info/<address>          full address-table dumps, per node
  ip_changes/ip_changes.csv   address rotations
  telemetry.csv               daily per-node counters (feeler success/fail, GETADDR served, ADDR dropped)
  state/, backup_N/           binary snapshots for resume
```

Column orders are a stable on-disk format; the record structs and their field
order are documented in `include/parser.h`. **Do not reorder fields without
updating `Data_Extractor`**, which parses these files.

---

## Analysis: Data_Extractor

The companion tool reads the `data/<ts>/` layout and computes graph metrics.

```
cd Data_Extractor
make                                   # -> ./main and ./compute_eigenvalues
./main [data_path] [output_path] [interval_seconds]
#   data_path        default: auto-discovered under data/
#   output_path      default: results/
#   interval_seconds default: 10800 (3 h) between computed snapshots
./compute_eigenvalues                  # spectral gap from snapshots/
```

It produces degree distribution, degree assortativity (full graph and the
reachable-induced subgraph), the normalized-Laplacian spectral gap, cache
coverage/accuracy, and address-visibility distributions. Plotting scripts live in
`Data_Extractor/plots.py`.

---

## API docs

The headers under `include/` carry Doxygen comments. To browse them:

```
doxygen Doxyfile        # -> docs/html/index.html   (needs doxygen + graphviz)
```

Navigate via the **Classes** and **Files** tabs. `docs/` is gitignored and
regenerated on demand.

# Results provenance

The CSV files and figures in this directory were generated from real program
runs on 22 October 2026. They are not analytical or synthesized measurements.

## Current validation environment

- Host: Windows 11 with WSL2, Linux kernel 6.6.87.2
- Compiler: GCC 13.3.0, C11, `-O2`
- Network: loopback; server and all clients ran on the same computer
- Workload: topic `cane`, ten random binary files of 1,258,291 bytes each
  (12,582,910 useful bytes total)
- Repetitions: three per point
- Part 1 concurrent clients: 1, 2, 4
- Part 2 connection failure probabilities: 0, 0.2, 0.5
- Enhanced connection counts: 1, 2, 4

The temporary workload was generated under `build/benchmark/` and is not part
of the repository. Its purpose is repeatable integration and instrumentation
validation, not to impersonate the Animals-10 data set.

## Commands

```sh
CLIENT_COUNTS="1 2 4" sh scripts/run_part1_experiments.sh build/benchmark/raw-img cane 3 Results
CONNECTION_PS="0 0.2 0.5" CHUNK_PS="0" sh scripts/run_case1_experiments.sh build/benchmark/raw-img cane 3 Results
CONNECTION_PS="0 0.2 0.5" CHUNK_PS="0" sh scripts/run_case2_experiments.sh build/benchmark/raw-img cane 3 Results
PARALLEL_NS="1 2 4" PROBABILITIES="0 0.2 0.5" sh scripts/run_case2_enhanced_experiments.sh build/benchmark/raw-img cane 3 Results
```

The plotting scripts require Python, NumPy, and Matplotlib. They fail when raw
CSV input is absent; they never manufacture replacement measurements.

## Submission limitation

The assignment asks for Animals-10 measurements using one server computer and
clients on two other computers. These loopback results verify the complete
measurement pipeline, but they are not a substitute for that required
three-computer experiment. Run `docs/THREE_COMPUTER_RUN.md` on the lab machines
and replace or augment these files before claiming that requirement is met.

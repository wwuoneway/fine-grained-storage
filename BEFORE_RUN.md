# Before a benchmark run

Isolate the CPUs (`isolcpus=` on the kernel command line, needs a reboot) and put
the same list in `FGS_BENCH_CPUS` in `.env`, which the runner passes to
`taskset -c` so the measured binary lands on those cores:

```bash
cat /sys/devices/system/cpu/isolated
```

```bash
sudo cpupower frequency-set --governor performance
cpupower frequency-info -o proc                          # verify

# ASLR off: same heap and stack layout every run, so cache and alignment
# effects stop moving between repetitions. Restore with 2.
echo 0 | sudo tee /proc/sys/kernel/randomize_va_space

sync
echo 3 | sudo tee /proc/sys/vm/drop_caches
```

Then run the benchmarks.

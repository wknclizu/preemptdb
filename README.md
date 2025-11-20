# PreemptDB: Low-Latency Transaction Scheduling via Userspace Interrupts

PreemptDB a database engine that supports lightweight userspace preemptible transactions enabled by Intel x86 user interrupt (uintr).
PreemptDB allows high-priority, short-transactions to quickly preempt and pause long-running, low-priority transactions which is resumed after the short-transaction concludes.

Check out more details in our [SIGMOD 2025 paper](https://kaisonghuang.github.io/resources/preemptdb-preprint.pdf) (received *Best Paper Award*!). If you use our work, please cite:

```
Low-Latency Transaction Scheduling via Userspace Interrupts: Why Wait or Yield When You Can Preempt?
Kaisong Huang, Jiatang Zhou, Zhuoyue Zhao, Dong Xie, Tianzheng Wang.
SIGMOD 2025 
```

### Step 1: Check if User Interrupt is Supported
Please build and run this [tool](https://github.com/UB-ADBLAB/check_uintr) to check if your CPU supports user interrupt.

### Step 2: Enable Kernel Support for User Interrupt
We have two options to enable kernel support for user interrupts:
1. Patched Kernel: We provide a patched kernel with uintr support. You can download it from [here](https://github.com/UB-ADBLAB/ubuntu-linux-uintr). Follow the instructions in the repository to build and install the kernel.
2. Character Device: If you prefer not using a patched kernel, you can use an alternative character device interface from [here](https://github.com/UB-ADBLAB/uintr-driver). This allows you to enable uintr support without modifying the kernel. If your kernel already enables uintr, do not use the character device. To build the system with the alternative device driver, please add `-DUSE_LIBUINTRDRIV=ON` flag to the cmake command in step 4.


### Step 3: Allocate Huge Pages
PreemptDB uses huge pages for memory allocation. You can allocate huge pages using the following command:
```bash
sudo sysctl -w vm.nr_hugepages=[preferred number of huge pages]

sudo sysctl -p
```

### Step 4: Build PreemptDB
```bash
git clone https://github.com/sfu-dis/preemptdb.git

cd preemptdb

mkdir build

cd build

# Default: Release, USE_LIBUINTRDRIV=OFF
cmake .. \
    -DCMAKE_BUILD_TYPE=[Debug|RelWithDebInfo|Release] \
    -DUSE_LIBUINTRDRIV=[ON|OFF] 

make
```

### Step 5: Run PreemptDB
Before you run PreemptDB, make sure you have chosen the desired parameter values:
- `--log_data_dir`: Directory for log data. Make sure the directory exists and is writable.
- `--node_memory_gb`: Memory allocated for each node in GB. Make sure the total memory allocated does not exceed the available hugepage memory.
- `--tpcc_scale_factor`: Scale factor for the TPC-C benchmark. This determines the size of the database.
- `--tpcc_workload_mix`: [New-Order, Payment, Credit-Check, Delivery, Order-Status, Stock-Level, TPC-H Query 2 variant, unused profile]: TPC-C transaction profiles with additional analytical query profiles. It is the workload mix run in the default transaction context. We typically use 0,0,0,0,0,0,100,0 which means only long-running TPC-H Query 2 is run in the default transaction context.
- `--tpcc_prioritized_workload_mix`: TPC-C transaction profiles with additional analytical query profiles. It is the workload mix run in the prioritized transaction context. We typically use 50,50,0,0,0,0,0,0 which means 50% TPC-C New-Order and 50% TPC-C Payment are run in the prioritized transaction context.
- `--scheduling_policy`: 1: Vanilla (FIFO wait), 2: Preemptive (PreemptDB), 3: Cooperative (userspace transaction context switch but not coroutine-based).
- `--prioritized_queue_size`: Size of the prioritized/preemptive transaction queue in each worker thread.
- `--global_queue_size`: Size of the global preemptive transaction queue in the uintr sender (admission control) thread.
- `--arrival_interval_us`: Arrival interval of preemptive transactions in microseconds.
- `--max_preempted_cycle_pct`: Maximum percentage of cycles that can be preempted from the default transaction context.
- `--latency_sample_size`: Number of latency samples to collect for each run.
```bash
mkdir /home/$USER/preemptdb-log

cd build

./benchmarks/tpcc/tpcc_SI_sequential \
--log_data_dir=/home/$USER/preemptdb-log \
--null_log_device=1 \
--pcommit=0 \
--node_memory_gb=64 \
--tpcc_scale_factor=8 \
--tpcc_workload_mix=0,0,0,0,0,0,100,0 \
--tpcc_prioritized_workload_mix=50,50,0,0,0,0,0,0 \
--scheduling_policy=2 \
--prioritized_queue_size=8 \
--global_queue_size=64 \
--arrival_interval_us=1000 \
--max_preempted_cycle_pct=100 \
--latency_sample_size=1000 \
--seconds=30 \
--threads=8
```

### Experiment Result
8 threads
```
========== Interrupt Timing Statistics ==========
Total _senduipi calls:          249701
Total interrupt_handler calls:  239991
  Normal path:                   239940 (99.98%)
  Quick exit path:               51 (0.02%)

Deliver Time (senduipi -> interrupt_handler_func):
  Samples: 239991
  Total:   184573590.71 ns
  Average: 769.09 ns
  Std Dev: 4866.78 ns
  Min:     374.29 ns
  Max:     800281.43 ns
  P50:     542.14 ns
  P90:     678.57 ns
  P95:     732.14 ns
  P99:     854.29 ns
  P99.9:   89975.71 ns

Switch Time (interrupt_handler_func start -> end):
  Total:   151575899.29 ns
  Average: 631.59 ns

  Normal Interrupt Path:
    Samples: 239940
    Count:   239940
    Switch Time Total:   151560605.71 ns
    Switch Time Average: 631.66 ns
    Std Dev: 218.28 ns
    Min:     199.29 ns
    Max:     13625.71 ns
    P50:     601.43 ns
    P90:     752.86 ns
    P95:     826.43 ns
    P99:     1067.14 ns
    P99.9:   2970.71 ns

  Quick Interrupt Path:
    Samples: 51
    Count:   51
    Switch Time Total:   15293.57 ns
    Switch Time Average: 299.87 ns
    Std Dev: 94.44 ns
    Min:     200.71 ns
    Max:     913.57 ns
    P50:     296.43 ns
    P90:     338.57 ns
    P95:     342.86 ns
    P99:     389.29 ns
    P99.9:   389.29 ns

Total Interrupt Handling Time:
  Total:   336149490.00 ns
  Average: 1400.68 ns
=================================================
```

31 threads
```
========== Interrupt Timing Statistics ==========
Total _senduipi calls:          240019
Total interrupt_handler calls:  239976
  Normal path:                   239963 (99.99%)
  Quick exit path:               13 (0.01%)

Deliver Time (senduipi -> interrupt_handler_func):
  Samples: 239976
  Total:   195659151.43 ns
  Average: 815.33 ns
  Std Dev: 6043.36 ns
  Min:     370.00 ns
  Max:     1127837.14 ns
  P50:     564.29 ns
  P90:     917.86 ns
  P95:     1075.00 ns
  P99:     1473.57 ns
  P99.9:   84451.43 ns

Switch Time (interrupt_handler_func start -> end):
  Total:   202886699.29 ns
  Average: 845.45 ns

  Normal Interrupt Path:
    Samples: 239963
    Count:   239963
    Switch Time Total:   202880773.57 ns
    Switch Time Average: 845.47 ns
    Std Dev: 367.72 ns
    Min:     216.43 ns
    Max:     16183.57 ns
    P50:     737.86 ns
    P90:     1245.71 ns
    P95:     1478.57 ns
    P99:     2298.57 ns
    P99.9:   3377.14 ns

  Quick Interrupt Path:
    Samples: 13
    Count:   13
    Switch Time Total:   5925.71 ns
    Switch Time Average: 455.82 ns
    Std Dev: 225.83 ns
    Min:     324.29 ns
    Max:     1227.14 ns
    P50:     400.00 ns
    P90:     428.57 ns
    P95:     462.14 ns
    P99:     462.14 ns
    P99.9:   462.14 ns

Total Interrupt Handling Time:
  Total:   398545850.71 ns
  Average: 1660.77 ns
=================================================
```
63 threads
```
========== Interrupt Timing Statistics ==========
Total _senduipi calls:          240002
Total interrupt_handler calls:  239931
  Normal path:                   239914 (99.99%)
  Quick exit path:               17 (0.01%)

Deliver Time (senduipi -> interrupt_handler_func):
  Samples: 239931
  Total:   433471110.00 ns
  Average: 1806.65 ns
  Std Dev: 7867.90 ns
  Min:     574.29 ns
  Max:     1143442.14 ns
  P50:     1104.29 ns
  P90:     2288.57 ns
  P95:     2857.14 ns
  P99:     4715.71 ns
  P99.9:   103417.14 ns

Switch Time (interrupt_handler_func start -> end):
  Total:   357474010.71 ns
  Average: 1489.90 ns

  Normal Interrupt Path:
    Samples: 239914
    Count:   239914
    Switch Time Total:   357464187.14 ns
    Switch Time Average: 1489.97 ns
    Std Dev: 880.19 ns
    Min:     221.43 ns
    Max:     17898.57 ns
    P50:     1207.14 ns
    P90:     2615.00 ns
    P95:     3252.14 ns
    P99:     4799.29 ns
    P99.9:   6852.14 ns

  Quick Interrupt Path:
    Samples: 17
    Count:   17
    Switch Time Total:   9823.57 ns
    Switch Time Average: 577.86 ns
    Std Dev: 346.40 ns
    Min:     222.14 ns
    Max:     1456.43 ns
    P50:     437.86 ns
    P90:     892.86 ns
    P95:     917.86 ns
    P99:     917.86 ns
    P99.9:   917.86 ns

Total Interrupt Handling Time:
  Total:   790945120.71 ns
  Average: 3296.55 ns
=================================================
```
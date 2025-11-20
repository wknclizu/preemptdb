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
Total _senduipi calls:          248510
Total interrupt_handler calls:  239991
  Normal path:                   239945 (99.98%)
  Quick exit path:               46 (0.02%)

Deliver Time (senduipi -> interrupt_handler_func):
  Samples: 239991
  Total:   172113857.14 ns
  Average: 717.17 ns
  Std Dev: 5523.45 ns
  Min:     370.71 ns
  Max:     936672.14 ns
  P50:     514.29 ns
  P90:     634.29 ns
  P95:     684.29 ns
  P99:     793.57 ns
  P99.9:   86203.57 ns

Switch Time (interrupt_handler_func start -> end):
  Total:   114514260.00 ns
  Average: 477.16 ns

  Normal Interrupt Path:
    Samples: 239945
    Count:   239945
    Switch Time Total:   114507460.00 ns
    Switch Time Average: 477.22 ns
    Std Dev: 153.67 ns
    Min:     99.29 ns
    Max:     46945.71 ns
    P50:     458.57 ns
    P90:     594.29 ns
    P95:     650.00 ns
    P99:     763.57 ns
    P99.9:   942.86 ns

  Quick Interrupt Path:
    Samples: 46
    Count:   46
    Switch Time Total:   6800.00 ns
    Switch Time Average: 147.83 ns
    Std Dev: 111.56 ns
    Min:     102.86 ns
    Max:     739.29 ns
    P50:     113.57 ns
    P90:     147.86 ns
    P95:     313.57 ns
    P99:     457.86 ns
    P99.9:   457.86 ns

Total Interrupt Handling Time:
  Total:   286628117.14 ns
  Average: 1194.33 ns
=================================================
```

31 threads
```
========== Interrupt Timing Statistics ==========
Total _senduipi calls:          240029
Total interrupt_handler calls:  239974
  Normal path:                   239961 (99.99%)
  Quick exit path:               13 (0.01%)

Deliver Time (senduipi -> interrupt_handler_func):
  Samples: 239974
  Total:   188108040.00 ns
  Average: 783.87 ns
  Std Dev: 6026.27 ns
  Min:     368.57 ns
  Max:     993612.86 ns
  P50:     518.57 ns
  P90:     862.14 ns
  P95:     1017.14 ns
  P99:     1366.43 ns
  P99.9:   85423.57 ns

Switch Time (interrupt_handler_func start -> end):
  Total:   168124490.71 ns
  Average: 700.59 ns

  Normal Interrupt Path:
    Samples: 239961
    Count:   239961
    Switch Time Total:   168121901.43 ns
    Switch Time Average: 700.62 ns
    Std Dev: 278.23 ns
    Min:     104.29 ns
    Max:     12613.57 ns
    P50:     645.71 ns
    P90:     1032.86 ns
    P95:     1200.71 ns
    P99:     1606.43 ns
    P99.9:   2600.71 ns

  Quick Interrupt Path:
    Samples: 13
    Count:   13
    Switch Time Total:   2589.29 ns
    Switch Time Average: 199.18 ns
    Std Dev: 188.41 ns
    Min:     84.29 ns
    Max:     807.86 ns
    P50:     127.86 ns
    P90:     162.86 ns
    P95:     371.43 ns
    P99:     371.43 ns
    P99.9:   371.43 ns

Total Interrupt Handling Time:
  Total:   356232530.71 ns
  Average: 1484.46 ns
=================================================
```
63 threads
```
========== Interrupt Timing Statistics ==========
Total _senduipi calls:          240019
Total interrupt_handler calls:  239919
  Normal path:                   239900 (99.99%)
  Quick exit path:               19 (0.01%)

Deliver Time (senduipi -> interrupt_handler_func):
  Samples: 239919
  Total:   430951373.57 ns
  Average: 1796.24 ns
  Std Dev: 9457.71 ns
  Min:     580.00 ns
  Max:     986242.14 ns
  P50:     949.29 ns
  P90:     1551.43 ns
  P95:     1830.00 ns
  P99:     4079.29 ns
  P99.9:   106830.00 ns

Switch Time (interrupt_handler_func start -> end):
  Total:   196280859.29 ns
  Average: 818.11 ns

  Normal Interrupt Path:
    Samples: 239900
    Count:   239900
    Switch Time Total:   196275985.71 ns
    Switch Time Average: 818.16 ns
    Std Dev: 394.43 ns
    Min:     114.29 ns
    Max:     13528.57 ns
    P50:     724.29 ns
    P90:     1290.00 ns
    P95:     1520.71 ns
    P99:     2140.00 ns
    P99.9:   3717.86 ns

  Quick Interrupt Path:
    Samples: 19
    Count:   19
    Switch Time Total:   4873.57 ns
    Switch Time Average: 256.50 ns
    Std Dev: 202.11 ns
    Min:     189.29 ns
    Max:     1109.29 ns
    P50:     205.71 ns
    P90:     232.86 ns
    P95:     285.00 ns
    P99:     285.00 ns
    P99.9:   285.00 ns

Total Interrupt Handling Time:
  Total:   627232232.86 ns
  Average: 2614.35 ns
=================================================
```
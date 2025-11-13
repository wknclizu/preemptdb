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
Total _senduipi calls:          250892
Total interrupt_handler calls:  239987
  Normal path:                   239951 (99.98%)
  Quick exit path:               36 (0.02%)

Deliver Time (senduipi -> interrupt_handler_func):
  Total:   176342930.00 ns
  Average: 734.80 ns

Switch Time (interrupt_handler_func start -> end):
  Total:   137598602.86 ns
  Average: 573.36 ns

  Normal Interrupt Path:
    Count:   239951
    Switch Time Total:   137591585.71 ns
    Switch Time Average: 573.42 ns

  Quick Interrupt Path:
    Count:   36
    Switch Time Total:   7017.14 ns
    Switch Time Average: 194.92 ns

Total Interrupt Handling Time:
  Total:   313941532.86 ns
  Average: 1308.16 ns
=================================================
```

2 threads
```
========== Interrupt Timing Statistics ==========
Total _senduipi calls:          117338133
Total interrupt_handler calls:  32364779
  Normal path:                   32336616 (99.91%)
  Quick exit path:               28163 (0.09%)

Deliver Time (senduipi -> interrupt_handler_func):
  Total:   2571569753.57 ns
  Average: 79.46 ns

Switch Time (interrupt_handler_func start -> end):
  Total:   17749341047.14 ns
  Average: 548.42 ns

  Normal Interrupt Path:
    Count:   32336616
    Switch Time Total:   17745789655.00 ns
    Switch Time Average: 548.78 ns

  Quick Interrupt Path:
    Count:   28163
    Switch Time Total:   3551392.14 ns
    Switch Time Average: 126.10 ns

Total Interrupt Handling Time:
  Total:   20320910800.71 ns
  Average: 627.87 ns
=================================================
```

31 threads
```
========== Interrupt Timing Statistics ==========
Total _senduipi calls:          239989
Total interrupt_handler calls:  239939
  Normal path:                   239924 (99.99%)
  Quick exit path:               15 (0.01%)

Deliver Time (senduipi -> interrupt_handler_func):
  Total:   206603758.57 ns
  Average: 861.07 ns

Switch Time (interrupt_handler_func start -> end):
  Total:   180008567.86 ns
  Average: 750.23 ns

  Normal Interrupt Path:
    Count:   239924
    Switch Time Total:   180002890.00 ns
    Switch Time Average: 750.25 ns

  Quick Interrupt Path:
    Count:   15
    Switch Time Total:   5677.86 ns
    Switch Time Average: 378.52 ns

Total Interrupt Handling Time:
  Total:   386612326.43 ns
  Average: 1611.29 ns
=================================================
```

63 threads
```
========== Interrupt Timing Statistics ==========
Total _senduipi calls:          240019
Total interrupt_handler calls:  239927
  Normal path:                   239907 (99.99%)
  Quick exit path:               20 (0.01%)

Deliver Time (senduipi -> interrupt_handler_func):
  Total:   400617197.86 ns
  Average: 1669.75 ns

Switch Time (interrupt_handler_func start -> end):
  Total:   215929592.14 ns
  Average: 899.98 ns

  Normal Interrupt Path:
    Count:   239907
    Switch Time Total:   215922160.00 ns
    Switch Time Average: 900.02 ns

  Quick Interrupt Path:
    Count:   20
    Switch Time Total:   7432.14 ns
    Switch Time Average: 371.61 ns

Total Interrupt Handling Time:
  Total:   616546790.00 ns
  Average: 2569.73 ns
=================================================
```
# PreemptDB: Low-Latency Transaction Scheduling via Userspace Interrupts

PreemptDB a database engine that supports lightweight userspace preemptible transactions enabled by Intel x86 user interrupt (uintr).
PreemptDB allows high-priority, short-transactions to quickly preempt and pause long-running, low-priority transactions which is resumed after the short-transaction concludes.

Check out more details in our [SIGMOD 2025 paper](https://kaisonghuang.github.io/resources/preemptdb-preprint.pdf) (received *Best Paper Award*!). If you use our work, please cite:

```
Low-Latency Transaction Scheduling via Userspace Interrupts: Why Wait or Yield When You Can Preempt?
Kaisong Huang, Jiatang Zhou, Zhuoyue Zhao, Dong Xie, Tianzheng Wang.
SIGMOD 2025 
```

### Stay tuned for build and run instructions

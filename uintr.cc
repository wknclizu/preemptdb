#include "uintr.h"
#include "dbcore/sm-thread.h"
#include <cassert>

thread_local volatile uint32_t worker_id = ~uint32_t{0};
thread_local std::atomic<uint64_t> lock_counter;
uint64_t interrupt_start_timestamps[MAX_CORES];
pcontext* curr_ctx[MAX_CORES];

// Global variables for timing measurements
std::atomic<int64_t> g_senduipi_count{0};
std::atomic<int64_t> g_total_deliver_time{0};
std::atomic<int64_t> g_total_switch_time{0};
std::atomic<uint64_t> g_senduipi_timestamps[MAX_CORES];
std::atomic<int64_t> g_switch_time_normal{0};
std::atomic<int64_t> g_switch_time_quick{0};

uint64_t g_deliver_time_samples[MAX_TIMING_SAMPLES] = {0};
uint64_t g_switch_time_normal_samples[MAX_TIMING_SAMPLES] = {0};
uint64_t g_switch_time_quick_samples[MAX_TIMING_SAMPLES] = {0};

std::atomic<size_t> g_deliver_sample_count{0};
std::atomic<size_t> g_interrupt_normal_count{0};
std::atomic<size_t> g_interrupt_quick_count{0};

pcontext::pcontext() {
  lock_counter.store(0);
  start_timestamp = 0;
  preempted_cycles = 0;
}

uint32_t GetWorkerId() {
  ASSERT(worker_id != ~uint32_t{0});
  return worker_id;
}

void pcontext::Set_Worker_Id(uint32_t id) {
  ASSERT(worker_id == ~uint32_t{0});
  worker_id = id;
}

pcontext* pcontext::get_current_context() {
  if(curr_ctx[GetWorkerId()] == nullptr){
    pcontext::set_current_context(ermia::thread::Thread::MainContext());
  }
  return curr_ctx[GetWorkerId()];
}

void pcontext::set_current_context(pcontext* ctx) {
  curr_ctx[GetWorkerId()] = ctx;
}

uint64_t ReadFSBase() {
  return _readfsbase_u64();
}

void pcontext::set_lock_counter(uint64_t counter) {
  lock_counter.store(counter);
}

uint64_t pcontext::get_lock_counter() {
  return lock_counter.load();
}

bool pcontext::locked() {
  return lock_counter.load() > 0;
}

void pcontext::lock() {
  lock_counter.fetch_add(1);
}

void pcontext::unlock() {
  lock_counter.fetch_sub(1);
}

void pcontext::reset_timer() {
  start_timestamp = rdtsc();
  preempted_cycles = 0;
}

void pcontext::add_preempted_time(uint64_t cycles) {
  preempted_cycles += cycles;
}

bool pcontext::starved() {
  uint64_t total_cycles = rdtsc() - start_timestamp;
  return 100 * (float)preempted_cycles / (float)total_cycles > ermia::config::max_preempted_cycle_pct;
}

void pcontext::SetRSP(void *rsp){
  reg[0] = rsp;
}

bool pcontext::ValidRSP(void *rsp){
  return (uint64_t)rsp >= stack_start && (uint64_t)rsp <= stack_end;
}

void pcontext::xsave() {
  _xsaveopt64(xsave_area, ~uint64_t{0});
}

void pcontext::xrstor() {
  if (!new_context) {
    _xrstor64(xsave_area, ~uint64_t{0});
  } else {
    new_context = false;
  }
}

extern "C" void record_interrupt_start() {
  uint64_t recv_timestamp = RdtscClock::now();
  uint32_t wid = GetWorkerId();
  interrupt_start_timestamps[wid] = recv_timestamp;
  // g_interrupt_handler_count.fetch_add(1, std::memory_order_relaxed);
  
  uint64_t send_timestamp = g_senduipi_timestamps[wid].load(std::memory_order_acquire);
  assert(send_timestamp != 0);
  uint64_t deliver_time = recv_timestamp - send_timestamp;
  g_total_deliver_time.fetch_add(deliver_time, std::memory_order_relaxed);
  
  size_t idx = g_deliver_sample_count.fetch_add(1, std::memory_order_relaxed);
  assert(idx < MAX_TIMING_SAMPLES);
  g_deliver_time_samples[idx] = deliver_time;
  
  g_total_switch_time.fetch_sub(recv_timestamp, std::memory_order_relaxed);
}

extern "C" void record_interrupt_end_normal() {
  uint64_t timestamp = RdtscClock::now();
  uint32_t wid = GetWorkerId();
  uint64_t switch_time = timestamp - interrupt_start_timestamps[wid];
  g_total_switch_time.fetch_add(timestamp, std::memory_order_relaxed);
  g_switch_time_normal.fetch_add(switch_time, std::memory_order_relaxed);
  
  size_t idx = g_interrupt_normal_count.fetch_add(1, std::memory_order_relaxed);
  assert(idx < MAX_TIMING_SAMPLES);
  g_switch_time_normal_samples[idx] = switch_time;
}

extern "C" void record_interrupt_end_quick() {
  uint64_t timestamp = RdtscClock::now();
  uint32_t wid = GetWorkerId();
  uint64_t switch_time = timestamp - interrupt_start_timestamps[wid];
  g_total_switch_time.fetch_add(timestamp, std::memory_order_relaxed);
  g_switch_time_quick.fetch_add(switch_time, std::memory_order_relaxed);
  
  size_t idx = g_interrupt_quick_count.fetch_add(1, std::memory_order_relaxed);
  assert(idx < MAX_TIMING_SAMPLES);
  g_switch_time_quick_samples[idx] = switch_time;
}

extern "C" void* handler_helper(void* rsp) {
  if (pcontext::locked()) {
    return rsp;
  }
  auto wid = worker_id;
  pcontext::get_current_context()->SetRSP(rsp);
  pcontext::get_current_context()->xsave();
  pcontext::set_current_context(ermia::thread::Thread::PreemptiveContext());
  pcontext::get_current_context()->xrstor();
  void* sp = pcontext::get_current_context()->reg[0];
  auto fs = pcontext::get_current_context()->fs;
  auto gs = pcontext::get_current_context()->gs;
  ASSERT(fs != 0);
  _writefsbase_u64(fs);
  _writegsbase_u64(gs);
  if (worker_id == ~uint32_t{0}) {
    worker_id = wid;
  }
  return sp;
}

extern "C" void swap_helper(pcontext* next_ctx) {
  auto wid = worker_id;
  pcontext::get_current_context()->xsave();
  pcontext::set_current_context(next_ctx);
  next_ctx->xrstor();
  auto fs = next_ctx->fs;
  auto gs = next_ctx->gs;
  ASSERT(fs != 0);
  _writefsbase_u64(fs);
  _writegsbase_u64(gs);
  if (worker_id == ~uint32_t{0}) {
    worker_id = wid;
  }
}

asm(R"(
  .text
	.globl	interrupt_handler_func
	.type	interrupt_handler_func, @function

interrupt_handler_func:
	.cfi_startproc
	.cfi_def_cfa_offset 16

	endbr64
  // stack 
  // ui_frame: rsp
  //           rflags
  //           rip
  // vector <- rsp point to
  // so need to add 8 to point to skip vector

  addq $8, %rsp      # pop vector
  pushq %rax
  pushq %rbx

  pushq %r11
  pushq %r10
  pushq %r9
  pushq %r8
  pushq %rcx
  pushq %rdx
  pushq %rsi
  pushq %rdi
  subq $8, %rsp      # align stack to 16 bytes before call
  cld
  call record_interrupt_start
  addq $8, %rsp
  popq %rdi
  popq %rsi
  popq %rdx
  popq %rcx
  popq %r8
  popq %r9
  popq %r10
  popq %r11

  // check if rip is in swap_context, if so, quick exit
  movq 0x10(%rsp), %rax # rax = rip

.check_swap_context:
  leaq .l_end_swap_context(%rip), %rbx
  cmpq %rbx, %rax #
  jg .continue_uintr_handler # if rip > .l_end_swap_context, then continue uintr
  leaq .l_swap_context(%rip), %rbx
  cmpq %rbx, %rax
  jg .uintr_quick_exit # if rip > .l_swap_context, then quick exit uintr

.continue_uintr_handler:
  pushq %rcx
  pushq %rdx
  pushq %rbp
  pushq %rsi
  pushq %rdi
  pushq %r8
  pushq %r9
  pushq %r10
  pushq %r11
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15

  # call handler_helper, pass current rsp as first param
  movq  %rsp, %rdi
  cld
  call handler_helper

  # rax = curr_ctx
  # switch stack, rsp = curr_ctx->reg[0]
  movq %rax, %rsp

  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %r11
  popq %r10
  popq %r9
  popq %r8
  popq %rdi
  popq %rsi
  popq %rbp
  popq %rdx
  popq %rcx
  popq %rbx
  popq %rax

  # Record interrupt end time before uiret (normal path)
  pushq %r11
  pushq %r10
  pushq %r9
  pushq %r8
  pushq %rax
  pushq %rcx
  pushq %rdx
  pushq %rsi
  pushq %rdi
  subq $8, %rsp      # align stack to 16 bytes before call
  cld
  call record_interrupt_end_normal
  addq $8, %rsp
  popq %rdi
  popq %rsi
  popq %rdx
  popq %rcx
  popq %rax
  popq %r8
  popq %r9
  popq %r10
  popq %r11

	uiret

.uintr_quick_exit:
  # Also record end time for quick exit (quick path)
  pushq %r11
  pushq %r10
  pushq %r9
  pushq %r8
  pushq %rcx
  pushq %rdx
  pushq %rsi
  pushq %rdi
  subq $8, %rsp      # align stack to 16 bytes before call
  cld
  call record_interrupt_end_quick
  addq $8, %rsp
  popq %rdi
  popq %rsi
  popq %rdx
  popq %rcx
  popq %r8
  popq %r9
  popq %r10
  popq %r11

  popq %rbx
  popq %rax
	uiret

	.cfi_endproc
  .size	interrupt_handler_func, .-interrupt_handler_func
  .section	.rodata
  .text
)");

asm(R"(
  .text
	.globl	init_stack
	.type	init_stack, @function

init_stack:
  .cfi_startproc
  endbr64
  # temp save rsp
  movq %rsp, %rcx

  movq %rdi, %rsp

  # build ui_frame
  andq $0xfffffffffffffff0, %rsp
  pushq %rdi
  pushfq
  pushq %rsi  # important: 2nd paramater, function pointer

  pushq %rax
  pushq %rbx
  pushq %rcx
  pushq %rdx
  pushq %rdi    # important: rdi is rbp
  pushq %rsi
  pushq %rdi
  pushq %r8
  pushq %r9
  pushq %r10
  pushq %r11
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15

  # save rsp as return value
  movq %rsp, %rax

  movq %rcx, %rsp
  ret
  .cfi_endproc
  .size	init_stack, .-init_stack
  .section	.rodata
  .text
)");

asm(R"(
  .text
	.globl	swap_context
	.type	swap_context, @function

swap_context:
.l_swap_context:
  .cfi_startproc
  endbr64
  clui

  # simulate uintr frame
  // stack 
  // ui_frame: rsp
  //           rflags
  //           rip
  // use caller saved register
  // %rcx = rip
  popq  %rcx
  pushq %rsp
  pushfq
  pushq %rcx

  # caller saved registers are dummy data, 
  # still need to save them, but they don't need to be accurate
  pushq %rax
  pushq %rbx
  pushq %rcx
  pushq %rdx
  pushq %rbp
  pushq %rsi
  pushq %rdi
  pushq %r8
  pushq %r9
  pushq %r10
  pushq %r11
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15

  # current_context.reg[0] = rsp
  movq  %rsp, 0x2000(%rdi)

  # temperatory save rsi (2nd param)
  pushq %rsi
  movq %rsi, %rdi
  cld
  call swap_helper
  popq %rsi

  # switch stack
  movq 0x2000(%rsi), %rsp
  
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %r11
  popq %r10
  popq %r9
  popq %r8
  popq %rdi
  popq %rsi
  popq %rbp
  popq %rdx
  popq %rcx
  
 
  // rsp
  // rflags
  // rip
  // rax 
  // rbx <- rsp points to

  movq 32(%rsp), %rax
  // rbx = rip
  movq 16(%rsp), %rbx
  movq %rbx, -0x80(%rax)
  popq %rbx
  popq %rax
  addq $8, %rsp
  popfq
  popq %rsp
  stui
  jmp *-0x80(%rsp)

.l_end_swap_context:
  nop

  .cfi_endproc
  .size	swap_context, .-swap_context
  .section	.rodata
  .text
)");

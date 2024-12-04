#include <numa.h>
#include <sched.h>
#include <sys/mman.h>

#include <atomic>
#include <future>

#include "sm-alloc.h"
//#include "sm-chkpt.h"
#include "sm-common.h"
#include "sm-object.h"
#include "../txn.h"
#include "../uintr.h"

namespace ermia {
namespace MM {

// tzwang (2015-11-01):
// gc_lsn is the LSN of the no-longer-needed versions, which is the end LSN
// of the epoch that most recently reclaimed (i.e., all **readers** are gone).
// It corresponds to two epochs ago of the most recently reclaimed epoch (in
// which all the **creators** are gone). Note that gc_lsn only records the
// LSN when the *creator* of the versions left---not the *visitor* of those
// versions. So consider the versions created in epoch N, we can have stragglers
// up to at most epoch N+2; to start a new epoch N+3, N must be reclaimed. This
// means versions last *visited* (not created!) in epoch N+2 are no longer
// needed after epoch N+4 is reclaimed.
//
// Note: when really recycling versions, we also need to make sure we leave
// one committed versions that is older than the begin LSN of any transaction.
// This "begin LSN of any tx" translates to the current log LSN for normal
// transactions, and the safesnap LSN for read-only transactions using a safe
// snapshot. For simplicity, the daemon simply leave one version with LSN < the
// gc_lsn available (so no need to figure out which safesnap lsn is the
// appropriate one to rely on - it changes as the daemon does its work).
uint64_t gc_lsn CACHE_ALIGNED;
epoch_num gc_epoch CACHE_ALIGNED;

static const uint64_t EPOCH_SIZE_NBYTES = 1 << 24;
static const uint64_t EPOCH_SIZE_COUNT = 2000;

// epoch_excl_begin_lsn belongs to the previous **ending** epoch, and we're
// using it as the **begin** lsn of the new epoch.
uint64_t epoch_excl_begin_lsn[3] = {0, 0, 0};
uint64_t epoch_reclaim_lsn[3] = {0, 0, 0};

static thread_local struct context_data sm_alloc_context_data CACHE_ALIGNED;

uint64_t safesnap_lsn = 0;

thread_local TlsFreeObjectPool *cls_free_object_pool CACHE_ALIGNED;
char **node_memory = nullptr;
uint64_t *allocated_node_memory = nullptr;
static uint64_t thread_local cls_allocated_node_memory CACHE_ALIGNED;
static const uint64_t tls_node_memory_mb = 200;

void prepare_node_memory() {
  if (!config::tls_alloc) {
    return;
  }

  ALWAYS_ASSERT(config::numa_nodes);
  allocated_node_memory =
      (uint64_t *)malloc(sizeof(uint64_t) * config::numa_nodes);
  node_memory = (char **)malloc(sizeof(char *) * config::numa_nodes);
  std::vector<std::future<void> > futures;
  LOG(INFO) << "Will run and allocate on " << config::numa_nodes << " nodes, "
            << config::node_memory_gb << "GB each";
  for (int i = 0; i < config::numa_nodes; i++) {
    LOG(INFO) << "Allocating " << config::node_memory_gb << "GB on node " << i;
    auto f = [=] {
      ALWAYS_ASSERT(config::node_memory_gb);
      allocated_node_memory[i] = 0;
      numa_set_preferred(i);
      node_memory[i] = (char *)mmap(
          nullptr, config::node_memory_gb * config::GB, PROT_READ | PROT_WRITE,
          MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB | MAP_POPULATE, -1, 0);
      THROW_IF(node_memory[i] == nullptr or node_memory[i] == MAP_FAILED,
               os_error, errno, "Unable to allocate huge pages");
      ALWAYS_ASSERT(node_memory[i]);
      LOG(INFO) << "Allocated " << config::node_memory_gb << "GB on node " << i;
    };
    futures.push_back(std::async(std::launch::async, f));
  }
  for (auto &f : futures) {
    f.get();
  }
}

void gc_version_chain(fat_ptr *oid_entry) {}

void *allocate(size_t size) {
  size = align_up(size);
  if (!config::tls_alloc) {
    return malloc(size);
  }

  void *p = NULL;

  // Try the tls free object store first
  if (cls_free_object_pool) {
    auto size_code = encode_size_aligned(size);
    fat_ptr ptr = cls_free_object_pool->Get(size_code);
    if (ptr.offset()) {
      p = (void *)ptr.offset();
      goto out;
    }
  }

  ALWAYS_ASSERT(not p);
  // Have to use the vanilla bump allocator, hopefully later we reuse them
  static thread_local char *cls_node_memory CACHE_ALIGNED;
  if (unlikely(not cls_node_memory) or
      cls_allocated_node_memory + size >= tls_node_memory_mb * config::MB) {
    cls_node_memory = (char *)allocate_onnode(tls_node_memory_mb * config::MB);
    cls_allocated_node_memory = 0;
  }

  if (likely(cls_node_memory)) {
    p = cls_node_memory + cls_allocated_node_memory;
    cls_allocated_node_memory += size;
    goto out;
  }

out:
  if (not p) {
    LOG(FATAL) << "Out of memory";
  }
  sm_alloc_context_data.nbytes += size;
  sm_alloc_context_data.counts += 1;
  return p;
}

// Allocate memory directly from the node pool
void *allocate_onnode(size_t size) {
  size = align_up(size);
  auto node = numa_node_of_cpu(sched_getcpu());
  ALWAYS_ASSERT(node < config::numa_nodes);
  auto offset = __sync_fetch_and_add(&allocated_node_memory[node], size);
  if (likely(offset + size <= config::node_memory_gb * config::GB)) {
    return node_memory[node] + offset;
  }
  return nullptr;
}

void deallocate(fat_ptr p) {}

// epoch mgr callbacks
void global_init(void *) {
  volatile_write(gc_lsn, 0);
  volatile_write(gc_epoch, 0);
}

epoch_mgr::cls_storage *get_cls(void *) {
  static thread_local epoch_mgr::cls_storage sm_alloc_cls;
  return &sm_alloc_cls;
}

void *context_registered(void *) {
  sm_alloc_context_data.initialized = true;
  sm_alloc_context_data.nbytes = 0;
  sm_alloc_context_data.counts = 0;
  return &sm_alloc_context_data;
}

void context_deregistered(void *cookie, void *context_cookie) {
  MARK_REFERENCED(cookie);
  auto *c = (context_data *)context_cookie;
  ASSERT(c == &sm_alloc_context_data);
  c->initialized = false;
  c->nbytes = 0;
  c->counts = 0;
}

void *epoch_ended(void *cookie, epoch_num e) {
  MARK_REFERENCED(cookie);
  // remember the epoch number so we can find it out when it's reclaimed later
  epoch_num *epoch = (epoch_num *)malloc(sizeof(epoch_num));
  *epoch = e;
  return (void *)epoch;
}

void *epoch_ended_context(void *cookie, void *epoch_cookie,
                         void *context_cookie) {
  MARK_REFERENCED(cookie);
  MARK_REFERENCED(context_cookie);
  return epoch_cookie;
}

void epoch_reclaimed(void *cookie, void *epoch_cookie) {
  MARK_REFERENCED(cookie);
  epoch_num e = *(epoch_num *)epoch_cookie;
  free(epoch_cookie);
  uint64_t my_begin_lsn = epoch_excl_begin_lsn[e % 3];
  if (!config::enable_gc || my_begin_lsn == 0) {
    return;
  }
  epoch_reclaim_lsn[e % 3] = my_begin_lsn;
  if (config::enable_safesnap) {
    // Make versions created during epoch N available for transactions
    // using safesnap. All transactions that created something in this
    // epoch has gone, so it's impossible for a reader using that
    // epoch's end lsn as both begin and commit timestamp to have
    // conflicts with these writer transactions. But future transactions
    // need to start with a pstamp of safesnap_lsn.
    // Take max here because after the loading phase we directly set
    // safesnap_lsn to log.cur_lsn, which might be actually larger than
    // safesnap_lsn generated during the loading phase, which is too
    // conservertive that some tx might not be able to see the tuples
    // because they were not loaded at that time...
    // Also use epoch e+1's begin LSN = epoch e's end LSN
    auto new_safesnap_lsn = epoch_excl_begin_lsn[(e + 1) % 3];
    volatile_write(safesnap_lsn, std::max(safesnap_lsn, new_safesnap_lsn));
  }
  if (e >= 2) {
    volatile_write(gc_lsn, epoch_reclaim_lsn[(e - 2) % 3]);
    volatile_write(gc_epoch, e - 2);
    epoch_reclaim_lsn[(e - 2) % 3] = 0;
  }
}

void epoch_exit(uint64_t s, epoch_num e) {
  // Transactions under a safesnap will pass s = 0 (INVALID_LSN)
  if (s != 0 && (sm_alloc_context_data.nbytes >= EPOCH_SIZE_NBYTES ||
                 sm_alloc_context_data.counts >= EPOCH_SIZE_COUNT)) {
    // epoch_ended() (which is called by new_epoch() in its critical section)
    // will pick up this safe lsn if new_epoch() succeeded (it could also be
    // set by somebody else who's also doing epoch_exit(), but this is captured
    // before new_epoch succeeds, so we're fine).  If instead, we do this in
    // epoch_ended(), that cur_lsn captured actually belongs to the *new* epoch
    // - not safe to gc based on this lsn. The real gc_lsn should be some lsn
    // at the end of the ending epoch, not some lsn after the next epoch.
    epoch_excl_begin_lsn[(e + 1) % 3] = s;
    if (mm_epochs.new_epoch_possible() && mm_epochs.new_epoch()) {
      sm_alloc_context_data.nbytes = sm_alloc_context_data.counts = 0;
    }
  }
  mm_epochs.context_exit();
}

epoch_mgr mm_epochs{{nullptr, &global_init, &get_cls, &context_registered,
                     &context_deregistered, &epoch_ended, &epoch_ended_context,
                     &epoch_reclaimed}};
}  // namespace MM
}  // namespace ermia

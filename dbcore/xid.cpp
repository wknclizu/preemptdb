#include "xid.h"
#include "epoch.h"
#include "serial.h"
#include "../txn.h"
#include <atomic>
#include <mutex>
#include <unistd.h>

namespace ermia {
namespace TXN {

/* There are a fixed number of transaction contexts in the system
   (some thousands of them), and our primary task is to create unique
   XIDs and then assign them contexts in a way that no two XID map to
   the same context at the same time. We accomplish this as follows:

   The system maintains four bitmaps:

              00                    01
   00 |................|    |................|

   10 |................|    |................|

   One row of bitmaps identifies contexts that are available for
   immediate allocation, and the other identifies contexts that have
   been recently freed (keeping those separate reduces contention and
   avoids ABA issues).

   Whenever the allocation bitmap row is exhausted, the system swaps
   labels, turning the "recently-freed" bitmap (which should be pretty
   full) into the "available" map, and vice-versa. Each row thus has a
   bit that maps to a given context.

   Meanwhile, the columns divide the allocation process into
   epochs. Allocation can only change columns (and possibly rows as
   well) if all stragglers have finished, thus solving the ABA problem
   where a straggler manages to reallocate the same context in the
   same epoch, after the context has been used and freed.

   As a concrete example, the system starts allocating from bitmap 00
   (in epoch 0); once 00 is exhausted, it advances to epoch 1 and
   begins allocating from bitmap 01. Once bitmap 01 is empty, the
   system advances to bitmap 10 (logically swapping the rows) and
   begins recycling contexts that were allocated from bitmap 00 and
   have since been freed. Allocation continues through 10 and 11 and
   eventually wraps around to 00 again. Now we have to worry about
   stragglers (some really old transaction could have started in epoch
   0 and---not realizing we're now in epoch 4---might end up with a
   TID from epoch 0 that duplicates the tid from two allocation cycles
   ago. This is where the epoch manager comes in: it can track
   stragglers and restricts the number of concurrent epochs so that
   the problem cannot arise.
 */
xid_context xid_contexts[NCONTEXTS];

xid_bitmap xid_bitmaps[NBITMAPS];

thread_local context_data xid_context_data CACHE_ALIGNED;

/***************************************
 * * * Callbacks for the epoch_mgr * * *
 ***************************************/
void global_init(void *) {
  /* Set the first row to all ones so we have something to allocate */
  for (int i = 0; i < 2; i++) {
    for (auto &w : xid_bitmaps[i].data) w = ~uint64_t(0);
  }
}

epoch_mgr::cls_storage *get_cls(void *) {
  static thread_local epoch_mgr::cls_storage xid_cls;
  return &xid_cls;
}

void *context_registered(void *) {
  xid_context_data.epoch = 0;
  xid_context_data.bitmap = 0;
  xid_context_data.base_id = 0;
  xid_context_data.initialized = true;
  return &xid_context_data;
}

void context_deregistered(void *, void *context_cookie) {
  auto *c = (context_data *)context_cookie;
  ASSERT(c == &xid_context_data);
  /*
   * FIXME(tzwang): 20191228: seems unnecessary?
  while (c->bitmap) {
    auto x = take_one(c);
    xid_free(x);
  }
  */
  c->initialized = false;
}

/* Don't need these... we track resources a different way
 */
void *epoch_ended(void *, epoch_mgr::epoch_num) { 
  return 0;
}

void *epoch_ended_context(void *, void *epoch_cookie, void *) {
  return epoch_cookie;
}

void epoch_reclaimed(void *, void *) {}

epoch_mgr xid_epochs{{nullptr, &global_init, &get_cls, &context_registered,
                      &context_deregistered, &epoch_ended, &epoch_ended_context,
                      &epoch_reclaimed}};

std::mutex xid_mutex;

XID xid_alloc() {
  pcontext::lock();
  if (not xid_context_data.initialized) {
    xid_epochs.context_init();
  }
  pcontext::unlock();

  while (not xid_context_data.bitmap) {
    /* Grab a whole machine word at a time. Use the epoch_mgr to
       protect us if we happen to straggle. Note that we may
       (through bad luck) acquire an empty word and need to retry.
     */
    auto e = xid_epochs.context_enter();
    DEFER_UNLESS(exited, xid_epochs.context_exit());
    auto &b = xid_bitmaps[e % NBITMAPS];
    auto i = volatile_read(b.widx);
    ASSERT(i <= xid_bitmap::NWORDS);
    while (i < xid_bitmap::NWORDS) {
      auto j = __sync_val_compare_and_swap(&b.widx, i, i + 1);
      if (j == i) {
        /* NOTE: no need for a goto: the compiler will thread
           the jump so we skip the overflow check entirely
        */
        break;
      }
      i = j;
    }

    if (i == xid_bitmap::NWORDS) {
      // overflow!
      xid_epochs.context_exit();
      exited = true;

      pcontext::lock();
      xid_mutex.lock();
      DEFER({xid_mutex.unlock(); pcontext::unlock();});

      if (e == xid_epochs.get_cur_epoch()) {
        /* Still at end, try to open a new epoch.

           If there are stragglers (highly unlikely) then
           sleep until they leave.
         */
        xid_bitmaps[(e + 1) % NBITMAPS].widx = 0;
        while (not xid_epochs.new_epoch()) usleep(1000);
      }

      continue;
    }

    xid_context_data.epoch = e;
    xid_context_data.base_id = (e % 2) * NCONTEXTS / 2 + i * xid_bitmap::BITS_PER_WORD;
    std::swap(xid_context_data.bitmap, b.data[i]);
  }

  return take_one(&xid_context_data);
}
#ifdef SSN
bool xid_context::set_sstamp(uint64_t s) {
  ALWAYS_ASSERT(!(s & xid_context::sstamp_final_mark));
  // If I'm not read-mostly, nobody else would call this
  if (xct->is_read_mostly() && config::ssn_read_opt_enabled()) {
    // This has to be a CAS because with read-optimization, the updater might
    // need
    // to update the reader's sstamp.
    uint64_t ss = sstamp.load(std::memory_order_acquire);
    do {
      if (ss & sstamp_final_mark) {
        return false;
      }
    } while ((ss == 0 || ss > s) &&
             !std::atomic_compare_exchange_strong(&sstamp, &ss, s));
  } else {
    sstamp.store(std::min(sstamp.load(std::memory_order_relaxed), s),
                 std::memory_order_relaxed);
  }
  return true;
}
#endif
}  // namespace TXN
}  // namespace ermia

#include "epoch.h"

#include <pthread.h>
#include <tuple>
#include <utility>

#define LOG(msg, ...) fprintf(stderr, msg "\n", ##__VA_ARGS__)

size_t x = 1;

void global_init(void *arg) { LOG("Initializing"); }
epoch_mgr::cls_storage *get_cls(void *) {
  static __thread epoch_mgr::cls_storage s;
  return &s;
}
void *context_registered(void *) {
  LOG("Context %zd registered", (size_t)pthread_self());
  return 0;
}
void context_deregistered(void *cookie, void *context_cookie) {
  LOG("Context %zd deregistered", (size_t)pthread_self());
}
void *epoch_ended(void *, epoch_mgr::epoch_num x) {
  LOG("Epoch %zd ended", x);
  return (void *)x;
}

void *epoch_ended_context(void *cookie, void *epoch_cookie,
                         void *context_cookie) {
  return epoch_cookie;
}
void epoch_reclaimed(void *cookie, void *epoch_cookie) {
  LOG("Epoch %zd reclaimed", (size_t)epoch_cookie);
}

struct state {};

static state s;

static epoch_mgr em{{&s, &global_init, &get_cls, &context_registered,
                     &context_deregistered, &epoch_ended, &epoch_ended_context,
                     &epoch_reclaimed}};

int main() {
  em.context_init();
  LOG("context_enter");
  em.context_enter();
  em.new_epoch();
  em.new_epoch();
  em.new_epoch();
  LOG("context_quiesce");
  em.context_quiesce();
  em.new_epoch();
  em.new_epoch();
  em.new_epoch();
  LOG("context_exit");
  em.context_exit();
  em.new_epoch();
  em.new_epoch();
  em.new_epoch();
  DEFER(em.context_fini());
}

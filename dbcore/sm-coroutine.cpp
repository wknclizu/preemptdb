#include "sm-coroutine.h"

namespace ermia {
namespace coro {

thread_local tcalloc coroutine_allocator; // TODO(pcontext?): leave it as-is for now until we need to support coroutine

} // namespace coro
} // namespace ermia

#include "event_queue.h"

namespace {

Event   g_buf[kEventQueueSize];
uint8_t g_head = 0;   // next slot to write
uint8_t g_tail = 0;   // next slot to read
uint8_t g_count = 0;  // number of unread events; simpler than the
                      // "leave one slot empty" trick and lets us tell
                      // full vs. empty without ambiguity.

inline uint8_t advance(uint8_t i) {
  return (i + 1) % kEventQueueSize;
}

}  // namespace

bool eventPush(const Event &e) {
  const bool was_full = (g_count == kEventQueueSize);
  g_buf[g_head] = e;
  g_head = advance(g_head);
  if (was_full) {
    // Overwrite oldest: tail walks forward too so we keep exactly
    // kEventQueueSize events buffered. Count stays at max.
    g_tail = advance(g_tail);
    return false;
  }
  ++g_count;
  return true;
}

bool eventPop(Event &out) {
  if (g_count == 0) return false;
  out = g_buf[g_tail];
  g_tail = advance(g_tail);
  --g_count;
  return true;
}

void eventQueueClear() {
  g_head = g_tail = g_count = 0;
}

uint8_t eventQueueSize() {
  return g_count;
}

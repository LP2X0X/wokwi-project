// Tiny SPSC ring buffer for Events.
//
// One producer (input polls on the main thread), one consumer (mode FSM tick
// on the same thread). Fully statically allocated, no heap, no mutex needed.
// Overwrite-oldest on full so a misbehaving sensor cannot stall the consumer.
//
// If sensor sampling ever moves onto another core, swap the indices for
// std::atomic<uint8_t> — the API doesn't change.

#pragma once

#include "event.h"

constexpr uint8_t kEventQueueSize = 16;  // power of two not required; head/tail wrap by modulo

// Push an event. Returns true if it landed in a fresh slot, false if it
// overwrote an unread one (queue saturated).
bool eventPush(const Event &e);

// Pop the oldest event. Returns true and fills `out` if there was one,
// false if the queue is empty.
bool eventPop(Event &out);

// Drop everything currently in the queue. Used on state transitions that
// want to ignore events emitted while a previous state was active.
void eventQueueClear();

// Diagnostic — how many events are currently buffered.
uint8_t eventQueueSize();

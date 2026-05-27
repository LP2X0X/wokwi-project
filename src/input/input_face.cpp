#include "input_face.h"

#include <math.h>

#include "../event/event_queue.h"
#include "../mode/world_state.h"

namespace {

// STUB: simulated face presence. Flip to false (or compile out) once a
// real face source is wired in. The stub is per-spec for exercising
// the FSM transitions:
//   * Boot at T+0  : no face
//   * T+8s         : face appears for 4s (Idle -> Interaction)
//   * T+12s        : face leaves    (Interaction -> Idle after timeout)
//   * Repeats every ~25s
constexpr bool     INPUT_FACE_STUB_ENABLED   = true;
constexpr uint32_t STUB_CYCLE_MS             = 25000;
constexpr uint32_t STUB_FACE_APPEARS_AT_MS   =  8000;
constexpr uint32_t STUB_FACE_DISAPPEARS_AT_MS = 12000;

bool g_initialized = false;
bool g_was_present = false;

void stub_eval(uint32_t now_ms) {
  const uint32_t phase = (now_ms - g_world.boot_ms) % STUB_CYCLE_MS;
  const bool present = (phase >= STUB_FACE_APPEARS_AT_MS &&
                        phase <  STUB_FACE_DISAPPEARS_AT_MS);

  // Continuous fields. A small sinusoid so face_x/y move while present
  // — the Interaction state's gaze tracking has something to follow.
  if (present) {
    const float t = (float)(phase - STUB_FACE_APPEARS_AT_MS) / 1000.0f;
    g_world.face_x = 6.0f * sinf(t * 1.5f);
    g_world.face_y = 3.0f * sinf(t * 0.7f);
    g_world.face_conf = 0.9f;
    g_world.face_last_seen_ms = now_ms;
  }

  // Edges -> events + WorldState.face_present + last_interaction_ms.
  if (present && !g_was_present) {
    g_world.face_present = true;
    g_world.last_interaction_ms = now_ms;
    Event ev{};
    ev.type             = EventType::FaceDetected;
    ev.t_ms             = now_ms;
    ev.payload.face.x   = g_world.face_x;
    ev.payload.face.y   = g_world.face_y;
    ev.payload.face.conf= g_world.face_conf;
    eventPush(ev);
  } else if (!present && g_was_present) {
    g_world.face_present = false;
    Event ev{};
    ev.type = EventType::FaceLost;
    ev.t_ms = now_ms;
    eventPush(ev);
  }

  g_was_present = present;
}

}  // namespace

void inputFaceInit(uint32_t /*now_ms*/) {
  g_initialized = true;
  g_was_present = false;
}

void inputFacePoll(uint32_t now_ms) {
  if (!g_initialized) inputFaceInit(now_ms);
  if (INPUT_FACE_STUB_ENABLED) stub_eval(now_ms);
}

// Edge-only event types fed into the Mode FSM.
//
// Sensors emit events ONLY on transitions (face appeared, button pressed, plug
// inserted, ...). Continuous values (face_x, noise_db, battery_pct) live on
// WorldState and are read directly by states. This keeps the event queue
// small and prevents per-frame event spam.
//
// Payloads live in a tagged union so the queue holds a fixed-size POD.

#pragma once

#include <Arduino.h>

enum class EventType : uint8_t {
  None = 0,

  // Face detector edges. Continuous face_x/y live on WorldState.
  FaceDetected,
  FaceLost,

  // Microphone edges. Continuous noise_floor_db lives on WorldState.
  NoiseLoud,
  NoiseQuiet,
  FingerSnap,

  // Physical button.
  ButtonPress,
  ButtonHold,

  // Power.
  BatteryPlugged,
  BatteryUnplugged,
  BatteryLow,

  Count_,  // sentinel for static asserts; never emitted.
};

struct Event {
  EventType type;
  uint32_t  t_ms;
  union {
    struct { float x, y, conf;     } face;
    struct { float db;             } noise;
    struct { uint8_t pct;          } battery;
    uint32_t raw;                       // catch-all so the union has a default member
  } payload;
};

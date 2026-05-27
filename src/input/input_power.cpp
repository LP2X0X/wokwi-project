#include "input_power.h"

#include "../mode/world_state.h"

void inputPowerInit(uint32_t /*now_ms*/) {
  // Wokwi default: pretend the toy is plugged in with a full battery.
  g_world.battery_pct     = 100;
  g_world.battery_plugged = true;
}

void inputPowerPoll(uint32_t /*now_ms*/) {
  // No-op until real ADC + charge-detect wiring lands.
}

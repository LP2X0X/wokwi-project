#include "world_state.h"

#include "mode.h"  // ModeId definition

WorldState g_world{};

void worldStateInit(uint32_t now_ms) {
  g_world = WorldState{};
  g_world.boot_ms             = now_ms;
  g_world.last_interaction_ms = now_ms;
  g_world.mode_entered_ms     = now_ms;
  g_world.battery_pct         = 100;
  g_world.battery_plugged     = true;
  g_world.current             = ModeId::Boot;
  g_world.previous            = ModeId::Boot;
}

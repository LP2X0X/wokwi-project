#include "display_power.h"

#include <TFT_eSPI.h>

namespace {

constexpr uint8_t kMaxCs = 4;

TFT_eSPI *g_tft           = nullptr;
int       g_cs_pins[kMaxCs]{ -1, -1, -1, -1 };
uint8_t   g_n_cs          = 0;
bool      g_active        = true;

void assertAllCsLow() {
  for (uint8_t i = 0; i < g_n_cs; ++i) {
    if (g_cs_pins[i] >= 0) digitalWrite(g_cs_pins[i], LOW);
  }
}

void deassertAllCs() {
  for (uint8_t i = 0; i < g_n_cs; ++i) {
    if (g_cs_pins[i] >= 0) digitalWrite(g_cs_pins[i], HIGH);
  }
}

}  // namespace

void displayPowerInit(TFT_eSPI *tft, const int *cs_pins, uint8_t n_cs) {
  g_tft  = tft;
  g_n_cs = (n_cs > kMaxCs) ? kMaxCs : n_cs;
  for (uint8_t i = 0; i < g_n_cs; ++i) g_cs_pins[i] = cs_pins[i];
  g_active = true;
}

void display_set_active(bool active) {
  if (!g_tft || g_active == active) return;
  g_active = active;

  // Drive every panel with the same command stream by holding all CS
  // lines LOW together (same trick setup() uses for init). Identical
  // hardware -> identical state transitions.
  assertAllCsLow();

  if (!active) {
    // Display off first (stops drawing), then sleep in (panel logic
    // enters low-power mode). Order matters per ILI9341 / ST7789 /
    // GC9A01 datasheets.
    g_tft->writecommand(0x28);          // TFT_DISPOFF / DISPLAY_OFF
    g_tft->writecommand(0x10);          // TFT_SLPIN   / ENTER_SLEEP_MODE
  } else {
    // Wake from sleep, then re-enable display. Datasheet requires
    // ~120 ms between SLPOUT and the next command on most controllers.
    g_tft->writecommand(0x11);          // TFT_SLPOUT  / EXIT_SLEEP_MODE
    delay(120);
    g_tft->writecommand(0x29);          // TFT_DISPON  / DISPLAY_ON
  }

  deassertAllCs();
}

bool display_is_active() { return g_active; }

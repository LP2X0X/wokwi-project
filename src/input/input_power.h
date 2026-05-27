// Battery / charge-state input adapter (stub).
//
// Real implementation: ADC-read the battery, detect plug-in via either a
// GPIO from the charge IC's STAT pin or a sudden voltage step.
//
// Edge events:
//   * BatteryPlugged   — toy was just plugged in.
//   * BatteryUnplugged — running on battery now.
//   * BatteryLow       — pct dropped below the alarm threshold.

#pragma once

#include <Arduino.h>

void inputPowerInit(uint32_t now_ms);
void inputPowerPoll(uint32_t now_ms);

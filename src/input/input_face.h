// Face detector input adapter.
//
// Real hardware: replace inputFacePoll() with whatever your face tracker
// (Person Sensor, dedicated MCU over I2C, ESP32-CAM with TFLite, ...)
// produces. The contract is unchanged:
//
//   * Update continuous fields on g_world (face_present, face_x/y,
//     face_conf, face_last_seen_ms).
//   * Push FaceDetected / FaceLost edge events when face_present
//     transitions.
//   * Bump g_world.last_interaction_ms whenever face_present becomes
//     true. This is the single signal "the user is here right now."
//
// The stub below scripts a face appearing every ~6 s for 4 s so the
// Idle <-> Interaction transition is visible in Wokwi without real
// hardware. Set INPUT_FACE_STUB_ENABLED = false in the .cpp to silence it.

#pragma once

#include <Arduino.h>

void inputFaceInit(uint32_t now_ms);
void inputFacePoll(uint32_t now_ms);

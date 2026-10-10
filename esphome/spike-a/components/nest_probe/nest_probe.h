#pragma once

#include "esphome/core/component.h"

namespace esphome::nest_probe {

/// Builds a stress scene comparable to the real ESP Nest UI (full-width
/// glow fading in and out, a 456 px ring with a sweeping arc, a 140 px
/// glyph label) using the raw LVGL API, and logs refresh statistics.
class NestProbe : public Component {
 public:
  void setup() override;
  void loop() override;
  // After the LVGL component (PROCESSOR) has initialised LVGL and the display.
  float get_setup_priority() const override { return setup_priority::PROCESSOR - 30.0f; }
};

}  // namespace esphome::nest_probe

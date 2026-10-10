#pragma once

#include "esphome/core/component.h"

namespace esphome::nest_display {

/// Owns the ST7701S RGB panel, LVGL and the MT8901 encoder. Everything LVGL
/// runs in a dedicated FreeRTOS task pinned to core 1; ESPHome's loop never
/// touches LVGL directly.
class NestDisplay : public Component {
 public:
  void set_hw_mirror_x(bool v) { this->hw_mirror_x_ = v; }
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

 protected:
  static void ui_task(void *arg);
  bool hw_mirror_x_{true};
};

}  // namespace esphome::nest_display

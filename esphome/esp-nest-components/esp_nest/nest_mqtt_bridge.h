#pragma once

#include <cstdint>
#include <string>

#include "esphome/components/mqtt/mqtt_client.h"

#include "blinds_sync.h"
#include "fan_sync.h"
#include "light_sync.h"
#include "nest_ui.h"

namespace esphome::esp_nest {

/*
 * The external-device MQTT glue (architecture section 4): zigbee2mqtt lights,
 * the fan, the stereo volume pulses and the ESPHome RF blinds bridge, over
 * ESPHome's `mqtt:` client, configured silent (no topic prefix, no
 * discovery). Topics, payloads, QoS, retain flags and timings are those of
 * the native firmware (main/ha_mqtt.c, v2.0.1).
 *
 * The bridge is glue only: payload parsing and formatting are mqtt_payload,
 * and the synchronisation rules are light_sync, fan_sync and blinds_sync.
 * It keeps no sync state of its own beyond the three module states and the
 * connection flag, and it never decides anything those modules decide.
 *
 * Threading: loopTask only. MQTT callbacks are dispatched there, the UI's
 * events are drained there, and every publish asserts it
 * (NEST_ASSERT_LOOP_TASK). Values for the UI go through the ui_set_*()
 * atomic pending slots, which nest_ui applies without a change
 * notification (the anti-loop rule).
 *
 * Time: every module call gets the same clock, esphome::millis_64() as
 * int64_t. It never wraps; the uint32 millis() would after 49.7 days.
 */

/// Where the external devices live: the Kconfig TOPIC_* options of the
/// native firmware, here from the `esp_nest:` YAML block.
struct MqttBridgeConfig {
  /// zigbee2mqtt light group: `<lights>` (state), `<lights>/set`, `<lights>/get`.
  std::string lights;
  /// Fan state, retained text Off / low / medium / high.
  std::string fan;
  /// Stereo volume base: `<stereo_volume>/set` takes UP / DOWN pulses.
  std::string stereo_volume;
  /// Blinds bridge prefix: `<prefix>/<group>_{1..3}/position/{state,command}`
  /// and `<prefix>/<group>_group/command`.
  std::string blinds_prefix;
  /// Blinds group ids, in UI order (0 = group A, 1 = group B).
  std::string blinds_groups[BLINDS_SYNC_GROUPS];
};

class MqttBridge {
 public:
  /// Stores the topics. Before setup().
  void configure(mqtt::MQTTClientComponent *client, MqttBridgeConfig config);

  /// Builds the topic strings, initialises the three sync modules and
  /// registers the eight QoS 1 subscriptions (ESPHome subscribes once the
  /// client is connected and re-subscribes on every reconnect).
  void setup();

  /// Every loop pass: connect-edge handling, the light and blinds ticks.
  void tick();

  /// A local change drained from the UI queue.
  void on_ui_event(ui_event_type_t type, int value);

  /// The current MQTT connection state as the bridge last saw it.
  bool connected() const { return this->connected_; }

  void dump_config();

 protected:
  /// One clock for every module call.
  static int64_t now_ms_();

  /// Detects a connect or disconnect edge and returns the connection state.
  /// Runs the native connect sequence on the false -> true edge:
  /// light_sync_init, a `/get` request, fan_sync_on_connect. Called at the
  /// start of every entry point, so the sequence always precedes the first
  /// message or event of a connection, whichever component loop runs first.
  /// Blinds state is deliberately not reset (native).
  bool sync_connection_();

  /// Publish helper: asserts loopTask, never publishes while the client
  /// reports itself disconnected, logs a failure.
  bool publish_(const std::string &topic, const char *payload, size_t length, bool retain);

  void publish_light_set_(int pct);
  void publish_light_get_();
  void publish_blinds_command_(const blinds_sync_cmd_t &cmd);

  void on_lights_(const std::string &payload);
  void on_fan_(const std::string &payload);
  void on_blinds_member_(int target, int member, const std::string &payload);

  mqtt::MQTTClientComponent *client_{nullptr};
  MqttBridgeConfig config_;

  // Topics derived from the configuration (setup()).
  std::string lights_set_;
  std::string lights_get_;
  std::string stereo_volume_set_;
  std::string blinds_state_[BLINDS_SYNC_GROUPS][BLINDS_SYNC_MEMBERS];
  std::string blinds_position_command_[BLINDS_SYNC_GROUPS][BLINDS_SYNC_MEMBERS];
  std::string blinds_group_command_[BLINDS_SYNC_GROUPS];

  light_sync_t light_sync_;
  fan_sync_t fan_sync_;
  blinds_sync_t blinds_sync_;

  bool connected_{false};
};

}  // namespace esphome::esp_nest

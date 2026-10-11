#include "nest_mqtt_bridge.h"

#include <cstring>
#include <utility>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "mqtt_payload.h"
#include "nest_core.h"

namespace esphome::esp_nest {

static const char *const TAG = "esp_nest.mqtt";

/// QoS of every subscription (native: esp_mqtt_client_subscribe(..., 1)).
static constexpr uint8_t SUBSCRIBE_QOS = 1;
/// QoS of every publish (native: esp_mqtt_client_publish(..., 0, ...)).
static constexpr uint8_t PUBLISH_QOS = 0;

int64_t MqttBridge::now_ms_() { return static_cast<int64_t>(millis_64()); }

void MqttBridge::configure(mqtt::MQTTClientComponent *client, MqttBridgeConfig config) {
  this->client_ = client;
  this->config_ = std::move(config);
}

void MqttBridge::setup() {
  NEST_ASSERT_LOOP_TASK();

  const MqttBridgeConfig &cfg = this->config_;
  this->lights_set_ = cfg.lights + "/set";
  this->lights_get_ = cfg.lights + "/get";
  this->stereo_volume_set_ = cfg.stereo_volume + "/set";
  for (int target = 0; target < BLINDS_SYNC_GROUPS; target++) {
    const std::string group = cfg.blinds_prefix + "/" + cfg.blinds_groups[target];
    this->blinds_group_command_[target] = group + "_group/command";
    for (int member = 0; member < BLINDS_SYNC_MEMBERS; member++) {
      const std::string cover = group + "_" + std::to_string(member + 1);
      this->blinds_state_[target][member] = cover + "/position/state";
      this->blinds_position_command_[target][member] = cover + "/position/command";
    }
  }

  light_sync_init(&this->light_sync_);
  fan_sync_init(&this->fan_sync_);
  blinds_sync_init(&this->blinds_sync_);

  // Exact topics, no wildcards, so every message reaches exactly one
  // handler (the config validation rejects duplicate topics).
  this->client_->subscribe(
      cfg.fan, [this](const std::string &, const std::string &payload) { this->on_fan_(payload); }, SUBSCRIBE_QOS);
  this->client_->subscribe(
      cfg.lights, [this](const std::string &, const std::string &payload) { this->on_lights_(payload); },
      SUBSCRIBE_QOS);
  for (int target = 0; target < BLINDS_SYNC_GROUPS; target++) {
    for (int member = 0; member < BLINDS_SYNC_MEMBERS; member++) {
      this->client_->subscribe(
          this->blinds_state_[target][member],
          [this, target, member](const std::string &, const std::string &payload) {
            this->on_blinds_member_(target, member, payload);
          },
          SUBSCRIBE_QOS);
    }
  }
}

void MqttBridge::dump_config() {
  ESP_LOGCONFIG(TAG,
                "MQTT bridge (external devices):\n"
                "  Lights: %s (/set, /get)\n"
                "  Fan: %s\n"
                "  Stereo volume: %s (/set)\n"
                "  Blinds: %s/<group>_{1..3}/position/{state,command}, %s/<group>_group/command\n"
                "  Blinds groups: '%s' / '%s'",
                this->config_.lights.c_str(), this->config_.fan.c_str(), this->config_.stereo_volume.c_str(),
                this->config_.blinds_prefix.c_str(), this->config_.blinds_prefix.c_str(),
                this->config_.blinds_groups[0].c_str(), this->config_.blinds_groups[1].c_str());
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

// Connect edges are detected by polling at every loop pass and callback entry.
// A disconnect plus reconnect inside a single pass would be missed, but
// ESPHome's reconnect takes seconds and the loop runs at least every 16 ms.
bool MqttBridge::sync_connection_() {
  const bool connected = this->client_->is_connected();
  if (connected == this->connected_)
    return connected;
  this->connected_ = connected;

  if (!connected) {
    ESP_LOGW(TAG, "MQTT disconnected");
    return false;
  }

  // Native order (ha_mqtt.c MQTT_EVENT_CONNECTED): the light sync restarts,
  // the current light state is requested, the fan forgets what it published
  // so the retained value delivered on subscribe wins. ESPHome has already
  // re-sent the subscriptions by now. Blinds state is NOT reset.
  ESP_LOGI(TAG, "MQTT connected");
  light_sync_init(&this->light_sync_);
  this->publish_light_get_();
  fan_sync_on_connect(&this->fan_sync_);
  return true;
}

bool MqttBridge::publish_(const std::string &topic, const char *payload, size_t length, bool retain) {
  NEST_ASSERT_LOOP_TASK(false);
  if (this->client_->publish(topic.c_str(), payload, length, PUBLISH_QOS, retain))
    return true;
  ESP_LOGW(TAG, "Publish to %s failed", topic.c_str());
  return false;
}

// ---------------------------------------------------------------------------
// Outbound
// ---------------------------------------------------------------------------

void MqttBridge::publish_light_set_(int pct) {
  char payload[MQTT_PAYLOAD_OUT_SIZE];
  const int length = mqtt_payload_light_set(pct, payload, sizeof payload);
  if (length < 0 || static_cast<size_t>(length) >= sizeof payload) {
    ESP_LOGE(TAG, "Light payload for %d%% does not fit its buffer", pct);
    return;
  }
  this->publish_(this->lights_set_, payload, static_cast<size_t>(length), false);
  ESP_LOGI(TAG, "Light publish: %s", payload);
}

void MqttBridge::publish_light_get_() {
  const char *payload = mqtt_payload_light_get();
  this->publish_(this->lights_get_, payload, strlen(payload), false);
}

// A command whose publish fails (client dropped since sync_connection_(), or
// esp-mqtt's outbound pool exhausted) is dropped, not re-queued: native
// ha_mqtt.c ignores the publish result the same way (deliberate parity).
void MqttBridge::publish_blinds_command_(const blinds_sync_cmd_t &cmd) {
  if (cmd.target < 0 || cmd.target >= BLINDS_SYNC_GROUPS)
    return;
  char payload[MQTT_PAYLOAD_OUT_SIZE];
  const int length = mqtt_payload_blinds_command(&cmd, payload, sizeof payload);
  if (length < 0 || static_cast<size_t>(length) >= sizeof payload) {
    ESP_LOGE(TAG, "Blinds command (target %d, kind %d) has no payload", cmd.target, static_cast<int>(cmd.kind));
    return;
  }
  const size_t size = static_cast<size_t>(length);

  switch (cmd.kind) {
    case BLINDS_SYNC_CMD_OPEN:
    case BLINDS_SYNC_CMD_CLOSE:
      // Endpoints: one broadcast RF burst moves all three motors and the
      // end-stops re-home the dead reckoning.
      this->publish_(this->blinds_group_command_[cmd.target], payload, size, false);
      ESP_LOGI(TAG, "Blinds publish: %s = %s", this->blinds_group_command_[cmd.target].c_str(), payload);
      break;
    case BLINDS_SYNC_CMD_POSITION:
      // One command stands for the three members: the same position to each.
      for (int member = 0; member < BLINDS_SYNC_MEMBERS; member++)
        this->publish_(this->blinds_position_command_[cmd.target][member], payload, size, false);
      ESP_LOGI(TAG, "Blinds publish: %s x3 = %s", this->config_.blinds_groups[cmd.target].c_str(), payload);
      break;
    default:
      break;
  }
}

void MqttBridge::on_ui_event(ui_event_type_t type, int value) {
  NEST_ASSERT_LOOP_TASK();
  const int64_t now_ms = now_ms_();
  const bool connected = this->sync_connection_();

  switch (type) {
    case UI_EVT_BRIGHTNESS:
      // Dropped while disconnected, light_sync untouched (native).
      if (!connected)
        break;
      if (light_sync_on_local_change(&this->light_sync_, now_ms, value) == LIGHT_SYNC_ACT_PUBLISH)
        this->publish_light_set_(value);
      break;

    case UI_EVT_FAN: {
      if (!connected)
        break;
      // The module decides (last published level, range) and records the
      // publish itself; the bridge keeps no fan state.
      if (!fan_sync_on_local(&this->fan_sync_, now_ms, value))
        break;
      const char *label = mqtt_payload_fan_label(value);
      if (label == nullptr)
        break;
      // Retained, like native.
      this->publish_(this->config_.fan, label, strlen(label), true);
      ESP_LOGI(TAG, "Fan publish: %s", label);
      break;
    }

    case UI_EVT_VOLUME: {
      if (!connected)
        break;
      // Stateless relative control: the stereo is driven over IR by an HA
      // automation bridging this topic to NEC codes.
      const char *payload = mqtt_payload_volume(value);
      if (payload == nullptr)
        break;
      this->publish_(this->stereo_volume_set_, payload, strlen(payload), false);
      ESP_LOGI(TAG, "Volume publish: %s", payload);
      break;
    }

    case UI_EVT_BLINDS: {
      // Queued regardless of the connection: the flush waits for it.
      const int target = value >> 8;
      const int step = value & 0xFF;
      if (!blinds_sync_on_local(&this->blinds_sync_, now_ms, target, step))
        ESP_LOGW(TAG, "Blinds step %d for target %d is out of range, ignored", step, target);
      break;
    }

    default:
      ESP_LOGW(TAG, "UI event of unknown type %d ignored", static_cast<int>(type));
      break;
  }
}

void MqttBridge::tick() {
  NEST_ASSERT_LOOP_TASK();
  const int64_t now_ms = now_ms_();
  const bool connected = this->sync_connection_();

  // Lights only while connected, like native ha_mqtt_process_pending.
  if (connected) {
    int pct = 0;
    switch (light_sync_tick(&this->light_sync_, now_ms, &pct)) {
      case LIGHT_SYNC_ACT_PUBLISH:
        this->publish_light_set_(pct);
        break;
      case LIGHT_SYNC_ACT_RESYNC:
        ESP_LOGD(TAG, "Authority window closed, requesting light state");
        this->publish_light_get_();
        break;
      default:
        break;
    }
  }

  // The blinds module gates on the connection itself: while disconnected
  // it returns 0 and keeps the queued steps.
  blinds_sync_cmd_t commands[BLINDS_SYNC_GROUPS];
  const int count = blinds_sync_tick(&this->blinds_sync_, now_ms, connected, commands);
  for (int i = 0; i < count; i++)
    this->publish_blinds_command_(commands[i]);
}

// ---------------------------------------------------------------------------
// Inbound
// ---------------------------------------------------------------------------

void MqttBridge::on_lights_(const std::string &payload) {
  NEST_ASSERT_LOOP_TASK();
  const int64_t now_ms = now_ms_();
  this->sync_connection_();

  mqtt_light_state_t state;
  mqtt_payload_parse_light(payload.data(), payload.size(), &state);
  const int pct = light_sync_on_state_msg(&this->light_sync_, now_ms, state.have_state, state.state_on,
                                          state.have_brightness, state.brightness, ui_get_brightness());
  if (pct < 0) {
    ESP_LOGD(TAG, "Light msg ignored (raw br=%d state=%s)", state.brightness, state.state);
    return;
  }
  ui_set_brightness(pct);
  ESP_LOGI(TAG, "Light from MQTT: %d%% (raw br=%d state=%s)", pct, state.brightness, state.state);
}

void MqttBridge::on_fan_(const std::string &payload) {
  NEST_ASSERT_LOOP_TASK();
  const int64_t now_ms = now_ms_();
  this->sync_connection_();

  // -1 (unrecognised payload) goes to the module unchanged: it maps it to
  // the current level. It also records an applied level as last published,
  // so the bridge never touches that.
  const int parsed = mqtt_payload_parse_fan(payload.data(), payload.size());
  const int level = fan_sync_on_message(&this->fan_sync_, now_ms, parsed, ui_get_fan_level());
  if (level < 0) {
    ESP_LOGD(TAG, "Fan msg ignored (level %d)", parsed);
    return;
  }
  ui_set_fan_level(level);
  ESP_LOGI(TAG, "Fan state from MQTT: level %d", level);
}

void MqttBridge::on_blinds_member_(int target, int member, const std::string &payload) {
  NEST_ASSERT_LOOP_TASK();
  const int64_t now_ms = now_ms_();
  this->sync_connection_();

  int position;
  if (!mqtt_payload_parse_int(payload.data(), payload.size(), &position))
    return;  // not a number: dropped before blinds_sync sees it (native)
  int average = 0;
  const int step = blinds_sync_on_member(&this->blinds_sync_, now_ms, target, member, position, &average);
  if (step < 0) {
    ESP_LOGD(TAG, "Blinds msg ignored (authority window or no member known)");
    return;
  }
  ui_set_blinds_pos(target, step);
  ESP_LOGI(TAG, "Blinds %s position from MQTT: avg %d%% (step %d)", this->config_.blinds_groups[target].c_str(),
           average, step);
}

}  // namespace esphome::esp_nest

#include "mt8901.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"

static const char *TAG = "mt8901";

static pcnt_unit_handle_t s_pcnt_unit = NULL;

void mt8901_init(int16_t sig_pin, int16_t dir_pin)
{
    /* Create PCNT unit */
    pcnt_unit_config_t unit_config = {
        .high_limit = INT16_MAX,
        .low_limit  = INT16_MIN,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &s_pcnt_unit));

    /* Set glitch filter (1000 ns) */
    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = 1000,
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt_unit, &filter_config));

    /* Create PCNT channel */
    pcnt_chan_config_t chan_config = {
        .edge_gpio_num  = sig_pin,
        .level_gpio_num = dir_pin,
    };
    pcnt_channel_handle_t pcnt_chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt_unit, &chan_config, &pcnt_chan));

    /* Configure counting edges:
     *   - Positive edge: decrement
     *   - Negative edge: increment
     *   - Level high on dir_pin: keep direction
     *   - Level low on dir_pin:  reverse direction
     */
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan,
                        PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                        PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(pcnt_chan,
                        PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                        PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    /* Enable and start */
    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt_unit));

    ESP_LOGI(TAG, "MT8901 encoder initialized (sig=%d, dir=%d)", sig_pin, dir_pin);
}

int mt8901_get_count(void)
{
    int count = 0;
    if (s_pcnt_unit) {
        pcnt_unit_get_count(s_pcnt_unit, &count);
    }
    return count;
}

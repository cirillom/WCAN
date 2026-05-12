#pragma once

#include <cstdint>

#include "esp_log.h"

#ifdef MEASURE_INSTR
#include "esp_timer.h"
#endif

#include "app_config.hpp"
#include "wcan.hpp"

void wcan_recv_callback(const data_packet_t &recv_packet)
{
    static const char *TAG = "wcan_recv_callback";
    if (recv_packet.data_count == 0) {
        return;
    }

#ifdef MEASURE_INSTR
    static bool s_first_rx_logged = false;
    if (!s_first_rx_logged) {
        s_first_rx_logged = true;
        ESP_LOGI("FIRST_RX_TS", "us=%lld id=0x%lx", esp_timer_get_time(),
                 static_cast<unsigned long>(recv_packet.can_id));
    }
#endif

    ESP_LOGI(TAG, "[%lx] tick=%lu [%lu..%lu] %u items", static_cast<unsigned long>(recv_packet.can_id),
             static_cast<unsigned long>(recv_packet.tick_count), static_cast<unsigned long>(recv_packet.data[0]),
             static_cast<unsigned long>(recv_packet.data[recv_packet.data_count - 1]), recv_packet.data_count);
}

namespace receiver_app {

inline void SetupReceiver()
{
    static const char *TAG = "RECEIVER_APP";

    if (app_config::ReceiverFilterEnabled()) {
        const size_t filter_count = app_config::ReceiverFilterCount();
        ESP_LOGI(TAG, "RECEIVER mode - active filter with %u CAN ID(s)", static_cast<unsigned>(filter_count));
        for (size_t i = 0; i < filter_count; ++i) {
            ESP_LOGI(TAG, "Receiver filter[%u] = 0x%lx", static_cast<unsigned>(i),
                     static_cast<unsigned long>(app_config::ReceiverFilterIds()[i]));
        }
        wcan_init(true, app_config::ReceiverFilterIds(), filter_count, nullptr, 0, 0);
        return;
    }

    ESP_LOGI(TAG, "RECEIVER mode - accepting all CAN IDs");
    wcan_init(false, nullptr, 0, nullptr, 0, 0);
}

} // namespace receiver_app

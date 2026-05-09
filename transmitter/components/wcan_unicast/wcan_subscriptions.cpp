#include "wcan_subscriptions.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "SUBS";

static subscriber_entry_t s_subscribers[WCAN_MAX_SUBSCRIBERS] = {};
static SemaphoreHandle_t s_subs_mutex = nullptr;

void subscription_init(void)
{
    if (s_subs_mutex != nullptr) {
        return;
    }
    s_subs_mutex = xSemaphoreCreateMutex();
    if (s_subs_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create subscription mutex");
    }
}

static int find_slot_locked(const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    for (size_t i = 0; i < WCAN_MAX_SUBSCRIBERS; i++) {
        if (s_subscribers[i].in_use &&
            std::memcmp(s_subscribers[i].mac_addr, mac, ESP_NOW_ETH_ALEN) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static int allocate_slot_locked(void)
{
    for (size_t i = 0; i < WCAN_MAX_SUBSCRIBERS; i++) {
        if (!s_subscribers[i].in_use) {
            return static_cast<int>(i);
        }
    }
    size_t oldest = 0;
    for (size_t i = 1; i < WCAN_MAX_SUBSCRIBERS; i++) {
        if (s_subscribers[i].last_seen_tick < s_subscribers[oldest].last_seen_tick) {
            oldest = i;
        }
    }
    ESP_LOGW(TAG, "table full; evicting slot %u", static_cast<unsigned>(oldest));
    return static_cast<int>(oldest);
}

void subscription_update(const uint8_t mac[ESP_NOW_ETH_ALEN], const uint32_t *ids, size_t n)
{
    if (s_subs_mutex == nullptr) {
        return;
    }
    if (xSemaphoreTake(s_subs_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "update mutex timeout");
        return;
    }

    int idx = find_slot_locked(mac);
    if (idx < 0) {
        idx = allocate_slot_locked();
        std::memset(&s_subscribers[idx], 0, sizeof(subscriber_entry_t));
        std::memcpy(s_subscribers[idx].mac_addr, mac, ESP_NOW_ETH_ALEN);
        s_subscribers[idx].in_use = true;
        ESP_LOGI(TAG, "new subscriber %02x:%02x:%02x:%02x:%02x:%02x in slot %d",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], idx);
    }

    auto &e = s_subscribers[idx];
    e.last_seen_tick = xTaskGetTickCount();
    e.consecutive_tx_failures = 0;

    if (n == 0) {
        e.wildcard = true;
        e.num_ids = 0;
    } else {
        e.wildcard = false;
        size_t to_copy = (n > WCAN_MAX_IDS_PER_SUBSCRIBER) ? WCAN_MAX_IDS_PER_SUBSCRIBER : n;
        if (n > WCAN_MAX_IDS_PER_SUBSCRIBER) {
            ESP_LOGW(TAG, "subscriber announced %u ids; truncating to %u",
                     static_cast<unsigned>(n), static_cast<unsigned>(WCAN_MAX_IDS_PER_SUBSCRIBER));
        }
        e.num_ids = static_cast<uint8_t>(to_copy);
        std::memcpy(e.subscribed_ids, ids, to_copy * sizeof(uint32_t));
    }

    xSemaphoreGive(s_subs_mutex);
}

void subscription_log_state(void)
{
    if (s_subs_mutex == nullptr) {
        return;
    }
    if (xSemaphoreTake(s_subs_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "log mutex timeout");
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    const TickType_t liveness = pdMS_TO_TICKS(WCAN_SUBSCRIBER_LIVENESS_MS);

    size_t alive = 0;
    for (size_t i = 0; i < WCAN_MAX_SUBSCRIBERS; i++) {
        if (s_subscribers[i].in_use && (now - s_subscribers[i].last_seen_tick) < liveness) {
            alive++;
        }
    }
    ESP_LOGI(TAG, "%u alive subscribers", static_cast<unsigned>(alive));

    for (size_t i = 0; i < WCAN_MAX_SUBSCRIBERS; i++) {
        const auto &e = s_subscribers[i];
        if (!e.in_use) {
            continue;
        }

        const uint32_t age_ms = static_cast<uint32_t>(pdTICKS_TO_MS(now - e.last_seen_tick));
        const bool is_alive = (now - e.last_seen_tick) < liveness;

        char ids_buf[WCAN_MAX_IDS_PER_SUBSCRIBER * 12 + 4] = "[";
        size_t off = 1;
        if (e.wildcard) {
            off += std::snprintf(ids_buf + off, sizeof(ids_buf) - off, "*");
        } else {
            for (size_t k = 0; k < e.num_ids && off < sizeof(ids_buf) - 14; k++) {
                off += std::snprintf(ids_buf + off, sizeof(ids_buf) - off, "%s0x%lx",
                                     (k > 0 ? "," : ""),
                                     static_cast<unsigned long>(e.subscribed_ids[k]));
            }
        }
        if (off < sizeof(ids_buf) - 1) {
            ids_buf[off] = ']';
            ids_buf[off + 1] = '\0';
        }

        ESP_LOGI(TAG, "  [%u] %02x:%02x:%02x:%02x:%02x:%02x age=%lums %s ids=%s fails=%u",
                 static_cast<unsigned>(i),
                 e.mac_addr[0], e.mac_addr[1], e.mac_addr[2], e.mac_addr[3],
                 e.mac_addr[4], e.mac_addr[5],
                 static_cast<unsigned long>(age_ms),
                 is_alive ? "ALIVE" : "STALE",
                 ids_buf,
                 e.consecutive_tx_failures);
    }

    xSemaphoreGive(s_subs_mutex);
}

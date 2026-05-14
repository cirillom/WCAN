#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef MEASURE_INSTR
#include "esp_timer.h"
#endif

#include "wcan.hpp"
#include "wcan_sender.hpp"
#include "wcan_subscriptions.hpp"
#include "wcan_utils.hpp"

const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
QueueHandle_t send_queue = nullptr;
QueueHandle_t *can_queues = nullptr;

SemaphoreHandle_t espnow_tx_sem = nullptr;

#ifdef MEASURE_INSTR
// Approximate radio rate. ESP-NOW uses 802.11 base rates; 6 Mbps is a
// conservative midrange choice that ignores preamble + 802.11 ACK air time.
// Bias is constant across variants — fine for relative comparison.
#define WCAN_PHY_RATE_MBPS 6
volatile uint64_t g_airtime_total_us = 0;
volatile uint64_t g_packets_sent_total = 0;

// HW-ACK latency stamps. The TX semaphore enforces single-in-flight, so
// these are race-free as long as the cb fires before the next esp_now_send.
volatile int64_t g_in_flight_send_us = 0;
volatile uint8_t g_in_flight_peer_mac[ESP_NOW_ETH_ALEN] = {};
#endif

#define WCAN_MULTICAST_ZERO_SUCCESS_FALLBACK_THRESHOLD 3

struct tx_request_t {
    std::unique_ptr<esp_now_packet_t> packet;
    SemaphoreHandle_t completion_sem;
    bool *completion_success;
    bool multicast;
    size_t target_count;
    uint8_t targets[WCAN_MAX_SUBSCRIBERS][ESP_NOW_ETH_ALEN];
};

static delivery_mode_t *s_delivery_modes = nullptr;
static uint8_t *s_zero_success_multicast_batches = nullptr;
static SemaphoreHandle_t s_delivery_state_mutex = nullptr;

SemaphoreHandle_t espnow_tx_status_sem = nullptr;

static volatile bool s_in_flight_active = false;
static volatile bool s_in_flight_multicast = false;
static volatile bool s_in_flight_success = false;
static volatile size_t s_in_flight_callbacks = 0;
static volatile size_t s_in_flight_expected_callbacks = 0;
static volatile TickType_t s_in_flight_first_success_tick = 0;
static uint8_t s_in_flight_targets[WCAN_MAX_SUBSCRIBERS][ESP_NOW_ETH_ALEN] = {};
static size_t s_in_flight_target_count = 0;

static std::unique_ptr<data_packet_t> collect_packet(size_t can_queue_index, uint32_t can_id)
{
    uint32_t data_point[WCAN_DATA_PACKET_MAX_DATA_COUNT];

    if (xQueueReceive(can_queues[can_queue_index], &data_point[0], portMAX_DELAY) != pdTRUE) {
        return nullptr;
    }

    size_t count = 1;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(linger_ms);
    while (count < WCAN_DATA_PACKET_MAX_DATA_COUNT) {
        const TickType_t now = xTaskGetTickCount();
        // avoids overflow issues with tick count wraparound
        if (static_cast<int32_t>(deadline - now) <= 0) {
            break;
        }
        const TickType_t remaining = deadline - now;
        if (xQueueReceive(can_queues[can_queue_index], &data_point[count], remaining) != pdTRUE) {
            break;
        }
        count++;
    }

    auto packet = std::make_unique<data_packet_t>();
    std::memcpy(packet->mac_addr.data(), own_mac_addr, ESP_NOW_ETH_ALEN);
    packet->can_id = can_id;
    packet->data_count = static_cast<uint8_t>(count);
    packet->tick_count = xTaskGetTickCount();
    packet->data = std::make_unique<uint32_t[]>(count);
    if (!packet->data) {
        return nullptr;
    }
    std::memcpy(packet->data.get(), data_point, count * sizeof(uint32_t));
    return packet;
}

static int get_queue_index_for_can_id(uint32_t can_id)
{
    const size_t queue_index = get_can_tx_queue_index(can_id);
    if (queue_index == SIZE_MAX || queue_index >= num_can_queues) {
        return -1;
    }
    return static_cast<int>(queue_index);
}

static delivery_mode_t get_delivery_mode(size_t can_queue_index)
{
    if (s_delivery_modes == nullptr || s_delivery_state_mutex == nullptr || can_queue_index >= num_can_queues) {
        return delivery_mode_t::BROADCAST_DISCOVERY;
    }

    delivery_mode_t mode = delivery_mode_t::BROADCAST_DISCOVERY;
    if (xSemaphoreTake(s_delivery_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        mode = s_delivery_modes[can_queue_index];
        xSemaphoreGive(s_delivery_state_mutex);
    }
    return mode;
}

static void mark_discovery_mode(size_t can_queue_index)
{
    if (s_delivery_modes == nullptr || s_zero_success_multicast_batches == nullptr ||
        s_delivery_state_mutex == nullptr || can_queue_index >= num_can_queues) {
        return;
    }

    if (xSemaphoreTake(s_delivery_state_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    s_delivery_modes[can_queue_index] = delivery_mode_t::BROADCAST_DISCOVERY;
    s_zero_success_multicast_batches[can_queue_index] = 0;
    xSemaphoreGive(s_delivery_state_mutex);
}

static void record_multicast_batch_result(size_t can_queue_index, bool had_success)
{
    if (s_delivery_modes == nullptr || s_zero_success_multicast_batches == nullptr ||
        s_delivery_state_mutex == nullptr || can_queue_index >= num_can_queues) {
        return;
    }

    if (xSemaphoreTake(s_delivery_state_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    if (had_success) {
        s_delivery_modes[can_queue_index] = delivery_mode_t::MULTICAST_ACTIVE;
        s_zero_success_multicast_batches[can_queue_index] = 0;
    } else {
        uint8_t &failures = s_zero_success_multicast_batches[can_queue_index];
        if (failures < UINT8_MAX) {
            failures++;
        }
        if (failures >= WCAN_MULTICAST_ZERO_SUCCESS_FALLBACK_THRESHOLD) {
            s_delivery_modes[can_queue_index] = delivery_mode_t::BROADCAST_DISCOVERY;
            failures = 0;
        }
    }

    xSemaphoreGive(s_delivery_state_mutex);
}

static bool enqueue_tx_request(std::unique_ptr<tx_request_t> req, const char *tag, uint32_t can_id)
{
    tx_request_t *raw = req.release();
    if (xQueueSend(send_queue, &raw, 0) == pdTRUE) {
        return true;
    }

    ESP_LOGW(tag, "Send queue full, dropping packet with CAN ID 0x%08lx",
             static_cast<unsigned long>(can_id));
    delete raw;
    return false;
}

static bool in_flight_contains_target(const uint8_t *mac_addr)
{
    for (size_t i = 0; i < s_in_flight_target_count; i++) {
        if (std::memcmp(s_in_flight_targets[i], mac_addr, ESP_NOW_ETH_ALEN) == 0) {
            return true;
        }
    }
    return false;
}

static void log_send_error(const char *tag, esp_err_t err)
{
    if (err == ESP_ERR_ESPNOW_CHAN) {
        ESP_LOGE(tag, "esp_now_send failed: ESP_ERR_ESPNOW_CHAN (Wi-Fi channel mismatch)");
    } else if (err == ESP_ERR_ESPNOW_IF) {
        ESP_LOGE(tag, "esp_now_send failed: ESP_ERR_ESPNOW_IF (Wi-Fi interface mismatch)");
    } else {
        ESP_LOGE(tag, "esp_now_send failed synchronously: %s", esp_err_to_name(err));
    }
}

static void clear_in_flight_state(void)
{
    s_in_flight_active = false;
    s_in_flight_multicast = false;
    s_in_flight_success = false;
    s_in_flight_callbacks = 0;
    s_in_flight_expected_callbacks = 0;
    s_in_flight_first_success_tick = 0;
    s_in_flight_target_count = 0;
}

static void arm_in_flight_state(const tx_request_t &req)
{
    s_in_flight_multicast = req.multicast;
    s_in_flight_success = false;
    s_in_flight_callbacks = 0;
    s_in_flight_expected_callbacks = req.multicast ? req.target_count : 1;
    s_in_flight_first_success_tick = 0;
    s_in_flight_target_count = req.target_count;
    for (size_t i = 0; i < req.target_count; i++) {
        std::memcpy(s_in_flight_targets[i], req.targets[i], ESP_NOW_ETH_ALEN);
    }
    s_in_flight_active = true;
}

static void complete_request(const tx_request_t &req, bool success)
{
    if (req.completion_success != nullptr) {
        *req.completion_success = success;
    }
    if (req.completion_sem != nullptr) {
        xSemaphoreGive(req.completion_sem);
    }
}

static bool wait_for_direct_completion(void)
{
    const bool got_status =
        (xSemaphoreTake(espnow_tx_status_sem, pdMS_TO_TICKS(WCAN_TX_SEM_TIMEOUT_MS)) == pdTRUE);
    return got_status && s_in_flight_success;
}

static bool wait_for_multicast_completion(void)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t fail_timeout = pdMS_TO_TICKS(WCAN_MULTICAST_FAIL_TIMEOUT_MS);
    const TickType_t drain_timeout = pdMS_TO_TICKS(WCAN_MULTICAST_POST_SUCCESS_DRAIN_MS);

    while (true) {
        const TickType_t now = xTaskGetTickCount();
        if (s_in_flight_success) {
            if (s_in_flight_callbacks >= s_in_flight_expected_callbacks) {
                return true;
            }
            if (static_cast<int32_t>(now - s_in_flight_first_success_tick) >=
                static_cast<int32_t>(drain_timeout)) {
                return true;
            }
        } else if (static_cast<int32_t>(now - start) >= static_cast<int32_t>(fail_timeout)) {
            return false;
        }

        xSemaphoreTake(espnow_tx_status_sem, pdMS_TO_TICKS(1));
    }
}

static bool send_multicast_and_wait(uint8_t targets[WCAN_MAX_SUBSCRIBERS][ESP_NOW_ETH_ALEN],
                                    size_t target_count,
                                    const data_packet_t &packet)
{
    static const char *TAG = "send_multicast_wait";

    auto pkt = encode_data_packet(packet);
    if (!pkt) {
        ESP_LOGE(TAG, "encode_data_packet failed, dropping packet with CAN ID 0x%08lx",
                 static_cast<unsigned long>(packet.can_id));
        return false;
    }

    auto req = std::make_unique<tx_request_t>();
    req->packet = std::move(pkt);
    req->completion_sem = nullptr;
    req->completion_success = nullptr;
    req->multicast = true;
    req->target_count = target_count;
    for (size_t i = 0; i < target_count; i++) {
        std::memcpy(req->targets[i], targets[i], ESP_NOW_ETH_ALEN);
    }

    bool success = false;
    SemaphoreHandle_t completion_sem = xSemaphoreCreateBinary();
    if (completion_sem == nullptr) {
        ESP_LOGE(TAG, "Failed to create completion semaphore");
        return false;
    }
    req->completion_sem = completion_sem;
    req->completion_success = &success;

    const bool queued = enqueue_tx_request(std::move(req), TAG, packet.can_id);
    if (!queued) {
        vSemaphoreDelete(completion_sem);
        return false;
    }

    const bool completed = (xSemaphoreTake(completion_sem, portMAX_DELAY) == pdTRUE);
    vSemaphoreDelete(completion_sem);
    return completed && success;
}

void can_processing_task(void *pv_parameter)
{
    const size_t can_queue_index = reinterpret_cast<size_t>(pv_parameter);

    char TAG[20];
    std::snprintf(TAG, sizeof(TAG), "CAN_PROC_%u", static_cast<unsigned>(can_queue_index));

    const uint32_t can_id = get_can_id_from_queue_index(can_queue_index);

    ESP_LOGI(TAG, "CAN processing task %u started", static_cast<unsigned>(can_queue_index));

    uint8_t targets[WCAN_MAX_SUBSCRIBERS][ESP_NOW_ETH_ALEN];

    while (true) {
        auto packet = collect_packet(can_queue_index, can_id);
        if (!packet) {
            continue;
        }

        const size_t fan_out = subscription_snapshot_targets(can_id, targets);
        if (fan_out == 0) {
            mark_discovery_mode(can_queue_index);
            send_data(BROADCAST_MAC, *packet);
            ESP_LOGI(TAG, "0x%08lx batch %d [%lu..%lu] at (%lu) -> broadcast discovery (no alive subscribers)",
                     static_cast<unsigned long>(packet->can_id), packet->data_count,
                     static_cast<unsigned long>(packet->data[0]),
                     static_cast<unsigned long>(packet->data[packet->data_count - 1]),
                     static_cast<unsigned long>(packet->tick_count));
            continue;
        }

        if (get_delivery_mode(can_queue_index) == delivery_mode_t::BROADCAST_DISCOVERY) {
            send_data(BROADCAST_MAC, *packet);
            ESP_LOGI(TAG, "0x%08lx batch %d [%lu..%lu] at (%lu) -> broadcast discovery",
                     static_cast<unsigned long>(packet->can_id), packet->data_count,
                     static_cast<unsigned long>(packet->data[0]),
                     static_cast<unsigned long>(packet->data[packet->data_count - 1]),
                     static_cast<unsigned long>(packet->tick_count));
            continue;
        }

        size_t success_count = 0;
        for (uint8_t attempt = 1; attempt <= WCAN_MULTICAST_BATCH_MAX_ATTEMPTS; attempt++) {
            const bool success = send_multicast_and_wait(targets, fan_out, *packet);
            success_count = success ? 1 : 0;
            if (success) {
                break;
            }
            if (attempt < WCAN_MULTICAST_BATCH_MAX_ATTEMPTS) {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }

        record_multicast_batch_result(can_queue_index, success_count > 0);

        ESP_LOGI(TAG, "0x%08lx batch %d [%lu..%lu] at (%lu) -> %u/%u peers",
                 static_cast<unsigned long>(packet->can_id), packet->data_count,
                 static_cast<unsigned long>(packet->data[0]),
                 static_cast<unsigned long>(packet->data[packet->data_count - 1]),
                 static_cast<unsigned long>(packet->tick_count),
                 static_cast<unsigned>(success_count),
                 static_cast<unsigned>(fan_out));
    }
}

void send_processing_task(void *)
{
    static const char *TAG = "SEND";

    ESP_LOGI(TAG, "Send processing task started");

    while (true) {
        tx_request_t *raw = nullptr;
        if (xQueueReceive(send_queue, &raw, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        std::unique_ptr<tx_request_t> req(raw);
        std::unique_ptr<esp_now_packet_t> &pkt = req->packet;

        if (!pkt) {
            ESP_LOGE(TAG, "TX request missing packet");
            if (req->completion_success) {
                *req->completion_success = false;
            }
            if (req->completion_sem != nullptr) {
                xSemaphoreGive(req->completion_sem);
            }
            continue;
        }

        if (xSemaphoreTake(espnow_tx_sem, pdMS_TO_TICKS(WCAN_TX_SEM_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "TX semaphore timeout - driver may be stuck, dropping packet");
            if (req->completion_success) {
                *req->completion_success = false;
            }
            if (req->completion_sem != nullptr) {
                xSemaphoreGive(req->completion_sem);
            }
            continue;
        }

        if (espnow_tx_status_sem == nullptr) {
            ESP_LOGE(TAG, "espnow_tx_status_sem is NULL - init order error");
            complete_request(*req, false);
            xSemaphoreGive(espnow_tx_sem);
            continue;
        }

        while (xSemaphoreTake(espnow_tx_status_sem, 0) == pdTRUE) {
        }

        if (req->multicast) {
            sync_multicast_peers(req->targets, req->target_count);
        } else {
            add_peer(pkt->mac_addr.data());
        }

        arm_in_flight_state(*req);

#ifdef MEASURE_INSTR
        g_in_flight_send_us = esp_timer_get_time();
        if (req->multicast) {
            std::memset(const_cast<uint8_t *>(g_in_flight_peer_mac), 0, ESP_NOW_ETH_ALEN);
        } else {
            std::memcpy(const_cast<uint8_t *>(g_in_flight_peer_mac), pkt->mac_addr.data(), ESP_NOW_ETH_ALEN);
        }
#endif

        const uint8_t *dest_mac = req->multicast ? nullptr : pkt->mac_addr.data();
        const esp_err_t err = esp_now_send(dest_mac, pkt->data.get(), pkt->data_len);
        if (err != ESP_OK) {
            log_send_error(TAG, err);
            clear_in_flight_state();
            complete_request(*req, false);
            xSemaphoreGive(espnow_tx_sem);
            continue;
        }
#ifdef MEASURE_INSTR
        g_airtime_total_us += (static_cast<uint64_t>(pkt->data_len) * 8) / WCAN_PHY_RATE_MBPS;
        g_packets_sent_total = g_packets_sent_total + 1;
#endif

        const bool success = req->multicast ? wait_for_multicast_completion() : wait_for_direct_completion();
        clear_in_flight_state();
        complete_request(*req, success);
        xSemaphoreGive(espnow_tx_sem);
    }
}

void send_data(const uint8_t *mac_addr, const data_packet_t &data_packet)
{
    static const char *TAG = "send_data";

    auto pkt = encode_data_packet(data_packet);
    if (!pkt) {
        ESP_LOGE(TAG, "encode_data_packet failed, dropping packet with CAN ID 0x%08lx",
                 static_cast<unsigned long>(data_packet.can_id));
        return;
    }
    std::memcpy(pkt->mac_addr.data(), mac_addr, ESP_NOW_ETH_ALEN);

    auto req = std::make_unique<tx_request_t>();
    req->packet = std::move(pkt);
    req->completion_sem = nullptr;
    req->completion_success = nullptr;
    req->multicast = false;
    req->target_count = 0;

    enqueue_tx_request(std::move(req), TAG, data_packet.can_id);
}

bool send_data_and_wait(const uint8_t *mac_addr, const data_packet_t &data_packet)
{
    static const char *TAG = "send_data_wait";

    auto pkt = encode_data_packet(data_packet);
    if (!pkt) {
        ESP_LOGE(TAG, "encode_data_packet failed, dropping packet with CAN ID 0x%08lx",
                 static_cast<unsigned long>(data_packet.can_id));
        return false;
    }
    std::memcpy(pkt->mac_addr.data(), mac_addr, ESP_NOW_ETH_ALEN);

    auto req = std::make_unique<tx_request_t>();
    bool success = false;
    SemaphoreHandle_t completion_sem = xSemaphoreCreateBinary();
    if (completion_sem == nullptr) {
        ESP_LOGE(TAG, "Failed to create completion semaphore");
        return false;
    }

    req->packet = std::move(pkt);
    req->completion_sem = completion_sem;
    req->completion_success = &success;
    req->multicast = false;
    req->target_count = 0;

    const bool queued = enqueue_tx_request(std::move(req), TAG, data_packet.can_id);
    if (!queued) {
        vSemaphoreDelete(completion_sem);
        return false;
    }

    const bool completed = (xSemaphoreTake(completion_sem, portMAX_DELAY) == pdTRUE);
    vSemaphoreDelete(completion_sem);
    return completed && success;
}

void sender_on_send_status(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    static const char *TAG = "SEND_STATUS";

    if (mac_addr == nullptr) {
        return;
    }

    const bool success = (status == ESP_NOW_SEND_SUCCESS);
    subscription_record_tx_status(mac_addr, success);

    if (!s_in_flight_active) {
        ESP_LOGV(TAG, "Ignoring late TX callback from %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3],
                 mac_addr[4], mac_addr[5]);
        return;
    }

    if (s_in_flight_multicast && !in_flight_contains_target(mac_addr)) {
        ESP_LOGV(TAG, "Ignoring TX callback from non-target %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3],
                 mac_addr[4], mac_addr[5]);
        return;
    }

    s_in_flight_callbacks = s_in_flight_callbacks + 1;
    if (success && !s_in_flight_success) {
        s_in_flight_success = true;
        s_in_flight_first_success_tick = xTaskGetTickCount();
    }

    if (espnow_tx_status_sem != nullptr) {
        xSemaphoreGive(espnow_tx_status_sem);
    }
}

void sender_init_delivery_state(void)
{
    static const char *TAG = "SEND_STATE";

    if (num_can_queues == 0 || s_delivery_modes != nullptr) {
        return;
    }

    s_delivery_modes = static_cast<delivery_mode_t *>(calloc(num_can_queues, sizeof(delivery_mode_t)));
    s_zero_success_multicast_batches = static_cast<uint8_t *>(calloc(num_can_queues, sizeof(uint8_t)));
    s_delivery_state_mutex = xSemaphoreCreateMutex();

    if (s_delivery_modes == nullptr || s_zero_success_multicast_batches == nullptr || s_delivery_state_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate delivery state");
        return;
    }

    for (size_t i = 0; i < num_can_queues; i++) {
        s_delivery_modes[i] = delivery_mode_t::BROADCAST_DISCOVERY;
        s_zero_success_multicast_batches[i] = 0;
    }
}

void sender_on_subscription_update(const uint32_t *ids, size_t n)
{
    if (s_delivery_modes == nullptr || s_zero_success_multicast_batches == nullptr ||
        s_delivery_state_mutex == nullptr || num_can_queues == 0) {
        return;
    }

    if (xSemaphoreTake(s_delivery_state_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    if (n == 0 || ids == nullptr) {
        for (size_t i = 0; i < num_can_queues; i++) {
            s_delivery_modes[i] = delivery_mode_t::MULTICAST_ACTIVE;
            s_zero_success_multicast_batches[i] = 0;
        }
        xSemaphoreGive(s_delivery_state_mutex);
        return;
    }

    for (size_t id_idx = 0; id_idx < n; id_idx++) {
        const int queue_index = get_queue_index_for_can_id(ids[id_idx]);
        if (queue_index < 0) {
            continue;
        }
        s_delivery_modes[queue_index] = delivery_mode_t::MULTICAST_ACTIVE;
        s_zero_success_multicast_batches[queue_index] = 0;
    }

    xSemaphoreGive(s_delivery_state_mutex);
}

#ifdef MEASURE_INSTR
#define MEASURE_LOG_INTERVAL_MS 5000

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
static void log_task_stats(void)
{
    const UBaseType_t n_tasks = uxTaskGetNumberOfTasks();
    if (n_tasks == 0) {
        return;
    }
    TaskStatus_t *task_status = static_cast<TaskStatus_t *>(
        pvPortMalloc(static_cast<size_t>(n_tasks) * sizeof(TaskStatus_t)));
    if (task_status == nullptr) {
        ESP_LOGW("MEASURE", "task stats: OOM (n_tasks=%u)", static_cast<unsigned>(n_tasks));
        return;
    }
    uint32_t total_runtime = 0;
    const UBaseType_t got = uxTaskGetSystemState(task_status, n_tasks, &total_runtime);
    for (UBaseType_t i = 0; i < got; i++) {
        const TaskStatus_t &t = task_status[i];
        const uint32_t hwm_bytes =
            static_cast<uint32_t>(t.usStackHighWaterMark) * sizeof(StackType_t);
        ESP_LOGI("TASK",
                 "name=%s prio=%lu state=%lu hwm_bytes=%lu runtime=%lu total_runtime=%lu",
                 t.pcTaskName,
                 static_cast<unsigned long>(t.uxCurrentPriority),
                 static_cast<unsigned long>(t.eCurrentState),
                 static_cast<unsigned long>(hwm_bytes),
                 static_cast<unsigned long>(t.ulRunTimeCounter),
                 static_cast<unsigned long>(total_runtime));
    }
    vPortFree(task_status);
}
#endif

static void measure_periodic_task(void *)
{
    static const char *TAG = "MEASURE";
    const TickType_t period = pdMS_TO_TICKS(MEASURE_LOG_INTERVAL_MS);
    uint64_t prev_airtime_us = 0;
    uint64_t prev_packets = 0;

    while (true) {
        vTaskDelay(period);
        const uint64_t airtime = g_airtime_total_us;
        const uint64_t packets = g_packets_sent_total;
        const uint64_t d_airtime = airtime - prev_airtime_us;
        const uint64_t d_packets = packets - prev_packets;
        prev_airtime_us = airtime;
        prev_packets = packets;

        // utilisation per mille (parts per thousand) over the interval window
        const uint64_t window_us = static_cast<uint64_t>(MEASURE_LOG_INTERVAL_MS) * 1000ULL;
        const uint32_t util_per_mille = window_us == 0 ? 0
            : static_cast<uint32_t>((d_airtime * 1000ULL) / window_us);

        ESP_LOGI(TAG,
                 "airtime_us_total=%llu packets_total=%llu d_airtime_us=%llu d_packets=%llu util_per_mille=%lu",
                 airtime, packets, d_airtime, d_packets, static_cast<unsigned long>(util_per_mille));

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
        log_task_stats();
#endif
    }
}

void measure_start(void)
{
    xTaskCreate(measure_periodic_task, "measure", 3072, nullptr, 1, nullptr);
}
#endif

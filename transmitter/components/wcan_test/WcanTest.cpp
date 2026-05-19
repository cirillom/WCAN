#include "WcanTest.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
#include "driver/usb_serial_jtag.h"
#endif

#ifndef CONFIG_ESP_CONSOLE_UART_NUM
#define CONFIG_ESP_CONSOLE_UART_NUM 0
#endif

#ifndef CONFIG_ESP_CONSOLE_UART_BAUDRATE
#define CONFIG_ESP_CONSOLE_UART_BAUDRATE 115200
#endif

#ifndef WCAN_TRANSPORT_NAME
#define WCAN_TRANSPORT_NAME "UNKNOWN"
#endif

namespace wcan_test {

namespace {

static const char* TAG = "WCAN_TEST";
static constexpr size_t kLineBufferSize = 256;
static constexpr uart_port_t kUartPort = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
static constexpr TickType_t kInputPollTicks = pdMS_TO_TICKS(20);

struct ParseResult {
    bool ok;
    const char* error;
};

const char* RoleName(Role role) {
    switch (role) {
    case Role::kSensor:
        return "sensor";
    case Role::kReceiver:
        return "receiver";
    case Role::kIdle:
        return "idle";
    }
    return "unknown";
}

void PrintHostLine(const char* line) {
    std::printf("%s\n", line);
    std::fflush(stdout);

#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
    if (usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_write_bytes(line, std::strlen(line), pdMS_TO_TICKS(20));
        usb_serial_jtag_write_bytes("\n", 1, pdMS_TO_TICKS(20));
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(20));
    }
#endif
}

void InitUartInput() {
    uart_config_t uart_config = {};
    uart_config.baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_param_config(kUartPort, &uart_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
    }

    err = uart_driver_install(kUartPort, 1024, 8192, 0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
    }

#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usb_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        err = usb_serial_jtag_driver_install(&usb_config);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(err));
        }
    }
#endif
}

bool ReadInputByte(uint8_t* ch) {
    if (uart_read_bytes(kUartPort, ch, 1, kInputPollTicks) > 0) {
        return true;
    }

#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
    if (usb_serial_jtag_is_driver_installed() &&
        usb_serial_jtag_read_bytes(ch, 1, kInputPollTicks) > 0) {
        return true;
    }
#endif

    return false;
}

bool ReadConfigLine(char* line, size_t line_size) {
    size_t len = 0;
    while (true) {
        uint8_t ch = 0;
        if (!ReadInputByte(&ch)) {
            continue;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            if (len == 0) {
                continue;
            }
            line[len] = '\0';
            return true;
        }
        if (len + 1 >= line_size) {
            line[0] = '\0';
            return false;
        }
        line[len++] = static_cast<char>(ch);
    }
}

bool ParseUint32(const char* value, uint32_t* out) {
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *out = static_cast<uint32_t>(parsed);
    return true;
}

bool ParseSize(const char* value, size_t* out) {
    uint32_t parsed = 0;
    if (!ParseUint32(value, &parsed)) {
        return false;
    }
    *out = static_cast<size_t>(parsed);
    return true;
}

bool CanIdValid(uint32_t can_id) {
    return can_id <= TestConfig::kMaxCanId;
}

bool StringEquals(const char* a, const char* b) {
    return std::strcmp(a, b) == 0;
}

void CopyTransport(TestConfig* config, const char* value) {
    std::strncpy(config->transport, value, sizeof(config->transport) - 1);
    config->transport[sizeof(config->transport) - 1] = '\0';
}

bool ParseFilterList(const char* value, TestConfig* config) {
    if (StringEquals(value, "all") || StringEquals(value, "accept-all")) {
        config->receiver_filter_enabled = false;
        config->receiver_filter_count = 0;
        return true;
    }
    if (StringEquals(value, "none") || StringEquals(value, "empty")) {
        config->receiver_filter_enabled = true;
        config->receiver_filter_count = 0;
        return true;
    }

    char scratch[kLineBufferSize] = {};
    std::strncpy(scratch, value, sizeof(scratch) - 1);

    size_t count = 0;
    char* saveptr = nullptr;
    for (char* part = strtok_r(scratch, ",", &saveptr); part != nullptr;
         part = strtok_r(nullptr, ",", &saveptr)) {
        if (count >= TestConfig::kMaxCanIds) {
            return false;
        }
        uint32_t can_id = 0;
        if (!ParseUint32(part, &can_id) || !CanIdValid(can_id)) {
            return false;
        }
        config->receiver_filter_ids[count++] = can_id;
    }

    config->receiver_filter_enabled = true;
    config->receiver_filter_count = count;
    return count > 0;
}

ParseResult ValidateConfig(const TestConfig& config) {
    if (config.transport[0] != '\0' && !StringEquals(config.transport, WCAN_TRANSPORT_NAME)) {
        return {false, "transport-mismatch"};
    }

    if (config.role == Role::kSensor) {
        if (config.sensor_hz < TestConfig::kMinSensorHz || config.sensor_hz > TestConfig::kMaxSensorHz) {
            return {false, "sensor-hz-range"};
        }
        if (config.sensor_can_id_count < 1 || config.sensor_can_id_count > TestConfig::kMaxCanIds) {
            return {false, "sensor-count-range"};
        }
        if (!CanIdValid(config.sensor_base_can_id)) {
            return {false, "sensor-can-id-range"};
        }
        const uint32_t last_offset = static_cast<uint32_t>(config.sensor_can_id_count - 1);
        if (config.sensor_base_can_id > TestConfig::kMaxCanId - last_offset) {
            return {false, "sensor-can-id-range"};
        }
        if (config.linger_ms > TestConfig::kMaxWcanLingerMs) {
            return {false, "linger-range"};
        }
    }

    if (config.test_duration_ms == 0 || config.test_duration_ms > TestConfig::kMaxTestDurationMs) {
        return {false, "test-duration-range"};
    }
    if (config.host_wait_time_ms == 0 || config.host_wait_time_ms > TestConfig::kMaxHostWaitTimeMs) {
        return {false, "host-wait-range"};
    }

    if (config.role == Role::kReceiver && config.receiver_filter_count > TestConfig::kMaxCanIds) {
        return {false, "receiver-filter-count-range"};
    }

    return {true, nullptr};
}

ParseResult ParseLine(char* line, TestConfig* config) {
    *config = TestConfig{};
    CopyTransport(config, WCAN_TRANSPORT_NAME);

    bool saw_role = false;
    bool saw_version = false;

    char* saveptr = nullptr;
    for (char* token = strtok_r(line, " \t", &saveptr); token != nullptr;
         token = strtok_r(nullptr, " \t", &saveptr)) {
        if (StringEquals(token, "wcan")) {
            continue;
        }

        char* equals = std::strchr(token, '=');
        if (equals == nullptr) {
            return {false, "expected-key-value"};
        }
        *equals = '\0';
        const char* key = token;
        const char* value = equals + 1;

        if (StringEquals(key, "v")) {
            uint32_t version = 0;
            if (!ParseUint32(value, &version) || version != TestConfig::kProtocolVersion) {
                return {false, "bad-version"};
            }
            saw_version = true;
        } else if (StringEquals(key, "role") || StringEquals(key, "mode")) {
            if (StringEquals(value, "sensor")) {
                config->role = Role::kSensor;
            } else if (StringEquals(value, "receiver")) {
                config->role = Role::kReceiver;
            } else if (StringEquals(value, "idle")) {
                config->role = Role::kIdle;
            } else {
                return {false, "bad-role"};
            }
            saw_role = true;
        } else if (StringEquals(key, "base")) {
            if (!ParseUint32(value, &config->sensor_base_can_id)) {
                return {false, "bad-base"};
            }
        } else if (StringEquals(key, "count")) {
            if (!ParseSize(value, &config->sensor_can_id_count)) {
                return {false, "bad-count"};
            }
        } else if (StringEquals(key, "hz")) {
            uint32_t hz = 0;
            if (!ParseUint32(value, &hz)) {
                return {false, "bad-hz"};
            }
            config->sensor_hz = static_cast<int>(hz);
        } else if (StringEquals(key, "linger")) {
            if (!ParseUint32(value, &config->linger_ms)) {
                return {false, "bad-linger"};
            }
        } else if (StringEquals(key, "test_duration_ms")) {
            if (!ParseUint32(value, &config->test_duration_ms)) {
                return {false, "bad-test-duration"};
            }
        } else if (StringEquals(key, "host_wait_time_ms")) {
            if (!ParseUint32(value, &config->host_wait_time_ms)) {
                return {false, "bad-host-wait"};
            }
        } else if (StringEquals(key, "filter") || StringEquals(key, "filters")) {
            if (!ParseFilterList(value, config)) {
                return {false, "bad-filter"};
            }
        } else if (StringEquals(key, "transport")) {
            CopyTransport(config, value);
        } else {
            return {false, "unknown-key"};
        }
    }

    if (!saw_version) {
        return {false, "missing-version"};
    }
    if (!saw_role) {
        return {false, "missing-role"};
    }

    return ValidateConfig(*config);
}

} // namespace

const char* TestConfig::role_name() const {
    return RoleName(role);
}

std::vector<wcan::CANId_t> TestConfig::sensor_tx_ids() const {
    std::vector<wcan::CANId_t> ids;
    ids.reserve(sensor_can_id_count);
    for (size_t i = 0; i < sensor_can_id_count; ++i) {
        ids.push_back(sensor_base_can_id + static_cast<uint32_t>(i));
    }
    return ids;
}

std::vector<wcan::CANId_t> TestConfig::receiver_rx_ids() const {
    std::vector<wcan::CANId_t> ids;
    if (!receiver_filter_enabled) {
        return ids;
    }
    ids.reserve(receiver_filter_count);
    for (size_t i = 0; i < receiver_filter_count; ++i) {
        ids.push_back(receiver_filter_ids[i]);
    }
    return ids;
}

TestConfig UartTestProtocol::wait_for_boot_config() {
    InitUartInput();
    PrintHostLine("WCAN_CFG_WAIT v=1");

    while (true) {
        char line[kLineBufferSize] = {};
        if (!ReadConfigLine(line, sizeof(line))) {
            PrintHostLine("WCAN_CFG_NACK line-too-long");
            continue;
        }

        TestConfig config = {};
        char parse_line[kLineBufferSize] = {};
        std::strncpy(parse_line, line, sizeof(parse_line) - 1);
        const ParseResult result = ParseLine(parse_line, &config);
        if (!result.ok) {
            char response[80] = {};
            std::snprintf(response, sizeof(response), "WCAN_CFG_NACK %s", result.error);
            PrintHostLine(response);
            ESP_LOGW(TAG, "rejected config: %s (%s)", result.error, line);
            continue;
        }

        PrintHostLine("WCAN_CFG_ACK");
        return config;
    }
}

void UartTestProtocol::wait_for_test_start() {
    while (true) {
        char line[kLineBufferSize] = {};
        if (!ReadConfigLine(line, sizeof(line))) {
            continue;
        }
        if (StringEquals(line, "WCAN_TEST_START")) {
            return;
        }
    }
}

void UartTestProtocol::print_ready(Role role) {
    std::printf("WCAN_TEST_READY role=%s\n", RoleName(role));
    std::fflush(stdout);
}

void UartTestProtocol::print_abort(Role role, const char* reason) {
    std::printf("WCAN_TEST_ABORT role=%s reason=%s\n", RoleName(role), reason);
    std::fflush(stdout);
}

void WcanTestSession::ready() const {
    UartTestProtocol::print_ready(_config.role);
}

void WcanTestSession::abort(const char* reason) const {
    UartTestProtocol::print_abort(_config.role, reason);
}

void WcanTestSession::wait_idle_start() const {
    UartTestProtocol::wait_for_test_start();
}

void WcanTestSession::run(wcan::TransceiverBase& transceiver, wcan_sensor::RampCanSensor* sensor) const {
    UartTestProtocol::wait_for_test_start();
    transceiver.stats().reset();

    if (sensor != nullptr) {
        sensor->set_send_failure_callback([](uint32_t can_id, uint32_t counter) {
            std::printf("S(FAIL):%lu:%lx:%lu\n",
                        static_cast<unsigned long>(pdTICKS_TO_MS(xTaskGetTickCount())),
                        static_cast<unsigned long>(can_id),
                        static_cast<unsigned long>(counter));
            std::fflush(stdout);
        });
        if (!sensor->start(static_cast<uint32_t>(_config.sensor_hz))) {
            abort("sensor-timer");
            return;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(_config.test_duration_ms));
    transceiver.stats().finish_test();

    if (sensor != nullptr) {
        sensor->stop();
    }

    transceiver.stop(stop_drain_timeout_ms());
    transceiver.stats().print_batch_stats();

    if (_config.role == Role::kSensor && sensor != nullptr) {
        transceiver.stats().print_sensor_end(sensor->generated_count());
    }
    if (_config.role == Role::kReceiver) {
        transceiver.stats().print_rx_ranges();
    }
    transceiver.stats().print_measures();
    std::printf("WCAN_TEST_END role=%s\n", _config.role_name());
    std::fflush(stdout);
}

uint32_t WcanTestSession::stop_drain_timeout_ms() const {
    return std::max<uint32_t>(1, std::min<uint32_t>(1000, _config.host_wait_time_ms / 2));
}

} // namespace wcan_test

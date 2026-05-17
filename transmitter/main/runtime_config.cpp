#include "runtime_config.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "wcan.hpp"

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

namespace runtime_config {
namespace {

static const char *TAG = "BOOT_CONFIG";
static constexpr size_t kLineBufferSize = 256;
static constexpr uart_port_t kUartPort = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
static constexpr TickType_t kInputPollTicks = pdMS_TO_TICKS(20);

struct ParseResult {
    bool ok;
    const char *error;
};

void PrintHostLine(const char *line)
{
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

void InitUartInput()
{
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

bool ReadInputByte(uint8_t *ch)
{
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

bool ReadConfigLine(char *line, size_t line_size)
{
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

bool ParseUint32(const char *value, uint32_t *out)
{
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *out = static_cast<uint32_t>(parsed);
    return true;
}

bool ParseSize(const char *value, size_t *out)
{
    uint32_t parsed = 0;
    if (!ParseUint32(value, &parsed)) {
        return false;
    }
    *out = static_cast<size_t>(parsed);
    return true;
}

bool CanIdValid(uint32_t can_id)
{
    return can_id <= CAN_ID_MAX;
}

bool StringEquals(const char *a, const char *b)
{
    return std::strcmp(a, b) == 0;
}

void CopyTransport(RuntimeConfig *config, const char *value)
{
    std::strncpy(config->transport, value, sizeof(config->transport) - 1);
    config->transport[sizeof(config->transport) - 1] = '\0';
}

bool ParseFilterList(const char *value, RuntimeConfig *config)
{
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
    char *saveptr = nullptr;
    for (char *part = strtok_r(scratch, ",", &saveptr); part != nullptr;
         part = strtok_r(nullptr, ",", &saveptr)) {
        if (count >= app_config::kMaxCanIds) {
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

ParseResult ValidateConfig(const RuntimeConfig &config)
{
    if (config.transport[0] != '\0' && !StringEquals(config.transport, WCAN_TRANSPORT_NAME)) {
        return {false, "transport-mismatch"};
    }

    if (config.role == Role::kSensor) {
        if (config.sensor_hz < app_config::kMinSensorHz || config.sensor_hz > app_config::kMaxSensorHz) {
            return {false, "sensor-hz-range"};
        }
        if (config.sensor_can_id_count < 1 || config.sensor_can_id_count > app_config::kMaxCanIds) {
            return {false, "sensor-count-range"};
        }
        if (!CanIdValid(config.sensor_base_can_id)) {
            return {false, "sensor-can-id-range"};
        }
        const uint32_t last_offset = static_cast<uint32_t>(config.sensor_can_id_count - 1);
        if (config.sensor_base_can_id > CAN_ID_MAX - last_offset) {
            return {false, "sensor-can-id-range"};
        }
        if (config.linger_ms > app_config::kMaxWcanLingerMs) {
            return {false, "linger-range"};
        }
    }

    if (config.role == Role::kReceiver && config.receiver_filter_count > app_config::kMaxCanIds) {
        return {false, "receiver-filter-count-range"};
    }

    return {true, nullptr};
}

ParseResult ParseLine(char *line, RuntimeConfig *config)
{
    *config = RuntimeConfig{};
    CopyTransport(config, WCAN_TRANSPORT_NAME);

    bool saw_role = false;
    bool saw_version = false;

    char *saveptr = nullptr;
    for (char *token = strtok_r(line, " \t", &saveptr); token != nullptr;
         token = strtok_r(nullptr, " \t", &saveptr)) {
        if (StringEquals(token, "wcan")) {
            continue;
        }

        char *equals = std::strchr(token, '=');
        if (equals == nullptr) {
            return {false, "expected-key-value"};
        }
        *equals = '\0';
        const char *key = token;
        const char *value = equals + 1;

        if (StringEquals(key, "v")) {
            uint32_t version = 0;
            if (!ParseUint32(value, &version) || version != app_config::kRuntimeConfigProtocolVersion) {
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

void LogConfig(const RuntimeConfig &config)
{
    ESP_LOGI(TAG, "accepted role=%s transport=%s", RoleName(config.role), config.transport);
    if (config.role == Role::kSensor) {
        ESP_LOGI(TAG, "sensor base=0x%lx count=%u hz=%d linger=%lu",
                 static_cast<unsigned long>(config.sensor_base_can_id),
                 static_cast<unsigned>(config.sensor_can_id_count), config.sensor_hz,
                 static_cast<unsigned long>(config.linger_ms));
    } else if (config.role == Role::kReceiver) {
        ESP_LOGI(TAG, "receiver filter=%s count=%u",
                 config.receiver_filter_enabled ? "active" : "accept-all",
                 static_cast<unsigned>(config.receiver_filter_count));
    }
}

} // namespace

const char *RoleName(Role role)
{
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

RuntimeConfig WaitForBootConfig()
{
    InitUartInput();
    ESP_LOGI(TAG, "waiting for UART config");
    PrintHostLine("WCAN_CFG_WAIT v=1");

    while (true) {
        char line[kLineBufferSize] = {};
        if (!ReadConfigLine(line, sizeof(line))) {
            PrintHostLine("WCAN_CFG_NACK line-too-long");
            continue;
        }

        RuntimeConfig config = {};
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

        LogConfig(config);
        PrintHostLine("WCAN_CFG_ACK");
        return config;
    }
}

} // namespace runtime_config

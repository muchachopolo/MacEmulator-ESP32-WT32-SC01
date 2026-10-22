/* Pi shim: ESP-IDF logging -> Circle's logger (HDMI/serial). */
#pragma once
#include <stdint.h>
#include <stdio.h>      /* the core relies on it, as with ESP-IDF */
#include "pi_platform.h"
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO,
               ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
#define ESP_LOGE(tag, fmt, ...) pi_log(ESP_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) pi_log(ESP_LOG_WARN, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) pi_log(ESP_LOG_INFO, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do { } while (0)
#define ESP_LOGV(tag, fmt, ...) do { } while (0)
static inline uint32_t esp_log_timestamp(void) { return (uint32_t)(pi_micros() / 1000); }
static inline void esp_log_level_set(const char *tag, esp_log_level_t l) { (void)tag; (void)l; }

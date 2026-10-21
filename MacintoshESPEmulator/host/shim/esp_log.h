/* Host shim: ESP-IDF logging -> stderr, "L (ms) tag: msg" like ESP-IDF. */
#pragma once
#include <stdio.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO,
               ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
uint32_t esp_log_timestamp(void);
extern FILE *host_log_file;   /* NULL -> stderr */
#define HOST_LOG_(L, tag, fmt, ...) \
    fprintf(host_log_file ? host_log_file : stderr, L " (%u) %s: " fmt "\n", \
            (unsigned)esp_log_timestamp(), tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) HOST_LOG_("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) HOST_LOG_("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) HOST_LOG_("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do { } while (0)
#define ESP_LOGV(tag, fmt, ...) do { } while (0)
static inline void esp_log_level_set(const char *tag, esp_log_level_t l) { (void)tag; (void)l; }
#ifdef __cplusplus
}
#endif

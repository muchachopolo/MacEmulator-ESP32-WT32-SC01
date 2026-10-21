/* Host shim: the bits of FreeRTOS the emulator core touches. */
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define configTICK_RATE_HZ    1000
#define configMAX_PRIORITIES  25
#define portMAX_DELAY         0xFFFFFFFFu
#define portTICK_PERIOD_MS    (1000 / configTICK_RATE_HZ)
#define pdMS_TO_TICKS(ms)     ((TickType_t)(ms))
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1

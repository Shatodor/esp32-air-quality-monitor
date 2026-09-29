#ifndef TASK_H
#define TASK_H
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(void (*fn)(void *), const char *name,
                       uint32_t stack, void *arg,
                       UBaseType_t prio, TaskHandle_t *handle);
void vTaskDelete(TaskHandle_t h);
void vTaskDelay(TickType_t t);
#endif

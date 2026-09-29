#ifndef SEMPHR_H
#define SEMPHR_H
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t h, TickType_t timeout);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t h);
#endif

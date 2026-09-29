#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *m = malloc(sizeof(*m));
    if (!m) return NULL;
    pthread_mutex_init(m, NULL);
    return (SemaphoreHandle_t)m;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t t)
{
    (void)t;
    if (!h) return pdFALSE;
    pthread_mutex_lock((pthread_mutex_t *)h);
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t h)
{
    if (!h) return pdFALSE;
    pthread_mutex_unlock((pthread_mutex_t *)h);
    return pdTRUE;
}

typedef struct { pthread_mutex_t mtx; EventBits_t bits; } eg_t;
EventGroupHandle_t xEventGroupCreate(void)
{
    eg_t *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    pthread_mutex_init(&g->mtx, NULL);
    return (EventGroupHandle_t)g;
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t h, EventBits_t b)
{
    eg_t *g = (eg_t *)h;
    if (!g) return 0;
    pthread_mutex_lock(&g->mtx);
    EventBits_t p = g->bits; g->bits |= b;
    pthread_mutex_unlock(&g->mtx);
    return p;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t h, EventBits_t b)
{
    eg_t *g = (eg_t *)h;
    if (!g) return 0;
    pthread_mutex_lock(&g->mtx);
    EventBits_t p = g->bits; g->bits &= ~b;
    pthread_mutex_unlock(&g->mtx);
    return p;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t h, EventBits_t b,
                                BaseType_t c, BaseType_t a, TickType_t t)
{
    (void)t; (void)a;
    eg_t *g = (eg_t *)h;
    if (!g) return 0;
    pthread_mutex_lock(&g->mtx);
    EventBits_t cur = g->bits;
    if (c) g->bits &= ~b;
    pthread_mutex_unlock(&g->mtx);
    return cur;
}
BaseType_t xTaskCreate(void (*fn)(void *), const char *n, uint32_t s,
                       void *a, UBaseType_t p, TaskHandle_t *h)
{
    (void)fn; (void)n; (void)s; (void)a; (void)p;
    if (h) *h = NULL;
    return pdPASS;
}
void vTaskDelete(TaskHandle_t h) { (void)h; }
void vTaskDelay(TickType_t t)    { (void)t; }

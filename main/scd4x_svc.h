#ifndef SCD4X_SVC_H
#define SCD4X_SVC_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Снимок измерения. Поле valid — приватное (используется внутри scd4x_svc
   для отслеживания «есть ли непрочитанное измерение»). Наружу смотреть
   на него не нужно. */
typedef struct {
    float co2;
    float temperature;
    float humidity;
    bool  valid;
} scd4x_data_t;

/* Инициализация сенсора на шине: регистрация, чтение метаданных,
   запуск периодических измерений. Задачу чтения НЕ запускает. */
esp_err_t scd4x_svc_setup(i2c_master_bus_handle_t bus);

/* Запустить фоновую задачу чтения. Период — period_ms миллисекунд. */
esp_err_t scd4x_svc_start(uint32_t period_ms);

/* Забрать последнее непрочитанное измерение.
   При успехе копирует данные в *out и помечает их прочитанными —
   следующий вызов вернёт false до нового измерения.
   Потокобезопасно. */
bool scd4x_svc_take_latest(scd4x_data_t *out);

/* true, если сенсор успешно инициализирован. */
bool scd4x_svc_is_ready(void);

#endif // SCD4X_SVC_H
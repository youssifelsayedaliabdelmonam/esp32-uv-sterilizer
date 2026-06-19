#ifndef BUZZER_H
#define BUZZER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "system_types.h"

void buzzerInit();
void buzzerStartTask();
void buzzerRequest(BeepPattern pattern);
QueueHandle_t buzzerGetQueue();

#endif // BUZZER_H

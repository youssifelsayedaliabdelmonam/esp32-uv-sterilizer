#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "system_types.h"

void webServerInit();
void webServerStartManagerTask();
void webServerToggle();
void webServerStop();
bool webServerIsActive();

#endif // WEB_SERVER_H

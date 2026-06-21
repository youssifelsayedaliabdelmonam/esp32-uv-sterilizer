#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "system_types.h"

void stateMachineInit();
void relaysSafeBootInit();
void stateMachineStartTask();
SystemState stateMachineGetState();
void stateMachineGetStatus(SystemStatus& out);
bool stateMachineIsIdleExitAssist();
bool stateMachineIsWebServerAllowed();
void stateMachineSetWebInfo(bool active, const char* ip);

#endif // STATE_MACHINE_H

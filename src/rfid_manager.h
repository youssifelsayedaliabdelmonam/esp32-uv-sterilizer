#ifndef RFID_MANAGER_H
#define RFID_MANAGER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "system_types.h"

void rfidInit();
void rfidStartTask();
QueueHandle_t rfidGetTagQueue();

// Enrollment control (called from web task)
void rfidStartEnrollment(EnrollType type);
bool rfidWaitEnrollmentResult(char* uidOut, size_t uidLen, uint32_t timeoutMs);
void rfidCancelEnrollment();
bool rfidIsEnrollmentActive();

// Query reader health flags
bool rfidEntranceHealthy();
bool rfidInsideHealthy();
uint8_t rfidEntranceVersion();
uint8_t rfidInsideVersion();
void rfidResetScanCooldown();

#endif // RFID_MANAGER_H

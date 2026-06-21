/**
 * UV Sterilization Room Controller – ESP32
 *
 * Main entry: initializes hardware, storage, and FreeRTOS tasks.
 * Boot button long-press or admin NFC tag toggles the WiFi admin web server.
 */

#include <Arduino.h>
#include "config.h"
#include "storage.h"
#include "buzzer.h"
#include "lcd_display.h"
#include "rfid_manager.h"
#include "state_machine.h"
#include "web_server.h"

// Long-press detection state for GPIO0 boot button
static uint32_t buttonPressStart = 0;
static bool buttonWasPressed = false;
static bool longPressHandled = false;

static void buttonTask(void* param) {
    (void)param;
    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

    for (;;) {
        bool pressed = (digitalRead(PIN_BOOT_BUTTON) == LOW);
        uint32_t now = millis();

        if (pressed) {
            if (!buttonWasPressed) {
                buttonWasPressed = true;
                buttonPressStart = now;
                longPressHandled = false;
            } else if (!longPressHandled && (now - buttonPressStart >= LONG_PRESS_DURATION_MS)) {
                longPressHandled = true;
                Serial.println("[Button] Long press – toggle web server");
                webServerToggle();
            }
        } else {
            buttonWasPressed = false;
            longPressHandled = false;
        }

        vTaskDelay(pdMS_TO_TICKS(BOOT_DEBOUNCE_MS));
    }
}

void setup() {
    // Keep active-low relays OFF (HIGH) before any other peripheral init
    relaysSafeBootInit();

    Serial.begin(115200);
    Serial.println();
    Serial.println("=== UV Sterilization Room Controller ===");

    if (!storage.begin()) {
        Serial.println("[Main] Storage init failed – continuing with defaults");
    }

    buzzerInit();
    lcdInit();
    rfidInit();
    stateMachineInit();
    webServerInit();

    buzzerStartTask();
    lcdStartTask();
    rfidStartTask();
    stateMachineStartTask();
    webServerStartManagerTask();

    xTaskCreatePinnedToCore(
        buttonTask, "buttonTask",
        TASK_STACK_BUTTON, nullptr,
        TASK_PRIO_BUTTON, nullptr,
        CORE_LCD_BUZZER
    );

    Serial.println("[Main] All tasks started. System ready.");
}

void loop() {
    // All work runs in FreeRTOS tasks; loop stays idle
    vTaskDelay(pdMS_TO_TICKS(1000));
}

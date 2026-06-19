#include "buzzer.h"
#include "config.h"

static QueueHandle_t buzzerQueue = nullptr;

static void buzzerWrite(bool on) {
    digitalWrite(PIN_BUZZER, on ? BUZZER_ACTIVE : BUZZER_INACTIVE);
}

static void playBeep(uint32_t durationMs) {
    buzzerWrite(true);
    vTaskDelay(pdMS_TO_TICKS(durationMs));
    buzzerWrite(false);
}

static void buzzerTask(void* param) {
    (void)param;
  BeepPattern pattern;

    for (;;) {
        if (xQueueReceive(buzzerQueue, &pattern, portMAX_DELAY) == pdTRUE) {
            switch (pattern) {
                case BEEP_SINGLE_SHORT:
                    playBeep(BEEP_SHORT_MS);
                    break;
                case BEEP_DOUBLE_SHORT:
                    playBeep(BEEP_SHORT_MS);
                    vTaskDelay(pdMS_TO_TICKS(BEEP_GAP_MS));
                    playBeep(BEEP_SHORT_MS);
                    break;
                case BEEP_LONG:
                    playBeep(BEEP_LONG_MS);
                    break;
                case BEEP_ERROR:
                    playBeep(BEEP_ERROR_MS);
                    break;
            }
        }
    }
}

void buzzerInit() {
    pinMode(PIN_BUZZER, OUTPUT);
    buzzerWrite(false);
    buzzerQueue = xQueueCreate(QUEUE_BUZZER, sizeof(BeepPattern));
}

void buzzerStartTask() {
    xTaskCreatePinnedToCore(
        buzzerTask, "buzzerTask",
        TASK_STACK_BUZZER, nullptr,
        TASK_PRIO_BUZZER, nullptr,
        CORE_LCD_BUZZER
    );
}

void buzzerRequest(BeepPattern pattern) {
    if (buzzerQueue) {
        xQueueSend(buzzerQueue, &pattern, 0);
    }
}

QueueHandle_t buzzerGetQueue() {
    return buzzerQueue;
}

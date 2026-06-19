#include "lcd_display.h"
#include "config.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);
static SemaphoreHandle_t statusMutex = nullptr;
static SystemStatus sharedStatus = {};

static void copyStatus(SystemStatus& dest, const SystemStatus& src) {
    if (xSemaphoreTake(statusMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        dest = src;
        xSemaphoreGive(statusMutex);
    }
}

void lcdUpdateSharedStatus(const SystemStatus& status) {
    if (statusMutex && xSemaphoreTake(statusMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        sharedStatus = status;
        xSemaphoreGive(statusMutex);
    }
}

static void padLine(char* buf, size_t len) {
    size_t n = strlen(buf);
    while (n < len - 1) buf[n++] = ' ';
    buf[len - 1] = '\0';
}

static void lcdTask(void* param) {
    (void)param;
    SystemStatus st;
    char line[LCD_COLS + 1];

    for (;;) {
        copyStatus(st, sharedStatus);

        // Line 1: system state
        snprintf(line, sizeof(line), "%-19s", systemStateName(st.state));
        lcd.setCursor(0, 0);
        lcd.print(line);

        // Line 2: door state + last user
        snprintf(line, sizeof(line), "E:%s X:%s",
                 st.entranceLocked ? "L" : "U",
                 st.exitLocked ? "L" : "U");
        if (st.lastUserUid[0]) {
            char tmp[12];
            snprintf(tmp, sizeof(tmp), " %s", st.lastUserUid);
            strncat(line, tmp, sizeof(line) - strlen(line) - 1);
        }
        padLine(line, LCD_COLS + 1);
        lcd.setCursor(0, 1);
        lcd.print(line);

        // Line 3: last product UID
        if (st.lastProductUid[0]) {
            snprintf(line, sizeof(line), "Prod:%s", st.lastProductUid);
        } else if (st.state == STATE_WAITING_FOR_PRODUCT) {
            snprintf(line, sizeof(line), "Place prod, scan");
        } else if (st.state == STATE_IDLE) {
            snprintf(line, sizeof(line), "System Ready");
        } else if (st.state == STATE_UV_ACTIVE) {
            snprintf(line, sizeof(line), "UV: %lu sec left", (unsigned long)st.uvTimeRemainingSec);
        } else if (st.state == STATE_UV_DONE) {
            snprintf(line, sizeof(line), "Cycle complete,exit");
        } else {
            snprintf(line, sizeof(line), " ");
        }
        padLine(line, LCD_COLS + 1);
        lcd.setCursor(0, 2);
        lcd.print(line);

        // Line 4: AP status or UV countdown on line 3 duplicate for UV
        if (st.webServerActive && st.apIp[0]) {
            snprintf(line, sizeof(line), "AP: %s", st.apIp);
        } else if (st.state == STATE_UV_ACTIVE) {
            snprintf(line, sizeof(line), "UV active...");
        } else if (st.webServerActive) {
            snprintf(line, sizeof(line), "Web server ON");
        } else {
            snprintf(line, sizeof(line), "AP off");
        }
        padLine(line, LCD_COLS + 1);
        lcd.setCursor(0, 3);
        lcd.print(line);

        vTaskDelay(pdMS_TO_TICKS(LCD_UPDATE_INTERVAL_MS));
    }
}

void lcdInit() {
    statusMutex = xSemaphoreCreateMutex();
    memset(&sharedStatus, 0, sizeof(sharedStatus));
    sharedStatus.entranceLocked = true;
    sharedStatus.exitLocked = true;

    Wire.begin(PIN_LCD_SDA, PIN_LCD_SCL);
    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("UV Sterilizer");
    lcd.setCursor(0, 1);
    lcd.print("Initializing...");
}

void lcdStartTask() {
    xTaskCreatePinnedToCore(
        lcdTask, "lcdTask",
        TASK_STACK_LCD, nullptr,
        TASK_PRIO_LCD, nullptr,
        CORE_LCD_BUZZER
    );
}

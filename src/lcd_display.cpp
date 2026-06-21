#include "lcd_display.h"
#include "config.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);
static SemaphoreHandle_t statusMutex = nullptr;
static SystemStatus sharedStatus = {};

static void copyStatus(SystemStatus& dest) {
    if (statusMutex && xSemaphoreTake(statusMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        dest = sharedStatus;
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

static const char* userLabel(const SystemStatus& st) {
    if (st.lastUserName[0]) return st.lastUserName;
    if (st.lastUserUid[0]) return st.lastUserUid;
    return nullptr;
}

static const char* productLabel(const SystemStatus& st) {
    if (st.lastProductName[0]) return st.lastProductName;
    if (st.lastProductUid[0]) return st.lastProductUid;
    return nullptr;
}

static uint32_t timeoutSec(const SystemStatus& st) {
    return (st.stateTimeoutRemainingMs + 999) / 1000;
}

static void lcdTask(void* param) {
    (void)param;
    SystemStatus st = {};
    char line[LCD_COLS + 1];

    for (;;) {
        copyStatus(st);

        // Line 1: state + countdown
        switch (st.state) {
            case STATE_IDLE:
                if (!st.entranceLocked && st.exitLocked) {
                    snprintf(line, sizeof(line), "Exit: %lus open",
                             (unsigned long)timeoutSec(st));
                } else {
                    snprintf(line, sizeof(line), "System Ready");
                }
                break;
            case STATE_DOOR_ENTRY:
                snprintf(line, sizeof(line), "Entry: %lus open",
                         (unsigned long)timeoutSec(st));
                break;
            case STATE_WAITING_FOR_PRODUCT:
                if (st.stateTimeoutRemainingMs > 0) {
                    snprintf(line, sizeof(line), "Product: %lus left",
                             (unsigned long)timeoutSec(st));
                } else {
                    snprintf(line, sizeof(line), "Scan product tag");
                }
                break;
            case STATE_UV_ACTIVE:
                snprintf(line, sizeof(line), "UV: %lus remaining",
                         (unsigned long)st.uvTimeRemainingSec);
                break;
            case STATE_UV_DONE:
                snprintf(line, sizeof(line), "Exit: %lus open",
                         (unsigned long)timeoutSec(st));
                break;
            default:
                snprintf(line, sizeof(line), "%s", systemStateName(st.state));
                break;
        }
        padLine(line, LCD_COLS + 1);
        lcd.setCursor(0, 0);
        lcd.print(line);

        // Line 2: user name
        const char* user = userLabel(st);
        if (user) {
            snprintf(line, sizeof(line), "User: %.13s", user);
        } else {
            snprintf(line, sizeof(line), "E:%s X:%s",
                     st.entranceLocked ? "Lock" : "Open",
                     st.exitLocked ? "Lock" : "Open");
        }
        padLine(line, LCD_COLS + 1);
        lcd.setCursor(0, 1);
        lcd.print(line);

        // Line 3: product name or prompt
        const char* prod = productLabel(st);
        if (prod) {
            snprintf(line, sizeof(line), "Prod: %.13s", prod);
        } else if (st.state == STATE_WAITING_FOR_PRODUCT) {
            snprintf(line, sizeof(line), "User tag=cancel");
        } else if (st.state == STATE_UV_DONE) {
            snprintf(line, sizeof(line), "Scan user when done");
        } else {
            snprintf(line, sizeof(line), " ");
        }
        padLine(line, LCD_COLS + 1);
        lcd.setCursor(0, 2);
        lcd.print(line);

        // Line 4: AP status or active countdown
        if (st.webServerActive && st.apIp[0]) {
            snprintf(line, sizeof(line), "AP: %s", st.apIp);
        } else if (st.state == STATE_UV_ACTIVE) {
            snprintf(line, sizeof(line), "UV lamp ON %lus",
                     (unsigned long)st.uvTimeRemainingSec);
        } else if (st.state == STATE_DOOR_ENTRY) {
            snprintf(line, sizeof(line), "Scan again=cancel");
        } else if (st.state == STATE_UV_DONE) {
            snprintf(line, sizeof(line), "Exit closes %lus",
                     (unsigned long)timeoutSec(st));
        } else if (st.webServerActive) {
            snprintf(line, sizeof(line), "Web server ON");
        } else {
            snprintf(line, sizeof(line), "BOOT/NFC=admin");
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

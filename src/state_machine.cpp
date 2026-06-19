#include "state_machine.h"
#include "config.h"
#include "storage.h"
#include "buzzer.h"
#include "lcd_display.h"
#include "rfid_manager.h"

static SystemStatus currentStatus = {};
static SemaphoreHandle_t statusMutex = nullptr;
static uint32_t stateEnterMs = 0;
static uint32_t uvEndMs = 0;

// Relay helpers – active HIGH
static void setEntranceLock(bool locked) {
    digitalWrite(PIN_ENTRANCE_LOCK, locked ? LOCK_ACTIVE : LOCK_INACTIVE);
    currentStatus.entranceLocked = locked;
}

static void setExitLock(bool locked) {
    digitalWrite(PIN_EXIT_LOCK, locked ? LOCK_ACTIVE : LOCK_INACTIVE);
    currentStatus.exitLocked = locked;
}

static void setUvLamp(bool on) {
    digitalWrite(PIN_UV_RELAY, on ? UV_ACTIVE : UV_INACTIVE);
    currentStatus.uvLampOn = on;
}

static void lockBothDoors() {
    setEntranceLock(true);
    setExitLock(true);
}

static void unlockBothDoors() {
    setEntranceLock(false);
    setExitLock(false);
}

static void publishStatus() {
    currentStatus.entranceRfidOk = rfidEntranceHealthy();
    currentStatus.insideRfidOk = rfidInsideHealthy();
    if (statusMutex && xSemaphoreTake(statusMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        lcdUpdateSharedStatus(currentStatus);
        xSemaphoreGive(statusMutex);
    }
}

static void transitionTo(SystemState newState) {
    currentStatus.state = newState;
    stateEnterMs = millis();
    Serial.printf("[State] -> %s\n", systemStateName(newState));

    switch (newState) {
        case STATE_IDLE:
            lockBothDoors();
            setUvLamp(false);
            currentStatus.stateTimeoutRemainingMs = 0;
            currentStatus.uvTimeRemainingSec = 0;
            break;

        case STATE_DOOR_ENTRY:
            setEntranceLock(false);
            setExitLock(true);
            currentStatus.stateTimeoutRemainingMs = ENTRY_TIMEOUT_MS;
            buzzerRequest(BEEP_SINGLE_SHORT);
            break;

        case STATE_WAITING_FOR_PRODUCT:
            lockBothDoors();
            currentStatus.stateTimeoutRemainingMs = 0;
            break;

        case STATE_UV_ACTIVE: {
            lockBothDoors();
            uint32_t durationSec = storage.getUvDurationSec();
            currentStatus.uvTimeRemainingSec = durationSec;
            uvEndMs = millis() + (durationSec * 1000UL);
            setUvLamp(true);
            buzzerRequest(BEEP_LONG);
            break;
        }

        case STATE_UV_DONE:
            unlockBothDoors();
            setUvLamp(false);
            currentStatus.stateTimeoutRemainingMs = EXIT_TIMEOUT_MS;
            buzzerRequest(BEEP_DOUBLE_SHORT);
            break;
    }
    publishStatus();
}

static void handleTagEvent(const TagEvent& evt) {
    SystemState state = currentStatus.state;

  switch (state) {
        case STATE_IDLE:
            if (evt.source == TAG_SOURCE_ENTRANCE && evt.isUser) {
                strncpy(currentStatus.lastUserUid, evt.uid, sizeof(currentStatus.lastUserUid) - 1);
                transitionTo(STATE_DOOR_ENTRY);
            } else if (evt.source == TAG_SOURCE_ENTRANCE) {
                buzzerRequest(BEEP_ERROR);
            }
            break;

        case STATE_DOOR_ENTRY:
            if (evt.source == TAG_SOURCE_ENTRANCE && evt.isUser) {
                // Re-auth extends entry window
                strncpy(currentStatus.lastUserUid, evt.uid, sizeof(currentStatus.lastUserUid) - 1);
                stateEnterMs = millis();
                currentStatus.stateTimeoutRemainingMs = ENTRY_TIMEOUT_MS;
                buzzerRequest(BEEP_SINGLE_SHORT);
                publishStatus();
            } else if (evt.source == TAG_SOURCE_INSIDE && evt.isProduct) {
                // Ignore product scans while entrance is unlocked
                Serial.println("[State] Product scan ignored during DOOR_ENTRY");
            } else {
                buzzerRequest(BEEP_ERROR);
            }
            break;

        case STATE_WAITING_FOR_PRODUCT:
            if (evt.source == TAG_SOURCE_INSIDE && evt.isProduct) {
                strncpy(currentStatus.lastProductUid, evt.uid, sizeof(currentStatus.lastProductUid) - 1);
                transitionTo(STATE_UV_ACTIVE);
            } else if (evt.source == TAG_SOURCE_INSIDE) {
                buzzerRequest(BEEP_ERROR);
            }
            break;

        case STATE_UV_ACTIVE:
            // Ignore all tags during UV
            break;

        case STATE_UV_DONE:
            // Entrance reader ignored; no tag handling
            break;
    }
}

static void updateTimeouts() {
    uint32_t now = millis();

    switch (currentStatus.state) {
        case STATE_DOOR_ENTRY:
            if (now - stateEnterMs >= ENTRY_TIMEOUT_MS) {
                transitionTo(STATE_WAITING_FOR_PRODUCT);
            } else {
                currentStatus.stateTimeoutRemainingMs = ENTRY_TIMEOUT_MS - (now - stateEnterMs);
            }
            break;

        case STATE_UV_ACTIVE:
            if (now >= uvEndMs) {
                transitionTo(STATE_UV_DONE);
            } else {
                currentStatus.uvTimeRemainingSec = (uvEndMs - now + 999) / 1000;
            }
            break;

        case STATE_UV_DONE:
            if (now - stateEnterMs >= EXIT_TIMEOUT_MS) {
                transitionTo(STATE_IDLE);
            } else {
                currentStatus.stateTimeoutRemainingMs = EXIT_TIMEOUT_MS - (now - stateEnterMs);
            }
            break;

        default:
            break;
    }
}

static void stateMachineTask(void* param) {
    (void)param;
    QueueHandle_t tagQueue = rfidGetTagQueue();
    TagEvent evt;

    for (;;) {
        // Non-blocking tag processing
        while (tagQueue && xQueueReceive(tagQueue, &evt, 0) == pdTRUE) {
            handleTagEvent(evt);
        }

        updateTimeouts();
        publishStatus();

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void stateMachineInit() {
    statusMutex = xSemaphoreCreateMutex();

    pinMode(PIN_ENTRANCE_LOCK, OUTPUT);
    pinMode(PIN_EXIT_LOCK, OUTPUT);
    pinMode(PIN_UV_RELAY, OUTPUT);

    memset(&currentStatus, 0, sizeof(currentStatus));
    currentStatus.state = STATE_IDLE;
    currentStatus.entranceLocked = true;
    currentStatus.exitLocked = true;
    currentStatus.uvLampOn = false;
    strncpy(currentStatus.apIp, "", sizeof(currentStatus.apIp));

    lockBothDoors();
    setUvLamp(false);
    stateEnterMs = millis();
    publishStatus();
}

void stateMachineStartTask() {
    xTaskCreatePinnedToCore(
        stateMachineTask, "stateTask",
        TASK_STACK_STATE, nullptr,
        TASK_PRIO_STATE, nullptr,
        CORE_RFID_WEB
    );
}

SystemState stateMachineGetState() {
    return currentStatus.state;
}

void stateMachineGetStatus(SystemStatus& out) {
    if (statusMutex && xSemaphoreTake(statusMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        out = currentStatus;
        xSemaphoreGive(statusMutex);
    } else {
        out = currentStatus;
    }
}

bool stateMachineIsWebServerAllowed() {
    // Web server can run in any state; UV cycle continues independently
    return true;
}

// Allow web server to update AP info in shared status
void stateMachineSetWebInfo(bool active, const char* ip) {
    currentStatus.webServerActive = active;
    if (ip) {
        strncpy(currentStatus.apIp, ip, sizeof(currentStatus.apIp) - 1);
        currentStatus.apIp[sizeof(currentStatus.apIp) - 1] = '\0';
    } else {
        currentStatus.apIp[0] = '\0';
    }
    publishStatus();
}

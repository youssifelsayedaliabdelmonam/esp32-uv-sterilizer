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
static char doorEntryLastUid[24] = {};
static uint32_t doorEntryLastScanMs = 0;
static bool idleExitAssist = false;  // IDLE with entrance open so user can leave room

static bool lockStatus(TickType_t timeout = pdMS_TO_TICKS(50)) {
    return statusMutex && xSemaphoreTake(statusMutex, timeout) == pdTRUE;
}

static void unlockStatus() {
    if (statusMutex) xSemaphoreGive(statusMutex);
}

// Relay helpers – caller must hold statusMutex when updating currentStatus
static void driveRelayPin(uint8_t pin, uint8_t level, const char* label) {
    digitalWrite(pin, level);
    Serial.printf("[Relay] %s GPIO%d -> %s\n", label, pin, level == HIGH ? "HIGH" : "LOW");
}

static void setEntranceLock(bool locked) {
    driveRelayPin(PIN_ENTRANCE_LOCK, locked ? LOCK_ACTIVE : LOCK_INACTIVE,
                  locked ? "Entrance LOCK" : "Entrance UNLOCK");
    currentStatus.entranceLocked = locked;
}

static void setExitLock(bool locked) {
    driveRelayPin(PIN_EXIT_LOCK, locked ? LOCK_ACTIVE : LOCK_INACTIVE,
                  locked ? "Exit LOCK" : "Exit UNLOCK");
    currentStatus.exitLocked = locked;
}

static void setUvLamp(bool on) {
    driveRelayPin(PIN_UV_RELAY, on ? UV_ACTIVE : UV_INACTIVE,
                  on ? "UV ON" : "UV OFF");
    currentStatus.uvLampOn = on;
}

static void lockBothDoors() {
    setEntranceLock(true);
    setExitLock(true);
}

// Exit door only – entrance stays locked (UV_DONE / post-cycle exit)
static void openExitDoorOnly() {
    setEntranceLock(true);
    setExitLock(false);
}

// Call as the first action in setup() – all coils OFF before anything else
void relaysSafeBootInit() {
    pinMode(PIN_ENTRANCE_LOCK, OUTPUT);
    pinMode(PIN_EXIT_LOCK, OUTPUT);
    pinMode(PIN_UV_RELAY, OUTPUT);
    digitalWrite(PIN_ENTRANCE_LOCK, RELAY_COIL_OFF);
    digitalWrite(PIN_EXIT_LOCK, RELAY_COIL_OFF);
    digitalWrite(PIN_UV_RELAY, RELAY_COIL_OFF);
    Serial.printf("[Relay] Boot safe: coils OFF on GPIO %d, %d, %d\n",
                  PIN_ENTRANCE_LOCK, PIN_EXIT_LOCK, PIN_UV_RELAY);
}

static void publishStatus() {
    SystemStatus snapshot;
    if (!lockStatus()) return;
    currentStatus.entranceRfidOk = rfidEntranceHealthy();
    currentStatus.insideRfidOk = rfidInsideHealthy();
    snapshot = currentStatus;
    unlockStatus();
    lcdUpdateSharedStatus(snapshot);
}

// Caller must hold statusMutex
static void clearCycleSession() {
    currentStatus.lastUserUid[0] = '\0';
    currentStatus.lastProductUid[0] = '\0';
    currentStatus.lastUserName[0] = '\0';
    currentStatus.lastProductName[0] = '\0';
}

// Caller must hold statusMutex
static void transitionTo(SystemState newState) {
    SystemState fromState = currentStatus.state;
    currentStatus.state = newState;
    stateEnterMs = millis();
    Serial.printf("[State] %s -> %s\n", systemStateName(fromState), systemStateName(newState));

    switch (newState) {
        case STATE_IDLE:
            lockBothDoors();
            setUvLamp(false);
            clearCycleSession();
            currentStatus.stateTimeoutRemainingMs = 0;
            currentStatus.uvTimeRemainingSec = 0;
            if (fromState == STATE_UV_DONE || fromState == STATE_WAITING_FOR_PRODUCT) {
                buzzerRequest(BEEP_SINGLE_SHORT);
                Serial.println("[State] Ready for next entry – scan user tag at entrance");
            }
            doorEntryLastUid[0] = '\0';
            idleExitAssist = false;
            break;

        case STATE_DOOR_ENTRY:
            if (currentStatus.lastUserUid[0] != '\0') {
                strncpy(doorEntryLastUid, currentStatus.lastUserUid, sizeof(doorEntryLastUid) - 1);
                doorEntryLastUid[sizeof(doorEntryLastUid) - 1] = '\0';
                doorEntryLastScanMs = stateEnterMs;
            }
            setEntranceLock(false);
            setExitLock(true);
            currentStatus.stateTimeoutRemainingMs = ENTRY_TIMEOUT_MS;
            buzzerRequest(BEEP_SINGLE_SHORT);
            break;

        case STATE_WAITING_FOR_PRODUCT:
            lockBothDoors();
            currentStatus.stateTimeoutRemainingMs = PRODUCT_WAIT_TIMEOUT_MS;
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
            openExitDoorOnly();
            setUvLamp(false);
            currentStatus.stateTimeoutRemainingMs = EXIT_TIMEOUT_MS;
            buzzerRequest(BEEP_DOUBLE_SHORT);
            storage.queueCycleLog(currentStatus.lastUserUid, currentStatus.lastProductUid);
            break;
    }
    rfidResetScanCooldown();
}

// Caller must hold statusMutex – cancel cycle and open entrance so occupant can exit
static void cancelCycleToIdle(const char* reason) {
    SystemState fromState = currentStatus.state;
    currentStatus.state = STATE_IDLE;
    stateEnterMs = millis();
    Serial.printf("[State] %s -> IDLE (exit assist)\n", systemStateName(fromState));

    clearCycleSession();
    setUvLamp(false);
    doorEntryLastUid[0] = '\0';
    idleExitAssist = true;
    setEntranceLock(false);
    setExitLock(true);
    currentStatus.uvTimeRemainingSec = 0;
    currentStatus.stateTimeoutRemainingMs = ENTRY_TIMEOUT_MS;
    buzzerRequest(BEEP_SINGLE_SHORT);
    Serial.printf("[State] %s – entrance open to exit\n", reason);
    rfidResetScanCooldown();
}

// Caller must hold statusMutex
static void setLastUser(const char* uid) {
    strncpy(currentStatus.lastUserUid, uid, sizeof(currentStatus.lastUserUid) - 1);
    currentStatus.lastUserUid[sizeof(currentStatus.lastUserUid) - 1] = '\0';
    String name;
    if (storage.isUserTag(String(uid), &name) && name.length() > 0) {
        strncpy(currentStatus.lastUserName, name.c_str(), sizeof(currentStatus.lastUserName) - 1);
    } else {
        currentStatus.lastUserName[0] = '\0';
    }
    currentStatus.lastUserName[sizeof(currentStatus.lastUserName) - 1] = '\0';
}

static void setLastProduct(const char* uid) {
    strncpy(currentStatus.lastProductUid, uid, sizeof(currentStatus.lastProductUid) - 1);
    currentStatus.lastProductUid[sizeof(currentStatus.lastProductUid) - 1] = '\0';
    String name;
    if (storage.isProductTag(String(uid), &name) && name.length() > 0) {
        strncpy(currentStatus.lastProductName, name.c_str(), sizeof(currentStatus.lastProductName) - 1);
    } else {
        currentStatus.lastProductName[0] = '\0';
    }
    currentStatus.lastProductName[sizeof(currentStatus.lastProductName) - 1] = '\0';
}

// Caller must hold statusMutex
static void handleTagEvent(const TagEvent& evt) {
    switch (currentStatus.state) {
        case STATE_IDLE:
            if (evt.isUser &&
                (evt.source == TAG_SOURCE_ENTRANCE ||
                 (idleExitAssist && evt.source == TAG_SOURCE_INSIDE))) {
                idleExitAssist = false;
                setLastUser(evt.uid);
                transitionTo(STATE_DOOR_ENTRY);
            } else if (evt.source == TAG_SOURCE_ENTRANCE ||
                       (idleExitAssist && evt.source == TAG_SOURCE_INSIDE)) {
                buzzerRequest(BEEP_ERROR);
            }
            break;

        case STATE_DOOR_ENTRY:
            if (evt.source == TAG_SOURCE_ENTRANCE && evt.isUser) {
                uint32_t now = millis();
                uint32_t sinceLast = now - doorEntryLastScanMs;
                if (doorEntryLastUid[0] != '\0' &&
                    strcmp(doorEntryLastUid, evt.uid) == 0 &&
                    sinceLast >= ENTRY_CANCEL_MIN_GAP_MS &&
                    sinceLast < ENTRY_CANCEL_DOUBLE_SCAN_MS) {
                    doorEntryLastUid[0] = '\0';
                    transitionTo(STATE_IDLE);
                    Serial.println("[State] Entry cancelled by user tag");
                    break;
                }
                strncpy(doorEntryLastUid, evt.uid, sizeof(doorEntryLastUid) - 1);
                doorEntryLastUid[sizeof(doorEntryLastUid) - 1] = '\0';
                doorEntryLastScanMs = now;
                setLastUser(evt.uid);
                stateEnterMs = now;
                currentStatus.stateTimeoutRemainingMs = ENTRY_TIMEOUT_MS;
                buzzerRequest(BEEP_SINGLE_SHORT);
            } else if (evt.source == TAG_SOURCE_INSIDE && evt.isProduct) {
                Serial.println("[State] Product scan ignored during DOOR_ENTRY");
            } else {
                buzzerRequest(BEEP_ERROR);
            }
            break;

        case STATE_WAITING_FOR_PRODUCT:
            if (evt.isUser &&
                (evt.source == TAG_SOURCE_ENTRANCE || evt.source == TAG_SOURCE_INSIDE)) {
                cancelCycleToIdle("Cycle cancelled");
            } else if (evt.source == TAG_SOURCE_INSIDE && evt.isProduct) {
                setLastProduct(evt.uid);
                transitionTo(STATE_UV_ACTIVE);
            } else if (evt.source == TAG_SOURCE_INSIDE) {
                buzzerRequest(BEEP_ERROR);
            }
            break;

        case STATE_UV_ACTIVE:
            break;

        case STATE_UV_DONE:
            if (evt.isUser &&
                (evt.source == TAG_SOURCE_ENTRANCE || evt.source == TAG_SOURCE_INSIDE)) {
                cancelCycleToIdle("Exit complete");
            }
            break;
    }
}

// Caller must hold statusMutex
static void updateTimeouts() {
    uint32_t now = millis();

    switch (currentStatus.state) {
        case STATE_DOOR_ENTRY:
            if ((int32_t)(now - stateEnterMs) >= (int32_t)ENTRY_TIMEOUT_MS) {
                transitionTo(STATE_WAITING_FOR_PRODUCT);
            } else {
                currentStatus.stateTimeoutRemainingMs = ENTRY_TIMEOUT_MS - (now - stateEnterMs);
            }
            break;

        case STATE_WAITING_FOR_PRODUCT:
            if (PRODUCT_WAIT_TIMEOUT_MS > 0) {
                if ((int32_t)(now - stateEnterMs) >= (int32_t)PRODUCT_WAIT_TIMEOUT_MS) {
                    cancelCycleToIdle("Product wait timeout");
                } else {
                    currentStatus.stateTimeoutRemainingMs =
                        PRODUCT_WAIT_TIMEOUT_MS - (now - stateEnterMs);
                }
            }
            break;

        case STATE_UV_ACTIVE:
            if ((int32_t)(now - uvEndMs) >= 0) {
                transitionTo(STATE_UV_DONE);
            } else {
                currentStatus.uvTimeRemainingSec = (uvEndMs - now + 999) / 1000;
            }
            break;

        case STATE_UV_DONE:
            if ((int32_t)(now - stateEnterMs) >= (int32_t)EXIT_TIMEOUT_MS) {
                transitionTo(STATE_IDLE);
                Serial.println("[State] Exit window ended – returning to idle");
            } else {
                currentStatus.stateTimeoutRemainingMs = EXIT_TIMEOUT_MS - (now - stateEnterMs);
            }
            break;

        case STATE_IDLE:
                if (idleExitAssist) {
                if ((int32_t)(now - stateEnterMs) >= (int32_t)ENTRY_TIMEOUT_MS) {
                    idleExitAssist = false;
                    setEntranceLock(true);
                    currentStatus.stateTimeoutRemainingMs = 0;
                    rfidResetScanCooldown();
                    Serial.println("[State] Exit assist ended – fully idle");
                } else {
                    currentStatus.stateTimeoutRemainingMs =
                        ENTRY_TIMEOUT_MS - (now - stateEnterMs);
                }
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
        while (tagQueue && xQueueReceive(tagQueue, &evt, 0) == pdTRUE) {
            if (lockStatus(portMAX_DELAY)) {
                handleTagEvent(evt);
                unlockStatus();
                publishStatus();
            }
        }

        if (lockStatus(portMAX_DELAY)) {
            updateTimeouts();
            unlockStatus();
        }

        storage.processLogQueue();
        publishStatus();

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void stateMachineInit() {
    statusMutex = xSemaphoreCreateMutex();

    // Relays already safe from relaysSafeBootInit(); enforce locked + UV off
    memset(&currentStatus, 0, sizeof(currentStatus));
    currentStatus.state = STATE_IDLE;
    currentStatus.entranceLocked = true;
    currentStatus.exitLocked = true;
    currentStatus.uvLampOn = false;
    currentStatus.apIp[0] = '\0';

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
    if (lockStatus(pdMS_TO_TICKS(20))) {
        SystemState s = currentStatus.state;
        unlockStatus();
        return s;
    }
    return currentStatus.state;
}

void stateMachineGetStatus(SystemStatus& out) {
    if (lockStatus(pdMS_TO_TICKS(100))) {
        out = currentStatus;
        unlockStatus();
    } else {
        out = currentStatus;
    }
}

bool stateMachineIsIdleExitAssist() {
    if (lockStatus(pdMS_TO_TICKS(20))) {
        bool assist = idleExitAssist;
        unlockStatus();
        return assist;
    }
    return idleExitAssist;
}

bool stateMachineIsWebServerAllowed() {
    return true;
}

void stateMachineSetWebInfo(bool active, const char* ip) {
    if (lockStatus(pdMS_TO_TICKS(100))) {
        currentStatus.webServerActive = active;
        if (ip) {
            strncpy(currentStatus.apIp, ip, sizeof(currentStatus.apIp) - 1);
            currentStatus.apIp[sizeof(currentStatus.apIp) - 1] = '\0';
        } else {
            currentStatus.apIp[0] = '\0';
        }
        unlockStatus();
    }
    publishStatus();
}

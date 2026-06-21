#include "rfid_manager.h"
#include "config.h"
#include "storage.h"
#include "state_machine.h"
#include "web_server.h"
#include "buzzer.h"
#include <SPI.h>

// Slow SPI clock for reliable dual-MFRC522 on ESP32 (must precede MFRC522 include)
#ifndef MFRC522_SPICLOCK
#define MFRC522_SPICLOCK RFID_SPI_SPEED_HZ
#endif
#include <MFRC522.h>

static MFRC522 rfidEntrance(PIN_ENTRANCE_RFID_SS, PIN_ENTRANCE_RFID_RST);
static MFRC522 rfidInside(PIN_INSIDE_RFID_SS, PIN_INSIDE_RFID_RST);

static QueueHandle_t tagEventQueue = nullptr;
static SemaphoreHandle_t spiMutex = nullptr;

static volatile bool enrollmentActive = false;
static volatile bool enrollmentDone = false;
static char enrollmentUid[24] = {};
static SemaphoreHandle_t enrollMutex = nullptr;

static volatile bool entranceHealthy = false;
static volatile bool insideHealthy = false;
static volatile uint8_t entranceVersion = 0;
static volatile uint8_t insideVersion = 0;
static uint8_t entranceFailCount = 0;
static uint8_t insideFailCount = 0;
static uint32_t lastEntranceReinit = 0;
static uint32_t lastInsideReinit = 0;
static uint32_t lastHealthCheckMs = 0;

// Any response other than bus-floating values means the chip is responding
static bool isValidMfrc522Version(byte version) {
    return version != 0x00 && version != 0xFF;
}

static bool lockSpi(TickType_t timeout = pdMS_TO_TICKS(200)) {
    return spiMutex && xSemaphoreTake(spiMutex, timeout) == pdTRUE;
}

static void unlockSpi() {
    if (spiMutex) xSemaphoreGive(spiMutex);
}

static void deselectAllReaders() {
    pinMode(PIN_ENTRANCE_RFID_SS, OUTPUT);
    pinMode(PIN_INSIDE_RFID_SS, OUTPUT);
    digitalWrite(PIN_ENTRANCE_RFID_SS, HIGH);
    digitalWrite(PIN_INSIDE_RFID_SS, HIGH);
}

static void formatUid(const MFRC522::Uid& uid, char* out, size_t outLen) {
    out[0] = '\0';
    if (uid.size == 0 || outLen < 4) return;
    char* p = out;
    size_t remaining = outLen;
    for (byte i = 0; i < uid.size; i++) {
        int written = snprintf(p, remaining, "%s%02X", (i > 0) ? ":" : "", uid.uidByte[i]);
        if (written < 0 || (size_t)written >= remaining) break;
        p += written;
        remaining -= written;
    }
}

// Read VersionReg with other reader deselected
static byte readReaderVersion(MFRC522& reader) {
    deselectAllReaders();
    delayMicroseconds(50);
    return reader.PCD_ReadRegister(MFRC522::VersionReg);
}

static bool initSingleReader(MFRC522& reader, const char* name,
                             volatile bool& healthyFlag, volatile uint8_t& versionOut) {
    deselectAllReaders();
    reader.PCD_Init();
    delay(RFID_INIT_DELAY_MS);

    for (int attempt = 0; attempt < RFID_INIT_RETRY_COUNT; attempt++) {
        byte version = readReaderVersion(reader);
        versionOut = version;
        if (isValidMfrc522Version(version)) {
            healthyFlag = true;
            Serial.printf("[RFID] %s init OK – version 0x%02X (attempt %d)\n",
                          name, version, attempt + 1);
            return true;
        }
        Serial.printf("[RFID] %s init retry %d – version 0x%02X\n",
                      name, attempt + 1, version);
        reader.PCD_Init();
        delay(RFID_INIT_DELAY_MS);
    }

    healthyFlag = false;
    Serial.printf("[RFID] %s init FAILED – last version 0x%02X\n", name, (byte)versionOut);
    return false;
}

static bool probeReaderHealth(MFRC522& reader, volatile bool& healthyFlag,
                              volatile uint8_t& versionOut, uint8_t& failCount,
                              uint32_t& lastReinit, const char* name) {
    byte version = readReaderVersion(reader);
    versionOut = version;

    if (isValidMfrc522Version(version)) {
        failCount = 0;
        healthyFlag = true;
        return true;
    }

    failCount++;
    Serial.printf("[RFID] %s probe 0x%02X (fail %u/%u)\n",
                  name, version, failCount, RFID_FAIL_THRESHOLD);

    if (failCount >= RFID_FAIL_THRESHOLD) {
        healthyFlag = false;
        uint32_t now = millis();
        if (now - lastReinit >= RFID_REINIT_COOLDOWN_MS) {
            Serial.printf("[RFID] Re-init %s\n", name);
            deselectAllReaders();
            reader.PCD_Init();
            lastReinit = now;
            vTaskDelay(pdMS_TO_TICKS(RFID_INIT_DELAY_MS));
            version = readReaderVersion(reader);
            versionOut = version;
            if (isValidMfrc522Version(version)) {
                failCount = 0;
                healthyFlag = true;
                Serial.printf("[RFID] %s recovered – version 0x%02X\n", name, version);
                return true;
            }
        }
    }
    return false;
}

static bool readTagFromReader(MFRC522& reader, char* uidOut, size_t uidLen) {
    deselectAllReaders();
    delayMicroseconds(50);
    if (!reader.PICC_IsNewCardPresent()) return false;
    if (!reader.PICC_ReadCardSerial()) return false;
    formatUid(reader.uid, uidOut, uidLen);
    reader.PICC_HaltA();
    reader.PCD_StopCrypto1();
    return uidOut[0] != '\0';
}

static void classifyTag(TagEvent& evt) {
    evt.recognized = false;
    evt.isUser = false;
    evt.isProduct = false;

    if (storage.isUserTag(String(evt.uid))) {
        evt.recognized = true;
        evt.isUser = true;
    } else if (storage.isProductTag(String(evt.uid))) {
        evt.recognized = true;
        evt.isProduct = true;
    }
}

static void runPeriodicHealthCheck() {
    uint32_t now = millis();
    if (now - lastHealthCheckMs < RFID_HEALTH_CHECK_INTERVAL_MS) return;
    lastHealthCheckMs = now;

    if (!lockSpi()) return;
    probeReaderHealth(rfidEntrance, entranceHealthy, entranceVersion,
                      entranceFailCount, lastEntranceReinit, "entrance");
#if INSIDE_RFID_ENABLED
    probeReaderHealth(rfidInside, insideHealthy, insideVersion,
                      insideFailCount, lastInsideReinit, "inside");
#endif
    unlockSpi();
}

static void rfidTask(void* param) {
    (void)param;
    char uid[24];
    TagEvent evt;

    // Run first health check immediately (don't wait 3 s after boot)
    lastHealthCheckMs = 0;
    runPeriodicHealthCheck();

    for (;;) {
        memset(&evt, 0, sizeof(evt));

        bool enrolling = false;
        if (enrollMutex && xSemaphoreTake(enrollMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            enrolling = enrollmentActive;
            xSemaphoreGive(enrollMutex);
        }

        runPeriodicHealthCheck();

        if (!lockSpi()) {
            vTaskDelay(pdMS_TO_TICKS(RFID_POLL_INTERVAL_MS));
            continue;
        }

        if (enrolling) {
            bool gotEnrollTag = false;
            if (entranceHealthy && readTagFromReader(rfidEntrance, uid, sizeof(uid))) {
                strncpy(evt.uid, uid, sizeof(evt.uid) - 1);
                evt.uid[sizeof(evt.uid) - 1] = '\0';
                gotEnrollTag = true;
            }
            unlockSpi();

            if (gotEnrollTag && xSemaphoreTake(enrollMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                strncpy(enrollmentUid, evt.uid, sizeof(enrollmentUid) - 1);
                enrollmentUid[sizeof(enrollmentUid) - 1] = '\0';
                enrollmentDone = true;
                enrollmentActive = false;
                xSemaphoreGive(enrollMutex);
                Serial.printf("[RFID] Enrollment tag: %s\n", evt.uid);
            }
            vTaskDelay(pdMS_TO_TICKS(RFID_POLL_INTERVAL_MS));
            continue;
        }

        SystemState state = stateMachineGetState();
        bool pollEntranceCycle = (state == STATE_IDLE || state == STATE_DOOR_ENTRY);
        bool pollEntranceUserAbort = (state == STATE_WAITING_FOR_PRODUCT || state == STATE_UV_DONE);
        bool pollInside = (state == STATE_WAITING_FOR_PRODUCT);
        bool gotEntranceTag = false;

        if (entranceHealthy && readTagFromReader(rfidEntrance, uid, sizeof(uid))) {
            strncpy(evt.uid, uid, sizeof(evt.uid) - 1);
            evt.uid[sizeof(evt.uid) - 1] = '\0';
            evt.source = TAG_SOURCE_ENTRANCE;
            gotEntranceTag = true;

            if (storage.isAdminTag(String(evt.uid))) {
                unlockSpi();
                webServerToggle();
                buzzerRequest(BEEP_DOUBLE_SHORT);
                Serial.printf("[RFID] Admin tag scanned – web UI toggled\n");
                vTaskDelay(pdMS_TO_TICKS(RFID_POLL_INTERVAL_MS));
                continue;
            }
        }

        if (gotEntranceTag) {
            classifyTag(evt);
            bool sendToStateMachine = false;
            if (pollEntranceCycle) {
                sendToStateMachine = true;
            } else if (pollEntranceUserAbort && evt.isUser) {
                sendToStateMachine = true;
            } else if (pollEntranceUserAbort) {
                buzzerRequest(BEEP_ERROR);
            }
            unlockSpi();
            if (sendToStateMachine) {
                xQueueSend(tagEventQueue, &evt, 0);
            }
        } else if (pollInside && insideHealthy) {
            if (readTagFromReader(rfidInside, uid, sizeof(uid))) {
                strncpy(evt.uid, uid, sizeof(evt.uid) - 1);
                evt.uid[sizeof(evt.uid) - 1] = '\0';
                evt.source = TAG_SOURCE_INSIDE;
                classifyTag(evt);
                unlockSpi();
                xQueueSend(tagEventQueue, &evt, 0);
            } else {
                unlockSpi();
            }
        } else {
            unlockSpi();
        }

        vTaskDelay(pdMS_TO_TICKS(RFID_POLL_INTERVAL_MS));
    }
}

void rfidInit() {
    enrollMutex = xSemaphoreCreateMutex();
    spiMutex = xSemaphoreCreateMutex();
    tagEventQueue = xQueueCreate(QUEUE_TAG_EVENTS, sizeof(TagEvent));
    if (!enrollMutex || !spiMutex || !tagEventQueue) {
        Serial.println("[RFID] Failed to create mutex/queue");
    }

    deselectAllReaders();
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
    delay(RFID_INIT_DELAY_MS);

    if (lockSpi(portMAX_DELAY)) {
        initSingleReader(rfidEntrance, "entrance", entranceHealthy, entranceVersion);
#if INSIDE_RFID_ENABLED
        initSingleReader(rfidInside, "inside", insideHealthy, insideVersion);
#else
        insideHealthy = true;
        insideVersion = 0;
        Serial.println("[RFID] Inside reader disabled in config");
#endif
        unlockSpi();
    } else {
        Serial.println("[RFID] SPI mutex unavailable at init");
    }

    lastHealthCheckMs = millis();
}

void rfidStartTask() {
    xTaskCreatePinnedToCore(
        rfidTask, "rfidTask",
        TASK_STACK_RFID, nullptr,
        TASK_PRIO_RFID, nullptr,
        CORE_RFID_WEB
    );
}

QueueHandle_t rfidGetTagQueue() {
    return tagEventQueue;
}

void rfidStartEnrollment(EnrollType type) {
    (void)type;
    if (xSemaphoreTake(enrollMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        enrollmentDone = false;
        enrollmentUid[0] = '\0';
        enrollmentActive = true;
        xSemaphoreGive(enrollMutex);
        Serial.println("[RFID] Enrollment started");
    }
}

bool rfidWaitEnrollmentResult(char* uidOut, size_t uidLen, uint32_t timeoutMs) {
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (xSemaphoreTake(enrollMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            bool done = enrollmentDone;
            if (done) {
                strncpy(uidOut, enrollmentUid, uidLen - 1);
                uidOut[uidLen - 1] = '\0';
                enrollmentDone = false;
                xSemaphoreGive(enrollMutex);
                return uidOut[0] != '\0';
            }
            xSemaphoreGive(enrollMutex);
        }
        if (!enrollmentActive && !enrollmentDone) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    rfidCancelEnrollment();
    return false;
}

void rfidCancelEnrollment() {
    if (xSemaphoreTake(enrollMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        enrollmentActive = false;
        enrollmentDone = false;
        xSemaphoreGive(enrollMutex);
    }
}

bool rfidIsEnrollmentActive() {
    bool active = false;
    if (enrollMutex && xSemaphoreTake(enrollMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        active = enrollmentActive;
        xSemaphoreGive(enrollMutex);
    }
    return active;
}

bool rfidEntranceHealthy() {
    return entranceHealthy;
}

bool rfidInsideHealthy() {
#if INSIDE_RFID_ENABLED
    return insideHealthy;
#else
    return true;
#endif
}

uint8_t rfidEntranceVersion() {
    return entranceVersion;
}

uint8_t rfidInsideVersion() {
    return insideVersion;
}

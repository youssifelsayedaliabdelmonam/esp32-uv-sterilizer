#include "rfid_manager.h"
#include "config.h"
#include "storage.h"
#include "state_machine.h"
#include <SPI.h>
#include <MFRC522.h>

static MFRC522 rfidEntrance(PIN_ENTRANCE_RFID_SS, PIN_ENTRANCE_RFID_RST);
static MFRC522 rfidInside(PIN_INSIDE_RFID_SS, PIN_INSIDE_RFID_RST);

static QueueHandle_t tagEventQueue = nullptr;

// Enrollment state (accessed by web task and rfid task)
static volatile bool enrollmentActive = false;
static volatile EnrollType enrollmentType = ENROLL_USER;
static volatile bool enrollmentDone = false;
static char enrollmentUid[24] = {};
static SemaphoreHandle_t enrollMutex = nullptr;

// Reader health counters (rfid task writes, other tasks read)
static bool entranceHealthy = true;
static bool insideHealthy = true;
static uint8_t entranceFailCount = 0;
static uint8_t insideFailCount = 0;
static uint32_t lastEntranceReinit = 0;
static uint32_t lastInsideReinit = 0;

// Format MFRC522 UID as "AA:BB:CC:DD"
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

static bool checkReaderHealth(MFRC522& reader, bool& healthyFlag, uint8_t& failCount,
                              uint32_t& lastReinit, const char* name) {
    byte version = reader.PCD_ReadRegister(MFRC522::VersionReg);
    // Valid version values: 0x88, 0x90, 0x91, 0x92
    if (version == 0x00 || version == 0xFF) {
        failCount++;
        if (failCount >= RFID_FAIL_THRESHOLD) {
            healthyFlag = false;
            uint32_t now = millis();
            if (now - lastReinit >= RFID_REINIT_COOLDOWN_MS) {
                Serial.printf("[RFID] Re-init %s reader (fail count %u)\n", name, failCount);
                reader.PCD_Init();
                lastReinit = now;
                version = reader.PCD_ReadRegister(MFRC522::VersionReg);
                if (version != 0x00 && version != 0xFF) {
                    failCount = 0;
                    healthyFlag = true;
                }
            }
        }
        return false;
    }
    failCount = 0;
    healthyFlag = true;
    return true;
}

static bool readTagFromReader(MFRC522& reader, char* uidOut, size_t uidLen) {
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

static void rfidTask(void* param) {
    (void)param;
    char uid[24];

    for (;;) {
        // Enrollment mode: only entrance reader, suspend normal auth
        if (enrollmentActive) {
            if (entranceHealthy && readTagFromReader(rfidEntrance, uid, sizeof(uid))) {
                if (xSemaphoreTake(enrollMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    strncpy(enrollmentUid, uid, sizeof(enrollmentUid) - 1);
                    enrollmentUid[sizeof(enrollmentUid) - 1] = '\0';
                    enrollmentDone = true;
                    enrollmentActive = false;
                    xSemaphoreGive(enrollMutex);
                    Serial.printf("[RFID] Enrollment tag: %s\n", uid);
                }
            } else {
                checkReaderHealth(rfidEntrance, entranceHealthy, entranceFailCount,
                                  lastEntranceReinit, "entrance");
            }
            vTaskDelay(pdMS_TO_TICKS(RFID_POLL_INTERVAL_MS));
            continue;
        }

        SystemState state = stateMachineGetState();

        bool pollEntrance = false;
        bool pollInside = false;

        switch (state) {
            case STATE_IDLE:
                pollEntrance = true;
                break;
            case STATE_DOOR_ENTRY:
                pollEntrance = true;
                break;
            case STATE_WAITING_FOR_PRODUCT:
                pollInside = true;
                break;
            case STATE_UV_ACTIVE:
            case STATE_UV_DONE:
                break;
            default:
                break;
        }

        if (pollEntrance && entranceHealthy) {
            if (readTagFromReader(rfidEntrance, uid, sizeof(uid))) {
                TagEvent evt = {};
                strncpy(evt.uid, uid, sizeof(evt.uid) - 1);
                evt.source = TAG_SOURCE_ENTRANCE;
                classifyTag(evt);
                xQueueSend(tagEventQueue, &evt, 0);
            } else {
                checkReaderHealth(rfidEntrance, entranceHealthy, entranceFailCount,
                                  lastEntranceReinit, "entrance");
            }
        }

        if (pollInside && insideHealthy) {
            if (readTagFromReader(rfidInside, uid, sizeof(uid))) {
                TagEvent evt = {};
                strncpy(evt.uid, uid, sizeof(evt.uid) - 1);
                evt.source = TAG_SOURCE_INSIDE;
                classifyTag(evt);
                xQueueSend(tagEventQueue, &evt, 0);
            } else {
                checkReaderHealth(rfidInside, insideHealthy, insideFailCount,
                                  lastInsideReinit, "inside");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(RFID_POLL_INTERVAL_MS));
    }
}

void rfidInit() {
    enrollMutex = xSemaphoreCreateMutex();
    tagEventQueue = xQueueCreate(QUEUE_TAG_EVENTS, sizeof(TagEvent));

    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_ENTRANCE_RFID_SS);
    rfidEntrance.PCD_Init();
    rfidInside.PCD_Init();

    checkReaderHealth(rfidEntrance, entranceHealthy, entranceFailCount,
                      lastEntranceReinit, "entrance");
    checkReaderHealth(rfidInside, insideHealthy, insideFailCount,
                      lastInsideReinit, "inside");

    Serial.printf("[RFID] Entrance: %s, Inside: %s\n",
                  entranceHealthy ? "OK" : "FAIL",
                  insideHealthy ? "OK" : "FAIL");
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
    return enrollmentActive;
}

bool rfidEntranceHealthy() {
    return entranceHealthy;
}

bool rfidInsideHealthy() {
    return insideHealthy;
}

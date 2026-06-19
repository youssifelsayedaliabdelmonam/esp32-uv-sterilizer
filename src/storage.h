#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <vector>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <ArduinoJson.h>
#include "config.h"

struct UserTag {
    String uid;
    String name;
};

struct ProductTag {
    String uid;
};

struct CycleLogEntry {
    String userUid;
    String productUid;
    String timestamp;
};

// Tag type used for enrollment conflict checks
enum StorageTagType : uint8_t {
    STORAGE_TAG_USER = 0,
    STORAGE_TAG_PRODUCT
};

enum TagConflictType : uint8_t {
    TAG_NO_CONFLICT = 0,
    TAG_CONFLICT_IS_USER,
    TAG_CONFLICT_IS_PRODUCT
};

class Storage {
public:
    bool begin();
    void end();

    // User tags (max MAX_USERS)
    bool loadUsers(std::vector<UserTag>& users);
    bool saveUsers(const std::vector<UserTag>& users);
    bool addUser(const String& uid, const String& name);
    bool deleteUser(const String& uid);
    bool isUserTag(const String& uid, String* outName = nullptr);

    // Product tags (unlimited)
    bool loadProducts(std::vector<ProductTag>& products);
    bool saveProducts(const std::vector<ProductTag>& products);
    bool addProduct(const String& uid);
    bool deleteProduct(const String& uid);
    bool isProductTag(const String& uid);

    // UV duration (NVS)
    uint32_t getUvDurationSec();
    bool setUvDurationSec(uint32_t seconds);

    // Cycle logging (thread-safe via queue + mutex)
    bool queueCycleLog(const char* userUid, const char* productUid);
    void processLogQueue();
    bool appendLog(const char* userUid, const char* productUid);
    bool loadLogs(std::vector<CycleLogEntry>& logs);

    // Tag conflict / reassignment
    TagConflictType checkTagConflict(const String& uid, StorageTagType enrollingAs);
    bool reassignTag(const String& uid, StorageTagType newType, const String& name = "");

    // System time (set via web UI; optionally restored from NVS at boot)
    bool setSystemTime(time_t epoch);
    time_t getSystemTime();
    String formatTime(time_t epoch);
    String formatCurrentTime();
    void applySavedTime();

private:
    bool spiffsReady;
    SemaphoreHandle_t storageMutex;
    QueueHandle_t logQueue;

    bool ensureSpiffs();
    bool lockStorage(TickType_t timeout = pdMS_TO_TICKS(STORAGE_MUTEX_TIMEOUT_MS));
    void unlockStorage();
    bool writeJsonFile(const char* path, const JsonDocument& doc);
    bool readJsonFile(const char* path, JsonDocument& doc);
    void createDefaultFiles();
    bool saveLastTime(time_t epoch);
};

extern Storage storage;

#endif // STORAGE_H

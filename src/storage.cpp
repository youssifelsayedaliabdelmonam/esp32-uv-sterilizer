#include "storage.h"
#include <SPIFFS.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <sys/time.h>
#include <time.h>
#include <algorithm>

Storage storage;

static Preferences prefs;

struct PendingLogEvent {
    char userUid[24];
    char productUid[24];
};

bool Storage::lockStorage(TickType_t timeout) {
    if (!storageMutex) return false;
    return xSemaphoreTake(storageMutex, timeout) == pdTRUE;
}

void Storage::unlockStorage() {
    if (storageMutex) xSemaphoreGive(storageMutex);
}

bool Storage::ensureSpiffs() {
    if (spiffsReady) return true;
    if (!SPIFFS.begin(true)) {
        Serial.println("[Storage] SPIFFS mount failed");
        return false;
    }
    spiffsReady = true;
    return true;
}

void Storage::createDefaultFiles() {
    if (!SPIFFS.exists(USERS_JSON_PATH)) {
        StaticJsonDocument<256> doc;
        doc.createNestedArray("users");
        writeJsonFile(USERS_JSON_PATH, doc);
    }
    if (!SPIFFS.exists(PRODUCTS_JSON_PATH)) {
        StaticJsonDocument<256> doc;
        doc.createNestedArray("products");
        writeJsonFile(PRODUCTS_JSON_PATH, doc);
    }
    if (!SPIFFS.exists(LOGS_JSON_PATH)) {
        StaticJsonDocument<256> doc;
        doc.createNestedArray("logs");
        writeJsonFile(LOGS_JSON_PATH, doc);
    }
}

bool Storage::begin() {
    spiffsReady = false;
    storageMutex = xSemaphoreCreateMutex();
    logQueue = xQueueCreate(QUEUE_LOG_EVENTS, sizeof(PendingLogEvent));
    if (!storageMutex || !logQueue) {
        Serial.println("[Storage] Failed to create mutex/queue");
        return false;
    }

    if (!ensureSpiffs()) return false;
    createDefaultFiles();

    if (!prefs.begin(NVS_NAMESPACE, false)) {
        Serial.println("[Storage] NVS open failed");
        return false;
    }
    if (!prefs.isKey(NVS_KEY_UV_DURATION)) {
        prefs.putUInt(NVS_KEY_UV_DURATION, UV_DURATION_DEFAULT_SEC);
    }

    setenv("TZ", "UTC0", 1);
    tzset();
    applySavedTime();
    return true;
}

void Storage::end() {
    prefs.end();
    if (spiffsReady) {
        SPIFFS.end();
        spiffsReady = false;
    }
}

bool Storage::writeJsonFile(const char* path, const JsonDocument& doc) {
    if (!ensureSpiffs()) return false;
    File f = SPIFFS.open(path, "w");
    if (!f) {
        Serial.printf("[Storage] Cannot write %s\n", path);
        return false;
    }
    if (serializeJson(doc, f) == 0) {
        f.close();
        return false;
    }
    f.close();
    return true;
}

bool Storage::readJsonFile(const char* path, JsonDocument& doc) {
    if (!ensureSpiffs()) return false;
    if (!SPIFFS.exists(path)) return false;
    File f = SPIFFS.open(path, "r");
    if (!f) return false;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    return !err;
}

bool Storage::loadUsers(std::vector<UserTag>& users) {
    users.clear();
    if (!lockStorage()) return false;
    DynamicJsonDocument doc(2048);
    bool ok = readJsonFile(USERS_JSON_PATH, doc);
    if (ok) {
        JsonArray arr = doc["users"].as<JsonArray>();
        if (!arr.isNull()) {
            for (JsonObject obj : arr) {
                UserTag u;
                u.uid  = obj["uid"].as<String>();
                u.name = obj["name"].as<String>();
                if (u.uid.length() > 0) users.push_back(u);
            }
        }
    }
    unlockStorage();
    return ok;
}

bool Storage::saveUsers(const std::vector<UserTag>& users) {
    if (!lockStorage()) return false;
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.createNestedArray("users");
    for (const auto& u : users) {
        JsonObject obj = arr.createNestedObject();
        obj["uid"]  = u.uid;
        obj["name"] = u.name;
    }
    bool ok = writeJsonFile(USERS_JSON_PATH, doc);
    unlockStorage();
    return ok;
}

bool Storage::addUser(const String& uid, const String& name) {
    std::vector<UserTag> users;
    loadUsers(users);
    for (const auto& u : users) {
        if (u.uid.equalsIgnoreCase(uid)) return false;
    }
    if ((int)users.size() >= MAX_USERS) return false;
    users.push_back({uid, name});
    return saveUsers(users);
}

bool Storage::deleteUser(const String& uid) {
    std::vector<UserTag> users;
    loadUsers(users);
    bool found = false;
    for (auto it = users.begin(); it != users.end(); ) {
        if (it->uid.equalsIgnoreCase(uid)) {
            it = users.erase(it);
            found = true;
        } else {
            ++it;
        }
    }
    return found && saveUsers(users);
}

bool Storage::isUserTag(const String& uid, String* outName) {
    std::vector<UserTag> users;
    loadUsers(users);
    for (const auto& u : users) {
        if (u.uid.equalsIgnoreCase(uid)) {
            if (outName) *outName = u.name;
            return true;
        }
    }
    return false;
}

bool Storage::loadProducts(std::vector<ProductTag>& products) {
    products.clear();
    if (!lockStorage()) return false;
    DynamicJsonDocument doc(4096);
    bool ok = readJsonFile(PRODUCTS_JSON_PATH, doc);
    if (ok) {
        JsonArray arr = doc["products"].as<JsonArray>();
        if (!arr.isNull()) {
            for (JsonObject obj : arr) {
                ProductTag p;
                p.uid  = obj["uid"].as<String>();
                p.name = obj["name"] | "";
                if (p.uid.length() > 0) products.push_back(p);
            }
        }
    }
    unlockStorage();
    return ok;
}

bool Storage::saveProducts(const std::vector<ProductTag>& products) {
    if (!lockStorage()) return false;
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.createNestedArray("products");
    for (const auto& p : products) {
        JsonObject obj = arr.createNestedObject();
        obj["uid"]  = p.uid;
        obj["name"] = p.name;
    }
    bool ok = writeJsonFile(PRODUCTS_JSON_PATH, doc);
    unlockStorage();
    return ok;
}

bool Storage::addProduct(const String& uid, const String& name) {
    std::vector<ProductTag> products;
    loadProducts(products);
    for (const auto& p : products) {
        if (p.uid.equalsIgnoreCase(uid)) return false;
    }
    products.push_back({uid, name});
    return saveProducts(products);
}

bool Storage::deleteProduct(const String& uid) {
    std::vector<ProductTag> products;
    loadProducts(products);
    bool found = false;
    for (auto it = products.begin(); it != products.end(); ) {
        if (it->uid.equalsIgnoreCase(uid)) {
            it = products.erase(it);
            found = true;
        } else {
            ++it;
        }
    }
    return found && saveProducts(products);
}

bool Storage::isProductTag(const String& uid, String* outName) {
    std::vector<ProductTag> products;
    loadProducts(products);
    for (const auto& p : products) {
        if (p.uid.equalsIgnoreCase(uid)) {
            if (outName) *outName = p.name;
            return true;
        }
    }
    return false;
}

String Storage::getUserDisplayName(const String& uid) {
    String name;
    if (isUserTag(uid, &name) && name.length() > 0) return name;
    return uid;
}

String Storage::getProductDisplayName(const String& uid) {
    String name;
    if (isProductTag(uid, &name) && name.length() > 0) return name;
    return uid;
}

uint32_t Storage::getUvDurationSec() {
    uint32_t val = prefs.getUInt(NVS_KEY_UV_DURATION, UV_DURATION_DEFAULT_SEC);
    if (val < UV_DURATION_MIN_SEC) val = UV_DURATION_MIN_SEC;
    if (val > UV_DURATION_MAX_SEC) val = UV_DURATION_MAX_SEC;
    return val;
}

bool Storage::setUvDurationSec(uint32_t seconds) {
    if (seconds < UV_DURATION_MIN_SEC || seconds > UV_DURATION_MAX_SEC) return false;
    prefs.putUInt(NVS_KEY_UV_DURATION, seconds);
    return true;
}

// -----------------------------------------------------------------------------
// System time
// -----------------------------------------------------------------------------

bool Storage::saveLastTime(time_t epoch) {
    return prefs.putLong(NVS_KEY_LAST_TIME, (long)epoch) > 0;
}

bool Storage::setSystemTime(time_t epoch) {
    struct timeval tv;
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
        Serial.println("[Storage] settimeofday failed");
        return false;
    }
    saveLastTime(epoch);
    Serial.printf("[Storage] System time set to %ld\n", (long)epoch);
    return true;
}

bool Storage::setSystemTimeFromParts(int year, int month, int day,
                                     int hour, int minute, int second) {
    if (year < 2020 || year > 2099 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = minute;
    t.tm_sec  = second;
    t.tm_isdst = -1;
    time_t epoch = mktime(&t);
    if (epoch < 0) return false;
    return setSystemTime(epoch);
}

time_t Storage::getSystemTime() {
    return time(nullptr);
}

void Storage::applySavedTime() {
    if (!prefs.isKey(NVS_KEY_LAST_TIME)) return;
    long saved = prefs.getLong(NVS_KEY_LAST_TIME, 0);
    if (saved > 0) {
        struct timeval tv;
        tv.tv_sec = saved;
        tv.tv_usec = 0;
        settimeofday(&tv, nullptr);
        Serial.printf("[Storage] Restored time from NVS: %ld (set via web UI previously)\n", saved);
    }
}

String Storage::formatTime(time_t epoch) {
    struct tm timeinfo;
    localtime_r(&epoch, &timeinfo);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buf);
}

String Storage::formatCurrentTime() {
    return formatTime(getSystemTime());
}

// -----------------------------------------------------------------------------
// Cycle logging
// -----------------------------------------------------------------------------

bool Storage::queueCycleLog(const char* userUid, const char* productUid) {
    if (!logQueue || !userUid || !productUid) return false;
    PendingLogEvent evt = {};
    strncpy(evt.userUid, userUid, sizeof(evt.userUid) - 1);
    evt.userUid[sizeof(evt.userUid) - 1] = '\0';
    strncpy(evt.productUid, productUid, sizeof(evt.productUid) - 1);
    evt.productUid[sizeof(evt.productUid) - 1] = '\0';
    if (xQueueSend(logQueue, &evt, 0) != pdTRUE) {
        Serial.println("[Storage] Log queue full – entry dropped");
        return false;
    }
    return true;
}

void Storage::processLogQueue() {
    if (!logQueue) return;
    PendingLogEvent evt;
    while (xQueueReceive(logQueue, &evt, 0) == pdTRUE) {
        appendLog(evt.userUid, evt.productUid);
    }
}

bool Storage::appendLog(const char* userUid, const char* productUid) {
    if (!userUid || !productUid || userUid[0] == '\0' || productUid[0] == '\0') {
        return false;
    }

    String timestamp = formatCurrentTime();
    String userName = getUserDisplayName(String(userUid));
    String productName = getProductDisplayName(String(productUid));

    if (!lockStorage(pdMS_TO_TICKS(STORAGE_MUTEX_TIMEOUT_MS))) return false;

    DynamicJsonDocument doc(LOG_JSON_DOC_SIZE);
    if (!readJsonFile(LOGS_JSON_PATH, doc)) {
        doc.createNestedArray("logs");
    }
    JsonArray arr = doc["logs"].to<JsonArray>();
    if (arr.isNull()) {
        arr = doc.createNestedArray("logs");
    }

    JsonObject entry = arr.createNestedObject();
    entry["user_uid"]      = userUid;
    entry["user_name"]     = userName;
    entry["product_uid"]   = productUid;
    entry["product_name"]  = productName;
    entry["timestamp"]     = timestamp;

    while ((int)arr.size() > MAX_LOG_ENTRIES) {
        arr.remove(0);
    }

    bool ok = writeJsonFile(LOGS_JSON_PATH, doc);
    unlockStorage();

    if (ok) {
        Serial.printf("[Storage] Logged cycle: user=%s product=%s\n", userUid, productUid);
    }
    return ok;
}

bool Storage::loadLogs(std::vector<CycleLogEntry>& logs) {
    logs.clear();
    if (!lockStorage(pdMS_TO_TICKS(STORAGE_MUTEX_TIMEOUT_MS))) return false;

    DynamicJsonDocument doc(LOG_JSON_DOC_SIZE);
    bool ok = readJsonFile(LOGS_JSON_PATH, doc);
    if (ok) {
        JsonArray arr = doc["logs"].as<JsonArray>();
        if (!arr.isNull()) {
            for (JsonObject obj : arr) {
                CycleLogEntry e;
                e.userUid      = obj["user_uid"].as<String>();
                e.userName     = obj["user_name"] | "";
                e.productUid   = obj["product_uid"].as<String>();
                e.productName  = obj["product_name"] | "";
                e.timestamp    = obj["timestamp"].as<String>();
                if (e.userName.length() == 0) e.userName = e.userUid;
                if (e.productName.length() == 0) e.productName = e.productUid;
                if (e.userUid.length() > 0) logs.push_back(e);
            }
        }
    }
    unlockStorage();

    // Newest first
    std::reverse(logs.begin(), logs.end());
    return ok;
}

// -----------------------------------------------------------------------------
// Tag conflict / reassignment
// -----------------------------------------------------------------------------

TagConflictType Storage::checkTagConflict(const String& uid, StorageTagType enrollingAs) {
    if (enrollingAs == STORAGE_TAG_USER && isProductTag(uid)) {
        return TAG_CONFLICT_IS_PRODUCT;
    }
    if (enrollingAs == STORAGE_TAG_PRODUCT && isUserTag(uid)) {
        return TAG_CONFLICT_IS_USER;
    }
    return TAG_NO_CONFLICT;
}

bool Storage::reassignTag(const String& uid, StorageTagType newType, const String& name) {
    if (!lockStorage(pdMS_TO_TICKS(STORAGE_MUTEX_TIMEOUT_MS))) return false;

    DynamicJsonDocument usersDoc(2048);
    DynamicJsonDocument productsDoc(4096);
    readJsonFile(USERS_JSON_PATH, usersDoc);
    readJsonFile(PRODUCTS_JSON_PATH, productsDoc);

    JsonArray usersArr = usersDoc["users"].to<JsonArray>();
    JsonArray productsArr = productsDoc["products"].to<JsonArray>();
    if (usersArr.isNull()) usersArr = usersDoc.createNestedArray("users");
    if (productsArr.isNull()) productsArr = productsDoc.createNestedArray("products");

    // Remove UID from both lists
    for (size_t i = 0; i < usersArr.size(); ) {
        const char* u = usersArr[i]["uid"] | "";
        if (uid.equalsIgnoreCase(u)) {
            usersArr.remove(i);
        } else {
            ++i;
        }
    }
    for (size_t i = 0; i < productsArr.size(); ) {
        const char* u = productsArr[i]["uid"] | "";
        if (uid.equalsIgnoreCase(u)) {
            productsArr.remove(i);
        } else {
            ++i;
        }
    }

    bool ok = false;
    if (newType == STORAGE_TAG_USER) {
        if ((int)usersArr.size() < MAX_USERS) {
            JsonObject obj = usersArr.createNestedObject();
            obj["uid"]  = uid;
            obj["name"] = name;
            ok = true;
        }
    } else {
        JsonObject obj = productsArr.createNestedObject();
        obj["uid"]  = uid;
        obj["name"] = name;
        ok = true;
    }

    if (ok) {
        ok = writeJsonFile(USERS_JSON_PATH, usersDoc)
          && writeJsonFile(PRODUCTS_JSON_PATH, productsDoc);
    }

    unlockStorage();
    return ok;
}

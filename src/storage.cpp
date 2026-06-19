#include "storage.h"
#include <SPIFFS.h>
#include <Preferences.h>

Storage storage;

static Preferences prefs;

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
}

bool Storage::begin() {
    spiffsReady = false;
    if (!ensureSpiffs()) return false;
    createDefaultFiles();

    if (!prefs.begin(NVS_NAMESPACE, false)) {
        Serial.println("[Storage] NVS open failed");
        return false;
    }
    if (!prefs.isKey(NVS_KEY_UV_DURATION)) {
        prefs.putUInt(NVS_KEY_UV_DURATION, UV_DURATION_DEFAULT_SEC);
    }
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
    DynamicJsonDocument doc(2048);
    if (!readJsonFile(USERS_JSON_PATH, doc)) return false;
    JsonArray arr = doc["users"].as<JsonArray>();
    if (arr.isNull()) return true;
    for (JsonObject obj : arr) {
        UserTag u;
        u.uid  = obj["uid"].as<String>();
        u.name = obj["name"].as<String>();
        if (u.uid.length() > 0) users.push_back(u);
    }
    return true;
}

bool Storage::saveUsers(const std::vector<UserTag>& users) {
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.createNestedArray("users");
    for (const auto& u : users) {
        JsonObject obj = arr.createNestedObject();
        obj["uid"]  = u.uid;
        obj["name"] = u.name;
    }
    return writeJsonFile(USERS_JSON_PATH, doc);
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
    DynamicJsonDocument doc(4096);
    if (!readJsonFile(PRODUCTS_JSON_PATH, doc)) return false;
    JsonArray arr = doc["products"].as<JsonArray>();
    if (arr.isNull()) return true;
    for (JsonObject obj : arr) {
        ProductTag p;
        p.uid = obj["uid"].as<String>();
        if (p.uid.length() > 0) products.push_back(p);
    }
    return true;
}

bool Storage::saveProducts(const std::vector<ProductTag>& products) {
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.createNestedArray("products");
    for (const auto& p : products) {
        JsonObject obj = arr.createNestedObject();
        obj["uid"] = p.uid;
    }
    return writeJsonFile(PRODUCTS_JSON_PATH, doc);
}

bool Storage::addProduct(const String& uid) {
    std::vector<ProductTag> products;
    loadProducts(products);
    for (const auto& p : products) {
        if (p.uid.equalsIgnoreCase(uid)) return false;
    }
    products.push_back({uid});
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

bool Storage::isProductTag(const String& uid) {
    std::vector<ProductTag> products;
    loadProducts(products);
    for (const auto& p : products) {
        if (p.uid.equalsIgnoreCase(uid)) return true;
    }
    return false;
}

uint32_t Storage::getUvDurationSec() {
    uint32_t val = prefs.getUInt(NVS_KEY_UV_DURATION, UV_DURATION_DEFAULT_SEC);
    if (val < UV_DURATION_MIN_SEC) val = UV_DURATION_MIN_SEC;
    if (val > UV_DURATION_MAX_SEC) val = UV_DURATION_MAX_SEC;
    return val;
}

bool Storage::setUvDurationSec(uint32_t seconds) {
    if (seconds < UV_DURATION_MIN_SEC || seconds > UV_DURATION_MAX_SEC) return false;
    return prefs.putUInt(NVS_KEY_UV_DURATION, seconds) > 0;
}

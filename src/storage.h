#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>
#include "config.h"

struct UserTag {
    String uid;
    String name;
};

struct ProductTag {
    String uid;
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

private:
    bool spiffsReady;
    bool ensureSpiffs();
    bool writeJsonFile(const char* path, const JsonDocument& doc);
    bool readJsonFile(const char* path, JsonDocument& doc);
    void createDefaultFiles();
};

extern Storage storage;

#endif // STORAGE_H

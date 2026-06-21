#include "web_server.h"
#include "config.h"
#include "storage.h"
#include "state_machine.h"
#include "rfid_manager.h"
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <sys/time.h>

static WebServer server(WEB_SERVER_PORT);
static QueueHandle_t webCmdQueue = nullptr;
static TaskHandle_t webTaskHandle = nullptr;
static volatile bool webActive = false;

static void sendJson(int code, const String& body) {
    server.send(code, "application/json", body);
}

static void sendError(int code, const char* msg) {
    StaticJsonDocument<128> doc;
    doc["error"] = msg;
    String out;
    serializeJson(doc, out);
    sendJson(code, out);
}

static void sendConflict(const char* message, const char* uid) {
    StaticJsonDocument<256> doc;
    doc["error"] = "conflict";
    doc["message"] = message;
    doc["uid"] = uid;
    String out;
    serializeJson(doc, out);
    sendJson(409, out);
}

static void handleRoot() {
    if (!SPIFFS.exists("/index.html")) {
        server.send(404, "text/plain", "index.html not found. Run uploadfs.");
        return;
    }
    File f = SPIFFS.open("/index.html", "r");
    if (!f) {
        server.send(500, "text/plain", "Cannot open index.html");
        return;
    }
    server.streamFile(f, "text/html");
    f.close();
}

static void handleStatus() {
    SystemStatus st;
    stateMachineGetStatus(st);
    StaticJsonDocument<512> doc;
    doc["state"] = systemStateName(st.state);
    doc["uv_time_remaining"] = st.uvTimeRemainingSec;
    doc["state_timeout_remaining_ms"] = st.stateTimeoutRemainingMs;
    doc["entrance_locked"] = st.entranceLocked;
    doc["exit_locked"] = st.exitLocked;
    doc["uv_lamp_on"] = st.uvLampOn;
    doc["last_user_uid"] = st.lastUserUid;
    doc["last_product_uid"] = st.lastProductUid;
    doc["last_user_name"] = st.lastUserName;
    doc["last_product_name"] = st.lastProductName;
    doc["web_server_active"] = st.webServerActive;
    doc["ap_ip"] = st.apIp;
    doc["entrance_rfid_ok"] = st.entranceRfidOk;
    doc["inside_rfid_ok"] = st.insideRfidOk;
    doc["entrance_rfid_version"] = rfidEntranceVersion();
    doc["inside_rfid_version"] = rfidInsideVersion();
    doc["uv_duration_sec"] = storage.getUvDurationSec();
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

static void handleGetUsers() {
    std::vector<UserTag> users;
    storage.loadUsers(users);
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.createNestedArray("users");
    for (const auto& u : users) {
        JsonObject obj = arr.createNestedObject();
        obj["uid"] = u.uid;
        obj["name"] = u.name;
    }
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

static void handlePostUsers() {
    if (!server.hasArg("plain")) {
        sendError(400, "Missing JSON body");
        return;
    }
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        sendError(400, "Invalid JSON");
        return;
    }
    const char* uid = doc["uid"];
    const char* name = doc["name"] | "";
    if (!uid || strlen(uid) == 0) {
        sendError(400, "uid required");
        return;
    }
    if (!storage.addUser(String(uid), String(name))) {
        sendError(409, "Cannot add user (duplicate or max reached)");
        return;
    }
    StaticJsonDocument<128> resp;
    resp["success"] = true;
    String out;
    serializeJson(resp, out);
    sendJson(201, out);
}

static void handlePutUsers() {
    if (!server.hasArg("plain")) {
        sendError(400, "Missing JSON body");
        return;
    }
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        sendError(400, "Invalid JSON");
        return;
    }
    const char* uid = doc["uid"];
    const char* name = doc["name"] | "";
    bool overwrite = doc["overwrite"] | false;
    if (!uid || strlen(uid) == 0) {
        sendError(400, "uid required");
        return;
    }
    if (overwrite) {
        if (!storage.reassignTag(String(uid), STORAGE_TAG_USER, String(name))) {
            sendError(409, "Cannot reassign tag as user");
            return;
        }
    } else if (!storage.addUser(String(uid), String(name))) {
        sendError(409, "Cannot add user (duplicate or max reached)");
        return;
    }
    sendJson(200, "{\"success\":true}");
}

static void handleDeleteUser() {
    if (!server.hasArg("uid")) {
        sendError(400, "uid query param required");
        return;
    }
    if (!storage.deleteUser(server.arg("uid"))) {
        sendError(404, "User not found");
        return;
    }
    sendJson(200, "{\"success\":true}");
}

static void handleGetProducts() {
    std::vector<ProductTag> products;
    storage.loadProducts(products);
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.createNestedArray("products");
    for (const auto& p : products) {
        JsonObject obj = arr.createNestedObject();
        obj["uid"]  = p.uid;
        obj["name"] = p.name;
    }
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

static void handlePostProduct() {
    if (!server.hasArg("plain")) {
        sendError(400, "Missing JSON body");
        return;
    }
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        sendError(400, "Invalid JSON");
        return;
    }
    const char* uid = doc["uid"];
    const char* name = doc["name"] | "";
    if (!uid || strlen(uid) == 0) {
        sendError(400, "uid required");
        return;
    }
    if (!storage.addProduct(String(uid), String(name))) {
        sendError(409, "Cannot add product (duplicate)");
        return;
    }
    sendJson(201, "{\"success\":true}");
}

static void handlePutProduct() {
    if (!server.hasArg("plain")) {
        sendError(400, "Missing JSON body");
        return;
    }
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        sendError(400, "Invalid JSON");
        return;
    }
    const char* uid = doc["uid"];
    const char* name = doc["name"] | "";
    bool overwrite = doc["overwrite"] | false;
    if (!uid || strlen(uid) == 0) {
        sendError(400, "uid required");
        return;
    }
    if (overwrite) {
        if (!storage.reassignTag(String(uid), STORAGE_TAG_PRODUCT, String(name))) {
            sendError(409, "Cannot reassign tag as product");
            return;
        }
    } else if (!storage.addProduct(String(uid), String(name))) {
        sendError(409, "Cannot add product (duplicate)");
        return;
    }
    sendJson(200, "{\"success\":true}");
}

static void handleDeleteProduct() {
    if (!server.hasArg("uid")) {
        sendError(400, "uid query param required");
        return;
    }
    if (!storage.deleteProduct(server.arg("uid"))) {
        sendError(404, "Product not found");
        return;
    }
    sendJson(200, "{\"success\":true}");
}

static void handleScanTag() {
    if (!server.hasArg("type")) {
        sendError(400, "type query param required (user|product)");
        return;
    }
    String type = server.arg("type");
    EnrollType et = ENROLL_USER;
    if (type.equalsIgnoreCase("product")) {
        et = ENROLL_PRODUCT;
    } else if (!type.equalsIgnoreCase("user")) {
        sendError(400, "type must be user or product");
        return;
    }

    if (rfidIsEnrollmentActive()) {
        sendError(409, "Scan already in progress");
        return;
    }

    rfidStartEnrollment(et);
    char uid[24] = {};
    bool ok = rfidWaitEnrollmentResult(uid, sizeof(uid), ENROLLMENT_TIMEOUT_MS);

    if (ok) {
        String uidStr(uid);
        StorageTagType stype = (et == ENROLL_PRODUCT) ? STORAGE_TAG_PRODUCT : STORAGE_TAG_USER;
        TagConflictType conflict = storage.checkTagConflict(uidStr, stype);
        if (conflict == TAG_CONFLICT_IS_PRODUCT) {
            sendConflict("This tag is already registered as a product. Reassign?", uid);
            return;
        }
        if (conflict == TAG_CONFLICT_IS_USER) {
            sendConflict("This tag is already registered as a user. Reassign?", uid);
            return;
        }
        StaticJsonDocument<128> doc;
        doc["uid"] = uid;
        String out;
        serializeJson(doc, out);
        sendJson(200, out);
    } else {
        sendError(408, "Scan timeout - no tag detected");
    }
}

static void handleReassign() {
    if (!server.hasArg("plain")) {
        sendError(400, "Missing JSON body");
        return;
    }
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        sendError(400, "Invalid JSON");
        return;
    }
    const char* uid = doc["uid"];
    const char* type = doc["type"];
    const char* name = doc["name"] | "";
    if (!uid || !type) {
        sendError(400, "uid and type required");
        return;
    }
    StorageTagType newType;
    if (strcmp(type, "user") == 0) {
        newType = STORAGE_TAG_USER;
    } else if (strcmp(type, "product") == 0) {
        newType = STORAGE_TAG_PRODUCT;
    } else {
        sendError(400, "type must be user or product");
        return;
    }
    if (!storage.reassignTag(String(uid), newType, String(name))) {
        sendError(409, "Reassign failed");
        return;
    }
    sendJson(200, "{\"success\":true}");
}

static void handleGetLogs() {
    std::vector<CycleLogEntry> logs;
    storage.loadLogs(logs);
    DynamicJsonDocument doc(LOG_JSON_DOC_SIZE);
    JsonArray arr = doc.createNestedArray("logs");
    for (const auto& e : logs) {
        JsonObject obj = arr.createNestedObject();
        obj["user_uid"]     = e.userUid;
        obj["user_name"]    = e.userName;
        obj["product_uid"]  = e.productUid;
        obj["product_name"] = e.productName;
        obj["timestamp"]    = e.timestamp;
    }
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

static void handleGetTime() {
    time_t now = storage.getSystemTime();
    StaticJsonDocument<256> doc;
    doc["timestamp"] = (long)now;
    doc["formatted"] = storage.formatCurrentTime();
    doc["set"] = (now > 1000000000L);
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

static void handlePostTime() {
    if (!server.hasArg("plain")) {
        sendError(400, "Missing JSON body");
        return;
    }
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        sendError(400, "Invalid JSON");
        return;
    }

    bool ok = false;
    if (doc.containsKey("year") && doc.containsKey("month") && doc.containsKey("day")) {
        int year   = doc["year"];
        int month  = doc["month"];
        int day    = doc["day"];
        int hour   = doc["hour"] | 0;
        int minute = doc["minute"] | 0;
        int second = doc["second"] | 0;
        ok = storage.setSystemTimeFromParts(year, month, day, hour, minute, second);
    } else if (doc.containsKey("timestamp")) {
        long ts = doc["timestamp"].as<long>();
        if (ts > 0) {
            ok = storage.setSystemTime((time_t)ts);
        }
    } else {
        sendError(400, "Send year/month/day/hour/minute or timestamp");
        return;
    }

    if (!ok) {
        sendError(400, "Failed to set system time");
        return;
    }
    StaticJsonDocument<256> resp;
    resp["success"] = true;
    resp["formatted"] = storage.formatCurrentTime();
    String out;
    serializeJson(resp, out);
    sendJson(200, out);
}

static void handleApStop() {
    webServerStop();
    sendJson(200, "{\"success\":true,\"message\":\"AP stopped\"}");
}

static void handleFactoryReset() {
    if (!server.hasArg("plain")) {
        sendError(400, "Missing JSON body");
        return;
    }
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        sendError(400, "Invalid JSON");
        return;
    }
    if (!(doc["confirm"] | false)) {
        sendError(400, "confirm:true required");
        return;
    }
    if (!storage.factoryReset()) {
        sendError(500, "Factory reset failed");
        return;
    }
    sendJson(200, "{\"success\":true,\"message\":\"Users, products, and logs cleared\"}");
}

static void handleNotFound() {
    sendError(404, "Not found");
}

static void handlePostSettings() {
    if (!server.hasArg("plain")) {
        sendError(400, "Missing JSON body");
        return;
    }
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        sendError(400, "Invalid JSON");
        return;
    }
    if (!doc.containsKey("uv_duration")) {
        sendError(400, "uv_duration required");
        return;
    }
    if (!doc["uv_duration"].is<uint32_t>() && !doc["uv_duration"].is<int>()) {
        sendError(400, "uv_duration must be a number");
        return;
    }
    uint32_t dur = doc["uv_duration"];
    if (!storage.setUvDurationSec(dur)) {
        sendError(400, "Invalid UV duration");
        return;
    }
    sendJson(200, "{\"success\":true}");
}

static void setupRoutes() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/users", HTTP_GET, handleGetUsers);
    server.on("/api/users", HTTP_POST, handlePostUsers);
    server.on("/api/users", HTTP_PUT, handlePutUsers);
    server.on("/api/users", HTTP_DELETE, handleDeleteUser);
    server.on("/api/products", HTTP_GET, handleGetProducts);
    server.on("/api/products", HTTP_POST, handlePostProduct);
    server.on("/api/products", HTTP_PUT, handlePutProduct);
    server.on("/api/products", HTTP_DELETE, handleDeleteProduct);
    server.on("/api/scan_tag", HTTP_GET, handleScanTag);
    server.on("/api/reassign", HTTP_POST, handleReassign);
    server.on("/api/logs", HTTP_GET, handleGetLogs);
    server.on("/api/time", HTTP_GET, handleGetTime);
    server.on("/api/time", HTTP_POST, handlePostTime);
    server.on("/api/ap/stop", HTTP_POST, handleApStop);
    server.on("/api/factory_reset", HTTP_POST, handleFactoryReset);
    server.on("/api/settings", HTTP_POST, handlePostSettings);
    server.onNotFound(handleNotFound);
}

static void startWebServer() {
    if (webActive) return;

    WiFi.mode(WIFI_AP);
    bool apOk = WiFi.softAP(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL, 0, WIFI_MAX_CONNECTIONS);
    if (!apOk) {
        Serial.println("[Web] AP start failed");
        return;
    }

    IPAddress ip = WiFi.softAPIP();
    char ipStr[16];
    snprintf(ipStr, sizeof(ipStr), "%s", ip.toString().c_str());

    setupRoutes();
    server.begin();
    webActive = true;
    stateMachineSetWebInfo(true, ipStr);
    Serial.printf("[Web] AP started: %s (%s)\n", WIFI_SSID, ipStr);
}

static void stopWebServer() {
    if (!webActive) return;

    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    webActive = false;
    stateMachineSetWebInfo(false, nullptr);
    Serial.println("[Web] AP stopped");
}

static void webServerTask(void* param) {
    (void)param;
    WebServerCmd cmd;

    for (;;) {
        if (xQueueReceive(webCmdQueue, &cmd, portMAX_DELAY) == pdTRUE) {
            if (cmd == WEB_CMD_START) {
                startWebServer();

                while (webActive) {
                    server.handleClient();

                    if (xQueueReceive(webCmdQueue, &cmd, 0) == pdTRUE) {
                        if (cmd == WEB_CMD_STOP) {
                            stopWebServer();
                            break;
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            } else if (cmd == WEB_CMD_STOP) {
                stopWebServer();
            }
        }
    }
}

void webServerInit() {
    webCmdQueue = xQueueCreate(QUEUE_WEB_CMD, sizeof(WebServerCmd));
}

void webServerStartManagerTask() {
    xTaskCreatePinnedToCore(
        webServerTask, "webTask",
        TASK_STACK_WEB, nullptr,
        TASK_PRIO_WEB, &webTaskHandle,
        CORE_RFID_WEB
    );
}

void webServerToggle() {
    if (!webCmdQueue) return;
    WebServerCmd cmd = webActive ? WEB_CMD_STOP : WEB_CMD_START;
    xQueueSend(webCmdQueue, &cmd, pdMS_TO_TICKS(500));
}

void webServerStop() {
    if (!webCmdQueue) return;
    WebServerCmd cmd = WEB_CMD_STOP;
    xQueueSend(webCmdQueue, &cmd, pdMS_TO_TICKS(500));
}

bool webServerIsActive() {
    return webActive;
}

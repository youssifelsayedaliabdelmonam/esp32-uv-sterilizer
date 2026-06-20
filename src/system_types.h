#ifndef SYSTEM_TYPES_H
#define SYSTEM_TYPES_H

#include <Arduino.h>

// System state machine states
enum SystemState : uint8_t {
    STATE_IDLE = 0,
    STATE_DOOR_ENTRY,
    STATE_WAITING_FOR_PRODUCT,
    STATE_UV_ACTIVE,
    STATE_UV_DONE
};

// Human-readable state names for API / LCD
inline const char* systemStateName(SystemState s) {
    switch (s) {
        case STATE_IDLE:                 return "IDLE";
        case STATE_DOOR_ENTRY:           return "DOOR_ENTRY";
        case STATE_WAITING_FOR_PRODUCT:  return "WAITING_FOR_PRODUCT";
        case STATE_UV_ACTIVE:            return "UV_ACTIVE";
        case STATE_UV_DONE:              return "UV_DONE";
        default:                         return "UNKNOWN";
    }
}

// Which RFID reader produced a tag event
enum TagSource : uint8_t {
    TAG_SOURCE_ENTRANCE = 0,
    TAG_SOURCE_INSIDE
};

// Tag event passed from RFID task to state machine
struct TagEvent {
    char uid[24];           // "AA:BB:CC:DD" format
    TagSource source;
    bool recognized;        // UID found in user or product list
    bool isUser;
    bool isProduct;
};

// Buzzer pattern commands
enum BeepPattern : uint8_t {
    BEEP_SINGLE_SHORT = 0,
    BEEP_DOUBLE_SHORT,
    BEEP_LONG,
    BEEP_ERROR
};

// Web server start/stop commands from button task
enum WebServerCmd : uint8_t {
    WEB_CMD_START = 0,
    WEB_CMD_STOP
};

// Enrollment request type
enum EnrollType : uint8_t {
    ENROLL_USER = 0,
    ENROLL_PRODUCT
};

// Shared system status (mutex-protected for LCD / web API)
struct SystemStatus {
    SystemState state;
    uint32_t uvTimeRemainingSec;
    uint32_t stateTimeoutRemainingMs;
    bool entranceLocked;
    bool exitLocked;
    bool uvLampOn;
    char lastUserUid[24];
    char lastProductUid[24];
    char lastUserName[20];
    char lastProductName[20];
    bool webServerActive;
    char apIp[16];
    bool entranceRfidOk;
    bool insideRfidOk;
};

#endif // SYSTEM_TYPES_H

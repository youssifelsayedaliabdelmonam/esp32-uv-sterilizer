#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
// UV Sterilization Room Controller – Central Configuration
// All pins, timeouts, WiFi credentials, and tunable options live here.
// =============================================================================

// -----------------------------------------------------------------------------
// GPIO Pin Assignments
// -----------------------------------------------------------------------------
#define PIN_ENTRANCE_RFID_SS    5
#define PIN_ENTRANCE_RFID_RST   25
#define PIN_INSIDE_RFID_SS      17
#define PIN_INSIDE_RFID_RST     26

#define PIN_SPI_SCK             18
#define PIN_SPI_MISO            19
#define PIN_SPI_MOSI            23

#define PIN_LCD_SDA             21
#define PIN_LCD_SCL             22

#define PIN_ENTRANCE_LOCK       14
#define PIN_EXIT_LOCK           12
#define PIN_UV_RELAY            13
#define PIN_BUZZER              27
#define PIN_BOOT_BUTTON         0

// Relay polarity: HIGH = active (lock engaged / UV on / buzzer on)
#define LOCK_ACTIVE             HIGH
#define LOCK_INACTIVE           LOW
#define UV_ACTIVE               HIGH
#define UV_INACTIVE             LOW
#define BUZZER_ACTIVE           HIGH
#define BUZZER_INACTIVE         LOW

// -----------------------------------------------------------------------------
// I2C LCD
// -----------------------------------------------------------------------------
#define LCD_I2C_ADDR            0x27
#define LCD_COLS                20
#define LCD_ROWS                4
#define LCD_UPDATE_INTERVAL_MS  300

// -----------------------------------------------------------------------------
// Timing (milliseconds)
// -----------------------------------------------------------------------------
#define ENTRY_TIMEOUT_MS        10000
#define EXIT_TIMEOUT_MS         30000
#define ENROLLMENT_TIMEOUT_MS   15000
#define LONG_PRESS_DURATION_MS  5000
#define BOOT_DEBOUNCE_MS        50

// UV duration default (seconds) – persisted in NVS
#define UV_DURATION_DEFAULT_SEC 30
#define UV_DURATION_MIN_SEC     5
#define UV_DURATION_MAX_SEC     600

// -----------------------------------------------------------------------------
// WiFi Access Point
// -----------------------------------------------------------------------------
#define WIFI_SSID               "UV-Sterilizer"
#define WIFI_PASSWORD           "admin123"
#define WIFI_CHANNEL            1
#define WIFI_MAX_CONNECTIONS    4

// -----------------------------------------------------------------------------
// Buzzer Patterns (milliseconds)
// -----------------------------------------------------------------------------
#define BEEP_SHORT_MS           100
#define BEEP_LONG_MS            500
#define BEEP_ERROR_MS           2000
#define BEEP_GAP_MS             100

// -----------------------------------------------------------------------------
// RFID / SPI Failsafe
// -----------------------------------------------------------------------------
#define RFID_POLL_INTERVAL_MS       80
#define RFID_FAIL_THRESHOLD         10
#define RFID_REINIT_COOLDOWN_MS     3000
#define RFID_SPI_SPEED_HZ           1000000

// -----------------------------------------------------------------------------
// Storage Limits
// -----------------------------------------------------------------------------
#define MAX_USERS               5

// SPIFFS file paths
#define USERS_JSON_PATH         "/users.json"
#define PRODUCTS_JSON_PATH      "/products.json"

// NVS namespace / keys
#define NVS_NAMESPACE           "uvsteril"
#define NVS_KEY_UV_DURATION     "uv_duration"

// -----------------------------------------------------------------------------
// FreeRTOS Task Configuration
// -----------------------------------------------------------------------------
#define TASK_STACK_RFID         4096
#define TASK_STACK_WEB          8192
#define TASK_STACK_LCD          3072
#define TASK_STACK_BUZZER       2048
#define TASK_STACK_BUTTON       2048
#define TASK_STACK_STATE        4096

#define TASK_PRIO_RFID          5
#define TASK_PRIO_STATE         4
#define TASK_PRIO_WEB           3
#define TASK_PRIO_LCD           2
#define TASK_PRIO_BUZZER        2
#define TASK_PRIO_BUTTON        2

#define CORE_RFID_WEB           1
#define CORE_LCD_BUZZER         0

// Queue depths
#define QUEUE_TAG_EVENTS        8
#define QUEUE_BUZZER            8
#define QUEUE_WEB_CMD           4

// Web server
#define WEB_SERVER_PORT         80

#endif // CONFIG_H

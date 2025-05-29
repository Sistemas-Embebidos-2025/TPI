/**
 * @file main.cpp
 * @brief Smart irrigation and lighting system for Arduino using FreeRTOS.
 *
 * This firmware manages automatic irrigation and lighting based on sensor readings,
 * logs events to EEPROM, and communicates via serial commands. It uses FreeRTOS tasks
 * for concurrent sensor reading, control, serial communication, and timekeeping.
 *
 * Features:
 * - Periodic sensor readings (moisture, light)
 * - Automatic irrigation and lighting control based on thresholds
 * - Event logging to EEPROM with wrap-around
 * - Serial command interface for configuration and log retrieval
 * - Persistent timekeeping with periodic EEPROM backup
 */

#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <EEPROM.h>
#include <semphr.h>
#include <task.h>
#include <avr/pgmspace.h>

#define EEPROM_ADDR_CURRENT_TIME 0
#define MOISTURE_SENSOR A2
#define LIGHT_SENSOR A3

#define LOG_HEADER "TS,T,V"
#define LOG_END "E"
#define ERROR "R"
#define SUCCESS "OK"
#define CMD_UNKNOWN "U"
#define CMD_TIME 'T'
#define CMD_GET_LOGS "G"
#define CMD_DELETE_LOGS "D"
#define CMD_GET_MOISTURE_THRESHOLD "X"
#define CMD_GET_LIGHT_THRESHOLD "Z"
#define CMD_SET_LIGHT_THRESHOLD 'L'
#define CMD_SET_MOISTURE_THRESHOLD 'M'
#define MOISTURE_SYMBOL 'm'
#define LIGHT_SYMBOL 'l'

constexpr int irrigationPins[] = {6};
constexpr int numIrrigation = sizeof(irrigationPins) / sizeof(irrigationPins[0]);
const int lightPins[] = {8, 9, 10, 11, 12};
constexpr int numLights = sizeof(lightPins) / sizeof(lightPins[0]);

struct Event {
    uint32_t timestamp;
    uint8_t eventType;
    uint16_t value;
};

constexpr int RECORD_SIZE = sizeof(Event);
constexpr int LOG_START_ADDRESS = sizeof(uint32_t);
unsigned int nextAddr = LOG_START_ADDRESS;

enum EventType {
    AUTO_IRRIGATION = 0,  // 0 Irrigation state change due to auto mode
    AUTO_LIGHT = 1,       // 1 Light state change due to auto mode (on/off threshold)
    LIGHT_TH = 2,         // 2 Light threshold changed
    MOISTURE_TH = 3,      // 3 Moisture threshold changed
    MEASURE_MOISTURE = 4, // 4 Periodic moisture sensor reading
    MEASURE_LIGHT = 5,    // 5 Periodic light sensor reading
};

static constexpr uint8_t logSensorPeriod = 5 * 60;     // Seconds to log sensor values
static constexpr uint32_t timeSaveInterval = 15 * 60;  // Seconds to save currentTime to EEPROM (15 minutes)

SemaphoreHandle_t eepromMutex;
SemaphoreHandle_t serialMutex;
SemaphoreHandle_t sensorMutex;

uint16_t moistureTh = 500;
uint16_t lightTh = 500;
uint16_t moisture, light;
uint8_t globalBrightness = 0;

volatile uint32_t currentTime = 1704067200;

static int sensorDelay = pdMS_TO_TICKS(2000);
static int ctrlDelay = pdMS_TO_TICKS(2500);
static int serialDelay = pdMS_TO_TICKS(1500);
static int oneSecond = pdMS_TO_TICKS(1000);

#define CMD_BUFFER_SIZE 12
char cmd_buffer[CMD_BUFFER_SIZE];
uint8_t buffer_index = 0;

/**
 * @brief Print "OK" to serial in a thread-safe manner.
 */
void printOK() {
    if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
        Serial.println(F(SUCCESS));
        xSemaphoreGive(serialMutex);
    }
}

/**
 * @brief Print an error message to serial in a thread-safe manner.
 * @param errorMsg Error message string
 */
void printError(const char *errorMsg){
    if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
        Serial.print(F(ERROR));
        Serial.println(errorMsg);
        xSemaphoreGive(serialMutex);
    }
}

/**
 * @brief Find the next available EEPROM address for logging.
 * @return Address of the next empty log slot.
 */
unsigned int findNextEEPROMAddress() {
    Event event{};
    const unsigned int logDataAreaLength = EEPROM.length() - LOG_START_ADDRESS;
    const unsigned int usableLogLength = logDataAreaLength / RECORD_SIZE * RECORD_SIZE;

    // Iterate only over the portion of EEPROM allocated for logs
    for (unsigned int offset = 0; offset < usableLogLength; offset += RECORD_SIZE) {
        EEPROM.get(static_cast<int>(LOG_START_ADDRESS + offset), event);
        if (event.timestamp == 0xFFFFFFFF) return LOG_START_ADDRESS + offset;
    }
    // If no empty slot is found (EEPROM log area is full), wrap around to the beginning of the log area.
    // No semaphore, since it's called before initializing the tasks.
    Serial.print(F(ERROR));
    Serial.println(F("OverwritingLogs"));
    return LOG_START_ADDRESS;
}

/**
 * @brief Log an event to EEPROM.
 * @param type Event type (see EventType)
 * @param value Associated value for the event
 */
void logEvent(const uint8_t type, const uint16_t value) {
    const unsigned int logDataAreaLength = EEPROM.length() - LOG_START_ADDRESS;
    // Ensure usableLogLength is positive
    const unsigned int usableLogLength =
            logDataAreaLength >= RECORD_SIZE ? logDataAreaLength / RECORD_SIZE * RECORD_SIZE : 0;

    if (usableLogLength == 0) {
        // Not enough space for even one log record
        printError("NoLogSpace");
        return;
    }

    if (xSemaphoreTake(eepromMutex, portMAX_DELAY) == pdTRUE) {
        const Event newEvent = {currentTime, type, value};
        EEPROM.put(static_cast<int>(nextAddr), newEvent);

        nextAddr += RECORD_SIZE;
        // Check if nextAddr needs to wrap around within the log data area
        if (nextAddr >= LOG_START_ADDRESS + usableLogLength) {
            nextAddr = LOG_START_ADDRESS; // Wrap to the beginning of the log area
        }
        xSemaphoreGive(eepromMutex);
    }
}

/**
 * @brief Print all logged events from EEPROM to serial.
 */
void printLogs() {
    const unsigned int logDataAreaLength = EEPROM.length() - LOG_START_ADDRESS;
    const unsigned int usableLogLength = logDataAreaLength / RECORD_SIZE * RECORD_SIZE;

    if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
        Serial.println(F(LOG_HEADER));
        if (usableLogLength > 0) {
            Event e{};
            for (unsigned int offset = 0; offset < usableLogLength; offset += RECORD_SIZE) {
                const unsigned int addr = LOG_START_ADDRESS + offset;
                EEPROM.get(static_cast<int>(addr), e);
                if (e.timestamp == 0xFFFFFFFFUL) break; // We use timestamp==0xFFFFFFFF to mark “empty” slots
                Serial.print(e.timestamp);
                Serial.print(F(","));
                Serial.print(e.eventType);
                Serial.print(F(","));
                Serial.println(e.value);
            }
        }
        Serial.println(F(LOG_END));
        xSemaphoreGive(serialMutex);
    }
}

/**
 * @brief Clear all logs from EEPROM.
 */
void clearLogs() {
    const unsigned int logDataAreaLength = EEPROM.length() - LOG_START_ADDRESS;
    if (xSemaphoreTake(eepromMutex, portMAX_DELAY) == pdTRUE) {
        for (unsigned int offset = 0; offset < logDataAreaLength; offset += RECORD_SIZE) {
            Event emptyEvent = {0xFFFFFFFF, 0, 0};
            EEPROM.put(static_cast<int>(LOG_START_ADDRESS + offset), emptyEvent);
        }
        // Reset nextAddr to the start of the log area
        nextAddr = LOG_START_ADDRESS;
        xSemaphoreGive(eepromMutex);
    }
}

/**
 * @brief Log the current system state (sensor values, thresholds, pin states).
 */
void logCurrentSystemState() {
    uint16_t localMoisture = 0, localLight = 0; // local copies of sensor values

    if (xSemaphoreTake(sensorMutex, portMAX_DELAY) == pdTRUE) {
        localMoisture = moisture;
        localLight = light;
        xSemaphoreGive(sensorMutex);
    }
    logEvent(MEASURE_MOISTURE, localMoisture); // Log current moisture
    logEvent(MEASURE_LIGHT, localLight);       // Log current light

    // Log current thresholds
    logEvent(MOISTURE_TH, moistureTh);
    logEvent(LIGHT_TH, lightTh);

    // Log current irrigation pin state
    logEvent(AUTO_IRRIGATION, digitalRead(irrigationPins[0]) == HIGH ? 1 : 0);
    // TODO: Use semaphore to protect
    logEvent(AUTO_LIGHT, globalBrightness);
}

/**
 * @brief Set the state of all irrigation pins.
 * @param state true to turn on, false to turn off
 */
void setIrrigationPins(const bool state) {
    for (const int pin: irrigationPins) { digitalWrite(pin, state ? HIGH : LOW); }
}

/**
 * @brief Set the brightness of all light pins.
 * @param brightness PWM value (0-255)
 */
void setLightPins(const int brightness) {
    globalBrightness = constrain(brightness, 0, 255);
    for (const int pin: lightPins) { analogWrite(pin, globalBrightness); }
}

/**
 * @brief Print an unknown command response to serial.
 * @param cmd The unknown command string
 */
void unknownCommand(const char *cmd) {
    if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
        Serial.print(F(CMD_UNKNOWN));
        Serial.println(cmd);
        xSemaphoreGive(serialMutex);
    }
}

/**
 * @brief Handle a received serial command.
 * @param cmd Null-terminated command string
 */
void handleCommand(const char *cmd) {
    if (cmd == nullptr || cmd[0] == '\0') return;

    // Time Command
    if (cmd[0] == CMD_TIME) {
        // Update current time
        currentTime = static_cast<uint32_t>(atol(cmd + 1));
        // Log the current time in the EEPROM
        if (xSemaphoreTake(eepromMutex, portMAX_DELAY) == pdTRUE) {
            EEPROM.put(EEPROM_ADDR_CURRENT_TIME, currentTime);
            xSemaphoreGive(eepromMutex);
        }
        printOK();
        return;
    }

    // Fixed String Commands
    if (strcmp(cmd, CMD_GET_LOGS) == 0) {
        // Get logs
        printLogs();
        return;
    }
    if (strcmp(cmd, CMD_DELETE_LOGS) == 0) {
        // Delete logs
        clearLogs();
        logCurrentSystemState();
        printOK();
        return;
    }
    if (strcmp(cmd, CMD_GET_MOISTURE_THRESHOLD) == 0) {
        // Get Moisture Threshold
        if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
            Serial.print(F(CMD_GET_MOISTURE_THRESHOLD));
            Serial.println(moistureTh);
            xSemaphoreGive(serialMutex);
        }
        return;
    }
    if (strcmp(cmd, CMD_GET_LIGHT_THRESHOLD) == 0) {
        // Get Light Threshold
        if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
            Serial.print(F(CMD_GET_LIGHT_THRESHOLD));
            Serial.println(lightTh);
            xSemaphoreGive(serialMutex);
        }
        return;
    }

    // Key-Value Commands
    // Check length before accessing indices
    if (strlen(cmd) >= 2) {
        const char key = cmd[0];
        const auto val = static_cast<uint16_t>(atoi(cmd + 1));
        if (key == CMD_SET_LIGHT_THRESHOLD) {
            lightTh = constrain(val, 0, 1023);
            logEvent(LIGHT_TH, lightTh);
            printOK();
            return;
        }
        if (key == CMD_SET_MOISTURE_THRESHOLD) {
            moistureTh = constrain(val, 0, 1023);
            logEvent(MOISTURE_TH, moistureTh);
            printOK();
            return;
        }
    }
    unknownCommand(cmd);
}

// TASKS ---------------------------------------------------------------------------------------------------------------

/**
 * @brief Task: Periodically read sensors and log values.
 * @param pvParameters Unused
 */
void readSensorsTask(void *pvParameters) {
    static uint32_t lastPeriodicLog = 0;

    while (true) {
        const uint16_t localMoisture = analogRead(MOISTURE_SENSOR); // Local reading of moisture
        const uint16_t localLight = analogRead(LIGHT_SENSOR);       // Local reading of light

        if (xSemaphoreTake(sensorMutex, portMAX_DELAY) == pdTRUE) {
            moisture = localMoisture;
            light = localLight;
            xSemaphoreGive(sensorMutex);
        }

        // Send sensor readings via serial
        if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
            Serial.print(MOISTURE_SYMBOL);
            Serial.print(localMoisture);
            Serial.print(LIGHT_SYMBOL);
            Serial.println(localLight);
            xSemaphoreGive(serialMutex);
        }

        // Log periodic sensor values
        if (currentTime - lastPeriodicLog >= logSensorPeriod) {
            logEvent(MEASURE_MOISTURE, localMoisture);
            logEvent(MEASURE_LIGHT, localLight);
            lastPeriodicLog = currentTime; // Update last log time
        }

        vTaskDelay(sensorDelay);
    }
}

/**
 * @brief Task: Automatic control of irrigation and lighting based on sensor readings.
 * @param pvParameters Unused
 */
void autoControlTask(void *pvParameters) {
    static bool wasLightLow = false;

    while (true) {
        uint16_t localMoisture = 0, localLight = 0; // Local copies of sensor values
        if (xSemaphoreTake(sensorMutex, portMAX_DELAY) == pdTRUE) {
            localMoisture = moisture;
            localLight = light;
            xSemaphoreGive(sensorMutex);
        }

        // Auto irrigation control
        const bool isOn = digitalRead(irrigationPins[0]) == HIGH;
        if (localMoisture > moistureTh && !isOn) {
            // Moisture above threshold means low moisture
            setIrrigationPins(true);
            logEvent(AUTO_IRRIGATION, 1);
        } else if (localMoisture <= moistureTh && isOn) {
            setIrrigationPins(false);
            logEvent(AUTO_IRRIGATION, 0);
        }

        // Auto light adjustment
        setLightPins(static_cast<int>(map(localLight, 0, lightTh, 255, 0))); // Inverse mapping

        const bool isLightLow = localLight < lightTh;
        if (isLightLow != wasLightLow) {
            logEvent(AUTO_LIGHT, isLightLow ? 1 : 0);
            wasLightLow = isLightLow; // Update state tracker
        }

        vTaskDelay(ctrlDelay);
    }
}

/**
 * @brief Task: Handle serial communication and command parsing.
 * @param pvParameters Unused
 */
void serialComTask(void *pvParameters) {
    while (true) {
        while (Serial.available()) {
            const char receivedChar = static_cast<char>(Serial.read());
            if (receivedChar == '\n' || receivedChar == '\r') {
                if (buffer_index > 0) {
                    // If we have finished a command
                    cmd_buffer[buffer_index] = '\0'; // Null-terminate the string
                    handleCommand(cmd_buffer);
                    buffer_index = 0; // Reset buffer index for next command
                }
            } else if (buffer_index < (CMD_BUFFER_SIZE - 1)) {
                // Add char to buffer if it's not newline and fits
                cmd_buffer[buffer_index++] = receivedChar;
            } else {
                Serial.print(F(ERROR));
                Serial.println(F("BufferOverflow"));
                // printError("BufferOverflow");
                // If I use a semaphore here, it will cause a deadlock. WHY?
                buffer_index = 0;
            }
        }
        vTaskDelay(serialDelay);
    }
}

/**
 * @brief Task: Increment and periodically save system time to EEPROM.
 * @param pvParameters Unused
 */
void updateTimeTask(void *pvParameters) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    uint32_t lastSavedTime = 0;

    while (true) {
        currentTime++; // Increment even if no NTP updates
        vTaskDelayUntil(&lastWakeTime, oneSecond);

        // Log current time in the EEPROM every timeSaveInterval seconds
        if (currentTime - lastSavedTime >= timeSaveInterval) {
            if (xSemaphoreTake(eepromMutex, portMAX_DELAY) == pdTRUE) {
                EEPROM.put(EEPROM_ADDR_CURRENT_TIME, currentTime);
                xSemaphoreGive(eepromMutex);
            }
            lastSavedTime = currentTime;
        }
    }
}

// SETUP & LOOP --------------------------------------------------------------------------------------------------------

/**
 * @brief Arduino setup function. Initializes hardware, EEPROM, mutexes, and tasks.
 */
void setup() {
    Serial.begin(115200);

    // Init irrigation and light pins
    for (const int pin: irrigationPins) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
    }
    for (const int pin: lightPins) {
        pinMode(pin, OUTPUT);
        analogWrite(pin, 0); // off
    }

    eepromMutex = xSemaphoreCreateMutex();
    serialMutex = xSemaphoreCreateMutex();
    sensorMutex = xSemaphoreCreateMutex();

    EEPROM.begin();

    uint32_t storedTime = 0;
    EEPROM.get(EEPROM_ADDR_CURRENT_TIME, storedTime);
    if (storedTime >= currentTime && storedTime < 0xFFFFFFFF) {
        // Basic validity check
        currentTime = storedTime;
    }

    nextAddr = findNextEEPROMAddress();

    xTaskCreate(readSensorsTask, "Sensors", 200, nullptr, 1, nullptr);
    xTaskCreate(autoControlTask, "AutoCtrl", 128, nullptr, 2, nullptr);
    xTaskCreate(serialComTask, "Serial", 300, nullptr, 3, nullptr);
    xTaskCreate(updateTimeTask, "Time", 70, nullptr, 1, nullptr);

    vTaskStartScheduler();
}

/**
 * @brief Arduino main loop (unused, required by framework).
 */
void loop() {}

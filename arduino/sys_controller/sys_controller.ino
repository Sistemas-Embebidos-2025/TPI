#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <EEPROM.h>
#include <semphr.h>
#include <task.h>

// Pin Definitions
#define MOISTURE_SENSOR A2
#define LIGHT_SENSOR A3

// Irrigation and Light Pins
const int irrigPins[] = {6};
const int numIrrig = sizeof(irrigPins) / sizeof(irrigPins[0]);
const int lightPins[] = {8, 9, 10, 11, 12};
const int numLights = sizeof(lightPins) / sizeof(lightPins[0]);

// EEPROM Event Log
struct Event {
    uint32_t timestamp;
    uint8_t eventType;
    uint16_t value;
};
const int RECORD_SIZE = sizeof(Event);
int nextAddr = 0;

enum EventType {
    AUTO_IRRIG = 0,        // 0 Irrigation state change due to auto mode
    AUTO_LIGHT = 1,        // 1 Light state change due to auto mode (on/off threshold)
    LIGHT_TH = 2,          // 2 Light threshold changed
    MOISTURE_TH = 3,       // 3 Moisture threshold changed
    MEASURE_MOISTURE = 4,  // 4 Periodic moisture sensor reading
    MEASURE_LIGHT = 5,     // 5 Periodic light sensor reading
};

static const uint8_t logSensorPeriod = 60;  // seconds

SemaphoreHandle_t eepromMutex;
SemaphoreHandle_t serialMutex;
SemaphoreHandle_t sensorMutex;

// Thresholds and State
uint16_t moistureTh = 500;
uint16_t lightTh = 500;
uint16_t moisture, light;
uint8_t globalBrightness = 0;
volatile uint32_t currentTime = 1;

static int sensorDelay = pdMS_TO_TICKS(2000);
static int ctrlDelay = pdMS_TO_TICKS(2500);
static int serialDelay = pdMS_TO_TICKS(1500);
static int oneSecond = pdMS_TO_TICKS(1000);

#define CMD_BUFFER_SIZE 12
char cmd_buffer[CMD_BUFFER_SIZE];
uint8_t buffer_index = 0;

void logEvent(uint8_t type, uint16_t value) {
    if (xSemaphoreTake(eepromMutex, portMAX_DELAY) == pdTRUE) {
        Event newEvent = {currentTime, type, value};
        EEPROM.put(nextAddr, newEvent);
        nextAddr = (nextAddr + RECORD_SIZE) % EEPROM.length();
        xSemaphoreGive(eepromMutex);
    }
}

int findNextEEPROMAddress() {
    Event event;
    for (int addr = 0; (unsigned int)addr < EEPROM.length();
    addr += RECORD_SIZE) {
        EEPROM.get(addr, event);
        if (event.timestamp == 0xFFFFFFFF) return addr;
    }
    return 0;
}

// Read through EEPROM from 0 up to first empty slot and dump each Event over
void printLogs() {
    static const char LOG_HEADER[] = "TS,T,V";
    static const char LOG_END[] = "E";

    Event e;
    if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
        Serial.println(LOG_HEADER);  // header row
        for (int addr = 0; (unsigned int)addr < EEPROM.length();
        addr += RECORD_SIZE) {
            EEPROM.get(addr, e);
            // We use timestamp==0xFFFFFFFF to mark “empty” slots
            if (e.timestamp == 0xFFFFFFFFUL) break;
            // Print as CSV: timestamp,event_type,event_value
            Serial.print(e.timestamp); Serial.print(F(",")); Serial.print(e.eventType); Serial.print(F(",")); Serial.println(e.value);
        }
        Serial.println(LOG_END);  // end of log marker
        xSemaphoreGive(serialMutex);
    }
}

void logCurrentSystemState() {
    uint16_t m, l;  // local copies of sensor values

    if (xSemaphoreTake(sensorMutex, portMAX_DELAY) == pdTRUE) {
        m = moisture;
        l = light;
        xSemaphoreGive(sensorMutex);
    }
    logEvent(MEASURE_MOISTURE, m);  // Log current moisture
    logEvent(MEASURE_LIGHT, l);     // Log current light

    // Log current thresholds
    logEvent(MOISTURE_TH, moistureTh);
    logEvent(LIGHT_TH, lightTh);

    // Log current irrigation pin state
    bool isIrrigationOn = digitalRead(irrigPins[0]) == HIGH;
    logEvent(AUTO_IRRIG, isIrrigationOn ? 1 : 0);
    // TODO: Use semaphore to protect
    logEvent(AUTO_LIGHT, globalBrightness);
}

// Clear all logs from EEPROM
void clearLogs() {
    if (xSemaphoreTake(eepromMutex, portMAX_DELAY) == pdTRUE) {
        for (int addr = 0; (unsigned int)addr < EEPROM.length();
        addr += RECORD_SIZE) {
            Event emptyEvent = {0xFFFFFFFF, 0, 0};
            EEPROM.put(addr, emptyEvent);
        }
        nextAddr = 0;
        // Print current system state after clearing logs
        xSemaphoreGive(eepromMutex);
    }
}

void setIrrigationPins(bool state) {
    for (int i = 0; i < numIrrig; i++) {
        digitalWrite(irrigPins[i], state ? HIGH : LOW);
    }
}

void setLightPins(int brightness) {
    globalBrightness = brightness;
    for (int i = 0; i < numLights; i++) {
        analogWrite(lightPins[i], brightness);
    }
}

void unknownCommand(const char *cmd) {
    static const char CMD_UNKNOWN = 'U';

    if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
        Serial.print(CMD_UNKNOWN); Serial.println(cmd);
        xSemaphoreGive(serialMutex);
    }
}

void handleCommand(const char *cmd) {
    static const char CMD_TIME = 'T';
    static const char CMD_GET_LOGS[] = "G";
    static const char CMD_DELETE_LOGS[] = "D";
    static const char CMD_GET_MOISTURE_THRESHOLD[] = "X";
    static const char CMD_GET_LIGHT_THRESHOLD[] = "Z";
    static const char CMD_SET_LIGHT_THRESHOLD = 'L';
    static const char CMD_SET_MOISTURE_THRESHOLD = 'M';
    // Serial.print(F("Handling: ")); Serial.println(cmd); // Debug

    // Time Command
    if (cmd[0] == CMD_TIME) {
        currentTime = (uint32_t)atol(cmd + 1);  // Parses large integer starting from the second character
        // Serial.println(currentTime);  // Debug
        return;
    }

    // Fixed String Commands
    if (strcmp(cmd, CMD_GET_LOGS) == 0) {  // Get logs
        printLogs();
        return;
    }
    if (strcmp(cmd, CMD_DELETE_LOGS) == 0) {  // Delete logs
        clearLogs();
        logCurrentSystemState();
        return;
    }
    if (strcmp(cmd, CMD_GET_MOISTURE_THRESHOLD) == 0) {  // Get Moisture Threshold
        if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
            Serial.print(CMD_GET_MOISTURE_THRESHOLD);
            Serial.println(moistureTh);
            xSemaphoreGive(serialMutex);
        }
        return;
    }
    if (strcmp(cmd, CMD_GET_LIGHT_THRESHOLD) == 0) {  // Get Light Threshold
        if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
            Serial.print(CMD_GET_LIGHT_THRESHOLD);
            Serial.println(lightTh);
            xSemaphoreGive(serialMutex);
        }
        return;
    }

    // Key-Value Commands
    // Check length before accessing indices
    if (strlen(cmd) >= 2) {
        char key = cmd[0];
        uint16_t val = atoi(cmd + 1);  // Parses integer starting from the second character
        // Serial.print(F("Key: ")); Serial.println(key); // Debug
        // Serial.print(F("Val: ")); Serial.println(val); // Debug
        if (key == CMD_SET_LIGHT_THRESHOLD) {
            lightTh = constrain(val, 1, 1023);
            logEvent(LIGHT_TH, lightTh);
            return;
        }
        if (key == CMD_SET_MOISTURE_THRESHOLD) {
            moistureTh = constrain(val, 1, 1023);
            logEvent(MOISTURE_TH, moistureTh);
            return;
        }
    }
    unknownCommand(cmd);
}

// TASKS
// ---------------------------------------------------------------------------------------------------------------

void readSensorsTask(void *pvParameters) {
    static uint32_t lastPeriodicLog = 0;
    static const char MOISTURE_SYMBOL = 'm';
    static const char LIGHT_SYMBOL = 'l';

    while (1) {
        uint16_t m = analogRead(MOISTURE_SENSOR);  // Local reading of moisture
        uint16_t l = analogRead(LIGHT_SENSOR);  // Local reading of light

        if (xSemaphoreTake(sensorMutex, portMAX_DELAY) == pdTRUE) {
            moisture = m;
            light = l;
            xSemaphoreGive(sensorMutex);
        }

        // Send sensor readings via serial
        if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
            Serial.print(MOISTURE_SYMBOL); Serial.print(m); Serial.print(LIGHT_SYMBOL); Serial.println(l);
            xSemaphoreGive(serialMutex);
        }

        // Log periodic sensor values every 30 seconds
        if (currentTime - lastPeriodicLog >= logSensorPeriod) {
            logEvent(MEASURE_MOISTURE, m);
            logEvent(MEASURE_LIGHT, l);
            lastPeriodicLog = currentTime;  // Update last log time
        }

        vTaskDelay(sensorDelay);
    }
}

void autoControlTask(void *pvParameters) {
    static bool wasLightLow = false;

    while (1) {
        uint16_t m, l;  // Local copies of sensor values
        if (xSemaphoreTake(sensorMutex, portMAX_DELAY) == pdTRUE) {
            m = moisture;
            l = light;
            xSemaphoreGive(sensorMutex);
        }

        // Auto irrigation control
        bool isOn = digitalRead(irrigPins[0]) == HIGH;
        if (m > moistureTh && !isOn) {  // Moisture above threshold means low moisture
            setIrrigationPins(true);
            logEvent(AUTO_IRRIG, 1);
        } else if (m <= moistureTh && isOn) {
            setIrrigationPins(false);
            logEvent(AUTO_IRRIG, 0);
        }

        // Auto light adjustment
        int brightness = map(l, 0, lightTh, 255, 0);  // Inverse mapping
        setLightPins(constrain(brightness, 0, 255));

        bool isLightLow = (l < lightTh);
        if (isLightLow != wasLightLow) {
            logEvent(AUTO_LIGHT, isLightLow ? 1 : 0);
            wasLightLow = isLightLow;  // Update state tracker
        }

        vTaskDelay(ctrlDelay);
    }
}

void serialComTask(void *pvParameters) {
    while (1) {
        while (Serial.available()) {
            // String cmd = Serial.readStringUntil('\n');
            char receivedChar = (char)Serial.read();
            if (receivedChar == '\n' || receivedChar == '\r') {
                if (buffer_index > 0) {  // If we have finished a command
                    cmd_buffer[buffer_index] = '\0';  // Null-terminate the string
                    // Serial.print(F("BUF:")); Serial.println(cmd_buffer); //

                    handleCommand(cmd_buffer);
                    buffer_index = 0;  // Reset buffer index for next command
                }
            } else if (buffer_index < (CMD_BUFFER_SIZE - 1)) {
                // Add char to buffer if it's not newline and fits
                cmd_buffer[buffer_index++] = receivedChar;
            } else {
                // Serial.println(F("ERR: CMD Buffer Overflow"));
                buffer_index = 0;
            }
        }
        vTaskDelay(serialDelay);
    }
}

// Update time task to handle NTP updates
void updateTimeTask(void *pvParameters) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    while (1) {
        currentTime++;  // Increment even if no NTP updates
        vTaskDelayUntil(&lastWakeTime, oneSecond);
    }
}

// SETUP & LOOP
// --------------------------------------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    EEPROM.begin();

    // Init irrigation and light pins
    for (int i = 0; i < numIrrig; i++) {
        pinMode(irrigPins[i], OUTPUT);
        digitalWrite(irrigPins[i], LOW);
    }

    for (int i = 0; i < numLights; i++) {
        pinMode(lightPins[i], OUTPUT);
        analogWrite(lightPins[i], 0);  // off
    }

    eepromMutex = xSemaphoreCreateMutex();
    serialMutex = xSemaphoreCreateMutex();
    sensorMutex = xSemaphoreCreateMutex();
    nextAddr = findNextEEPROMAddress();

    xTaskCreate(readSensorsTask, "Sensors", 200, NULL, 1, NULL);
    xTaskCreate(autoControlTask, "AutoCtrl", 128, NULL, 1, NULL);
    xTaskCreate(serialComTask, "Serial", 300, NULL, 2, NULL);
    xTaskCreate(updateTimeTask, "Time", 70, NULL, 1, NULL);

    vTaskStartScheduler();
}

void loop() {}

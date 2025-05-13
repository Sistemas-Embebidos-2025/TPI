#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <EEPROM.h>
#include <semphr.h>
#include <task.h>

// Pin Definitions
#define MOISTURE_SENSOR A2
#define LIGHT_SENSOR A3

// Irrigation and Light Pins
const int iPins[] = {6};
const int numI = sizeof(iPins) / sizeof(iPins[0]);
const int lPins[] = {8, 9, 10, 11, 12};
const int numL = sizeof(lPins) / sizeof(lPins[0]);

// EEPROM Event Log
struct Event {
    uint32_t timestamp;
    uint8_t eventType;
    uint16_t value;
};
const int RECORD_SIZE = sizeof(Event);
int nextAddr = 0;

enum EventType {
    AUTO_I,  // 0 Irrigation state change due to auto mode
    AUTO_L,  // 1 Light state change due to auto mode (on/off threshold)
    L_TH,    // 2 Light threshold changed
    M_TH,    // 3 Moisture threshold changed
    M,       // 4 Periodic moisture sensor reading
    L,       // 5 Periodic light sensor reading
};

const uint8_t logSensorPeriod = 60;  // seconds

SemaphoreHandle_t eMutex;
SemaphoreHandle_t serMutex;
SemaphoreHandle_t senMutex;

// Thresholds and State
uint16_t mTh = 500;
uint16_t lTh = 500;
uint16_t moisture, light;
uint8_t globalBrightness = 0;
volatile uint32_t currentTime = 0;

const int sensorDelay = pdMS_TO_TICKS(2000);
const int ctrlDelay = pdMS_TO_TICKS(2500);
const int serialDelay = pdMS_TO_TICKS(1500);
const int oneSecond = pdMS_TO_TICKS(1000);

#define CMD_BUFFER_SIZE 12
char cmd_buffer[CMD_BUFFER_SIZE];
uint8_t buffer_index = 0;

void logEvent(uint8_t type, uint16_t value) {
    if (xSemaphoreTake(eMutex, portMAX_DELAY) == pdTRUE) {
        Event newEvent = {currentTime, type, value};
        EEPROM.put(nextAddr, newEvent);
        nextAddr = (nextAddr + RECORD_SIZE) % EEPROM.length();
        xSemaphoreGive(eMutex);
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
    Event e;
    if (xSemaphoreTake(serMutex, portMAX_DELAY) == pdTRUE) {
        Serial.println(F("TS,T,V"));  // header row
        for (int addr = 0; (unsigned int)addr < EEPROM.length();
             addr += RECORD_SIZE) {
            EEPROM.get(addr, e);
            // We use timestamp==0xFFFFFFFF to mark “empty” slots
            if (e.timestamp == 0xFFFFFFFFUL || e.timestamp == 0) break;

            // Print as CSV: timestamp,event_type,event_value
            Serial.print(e.timestamp); Serial.print(F(",")); Serial.print(e.eventType); Serial.print(F(",")); Serial.println(e.value);
        }
        Serial.println(F("E"));  // end of log marker
        xSemaphoreGive(serMutex);
    }
}

void logCurrentSystemState() {
    uint16_t m, l;

    if (xSemaphoreTake(senMutex, portMAX_DELAY) == pdTRUE) {
        m = moisture;
        l = light;
        xSemaphoreGive(senMutex);
    }
    logEvent(M, m);  // Log current moisture
    logEvent(L, l);  // Log current light

    // Log current thresholds
    logEvent(M_TH, mTh);
    logEvent(L_TH, lTh);

    // Log current irrigation pin state
    bool isIrrigationOn = digitalRead(iPins[0]) == HIGH;
    logEvent(AUTO_I, isIrrigationOn ? 1 : 0);
    // TODO: Use semaphore to protect
    logEvent(AUTO_L, globalBrightness);
}

// Clear all logs from EEPROM
void clearLogs() {
    if (xSemaphoreTake(eMutex, portMAX_DELAY) == pdTRUE) {
        for (int addr = 0; (unsigned int)addr < EEPROM.length();
             addr += RECORD_SIZE) {
            Event emptyEvent = {0xFFFFFFFF, 0, 0};
            EEPROM.put(addr, emptyEvent);
        }
        nextAddr = 0;
        // Print current system state after clearing logs
        xSemaphoreGive(eMutex);
    }
}

void setIrrigationPins(bool state) {
    for (int i = 0; i < numI; i++) {
        digitalWrite(iPins[i], state ? HIGH : LOW);
    }
}

void setLightPins(int brightness) {
    globalBrightness = brightness;
    for (int i = 0; i < numL; i++) {
        analogWrite(lPins[i], brightness);
    }
}

void unknownCommand(const char *cmd) {
    if (xSemaphoreTake(serMutex, portMAX_DELAY) == pdTRUE) {
        Serial.print(F("U")); Serial.println(cmd);
        xSemaphoreGive(serMutex);
    }
}

void handleCommand(const char *cmd) {
    // Serial.print(F("Handling: ")); Serial.println(cmd); // Debug

    // Time Command
    if (cmd[0] == 'T') {
        currentTime = (uint32_t)atol(cmd +1);  // Parses large integer starting from the second character
        // Serial.println(currentTime);  // Debug
        return;
    }

    // Fixed String Commands
    if (strcmp(cmd, "G") == 0) {  // Get logs
        printLogs();
        return;
    }
    if (strcmp(cmd, "D") == 0) {  // Delete logs
        clearLogs();
        logCurrentSystemState();
        return;
    }

    // Key-Value Commands
    // Check length before accessing indices
    if (strlen(cmd) >= 2) {
        char key = cmd[0];
        uint16_t val = atoi(cmd + 1);  // Parses integer starting from the second character
        // Serial.print(F("Key: ")); Serial.println(key); // Debug
        // Serial.print(F("Val: ")); Serial.println(val); // Debug
        if (key == 'L') {
            lTh = constrain(val, 1, 1023);
            logEvent(L_TH, lTh);
            return;
        }
        if (key == 'M') {
            mTh = constrain(val, 1, 1023);
            logEvent(M_TH, mTh);
            return;
        }
    }
    unknownCommand(cmd);
}

// TASKS
// ---------------------------------------------------------------------------------------------------------------

void readSensorsTask(void *pvParameters) {
    static uint32_t lastPeriodicLog = 0;

    while (1) {
        uint16_t m = analogRead(MOISTURE_SENSOR);
        uint16_t l = analogRead(LIGHT_SENSOR);

        if (xSemaphoreTake(senMutex, portMAX_DELAY) == pdTRUE) {
            moisture = m;
            light = l;
            xSemaphoreGive(senMutex);
        }

        // Send sensor readings via serial
        if (xSemaphoreTake(serMutex, portMAX_DELAY) == pdTRUE) {
            Serial.print(F("m")); Serial.print(m); Serial.print(F("l")); Serial.println(l);
            xSemaphoreGive(serMutex);
        }

        // Log periodic sensor values every 30 seconds
        if (currentTime - lastPeriodicLog >= logSensorPeriod) {
            logEvent(M, m);
            logEvent(L, l);
            lastPeriodicLog = currentTime;  // Update last log time
        }

        vTaskDelay(sensorDelay);
    }
}

void autoControlTask(void *pvParameters) {
    static bool wasLightLow = false;

    while (1) {
        uint16_t m, l;  // local copies of sensor values
        if (xSemaphoreTake(senMutex, portMAX_DELAY) == pdTRUE) {
            m = moisture;
            l = light;
            xSemaphoreGive(senMutex);
        }

        // Auto irrigation control
        bool isOn = digitalRead(iPins[0]) == HIGH;
        if (m > mTh && !isOn) {
            setIrrigationPins(true);
            logEvent(AUTO_I, 1);
        } else if (m <= mTh && isOn) {
            setIrrigationPins(false);
            logEvent(AUTO_I, 0);
        }

        // Auto light adjustment
        int brightness = map(l, 0, lTh, 255, 0);  // Inverse mapping
        setLightPins(constrain(brightness, 0, 255));

        bool isLightLow = (l < lTh);
        if (isLightLow != wasLightLow) {
            logEvent(AUTO_L, isLightLow ? 1 : 0);
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
    for (int i = 0; i < numI; i++) {
        pinMode(iPins[i], OUTPUT);
        digitalWrite(iPins[i], LOW);
    }

    for (int i = 0; i < numL; i++) {
        pinMode(lPins[i], OUTPUT);
        analogWrite(lPins[i], 0);  // off
    }

    eMutex = xSemaphoreCreateMutex();
    serMutex = xSemaphoreCreateMutex();
    senMutex = xSemaphoreCreateMutex();
    nextAddr = findNextEEPROMAddress();

    xTaskCreate(readSensorsTask, "Sensors", 200, NULL, 1, NULL);
    xTaskCreate(autoControlTask, "AutoCtrl", 128, NULL, 1, NULL);
    xTaskCreate(serialComTask, "Serial", 300, NULL, 2, NULL);
    xTaskCreate(updateTimeTask, "Time", 70, NULL, 1, NULL);

    vTaskStartScheduler();
}

void loop() {}

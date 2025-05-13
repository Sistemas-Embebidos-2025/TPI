#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <EEPROM.h>
#include <semphr.h>
#include <task.h>

// Pin Definitions
#define MOISTURE_SENSOR A2
#define LIGHT_SENSOR A3

const int irrigationPins[] = {6};
const int numIrrigation = sizeof(irrigationPins) / sizeof(irrigationPins[0]);

const int lightPins[] = {8, 9, 10, 11, 12};
const int numLights = sizeof(lightPins) / sizeof(lightPins[0]);

// EEPROM Event Log
struct Event {
    uint32_t timestamp;
    uint8_t event_type;
    uint16_t value;
};
const int RECORD_SIZE = sizeof(Event);
int nextAddress = 0;

enum EventType {
    AUTO_IRRIG,  // 0 Irrigation state change due to auto mode
    AUTO_LIGHT,  // 2 Light state change due to auto mode (on/off threshold)
    LIGHT_TH,    // 4 Light threshold changed
    MOIST_TH,    // 5 Moisture threshold changed
    MOISTURE,    // 6 Periodic moisture sensor reading
    LIGHT,       // 7 Periodic light sensor reading
};

const int logSensorPeriod = 60;  // seconds

SemaphoreHandle_t eepromMutex;
SemaphoreHandle_t serialMutex;
SemaphoreHandle_t sensorMutex;

// Thresholds and State
uint16_t moistureTh = 500;
uint16_t lightTh = 500;
uint16_t moisture, light;
uint8_t globalBrightness = 0;
volatile uint32_t currentTime = 0;

const int sensorDelay = pdMS_TO_TICKS(5000);
const int ctrlDelay = pdMS_TO_TICKS(5500);
const int serialDelay = pdMS_TO_TICKS(1000);
const int oneSecond = pdMS_TO_TICKS(1000);

void logEvent(uint8_t type, uint16_t value) {
    if (xSemaphoreTake(eepromMutex, portMAX_DELAY) == pdTRUE) {
        Event newEvent = {currentTime, type, value};
        EEPROM.put(nextAddress, newEvent);
        nextAddress = (nextAddress + RECORD_SIZE) % EEPROM.length();
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
    Event e;
    if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
        Serial.println(F("TS,T,V"));  // header row
        for (int addr = 0; (unsigned int)addr < EEPROM.length();
             addr += RECORD_SIZE) {
            EEPROM.get(addr, e);
            // We use timestamp==0xFFFFFFFF to mark “empty” slots
            if (e.timestamp == 0xFFFFFFFFUL || e.timestamp == 0) break;

            // Print as CSV: timestamp,event_type,event_value
            Serial.print(e.timestamp);
            Serial.print(F(","));
            Serial.print(e.event_type);
            Serial.print(F(","));
            Serial.println(e.value);
        }
        Serial.println(F("E"));  // end of log marker
        xSemaphoreGive(serialMutex);
    }
}

void logCurrentSystemState() {
    uint16_t currentM, currentL;

    if (xSemaphoreTake(sensorMutex, portMAX_DELAY) == pdTRUE) {
        currentM = moisture;
        currentL = light;
        xSemaphoreGive(sensorMutex);
    }
    logEvent(MOISTURE, currentM);  // Log current moisture
    logEvent(LIGHT, currentL);     // Log current light

    // Log current thresholds
    logEvent(MOIST_TH, moistureTh);
    logEvent(LIGHT_TH, lightTh);

    // Log current irrigation pin state
    bool isIrrigationOn = digitalRead(irrigationPins[0]) == HIGH;
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
        nextAddress = 0;
        // Print current system state after clearing logs
        // logCurrentSystemState();
        xSemaphoreGive(eepromMutex);
    }
}

void setIrrigationPins(bool state) {
    for (int i = 0; i < numIrrigation; i++) {
        digitalWrite(irrigationPins[i], state ? HIGH : LOW);
    }
}

void setLightPins(int brightness) {
    globalBrightness = brightness;
    for (int i = 0; i < numLights; i++) {
        analogWrite(lightPins[i], brightness);
    }
}

void unknownCommand(const String &cmd) {
    if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
        Serial.print(F("U"));
        Serial.println(cmd);
        xSemaphoreGive(serialMutex);
    }
}

void handleCommand(const String &cmd) {
    if (cmd[0] == 'T') {
        currentTime = cmd.substring(1).toInt();
        return;
    }
    if (cmd == "G") {  // Get logs
        printLogs();
        return;
    }
    if (cmd == "D") {  // Delete logs
        clearLogs();
        return;
    }
    char key = cmd[0];
    int val = cmd.substring(1).toInt();
    if (cmd[0] == 'L') {
        lightTh = constrain(val, 1, 1023);
        logEvent(LIGHT_TH, lightTh);
        return;
    }
    if (cmd[0] == 'M') {
        moistureTh = constrain(val, 1, 1023);
        logEvent(MOIST_TH, moistureTh);
        return;
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

        if (xSemaphoreTake(sensorMutex, portMAX_DELAY) == pdTRUE) {
            moisture = m;
            light = l;
            xSemaphoreGive(sensorMutex);
        }

        // Send sensor readings via serial
        if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
            Serial.print(F("m"));
            Serial.print(m);
            Serial.print(F("l"));
            Serial.println(l);
            xSemaphoreGive(serialMutex);
        }

        // Log periodic sensor values every 30 seconds
        if (currentTime - lastPeriodicLog >= logSensorPeriod) {
            logEvent(MOISTURE, m);
            logEvent(LIGHT, l);
            lastPeriodicLog = currentTime;  // Update last log time
        }

        vTaskDelay(sensorDelay);
    }
}

void autoControlTask(void *pvParameters) {
    static bool wasLightLow = false;

    while (1) {
        uint16_t m, l;  // local copies of sensor values
        if (xSemaphoreTake(sensorMutex, portMAX_DELAY) == pdTRUE) {
            m = moisture;
            l = light;
            xSemaphoreGive(sensorMutex);
        }

        // Auto irrigation control
        bool isOn = digitalRead(irrigationPins[0]) == HIGH;
        if (m > moistureTh && !isOn) {
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
        if (Serial.available()) {
            String cmd = Serial.readStringUntil('\n');
            cmd.trim();  // Remove leading/trailing whitespace
            if (cmd.length() > 0) {
                handleCommand(cmd);
            } else
                continue;
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
    for (int i = 0; i < numIrrigation; i++) {
        pinMode(irrigationPins[i], OUTPUT);
        digitalWrite(irrigationPins[i], LOW);
    }

    for (int i = 0; i < numLights; i++) {
        pinMode(lightPins[i], OUTPUT);
        analogWrite(lightPins[i], 0);  // off
    }

    eepromMutex = xSemaphoreCreateMutex();
    serialMutex = xSemaphoreCreateMutex();
    sensorMutex = xSemaphoreCreateMutex();
    nextAddress = findNextEEPROMAddress();

    xTaskCreate(readSensorsTask, "Sensors", 200, NULL, 1, NULL);
    xTaskCreate(autoControlTask, "AutoCtrl", 128, NULL, 1, NULL);
    xTaskCreate(serialComTask, "Serial", 300, NULL, 2, NULL);
    xTaskCreate(updateTimeTask, "Time", 70, NULL, 1, NULL);

    vTaskStartScheduler();
}

void loop() {}

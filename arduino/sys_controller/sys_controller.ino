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
int nextLogAddress = 0;

enum EventType {
    AUTO_IRRIG,  // 0 Irrigation state change due to auto mode
    MAN_IRRIG,   // 1 Irrigation state change due to manual command
    AUTO_LIGHT,  // 2 Light state change due to auto mode (on/off threshold)
    MAN_LIGHT,   // 3 Light state change due to manual command (brightness)
    LIGHT_TH,    // 4 Light threshold changed
    MOIST_TH,    // 5 Moisture threshold changed
    MOISTURE,    // 6 Periodic moisture sensor reading
    LIGHT,       // 7 Periodic light sensor reading
    AUTO_IRRIG_MODE,  // 8 Current auto irrigation mode (1 for on, 0 for off)
    AUTO_LIGHT_MODE,  // 9 Current auto light mode (1 for on, 0 for off)
};

const int logSensorPeriod = 60;  // seconds

SemaphoreHandle_t eepromMutex;
SemaphoreHandle_t serialMutex;
SemaphoreHandle_t sensorMutex;

// Thresholds and State
bool autoIrrigation = true;
bool autoLight = true;
uint16_t moisture_threshold = 500;
uint16_t light_threshold = 500;
uint16_t moisture, light;
uint8_t globalBrightness = 0;
volatile uint32_t currentTime = 0;

const int sensorDelay = pdMS_TO_TICKS(1500);
const int ctrlDelay = pdMS_TO_TICKS(1000);
const int serialDelay = pdMS_TO_TICKS(500);
const int oneSecond = pdMS_TO_TICKS(1000);

void logEvent(uint8_t type, uint16_t value) {
    if (xSemaphoreTake(eepromMutex, portMAX_DELAY) == pdTRUE) {
        Event newEvent = {currentTime, type, value};
        EEPROM.put(nextLogAddress, newEvent);
        nextLogAddress = (nextLogAddress + RECORD_SIZE) % EEPROM.length();
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
        Serial.println(F("EL"));  // end of log marker
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
    logEvent(MOIST_TH, moisture_threshold);
    logEvent(LIGHT_TH, light_threshold);

    // Log current operational modes
    logEvent(AUTO_IRRIG_MODE, autoIrrigation ? 1 : 0);
    logEvent(AUTO_LIGHT_MODE, autoLight ? 1 : 0);

    // Log current irrigation pin state
    bool isIrrigationOn = digitalRead(irrigationPins[0]) == HIGH;
    if (autoIrrigation) {
        logEvent(AUTO_IRRIG, isIrrigationOn ? 1 : 0);
    } else {
        logEvent(MAN_IRRIG, isIrrigationOn ? 1 : 0);
    }

    if (autoLight) {
        logEvent(AUTO_LIGHT, globalBrightness);
    } else {
        logEvent(MAN_LIGHT, globalBrightness);
    }
}

// Clear all logs from EEPROM
void clearLogs() {
    if (xSemaphoreTake(eepromMutex, portMAX_DELAY) == pdTRUE) {
        for (int addr = 0; (unsigned int)addr < EEPROM.length();
             addr += RECORD_SIZE) {
            Event emptyEvent = {0xFFFFFFFF, 0, 0};
            EEPROM.put(addr, emptyEvent);
        }
        nextLogAddress = 0;
        // Print current system state after clearing logs
        // logCurrentSystemState();
        xSemaphoreGive(eepromMutex);
    }
    if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
        Serial.println(F("Logs cleared"));
        xSemaphoreGive(serialMutex);
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
        Serial.print(F("UNK:"));
        Serial.println(cmd);
        xSemaphoreGive(serialMutex);
    }
}

void handleCommand(const String &cmd) {
    Serial.print(F("CMD:"));
    Serial.println(cmd);
    if (cmd[0] == 'T') {
        currentTime = cmd.substring(1).toInt();
        return;
    }
    if (cmd == "GL") {
        printLogs();
        return;
    }
    if (cmd == "CL") {
        clearLogs();
        return;
    }
    String key = cmd.substring(0, 2);
    String valStr = cmd.substring(2);
    int val = valStr.toInt();
    if (key == "LT") {
        light_threshold = constrain(val, 1, 1023);
        logEvent(LIGHT_TH, light_threshold);
        return;
    }
    if (key == "MT") {
        moisture_threshold = constrain(val, 1, 1023);
        logEvent(MOIST_TH, moisture_threshold);
        return;
    }
    if (key == "AI") {
        autoIrrigation = (val == 1);
        logEvent(AUTO_IRRIG_MODE, autoIrrigation ? 1 : 0);
        return;
    }
    if (key == "AL") {
        autoLight = (val == 1);
        logEvent(AUTO_LIGHT_MODE, autoLight ? 1 : 0);
        return;
    }
    if (key == "MI") {
        setIrrigationPins(val == 1);
        logEvent(MAN_IRRIG, val);
        return;
    }
    if (key == "ML") {
        int brightness = constrain(val, 0, 255);
        setLightPins(brightness);
        logEvent(MAN_LIGHT, brightness);
        return;
    }
    unknownCommand(cmd);
}

// TASKS ---------------------------------------------------------------------------------------------------------------

void readSensorsTask(void *pvParameters) {
    static uint32_t lastPeriodicLog = 0;

    while (1) {
        uint16_t localM = analogRead(MOISTURE_SENSOR);
        uint16_t localL = analogRead(LIGHT_SENSOR);

        if (xSemaphoreTake(sensorMutex, portMAX_DELAY) == pdTRUE) {
            moisture = localM;
            light = localL;
            xSemaphoreGive(sensorMutex);
        }

        // Send sensor readings via serial
        if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
            Serial.print(F("M:"));
            Serial.print(localM);
            Serial.print(F(";L:"));
            Serial.println(localL);
            xSemaphoreGive(serialMutex);
        }

        // Log periodic sensor values every 30 seconds
        if (currentTime - lastPeriodicLog >= logSensorPeriod) {
            logEvent(MOISTURE, localM);
            logEvent(LIGHT, localL);
            lastPeriodicLog = currentTime;  // Update last log time
        }

        vTaskDelay(sensorDelay);
    }
}

void autoControlTask(void *pvParameters) {
    static bool wasLightLow = false;

    while (1) {
        uint16_t localM, localL;

        if (xSemaphoreTake(sensorMutex, portMAX_DELAY) == pdTRUE) {
            localM = moisture;
            localL = light;
            xSemaphoreGive(sensorMutex);
        }

        // Auto irrigation control
        if (autoIrrigation) {
            bool isOn = digitalRead(irrigationPins[0]) == HIGH;
            if (localM > moisture_threshold && !isOn) {
                setIrrigationPins(true);
                logEvent(AUTO_IRRIG, 1);
            } else if (localM <= moisture_threshold && isOn) {
                setIrrigationPins(false);
                logEvent(AUTO_IRRIG, 0);
            }
        }

        // Auto light adjustment
        if (autoLight) {
            int brightness = map(localL, 0, light_threshold, 255, 0);  // Inverse mapping
            setLightPins(constrain(brightness, 0, 255));

            bool isLightLow = (localL < light_threshold);
            if (isLightLow != wasLightLow) {
                logEvent(AUTO_LIGHT, isLightLow ? 1 : 0);
                wasLightLow = isLightLow;  // Update state tracker
            }
        }

        vTaskDelay(ctrlDelay);
    }
}

void serialComTask(void *pvParameters) {
    while (1) {
        if (Serial.available()) {
            String cmd = Serial.readStringUntil('\n');
            cmd.trim();  // Remove leading/trailing whitespace
            if (cmd.length() > 1)
                handleCommand(cmd);
            else
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

// SETUP & LOOP --------------------------------------------------------------------------------------------------------

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
    nextLogAddress = findNextEEPROMAddress();

    xTaskCreate(readSensorsTask, "Sensors", 256, NULL, 1, NULL);
    xTaskCreate(autoControlTask, "AutoCtrl", 128, NULL, 1, NULL);
    xTaskCreate(serialComTask, "Serial", 256, NULL, 2, NULL);
    xTaskCreate(updateTimeTask, "Time", 64, NULL, 1, NULL);

    vTaskStartScheduler();
}

void loop() {}

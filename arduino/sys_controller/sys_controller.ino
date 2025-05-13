#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <EEPROM.h>
#include <semphr.h>
#include <task.h>

// TODO: Prevent logging same event multiple times (LOW_MOISTURE)

// Pin Definitions
#define MOISTURE_SENSOR A2
#define LIGHT_SENSOR A3

// Irrigation on pins 6 & 7
const int irrigationPins[] = {6};
const int numIrrigation = sizeof(irrigationPins) / sizeof(irrigationPins[0]);

// Lighting (PWM) on pins 9, 10, 11, 12
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
    AUTO_IRRIGATION,  // 0
    MAN_IRRIGATION,   // 1
    AUTO_LIGHT,       // 2
    MAN_LIGHT,        // 3
    LOW_MOISTURE,     // 4
    LOW_LIGHT,        // 5
    LIGHT_TH,         // 6
    MOIST_TH          // 7
};

SemaphoreHandle_t eepromMutex;
SemaphoreHandle_t serialMutex;
SemaphoreHandle_t sensorMutex;

// Thresholds and State
bool auto_irrigation = true;
bool auto_light = true;
uint16_t moisture_threshold = 500;
uint16_t light_threshold = 500;
uint16_t moisture, light;
volatile uint32_t current_time = 0;

// RTOS Tasks
void readSensorsTask(void *pvParameters);
void autoControlTask(void *pvParameters);
void serialComTask(void *pvParameters);
void updateTimeTask(void *pvParameters);
void logEvent(uint8_t type, uint16_t value);
int findNextEEPROMAddress();

void setup() {
    Serial.begin(115200);
    EEPROM.begin();

    // Init irrigation pins
    for (int i = 0; i < numIrrigation; i++) {
        pinMode(irrigationPins[i], OUTPUT);
        digitalWrite(irrigationPins[i], LOW);
    }

    // Init light pins
    for (int i = 0; i < numLights; i++) {
        pinMode(lightPins[i], OUTPUT);
        analogWrite(lightPins[i], 0);  // off
    }

    eepromMutex = xSemaphoreCreateMutex();
    serialMutex = xSemaphoreCreateMutex();
    sensorMutex = xSemaphoreCreateMutex();
    nextLogAddress = findNextEEPROMAddress();

    xTaskCreate(readSensorsTask, "Sensors", 128, NULL, 1, NULL);
    xTaskCreate(autoControlTask, "AutoCtrl", 128, NULL, 1, NULL);
    xTaskCreate(serialComTask, "Serial", 256, NULL, 1, NULL);
    xTaskCreate(updateTimeTask, "Time", 128, NULL, 1, NULL);

    vTaskStartScheduler();
}

void loop() {}

void logEvent(uint8_t type, uint16_t value) {
    if (xSemaphoreTake(eepromMutex, portMAX_DELAY) == pdTRUE) {
        Event newEvent = {current_time, type, value};
        EEPROM.put(nextLogAddress, newEvent);
        nextLogAddress = (nextLogAddress + RECORD_SIZE) % EEPROM.length();
        xSemaphoreGive(eepromMutex);
    }
}

int findNextEEPROMAddress() {
    Event event;
    for (int addr = 0; addr < EEPROM.length(); addr += RECORD_SIZE) {
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
        for (int addr = 0; addr < EEPROM.length(); addr += RECORD_SIZE) {
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

// Clear all logs from EEPROM
void clearLogs() {
    if (xSemaphoreTake(eepromMutex, portMAX_DELAY) == pdTRUE) {
        for (int addr = 0; addr < EEPROM.length(); addr += RECORD_SIZE) {
            Event emptyEvent = {0xFFFFFFFF, 0, 0};
            EEPROM.put(addr, emptyEvent);
        }
        nextLogAddress = 0;
        xSemaphoreGive(eepromMutex);
    }
}

void handleCommand(const String &cmd) {
    if (cmd[0] == 'T') {
        current_time = cmd.substring(1).toInt();
        Serial.println(current_time);
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
    // Serial.println("k " + key + " v " + String(valStr));
    int val = valStr.toInt();
    // Serial.println(val);
    if (key == "LT") {
        light_threshold = val;
        logEvent(LIGHT_TH, light_threshold);
        return;
    }
    if (key == "MT") {
        moisture_threshold = val;
        logEvent(MOIST_TH, moisture_threshold);
        return;
    }
    if (key == "AI") {
        auto_irrigation = (val == 1);
        return;
    }
    if (key == "AL") {
        auto_light = (val == 1);
        return;
    }
    if (key == "MI") {
        bool on = (val == 1);
        setIrrigationPins(on);
        logEvent(MAN_IRRIGATION, val);
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

void setIrrigationPins(bool state) {
    for (int i = 0; i < numIrrigation; i++) {
        digitalWrite(irrigationPins[i], state ? HIGH : LOW);
    }
}

void setLightPins(int brightness) {
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

void readSensorsTask(void *pvParameters) {
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

        if (moisture < moisture_threshold) {
            logEvent(LOW_MOISTURE, moisture);
        }
        if (light < light_threshold) {
            logEvent(LOW_LIGHT, light);
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

void autoControlTask(void *pvParameters) {
    while (1) {
        uint16_t localM, localL;

        if (xSemaphoreTake(sensorMutex, portMAX_DELAY) == pdTRUE) {
            localM = moisture;
            localL = light;
            xSemaphoreGive(sensorMutex);
        }

        // Auto irrigation control
        if (auto_irrigation) {
            bool isOn = digitalRead(irrigationPins[0]) == HIGH;
            if (localM > moisture_threshold && !isOn) {
                setIrrigationPins(true);
                logEvent(AUTO_IRRIGATION, 1);
            } else if (localM <= moisture_threshold && isOn) {
                setIrrigationPins(false);
                logEvent(AUTO_IRRIGATION, 0);
            }
        }

        // Auto light adjustment
        if (auto_light) {
            int brightness = map(localL, 0, light_threshold, 255, 0);  // Inverse mapping
            brightness = constrain(brightness, 0, 255);
            setLightPins(brightness);
            if (localL < light_threshold) {
                logEvent(AUTO_LIGHT, 1);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void serialComTask(void *pvParameters) {
    while (1) {
        if (Serial.available()) {
            String cmd = Serial.readStringUntil('\n');
            cmd.trim();  // Remove leading/trailing whitespace

            // Serial.print(F("CMD:"));  // Debugging
            // Serial.println(cmd);

            if (cmd.length() > 1)
                handleCommand(cmd);
            else
                continue;
        }
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}

// Update time task to handle NTP updates
void updateTimeTask(void *pvParameters) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    while (1) {
        current_time++;  // Increment even if no NTP updates
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(1000));
    }
}

#include <Arduino_FreeRTOS.h>
#include <EEPROM.h>
#include <semphr.h>
#include <task.h>

// Pin Definitions
#define MOISTURE_SENSOR A0
#define LIGHT_SENSOR A3

// Irrigation on pins 6 & 7
const int irrigationPins[] = {6, 7};
const int numIrrigation = sizeof(irrigationPins) / sizeof(irrigationPins[0]);

// Lighting (PWM) on pins 9, 10, 11, 12
const int lightPins[] = {9, 10, 11, 12};
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
    AUTO_IRRIGATION_START,
    AUTO_IRRIGATION_STOP,
    MANUAL_IRRIGATION_START,
    MANUAL_IRRIGATION_STOP,
    AUTO_LIGHT_ADJUSTMENT,
    MANUAL_LIGHT_ADJUSTMENT,
    MOISTURE_ALERT,
    LIGHT_ALERT,
    CONFIG_CHANGE
};

SemaphoreHandle_t eepromMutex;
SemaphoreHandle_t serialMutex;

// Thresholds and State
bool auto_irrigation = true;
bool auto_light = true;
uint16_t moisture_threshold = 500;
uint16_t light_threshold = 300;
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

    EEPROM.begin();
    eepromMutex = xSemaphoreCreateMutex();
    serialMutex = xSemaphoreCreateMutex();
    nextLogAddress = findNextEEPROMAddress();

    xTaskCreate(readSensorsTask, "Sensors", 128, NULL, 2, NULL);
    xTaskCreate(autoControlTask, "AutoCtrl", 128, NULL, 2, NULL);
    xTaskCreate(serialComTask, "Serial", 256, NULL, 1, NULL);
    xTaskCreate(updateTimeTask, "Time", 64, NULL, 1, NULL);

    vTaskStartScheduler();
}

void loop() {}

void readSensorsTask(void *pvParameters) {
    while (1) {
        uint16_t moisture = analogRead(MOISTURE_SENSOR);
        uint16_t light = analogRead(LIGHT_SENSOR);

        // Send sensor readings via serial
        xSemaphoreTake(serialMutex, portMAX_DELAY);
        Serial.print("SENSORS:MOISTURE:");
        Serial.print(moisture);
        Serial.print(";LIGHT:");
        Serial.println(light);
        xSemaphoreGive(serialMutex);

        if (moisture < moisture_threshold) {
            logEvent(MOISTURE_ALERT, moisture);
        }
        if (light < light_threshold) {
            logEvent(LIGHT_ALERT, light);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void autoControlTask(void *pvParameters) {
    while (1) {
        uint16_t moisture = analogRead(MOISTURE_SENSOR);
        uint16_t light = analogRead(LIGHT_SENSOR);

        // Auto irrigation control
        if (auto_irrigation) {
            bool isOn = digitalRead(irrigationPins[0]) == HIGH;
            if (moisture < moisture_threshold && !isOn) {
                for (int i = 0; i < numIrrigation; i++)
                    digitalWrite(irrigationPins[i], HIGH);
                logEvent(AUTO_IRRIGATION_START, moisture);
            } else if (moisture >= moisture_threshold && isOn) {
                for (int i = 0; i < numIrrigation; i++)
                    digitalWrite(irrigationPins[i], LOW);
                logEvent(AUTO_IRRIGATION_STOP, moisture);
            }
        }

        // Auto light adjustment
        if (auto_light) {
            int brightness =
                map(light, 0, light_threshold, 255, 0);  // Inverse mapping
            brightness = constrain(brightness, 0, 255);
            for (int i = 0; i < numLights; i++)
                analogWrite(lightPins[i], brightness);
            if (light < light_threshold) {
                logEvent(AUTO_LIGHT_ADJUSTMENT, brightness);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void serialComTask(void *pvParameters) {
    while (1) {
        if (Serial.available()) {
            String cmd = Serial.readStringUntil('\n');
            cmd.trim();
            int colon = cmd.indexOf(':');
            if (colon != -1) {
                String key = cmd.substring(0, colon);
                key.trim();
                int val = cmd.substring(colon + 1).toInt();

                if (key == "TIME") {
                    current_time = val;
                    Serial.print("TIME_SET:");
                    Serial.println(current_time);
                } else if (key == "SET_MOISTURE_THRESH") {
                    moisture_threshold = val;
                    logEvent(CONFIG_CHANGE, moisture_threshold);
                } else if (key == "SET_LIGHT_THRESH") {
                    light_threshold = val;
                    logEvent(CONFIG_CHANGE, light_threshold);
                } else if (key == "AUTO_IRRIGATION") {
                    auto_irrigation = (val == 1);
                    logEvent(CONFIG_CHANGE, auto_irrigation ? 1 : 0);
                } else if (key == "AUTO_LIGHT") {
                    auto_light = (val == 1);
                    logEvent(CONFIG_CHANGE, auto_light ? 1 : 0);
                } else if (key == "MANUAL_IRRIGATION") {
                    bool on = (val == 1);
                    for (int i = 0; i < numIrrigation; i++)
                        digitalWrite(irrigationPins[i], on ? HIGH : LOW);
                    logEvent(
                        on ? MANUAL_IRRIGATION_START : MANUAL_IRRIGATION_STOP,
                        val);
                } else if (key == "MANUAL_LIGHT") {
                    int brightness = constrain(val, 0, 255);
                    for (int i = 0; i < numLights; i++)
                        analogWrite(lightPins[i], brightness);
                    logEvent(MANUAL_LIGHT_ADJUSTMENT, brightness);
                } else if (key == "GET_LOGS") {
                    xSemaphoreTake(serialMutex, portMAX_DELAY);
                    printLogs();
                    xSemaphoreGive(serialMutex);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
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
// Serial
void printLogs() {
    Event e;
    Serial.println(F("TS,TYPE,VALUE"));  // header row (optional)
    for (int addr = 0; addr < EEPROM.length(); addr += RECORD_SIZE) {
        EEPROM.get(addr, e);
        // We use timestamp==0xFFFFFFFF to mark “empty” slots
        if (e.timestamp == 0xFFFFFFFFUL) break;

        // Print as CSV: timestamp,event_type,event_value
        Serial.print(e.timestamp);
        Serial.print(',');
        Serial.print(e.event_type);
        Serial.print(',');
        Serial.println(e.value);
    }
}

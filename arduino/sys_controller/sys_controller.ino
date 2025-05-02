#include <Arduino.h>
#include <EEPROM.h>
#include <Arduino_FreeRTOS.h>
#include <task.h>
#include <semphr.h>

// Pin Definitions
#define MOISTURE_SENSOR A0
#define LIGHT_SENSOR A3
#define IRRIGATION_PIN 6
#define LIGHT_ADJUST_PIN 9

// EEPROM Event Log
struct Event {
    uint32_t timestamp;
    uint8_t event_type;
    uint16_t value;
};

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

const int RECORD_SIZE = sizeof(Event);
int nextLogAddress = 0;
SemaphoreHandle_t eepromMutex;
SemaphoreHandle_t serialMutex;

// Thresholds and State
uint16_t moisture_threshold = 500;
uint16_t light_threshold = 300;
bool auto_irrigation = true;
bool auto_light = true;
volatile uint32_t current_time = 0;

// RTOS Tasks
void readSensorsTask(void *pvParameters);
void autoControlTask(void *pvParameters);
void serialComTask(void *pvParameters);
void logEvent(uint8_t type, uint16_t value);
int findNextEEPROMAddress();

void setup() {
    Serial.begin(115200);
    pinMode(IRRIGATION_PIN, OUTPUT);
    pinMode(LIGHT_ADJUST_PIN, OUTPUT);

    EEPROM.begin();
    eepromMutex = xSemaphoreCreateMutex();
    serialMutex = xSemaphoreCreateMutex();
    nextLogAddress = findNextEEPROMAddress();

    xTaskCreate(readSensorsTask, "Sensors", 128, NULL, 2, NULL);
    xTaskCreate(autoControlTask, "AutoCtrl", 128, NULL, 2, NULL);
    xTaskCreate(serialComTask, "Serial", 128, NULL, 1, NULL);
    xTaskCreate(updateTimeTask, "Time", 128, NULL, 1, NULL);
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

        if (auto_irrigation) {
            if (moisture < moisture_threshold && digitalRead(IRRIGATION_PIN) == LOW) {
                digitalWrite(IRRIGATION_PIN, HIGH);
                logEvent(AUTO_IRRIGATION_START, moisture);
            } else if (moisture >= moisture_threshold && digitalRead(IRRIGATION_PIN) == HIGH) {
                digitalWrite(IRRIGATION_PIN, LOW);
                logEvent(AUTO_IRRIGATION_STOP, moisture);
            }
        }

        if (auto_light) {
            int brightness = map(light, 0, light_threshold, 255, 0);
            analogWrite(LIGHT_ADJUST_PIN, brightness);
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
                }
                else if (key == "SET_MOISTURE_THRESH") {
                    moisture_threshold = val;
                    logEvent(CONFIG_CHANGE, moisture_threshold);
                }
                else if (key == "SET_LIGHT_THRESH") {
                    light_threshold = val;
                    logEvent(CONFIG_CHANGE, light_threshold);
                }
                else if (key == "AUTO_IRRIGATION") {
                    auto_irrigation = (val == 1);
                    logEvent(CONFIG_CHANGE, auto_irrigation ? 1 : 0);
                }
                else if (key == "AUTO_LIGHT") {
                    auto_light = (val == 1);
                    logEvent(CONFIG_CHANGE, auto_light ? 1 : 0);
                }
                else if (key == "MANUAL_IRRIGATION") {
                    digitalWrite(IRRIGATION_PIN, val == 1 ? HIGH : LOW);
                    logEvent(val == 1 ? MANUAL_IRRIGATION_START : MANUAL_IRRIGATION_STOP, val);
                }
                else if (key == "MANUAL_LIGHT") {
                    int brightness = constrain(val, 0, 255);
                    analogWrite(LIGHT_ADJUST_PIN, brightness);
                    logEvent(MANUAL_LIGHT_ADJUSTMENT, brightness);
                }
            }
        }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}

// Update time task to handle NTP updates
void updateTimeTask(void *pvParameters) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    while (1) {
        current_time++; // Increment even if no NTP updates
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
        if (event.timestamp == 0xFFFFFFFF)
            return addr;
    }
    return 0;
}

#include <Arduino_FreeRTOS.h>
#include <EEPROM.h>
#include <semphr.h>
#include <task.h>

// Pin Definitions
#define MOISTURE_SENSOR A2
#define LIGHT_SENSOR A3

// Irrigation on pins 6 & 7
const int irrigationPins[] = {6, 7, 8};
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
    AUTO_IRRIGATION_START,    // 0
    AUTO_IRRIGATION_STOP,     // 1
    MANUAL_IRRIGATION_START,  // 2
    MANUAL_IRRIGATION_STOP,   // 3
    AUTO_LIGHT,               // 4
    MANUAL_LIGHT,             // 5
    MOISTURE_ALERT,           // 6
    LIGHT_ALERT,              // 7
    CONFIG_CHANGE             // 8
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
    sensorMutex = xSemaphoreCreateMutex();
    nextLogAddress = findNextEEPROMAddress();

    xTaskCreate(readSensorsTask, "Sensors", 128, NULL, 2, NULL);
    xTaskCreate(autoControlTask, "AutoCtrl", 128, NULL, 2, NULL);
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
// Serial
void printLogs() {
    Event e;
    Serial.println(F("TS,TYPE,VALUE"));  // header row
    for (int addr = 0; addr < EEPROM.length(); addr += RECORD_SIZE) {
        EEPROM.get(addr, e);
        // We use timestamp==0xFFFFFFFF to mark “empty” slots
        if (e.timestamp == 0xFFFFFFFFUL) break;

        // Print as CSV: timestamp,event_type,event_value
        Serial.print(e.timestamp);
        Serial.print(F(","));
        Serial.print(e.event_type);
        Serial.print(F(","));
        Serial.println(e.value);
    }
    Serial.println(F("END_LOG"));  // end of log marker
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

void handleCommand(const String &key, int val) {
    if (key == "TIME") {
        current_time = val;
    } else if (key == "M_THRESH") {
        moisture_threshold = val;
        logEvent(CONFIG_CHANGE, moisture_threshold);
    } else if (key == "L_THRESH") {
        light_threshold = val;
        logEvent(CONFIG_CHANGE, light_threshold);
    } else if (key == "AUTO_I") {
        auto_irrigation = (val == 1);
        logEvent(CONFIG_CHANGE, auto_irrigation ? 1 : 0);
    } else if (key == "AUTO_L") {
        auto_light = (val == 1);
        logEvent(CONFIG_CHANGE, auto_light ? 1 : 0);
    } else if (key == "MAN_I") {
        bool on = (val == 1);
        setIrrigationPins(on);
        logEvent(on ? MANUAL_IRRIGATION_START : MANUAL_IRRIGATION_STOP, val);
    } else if (key == "MAN_L") {
        int brightness = constrain(val, 0, 255);
        setLightPins(brightness);
        logEvent(MANUAL_LIGHT, brightness);
    } else if (key == "GET_LOGS") {
        if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
            printLogs();
            xSemaphoreGive(serialMutex);
        }
    } else if (key == "CLEAR_LOGS") {
        clearLogs();
    }
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

// void monitorStackUsage() {
//     UBaseType_t highWaterMark = uxTaskGetStackHighWaterMark(NULL);
//     Serial.print("Stack high water mark: ");
//     Serial.println(highWaterMark);
// }

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
            logEvent(MOISTURE_ALERT, moisture);
        }
        if (light < light_threshold) {
            logEvent(LIGHT_ALERT, light);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
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
                logEvent(AUTO_IRRIGATION_START, localM);
            } else if (localM <= moisture_threshold && isOn) {
                setIrrigationPins(false);
                logEvent(AUTO_IRRIGATION_STOP, localM);
            }
        }

        // Auto light adjustment
        if (auto_light) {
            int brightness =
                map(localL, 0, light_threshold, 255, 0);  // Inverse mapping
            brightness = constrain(brightness, 0, 255);
            setLightPins(brightness);
            if (localL < light_threshold) {
                logEvent(AUTO_LIGHT, brightness);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void serialComTask(void *pvParameters) {
    while (1) {
        if (Serial.available()) {
            String cmd = Serial.readStringUntil('\n');
            cmd.trim();  // Remove leading/trailing whitespace
            // Serial.print(F("Received command: "));  // Debugging
            // Serial.println(cmd);

            // Debug raw command bytes
            // Serial.print(F("Raw command bytes: "));
            // for (size_t i = 0; i < cmd.length(); i++) {
            //     Serial.print((int)cmd[i]);
            //     Serial.print(" ");
            // }
            // Serial.println();

            int colon = cmd.indexOf(':');
            if (colon != -1) {
                String key = cmd.substring(0, colon);
                key.trim();  // Remove any extra spaces
                String valStr = cmd.substring(colon + 1);
                valStr.trim();  // Remove any extra spaces

                // Ensure valStr contains only numeric characters
                for (int i = 0; i < valStr.length(); i++) {
                    if (!isDigit(valStr[i])) {
                        Serial.println(
                            F("Error: Non-numeric value in command"));
                        return;
                    }
                }

                int val = valStr.toInt();
                // Serial.print(F("Parsed key: "));  // Debugging
                // Serial.println(key);
                // Serial.print(F("Parsed value: "));  // Debugging
                // Serial.println(val);
                handleCommand(key, val);
            } else {
                if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
                    Serial.print(F("UNKNOWN_CMD: "));
                    Serial.println(cmd);
                    xSemaphoreGive(serialMutex);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// Update time task to handle NTP updates
void updateTimeTask(void *pvParameters) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    while (1) {
        current_time++;  // Increment even if no NTP updates
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(10000));
    }
}

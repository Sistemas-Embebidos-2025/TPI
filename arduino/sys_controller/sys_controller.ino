#include <EEPROM.h>
#include <Arduino_FreeRTOS.h>

// Pin Definitions
#define MOISTURE_SENSOR A0
#define LIGHT_SENSOR_PIN A3
#define IRRIGATION_PIN 9
#define EEPROM_LOG_START 100

// EEPROM Structure
struct Config {
  int moisture_thresh;
  int light_thresh;
};

struct EventLog {
  uint32_t timestamp;
  char event_type[20];
  int value;
};

// Global Variables
Config config;
int moisture, light;
bool auto_mode = true;
QueueHandle_t xEventQueue;

void TaskSensorRead(void *pv) {
  for (;;) {
    moisture = analogRead(MOISTURE_SENSOR);
    light = analogRead(LIGHT_SENSOR_PIN);
    vTaskDelay(pdMS_TO_TICKS(1000));  // Read every second
  }
}

void TaskAutoControl(void *pv) {
  for (;;) {
    if (auto_mode) {
      // Automatic irrigation control
      if (moisture < config.moisture_thresh) {
        digitalWrite(IRRIGATION_PIN, HIGH);
        logEvent("AUTO_IRRIGATION", 1);
      } else {
        digitalWrite(IRRIGATION_PIN, LOW);
      }

      // Light adjustment
      // int light_level = map(light, 0, config.light_thresh, 255, 0);
      if (light < config.light_thresh) {
        analogWrite(IRRIGATION_PIN, 255);
        logEvent("AUTO_LIGHT", 1);
      } else {
        analogWrite(IRRIGATION_PIN, 0);
      }
      analogWrite(9, constrain(light_level, 0, 255));
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void TaskSerialComm(void *pv) {
  for (;;) {
    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim();
      processCommand(cmd);
    }
    sendSensorData();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void processCommand(String cmd) {
  // Handle threshold updates
  if (cmd.startsWith("THRESHOLD:")) {
    int firstColon = cmd.indexOf(':');                   // Position of first colon
    int secondColon = cmd.indexOf(':', firstColon + 1);  // Position of second colon

    String type = cmd.substring(firstColon + 1, secondColon);  // "light" or "moisture"
    int value = cmd.substring(secondColon + 1).toInt();        // Threshold value

    if (type == "light") {
      config.light_thresh = value;
    } else if (type == "moisture") {
      config.moisture_thresh = value;
    } else {
      Serial.println("ERROR:INVALID_TYPE");
      return;
    }

    // Save to EEPROM
    EEPROM.put(0, config);
    logEvent("THRESHOLD_CHANGE", value);
  }

  // Handle mode changes (AUTO/MANUAL)
  else if (cmd.startsWith("MODE:")) {
    String mode = cmd.substring(5);  // "AUTO" or "MANUAL"
    auto_mode = (mode == "AUTO");
    logEvent("MODE_CHANGE", auto_mode);
  }

  // Handle log requests
  else if (cmd == "GET_LOGS") {
    // Send logs via serial
    for (int addr = EEPROM_LOG_START; addr < EEPROM.length(); addr += sizeof(EventLog)) {
      EventLog log;
      EEPROM.get(addr, log);
      Serial.print("LOG:");
      Serial.print(log.timestamp);
      Serial.print(",");
      Serial.print(log.event_type);
      Serial.print(",");
      Serial.println(log.value);
    }
  }

  // Error handling for unknown commands
  else {
    Serial.println("ERROR:INVALID_COMMAND");
  }
}

void logEvent(const char *type, int value) {
  EventLog log;
  log.timestamp = millis();
  strncpy(log.event_type, type, 19);
  log.value = value;

  static int log_addr = EEPROM_LOG_START;
  EEPROM.put(log_addr, log);
  log_addr = (log_addr + sizeof(EventLog)) % (EEPROM.length() - sizeof(EventLog));
}

void setup() {
  Serial.begin(57600);
  pinMode(IRRIGATION_PIN, OUTPUT);

  // Load config from EEPROM
  EEPROM.get(0, config);

  xTaskCreate(TaskSensorRead, "Sensors", 128, NULL, 1, NULL);
  xTaskCreate(TaskAutoControl, "Control", 128, NULL, 2, NULL);
  xTaskCreate(TaskSerialComm, "Serial", 128, NULL, 1, NULL);
}

void loop() {}

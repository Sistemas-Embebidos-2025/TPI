import re
import sys
import time
from datetime import datetime, timedelta
from threading import Thread, Lock

import ntplib
import serial
from flask import Flask, render_template, request
from flask_socketio import SocketIO

from config import SERIAL_TIMEOUT, BAUD_RATE, SERIAL_PORT, NTP_SERVER, TIME_ZONE_OFFSET, logger

app = Flask(__name__)
socketio = SocketIO(app)

try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=SERIAL_TIMEOUT)
except serial.SerialException as port_error:
    logger.error(f"Error opening serial port: {port_error}")
    ser = None  # Set ser to None if port cannot be opened

lock = Lock()

MOISTURE_THRESH = 500
LIGHT_THRESH = 500

CMD_TIME = 'T'
CMD_SET_LIGHT_THRESH = 'L'
CMD_SET_MOISTURE_THRESH = 'M'
CMD_GET_LIGHT_THRESH = "Z"
CMD_GET_MOISTURE_THRESH = "X"
CMD_CLEAR_LOGS = "D"
CMD_GET_LOGS = "G"
CMD_UNK = "U"
LOG_END = "E"
LOG_HEADER = "TS,T,V"
MEASUREMENT_MOISTURE = 'm'
MEASUREMENT_LIGHT = 'l'
KET_LIGHT_THRESH = 'LT'
KEY_MOISTURE_THRESH = 'MT'


def get_ntp_time():
    client = ntplib.NTPClient()
    try:
        response = client.request(NTP_SERVER, version=4)
        # Adjusting with local offset AFTER getting UTC timestamp
        return response.tx_time + TIME_ZONE_OFFSET * 3600
    except Exception as e:
        logger.warning(f"NTP Error, syncing with system time: {e}")
        # Fallback to system time, ensuring it's timezone-aware if possible,
        return (datetime.now() + timedelta(hours=TIME_ZONE_OFFSET)).timestamp()


def sync_time():
    """Periodic time synchronization loop."""
    logger.info("Starting periodic time sync.")
    while True:
        sync_time_internal()  # Attempt sync
        time.sleep(3600)  # Sync periodically (e.g., every hour)


def sync_time_internal():
    """Attempts a single time sync."""
    try:
        current_time = int(get_ntp_time())
        command = f"{CMD_TIME}{current_time}\n"
        send_arduino_command(command)
        return True  # Indicate success
    except ntplib.NTPException as e:
        logger.warning(f"NTP Error, syncing with system time: {e}")
        return False
    except Exception as e:
        logger.error(f"Initial Time sync failed: {e}")
        return False


def request_initial_thresholds():
    """Requests initial thresholds from Arduino."""
    logger.info("Requesting initial moisture threshold...")
    send_arduino_command("%s\n" % CMD_GET_MOISTURE_THRESH)
    logger.info("Requesting initial light threshold...")
    send_arduino_command("%s\n" % CMD_GET_LIGHT_THRESH)


def serial_reader():
    global MOISTURE_THRESH, LIGHT_THRESH
    reading_logs = False
    current_logs = []
    log_pattern = re.compile(r"^(\d+),(\d+),(\d+)$")

    # Regex for threshold values from Arduino
    moisture_th_pattern = re.compile(rf"^{CMD_GET_MOISTURE_THRESH}(\d+)$")
    light_th_pattern = re.compile(rf"^{CMD_GET_LIGHT_THRESH}(\d+)$")
    sensor_data_pattern = re.compile(rf"^{MEASUREMENT_MOISTURE}(\d+){MEASUREMENT_LIGHT}(\d+)$")

    while True:
        if reading_logs and (not ser or not ser.is_open):
            logger.warning("Serial port closed or error while reading logs. Sending partial logs.")
            if current_logs:
                socketio.emit("log_data", {"logs": current_logs})
            current_logs, reading_logs = reset_logs()
            time.sleep(1)
            continue

        line = None
        try:
            if ser and ser.in_waiting > 0:
                line = ser.readline().decode(errors="ignore").strip()
        except serial.SerialException as e:
            logger.error(f"Serial Exception during read: {e}")
            if reading_logs:
                socketio.emit("error", {"message": f"Serial error during log read: {e}"})
                current_logs, reading_logs = reset_logs()
            time.sleep(1)
            continue
        except UnicodeDecodeError:
            logger.error("Serial read warning: UnicodeDecodeError")
            socketio.emit("error", {"message": "UnicodeDecodeError in serial read"})
            line = None
        except Exception as e:
            logger.error(f"Unexpected error in serial_reader loop: {e}")
            if reading_logs:
                socketio.emit("error", {"message": f"Unexpected error: {e}"})
                current_logs, reading_logs = reset_logs()
            time.sleep(1)
            continue

        if line is None:
            time.sleep(0.1)  # Prevent busy-waiting
            continue

        if reading_logs:
            current_logs, reading_logs = read_logs(current_logs, line, log_pattern, reading_logs)
        else:
            current_logs, reading_logs = handle_command(current_logs, light_th_pattern, line, moisture_th_pattern, reading_logs, sensor_data_pattern)


def handle_command(current_logs, light_th_pattern, line, moisture_th_pattern, reading_logs, sensor_data_pattern):
    global MOISTURE_THRESH, LIGHT_THRESH
    # Check for threshold responses
    mt_match = moisture_th_pattern.match(line)
    lt_match = light_th_pattern.match(line)
    sensor_match = sensor_data_pattern.match(line)
    if mt_match:
        try:
            MOISTURE_THRESH = int(mt_match.group(1))
            logger.info(f"Received initial moisture threshold: {MOISTURE_THRESH}")
            socketio.emit("threshold_update", {
                "key": KEY_MOISTURE_THRESH,
                "value": MOISTURE_THRESH
            })
        except ValueError:
            logger.error(f"Could not parse moisture threshold value from: {line}")
            socketio.emit("error", {"message": f"Could not parse moisture threshold value from: {line}"})
    elif lt_match:
        try:
            LIGHT_THRESH = int(lt_match.group(1))
            logger.info(f"Received initial light threshold: {LIGHT_THRESH}")
            socketio.emit("threshold_update", {
                "key": KET_LIGHT_THRESH,
                "value": LIGHT_THRESH
            })
        except ValueError:
            logger.error(f"Could not parse light threshold value from: {line}")
            socketio.emit("error", {"message": f"Could not parse light threshold value from: {line}"})
    elif line == LOG_HEADER:
        logger.info(f"Detected log header, starting log capture.")
        reading_logs = True
        current_logs = []
    elif sensor_match:  # Sensor data m{moisture}l{light}
        data = {}
        l_part = line.find(MEASUREMENT_LIGHT)
        if l_part != -1:
            moisture_str = line[1:l_part]
            light_str = line[l_part + 1:]
            try:
                data[CMD_SET_MOISTURE_THRESH] = int(moisture_str)
                data[CMD_SET_LIGHT_THRESH] = int(light_str)
                socketio.emit("sensor_update", {
                    "moisture": data.get(CMD_SET_MOISTURE_THRESH, 0),
                    "light": data.get(CMD_SET_LIGHT_THRESH, 0)
                })
            except ValueError:
                logger.error(f"Could not parse moisture value from: {line}")
                socketio.emit("error", {"message": f"Could not parse sensor values: {line}"})
        else:
            logger.error(f"Could not parse sensor value from: {line}")
            socketio.emit("error", {"message": f"Malformed sensor data: {line}"})
    elif line.startswith(CMD_UNK):
        logger.error(f"Command was unrecognized by Arduino: {line[1:]}")
        socketio.emit("error", {"message": f"Arduino error: {line[1:]}"})
    else:
        if line:  # Avoid printing empty lines if any
            logger.debug(f"Received serial data: {line}")
    return current_logs, reading_logs


def read_logs(current_logs, line, log_pattern, reading_logs):
    if line == LOG_END:
        logger.info("Detected E (End logs) marker.")
        socketio.emit("log_data", {"logs": current_logs})
        logger.debug(f"Current logs: {current_logs}")
        current_logs, reading_logs = reset_logs()
    else:
        match = log_pattern.match(line)
        if match:
            timestamp, event_type, value = match.groups()
            current_logs.append({
                "timestamp": int(timestamp),
                "type": int(event_type),
                "value": int(value)
            })
        else:
            logger.warning(f"Received unexpected line while reading logs: '{line}'")
    return current_logs, reading_logs


def reset_logs():
    return [], False


@app.route('/')
def index():
    return render_template("index.html")


@socketio.on("threshold_update")
def threshold_update(data):
    global MOISTURE_THRESH, LIGHT_THRESH
    key = data.get("key")
    value = data.get("value")
    logger.info(f"Received new threshold value: {value}")
    if key not in [KEY_MOISTURE_THRESH, KET_LIGHT_THRESH]:
        logger.error(f"Invalid key for threshold update: {key}")
        socketio.emit("error", {"message": f"Invalid threshold key: {key}"})
        return

    try:
        val_int = max(min(int(value), 1023), 0)
        command_prefix = CMD_SET_MOISTURE_THRESH if key == KEY_MOISTURE_THRESH else CMD_SET_LIGHT_THRESH
        command = f"{command_prefix}{val_int}\n"
        success_arduino = send_arduino_command(command)

        if success_arduino:
            # Update server-side stored threshold
            if key == KEY_MOISTURE_THRESH:
                MOISTURE_THRESH = val_int
            else:  # key == KET_LIGHT_THRESH
                LIGHT_THRESH = val_int

            logger.info(f"Broadcasting synced threshold: {key} = {val_int}")
            socketio.emit("threshold_update", {"key": key, "value": val_int})

    except ValueError:
        logger.error(f"Could not parse threshold value from: {value}")
        socketio.emit("error", {"message": f"Invalid threshold value: {value}"}, room=request.sid)
    except Exception as e:  # Catch any other unexpected errors
        logger.error(f"Unexpected error during threshold update: {e}")
        socketio.emit("error", {"message": f"Unexpected error during threshold update: {e}"}, room=request.sid)


@socketio.on("logs_request")
def handle_log_request():
    logger.info("Received log request from client.")
    command = "%s\n" % CMD_GET_LOGS
    if send_arduino_command(command):
        socketio.emit("log_request_sent", {"message": "Log request sent to Arduino."})


@socketio.on("clear_logs_request")
def handle_clear_logs_request():
    """
    Handles request from frontend to clear logs.
    """

    logger.info("Received clear log request from client.")
    command = "%s\n" % CMD_CLEAR_LOGS
    send_arduino_command(command)


@socketio.on("connect")
def handle_connect():
    logger.info(f"Client connected with SID: {request.sid}")
    if MOISTURE_THRESH is not None:  # Check if it has a value
        socketio.emit("threshold_update", {
            "key": KEY_MOISTURE_THRESH,
            "value": MOISTURE_THRESH
        }, room=request.sid)
    if LIGHT_THRESH is not None:  # Check if it has a value
        socketio.emit("threshold_update", {
            "key": KET_LIGHT_THRESH,
            "value": LIGHT_THRESH
        }, room=request.sid)


def send_arduino_command(command_str: str):
    """Sends a command string to the Arduino, ensuring it's newline terminated."""
    if not ser:
        logger.error(f"Arduino not connected to a serial port. Cannot send command: {command_str}")
        return False
    try:
        full_command = command_str if command_str.endswith('\n') else f"{command_str}\n"
        with lock:
            ser.write(full_command.encode())
        logger.info(f"Sent command to Arduino: {full_command}")
        time.sleep(0.1)
        return True
    except Exception as e:
        logger.error(f"Error sending command '{command_str}': {e}")
        socketio.emit("error", {"message": f"Error sending command {command_str}: {e}"})
        return False


if __name__ == "__main__":
    if ser:
        logger.info(f"Arduino connected to a serial port.")
        time.sleep(2)  # Allow Arduino to settle
        ser.reset_input_buffer()  # Clear any stale data from Arduino's buffer
        logger.info("Attempting initial time sync...")
        if not sync_time_internal():
            logger.warning("Initial time sync failed. Arduino time may be incorrect until next sync.")
            socketio.emit("error", {"message": "Initial time sync failed. Arduino time may be incorrect until next sync."})
        else:
            logger.info("Initial time sync command sent successfully.")

        request_initial_thresholds()

        Thread(target=sync_time, daemon=True).start()
        Thread(target=serial_reader, daemon=True).start()
    else:
        logger.error("Arduino not connected to a serial port.")
        sys.exit(1)

    socketio.run(app, host="0.0.0.0", port=5000, allow_unsafe_werkzeug=True)

import re
import time
from datetime import datetime, timedelta
from threading import Thread, Lock

import ntplib
import serial
from flask import Flask, render_template, request
from flask_socketio import SocketIO

from config import SERIAL_TIMEOUT, BAUD_RATE, SERIAL_PORT, NTP_SERVER, TIME_ZONE_OFFSET

app = Flask(__name__)
socketio = SocketIO(app)

try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=SERIAL_TIMEOUT)
except serial.SerialException as port_error:
    print(f"Error opening serial port: {port_error}")
    ser = None  # Set ser to None if port cannot be opened

lock = Lock()

MOISTURE_THRESH = 500
LIGHT_THRESH = 500


def get_ntp_time():
    client = ntplib.NTPClient()
    try:
        response = client.request(NTP_SERVER, version=4)
        # Adjusting with local offset AFTER getting UTC timestamp
        return response.tx_time + TIME_ZONE_OFFSET * 3600
    except Exception as e:
        print(f"NTP Error: {e}")
        # Fallback to system time, ensuring it's timezone-aware if possible,
        return (datetime.now() + timedelta(hours=TIME_ZONE_OFFSET)).timestamp()


def sync_time_internal():
    """Attempts a single time sync."""
    try:
        current_time = int(get_ntp_time())
        command = f"T{current_time}\n"
        send_arduino_command(command)
        return True  # Indicate success
    except ntplib.NTPException as e:
        print(f"Initial NTP sync failed: {e}")
        return False
    except Exception as e:
        print(f"Initial Time sync failed: {e}")
        return False


def sync_time():
    """Periodic time synchronization loop."""
    print("Starting periodic time sync...")
    while True:
        sync_time_internal()  # Attempt sync
        time.sleep(3600)  # Sync periodically (e.g., every hour)


def request_initial_thresholds():
    """Requests initial thresholds from Arduino."""
    print("Requesting initial moisture threshold...")
    send_arduino_command("X\n")
    print("Requesting initial light threshold...")
    send_arduino_command("Z\n")


def serial_reader():
    global MOISTURE_THRESH, LIGHT_THRESH
    reading_logs = False
    current_logs = []
    log_pattern = re.compile(r"^(\d+),(\d+),(\d+)$")

    # Regex for threshold values from Arduino
    moisture_th_pattern = re.compile(r"^X(\d+)$")
    light_th_pattern = re.compile(r"^Z(\d+)$")
    sensor_data_pattern = re.compile(r"^m(\d+)l(\d+)$")

    while True:
        if reading_logs and (not ser or not ser.is_open):
            print("Serial port closed or error while reading logs. Sending partial logs.")
            if current_logs:
                socketio.emit('log_data', {'logs': current_logs})
            current_logs, reading_logs = reset_logs()
            time.sleep(1)
            continue

        line = None
        try:
            if ser and ser.in_waiting > 0:
                line = ser.readline().decode(errors='ignore').strip()
            elif reading_logs:
                print("Timeout waiting for log data or E (End logs). Sending collected logs.")
                if current_logs:  # Ensure current_logs is not empty
                    socketio.emit('log_data', {'logs': current_logs})
                current_logs, reading_logs = reset_logs()
                time.sleep(1)
                continue
        except serial.SerialException as e:
            print(f"Serial Exception during read: {e}")
            if reading_logs:
                socketio.emit('error', {'message': f'Serial error during log read: {e}'})
                current_logs, reading_logs = reset_logs()
            time.sleep(1)
            continue
        except UnicodeDecodeError:
            print("Serial read warning: UnicodeDecodeError")
            socketio.emit('error', {'message': 'UnicodeDecodeError in serial read'})
            line = None
        except Exception as e:
            print(f"Unexpected error in serial_reader loop: {e}")
            if reading_logs:
                socketio.emit('error', {'message': f'Unexpected error: {e}'})
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
            print(f"Received initial moisture threshold: {MOISTURE_THRESH}")
            socketio.emit('threshold_update', {
                'key': 'MT',
                'value': MOISTURE_THRESH
            })
        except ValueError:
            print(f"Could not parse moisture threshold value from: {line}")
    elif lt_match:
        try:
            LIGHT_THRESH = int(lt_match.group(1))
            print(f"Received initial light threshold: {LIGHT_THRESH}")
            socketio.emit('threshold_update', {
                'key': 'LT',
                'value': LIGHT_THRESH
            })
        except ValueError:
            print(f"Could not parse light threshold value from: {line}")
    elif line == "TS,T,V":
        print("Detected log header, starting log capture.")
        reading_logs = True
        current_logs = []
    elif sensor_match:  # Sensor data m{moisture}l{light}
        data = {}
        l_part = line.find('l')
        if l_part != -1:
            moisture_str = line[1:l_part]
            light_str = line[l_part + 1:]
            try:
                data['M'] = int(moisture_str)
                data['L'] = int(light_str)
                socketio.emit('sensor_update', {
                    'moisture': data.get('M', 0),
                    'light': data.get('L', 0)
                })
            except ValueError:
                print(f"Could not parse sensor values from: {line}")
                socketio.emit('error', {'message': f'Could not parse sensor values: {line}'})
        else:
            print(f"Malformed sensor data line: {line}")
            socketio.emit('error', {'message': f'Malformed sensor data: {line}'})
    elif line.startswith("U"):
        print(f"Arduino error handling command: {line[1:]}")
        socketio.emit('error', {'message': f'Arduino error: {line[1:]}'})
    else:
        if line:  # Avoid printing empty lines if any
            print(f"Received serial data: {line}")
    return current_logs, reading_logs


def read_logs(current_logs, line, log_pattern, reading_logs):
    if line == "E":
        print("Detected E (End logs) marker.")
        socketio.emit('log_data', {'logs': current_logs})
        print(f"Logs:\n{current_logs}\n")
        current_logs, reading_logs = reset_logs()
    else:
        match = log_pattern.match(line)
        if match:
            timestamp, event_type, value = match.groups()
            current_logs.append({
                'timestamp': int(timestamp),
                'type': int(event_type),
                'value': int(value)
            })
        else:
            print(f"Warning: Received unexpected line while reading logs: '{line}'")
    return current_logs, reading_logs


def reset_logs():
    return [], False


@app.route('/')
def index():
    return render_template('index.html')


@socketio.on("threshold_update")
def threshold_update(data):
    global MOISTURE_THRESH, LIGHT_THRESH
    key = data.get('key')
    value = data.get('value')
    print(f"Received threshold update from client from client SID {request.sid}: {key} = {value}")

    if key not in ['MT', 'LT']:
        print(f"Invalid key for threshold update: {key}")
        socketio.emit('error', {'message': f'Invalid threshold key: {key}'})
        return

    try:
        val_int = max(min(int(value), 1023), 0)
        command_prefix = 'M' if key == 'MT' else 'L'
        command = f"{command_prefix}{val_int}\n"
        success_arduino = send_arduino_command(command)

        if success_arduino:
            # Update server-side stored threshold
            if key == 'MT':
                MOISTURE_THRESH = val_int
            else:  # LT
                LIGHT_THRESH = val_int

            print(f"Broadcasting synced threshold: {key} = {val_int}")
            socketio.emit('threshold_update', {'key': key, 'value': val_int})

    except ValueError:
        print(f"Invalid value for threshold update: {value}")
        socketio.emit('error', {'message': f'Invalid threshold value: {value}'}, room=request.sid)
    except Exception as e:  # Catch any other unexpected errors
        print(f"Unexpected error during threshold update: {e}")
        socketio.emit('error', {'message': f'Unexpected error during threshold update: {e}'}, room=request.sid)


@socketio.on("logs_request")
def handle_log_request():
    print("Received log request from client.")
    command = "G\n"
    if send_arduino_command(command):
        socketio.emit('log_request_sent', {'message': 'Log request sent to Arduino.'})


@socketio.on("clear_logs_request")
def handle_clear_logs_request():
    """
    Handles request from frontend to clear logs.
    """

    print("Received clear logs request from client.")
    command = "D\n"
    send_arduino_command(command)


@socketio.on('connect')
def handle_connect():
    print(f'Client connected with SID: {request.sid}')
    if MOISTURE_THRESH is not None:  # Check if it has a value
        socketio.emit('threshold_update', {
            'key': 'MT',
            'value': MOISTURE_THRESH
        }, room=request.sid)  # 'room=request.sid' sends only to this client
    if LIGHT_THRESH is not None:  # Check if it has a value
        socketio.emit('threshold_update', {
            'key': 'LT',
            'value': LIGHT_THRESH
        }, room=request.sid)


def send_arduino_command(command_str: str):
    """Sends a command string to the Arduino, ensuring it's newline terminated."""
    if not ser:
        print(f"Serial port not available. Cannot send command: {command_str}")
        return False
    try:
        full_command = command_str if command_str.endswith('\n') else f"{command_str}\n"
        with lock:
            ser.write(full_command.encode())
        print(f"Sent command to Arduino: {full_command.strip()}")
        time.sleep(0.1)
        return True
    except Exception as e:
        print(f"Error sending command '{command_str}': {e}")
        socketio.emit('error', {'message': f'Error sending command {command_str}: {e}'})
        return False


if __name__ == '__main__':
    if ser:
        print(f"Serial port {ser.name} opened successfully.")
        time.sleep(2)  # Allow Arduino to settle
        ser.reset_input_buffer()  # Clear any stale data from Arduino's buffer
        print("Attempting initial time sync...")
        if not sync_time_internal():
            print("Initial time sync failed. Arduino time may be incorrect until next sync.")
        else:
            print("Initial time sync command sent successfully.")

        request_initial_thresholds()

        Thread(target=sync_time, daemon=True).start()
        Thread(target=serial_reader, daemon=True).start()
    else:
        print("Running without serial connection.")

    socketio.run(app, host='0.0.0.0', port=5000, allow_unsafe_werkzeug=True)

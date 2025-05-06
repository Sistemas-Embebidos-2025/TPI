import re  # Import regex module
import time
from datetime import datetime, timedelta
from threading import Thread, Lock

import ntplib
import serial
from flask import Flask, render_template
from flask_socketio import SocketIO

app = Flask(__name__)
socketio = SocketIO(app)
# Ensure you use the correct COM port for your system
try:
    # ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1) # Linux
    ser = serial.Serial('COM3', 115200, timeout=1)  # Opens the serial port COMX (WINDOWS)
except serial.SerialException as port_error:
    print(f"Error opening serial port: {port_error}")
    ser = None  # Set ser to None if port cannot be opened

lock = Lock()

# Threshold defaults
MOISTURE_THRESH = 500
LIGHT_THRESH = 300

# NTP Server configuration
NTP_SERVER = 'pool.ntp.org'
TIME_ZONE_OFFSET = -3  # Argentina Time (UTC-3)


# ---- NTP and Time Sync Functions (keep as is) ----
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


def sync_time():
    while True:
        if not ser: time.sleep(60); continue  # Don't try if serial is not open
        try:
            current_time = int(get_ntp_time())
            with lock:
                ser.write(f"TIME:{current_time}\n".encode())
            print(f"Synced time: {datetime.fromtimestamp(current_time)}")
        except Exception as e:
            print(f"Time sync failed: {e}")

        time.sleep(3600)  # Sync every hour


def serial_reader():
    # SENSORS:MOISTURE:369;LIGHT:158
    reading_logs = False
    current_logs = []
    log_pattern = re.compile(r"^(\d+),(\d+),(\d+)$")  # Regex for TS,TYPE,VALUE
    while True:
        if not ser or ser.in_waiting <= 0:  # Check if serial is open and has data
            time.sleep(100)
            # If we were reading logs and input stops, send what we have
            if reading_logs and current_logs:
                socketio.emit('log_data', {'logs': current_logs})
                print(f"Finished reading logs, {len(current_logs)} entries.")
                reading_logs = False
                current_logs = []
            continue
        try:
            # Reads a full line of data, decodes the bytes to a string, and removes leading/trailing whitespaces.
            line = ser.readline().decode(errors='ignore').strip()
            if not line:
                # Empty line often signifies end of transmission or pause
                if reading_logs:
                    socketio.emit('log_data', {'logs': current_logs})
                    print(f"Finished reading logs (empty line), {len(current_logs)} entries.")
                    reading_logs = False
                    current_logs = []
                continue  # Skip processing empty lines further

            # --- Standard message processing ---
            if not reading_logs:
                if line.startswith("SENSORS:"):
                    # Parse sensor data
                    line_data = line.replace("SENSORS:", "")
                    data = {}
                    parts = line_data.split(';')
                    for part in parts:
                        if ':' in part:
                            key, val = part.split(':', 1)
                            try:
                                data[key] = int(val)
                            except ValueError:
                                print(f"Warning: Could not parse sensor value '{val}' for key '{key}'")
                                data[key] = -1
                    socketio.emit('sensor_update', {
                        'moisture': data.get('MOISTURE', 0),
                        'light': data.get('LIGHT', 0),
                        'timestamp': datetime.now().isoformat()
                    })
                elif line.startswith("TIME_SET:"):
                    try:
                        timestamp = int(line.split(':')[1])
                        human_time = datetime.fromtimestamp(timestamp).isoformat()
                        socketio.emit('time_update', {'timestamp': timestamp, 'human': human_time})
                    except (IndexError, ValueError):
                        print(f"Warning: Could not parse TIME_SET line: {line}")
                elif line == "TS,TYPE,VALUE":  # Check for log header
                    print("Detected log header, starting log capture.")
                    reading_logs = True
                    current_logs = []  # Clear previous logs if any
                else:
                    # Emit other non-log, non-sensor data if needed
                    socketio.emit('serial_data', {'data': line})
            # --- Log message processing ---
            elif reading_logs:
                match = log_pattern.match(line)
                if match:
                    timestamp, event_type, value = match.groups()
                    current_logs.append({
                        'timestamp': int(timestamp),
                        'type': int(event_type),
                        'value': int(value)
                    })
                else:
                    # Line doesn't match log format, assume logs finished
                    print(f"Detected non-log line ('{line}'), finishing log capture.")
                    socketio.emit('log_data', {'logs': current_logs})
                    reading_logs = False
                    current_logs = []
                    # Process this line as a normal message if needed
                    if line.startswith("SENSORS:") or line.startswith("TIME_SET:"):
                        # Handle cases where normal messages might arrive unexpectedly
                        print(f"Processing standard message received while expecting logs: {line}")
                        pass  # Or re-process based on the line content
                    else:
                        socketio.emit('serial_data', {'data': line})
        except UnicodeDecodeError:
            print("Serial read warning: UnicodeDecodeError")  # Handle potential garbage data
        except Exception as e:
            print(f"Serial read error: {e}")
            # Reset log reading state on error to avoid getting stuck
            if reading_logs:
                print("Resetting log reading state due to error.")
                reading_logs = False
                current_logs = []


# --- Route and SocketIO handlers ---

@app.route('/')
def index():
    """
    Renders the main page of the web application.
    """
    return render_template('index.html')


@socketio.on("threshold_update")
def threshold_update(data):
    if not ser: return
    key = data.get('key')
    value = data.get('value')
    print(f"Received threshold update: {key} = {value}")  # Debug print
    if key not in ['SET_MOISTURE_THRESH', 'SET_LIGHT_THRESH']:
        print(f"Invalid key for threshold update: {key}")
        return

    try:
        val_int = int(value)  # Check if value is integer
        command = f"{key}:{val_int}\n"  # Construct command like "SET_MOISTURE_THRESH:500\n"
        with lock:
            ser.write(command.encode())
        print(f"Sent command: {command.strip()}")
    except ValueError:
        print(f"Invalid value for threshold update: {value}")
    except Exception as e:
        print(f"Error sending threshold update: {e}")


@socketio.on("toggle_auto_control")
def toggle_auto_control(data):
    if not ser: return  # Check serial port
    key = data.get('key')
    state = data.get('state')
    print(f"Received toggle auto control: {key} = {state}")  # Debug print
    if key not in ['AUTO_IRRIGATION', 'AUTO_LIGHT']:
        print(f"Invalid key for toggle auto control: {key}")
        return

    command = f"{key}:{1 if state else 0}\n"
    with lock:
        ser.write(command.encode())
    print(f"Sent command: {command.strip()}")


@socketio.on("manual_control")
def manual_control(data):
    if not ser: return  # Check serial port
    key = data.get('key')
    state = data.get('state')
    print(f"Received manual control: {key} = {state}")  # Debug print
    if key not in ['MANUAL_IRRIGATION', 'MANUAL_LIGHT']:
        print(f"Invalid key for manual control: {key}")
        return

    command = ""
    if key == 'MANUAL_IRRIGATION':
        command = f"MANUAL_IRRIGATION:{1 if state else 0}\n"
    elif key == 'MANUAL_LIGHT':
        try:
            brightness = int(state)
            command = f"MANUAL_LIGHT:{brightness}\n"
        except ValueError:
            print(f"Invalid value for manual light: {state}")
            return

    if command:
        with lock:
            ser.write(command.encode())
        print(f"Sent command: {command.strip()}")


@socketio.on("get_logs_request")
def handle_log_request(data):
    """
    Handles request from frontend to fetch logs.
    """
    if not ser:  # Check serial port
        print("Log request failed: Serial port not available.")
        socketio.emit('log_error', {'message': 'Serial port not available.'})
        return

    print("Received log request from client.")
    command = "GET_LOGS\n"
    try:
        with lock:
            ser.write(command.encode())  # Send command to Arduino
        print(f"Sent command: {command.strip()}")
        socketio.emit('log_request_sent', {'message': 'Log request sent to Arduino.'})
    except Exception as e:
        print(f"Error sending GET_LOGS command: {e}")
        socketio.emit('log_error', {'message': f'Error sending command to Arduino: {e}'})


if __name__ == '__main__':
    if ser:
        print(f"Serial port {ser.name} opened successfully.")
        Thread(target=sync_time, daemon=True).start()
        Thread(target=serial_reader, daemon=True).start()
    else:
        print("Running without serial connection. Some features will be disabled.")

    socketio.run(app, host='0.0.0.0', port=5000, allow_unsafe_werkzeug=True)  # allow_unsafe_werkzeug for development auto-reload

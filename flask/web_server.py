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
TIME_ZONE_OFFSET = 0  # Argentina Time (UTC-3)


# NTP and Time Sync Functions (keep as is)
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
    if not ser:
        print("sync_time_internal: Serial port not available.")
        return False  # Indicate failure
    try:
        current_time = int(get_ntp_time())
        command = f"TIME:{current_time}\n"
        with lock:
            ser.write(command.encode())
        print(f"Time sync command sent: TIME:{current_time} | {datetime.fromtimestamp(current_time)}")
        time.sleep(0.5)
        return True  # Indicate success
    except ntplib.NTPException as e:
        print(f"Initial NTP sync failed: {e}")
        return False
    except Exception as e:
        print(f"Initial Time sync failed (other error): {e}")
        return False


def sync_time():
    """Periodic time synchronization loop."""
    print("Starting periodic time sync...")
    while True:
        sync_time_internal()  # Attempt sync
        time.sleep(3600)  # Sync periodically (e.g., every hour)


def serial_reader():
    reading_logs = False
    current_logs = []
    log_pattern = re.compile(r"^(\d+),(\d+),(\d+)$")  # Regex for TS,TYPE,VALUE

    while True:
        # Add a check for reading_logs timeout as a fallback
        if reading_logs and (not ser or not ser.is_open):
            print("Serial port closed or error while reading logs. Sending partial logs.")
            if current_logs:
                socketio.emit('log_data', {'logs': current_logs})
            reading_logs = False
            current_logs = []
            time.sleep(100)  # Pause before trying again
            continue

        # Handle serial reading with timeout
        line = None

        try:
            if ser and ser.in_waiting > 0:  # Check if data exists before reading
                line_bytes = ser.readline()
                line = line_bytes.decode(errors='ignore').strip()
            elif reading_logs:
                # If reading logs and no data comes within timeout, assume Arduino stopped
                # This is a fallback in case END_LOG is missed
                print("Timeout waiting for log data or END_LOG. Sending collected logs.")
                socketio.emit('log_data', {'logs': current_logs})
                reading_logs = False
                current_logs = []
                time.sleep(10)  # Short sleep after timeout
                continue
        except serial.SerialException as e:  # Handle serial errors during read
            print(f"Serial Exception during read: {e}")
            if reading_logs:
                print("Aborting log read due to serial error.")
                socketio.emit('log_error', {'message': f'Serial error during log read: {e}'})
                reading_logs = False
                current_logs = []
            time.sleep(10)
            continue
        except UnicodeDecodeError:
            print("Serial read warning: UnicodeDecodeError")  # Handle potential garbage data
            line = None  # Ignore undecodable lines
        except Exception as e:
            print(f"Unexpected error in serial_reader loop: {e}")
            if reading_logs:  # Reset state on unexpected errors
                reading_logs = False
                current_logs = []
            time.sleep(10)
            continue  # Continue the loop

        if line is None:  # If readline timed out or produced no data
            time.sleep(10)  # Prevent busy-waiting if serial is idle
            continue

        # Process the received line
        if reading_logs:
            if line == "END_LOG":
                print("Detected END_LOG marker.")
                socketio.emit('log_data', {'logs': current_logs})  # Send collected logs
                reading_logs = False  # Stop reading logs
                current_logs = []  # Clear the buffer
            else:
                # Try to match log pattern
                match = log_pattern.match(line)
                if match:
                    # It's a log line, parse and store it
                    timestamp, event_type, value = match.groups()
                    current_logs.append({
                        'timestamp': int(timestamp),
                        'type': int(event_type),
                        'value': int(value)
                    })
                else:
                    # Received unexpected line while waiting for logs or END_LOG
                    print(f"Warning: Received unexpected line while reading logs: '{line}'")
        else:
            if line == "TS,TYPE,VALUE":  # Check for log header
                print("Detected log header, starting log capture.")
                reading_logs = True  # Start reading logs
                current_logs = []  # Clear previous logs
            elif line.startswith("SENSORS:"):
                line_data = line.replace("SENSORS:", "")
                data = {}
                parts = line_data.split(';')
                print(f"Sensor data received: {line_data}")  # Debug print
                for part in parts:
                    if ':' in part:
                        key, val_str = part.split(':', 1)
                        try:
                            data[key] = int(val_str)
                        except ValueError:
                            print(f"Warning: Could not parse sensor value '{val_str}' for key '{key}'")
                            data[key] = 0
                socketio.emit('sensor_update', {
                    'moisture': data.get('MOISTURE', 0),
                    'light': data.get('LIGHT', 0)
                })
            elif line.startswith("TIME_SET:"):
                try:
                    timestamp = int(line.split(':')[1])
                    human_time = datetime.fromtimestamp(timestamp).isoformat()
                    socketio.emit('time_update', {'timestamp': timestamp, 'human': human_time})
                except (IndexError, ValueError):
                    print(f"Warning: Could not parse TIME_SET line: {line}")
            else:
                # Emit other non-log, non-sensor data if needed
                if line:
                    print(f"Received serial data: {line}")  # Debug print


# Route and SocketIO handlers

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
def handle_log_request():
    """
    Handles request from frontend to fetch logs.
    """
    if not ser:  # Check serial port
        print("Log request failed: Serial port not available.")
        socketio.emit('log_error', {'message': 'Serial port not available.'})
        return

    print("Received log request from client.")
    command = "GET_LOGS:0\n"
    try:
        with lock:
            ser.write(command.encode())  # Send command to Arduino
        print(f"Sent command: {command.strip()}")
        socketio.emit('log_request_sent', {'message': 'Log request sent to Arduino.'})
    except Exception as e:
        print(f"Error sending GET_LOGS command: {e}")
        socketio.emit('log_error', {'message': f'Error sending command to Arduino: {e}'})


@socketio.on("clear_logs_request")
def handle_clear_logs_request():
    """
    Handles request from frontend to clear logs.
    """
    if not ser:  # Check serial port
        print("Clear logs request failed: Serial port not available.")
        socketio.emit('clear_logs_response', {'message': 'Serial port not available.'})
        return

    print("Received clear logs request from client.")
    command = "CLEAR_LOGS:0\n"  # Command to clear logs on Arduino
    try:
        with lock:
            ser.write(command.encode())  # Send command to Arduino
        print(f"Sent command: {command.strip()}")
        socketio.emit('clear_logs_response', {'message': 'Logs cleared successfully.'})
    except Exception as e:
        print(f"Error sending CLEAR_LOGS command: {e}")
        socketio.emit('clear_logs_response', {'message': f'Error clearing logs: {e}'})


if __name__ == '__main__':
    if ser:
        print(f"Serial port {ser.name} opened successfully.")
        print("Attempting initial time sync...")
        if not sync_time_internal():
             print("Initial time sync failed. Arduino time may be incorrect until next sync.")
        else:
             print("Initial time sync command sent successfully.")
        Thread(target=sync_time, daemon=True).start()
        Thread(target=serial_reader, daemon=True).start()
    else:
        print("Running without serial connection.")

    socketio.run(app, host='0.0.0.0', port=5000, allow_unsafe_werkzeug=True)  # allow_unsafe_werkzeug for development auto-reload

# TODO: When light threshold is set to 0 the light should be turned off

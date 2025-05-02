from datetime import datetime

import ntplib
from flask import Flask, render_template
from flask_socketio import SocketIO
import serial
from threading import Thread, Lock
import time

app = Flask(__name__)
socketio = SocketIO(app)
ser = serial.Serial('COM3', 115200, timeout=1)  # Opens the serial port COMX (WINDOWS).
lock = Lock()

# Threshold defaults
MOISTURE_THRESH = 500
LIGHT_THRESH = 300

# NTP Server configuration
NTP_SERVER = 'pool.ntp.org'
TIME_ZONE_OFFSET = -3  # Argentina Time (UTC-3)

def get_ntp_time():
    client = ntplib.NTPClient()
    try:
        response = client.request(NTP_SERVER, version=4)
        return response.tx_time + TIME_ZONE_OFFSET * 3600
    except Exception as e:
        print(f"NTP Error: {e}")
        return datetime.now().timestamp()  # Fallback to system time

def sync_time():
    while True:
        try:
            current_time = int(get_ntp_time())
            ser.write(f"TIME {current_time}\n".encode())
            print(f"Synced time: {datetime.fromtimestamp(current_time)}")
        except Exception as e:
            print(f"Time sync failed: {e}")

        time.sleep(60)  # Sync every minute

def serial_reader():
    while True:
        try:
            # Reads a full line of data, decodes the bytes to a string, and removes leading/trailing whitespaces.
            if ser.in_waiting <= 0:
                continue
            # Checks if there's data in the serial buffer.
            line = ser.readline().decode().strip()
            if not line:
                continue
            if line.startswith("SENSORS:"):
                # Parse sensor data
                data = {}
                parts = line.split(';')
                for part in parts:
                    if ':' in part:
                        key, val = part.split(':', 1)
                        data[key] = int(val)
                socketio.emit('sensor_update', {
                    'moisture': data.get('MOISTURE', 0),
                    'light': data.get('LIGHT', 0),
                    'timestamp': datetime.now().isoformat()
                })
            elif line.startswith("TIME_SET:"):
                timestamp = int(line.split(':')[1])
                human_time = datetime.fromtimestamp(timestamp).isoformat()
                socketio.emit('time_update', {'timestamp': timestamp, 'human': human_time})
            else:
                socketio.emit('serial_data', {'data': line})

        except Exception as e:
            print("Serial read error:", e)
        time.sleep(0.1)

@app.route('/')
def index():
    """
    Renders the main page of the web application.
    """
    return render_template('index.html')

@socketio.on("threshold_update")
def threshold_update(data):
    key = data.get('key')
    value = data.get('value')
    if key not in ['MOISTURE_THRESH', 'LIGHT_THRESH']:
        return
    if key == 'MOISTURE_THRESH':
        global MOISTURE_THRESH
        MOISTURE_THRESH = int(value)
    elif key == 'LIGHT_THRESH':
        global LIGHT_THRESH
        LIGHT_THRESH = int(value)

    with lock:
        ser.write(f"{key}:{int(value)}\n".encode())

@socketio.on("log_request")
def log_request(data):
    pass

if __name__ == '__main__':
    Thread(target=sync_time, daemon=True).start()
    Thread(target=serial_reader, daemon=True).start()
    socketio.run(app, host='0.0.0.0', port=5000)

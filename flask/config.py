import logging

# Configuration file for Flask application

# Serial communication configuration
SERIAL_TIMEOUT = 5
BAUD_RATE = 115200
SERIAL_PORT = 'COM4'

# NTP Server configuration
NTP_SERVER = 'pool.ntp.org'
TIME_ZONE_OFFSET = 0

# Logging configuration
logger = logging.getLogger(__name__)
logging.basicConfig(format="%(asctime)s || %(levelname)s || %(message)s", datefmt="%Y-%m-%d %H:%M:%S", level=logging.INFO)

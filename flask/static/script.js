const socket = io();
const logTableBody = document.getElementById('logTable')?.querySelector('tbody');
const getLogsButton = document.getElementById('getLogsButton');
const logStatus = document.getElementById('logStatus');
const clearLogsButton = document.getElementById('clearLogsButton');

// Map Arduino EventType  enum values to readable strings
const eventTypeMap = {
	0: "Auto Irrigation",
	1: "Auto Light",
	2: "Light Threshold Change",
	3: "Moisture Threshold Change",
	4: "Moisture Measurement",
	5: "Light Measurement",
};

let moistureChart, lightChart;
let sensorData = {
	moisture: [],
	light: [],
	labels: []
};

/**
 * Updates the logStatus element with a message and style.
 * @param {string} message - The message to display.
 * @param {'info'|'error'} [type='info'] - The type of message.
 */
function updateLogStatus(message, type = 'info') {
	if (logStatus) {
		logStatus.textContent = message;
		logStatus.className = ''; // Clear existing classes
		if (type === 'error') {
			logStatus.classList.add('log-status-error');
		} else {
			logStatus.classList.add('log-status-info');
		}
	}
}

/**
 * Displays a snackbar message to the user.
 * @param {string} message - The message to show.
 * @param {'info'|'error'} [type='info'] - The type of message.
 */
function showSnackbar(message, type = 'info') {
	const container = document.getElementById('snackbar-container');
	if (!container) {
		console.error('Snackbar container not found!');
		return;
	}

	const snackbar = document.createElement('div');
	snackbar.className = `snackbar snackbar-${type}`; // Apply base and type-specific class
	snackbar.textContent = message;

	container.appendChild(snackbar);

	// Trigger the animation by adding 'show' class
	// requestAnimationFrame ensures the element is in DOM and styles are applied before transition starts
	requestAnimationFrame(() => {
		snackbar.classList.add('show');
	});

	// Automatically remove the snackbar after some time
	setTimeout(() => {
		snackbar.classList.remove('show'); // Trigger slide-out animation

		// Remove the element from DOM after animation finishes
		snackbar.addEventListener('transitionend', () => {
			// Check if the element still has a parent (i.e., it hasn't been removed already)
			if (snackbar.parentNode) {
				snackbar.remove();
			}
		});

		// Fallback: if transitionend doesn't fire for some reason (e.g., display: none was set abruptly)
		// ensure the element is removed. The timeout should be slightly longer than the CSS transition.
		setTimeout(() => {
			if (snackbar.parentNode) {
				snackbar.remove();
			}
		}, 500); // Corresponds to transition duration + a small buffer

	}, 4000); // Snackbar visible for 4 seconds
}

/**
 * Initializes the moisture and light charts.
 */
function initCharts() {
	const ctx1 = document.getElementById('moistureChart').getContext('2d');
	const ctx2 = document.getElementById('lightChart').getContext('2d');

	moistureChart = new Chart(ctx1, {
		type: 'line',
		data: {
			labels: sensorData.labels,
			datasets: [{
				label: 'Soil Moisture',
				data: sensorData.moisture,
				borderColor: '#2b6cb0',
				tension: 0.3
			}]
		}
	});

	lightChart = new Chart(ctx2, {
		type: 'line',
		data: {
			labels: sensorData.labels,
			datasets: [{
				label: 'Light Level',
				data: sensorData.light,
				borderColor: '#ecc94b',
				tension: 0.3
			}]
		}
	});
}

/**
 * Updates the numeric displays and charts with new sensor data.
 * @param {{moisture: number, light: number}} data - The sensor data.
 */
function updateDisplay(data) {
	// Update numeric displays
	$('#moistureValue').text(data.moisture);
	$('#lightValue').text(data.light);

	// Update charts
	const now = new Date().toLocaleTimeString();
	sensorData.labels.push(now);
	sensorData.moisture.push(data.moisture);
	sensorData.light.push(data.light);

	if (sensorData.labels.length > 15) {
		sensorData.labels.shift();
		sensorData.moisture.shift();
		sensorData.light.shift();
	}

	moistureChart.update();
	lightChart.update();
}

/**
 * Debounces a function to limit how often it can fire.
 * @param {Function} func - The function to debounce.
 * @param {number} delay - Delay in milliseconds.
 * @returns {Function}
 */
function debounce(func, delay) {
	let timeout;
	return function (...args) {
		clearTimeout(timeout);
		timeout = setTimeout(() => func.apply(this, args), delay);
	};
}

/**
 * Handles the "Get Logs" button click event.
 * Requests logs from the backend and clears the log table.
 */
if (getLogsButton) { // Check if button exists
	getLogsButton.addEventListener('click', () => {
		updateLogStatus('Requesting logs...', 'info');
		socket.emit('logs_request');
		// Clear the table while waiting for new logs
		if (logTableBody) logTableBody.innerHTML = ''; // Clear old logs
	});
}

/**
 * Handles the "Clear Logs" button click event.
 * Requests log clearing from the backend and clears the log table.
 */
if (clearLogsButton) { // Check if button exists
	clearLogsButton.addEventListener('click', () => {
		showSnackbar('Clearing logs...', 'info');
		socket.emit('clear_logs_request'); // Emit event to backend
		logStatus.textContent = '';
		logStatus.className = '';
		if (logTableBody) logTableBody.innerHTML = ''; // Clear old logs
	});
}

/**
 * Receives sensor data updates from the backend and updates the display.
 */
socket.on('sensor_update', updateDisplay);

/**
 * Receives log data from the backend and populates the log table.
 */
socket.on('log_data', (data) => { // Listen for logs from backend
	console.log("Received logs:", data.logs); // Log received data

	if (!logTableBody || !data.logs) {
		updateLogStatus('Error displaying logs. Data unavailable.', 'error');
		return;
	}

	logTableBody.innerHTML = ''; // Clear previous logs (could append too)
	if (data.logs.length === 0) {
		updateLogStatus('No logs found or received.', 'info');
		return;
	}

	data.logs.forEach(log => { // Iterate through each log entry
		const row = logTableBody.insertRow(); // Create new table row
		row.insertCell().textContent = new Date(log.timestamp * 1000).toLocaleString();
		row.insertCell().textContent = log.type;
		row.insertCell().textContent = log.value;
		row.insertCell().textContent = eventTypeMap[log.type] || "Unknown Event";
	});

	updateLogStatus(`Received ${data.logs.length} log entries.`, 'info');
});

/**
 * Receives threshold updates from the backend and updates the UI sliders and displays.
 */
socket.on('threshold_update', (data) => {
	console.log("Received threshold update:", data);
	const {key, value} = data;
	const slider = $(`.threshold-input[data-type='${key}']`);
	const valueDisplay = $(`#${key}Value`);

	// Find the slider and value display elements

	if (slider.length && valueDisplay.length) {
		// Check if the current slider value is already what we received.
		if (parseInt(slider.val()) !== parseInt(value)) {
			slider.val(value);
		}
		valueDisplay.text(value);
	}
});

/**
 * Receives error messages from the backend and displays them as snackbars.
 */
socket.on('error', (data) => {
	showSnackbar(`Error: ${data.message}`, 'error');
});

/**
 * Receives informational messages from the backend and displays them as snackbars.
 */
socket.on('info', (data) => {
	showSnackbar(data.message, 'info');
});

/**
 * Handles slider input changes for threshold updates.
 * @param slider - The slider element that triggered the change.
 * @param displayId - The ID of the display element to update.
 */
function updateThresholdDisplay(slider, displayId) {
	document.getElementById(displayId).textContent = slider.value;
}

/**
 * Debounced function to send threshold updates to the backend.
 * @type {(function(...[*]): void)|*} - A debounced function that sends the threshold update.
 */
const sendThresholdUpdate = debounce((slider) => {
	const type = slider.getAttribute('data-type');
	const value = slider.value;
	socket.emit('threshold_update', {key: type, value: value});
}, 300);

// Initialize charts when page loads
$(document).ready(initCharts);

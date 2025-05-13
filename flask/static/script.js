const socket = io();
const logTableBody = document.getElementById('logTable')?.querySelector('tbody');
const getLogsButton = document.getElementById('getLogsButton');
const logStatus = document.getElementById('logStatus');
const clearLogsButton = document.getElementById('clearLogsButton');

// Map Arduino EventType enum values to readable strings
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

// Initialize charts
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

// Debounce function to limit frequent calls
function debounce(func, delay) {
	let timeout;
	return function (...args) {
		clearTimeout(timeout);
		timeout = setTimeout(() => func.apply(this, args), delay);
	};
}

if (getLogsButton) { // Check if button exists
	getLogsButton.addEventListener('click', () => {
		logStatus.textContent = 'Requesting logs...';
		socket.emit('logs_request');
		// Clear the table while waiting for new logs
		if (logTableBody) logTableBody.innerHTML = ''; // Clear old logs
	});
}

if (clearLogsButton) { // Check if button exists
	clearLogsButton.addEventListener('click', () => {
		logStatus.textContent = 'Clearing logs...';
		socket.emit('clear_logs_request'); // Emit event to backend
	});
}

socket.on('sensor_update', updateDisplay);

socket.on('log_data', (data) => { // Listen for logs from backend
	console.log("Received logs:", data.logs); // Log received data

	if (!logTableBody || !data.logs) {
		logStatus.textContent = 'Error displaying logs.';
		return;
	}

	logTableBody.innerHTML = ''; // Clear previous logs (could append too)
	if (data.logs.length === 0) {
		logStatus.textContent = 'No logs found or received.';
		return;
	}

	data.logs.forEach(log => { // Iterate through each log entry
		const row = logTableBody.insertRow(); // Create new table row
		row.insertCell().textContent = new Date(log.timestamp * 1000).toLocaleString();
		row.insertCell().textContent = log.type;
		row.insertCell().textContent = log.value;
		row.insertCell().textContent = eventTypeMap[log.type] || "Unknown Event";
	});

	logStatus.textContent = `Received ${data.logs.length} log entries.`;
});


// SocketIO Listener for Clear Logs Response
socket.on('clear_logs_response', (data) => {
	logStatus.textContent = data.message;
	if (logTableBody) logTableBody.innerHTML = '';
});

// SocketIO Listener for synced threshold updates from backend.
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

socket.on('error', (data) => {
	logStatus.textContent = `Error: ${data.message}`;
});

// Threshold update functions
function updateThresholdDisplay(slider, displayId) {
	document.getElementById(displayId).textContent = slider.value;
}

const sendThresholdUpdate = debounce((slider) => {
	const type = slider.getAttribute('data-type');
	const value = slider.value;
	socket.emit('threshold_update', {key: type, value: value});
}, 300);

// Initialize charts when page loads
$(document).ready(initCharts);

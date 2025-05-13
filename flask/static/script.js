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

// Update sensor displays and charts
socket.on('sensor_update', function (data) {
	update_display(data);
});

function update_display(data) {
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

// Event Listener for Get Logs Button
if (getLogsButton) { // Check if button exists
	getLogsButton.addEventListener('click', () => {
		console.log("Requesting logs..."); // Log action
		if (logStatus) logStatus.textContent = 'Requesting logs...'; // Update status
		socket.emit('logs_request'); // Emit event to backend
		// Clear the table while waiting for new logs
		if (logTableBody) logTableBody.innerHTML = ''; // Clear old logs
	});
}

// SocketIO Listener for Log Data
socket.on('log_data', function (data) { // Listen for logs from backend
	console.log("Received logs:", data.logs); // Log received data
	if (logTableBody && data.logs) { // Check if table body and logs exist
		logTableBody.innerHTML = ''; // Clear previous logs (optional, could append too)

		if (data.logs.length === 0) {
			if (logStatus) logStatus.textContent = 'No logs found or received.'; // Update status
			return;
		}

		data.logs.forEach(log => { // Iterate through each log entry
			const row = logTableBody.insertRow(); // Create new table row

			const timestampCell = row.insertCell();
			// Convert Unix timestamp to readable date/time
			const date = new Date(log.timestamp * 1000); // Arduino sends seconds timestamp
			timestampCell.textContent = date.toLocaleString(); // Format date nicely

			const typeCell = row.insertCell();
			typeCell.textContent = log.type; // Show raw type number

			const valueCell = row.insertCell();
			valueCell.textContent = log.value; // Show raw value

			const descriptionCell = row.insertCell(); // Add a cell for description
			descriptionCell.textContent = eventTypeMap[log.type] || "Unknown Event"; // Map type to string
		});

		if (logStatus) logStatus.textContent = `Received ${data.logs.length} log entries.`; // Update status
	} else {
		console.error("Log table body not found or no logs received."); // Error handling
		if (logStatus) logStatus.textContent = 'Error displaying logs.'; // Update status
	}
});

if (clearLogsButton) { // Check if button exists
	clearLogsButton.addEventListener('click', () => {
		console.log("Clearing logs..."); // Log action
		if (logStatus) logStatus.textContent = 'Clearing logs...'; // Update status
		socket.emit('clear_logs_request'); // Emit event to backend
	});
}

// SocketIO Listener for Clear Logs Response
socket.on('clear_logs_response', function (data) {
	console.log("Clear logs response:", data.message); // Log response
	if (logStatus) logStatus.textContent = data.message; // Update status
	if (logTableBody) logTableBody.innerHTML = ''; // Clear the log table
});

// SocketIO Listener for synced threshold updates from backend.
socket.on('threshold_update', function (data) {
	console.log("Received threshold update:", data);
	const {key, value} = data;

	// Find the slider and value display elements
	const slider = $(`.threshold-input[data-type='${key}']`);
	const valueDisplay = $(`#${key}Value`);

	if (slider.length && valueDisplay.length) {
		// Check if the current slider value is already what we received.
		if (parseInt(slider.val()) !== parseInt(value)) {
			slider.val(value); // Set the slider's value
		}
		valueDisplay.text(value); // Update the displayed number
		console.log(`UI Updated for ${key} threshold to ${value}.`);
	} else {
		console.warn(`Could not find UI elements for threshold: ${key}`);
	}
});

// The existing vanilla JS functions for threshold display and sending:
function updateThresholdDisplay(slider, displayId) {
	// This function is called on 'oninput' and updates the local text.
	document.getElementById(displayId).textContent = slider.value;
}

function sendThresholdUpdate(slider) {
	// This function is called on 'onchange' (when user releases mouse from slider).
	const type = slider.getAttribute('data-type'); // 'MT' or 'LT'
	const value = slider.value;
	socket.emit('threshold_update', {key: type, value: value});
	console.log(`User sent threshold update via UI: ${type} = ${value}`);
}

// SocketIO Listener for Potential Errors
socket.on('log_error', function (data) { // Listen for errors from backend
	console.error("Error from backend:", data.message); // Log error
	if (logStatus) logStatus.textContent = `Error: ${data.message}`; // Display error
});

// Initialize charts when page loads
$(document).ready(initCharts);

const socket = io();  // Connect to the web socket  "http://" + location.hostname + ":" + location.port
const logTableBody = document.getElementById('logTable')?.querySelector('tbody'); // Get table body
const getLogsButton = document.getElementById('getLogsButton'); // Get button
const logStatus = document.getElementById('logStatus'); // Get status element

// Map Arduino EventType enum values to readable strings
const eventTypeMap = {
    0: "Auto Irrigation Start",
    1: "Auto Irrigation Stop",
    2: "Manual Irrigation Start",
    3: "Manual Irrigation Stop",
    4: "Auto Light Adjust",
    5: "Manual Light Adjust",
    6: "Moisture Alert",
    7: "Light Alert",
    8: "Config Change"
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

// Threshold Controls
$('.threshold-input').on('input', function () {
	const key = $(this).data('type'); // e.g., MOISTURE_THRESH
	const value = $(this).val();
	$(`#${key}Value`).text(value);
	// Use the command key expected by Arduino
	const commandKey = `SET_${key}`; // e.g., SET_MOISTURE_THRESH
	socket.emit('threshold_update', {key: commandKey, value});
});

// Auto Mode Toggles
$('#autoIrrigationToggle').change(function () {
	const state = $(this).prop('checked');
	$('#irrigationModeStatus').text(state ? 'Auto' : 'Manual');
	$('#irrigationControls').toggleClass('active', !state);
	socket.emit('toggle_auto_control', {
		key: 'AUTO_IRRIGATION',
		state: state
	});
});

$('#autoLightToggle').change(function () {
	const state = $(this).prop('checked');
	$('#lightModeStatus').text(state ? 'Auto' : 'Manual');
	$('#lightControls').toggleClass('active', !state);
	socket.emit('toggle_auto_control', {
		key: 'AUTO_LIGHT',
		state: state
	});
});

// Manual Irrigation Control
$('#manualIrrigationToggle').click(() => {
	const currentState = $('#irrigationState').text() === 'ON';
	const newState = !currentState;
	$('#irrigationState').text(newState ? 'ON' : 'OFF');
	socket.emit('manual_control', {
		key: 'MANUAL_IRRIGATION',
		state: newState
	});
});

// Manual Light Control
$('#manualLightSlider').on('input', function () {
	const brightness = $(this).val();
	$('#manualLightValue').text(brightness);
	socket.emit('manual_control', {
		key: 'MANUAL_LIGHT',
		state: brightness
	});
});

// --- Event Listener for Get Logs Button ---
if (getLogsButton) { // Check if button exists
    getLogsButton.addEventListener('click', () => {
        console.log("Requesting logs..."); // Log action
        if(logStatus) logStatus.textContent = 'Requesting logs...'; // Update status
        socket.emit('get_logs_request'); // Emit event to backend
        // Clear the table while waiting for new logs
        if (logTableBody) logTableBody.innerHTML = ''; // Clear old logs
    });
}


// --- SocketIO Listener for Log Data ---
socket.on('log_data', function(data) { // Listen for logs from backend
    console.log("Received logs:", data.logs); // Log received data
    if (logTableBody && data.logs) { // Check if table body and logs exist
        logTableBody.innerHTML = ''; // Clear previous logs (optional, could append too)

        if (data.logs.length === 0) {
             if(logStatus) logStatus.textContent = 'No logs found or received.'; // Update status
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

        if(logStatus) logStatus.textContent = `Received ${data.logs.length} log entries.`; // Update status
    } else {
        console.error("Log table body not found or no logs received."); // Error handling
        if(logStatus) logStatus.textContent = 'Error displaying logs.'; // Update status
    }
});


// --- SocketIO Listener for Potential Errors ---
socket.on('log_error', function(data) { // Listen for errors from backend
	console.error("Log retrieval error:", data.message); // Log error
	if(logStatus) logStatus.textContent = `Error: ${data.message}`; // Display error
});

// Initialize charts when page loads
$(document).ready(initCharts);


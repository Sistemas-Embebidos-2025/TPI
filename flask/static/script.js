const socket = io();  // Connect to the web socket  "http://" + location.hostname + ":" + location.port

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
	const key = $(this).data('type');
	const value = $(this).val();
	$(`#${key}Value`).text(value);
	socket.emit('threshold_update', {key, value});
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

// Initialize charts when page loads
$(document).ready(initCharts);


const socket = io();  // Connect to the web socket  "http://" + location.hostname + ":" + location.port

function updateDisplay(data) {
	// Update PWM LEDs
	$('#L1').val(data.L1);
	$('#L1_val').text(data.L1);
	$('#L2').val(data.L2);
	$('#L2_val').text(data.L2);
	$('#L3').val(data.L3);
	$('#L3_val').text(data.L3);

	// Update Digital LED
	$('#D13_state').text(data.D13 ? 'ON' : 'OFF');

	// Update LDR
	$('#A3_val').text(data.A3);
}

// Update the display when new data is received
socket.on('update_data', function(data) {
    updateDisplay(data);
});

// PWM Slider Events
$('input[type="range"]').on('input', function () {
	const key = $(this).attr('id');
	const value = $(this).val();
	socket.emit("control_led", {key, value});
});

// Toggle Button Event
$('#D13_toggle').click(() => {
	const current = $('#D13_state').text() === 'ON';
	const key = 'D13';
	const value= !current ? 1 : 0;
	socket.emit("control_led", {key, value});
});

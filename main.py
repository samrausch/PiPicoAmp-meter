import network
import socket
import asyncio
from machine import I2C, Pin, ADC
from ssd1306 import SSD1306_I2C

# ============================================================
# Pin Definitions
# ============================================================
VOLTAGE_PIN     = 26        # ADC0 - Voltage divider output
CURRENT_PIN     = 27        # ADC1 - ACS712 output
I2C_SDA         = 4         # GPIO4 - OLED SDA
I2C_SCL         = 5         # GPIO5 - OLED SCL

# ============================================================
# OLED Settings
# 128x32 display
# The built-in MicroPython framebuf font is 8x8 pixels
# At that size, 15 characters = 120 pixels wide (fits 128px)
# and 2 rows of text fit in 32px height (8px per row,
# with 16px used leaving 16px for a second row)
# We will use font scale x2 (16x16 per char) for the
# readings row giving large clear digits, and scale x1
# for any status text
# Since framebuf does not natively support font scaling,
# we will use the writer.py / font approach or restrict
# to 8x8 and fill the display sensibly.
#
# Layout (128x32):
#   Row 0 (y=0):  "DC Power Monitor"  - text size 1 (8px tall)
#   Row 1 (y=8):  separator line
#   Row 2 (y=16): "V:xx.xx I:xx.xx"  - text size 1 (8px tall)
#                  with each field 7 chars wide
#   Row 3 (y=24): WiFi IP or blank
# ============================================================
SCREEN_WIDTH    = 128
SCREEN_HEIGHT   = 32
OLED_ADDR       = 0x3C

# ============================================================
# WiFi Settings
# ============================================================
WIFI_SSID       = "hotcockmachine"
WIFI_PASSWORD   = "starbucks"
WIFI_TIMEOUT_S  = 10

# ============================================================
# ADC / Sensor Settings
# Pico W ADC: 16-bit scaled (0-65535), reference 3.3V
# Note: MicroPython returns 16-bit values from ADC.read_u16()
# regardless of the underlying hardware resolution
# ============================================================
ADC_MAX         = 65535.0
ADC_REF_V       = 3.3

# Voltage divider: 7.5k / (7.5k + 30k) = 0.2
DIVIDER_RATIO   = 0.2

# ACS712 30A powered from 3.3V
# Zero current = VCC/2 = 1.65V
# Sensitivity  = 66mV/A
ACS712_ZERO_V   = 1.65
ACS712_SENS     = 0.066

# Averaging window in milliseconds
SAMPLE_WINDOW_MS = 300

# ADC valid range - 16-bit scaled
# Readings stuck at 0 or 65535 on every sample = ERR
ADC_MIN_VALID   = 100
ADC_MAX_VALID   = 65435

# ============================================================
# Shared state
# Using a simple dict so both async tasks can access it
# ============================================================
state = {
    "voltage":          0.0,
    "current":          0.0,
    "voltage_error":    False,
    "current_error":    False,
    "wifi_connected":   False,
    "ip_address":       "",
}

# ============================================================
# Hardware Initialisation
# ============================================================
# ADC inputs
adc_voltage = ADC(Pin(VOLTAGE_PIN))
adc_current = ADC(Pin(CURRENT_PIN))

# I2C and OLED
i2c = I2C(0, sda=Pin(I2C_SDA), scl=Pin(I2C_SCL), freq=400000)
oled = SSD1306_I2C(SCREEN_WIDTH, SCREEN_HEIGHT, i2c, addr=OLED_ADDR)


# ============================================================
# Function: read_averaged_adc
# Samples the given ADC object repeatedly over
# SAMPLE_WINDOW_MS milliseconds and returns the average.
# Sets error to True if every sample is out of valid range.
# Uses asyncio.sleep_ms(1) between samples to yield control
# to other tasks while sampling.
# ============================================================
async def read_averaged_adc(adc_obj):
    total       = 0
    count       = 0
    error_count = 0
    start       = asyncio.ticks_ms()

    while asyncio.ticks_diff(asyncio.ticks_ms(), start) < SAMPLE_WINDOW_MS:
        sample = adc_obj.read_u16()

        if sample < ADC_MIN_VALID or sample > ADC_MAX_VALID:
            error_count += 1

        total += sample
        count += 1
        await asyncio.sleep_ms(1)

    if count == 0:
        return 0.0, True

    if error_count == count:
        return 0.0, True

    return total / count, False


# ============================================================
# Function: read_sensors
# Reads both ADC channels and updates shared state
# ============================================================
async def read_sensors():
    # Voltage
    raw, err = await read_averaged_adc(adc_voltage)
    state["voltage_error"] = err

    if not err:
        adc_v = (raw / ADC_MAX) * ADC_REF_V
        volts = adc_v / DIVIDER_RATIO
        state["voltage"] = max(0.0, volts)

    # Current
    raw, err = await read_averaged_adc(adc_current)
    state["current_error"] = err

    if not err:
        adc_v   = (raw / ADC_MAX) * ADC_REF_V
        offset  = adc_v - ACS712_ZERO_V
        amps    = offset / ACS712_SENS
        state["current"] = max(0.0, amps)


# ============================================================
# Function: format_voltage
# Returns a 7-character string for voltage: "V:xx.xx"
# or "V: ERR " if error
# ============================================================
def format_voltage():
    if state["voltage_error"]:
        return "V: ERR "
    val = "{:.2f}".format(state["voltage"])
    # Pad or truncate to ensure consistent width
    # "V:" = 2 chars, value up to 5 chars (xx.xx) = 7 total
    return "V:{:<5}".format(val)


# ============================================================
# Function: format_current
# Returns a 7-character string for current: "I:xx.xx"
# or "I: ERR " if error
# ============================================================
def format_current():
    if state["current_error"]:
        return "I: ERR "
    val = "{:.2f}".format(state["current"])
    return "I:{:<5}".format(val)


# ============================================================
# Function: update_display
# Redraws the OLED with current state
#
# Layout:
#   y=0:  "DC Power Monitor"
#   y=9:  horizontal separator line
#   y=12: "V:xx.xx I:xx.xx"
#   y=24: IP address (small) or blank if no WiFi
# ============================================================
def update_display():
    oled.fill(0)

    # Title
    oled.text("DC Power Monitor", 0, 0, 1)

    # Separator line
    oled.hline(0, 9, SCREEN_WIDTH, 1)

    # Readings on one line
    # Each field is 7 chars, 1 space between = 15 chars total
    # At 8px per char = 120px wide, centred in 128px
    reading_str = "{} {}".format(format_voltage(), format_current())
    # Centre the 15-char string: (128 - 15*8) / 2 = 4px offset
    oled.text(reading_str, 4, 12, 1)

    # Bottom line: IP address if WiFi connected, else blank
    if state["wifi_connected"]:
        oled.text(state["ip_address"], 0, 24, 1)

    oled.show()


# ============================================================
# Function: connect_wifi
# Attempts to connect to WiFi.
# Returns (True, ip_string) or (False, "")
# ============================================================
def connect_wifi():
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    wlan.connect(WIFI_SSID, WIFI_PASSWORD)

    import time
    start = time.time()

    while not wlan.isconnected():
        if time.time() - start > WIFI_TIMEOUT_S:
            print("WiFi Error: Could not connect to network '{}'".format(WIFI_SSID))
            return False, ""
        time.sleep(0.25)

    ip = wlan.ifconfig()[0]
    print("WiFi connected. IP: {}".format(ip))
    return True, ip


# ============================================================
# Function: build_web_page
# Returns the HTML string for the web page
# ============================================================
def build_web_page():
    voltage_str = "ERR" if state["voltage_error"] else "{:.2f}".format(state["voltage"])
    current_str = "ERR" if state["current_error"] else "{:.2f}".format(state["current"])

    v_class = "error" if state["voltage_error"] else "value"
    i_class = "error" if state["current_error"] else "value"

    v_unit  = "" if state["voltage_error"] else " V"
    i_unit  = "" if state["current_error"] else " A"

    html = """<!DOCTYPE html>
<html>
<head>
<meta charset='utf-8'>
<meta http-equiv='refresh' content='1'>
<title>DC Power Monitor</title>
<style>
body {{
    font-family: Arial, sans-serif;
    background: #1a1a1a;
    color: #f0f0f0;
    display: flex;
    justify-content: center;
    align-items: center;
    height: 100vh;
    margin: 0;
}}
.card {{
    background: #2a2a2a;
    border-radius: 12px;
    padding: 40px;
    text-align: center;
    box-shadow: 0 4px 20px rgba(0,0,0,0.5);
    min-width: 280px;
}}
h1 {{
    margin: 0 0 30px 0;
    font-size: 1.4em;
    color: #aaa;
    letter-spacing: 2px;
}}
.reading {{
    margin: 15px 0;
}}
.label {{
    font-size: 0.9em;
    color: #888;
    margin-bottom: 4px;
}}
.value {{
    font-size: 2.4em;
    font-weight: bold;
    color: #4fc3f7;
}}
.error {{
    font-size: 2.4em;
    font-weight: bold;
    color: #ef5350;
}}
</style>
</head>
<body>
<div class='card'>
<h1>DC POWER MONITOR</h1>
<div class='reading'>
    <div class='label'>VOLTAGE</div>
    <div class='{v_class}'>{voltage_str}{v_unit}</div>
</div>
<div class='reading'>
    <div class='label'>CURRENT</div>
    <div class='{i_class}'>{current_str}{i_unit}</div>
</div>
</div>
</body>
</html>""".format(
        v_class=v_class,
        voltage_str=voltage_str,
        v_unit=v_unit,
        i_class=i_class,
        current_str=current_str,
        i_unit=i_unit,
    )

    return html


# ============================================================
# Function: handle_client
# Handles a single HTTP client connection
# ============================================================
async def handle_client(reader, writer):
    try:
        # Read the request line - we don't need to parse it
        # since we only serve one page
        request_line = await asyncio.wait_for(reader.readline(), timeout=5.0)

        # Drain remaining headers
        while True:
            line = await asyncio.wait_for(reader.readline(), timeout=5.0)
            if line == b"\r\n" or line == b"":
                break

        # Build and send response
        response_body = build_web_page()
        response = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n"
            "Content-Length: {}\r\n"
            "\r\n"
            "{}"
        ).format(len(response_body), response_body)

        writer.write(response.encode())
        await writer.drain()

    except Exception as e:
        print("Client handler error: {}".format(e))

    finally:
        writer.close()
        await writer.wait_closed()


# ============================================================
# Task: sensor_task
# Continuously reads sensors and updates the OLED
# Runs as an asyncio task
# ============================================================
async def sensor_task():
    while True:
        await read_sensors()
        update_display()

        # Debug to USB serial
        v_str = "ERR" if state["voltage_error"] else "{:.2f}V".format(state["voltage"])
        i_str = "ERR" if state["current_error"] else "{:.2f}A".format(state["current"])
        print("Voltage: {}   Current: {}".format(v_str, i_str))


# ============================================================
# Task: web_server_task
# Starts the async TCP server and listens for connections
# Only started if WiFi connected successfully
# ============================================================
async def web_server_task():
    server = await asyncio.start_server(handle_client, "0.0.0.0", 80)
    print("Web server listening on port 80")
    async with server:
        await server.wait_closed()


# ============================================================
# Main entry point
# ============================================================
async def main():
    # Show startup message on OLED
    oled.fill(0)
    oled.text("DC Power Monitor", 0, 0, 1)
    oled.text("Starting...", 0, 12, 1)
    oled.show()

    # Attempt WiFi connection (blocking, done before async loop)
    wifi_ok, ip = connect_wifi()
    state["wifi_connected"] = wifi_ok
    state["ip_address"]     = ip[:16] if wifi_ok else ""  # Truncate to 16 chars for OLED

    # Start sensor task (always runs)
    asyncio.create_task(sensor_task())

    # Start web server task only if WiFi connected
    if wifi_ok:
        asyncio.create_task(web_server_task())

    # Keep the main coroutine alive
    while True:
        await asyncio.sleep(1)


# Run
asyncio.run(main())

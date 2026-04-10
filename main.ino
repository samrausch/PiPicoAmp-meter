// The code herein was partially created with the assistance of AI tools.
// All AI-generated output has been reviewed, tested, and refined by the human author(s) 
// to ensure accuracy and quality. 
// The human authors remain solely responsible for the content and its fitness for purpose
//
// Pi Pico Ammeter/Amp-meter v1.1
// Sam Rausch, 2026

// Hardware includes:
// Pi Pico W 2
// ACS712 30A current sensor module (Hiletgo)
// Voltage divider 7k/30k (1/4w 5% resistors)
// SSD1306 128x64 I2C OLED display

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>

// ============================================================
// Pin Definitions
// All GPIO numbers refer to Pico W GPIO numbers, not
// physical pin numbers
// ============================================================
#define VOLTAGE_PIN     26      // GPIO26 / ADC0 - Voltage divider
#define CURRENT_PIN     27      // GPIO27 / ADC1 - ACS712 output

// I2C for OLED
// GPIO4 = SDA, GPIO5 = SCL (I2C0 on Pico W)
#define I2C_SDA         4
#define I2C_SCL         5

// ============================================================
// OLED Display Settings
// ============================================================
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define OLED_ADDRESS    0x3C
// Note: Some SSD1306 modules labelled 0x7C are actually 0x3C
// in 7-bit I2C addressing (0x7C is the 8-bit write address).
// If the display doesn't work, try 0x3D.

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ============================================================
// WiFi Settings
// ============================================================
#define WIFI_SSID           "your_ssid"
#define WIFI_PASSWORD       "your_password"
#define WIFI_TIMEOUT_MS     10000   // 10 seconds to connect

// ============================================================
// ADC / Sensor Settings
// Pico W ADC: 12-bit (0-4095), reference voltage 3.3V
// ============================================================
#define ADC_RESOLUTION      4095.0
#define ADC_REF_VOLTAGE     3.3

// Voltage divider ratio: 7.5k / (7.5k + 30k) = 0.2
// At 15V input, divider output = 3.0V, well within 3.3V ADC range
#define DIVIDER_RATIO       0.2

// ACS712 30A powered from 3.3V:
// Zero current output = VCC/2 = 1.65V
// Sensitivity = 66mV/A
// Maximum output = 3.3V (at ~25A, safely within ADC range)
#define ACS712_SENSITIVITY  0.066       // Volts per Amp
#define ACS712_ZERO_VOLTAGE 1.65        // Volts at zero current

// Averaging window
#define SAMPLE_WINDOW_MS    300

// ADC valid range - if every sample is outside this = ERR
// 12-bit ADC so range is 0-4095
#define ADC_MIN_VALID       1
#define ADC_MAX_VALID       4094

float   gVoltage        = 0.0;
float   gCurrent        = 0.0;
bool    gVoltageError   = false;
bool    gCurrentError   = false;
bool    gWifiConnected  = false;

WebServer server(80);

// ============================================================
// Function: connectToWifi
// Attempts to connect to WiFi within WIFI_TIMEOUT_MS
// Returns true if successful
// ============================================================
bool connectToWifi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long startTime = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - startTime > WIFI_TIMEOUT_MS)
        {
            return false;
        }
        delay(250);
    }

    return true;
}

// ============================================================
// Setup
// ============================================================
void setup()
{
    Serial.begin(115200);

    // Initialise I2C on chosen pins
    Wire.setSDA(I2C_SDA);
    Wire.setSCL(I2C_SCL);
    Wire.begin();

    // Set ADC resolution to 12-bit
    analogReadResolution(12);

    // Initialise OLED
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS))
    {
        Serial.println(F("SSD1306 init failed. Check wiring and I2C address."));
        while (true) { delay(1000); }
    }

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(F("DC Power Monitor"));
    display.println(F("Connecting WiFi..."));
    display.display();

    // Attempt WiFi connection
    gWifiConnected = connectToWifi();

    if (gWifiConnected)
    {
        Serial.print(F("WiFi connected. IP: "));
        Serial.println(WiFi.localIP());

        // Start web server
        server.on("/", handleRoot);
        server.begin();

        Serial.println(F("Web server started."));

        // Show IP on OLED briefly
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println(F("DC Power Monitor"));
        display.println(F("WiFi connected"));
        display.println(F("IP:"));
        display.println(WiFi.localIP().toString());
        display.display();
        delay(2000);
    }
    else
    {
        Serial.println(F("WiFi connection failed."));

        display.clearDisplay();
        display.setCursor(0, 0);
        display.println(F("DC Power Monitor"));
        display.println(F("WiFi Error"));
        display.println(F("Continuing without"));
        display.println(F("web server."));
        display.display();
        delay(2000);
    }
}

// ============================================================
// Function: readAveragedADC
// Samples the given pin repeatedly over SAMPLE_WINDOW_MS and
// returns the average ADC value as a float.
// Sets errorFlag to true if every sample is outside the valid
// range, indicating a likely sensor/wiring fault.
// ============================================================
float readAveragedADC(int pin, bool &errorFlag)
{
    long    sum         = 0;
    int     count       = 0;
    int     errorCount  = 0;
    unsigned long startTime = millis();

    while (millis() - startTime < SAMPLE_WINDOW_MS)
    {
        int sample = analogRead(pin);

        if (sample < ADC_MIN_VALID || sample > ADC_MAX_VALID)
        {
            errorCount++;
        }

        sum += sample;
        count++;
        delay(1);
    }

    if (count == 0)
    {
        errorFlag = true;
        return 0.0;
    }

    if (errorCount == count)
    {
        errorFlag = true;
        return 0.0;
    }

    errorFlag = false;
    return (float)sum / (float)count;
}

// ============================================================
// Function: readSensors
// Reads both sensors and updates global variables
// ============================================================
void readSensors()
{
    // --- Voltage ---
    float voltageADC = readAveragedADC(VOLTAGE_PIN, gVoltageError);

    if (!gVoltageError)
    {
        float adcVoltage = (voltageADC / ADC_RESOLUTION) * ADC_REF_VOLTAGE;
        gVoltage = adcVoltage / DIVIDER_RATIO;
        if (gVoltage < 0.0) gVoltage = 0.0;
    }

    // --- Current ---
    float currentADC = readAveragedADC(CURRENT_PIN, gCurrentError);

    if (!gCurrentError)
    {
        float adcVoltage    = (currentADC / ADC_RESOLUTION) * ADC_REF_VOLTAGE;
        float offsetVoltage = adcVoltage - ACS712_ZERO_VOLTAGE;
        gCurrent            = offsetVoltage / ACS712_SENSITIVITY;
        if (gCurrent < 0.0) gCurrent = 0.0;
    }
}

// ============================================================
// Function: updateDisplay
// Layout:
//   Title bar     (y=0,  size 1)
//   Voltage       (y=14, size 2)
//   Current       (y=34, size 2)
//   WiFi status   (y=56, size 1) - only shown if WiFi failed
// ============================================================
void updateDisplay()
{
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    // --- Title ---
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(F("DC Power Monitor"));
    display.drawLine(0, 10, SCREEN_WIDTH - 1, 10, SSD1306_WHITE);

    // --- Voltage ---
    display.setTextSize(2);
    display.setCursor(0, 14);
    display.print(F("V: "));

    if (gVoltageError)
        display.print(F("ERR"));
    else
    {
        display.print(gVoltage, 2);
        display.print(F("V"));
    }

    // --- Current ---
    display.setCursor(0, 34);
    display.print(F("I: "));

    if (gCurrentError)
        display.print(F("ERR"));
    else
    {
        display.print(gCurrent, 2);
        display.print(F("A"));
    }

    // --- WiFi Status (bottom line, small text) ---
    display.setTextSize(1);
    display.setCursor(0, 56);

    if (!gWifiConnected)
        display.print(F("WiFi Error"));
    else
    {
        display.println(WiFi.localIP().toString());
    }
    display.display();
}

// ============================================================
// Function: buildWebPage
// Constructs the HTML page as a String
// ============================================================
String buildWebPage()
{
    String voltageStr = gVoltageError ? "ERR" : String(gVoltage, 2);
    String currentStr = gCurrentError ? "ERR" : String(gCurrent, 2);

    String html = F("<!DOCTYPE html><html><head>");
    html += F("<meta charset='utf-8'>");
    html += F("<meta http-equiv='refresh' content='1'>");
    html += F("<title>DC Power Monitor</title>");
    html += F("<style>");
    html += F("body{font-family:Arial,sans-serif;background:#1a1a1a;color:#f0f0f0;");
    html += F("display:flex;justify-content:center;align-items:center;height:100vh;margin:0;}");
    html += F(".card{background:#2a2a2a;border-radius:12px;padding:40px;text-align:center;");
    html += F("box-shadow:0 4px 20px rgba(0,0,0,0.5);min-width:280px;}");
    html += F("h1{margin:0 0 30px 0;font-size:1.4em;color:#aaa;letter-spacing:2px;}");
    html += F(".reading{margin:15px 0;}");
    html += F(".label{font-size:0.9em;color:#888;margin-bottom:4px;}");
    html += F(".value{font-size:2.4em;font-weight:bold;color:#4fc3f7;}");
    html += F(".error{color:#ef5350;}");
    html += F("</style></head><body><div class='card'>");
    html += F("<h1>DC POWER MONITOR</h1>");

    // Voltage
    html += F("<div class='reading'>");
    html += F("<div class='label'>VOLTAGE</div>");
    html += F("<div class='value");
    if (gVoltageError) html += F(" error");
    html += F("'>");
    html += voltageStr;
    if (!gVoltageError) html += F(" V");
    html += F("</div></div>");

    // Current
    html += F("<div class='reading'>");
    html += F("<div class='label'>CURRENT</div>");
    html += F("<div class='value");
    if (gCurrentError) html += F(" error");
    html += F("'>");
    html += currentStr;
    if (!gCurrentError) html += F(" A");
    html += F("</div></div>");

    html += F("</div></body></html>");

    return html;
}

// ============================================================
// Function: handleRoot
// Web server handler for the root page
// ============================================================
void handleRoot()
{
    server.send(200, "text/html", buildWebPage());
}

// ============================================================
// Loop
// ============================================================
void loop()
{
    readSensors();
    updateDisplay();

    if (gWifiConnected)
    {
        server.handleClient();
    }

    // Debug output to USB serial
    Serial.print(F("Voltage: "));
    if (gVoltageError)
        Serial.print(F("ERR"));
    else
    {
        Serial.print(gVoltage, 2);
        Serial.print(F("V"));
    }

    Serial.print(F("   Current: "));
    if (gCurrentError)
        Serial.println(F("ERR"));
    else
    {
        Serial.print(gCurrent, 2);
        Serial.println(F("A"));
    }
}

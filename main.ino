// The code herein was partially created with the assistance of AI tools.
// All AI-generated output has been reviewed, tested, and refined by the human author(s) 
// to ensure accuracy and quality. 
// The human authors remain solely responsible for the content and its fitness for purpose
//
// Pi Pico Ammeter/Amp-meter v1.3
// Sam Rausch, 2026

// Hardware includes:
// Pi Pico 2 (no WiFi)
// ACS712 30A current sensor module (Hiletgo)
// Voltage divider 7k/30k (1/4w 5% resistors)
// SSD1306 128x64 or 128x32 I2C OLED display
// (set DISPLAY_TYPE below to match the display you have installed)

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ============================================================
// Pin Definitions
// All GPIO numbers refer to Pico GPIO numbers, not
// physical pin numbers
// ============================================================
#define VOLTAGE_PIN     26      // GPIO26 / ADC0 - Voltage divider
#define CURRENT_PIN     27      // GPIO27 / ADC1 - ACS712 output

// I2C for OLED
// GPIO4 = SDA, GPIO5 = SCL (I2C0 on Pico)
#define I2C_SDA         4
#define I2C_SCL         5

// ============================================================
// OLED Display Settings
// ============================================================
// Set DISPLAY_TYPE to the display you have installed.
// DISPLAY_TYPE_128X64 - full layout: title, voltage, current, watts
// DISPLAY_TYPE_128X32 - compact layout: voltage and current only
#define DISPLAY_TYPE_128X64     64
#define DISPLAY_TYPE_128X32     32

#define DISPLAY_TYPE            DISPLAY_TYPE_128X64

#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   DISPLAY_TYPE
#define OLED_RESET      -1
#define OLED_ADDRESS    0x3C
// Note: Some SSD1306 modules labelled 0x7C are actually 0x3C
// in 7-bit I2C addressing (0x7C is the 8-bit write address).
// If the display doesn't work, try 0x3D.

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ============================================================
// ADC / Sensor Settings
// Pico ADC: 12-bit (0-4095), reference voltage 3.3V
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
float   gPower          = 0.0;
bool    gVoltageError   = false;
bool    gCurrentError   = false;

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
    display.println(F("Starting up..."));
    display.display();
    delay(1000);
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

    // --- Power ---
    if (!gVoltageError && !gCurrentError)
    {
        gPower = gVoltage * gCurrent;
    }
}

// ============================================================
// Function: updateDisplay
//
// Layout (128x64):
//   Title bar     (y=0,  size 1)
//   Voltage       (y=14, size 2)
//   Current       (y=32, size 2)  <- moved up 2px to make room for watts
//   Watts         (y=50, size 1)
//
// Layout (128x32):
//   Voltage       (y=0,  size 2)
//   Current       (y=16, size 2)
//   (title and watts omitted - not enough vertical space)
// ============================================================
void updateDisplay()
{
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

#if DISPLAY_TYPE == DISPLAY_TYPE_128X64
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
    display.setCursor(0, 32);
    display.print(F("I: "));

    if (gCurrentError)
        display.print(F("ERR"));
    else
    {
        display.print(gCurrent, 2);
        display.print(F("A"));
    }

    // --- Watts ---
    display.setTextSize(1);
    display.setCursor(0, 50);
    display.print(F("W: "));

    if (gVoltageError || gCurrentError)
        display.print(F("ERR"));
    else
    {
        display.print(gPower, 2);
        display.print(F("W"));
    }

#elif DISPLAY_TYPE == DISPLAY_TYPE_128X32
    // --- Voltage ---
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.print(F("V: "));

    if (gVoltageError)
        display.print(F("ERR"));
    else
    {
        display.print(gVoltage, 2);
        display.print(F("V"));
    }

    // --- Current ---
    display.setCursor(0, 16);
    display.print(F("I: "));

    if (gCurrentError)
        display.print(F("ERR"));
    else
    {
        display.print(gCurrent, 2);
        display.print(F("A"));
    }
#endif

    display.display();
}

// ============================================================
// Loop
// ============================================================
void loop()
{
    readSensors();
    updateDisplay();

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
        Serial.print(F("ERR"));
    else
    {
        Serial.print(gCurrent, 2);
        Serial.print(F("A"));
    }

    Serial.print(F("   Power: "));
    if (gVoltageError || gCurrentError)
        Serial.println(F("ERR"));
    else
    {
        Serial.print(gPower, 2);
        Serial.println(F("W"));
    }
}

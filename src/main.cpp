#include <Arduino.h>
#include <Adafruit_ThinkInk.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <SensirionI2cScd4x.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

namespace {

// Adafruit ESP32-C6 Feather -> Adafruit 2.13" eInk breakout wiring.
// The display, SRAM, and microSD card share the Feather's hardware SPI bus.
constexpr int8_t kEpdCs = SS;      // GPIO 0 -> ECS
constexpr int8_t kEpdDc = A0;      // GPIO 1 -> D/C
constexpr int8_t kSramCs = A5;     // GPIO 2 -> SRCS
constexpr int8_t kEpdReset = A4;   // GPIO 3 -> RST
constexpr int8_t kEpdBusy = 7;     // GPIO 7 -> BUSY
constexpr int8_t kEpdEnable = A2;  // GPIO 6 -> ENA (HIGH = powered)
constexpr int8_t kSdCs = TX;       // GPIO 16 -> SDCS (LOW = selected)

// Sleep duration AFTER each measurement and display refresh. Change to 120
// for two hours; use a short interval when testing on hardware.
constexpr uint32_t kSleepIntervalMinutes = 60;
constexpr uint64_t kSleepIntervalUs =
    static_cast<uint64_t>(kSleepIntervalMinutes) * 60ULL * 1000000ULL;
constexpr uint32_t kSensorPollIntervalMs = 1000;
constexpr uint32_t kMeasurementTimeoutMs = 45UL * 1000UL;
constexpr uint32_t kDisplayPowerUpDelayMs = 100;
constexpr uint32_t kSensorPowerUpDelayMs = 1000;
constexpr uint32_t kProgrammingHoldMs = 10UL * 1000UL;
constexpr int8_t kBootButton = 9;
// REQUIRED jumper: IO9 -> A3. GPIO9 cannot wake the C6 from deep sleep.
constexpr gpio_num_t kWakeButton = GPIO_NUM_5;
constexpr int16_t kLayoutOffsetX = -4;
constexpr int16_t kSidebarContentOffsetX = 3;
constexpr bool kUseFahrenheit = true;
constexpr int16_t kNoError = 0;
// The onboard MAX17048 shares I2C with the SCD-40. Leave enough charge for a
// final eInk refresh before the battery or regulator cuts off power.
constexpr uint8_t kBatteryMonitorAddress = 0x36;
constexpr float kLowBatteryPercent = 20.0F;
constexpr float kDepletedBatteryPercent = 5.0F;
constexpr float kDepletedBatteryVoltage = 3.4F;

static_assert(kSleepIntervalMinutes > 0);
static_assert(kDepletedBatteryPercent < kLowBatteryPercent);

// This display is marked FPC-7528B. Its SSD1680 framebuffer needs the BN/B74
// memory offset; using the GDEY0213B74 driver leaves a noisy 8-pixel strip.
ThinkInk_213_Mono_BN display(kEpdDc, kEpdReset, kEpdCs, kSramCs, kEpdBusy,
                             &SPI);
SensirionI2cScd4x scd40;

struct AirReading {
  uint16_t co2Ppm;
  float temperatureC;
  float humidityPercent;
};

enum class BatteryState { Unknown, Normal, Low, Depleted };

BatteryState batteryState = BatteryState::Unknown;
bool sensorAvailable = false;
bool measurementRunning = false;
bool programmingMode = false;
uint32_t lastSensorPollMs = 0;
uint32_t measurementStartedMs = 0;

bool readBatteryRegister(uint8_t address, uint16_t& value) {
  Wire.beginTransmission(kBatteryMonitorAddress);
  Wire.write(address);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(kBatteryMonitorAddress, static_cast<size_t>(2)) != 2) {
    return false;
  }
  value = static_cast<uint16_t>(Wire.read()) << 8;
  value |= static_cast<uint16_t>(Wire.read());
  return true;
}

void updateBatteryStatus(bool logReading = false) {
  uint16_t version = 0;
  uint16_t rawVoltage = 0;
  uint16_t rawPercent = 0;
  // Read without resetting/quick-starting the gauge on each deep-sleep wake;
  // its state-of-charge estimate must continue tracking while the ESP32 sleeps.
  // An absent battery or I2C failure must not be mistaken for an empty battery.
  if (!readBatteryRegister(0x08, version) ||
      (version & 0xFFF0) != 0x0010 ||
      !readBatteryRegister(0x02, rawVoltage) ||
      !readBatteryRegister(0x04, rawPercent) || rawVoltage == 0 ||
      rawPercent == 0xFFFF) {
    if (logReading) {
      Serial.println("Battery reading unavailable; retaining last known state.");
    }
    return;
  }

  // MAX17048 VCELL is 78.125 uV/LSB; SOC is 1/256 percent/LSB.
  const float voltage = rawVoltage * 0.000078125F;
  const float percent = min(rawPercent / 256.0F, 100.0F);
  if (voltage > 4.5F) {
    return;
  }
  if (percent <= kDepletedBatteryPercent ||
      voltage <= kDepletedBatteryVoltage) {
    batteryState = BatteryState::Depleted;
  } else if (percent <= kLowBatteryPercent) {
    batteryState = BatteryState::Low;
  } else {
    batteryState = BatteryState::Normal;
  }
  if (logReading) {
    Serial.printf("Battery: %.1f%%, %.3f V\n", percent, voltage);
  }
}

void printSensorError(const char* operation, int16_t error) {
  char message[64] = {};
  errorToString(error, message, sizeof(message));
  Serial.printf("SCD-40 %s failed: %s (error %d)\n", operation, message,
                error);
}

void initializeDisplay() {
  // Keep the breakout rail powered; display.display() puts the panel controller
  // into sleep after refreshing. Reinitialize after each ESP32 deep-sleep wake.
  pinMode(kEpdEnable, OUTPUT);
  digitalWrite(kEpdEnable, HIGH);
  delay(kDisplayPowerUpDelayMs);

  display.begin(THINKINK_MONO);
  display.setRotation(2);
  display.setTextColor(EPD_BLACK);
  display.setTextWrap(false);
}

void finishDisplayUpdate() {
  Serial.println("Refreshing eInk display...");
  const uint32_t refreshStartedMs = millis();
  display.display();
  Serial.printf("eInk refresh complete in %lu ms; panel controller asleep.\n",
                static_cast<unsigned long>(millis() - refreshStartedMs));
}

void drawCenteredText(const char* text, int16_t centerX, int16_t baselineY,
                      const GFXfont* font) {
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t width = 0;
  uint16_t height = 0;

  display.setFont(font);
  display.getTextBounds(text, 0, baselineY, &x1, &y1, &width, &height);
  display.setCursor(centerX - static_cast<int16_t>(width) / 2 - x1,
                    baselineY);
  display.print(text);
}

void drawBatteryIcon(int16_t x, int16_t y, uint8_t scale, bool empty) {
  display.drawRect(x, y, 20 * scale, 12 * scale, EPD_BLACK);
  display.drawRect(x + scale, y + scale, 18 * scale, 10 * scale,
                   EPD_BLACK);
  display.fillRect(x + 20 * scale, y + 4 * scale, 2 * scale, 4 * scale,
                   EPD_BLACK);
  if (!empty) {
    display.fillRect(x + 3 * scale, y + 3 * scale, 3 * scale, 6 * scale,
                     EPD_BLACK);
  }
}

void drawLowBatteryWarning() {
  if (batteryState == BatteryState::Low) {
    drawBatteryIcon(4, 4, 1, false);
  }
}

void showDepletedBattery() {
  display.clearBuffer();
  display.setTextSize(1);
  const int16_t centerX = display.width() / 2 + kLayoutOffsetX;
  drawBatteryIcon(centerX - 33, 18, 3, true);
  drawCenteredText("LOW BATTERY", centerX, 80, &FreeSansBold9pt7b);
  drawCenteredText("PLEASE RECHARGE", centerX, 104, nullptr);
  finishDisplayUpdate();
}

void drawCo2Value(uint16_t co2Ppm) {
  char value[8] = {};
  snprintf(value, sizeof(value), "%u", co2Ppm);

  // A three-digit value uses the 24pt font at 2x scale. Step down to its native
  // size at four digits so both cases remain large without touching the rule.
  constexpr GFXfont const* kValueFont = &FreeSansBold24pt7b;
  const uint8_t valueScale = co2Ppm < 1000 ? 2 : 1;

  int16_t valueX1 = 0;
  int16_t valueY1 = 0;
  int16_t ppmX1 = 0;
  int16_t ppmY1 = 0;
  int16_t co2X1 = 0;
  int16_t co2Y1 = 0;
  uint16_t valueWidth = 0;
  uint16_t valueHeight = 0;
  uint16_t ppmWidth = 0;
  uint16_t ppmHeight = 0;
  uint16_t co2Width = 0;
  uint16_t co2Height = 0;

  display.setFont(kValueFont);
  display.setTextSize(valueScale);
  display.getTextBounds(value, 0, 0, &valueX1, &valueY1, &valueWidth,
                        &valueHeight);
  display.setFont(nullptr);
  display.setTextSize(1);
  display.getTextBounds("PPM", 0, 0, &ppmX1, &ppmY1, &ppmWidth, &ppmHeight);
  display.getTextBounds("CO2", 0, 0, &co2X1, &co2Y1, &co2Width, &co2Height);

  constexpr int16_t kLabelGap = 7;
  const int16_t sidebarLeft =
      display.width() * 3 / 4 + kLayoutOffsetX;
  const int16_t co2AreaCenterX = (kLayoutOffsetX + sidebarLeft) / 2;
  const int16_t labelsWidth =
      max(static_cast<int16_t>(ppmWidth), static_cast<int16_t>(co2Width));
  const int16_t groupWidth =
      static_cast<int16_t>(valueWidth) + kLabelGap + labelsWidth;
  const int16_t valueLeft = co2AreaCenterX - groupWidth / 2;
  const int16_t labelsLeft =
      valueLeft + static_cast<int16_t>(valueWidth) + kLabelGap;

  // Center the number vertically, then center the stacked labels against the
  // number's visible glyph bounds instead of its font baseline.
  const int16_t valueTop =
      (display.height() - static_cast<int16_t>(valueHeight)) / 2;
  const int16_t valueBaselineY = valueTop - valueY1;
  constexpr int16_t kLabelRowGap = 2;
  const int16_t labelsHeight = static_cast<int16_t>(ppmHeight) +
                               kLabelRowGap +
                               static_cast<int16_t>(co2Height);
  const int16_t labelsTop =
      valueTop + (static_cast<int16_t>(valueHeight) - labelsHeight) / 2;
  const int16_t ppmTop = labelsTop - ppmY1;
  const int16_t co2Top = labelsTop + static_cast<int16_t>(ppmHeight) +
                         kLabelRowGap - co2Y1;

  display.setFont(kValueFont);
  display.setTextSize(valueScale);
  display.setCursor(valueLeft - valueX1, valueBaselineY);
  display.print(value);

  display.setFont(nullptr);
  display.setTextSize(1);
  display.setCursor(labelsLeft - ppmX1, ppmTop);
  display.print("PPM");
  display.setCursor(labelsLeft - co2X1, co2Top);
  display.print("CO2");
}

void drawMetric(int16_t centerX, int16_t boxTop, int16_t boxBottom,
                const char* label, const char* value) {
  int16_t labelX1 = 0;
  int16_t labelY1 = 0;
  int16_t valueX1 = 0;
  int16_t valueY1 = 0;
  uint16_t labelWidth = 0;
  uint16_t labelHeight = 0;
  uint16_t valueWidth = 0;
  uint16_t valueHeight = 0;

  display.setFont(nullptr);
  display.setTextSize(1);
  display.getTextBounds(label, 0, 0, &labelX1, &labelY1, &labelWidth,
                        &labelHeight);
  display.setFont(&FreeSansBold9pt7b);
  display.getTextBounds(value, 0, 0, &valueX1, &valueY1, &valueWidth,
                        &valueHeight);

  constexpr int16_t kLabelValueGap = 10;
  const int16_t groupHeight = static_cast<int16_t>(labelHeight) +
                              kLabelValueGap +
                              static_cast<int16_t>(valueHeight);
  const int16_t groupTop =
      boxTop + (boxBottom - boxTop + 1 - groupHeight) / 2;
  const int16_t valueTop =
      groupTop + static_cast<int16_t>(labelHeight) + kLabelValueGap;

  display.setFont(nullptr);
  display.setTextSize(1);
  display.setCursor(centerX - static_cast<int16_t>(labelWidth) / 2 - labelX1,
                    groupTop - labelY1);
  display.print(label);

  display.setFont(&FreeSansBold9pt7b);
  display.setCursor(centerX - static_cast<int16_t>(valueWidth) / 2 - valueX1,
                    valueTop - valueY1);
  display.print(value);
}

void drawDashboard(const AirReading& reading) {
  char temperature[16] = {};
  char humidity[16] = {};

  const float shownTemperature =
      kUseFahrenheit ? reading.temperatureC * 1.8F + 32.0F
                     : reading.temperatureC;
  snprintf(temperature, sizeof(temperature), "%.1f %c", shownTemperature,
           kUseFahrenheit ? 'F' : 'C');
  snprintf(humidity, sizeof(humidity), "%.0f%%", reading.humidityPercent);

  display.clearBuffer();

  drawCo2Value(reading.co2Ppm);

  const int16_t sidebarLeft =
      display.width() * 3 / 4 + kLayoutOffsetX;
  const int16_t contentRight = display.width() + kLayoutOffsetX;
  const int16_t sidebarCenterX =
      (sidebarLeft + contentRight) / 2 + kSidebarContentOffsetX;
  const int16_t middleY = display.height() / 2;

  display.drawFastVLine(sidebarLeft, 4, display.height() - 8, EPD_BLACK);
  display.drawFastHLine(sidebarLeft, middleY, display.width() - sidebarLeft,
                        EPD_BLACK);

  drawMetric(sidebarCenterX, 4, middleY - 1, "TEMP", temperature);
  drawMetric(sidebarCenterX, middleY + 1, display.height() - 5, "HUMIDITY",
             humidity);
  drawLowBatteryWarning();
}

void showSensorError() {
  updateBatteryStatus(true);
  if (batteryState == BatteryState::Depleted) {
    showDepletedBattery();
    return;
  }
  display.clearBuffer();
  display.setTextSize(1);
  drawCenteredText("SCD-40", display.width() / 2 + kLayoutOffsetX, 35,
                   &FreeSansBold9pt7b);
  drawCenteredText("SENSOR ERROR", display.width() / 2 + kLayoutOffsetX, 67,
                   &FreeSansBold9pt7b);
  drawCenteredText("CHECK STEMMA QT", display.width() / 2 + kLayoutOffsetX,
                   96, nullptr);
  drawLowBatteryWarning();
  finishDisplayUpdate();
}

void showReading(const AirReading& reading) {
  updateBatteryStatus(true);
  if (batteryState == BatteryState::Depleted) {
    showDepletedBattery();
    return;
  }
  drawDashboard(reading);
  finishDisplayUpdate();
}

bool initializeSensor() {
  scd40.begin(Wire, SCD40_I2C_ADDR_62);
  delay(30);

  // A microcontroller reset does not reset a separately powered SCD-40. Stop
  // any measurement left running by the previous boot before reinitializing.
  int16_t error = scd40.stopPeriodicMeasurement();
  if (error != kNoError) {
    printSensorError("stopPeriodicMeasurement", error);
    return false;
  }

  error = scd40.reinit();
  if (error != kNoError) {
    printSensorError("reinit", error);
    return false;
  }

  uint64_t serialNumber = 0;
  error = scd40.getSerialNumber(serialNumber);
  if (error != kNoError) {
    printSensorError("getSerialNumber", error);
    return false;
  }

  Serial.printf("SCD-40 detected, serial 0x%04X%08X\n",
                static_cast<uint16_t>(serialNumber >> 32),
                static_cast<uint32_t>(serialNumber));

  return true;
}

bool startMeasurementCycle() {
  const int16_t error = scd40.startLowPowerPeriodicMeasurement();
  if (error != kNoError) {
    printSensorError("startLowPowerPeriodicMeasurement", error);
    return false;
  }

  measurementRunning = true;
  measurementStartedMs = millis();
  lastSensorPollMs = measurementStartedMs;
  Serial.println("SCD-40 measurement started; waiting about 30 seconds...");
  return true;
}

bool stopMeasurementCycle() {
  const int16_t error = scd40.stopPeriodicMeasurement();
  if (error != kNoError) {
    printSensorError("stopPeriodicMeasurement", error);
    return false;
  }

  measurementRunning = false;
  return true;
}

bool readSensor(AirReading& reading) {
  bool dataReady = false;
  int16_t error = scd40.getDataReadyStatus(dataReady);
  if (error != kNoError) {
    printSensorError("getDataReadyStatus", error);
    return false;
  }

  if (!dataReady) {
    return false;
  }

  error = scd40.readMeasurement(reading.co2Ppm, reading.temperatureC,
                                reading.humidityPercent);
  if (error != kNoError) {
    printSensorError("readMeasurement", error);
    return false;
  }

  if (reading.co2Ppm == 0) {
    Serial.println("SCD-40 returned an invalid zero-ppm sample; ignoring it.");
    return false;
  }

  Serial.printf("CO2: %u ppm, temperature: %.2f C, humidity: %.2f %%RH\n",
                reading.co2Ppm, reading.temperatureC,
                reading.humidityPercent);
  return true;
}

// A held button delays all other work so sleep cannot interrupt the gesture.
// Called before sensor initialization, while measuring, and before sleeping.
bool checkProgrammingButton() {
  if (programmingMode) {
    return true;
  }
  if (digitalRead(kBootButton) != LOW) {
    return false;
  }

  const uint32_t pressedMs = millis();
  while (digitalRead(kBootButton) == LOW) {
    if (millis() - pressedMs >= kProgrammingHoldMs) {
      programmingMode = true;
      if (measurementRunning) {
        stopMeasurementCycle();
      }
      display.clearBuffer();
      display.setTextSize(1);
      drawCenteredText("PROGRAMMING", display.width() / 2, 42,
                       &FreeSansBold9pt7b);
      drawCenteredText("MODE", display.width() / 2, 67,
                       &FreeSansBold9pt7b);
      drawCenteredText("RESET TO RESUME", display.width() / 2, 96, nullptr);
      drawLowBatteryWarning();
      finishDisplayUpdate();
      Serial.println("Programming mode: deep sleep disabled until reset.");
      return true;
    }
    delay(10);
  }
  return false;
}

void sleepUntilNextReading() {
  if (checkProgrammingButton()) {
    return;
  }
  // Retry stopping after a sensor fault before putting the host to sleep.
  if (measurementRunning) {
    stopMeasurementCycle();
  }
  ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(kSleepIntervalUs));
  ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup(
      1ULL << kWakeButton, ESP_GPIO_WAKEUP_GPIO_LOW));

  // Release I2C and its internal pull-ups before removing STEMMA QT power.
  // Retain the high-impedance pins so the host cannot feed the unpowered sensor.
  Wire.end();
  pinMode(SDA, INPUT);
  pinMode(SCL, INPUT);
  ESP_ERROR_CHECK(gpio_hold_en(static_cast<gpio_num_t>(SDA)));
  ESP_ERROR_CHECK(gpio_hold_en(static_cast<gpio_num_t>(SCL)));
  digitalWrite(PIN_NEOPIXEL_I2C_POWER, LOW);

  // Retain STEMMA power OFF, display power ON, and SD deselection during sleep.
  // Keeping the display powered avoids back-powering it through SPI/control pins.
  ESP_ERROR_CHECK(gpio_hold_en(static_cast<gpio_num_t>(kEpdEnable)));
  ESP_ERROR_CHECK(gpio_hold_en(static_cast<gpio_num_t>(kSdCs)));
  ESP_ERROR_CHECK(gpio_hold_en(
      static_cast<gpio_num_t>(PIN_NEOPIXEL_I2C_POWER)));
  // ESP32-C6 retains individual pad holds in deep sleep without a global hold.
  Serial.printf("Deep sleeping for %lu minutes. Hold BOOT for programming.\n",
                static_cast<unsigned long>(kSleepIntervalMinutes));
  Serial.flush();
  esp_deep_sleep_start();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(kBootButton, INPUT_PULLUP);
  pinMode(kWakeButton, INPUT_PULLUP);

  // Set output levels before releasing the holds from the previous sleep.
  pinMode(kSdCs, OUTPUT);
  digitalWrite(kSdCs, HIGH);
  pinMode(kEpdEnable, OUTPUT);
  digitalWrite(kEpdEnable, HIGH);
  pinMode(PIN_NEOPIXEL_I2C_POWER, OUTPUT);
  digitalWrite(PIN_NEOPIXEL_I2C_POWER, HIGH);
  gpio_hold_dis(static_cast<gpio_num_t>(kEpdEnable));
  gpio_hold_dis(static_cast<gpio_num_t>(kSdCs));
  gpio_hold_dis(static_cast<gpio_num_t>(PIN_NEOPIXEL_I2C_POWER));
  gpio_hold_dis(static_cast<gpio_num_t>(SDA));
  gpio_hold_dis(static_cast<gpio_num_t>(SCL));
  // Unlike a host-only reset, a timer wake now cold-starts the SCD-40.
  // Allow its supply and startup to settle before sending any I2C commands.
  delay(kSensorPowerUpDelayMs);

  initializeDisplay();
  if (checkProgrammingButton()) {
    return;
  }
  Wire.begin();
  updateBatteryStatus(true);
  if (batteryState == BatteryState::Depleted) {
    showDepletedBattery();
    sleepUntilNextReading();
    return;
  }
  sensorAvailable = initializeSensor();
  if (sensorAvailable) {
    sensorAvailable = startMeasurementCycle();
  }
  if (!sensorAvailable) {
    showSensorError();
    sleepUntilNextReading();
  }
}

void loop() {
  if (checkProgrammingButton()) {
    delay(10);
    return;
  }
  if (!sensorAvailable) {
    sleepUntilNextReading();
    return;
  }

  const uint32_t now = millis();
  if (now - measurementStartedMs >= kMeasurementTimeoutMs) {
    Serial.println("SCD-40 timed out waiting for a valid measurement.");
    showSensorError();
    sleepUntilNextReading();
    return;
  }
  if (now - lastSensorPollMs < kSensorPollIntervalMs) {
    delay(10);
    return;
  }
  lastSensorPollMs = now;

  updateBatteryStatus();
  if (batteryState == BatteryState::Depleted) {
    showDepletedBattery();
    sleepUntilNextReading();
    return;
  }

  AirReading reading = {};
  if (!readSensor(reading)) {
    return;
  }
  if (!stopMeasurementCycle()) {
    showSensorError();
    sleepUntilNextReading();
    return;
  }
  showReading(reading);
  sleepUntilNextReading();
}

#include <Arduino.h>
#include <Adafruit_ThinkInk.h>

namespace {

// Adafruit ESP32-C6 Feather -> Adafruit 2.13" eInk breakout wiring.
// The display and SRAM share the Feather's hardware SPI bus.
constexpr int8_t kEpdCs = SS;     // GPIO 0 -> ECS
constexpr int8_t kEpdDc = A0;     // GPIO 1 -> D/C
constexpr int8_t kSramCs = A5;    // GPIO 2 -> SRCS
constexpr int8_t kEpdReset = A4;  // GPIO 3 -> RST
constexpr int8_t kEpdBusy = 7;    // GPIO 7 -> BUSY
constexpr int8_t kEpdEnable = A2; // GPIO 6 -> ENA (HIGH = powered)
constexpr int8_t kSdCs = TX;      // GPIO 16 -> SDCS (LOW = selected)

// Product 4197 has shipped with the GDEY0213B74 panel since August 2024.
// That panel needs this driver to account for its different memory alignment.
ThinkInk_213_Mono_GDEY0213B74 display(kEpdDc, kEpdReset, kEpdCs, kSramCs,
                                      kEpdBusy, &SPI);

void drawSmiley() {
  display.clearBuffer();

  const int16_t centerX = display.width() / 2;
  const int16_t centerY = display.height() / 2;
  const int16_t radius = min(display.width(), display.height()) / 2 - 6;

  // Three outlines make the face readable after the eInk refresh.
  for (int16_t inset = 0; inset < 3; ++inset) {
    display.drawCircle(centerX, centerY, radius - inset, EPD_BLACK);
  }

  const int16_t eyeOffsetX = radius * 2 / 5;
  const int16_t eyeY = centerY - radius / 4;
  const int16_t eyeRadius = max<int16_t>(4, radius / 9);
  display.fillCircle(centerX - eyeOffsetX, eyeY, eyeRadius, EPD_BLACK);
  display.fillCircle(centerX + eyeOffsetX, eyeY, eyeRadius, EPD_BLACK);

  // Draw a thick quadratic curve. The center is lower than the corners,
  // producing a smile in screen coordinates where Y increases downward.
  const int16_t mouthHalfWidth = radius * 3 / 5;
  const int16_t mouthCornerY = centerY + radius / 4;
  const int16_t mouthDepth = radius / 3;
  const int32_t mouthWidthSquared =
      static_cast<int32_t>(mouthHalfWidth) * mouthHalfWidth;

  int16_t previousX = centerX - mouthHalfWidth;
  int16_t previousY = mouthCornerY;

  for (int16_t offsetX = -mouthHalfWidth + 1; offsetX <= mouthHalfWidth;
       ++offsetX) {
    const int16_t x = centerX + offsetX;
    const int16_t y = mouthCornerY + mouthDepth -
                      (static_cast<int32_t>(mouthDepth) * offsetX * offsetX) /
                          mouthWidthSquared;

    for (int16_t thickness = -1; thickness <= 1; ++thickness) {
      display.drawLine(previousX, previousY + thickness, x, y + thickness,
                       EPD_BLACK);
    }

    previousX = x;
    previousY = y;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);

  // ENA controls the breakout's regulator. Power must be stable before the
  // display and its external SRAM are initialized.
  pinMode(kEpdEnable, OUTPUT);
  digitalWrite(kEpdEnable, HIGH);

  // The microSD socket shares SPI with the display and SRAM. Keep it
  // deselected until SD support is initialized in a future revision.
  pinMode(kSdCs, OUTPUT);
  digitalWrite(kSdCs, HIGH);
  delay(10);

  Serial.println("Starting 2.13-inch eInk smiley demo");
  display.begin(THINKINK_MONO);
  display.setRotation(0);

  drawSmiley();
  Serial.println("Refreshing eInk display...");
  display.display();
  Serial.println("Smiley drawn. Powering down the eInk breakout.");

  // display.display() does not return until the refresh is complete and puts
  // the panel controller to sleep. ENA can now go low without losing the image.
  // The breakout SRAM is volatile, so initialize and redraw after every wake.
  delay(10);
  digitalWrite(kEpdEnable, LOW);
}

void loop() {
  // eInk only needs a refresh when its image changes. Avoid needless refreshes.
  delay(1000);
}

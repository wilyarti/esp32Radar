#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif

namespace {
constexpr double kEmeraldLatitude = -23.5273;
constexpr double kEmeraldLongitude = 148.1635;
constexpr uint8_t kZoom = 6;
constexpr uint16_t kTileSize = 256;
constexpr uint8_t kRadarColor = 2;
constexpr char kTileOptions[] = "1_1";
constexpr char kWeatherMapsUrl[] = "https://api.rainviewer.com/public/weather-maps.json";
constexpr uint32_t kRefreshIntervalMs = 10UL * 60UL * 1000UL;
constexpr int kBacklightPin = 21;
constexpr int kScreenWidth = 320;
constexpr int kScreenHeight = 240;
constexpr int kHeaderHeight = 34;
constexpr int kFooterHeight = 22;
constexpr int kMapHeight = kScreenHeight - kHeaderHeight - kFooterHeight;
constexpr long kUtcOffsetSeconds = 10L * 60L * 60L;

TFT_eSPI tft;
PNG png;
uint32_t lastRefresh = 0;
int mapLeftWorldPixel = 0;
int mapTopWorldPixel = 0;
uint32_t drawingTileX = 0;
uint32_t drawingTileY = 0;

struct TilePosition {
  uint32_t x;
  uint32_t y;
  int pixelX;
  int pixelY;
};

void emeraldWorldPixel(double &worldX, double &worldY) {
  const double worldSize = static_cast<double>(kTileSize) * (1UL << kZoom);
  const double latRad = kEmeraldLatitude * M_PI / 180.0;
  worldX = (kEmeraldLongitude + 180.0) / 360.0 * worldSize;
  worldY = (1.0 - asinh(tan(latRad)) / M_PI) / 2.0 * worldSize;
}

TilePosition emeraldTilePosition() {
  double worldX, worldY;
  emeraldWorldPixel(worldX, worldY);
  TilePosition position;
  position.x = static_cast<uint32_t>(floor(worldX / kTileSize));
  position.y = static_cast<uint32_t>(floor(worldY / kTileSize));
  position.pixelX = static_cast<int>(floor(worldX)) % kTileSize;
  position.pixelY = static_cast<int>(floor(worldY)) % kTileSize;
  return position;
}

int drawPngLine(PNGDRAW *draw) {
  static uint16_t linePixels[kTileSize];
  png.getLineAsRGB565(draw, linePixels, PNG_RGB565_BIG_ENDIAN, 0);

  const int originX = static_cast<int>(drawingTileX * kTileSize) - mapLeftWorldPixel;
  const int originY = static_cast<int>(drawingTileY * kTileSize) - mapTopWorldPixel;
  const int screenY = originY + draw->y;
  if (screenY < kHeaderHeight || screenY >= kScreenHeight - kFooterHeight) return 1;

  const int visibleStart = max(0, originX);
  const int visibleEnd = min(kScreenWidth, originX + draw->iWidth);
  if (visibleStart < visibleEnd) {
    tft.pushImage(visibleStart, screenY, visibleEnd - visibleStart, 1,
                  linePixels + (visibleStart - originX));
  }
  return 1;
}

void drawOverlay(const char *timestamp) {
  tft.fillRect(0, 0, kScreenWidth, kHeaderHeight, TFT_BLACK);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Emerald radar", kScreenWidth - 6, 3, 2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(timestamp, kScreenWidth - 6, 21, 1);
  tft.setTextDatum(TL_DATUM);

  tft.fillRect(0, kScreenHeight - kFooterHeight, kScreenWidth, kFooterHeight, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("RAINVIEWER", 5, kScreenHeight - 17, 1);

  double worldX, worldY;
  emeraldWorldPixel(worldX, worldY);
  const int markerX = static_cast<int>(floor(worldX)) - mapLeftWorldPixel;
  const int markerY = static_cast<int>(floor(worldY)) - mapTopWorldPixel;
  tft.drawCircle(markerX, markerY, 5, TFT_WHITE);
  tft.drawFastHLine(markerX - 8, markerY, 17, TFT_WHITE);
  tft.drawFastVLine(markerX, markerY - 8, 17, TFT_WHITE);
}

void drawRefreshedOverlay() {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    drawOverlay("Refreshed: time unavailable");
    return;
  }
  now += kUtcOffsetSeconds;
  struct tm localTime;
  gmtime_r(&now, &localTime);
  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "Refreshed %d %b %H:%M", &localTime);
  drawOverlay(timestamp);
}

void showMessage(const char *message, uint16_t color = TFT_WHITE) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Emerald radar", kScreenWidth / 2, 54, 2);
  tft.drawCentreString(message, kScreenWidth / 2, 118, 2);
  tft.setTextDatum(TL_DATUM);
}

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (strlen(WIFI_SSID) == 0) {
    showMessage("Set WiFi in include/secrets.h", TFT_YELLOW);
    return false;
  }
  showMessage("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

bool downloadAndDrawTile(HTTPClient &http, WiFiClientSecure &client,
                         const String &host, const char *path,
                         uint32_t tileX, uint32_t tileY) {
  drawingTileX = tileX;
  drawingTileY = tileY;
  String tileUrl = host + path + "/256/" + String(kZoom) + "/" +
                   String(tileX) + "/" + String(tileY) + "/" +
                   String(kRadarColor) + "/" + kTileOptions + ".png";
  Serial.printf("Loading radar tile: %s\n", tileUrl.c_str());

  if (!http.begin(client, tileUrl)) return false;
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("Radar tile request failed: HTTP %d\n", status);
    http.end();
    return false;
  }
  const int contentLength = http.getSize();
  if (contentLength <= 0 || contentLength > 200000) {
    Serial.printf("Unexpected radar tile size: %d\n", contentLength);
    http.end();
    return false;
  }

  uint8_t *image = static_cast<uint8_t *>(malloc(contentLength));
  if (image == nullptr) {
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  const size_t bytesRead = stream->readBytes(image, contentLength);
  http.end();
  if (bytesRead != static_cast<size_t>(contentLength)) {
    Serial.printf("Incomplete radar tile: %u of %d bytes\n", static_cast<unsigned>(bytesRead), contentLength);
    free(image);
    return false;
  }

  const int openResult = png.openRAM(image, contentLength, drawPngLine);
  bool decoded = false;
  if (openResult == PNG_SUCCESS) {
    decoded = png.decode(NULL, 0) == PNG_SUCCESS;
    png.close();
  } else {
    Serial.printf("PNG decoder open error: %d\n", openResult);
  }
  free(image);
  return decoded;
}

bool fetchLatestRadarTile() {
  WiFiClientSecure client;
  // RainViewer uses HTTPS. For production deployments, replace this with certificate validation.
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, kWeatherMapsUrl)) return false;
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("Weather maps request failed: HTTP %d\n", status);
    http.end();
    return false;
  }

  DynamicJsonDocument document(16 * 1024);
  const DeserializationError jsonError = deserializeJson(document, http.getString());
  http.end();
  if (jsonError) {
    Serial.printf("Could not parse RainViewer metadata: %s\n", jsonError.c_str());
    return false;
  }
  const char *hostValue = document["host"] | "";
  JsonArray past = document["radar"]["past"].as<JsonArray>();
  if (hostValue[0] == '\0' || past.isNull() || past.size() == 0) return false;
  JsonObject latest = past[past.size() - 1];
  const char *path = latest["path"] | "";
  if (path[0] == '\0') return false;

  double worldX, worldY;
  emeraldWorldPixel(worldX, worldY);
  mapLeftWorldPixel = static_cast<int>(floor(worldX - kScreenWidth / 2.0));
  mapTopWorldPixel = static_cast<int>(floor(worldY - kMapHeight / 2.0));
  const uint32_t firstTileX = static_cast<uint32_t>(mapLeftWorldPixel / kTileSize);
  const uint32_t lastTileX = static_cast<uint32_t>((mapLeftWorldPixel + kScreenWidth - 1) / kTileSize);
  const uint32_t firstTileY = static_cast<uint32_t>(mapTopWorldPixel / kTileSize);
  const uint32_t lastTileY = static_cast<uint32_t>((mapTopWorldPixel + kMapHeight - 1) / kTileSize);

  tft.fillScreen(TFT_BLACK);
  const String host(hostValue);
  bool allTilesDecoded = true;
  for (uint32_t y = firstTileY; y <= lastTileY; ++y) {
    for (uint32_t x = firstTileX; x <= lastTileX; ++x) {
      if (!downloadAndDrawTile(http, client, host, path, x, y)) allTilesDecoded = false;
    }
  }
  if (!allTilesDecoded) return false;
  drawRefreshedOverlay();
  return true;
}
}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(kBacklightPin, OUTPUT);
  digitalWrite(kBacklightPin, HIGH);
  tft.init();
  // Rotation 3 is the opposite landscape orientation to 1; change this if your USB edge is elsewhere.
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  showMessage("Starting...");
  WiFi.setSleep(false);
}

void loop() {
  const uint32_t now = millis();
  if (lastRefresh == 0 || now - lastRefresh >= kRefreshIntervalMs) {
    lastRefresh = now;
    if (!connectWifi()) {
      if (WiFi.status() != WL_CONNECTED && strlen(WIFI_SSID) != 0) {
        showMessage("WiFi connection failed", TFT_RED);
      }
    } else {
      // Queensland uses AEST (UTC+10) year-round.
      configTime(kUtcOffsetSeconds, 0, "pool.ntp.org", "time.nist.gov");
      if (!fetchLatestRadarTile()) showMessage("Radar unavailable; retrying", TFT_RED);
    }
  }
  delay(100);
}

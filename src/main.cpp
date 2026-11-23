#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <stdlib.h>

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

TFT_eSPI tft;
PNG png;
uint32_t lastRefresh = 0;

struct TilePosition {
  uint32_t x;
  uint32_t y;
  int pixelX;
  int pixelY;
};

TilePosition emeraldTilePosition() {
  const double worldSize = static_cast<double>(kTileSize) * (1UL << kZoom);
  const double latRad = kEmeraldLatitude * M_PI / 180.0;
  const double worldX = (kEmeraldLongitude + 180.0) / 360.0 * worldSize;
  const double worldY = (1.0 - asinh(tan(latRad)) / M_PI) / 2.0 * worldSize;

  TilePosition position;
  position.x = static_cast<uint32_t>(floor(worldX / kTileSize));
  position.y = static_cast<uint32_t>(floor(worldY / kTileSize));
  position.pixelX = static_cast<int>(floor(worldX)) % kTileSize;
  position.pixelY = static_cast<int>(floor(worldY)) % kTileSize;
  return position;
}

void drawPngLine(PNGDRAW *draw) {
  const int originX = (kScreenWidth - kTileSize) / 2 - (emeraldTilePosition().pixelX - kTileSize / 2);
  const int originY = (kScreenHeight - kTileSize) / 2 - (emeraldTilePosition().pixelY - kTileSize / 2);
  const int y = originY + draw->y;
  if (y < 0 || y >= kScreenHeight) return;

  const int startX = originX + draw->x;
  const int endX = startX + draw->iWidth;
  const int visibleStart = max(0, startX);
  const int visibleEnd = min(kScreenWidth, endX);
  if (visibleStart >= visibleEnd) return;

  uint16_t *pixels = reinterpret_cast<uint16_t *>(draw->pPixels);
  tft.pushImage(visibleStart, y, visibleEnd - visibleStart, 1,
                pixels + (visibleStart - startX));
}

void showMessage(const char *message, uint16_t color = TFT_WHITE) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Emerald QLD radar", kScreenWidth / 2, 54, 2);
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
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
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

  const char *host = document["host"] | "";
  JsonArray past = document["radar"]["past"].as<JsonArray>();
  if (host[0] == '\0' || past.isNull() || past.size() == 0) return false;

  JsonObject latest = past[past.size() - 1];
  const char *path = latest["path"] | "";
  if (path[0] == '\0') return false;

  const TilePosition position = emeraldTilePosition();
  String tileUrl = String(host) + path + "/256/" + String(kZoom) + "/" +
                   String(position.x) + "/" + String(position.y) + "/" +
                   String(kRadarColor) + "/" + kTileOptions + ".png";
  Serial.printf("Loading radar tile: %s\n", tileUrl.c_str());

  WiFiClientSecure tileClient;
  tileClient.setInsecure();
  if (!http.begin(tileClient, tileUrl)) return false;
  const int tileStatus = http.GET();
  if (tileStatus != HTTP_CODE_OK) {
    Serial.printf("Radar tile request failed: HTTP %d\n", tileStatus);
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

  tft.fillScreen(TFT_BLACK);
  const int result = png.open(image, contentLength, drawPngLine);
  if (result == PNG_SUCCESS) {
    png.decode(NULL, 0);
    png.close();
    const int markerX = kScreenWidth / 2;
    const int markerY = kScreenHeight / 2;
    tft.drawCircle(markerX, markerY, 5, TFT_WHITE);
    tft.drawFastHLine(markerX - 8, markerY, 17, TFT_WHITE);
    tft.drawFastVLine(markerX, markerY - 8, 17, TFT_WHITE);
    tft.fillRect(0, kScreenHeight - 22, kScreenWidth, 22, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("EMERALD, QLD  |  RAINVIEWER", 5, kScreenHeight - 18, 1);
  } else {
    Serial.printf("PNG decoder error: %d\n", result);
  }

  free(image);
  return result == PNG_SUCCESS;
}
}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(kBacklightPin, OUTPUT);
  digitalWrite(kBacklightPin, HIGH);
  tft.init();
  tft.setRotation(1);
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
    } else if (!fetchLatestRadarTile()) {
      showMessage("Radar unavailable; retrying", TFT_RED);
    }
  }
  delay(100);
}

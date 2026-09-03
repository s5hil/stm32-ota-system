#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

// Copy secrets.h.example to secrets.h and fill in WIFI_SSID, WIFI_PASS, PI_HOST.
#include "secrets.h"

// must match UPDATE_HEADER_MAGIC in the bootloader
const uint8_t UPDATE_HEADER_MAGIC = 0xB2;

uint8_t* imageBuffer = nullptr;
uint32_t imageSize = 0;
uint32_t imageCrc = 0;
uint32_t serverBuild = 0;
String imageName = "";

uint8_t stagedSlot = 0xFF;

unsigned long lastPoll = 0;
const unsigned long POLL_INTERVAL_MS = 30000;
bool haveServerInfo = false;
bool imageReady = false;

// STM32 hardware CRC32: poly 0x04C11DB7, init 0xFFFFFFFF, no reflection, no final xor
uint32_t stm32Crc32(const uint8_t *data, uint32_t length) {
  uint32_t crc = 0xFFFFFFFF;

  for (uint32_t i = 0; i < length; i += 4) {
    uint32_t word = (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) | ((uint32_t)data[i + 2] << 16) | ((uint32_t)data[i + 3] << 24);
    crc ^= word;
    for (int b = 0; b < 32; b++) {
      if (crc & 0x80000000) {
        crc = (crc << 1) ^ 0x04C11DB7;
      } else {
        crc = crc << 1;
      }
    }
  }

  return crc;
}

String jsonField(const String& json, const String& key) {
  int k = json.indexOf("\"" + key + "\"");
  if (k < 0) return "";
  int colon = json.indexOf(":", k);
  if (colon < 0) return "";

  int start = colon + 1;
  while (start < (int)json.length() && (json[start] == ' ' || json[start] == '"')) start++;

  int end = start;
  while (end < (int)json.length() && json[end] != ',' && json[end] != '}' && json[end] != '"') end++;

  return json.substring(start, end);
}

String jsonObject(const String& json, const String& key) {
  int k = json.indexOf("\"" + key + "\"");
  if (k < 0) return "";
  int open = json.indexOf('{', k);
  if (open < 0) return "";

  int depth = 0;
  for (int i = open; i < (int)json.length(); i++) {
    if (json[i] == '{') {
      depth++;
    } else if (json[i] == '}') {
      depth--;
      if (depth == 0) return json.substring(open, i + 1);
    }
  }

  return "";
}

String cachedBody = "";
bool fetchLatestInfo() {
  HTTPClient http;
  http.begin(String(PI_HOST) + "/firmware/latest");

  int code = http.GET();
  if (code != 200) {
    Serial.printf("metadata fetch failed: %d\n", code);
    http.end();
    return false;
  }

  cachedBody = http.getString();
  http.end();

  serverBuild = jsonField(cachedBody, "build").toInt();


  Serial.printf("Server: build=%u\n", serverBuild);

  return (serverBuild > 0);
}

bool selectSlotImage(uint8_t slot) {
  String obj = jsonObject(cachedBody, (slot == 0) ? "slot_a" : "slot_b");
  if (obj.length() == 0) {
    Serial.println("Slot object not found");
    return false;
  }

  imageName = jsonField(obj, "filename");
  imageSize = jsonField(obj, "size").toInt();
  imageCrc = strtoul(jsonField(obj, "crc32").c_str(), nullptr, 16);

  Serial.printf("Slot %c image: %s size=%u crc=0x%08X\n", (slot == 0) ? 'A' : 'B', imageName.c_str(), imageSize, imageCrc);
  return (imageSize > 0 && imageName.length() > 0);
}

bool downloadImage() {
  if (imageBuffer) {
    free(imageBuffer);
    imageBuffer = nullptr;
  }

  imageBuffer = (uint8_t*)malloc(imageSize);
  if (!imageBuffer) {
    Serial.println("Buffer allocation failed");
    return false;
  }

  HTTPClient http;
  http.begin(String(PI_HOST) + "/firmware/download/" + imageName);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("Download failed: %d\n", code);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint32_t received = 0;
  unsigned long start = millis();

  while (received < imageSize && millis() - start < 30000) {
    size_t avail = stream->available();
    if (avail) {
      int n = stream->readBytes(&imageBuffer[received], min(avail, (size_t)(imageSize - received)));
      received += n;
    }
    delay(1);
  }

  http.end();

  if (received != imageSize) {
    Serial.printf("Short download: %u / %u\n", received, imageSize);
    return false;
  }

  uint32_t computed = stm32Crc32(imageBuffer, imageSize);
  if (computed != imageCrc) {
    Serial.printf("Download CRC mismatch: got 0x%08X expected 0x%08X\n", computed, imageCrc);
    return false;
  }

  Serial.println("Download verified");
  imageReady = true;
  return true;
}

bool sendToStm32() {
  Serial1.write('U');
  delay(50);

  uint8_t header[13];
  header[0] = UPDATE_HEADER_MAGIC;
  memcpy(&header[1], &imageSize, 4);
  memcpy(&header[5], &imageCrc, 4);
  memcpy(&header[9], &serverBuild, 4);
  Serial1.write(header, 13);

  char resp[2] = {0};
  unsigned long start = millis();
  int got = 0;
  while (got < 2 && millis() - start < 3000) {
    if (Serial1.available()) resp[got++] = Serial1.read();
  }

  if (resp[0] != 'O' || resp[1] != 'K') {
    Serial.printf("Header rejected: %c%c\n", resp[0], resp[1]);
    return false;
  }

  Serial.println("Waiting for erase");
  start = millis();
  bool ready = false;
  while (millis() - start < 10000) {
    if (Serial1.available() && Serial1.read() == 'R') { ready = true; break; }
  }
  if (!ready) {
    Serial.println("No ready signal");
    return false;
  }

  uint32_t sent = 0;
  while (sent < imageSize) {
    uint32_t chunk = min((uint32_t)256, imageSize - sent);
    Serial1.write(&imageBuffer[sent], chunk);
    sent += chunk;

    start = millis();
    bool acked = false;
    while (millis() - start < 5000) {
      if (Serial1.available() && Serial1.read() == 'A') { acked = true; break; }
    }
    if (!acked) {
      Serial.println("Chunk not acked");
      return false;
    }

    if ((sent % 2048) == 0 || sent == imageSize) {
      Serial.printf("Sent %u / %u\n", sent, imageSize);
    }
  }

  Serial.println("Transfer complete");
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, 18, 17); // RX=18, T=17
  delay(500);

  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nConnected, IP %s\n", WiFi.localIP().toString().c_str());

  haveServerInfo = fetchLatestInfo();
  lastPoll = millis();
}

void loop() {
  // refresh server metadata on own schedule
  if (millis() - lastPoll > POLL_INTERVAL_MS) {
    haveServerInfo = fetchLatestInfo();
    lastPoll = millis();
  }

  if (!Serial1.available()) {
    delay(10);
    return;
  }

  if (Serial1.read() != '?') return;

  uint8_t vb[4];
  unsigned long start = millis();
  int got = 0;
  while (got < 4 && millis() - start < 1000) {
    if (Serial1.available()) vb[got++] = Serial1.read();
  }

  if (got < 4) {
    Serial.println("Incomplete prompt");
    return;
  }

  uint32_t deviceBuild;
  memcpy(&deviceBuild, vb, 4);

  // 6th byte: which slot the update will land in
  uint8_t targetSlot = 0;
  bool gotSlot = false;
  start = millis();
  while(millis() - start < 1000) {
    if (Serial1.available()) {
      targetSlot = Serial1.read();
      gotSlot = true;
      break;
    }
  }

  if (!gotSlot) {
    Serial.println("No slot byte");
    return;
  }

  if (!haveServerInfo) {
    Serial.println("No server info cached - not offering");
    return;
  }

  Serial.printf("Device build %u, server build %u\n", deviceBuild, serverBuild);

  if (serverBuild <= deviceBuild) {
    Serial.println("Already up to date");
    return;
  }

  if (!selectSlotImage(targetSlot)) {
    return;
  }

  // staged image only counts if it matches the slot being asked for
  if (imageReady && stagedSlot == targetSlot) {
    if (sendToStm32()) {
      imageReady = false;
      stagedSlot = 0xFF;
    }
    return;
  }

  // nothing usable staged, fetch it next time
  if (downloadImage()) {
    stagedSlot = targetSlot;
    Serial.println("Image staged - will offer on next prompt");
  }
}

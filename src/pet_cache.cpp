#include "pet_cache.h"
#include <HTTPClient.h>
#include <mbedtls/sha256.h>

PetCache petCache;
namespace {
String pathFor(const PetClip &clip) { return String("/") + clip.id + ".pak"; }

bool hex(const char *value, size_t length) {
  if (!value || strlen(value) != length) return false;
  for (size_t i = 0; i < length; ++i)
    if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
  return true;
}

bool parse(JsonObjectConst value, PetClip &clip) {
  const char *id = value["id"] | "", *sha = value["sha256"] | "";
  if (!hex(id, 16) || !hex(sha, 64)) return false;
  strlcpy(clip.id, id, sizeof(clip.id));
  strlcpy(clip.sha256, sha, sizeof(clip.sha256));
  clip.bytes = value["bytes"] | 0U;
  const char *role = value["role"] | "action";
  clip.role = strcmp(role, "idle") == 0 ? 0 : strcmp(role, "run") == 0 ? 1 : 2;
  return clip.bytes > 280 && clip.bytes <= petCacheLimit;
}

bool hashMatches(File &file, const char *expected) {
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  mbedtls_sha256_starts(&hash, 0);
  uint8_t block[1024], digest[32];
  while (size_t bytes = file.read(block, sizeof(block))) mbedtls_sha256_update(&hash, block, bytes);
  mbedtls_sha256_finish(&hash, digest);
  mbedtls_sha256_free(&hash);
  char encoded[65];
  for (int i = 0; i < 32; ++i) snprintf(encoded + i * 2, 3, "%02x", digest[i]);
  return strcmp(encoded, expected) == 0;
}

struct Header {
  char magic[8];
  uint16_t size, fps, frames, colors;
  uint32_t bytes;
};
static_assert(sizeof(Header) == 20, "Animation header layout");
}

bool PetCache::inspect(PetClip &clip) {
  File file = LittleFS.open(pathFor(clip), "r");
  Header header{};
  if (!file || file.size() != clip.bytes || file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) != sizeof(header) ||
      memcmp(header.magic, "FISHPAK2", 8) || header.size != petSize || header.colors != 128 ||
      !header.frames || header.frames > 600 || !header.fps || header.fps > 30 ||
      sizeof(header) + 256 + (header.frames + 1) * 4 + header.bytes != clip.bytes) return false;
  file.seek(sizeof(header) + 256);
  uint32_t previous = 0;
  for (int i = 0; i <= header.frames; ++i) {
    uint32_t offset = 0;
    if (file.read(reinterpret_cast<uint8_t *>(&offset), 4) != 4 || offset > header.bytes ||
        (i == 0 ? offset != 0 : offset <= previous)) return false;
    previous = offset;
  }
  if (previous != header.bytes) return false;
  file.seek(0);
  if (!hashMatches(file, clip.sha256)) return false;
  clip.frames = header.frames;
  clip.fps = header.fps;
  return true;
}

void PetCache::load() {
  count = 0;
  revision = "";
  JsonDocument manifest;
  File file = LittleFS.open("/playlist.json", "r");
  if (!file) file = LittleFS.open("/playlist.tmp", "r");
  if (!file || deserializeJson(manifest, file)) return;
  file.close();
  bool complete = true;
  for (JsonObjectConst item : manifest["clips"].as<JsonArrayConst>()) {
    PetClip clip;
    if (count < maxPetClips && parse(item, clip) && inspect(clip)) clips[count++] = clip;
    else complete = false;
  }
  if (complete && count) revision = manifest["revision"] | "";
  ++generation;
}

void PetCache::begin() {
  mutex = xSemaphoreCreateMutex();
  if (!mutex) { status = "error"; error = "Cache mutex allocation failed"; return; }
  ready = LittleFS.begin(true, "/littlefs", 4, "spiffs");
  if (!ready) { status = "error"; error = "Unable to mount animation cache"; return; }
  capacity = LittleFS.totalBytes();
  load();
  used = LittleFS.usedBytes();
  status = count ? "ready" : "waiting";
}

void PetCache::telemetry(JsonDocument &report) const {
  report["library_revision"] = revision;
  report["sync_status"] = status;
  report["sync_error"] = error;
  report["cache_bytes"] = used;
  report["cache_capacity"] = capacity;
  report["cached_clips"] = count;
  report["downloaded"] = downloaded;
  report["download_total"] = total;
}

bool PetCache::download(const PetClip &clip, const String &base, const std::function<void()> &report) {
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(5000);
  if (!http.begin(base + "/assets/" + clip.id + ".pak")) return false;
  if (http.GET() != HTTP_CODE_OK || http.getSize() != static_cast<int>(clip.bytes)) { http.end(); return false; }
  File output = LittleFS.open("/download.part", "w");
  if (!output) { http.end(); return false; }
  uint8_t block[1024];
  uint32_t received = 0, lastData = millis(), lastReport = millis();
  auto *stream = http.getStreamPtr();
  bool valid = true;
  while (received < clip.bytes) {
    size_t available = stream->available();
    if (available) {
      size_t size = min(sizeof(block), min(available, size_t(clip.bytes - received)));
      int bytes = stream->read(block, size);
      if (bytes <= 0 || output.write(block, bytes) != static_cast<size_t>(bytes)) { valid = false; break; }
      received += bytes;
      downloaded += bytes;
      lastData = millis();
    } else if (!http.connected() || millis() - lastData > 7000) { valid = false; break; }
    else vTaskDelay(pdMS_TO_TICKS(2));
    if (millis() - lastReport >= 1500) { lastReport = millis(); report(); }
  }
  output.close();
  http.end();
  if (valid && received == clip.bytes) {
    File input = LittleFS.open("/download.part", "r");
    valid = input && hashMatches(input, clip.sha256);
    input.close();
  } else valid = false;
  if (valid) {
    LittleFS.remove(pathFor(clip));
    valid = LittleFS.rename("/download.part", pathFor(clip));
  }
  if (!valid) LittleFS.remove("/download.part");
  return valid;
}

void PetCache::sync(JsonObjectConst playlist, const String &base, const std::function<void()> &report) {
  String target = playlist["revision"] | "";
  if (!ready || target == revision || !hex(target.c_str(), 16) ||
      (lastAttempt && millis() - lastAttempt < 10000)) return;
  lastAttempt = millis();
  PetClip desired[maxPetClips];
  int desiredCount = 0;
  uint32_t required = 0;
  for (JsonObjectConst item : playlist["clips"].as<JsonArrayConst>()) {
    if (desiredCount == maxPetClips || !parse(item, desired[desiredCount])) return;
    for (int i = 0; i < desiredCount; ++i) if (!strcmp(desired[i].id, desired[desiredCount].id)) return;
    required += desired[desiredCount++].bytes;
  }
  if (!desiredCount || required > petCacheLimit) { status = "error"; error = "Invalid cache selection"; return; }
  xSemaphoreTake(mutex, portMAX_DELAY);
  status = "downloading";
  error = "";
  revision = "";
  downloaded = 0;
  total = required;
  report();
  LittleFS.remove("/download.part");
  File directory = LittleFS.open("/");
  File entry = directory.openNextFile();
  while (entry) {
    String path = entry.path();
    entry.close();
    bool keep = !path.endsWith(".pak");
    for (int i = 0; i < desiredCount; ++i) if (path == pathFor(desired[i])) keep = true;
    if (!keep) LittleFS.remove(path);
    entry = directory.openNextFile();
  }
  directory.close();
  bool success = true;
  for (int i = 0; i < desiredCount; ++i) {
    if (inspect(desired[i])) { downloaded += desired[i].bytes; continue; }
    LittleFS.remove(pathFor(desired[i]));
    if (capacity - LittleFS.usedBytes() < desired[i].bytes + 32768 ||
        !download(desired[i], base, report) || !inspect(desired[i])) {
      success = false;
      error = String("Download or verification failed: ") + desired[i].id;
      break;
    }
  }
  if (success) {
    JsonDocument manifest;
    manifest.set(playlist);
    File output = LittleFS.open("/playlist.tmp", "w");
    size_t bytes = output ? serializeJson(manifest, output) : 0;
    output.close();
    if (bytes != measureJson(manifest)) { success = false; error = "Unable to save cache manifest"; }
    else {
      LittleFS.remove("/playlist.json");
      success = LittleFS.rename("/playlist.tmp", "/playlist.json");
      if (!success) error = "Unable to commit cache manifest";
    }
  }
  if (success) {
    count = desiredCount;
    for (int i = 0; i < count; ++i) clips[i] = desired[i];
    revision = target;
    ++generation;
    status = "ready";
  } else {
    load();
    status = "error";
  }
  used = LittleFS.usedBytes();
  xSemaphoreGive(mutex);
  report();
}

bool PetCache::open(uint8_t index, uint16_t frame, File &file, uint16_t *palette, uint32_t &length) {
  if (index >= count || frame >= clips[index].frames) return false;
  file = LittleFS.open(pathFor(clips[index]), "r");
  if (!file || !file.seek(20) || file.read(reinterpret_cast<uint8_t *>(palette), 256) != 256 ||
      !file.seek(276 + frame * 4)) return false;
  uint32_t offsets[2];
  if (file.read(reinterpret_cast<uint8_t *>(offsets), 8) != 8 || offsets[1] <= offsets[0]) return false;
  length = offsets[1] - offsets[0];
  return file.seek(276 + (clips[index].frames + 1) * 4 + offsets[0]);
}

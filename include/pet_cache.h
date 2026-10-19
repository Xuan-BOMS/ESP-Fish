#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <functional>

constexpr int petSize = 168;
constexpr uint32_t petCacheLimit = 1750000;
constexpr int maxPetClips = 16;
struct PetClip {
  char id[17] = {};
  char sha256[65] = {};
  uint32_t bytes = 0;
  uint16_t frames = 0, fps = 6;
  uint8_t role = 0; // idle, run, action
};

class PetCache {
 public:
  SemaphoreHandle_t mutex = nullptr;
  PetClip clips[maxPetClips];
  uint8_t count = 0;
  uint32_t generation = 0;
  void begin();
  void sync(JsonObjectConst playlist, const String &base, const std::function<void()> &report);
  void telemetry(JsonDocument &report) const;
  bool open(uint8_t index, uint16_t frame, File &file, uint16_t *palette, uint32_t &length);
 private:
  bool ready = false;
  String revision, status = "waiting", error;
  uint32_t downloaded = 0, total = 0, used = 0, capacity = 0, lastAttempt = 0;
  void load();
  bool inspect(PetClip &clip);
  bool download(const PetClip &clip, const String &base, const std::function<void()> &report);
};

extern PetCache petCache;

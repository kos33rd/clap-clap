#pragma once

#include <cstdint>

class ClapDetector {
public:
  static const bool kModelAvailable;

  struct Result {
    bool clap = false;
    bool pair = false;
    float score = 0;
    uint64_t sample = 0;
  };

  void reset();
  Result push(int16_t sample);

private:
  static constexpr int kHop = 128;
  static constexpr int kFft = 256;
  static constexpr int kWindow = 2048;
  static constexpr int kPreRoll = 384;
  static constexpr int kBands = 16;
  static constexpr int kRmsHistory = 64;
  static constexpr int kPcmHistory = 4096;

  bool isCandidate(uint64_t block, const float* bandLevels) const;
  float scoreWindow(uint64_t start);
  void fft();

  float audio[kPcmHistory];
  float levels[kRmsHistory];
  float upperLevels[kRmsHistory];
  float real[kFft];
  float imag[kFft];
  uint64_t received = 0;
  uint64_t firstClap = 0;
  uint64_t blockedUntil = 0;
  float firstScore = 0;
  float lastRaw = 0;
  float filtered = 0;
  float blockEnergy = 0;
  float upperFiltered = 0;
  float upperEnergy = 0;
};

#include "ClapDetector.h"

#if __has_include("ClapModel.h")
#include "ClapModel.h"

#include <algorithm>
#include <cmath>

const bool ClapDetector::kModelAvailable = true;

void ClapDetector::reset() {
  received = firstClap = blockedUntil = 0;
  firstScore = lastRaw = filtered = blockEnergy = upperFiltered = upperEnergy = 0;
}

ClapDetector::Result ClapDetector::push(int16_t sample) {
  if (received == 0) {
    lastRaw = sample;
  }
  const float previousFiltered = filtered;
  filtered = 0.9615f * (filtered + sample - lastRaw);
  lastRaw = sample;
  audio[received++ % kPcmHistory] = filtered;
  blockEnergy += filtered * filtered;
  // A separate noise floor above about 700 Hz lets claps stand out over low-frequency noise.
  upperFiltered = 0.75845f * (upperFiltered + filtered - previousFiltered);
  upperEnergy += upperFiltered * upperFiltered;
  if (received % kHop != 0) {
    return {};
  }

  const uint64_t blocks = received / kHop;
  levels[(blocks - 1) % kRmsHistory] = std::sqrt(blockEnergy / kHop);
  upperLevels[(blocks - 1) % kRmsHistory] = std::sqrt(upperEnergy / kHop);
  blockEnergy = upperEnergy = 0;
  constexpr int kLookAhead = (kWindow - kPreRoll) / kHop;
  if (blocks < 32 + kLookAhead) {
    return {};
  }
  const uint64_t center = blocks - kLookAhead;
  const uint64_t peak = center * kHop;
  if (peak < blockedUntil || (!isCandidate(center, levels) && !isCandidate(center, upperLevels))) {
    return {};
  }

  const float score = scoreWindow(peak - kPreRoll);
  if (score < ClapModel::kMinScore) {
    return {};
  }
  if (!firstClap || peak - firstClap > 10400) {
    firstClap = peak;
    firstScore = score;
  } else if (peak - firstClap >= 1920) {
    // One clearly recognized clap anchors the gesture; a second may be much quieter.
    if (std::max(firstScore, score) >= ClapModel::kAnchorScore) {
      firstClap = 0;
      blockedUntil = peak + 12800;
      return {true, true, score, peak};
    }
    firstClap = peak;
    firstScore = score;
  }
  return {true, false, score, peak};
}

bool ClapDetector::isCandidate(uint64_t block, const float* bandLevels) const {
  const float level = bandLevels[block % kRmsHistory];
  if (level < 65) {
    return false;
  }
  for (int offset = 1; offset < 12; ++offset) {
    if (bandLevels[(block - offset) % kRmsHistory] >= level ||
        bandLevels[(block + offset) % kRmsHistory] > level) {
      return false;
    }
  }

  // Estimate the background before the attack so a loud first clap does not mask the second.
  const uint64_t blocks = received / kHop;
  const uint64_t start = blocks > kRmsHistory ? blocks - kRmsHistory : 0;
  const size_t count = block - 8 - start;
  float history[kRmsHistory];
  for (size_t i = 0; i < count; ++i) {
    history[i] = bandLevels[(start + i) % kRmsHistory];
  }
  auto background = history + 35 * count / 100;
  std::nth_element(history, background, history + count);
  return level >= 5 * *background;
}

float ClapDetector::scoreWindow(uint64_t start) {
  using namespace ClapModel;
  float energy = 0;
  for (int i = 0; i < kWindow; ++i) {
    const float sample = audio[(start + i) % kPcmHistory];
    energy += sample * sample;
  }
  const float gain = 1 / std::max(std::sqrt(energy / kWindow), 1.0f);
  float hidden[16];
  std::copy_n(kB1, 16, hidden);
  int feature = 0;
  for (int frame = 0; frame <= kWindow - kFft; frame += kHop) {
    for (int i = 0; i < kFft; ++i) {
      real[i] = audio[(start + frame + i) % kPcmHistory] * gain * kHann[i];
      imag[i] = 0;
    }
    fft();
    for (int i = 0; i <= kFft / 2; ++i) {
      real[i] = real[i] * real[i] + imag[i] * imag[i];
    }
    for (int band = 0; band < kBands; ++band, ++feature) {
      float power = 0;
      for (int bin = kMelFirstBin[band]; bin < kMelEndBin[band]; ++bin) {
        power += real[bin] * kMel[band * (kFft / 2 + 1) + bin];
      }
      const float value = (std::log(std::max(power, 1e-10f)) - kMean[feature]) / kScale[feature];
      for (int unit = 0; unit < 16; ++unit) {
        hidden[unit] += value * kW1[feature * 16 + unit];
      }
    }
  }

  float score = kB2[0];
  for (int unit = 0; unit < 16; ++unit) {
    score += std::max(hidden[unit], 0.0f) * kW2[unit];
  }
  return score;
}

void ClapDetector::fft() {
  for (int index = 1, reversed = 0; index < kFft; ++index) {
    int bit = kFft / 2;
    for (; reversed & bit; bit >>= 1) {
      reversed ^= bit;
    }
    reversed ^= bit;
    if (index < reversed) {
      std::swap(real[index], real[reversed]);
      std::swap(imag[index], imag[reversed]);
    }
  }
  for (int width = 2; width <= kFft; width *= 2) {
    for (int base = 0; base < kFft; base += width) {
      for (int offset = 0; offset < width / 2; ++offset) {
        const int left = base + offset;
        const int right = left + width / 2;
        const int k = offset * (kFft / width);
        const float wr = ClapModel::kTwiddleReal[k];
        const float wi = ClapModel::kTwiddleImag[k];
        const float tr = wr * real[right] - wi * imag[right];
        const float ti = wr * imag[right] + wi * real[right];
        real[right] = real[left] - tr;
        imag[right] = imag[left] - ti;
        real[left] += tr;
        imag[left] += ti;
      }
    }
  }
}

#else

// Keep microphone diagnostics and USB recording available on a fresh checkout.
const bool ClapDetector::kModelAvailable = false;

void ClapDetector::reset() {}

ClapDetector::Result ClapDetector::push(int16_t) {
  return {};
}

#endif

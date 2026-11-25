#include <Arduino.h>
#include <Matter.h>
#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <driver/usb_serial_jtag.h>
#include <esp_attr.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <math.h>
#include <mbedtls/base64.h>
#include <atomic>

#include "ClapDetector.h"

namespace {
constexpr uint8_t kBootPin = 9;
constexpr uint32_t kDebounceMs = 25;
constexpr gpio_num_t kMicBclk = GPIO_NUM_2;
constexpr gpio_num_t kMicWs = GPIO_NUM_3;
constexpr gpio_num_t kMicData = GPIO_NUM_6;
constexpr uint32_t kSampleRate = 16000;
constexpr uint32_t kMicWarmupMs = 300;
constexpr uint32_t kMicTestMs = 12000;
constexpr uint32_t kRecordingFrames = 3 * kSampleRate;

class UsbConsole : public Print {
public:
  void begin() {
    usb_serial_jtag_driver_config_t config = { .tx_buffer_size = 2048, .rx_buffer_size = 256 };
    ready = usb_serial_jtag_driver_install(&config) == ESP_OK;
  }

  int read() {
    uint8_t byte = 0;
    return ready && usb_serial_jtag_read_bytes(&byte, 1, 0) == 1 ? byte : -1;
  }

  size_t write(uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t *bytes, size_t length) override {
    return ready ? max(0, usb_serial_jtag_write_bytes(bytes, length, pdMS_TO_TICKS(20))) : 0;
  }

private:
  bool ready = false;
};

UsbConsole usbConsole;
MatterGenericSwitch matterSwitch;
bool matterStarted = false;
bool lastRawPressed = false;
bool stablePressed = false;
bool pressSeen = false;
uint32_t lastChangeMs = 0;
i2s_chan_handle_t microphone = nullptr;
ClapDetector detector;
std::atomic<bool> audioOverrun{false};
bool micReady = false;
bool detectorEnabled = ClapDetector::kModelAvailable;
bool micTestRunning = false;
bool micRecording = false;
uint32_t micPowerUpMs = 0;
uint32_t micStartMs = 0;
uint32_t micFrames = 0;
uint64_t micSumSquares = 0;
int32_t micLeftPeak = 0;
int32_t micRightPeak = 0;

void publishClick(const char *source) {
  if (matterStarted && Matter.isDeviceCommissioned() && Matter.isDeviceConnected()) {
    matterSwitch.click();
    usbConsole.printf("%s: Matter click requested\n", source);
  } else {
    usbConsole.printf("%s: Matter is offline\n", source);
  }
}

bool IRAM_ATTR onAudioOverrun(i2s_chan_handle_t, i2s_event_data_t *, void *) {
  audioOverrun.store(true, std::memory_order_relaxed);
  return false;
}

void startMicrophone() {
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  channel.dma_desc_num = 10;
  esp_err_t error = i2s_new_channel(&channel, nullptr, &microphone);
  if (error == ESP_OK) {
    i2s_std_config_t config = {};
    config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    config.gpio_cfg.bclk = kMicBclk;
    config.gpio_cfg.ws = kMicWs;
    config.gpio_cfg.dout = I2S_GPIO_UNUSED;
    config.gpio_cfg.din = kMicData;
    error = i2s_channel_init_std_mode(microphone, &config);
  }
  if (error == ESP_OK) {
    gpio_pulldown_en(kMicData);
    i2s_event_callbacks_t callbacks = {};
    callbacks.on_recv_q_ovf = onAudioOverrun;
    error = i2s_channel_register_event_callback(microphone, &callbacks, nullptr);
  }
  if (error == ESP_OK) {
    error = i2s_channel_enable(microphone);
  }
  if (error != ESP_OK) {
    if (microphone) {
      i2s_del_channel(microphone);
      microphone = nullptr;
    }
    usbConsole.printf("Mic: I2S unavailable: %s\n", esp_err_to_name(error));
    return;
  }
  micPowerUpMs = millis();
  micReady = true;
}

void startMicTest(bool record) {
  if (!micReady || micTestRunning) {
    usbConsole.println(micReady ? "Mic: busy" : "Mic: unavailable");
    return;
  }
  detector.reset();
  micFrames = 0;
  micSumSquares = 0;
  micLeftPeak = micRightPeak = 0;
  micStartMs = millis();
  micRecording = record;
  micTestRunning = true;
  if (!record) {
    usbConsole.println("Mic: measuring for 12 s (GPIO2 BCLK, GPIO3 WS, GPIO6 SD)");
  }
}

void pollAudio(uint32_t now) {
  if (!micReady) {
    return;
  }
  const uint32_t elapsed = now - micStartMs;
  if (micTestRunning && elapsed >= kMicTestMs) {
    micTestRunning = false;
    usbConsole.println(micRecording ? "Mic: recording timed out" : "Mic: test complete");
  }

  int32_t stereo[320];
  size_t received = 0;
  i2s_channel_read(microphone, stereo, sizeof(stereo), &received, 0);
  if (audioOverrun.exchange(false, std::memory_order_relaxed)) {
    detector.reset();
    micTestRunning = false;
    usbConsole.println("Mic: I2S overrun, dropped audio");
    return;
  }
  if (now - micPowerUpMs < kMicWarmupMs) {
    return;
  }
  const size_t frames = received / (2 * sizeof(int32_t));
  if (!micTestRunning) {
    if (detectorEnabled) {
      for (size_t frame = 0; frame < frames; ++frame) {
        const auto result = detector.push(static_cast<int16_t>(stereo[2 * frame] >> 16));
        if (result.clap) {
          usbConsole.printf("Clap: accepted, score %.2f\n", result.score);
        }
        if (result.pair) {
          publishClick("Double clap");
        }
      }
    }
    return;
  }
  if (micRecording) {
    if (!frames) {
      return;
    }
    if (micFrames == 0) {
      usbConsole.printf("PCM START %lu %lu\n", static_cast<unsigned long>(kSampleRate), static_cast<unsigned long>(kRecordingFrames));
    }
    int16_t mono[160];
    const size_t count = min(frames, static_cast<size_t>(kRecordingFrames - micFrames));
    for (size_t frame = 0; frame < count; ++frame) {
      mono[frame] = static_cast<int16_t>(stereo[2 * frame] >> 16);
    }
    unsigned char encoded[4 * ((sizeof(mono) + 2) / 3) + 1];
    size_t encodedSize = 0;
    mbedtls_base64_encode(encoded, sizeof(encoded), &encodedSize, reinterpret_cast<const unsigned char *>(mono), count * sizeof(int16_t));
    usbConsole.printf("PCM %lu %s\n", static_cast<unsigned long>(micFrames), reinterpret_cast<const char *>(encoded));
    micFrames += count;
    if (micFrames == kRecordingFrames) {
      micTestRunning = false;
      usbConsole.println("PCM END");
    }
    return;
  }
  for (size_t frame = 0; frame < frames; ++frame) {
    const int32_t left = stereo[2 * frame] >> 16;
    const int32_t right = stereo[2 * frame + 1] >> 16;
    micSumSquares += left * left;
    micLeftPeak = max(micLeftPeak, abs(left));
    micRightPeak = max(micRightPeak, abs(right));
    if (++micFrames == kSampleRate / 4) {
      const double rms = sqrt(static_cast<double>(micSumSquares) / micFrames);
      usbConsole.printf("Mic: left RMS=%.1f peak=%ld | right peak=%ld\n", rms, static_cast<long>(micLeftPeak), static_cast<long>(micRightPeak));
      micFrames = 0;
      micSumSquares = 0;
      micLeftPeak = micRightPeak = 0;
    }
  }
}

void logMatterEvent(matterEvent_t event, const chip::DeviceLayer::ChipDeviceEvent *details) {
  using namespace chip::DeviceLayer;
  switch (event) {
    case MATTER_CHIPOBLE_ADVERTISING_CHANGE:
      usbConsole.println(details->CHIPoBLEAdvertisingChange.Result == kActivity_Started ? "Matter: BLE advertising started" : "Matter: BLE advertising stopped");
      break;
    case MATTER_CHIPOBLE_CONNECTION_ESTABLISHED: usbConsole.println("Matter: BLE connected"); break;
    case MATTER_CHIPOBLE_CONNECTION_CLOSED: usbConsole.println("Matter: BLE disconnected"); break;
    case MATTER_COMMISSIONING_SESSION_STARTED: usbConsole.println("Matter: commissioning session started"); break;
    case MATTER_COMMISSIONING_SESSION_STOPPED: usbConsole.println("Matter: commissioning session stopped"); break;
    case MATTER_DNSSD_INITIALIZED: usbConsole.println("Matter: DNS-SD initialized"); break;
    case MATTER_COMMISSIONING_COMPLETE: usbConsole.println("Matter: commissioning complete"); break;
    case MATTER_FAIL_SAFE_TIMER_EXPIRED: usbConsole.println("Matter: commissioning timed out"); break;
    case MATTER_INTERFACE_IP_ADDRESS_CHANGED:
      if (details->InterfaceIpAddressChanged.Type == InterfaceIpChangeType::kIpV6_Assigned) {
        usbConsole.println("Matter: IPv6 address assigned");
      }
      break;
    default: break;
  }
}

void logWifiEvent(void *, esp_event_base_t, int32_t event, void *data) {
  if (event == WIFI_EVENT_STA_CONNECTED) {
    const auto *connection = static_cast<const wifi_event_sta_connected_t *>(data);
    usbConsole.printf("Wi-Fi: associated on channel %u\n", connection->channel);
  } else if (event == WIFI_EVENT_STA_DISCONNECTED) {
    const auto *disconnection = static_cast<const wifi_event_sta_disconnected_t *>(data);
    usbConsole.printf("Wi-Fi: disconnected, reason %u\n", disconnection->reason);
  }
}

void printMatterStatus() {
  if (!matterStarted) {
    usbConsole.println("Matter startup failed");
  } else if (!Matter.isDeviceCommissioned()) {
    usbConsole.println("Add the device over BLE using this Matter pairing code:");
    usbConsole.println(Matter.getManualPairingCode());
  } else {
    usbConsole.println(Matter.isDeviceConnected() ? "Matter ready on Wi-Fi" : "Matter waiting for Wi-Fi");
  }
}

void printDetectorStatus() {
  if (!micReady) {
    usbConsole.println("Clap detector: microphone unavailable");
  } else if (!ClapDetector::kModelAvailable) {
    usbConsole.println("Clap detector: no model. USB recording is available.");
  } else {
    usbConsole.println(detectorEnabled ? "Clap detector: enabled" : "Clap detector: paused");
  }
}

}  // namespace

void setup() {
  usbConsole.begin();
  pinMode(kBootPin, INPUT_PULLUP);
  lastRawPressed = stablePressed = digitalRead(kBootPin) == LOW;
  lastChangeMs = millis();

  Matter.selectNetwork(MATTER_NETWORK_WIFI);
  if (matterSwitch.begin()) {
    matterSetExampleIdentity("Clap Button");
    Matter.onEvent(logMatterEvent);
    Matter.begin();
    matterStarted = Matter.isStackStarted();
    if (matterStarted && esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, logWifiEvent, nullptr) != ESP_OK) {
      usbConsole.println("Wi-Fi: event logging unavailable");
    }
  }
  startMicrophone();
  usbConsole.println("Clap Button: ? for status, a to toggle clap detection, m to measure, r to record.");
  printMatterStatus();
  printDetectorStatus();
}

void loop() {
  const int command = usbConsole.read();
  if (command == '?') {
    printMatterStatus();
    printDetectorStatus();
  } else if (command == 'p') {
    usbConsole.println("REC READY");
  } else if (command == 'm') {
    startMicTest(false);
  } else if (command == 'r') {
    startMicTest(true);
  } else if (command == 'a') {
    if (micReady && ClapDetector::kModelAvailable) {
      detectorEnabled = !detectorEnabled;
      detector.reset();
    }
    printDetectorStatus();
  }

  const uint32_t now = millis();
  const bool pressed = digitalRead(kBootPin) == LOW;
  if (pressed != lastRawPressed) {
    lastRawPressed = pressed;
    lastChangeMs = now;
  }
  if (pressed != stablePressed && now - lastChangeMs >= kDebounceMs) {
    stablePressed = pressed;
    if (pressed) {
      pressSeen = true;
    } else if (pressSeen) {
      pressSeen = false;
      publishClick("BOOT click");
    }
  }
  pollAudio(now);
  delay(1);
}

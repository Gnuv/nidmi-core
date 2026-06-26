#pragma once

#include <Arduino.h>
#include <OSCMessage.h>
#include <WiFiUdp.h>
#include <functional>

namespace nidmi_core {

/** Meme semantique que l OSCManager NiDMI pour le broadcast / interface. */
enum class OscNetInterface : uint8_t {
  AP = 0,
  STA = 1,
  BOTH = 2
};

/**
 * OSC via UDP (WiFi). Compatible avec les usages NiDMI (float, int, notes, MIDI triplet).
 * Lib CNMAT OSC : sous PlatformIO, utiliser scripts/pio_osc_disable_bluetooth_slip.py (voir docs/OSC_PLATFORMIO.md).
 */
class OscUdpService {
public:
  OscUdpService() = default;

  using ReceiveCallback = std::function<void(const char* address, OSCMessage& msg)>;

  bool begin(uint16_t localPort);
  void end();

  void setEnabled(bool on);
  bool isEnabled() const { return enabled_; }
  bool isInitialized() const { return initialized_; }

  void setTarget(const char* targetIp, uint16_t targetPort);
  void setBroadcast(bool enable);
  void setInterface(OscNetInterface iface);

  void setReceiveCallback(ReceiveCallback cb);

  bool sendFloat(const char* address, float value);
  bool sendInt(const char* address, int value);
  bool sendNote(const char* address, uint8_t note, uint8_t velocity);
  /** Trois entiers : utile pour router du MIDI vers OSC (data1, data2, canal). */
  bool sendMidiTriplet(const char* address, int data1, int data2, int channel);
  bool sendMultiFloat(const char* address, const float* values, int count);

  void update();

private:
  bool sendOSCMessage(OSCMessage& msg);

  WiFiUDP udp_;
  String targetIP_;
  uint16_t targetPort_ = 8000;
  uint16_t localPort_ = 4000;
  bool initialized_ = false;
  bool enabled_ = true;
  bool broadcastEnabled_ = false;
  OscNetInterface netIf_ = OscNetInterface::AP;
  ReceiveCallback onReceive_;
};

}  // namespace nidmi_core

#include "OscUdpService.h"
#include <OSCMessage.h>
#include <WiFi.h>

namespace nidmi_core {

static void computeBroadcastSta(String& out) {
  if (WiFi.status() != WL_CONNECTED) {
    out = "";
    return;
  }
  IPAddress ip = WiFi.localIP();
  IPAddress subnet = WiFi.subnetMask();
  IPAddress broadcast =
      IPAddress(ip[0] | static_cast<uint8_t>(~subnet[0]), ip[1] | static_cast<uint8_t>(~subnet[1]),
                ip[2] | static_cast<uint8_t>(~subnet[2]), ip[3] | static_cast<uint8_t>(~subnet[3]));
  out = broadcast.toString();
}

bool OscUdpService::begin(uint16_t localPort) {
  end();
  localPort_ = localPort;
  if (!udp_.begin(localPort_)) {
    return false;
  }
  initialized_ = true;
  enabled_ = true;
  return true;
}

void OscUdpService::end() {
  if (initialized_) {
    udp_.stop();
    initialized_ = false;
    enabled_ = false;
  }
}

void OscUdpService::setEnabled(bool on) {
  enabled_ = on && initialized_;
}

void OscUdpService::setTarget(const char* targetIp, uint16_t targetPort) {
  targetIP_ = targetIp ? targetIp : "";
  targetPort_ = targetPort;
}

void OscUdpService::setBroadcast(bool enable) {
  broadcastEnabled_ = enable;
}

void OscUdpService::setUsbBroadcastAddress(const char* address) {
  if (address && *address) {
    usbBroadcast_ = address;
  }
}

void OscUdpService::setInterface(OscNetInterface iface) {
  netIf_ = iface;
}

void OscUdpService::setReceiveCallback(ReceiveCallback cb) {
  onReceive_ = std::move(cb);
}

bool OscUdpService::sendFloat(const char* address, float value) {
  if (!enabled_) {
    return false;
  }
  OSCMessage msg(address);
  msg.add(value);
  return sendOSCMessage(msg);
}

bool OscUdpService::sendInt(const char* address, int value) {
  if (!enabled_) {
    return false;
  }
  OSCMessage msg(address);
  msg.add((int)value);
  return sendOSCMessage(msg);
}

bool OscUdpService::sendNote(const char* address, uint8_t note, uint8_t velocity) {
  if (!enabled_) {
    return false;
  }
  OSCMessage msg(address);
  msg.add((int)note);
  msg.add((int)velocity);
  return sendOSCMessage(msg);
}

bool OscUdpService::sendMidiTriplet(const char* address, int data1, int data2, int channel) {
  if (!enabled_) {
    return false;
  }
  OSCMessage msg(address);
  msg.add((int)data1);
  msg.add((int)data2);
  msg.add((int)channel);
  return sendOSCMessage(msg);
}

bool OscUdpService::sendMultiFloat(const char* address, const float* values, int count) {
  if (!enabled_ || !values || count <= 0) {
    return false;
  }
  OSCMessage msg(address);
  for (int i = 0; i < count; ++i) {
    msg.add(values[i]);
  }
  return sendOSCMessage(msg);
}

bool OscUdpService::sendOSCMessage(OSCMessage& msg) {
  if (!enabled_) {
    return false;
  }

  const int maxRetries = 2;
  bool success = false;

  if (broadcastEnabled_) {
    if (netIf_ == OscNetInterface::AP || netIf_ == OscNetInterface::BOTH) {
      int retry = 0;
      while (retry <= maxRetries && !success) {
        if (udp_.beginPacket("192.168.4.255", targetPort_)) {
          msg.send(udp_);
          if (udp_.endPacket()) {
            success = true;
          }
        }
        ++retry;
      }
    }
    // Lien USB : l'adresse vient de setUsbBroadcastAddress(), le sous-reseau
    // n'etant pas fixe comme celui de l'AP.
    if (netIf_ == OscNetInterface::USB && usbBroadcast_.length() > 0) {
      int retry = 0;
      while (retry <= maxRetries && !success) {
        if (udp_.beginPacket(usbBroadcast_.c_str(), targetPort_)) {
          msg.send(udp_);
          if (udp_.endPacket()) {
            success = true;
          }
        }
        ++retry;
      }
    }
    if ((netIf_ == OscNetInterface::STA || netIf_ == OscNetInterface::BOTH) && WiFi.status() == WL_CONNECTED) {
      String bcastSta;
      computeBroadcastSta(bcastSta);
      if (bcastSta.length() > 0) {
        int retry = 0;
        while (retry <= maxRetries && !success) {
          if (udp_.beginPacket(bcastSta.c_str(), targetPort_)) {
            msg.send(udp_);
            if (udp_.endPacket()) {
              success = true;
            }
          }
          ++retry;
        }
      }
    }
  } else {
    if (targetIP_.length() == 0) {
      return false;
    }
    int retry = 0;
    while (retry <= maxRetries && !success) {
      if (udp_.beginPacket(targetIP_.c_str(), targetPort_)) {
        msg.send(udp_);
        if (udp_.endPacket()) {
          success = true;
        }
      }
      ++retry;
    }
  }

  return success;
}

void OscUdpService::update() {
  if (!enabled_ || !initialized_) {
    return;
  }

  const int packetSize = udp_.parsePacket();
  if (packetSize <= 0) {
    return;
  }

  uint8_t buffer[1024];
  const int len = udp_.read(buffer, static_cast<int>(sizeof(buffer) - 1));
  if (len <= 0) {
    return;
  }

  OSCMessage msg;
  msg.fill(buffer, static_cast<size_t>(len));
  if (msg.hasError() || !onReceive_) {
    return;
  }

  char addrBuf[128];
  addrBuf[0] = '\0';
  msg.getAddress(addrBuf, sizeof(addrBuf));
  const char* ap = addrBuf[0] ? addrBuf : msg.getAddress();
  onReceive_(ap ? ap : "", msg);
}

}  // namespace nidmi_core

#include "EspUartMidiTransport.h"

namespace nidmi_core {

namespace {
// UART1/2 uniquement (Serial1/Serial2) : l'UART0 est reserve a la console
// (nomme Serial0 ou Serial selon ARDUINO_USB_CDC_ON_BOOT, cf. docs/UART_MIDI.md
// "eviter les conflits avec la console"). Peripheriques disponibles variables
// selon la cible (ex. ESP32-C3 = 2 UART, pas de Serial2).
HardwareSerial* uartByIndex(uint8_t index) {
  switch (index) {
#if SOC_UART_NUM > 1
    case 1: return &Serial1;
#endif
#if SOC_UART_NUM > 2
    case 2: return &Serial2;
#endif
    default: return nullptr;
  }
}
}  // namespace

bool EspUartMidiTransport::begin(const UartMidiConfig& cfg) {
  ready_ = false;
  if (!cfg.enable) return false;

  serial_ = uartByIndex(cfg.uartIndex);
  if (!serial_) return false;

  const bool hasTx = cfg.txPin >= 0;
  const bool hasRx = cfg.rxPin >= 0;
  if (!hasTx && !hasRx) return false;

  rxEnabled_ = hasRx;
  serial_->begin(31250, SERIAL_8N1, hasRx ? cfg.rxPin : -1, hasTx ? cfg.txPin : -1);
  ready_ = true;
  return true;
}

void EspUartMidiTransport::update() {
  if (!ready_ || !rxEnabled_) return;
  // Reception non geree en v1 (voir docs/UART_MIDI.md) : on draine pour eviter
  // un debordement de buffer si RX est cable malgre tout.
  while (serial_->available()) serial_->read();
}

void EspUartMidiTransport::writeStatusData(uint8_t status, uint8_t channel, uint8_t d1, uint8_t d2, uint8_t len) {
  if (!ready_) return;
  if (channel < 1 || channel > 16) return;

  const uint8_t statusByte = static_cast<uint8_t>(status | ((channel - 1) & 0x0F));
  serial_->write(statusByte);
  if (len >= 2) serial_->write(static_cast<uint8_t>(d1 & 0x7F));
  if (len >= 3) serial_->write(static_cast<uint8_t>(d2 & 0x7F));
}

void EspUartMidiTransport::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
  writeStatusData(0x90, channel, note, velocity, 3);
}

void EspUartMidiTransport::sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
  writeStatusData(0x80, channel, note, velocity, 3);
}

void EspUartMidiTransport::sendControlChange(uint8_t channel, uint8_t control, uint8_t value) {
  writeStatusData(0xB0, channel, control, value, 3);
}

void EspUartMidiTransport::sendProgramChange(uint8_t channel, uint8_t program) {
  writeStatusData(0xC0, channel, program, 0, 2);
}

void EspUartMidiTransport::sendPitchBend(uint8_t channel, int bend) {
  // bend attendu dans [-8192, 8191] ; encode en 14 bits (0..16383, centre 8192).
  int v = bend + 8192;
  if (v < 0) v = 0;
  if (v > 16383) v = 16383;
  const uint8_t lsb = static_cast<uint8_t>(v & 0x7F);
  const uint8_t msb = static_cast<uint8_t>((v >> 7) & 0x7F);
  writeStatusData(0xE0, channel, lsb, msb, 3);
}

void EspUartMidiTransport::sendAftertouch(uint8_t channel, uint8_t pressure) {
  writeStatusData(0xD0, channel, pressure, 0, 2);
}

void EspUartMidiTransport::sendKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure) {
  writeStatusData(0xA0, channel, note, pressure, 3);
}

void EspUartMidiTransport::sendClock() {
  if (!ready_) return;
  serial_->write(static_cast<uint8_t>(0xF8));
}

void EspUartMidiTransport::sendStart() {
  if (!ready_) return;
  serial_->write(static_cast<uint8_t>(0xFA));
}

void EspUartMidiTransport::sendStop() {
  if (!ready_) return;
  serial_->write(static_cast<uint8_t>(0xFC));
}

void EspUartMidiTransport::sendContinue() {
  if (!ready_) return;
  serial_->write(static_cast<uint8_t>(0xFB));
}

}  // namespace nidmi_core

// LoRa gateway: XIAO ESP32S3 + Wio-SX1262.
//
// The laptop server is the main store. This board only
//   - receives node packets and prints them on USB serial as one line each,
//   - answers each ping with a pong and each data packet with an ACK; both
//     carry the current time.
//
// Serial lines to the server:
//   GW HELLO v1 <pinout>
//   GW PKT rssi=<dBm> snr=<dB> hex=<packet bytes>
//   GW ACK dev=<id> seq=<n> time=<unix> st=<RadioLib code>
//   GW PONG dev=<id> seq=0 time=<unix> st=<RadioLib code>
//   GW NOACK dev=<id> seq=<n>          (server not connected: packet not answered)
//   GW CRC rssi=<dBm>
//   GW ALIVE up=<s> ok=<n> crc=<n> acks=<n> time=<unix> noise=<dBm>
// Serial lines from the server:
//   T <unix>      current time; also the "server is alive" heartbeat
//
// A packet is answered only while the server heartbeat is fresh. Without the
// server the node gets no pong, sends no data and keeps its records, so
// nothing is lost while the laptop is off.
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

// Radio settings, must match the node (code/Drivers/BSP/bsp_radio.h).
#define LORA_FREQ_MHZ   866.0   // India 865-867 MHz band
#define LORA_BW_KHZ     125.0
#define LORA_SF         7
#define LORA_CR         5       // 4/5
#define LORA_SYNCWORD   0x12    // private network
#define LORA_PREAMBLE   8
#define LORA_TX_DBM     10
#define LORA_TCXO_V     1.8     // Wio-SX1262 TCXO is powered from DIO3

// Link protocol (code/App/Inc/link.h)
#define LINK_MAGIC      0xA5
#define LINK_TYPE_DATA  1
#define LINK_TYPE_ACK   2
#define LINK_TYPE_PING  3
#define LINK_TYPE_PONG  4
#define LINK_TYPE_TEST  5
#define LINK_PING_LEN   4
#define LINK_HDR_LEN    8
#define LINK_REC_LEN    16

#define ACK_DELAY_MS        30      // let the node switch to receive
#define SERVER_TIMEOUT_MS   20000   // heartbeat older than this: server is gone

// SPI is the same on both kit versions: SCK=GPIO7 (D8), MISO=GPIO8 (D9), MOSI=GPIO9 (D10).
#define PIN_SCK   7
#define PIN_MISO  8
#define PIN_MOSI  9

struct Pinout { const char *name; int nss, dio1, rst, busy, rxen; };
static const Pinout pinouts[] = {
  {"b2b", 41, 39, 42, 40, 38},
  {"header", 5 /*D4*/, 2 /*D1*/, 3 /*D2*/, 4 /*D3*/, 6 /*D5*/},
};

static Module *mod;
static SX1262 *radio;
static const Pinout *found;
static volatile bool rxFlag;
static uint32_t nOk, nCrc, nAck, lastBeat;

// time from the server
static bool haveTime;
static uint32_t timeUnix;       // server time at timeMillis
static uint32_t timeMillis;
static uint32_t lastServerMs;
static bool serverSeen;

static char lineBuf[48];
static size_t lineLen;

static void IRAM_ATTR onDio1() { rxFlag = true; }

static uint32_t nowUnix() {
  if (!haveTime) return 0;
  return timeUnix + (millis() - timeMillis) / 1000;
}

static bool serverAlive() { return serverSeen && millis() - lastServerMs < SERVER_TIMEOUT_MS; }

static uint16_t get16(const uint8_t *p) { return (uint16_t) (p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static bool tryPinout(const Pinout &p) {
  mod = new Module(p.nss, p.dio1, p.rst, p.busy);
  radio = new SX1262(mod);
  int st = radio->begin(LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR, LORA_SYNCWORD, LORA_TX_DBM, LORA_PREAMBLE, LORA_TCXO_V);
  if (st != RADIOLIB_ERR_NONE) { delete radio; delete mod; return false; }
  radio->setRfSwitchPins(p.rxen, RADIOLIB_NC);  // RXEN high in RX; TX path is switched by DIO2
  radio->setDio2AsRfSwitch(true);
  radio->setCRC(true);
  found = &p;
  return true;
}

static void handleServerLine(char *line) {
  if (line[0] == 'T' && line[1] == ' ') {
    uint32_t t = strtoul(line + 2, nullptr, 10);
    if (t > 1600000000UL) {
      timeUnix = t;
      timeMillis = millis();
      haveTime = true;
    }
    lastServerMs = millis();
    serverSeen = true;
  }
}

static void pollServer() {
  while (Serial.available()) {
    char c = (char) Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen) { lineBuf[lineLen] = 0; handleServerLine(lineBuf); lineLen = 0; }
    } else if (lineLen < sizeof lineBuf - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

static void handlePacket(const uint8_t *buf, size_t len) {
  Serial.printf("GW PKT rssi=%.1f snr=%.1f hex=", radio->getRSSI(), radio->getSNR());
  for (size_t i = 0; i < len; i++) Serial.printf("%02X", buf[i]);
  Serial.println();

  // a ping or a data packet from a node?
  if (len < LINK_PING_LEN || buf[0] != LINK_MAGIC) return;
  uint16_t dev = get16(buf + 2);
  uint8_t type;
  uint32_t seq = 0;
  if (buf[1] == LINK_TYPE_PING && len == LINK_PING_LEN) {
    type = LINK_TYPE_PONG;
  } else if (buf[1] == LINK_TYPE_DATA && len >= LINK_HDR_LEN + LINK_REC_LEN) {
    uint8_t count = buf[4];
    if (count == 0 || len != (size_t) LINK_HDR_LEN + (size_t) count * LINK_REC_LEN) return;
    type = LINK_TYPE_ACK;
    seq = get32(buf + LINK_HDR_LEN + (count - 1) * LINK_REC_LEN);
  } else if (buf[1] == LINK_TYPE_TEST && len >= 6) {
    // packet of a power-supply test build: answered like a data packet
    type = LINK_TYPE_ACK;
    seq = (uint32_t) buf[4] | ((uint32_t) buf[5] << 8);
  } else {
    return;
  }

  pollServer();
  if (!serverAlive()) {
    Serial.printf("GW NOACK dev=%u seq=%lu\n", dev, (unsigned long) seq);
    return;
  }
  // pong and ACK have the same length, so the node can time one for the other
  uint8_t ack[12];
  uint32_t t = nowUnix();
  ack[0] = LINK_MAGIC;
  ack[1] = type;
  ack[2] = (uint8_t) dev;
  ack[3] = (uint8_t) (dev >> 8);
  put32(ack + 4, seq);
  put32(ack + 8, t);
  delay(ACK_DELAY_MS);
  int st = radio->transmit(ack, sizeof ack);
  if (st == RADIOLIB_ERR_NONE) nAck++;
  Serial.printf("GW %s dev=%u seq=%lu time=%lu st=%d\n", type == LINK_TYPE_PONG ? "PONG" : "ACK", dev,
                (unsigned long) seq, (unsigned long) t, st);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);
  while (!found) {
    for (const Pinout &p : pinouts) if (tryPinout(p)) break;
    if (!found) { Serial.println("GW ERROR SX1262 not found, retrying"); delay(2000); }
  }
  radio->setPacketReceivedAction(onDio1);
  radio->startReceive();
  Serial.printf("GW HELLO v2 %s\n", found->name);
}

void loop() {
  pollServer();

  if (rxFlag) {
    rxFlag = false;
    uint8_t buf[256];
    size_t len = radio->getPacketLength();
    int st = radio->readData(buf, len);
    if (st == RADIOLIB_ERR_NONE) {
      nOk++;
      handlePacket(buf, len);
    } else if (st == RADIOLIB_ERR_CRC_MISMATCH) {
      nCrc++;
      Serial.printf("GW CRC rssi=%.1f\n", radio->getRSSI());
    }
    rxFlag = false;  // DIO1 also fires on TX done; that is not a received packet
    radio->startReceive();
  }

  if (millis() - lastBeat >= 5000) {
    lastBeat = millis();
    Serial.printf("GW ALIVE up=%lu ok=%lu crc=%lu acks=%lu time=%lu noise=%.1f\n", (unsigned long) (millis() / 1000),
                  (unsigned long) nOk, (unsigned long) nCrc, (unsigned long) nAck,
                  (unsigned long) (serverAlive() ? nowUnix() : 0), radio->getRSSI(false));
  }
}

#include <SPI.h>
#include <MFRC522.h>

// Mismos pines que usa CerrojoRelay.ino.
const uint8_t RFID_SS_PIN = 21;
const uint8_t RFID_RST_PIN = 22;
const uint8_t RFID_SCK_PIN = 18;
const uint8_t RFID_MISO_PIN = 19;
const uint8_t RFID_MOSI_PIN = 23;
const uint8_t STATUS_LED_PIN = 2;

MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
unsigned long lastHealthReport = 0;
unsigned long lastReadAt = 0;
String lastUid;

String uidToString(const byte *uid, uint8_t length)
{
  String value;
  char part[3];
  for (uint8_t i = 0; i < length; i++) {
    snprintf(part, sizeof(part), "%02X", uid[i]);
    value += part;
  }
  return value;
}

byte initializeReader()
{
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
  delay(4);
  return rfid.PCD_ReadRegister(MFRC522::VersionReg);
}

void setup()
{
  Serial.begin(115200);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  const byte version = initializeReader();

  Serial.println();
  Serial.println("=== DIAGNOSTICO RC522 ===");
  Serial.printf("VersionReg inicial: 0x%02X\n", version);
  Serial.println("Aleja y acerca una tarjeta varias veces.");
  Serial.println("Cada lectura valida debe imprimir su UID y encender el LED.");
}

void loop()
{
  if (millis() - lastHealthReport >= 2000) {
    lastHealthReport = millis();
    const byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    Serial.printf("SALUD RC522 VersionReg=0x%02X\n", version);
    if (version == 0x00 || version == 0xFF) {
      Serial.println("FALLO SPI/ALIMENTACION: reinicializando lector...");
      initializeReader();
    }
  }

  byte atqa[2];
  byte atqaSize = sizeof(atqa);
  const MFRC522::StatusCode status = rfid.PICC_WakeupA(atqa, &atqaSize);
  if (status != MFRC522::STATUS_OK && status != MFRC522::STATUS_COLLISION) {
    delay(10);
    return;
  }
  if (!rfid.PICC_ReadCardSerial()) {
    delay(10);
    return;
  }

  const String uid = uidToString(rfid.uid.uidByte, rfid.uid.size);
  if (uid != lastUid || millis() - lastReadAt >= 1000) {
    lastUid = uid;
    lastReadAt = millis();
    Serial.printf("TARJETA LEIDA uid=%s longitud=%u\n", uid.c_str(), rfid.uid.size);
    digitalWrite(STATUS_LED_PIN, HIGH);
    delay(80);
    digitalWrite(STATUS_LED_PIN, LOW);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  initializeReader();
  delay(20);
}

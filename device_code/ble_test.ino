// File: ble_nus_echo.ino

/*
/private/var/folders/j1/441zhjvd7vq9fncs7y4vlbrh0000gn/T/.arduinoIDE-unsaved202579-22963-61i3ie.zqnz6/BareMinimum/BareMinimum.ino: In function 'void setup()':
/private/var/folders/j1/441zhjvd7vq9fncs7y4vlbrh0000gn/T/.arduinoIDE-unsaved202579-22963-61i3ie.zqnz6/BareMinimum/BareMinimum.ino:25:10: error: 'void setup()::CB::onWrite(NimBLECharacteristic*)' marked 'override', but does not override
   25 |     void onWrite(NimBLECharacteristic* c) override { onWriteCB(c); }
      |          ^~~~~~~
exit status 1

Compilation error: 'void setup()::CB::onWrite(NimBLECharacteristic*)' marked 'override', but does not override
*/

#include <NimBLEDevice.h>

NimBLEServer *server;
NimBLECharacteristic *rxChar, *txChar;

void onWriteCB(NimBLECharacteristic* c){
  std::string s = c->getValue();
  // Echo back
  txChar->setValue(s);
  txChar->notify();
}

void setup() {
  Serial.begin(115200);
  NimBLEDevice::init("TimerDevice");
  server = NimBLEDevice::createServer();

  // Nordic UART Service UUIDs
  auto svc = server->createService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
  rxChar = svc->createCharacteristic("6E400002-B5A3-F393-E0A9-E50E24DCCA9E", NIMBLE_PROPERTY::WRITE);
  txChar = svc->createCharacteristic("6E400003-B5A3-F393-E0A9-E50E24DCCA9E", NIMBLE_PROPERTY::NOTIFY);

  struct CB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override { onWriteCB(c); }
  };
  rxChar->setCallbacks(new CB());

  svc->start();
  auto adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(svc->getUUID());
  adv->start();

  Serial.println("BLE NUS ready. Connect and write text to echo.");
}

void loop() { delay(50); }

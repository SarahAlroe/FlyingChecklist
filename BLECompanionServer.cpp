#include "BLECompanionServer.h"

#include "FS.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLE2901.h>

static const char* TAG = "BLECompanionServer";

// Random uuids for custom service
#define FILE_SERVICE_UUID "e2c56db5-dffb-48d2-b060-d0f5a71096e0"
#define FILE_NAME_UUID "a1b2c3d4-e5f6-47a8-9b0c-1d2e3f4a5b6c"
#define FILE_SIZE_UUID "747f65c5-1faa-4629-9205-a2306f706612"
#define FILE_HASH_UUID "102e95b9-8582-45e0-bdab-98b1716aadfb"
#define FILE_DATA_UUID "f0e1d2c3-b4a5-6789-0abc-def123456789"
#define FILE_PENDING_UUID "f0e1d2c3-b4a5-6789-0abc-def123456711"
#define COMMAND_UUID "b7e6c8d2-9f3a-4b1e-8e2c-7a6b5c4d3e2f"

#define COMMAND_DONE 0x0
#define COMMAND_NEXT_CHECKLIST_UPLOAD 0x1
#define COMMAND_RESTART_CHECKLIST_UPLOAD 0x2
#define COMMAND_CHECKLIST_DOWNLOAD 0x3
#define COMMAND_ADD_LIST_ITEM 0x4

const BLEUUID TIME_SERVICE_UUID = BLEUUID((uint16_t)0x1805);
const BLEUUID TIME_CHAR_UUID = BLEUUID((uint16_t)0x2a2b);
const BLEUUID BATTERY_SERVICE_UUID = BLEUUID((uint16_t)0x180F);
const BLEUUID BATTERY_CHAR_UUID = BLEUUID((uint16_t)0x2A19);

BLECompanionServer::BLECompanionServer() {
}

class BLECompanionServerServerCallbacks : public BLEServerCallbacks {
  BLECompanionServer* parent;
public:
  BLECompanionServerServerCallbacks(BLECompanionServer* p)
    : parent(p) {}
  void onConnect(BLEServer* pServer) override {
    parent->deviceConnected = true;
    parent->deviceWasConnected = true;
    ESP_LOGI(TAG, "Device connected");
  }
  void onDisconnect(BLEServer* pServer) override {
    parent->deviceConnected = false;
    ESP_LOGI(TAG, "Device disconnected");
  }
  /*void onMtuChanged(BLEServer* pServer, ble_gap_conn_desc *desc, uint16_t mtu) override {
        parent->chunkSize = mtu - 3; // 3 bytes for ATT header
        ESP_LOGI(TAG, "MTU changed to %d, chunk size set to %d", mtu, parent->chunkSize);
    }*/
};


class BLECompanionServer::TimeCharacteristicCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    const uint8_t* data = pCharacteristic->getData();
    if (data) {
      // Characteristic is current time https://btprodspecificationrefs.blob.core.windows.net/gatt-specification-supplement/GATT_Specification_Supplement.pdf#3.66.3
      // See https://iot.stackexchange.com/questions/7885/where-is-ble-current-time-service-data-format
      uint16_t year = data[0] + ((uint16_t)data[1] << 8);  // Year is lsb first
      uint8_t month = data[2];
      uint8_t day = data[3];
      uint8_t hours = data[4];
      uint8_t minutes = data[5];
      uint8_t seconds = data[6];
      uint8_t secondFraction = data[8];
      struct tm tm;
      tm.tm_year = year - 1900;  // tm_year is years since 1900
      tm.tm_mon = month - 1;     // tm_mon is 0-11
      tm.tm_mday = day;
      tm.tm_hour = hours;
      tm.tm_min = minutes;
      tm.tm_sec = seconds;
      time_t t = mktime(&tm);
      struct timeval tv;
      tv.tv_sec = t;
      tv.tv_usec = (secondFraction * 1000000) / 256;
      settimeofday(&tv, nullptr);
      ESP_LOGI(TAG, "Time updated via BLE: %lu.%lu", (unsigned long)tv.tv_sec, (unsigned long)tv.tv_usec);
    }
  }
};

class BLECompanionServer::CommandCallback : public BLECharacteristicCallbacks {
  BLECompanionServer* parent;
public:
  CommandCallback(BLECompanionServer* p)
    : parent(p) {}
  void onWrite(BLECharacteristic* pCharacteristic) override {
    uint8_t* command = pCharacteristic->getData();
    ESP_LOGI(TAG, "Command Characteristic updated with %s", command);
    parent->onCommand(command);
  }
};

void BLECompanionServer::begin(fs::FS& fsRef, void (*newItemCB)(ListItemToAdd, bool), bool keepFile) {
  fs = &fsRef;
  newItemCallback = newItemCB;
  keepFileAfterTransfer = keepFile;
  ESP_LOGI(TAG, "Initialising BLE Device + Server");
  BLEDevice::init("Checklist");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new BLECompanionServerServerCallbacks(this));

  // File transfer service and characteristics
  BLEService* pFileService = pServer->createService(FILE_SERVICE_UUID);
  pFilenameCharacteristic = pFileService->createCharacteristic(FILE_NAME_UUID,
                                                               BLECharacteristic::PROPERTY_READ  // | BLECharacteristic::PROPERTY_NOTIFY
  );
  pFileSizeCharacteristic = pFileService->createCharacteristic(FILE_SIZE_UUID, BLECharacteristic::PROPERTY_READ);
  pFileHashCharacteristic = pFileService->createCharacteristic(FILE_HASH_UUID, BLECharacteristic::PROPERTY_READ);
  pFilePendingCharacteristic = pFileService->createCharacteristic(FILE_PENDING_UUID, BLECharacteristic::PROPERTY_READ);
  pDataCharacteristic = pFileService->createCharacteristic(FILE_DATA_UUID,
                                                           BLECharacteristic::PROPERTY_READ |
                                                             BLECharacteristic::PROPERTY_NOTIFY |
                                                             //BLECharacteristic::PROPERTY_INDICATE |
                                                             BLECharacteristic::PROPERTY_WRITE);
  pCommandCharacteristic = pFileService->createCharacteristic(COMMAND_UUID,
                                                              BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pCommandCharacteristic->setCallbacks(new CommandCallback(this));
  pFileService->start();

  // Battery service and characteristic
  BLEService* pBatteryService = pServer->createService(BATTERY_SERVICE_UUID);
  pBatteryCharacteristic = pBatteryService->createCharacteristic(BATTERY_CHAR_UUID,
                                                                 BLECharacteristic::PROPERTY_READ);
  pBatteryService->start();

  // Add time service/characteristic
  BLEService* pTimeService = pServer->createService(TIME_SERVICE_UUID);
  pTimeCharacteristic = pTimeService->createCharacteristic(TIME_CHAR_UUID,
                                                           BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pTimeCharacteristic->setCallbacks(new TimeCharacteristicCallback());
  pTimeService->start();

  // Start advertising
  ESP_LOGI(TAG, "Starting Advertising");
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(FILE_SERVICE_UUID);
  pAdvertising->addServiceUUID(BATTERY_SERVICE_UUID);
  pAdvertising->addServiceUUID(TIME_SERVICE_UUID);
  pAdvertising->setMinPreferred(0x06);  // functions that help with iPhone connections issue
  pAdvertising->setMaxPreferred(0x12);
  pAdvertising->start();
  //pAdvertising->setScanResponse(false);
  //pAdvertising->setMinPreferred(0x0);  // set value to 0x00 to not advertise this parameter
  BLEDevice::startAdvertising();
  filesPending = hasFilesPending();
  if (filesPending) {
    prepareNextFile();
  }
}
void BLECompanionServer::setBattery(uint8_t battery) {
  pBatteryCharacteristic->setValue(&battery, 1);
}

void BLECompanionServer::onCommand(uint8_t* command) {
  if (command[0] != 0) {
    if (command[0] == COMMAND_ADD_LIST_ITEM) {
      ListItemToAdd itemToAdd;
      itemToAdd.text = pDataCharacteristic->getValue();
      itemToAdd.listIndex = 0;  //TODO
      newItemCallback(itemToAdd, true);
    }
    if (command[0] == COMMAND_NEXT_CHECKLIST_UPLOAD) {
      if (bytesSent != 0) {
        removeSentFile();
        if (filesPending) {
          prepareNextFile();
        }
      }
      startTransfer();
    }
    if (command[0] == COMMAND_RESTART_CHECKLIST_UPLOAD) {
      prepareNextFile();
      startTransfer();
    }
  }
}

void BLECompanionServer::startTransfer() {
  transferring = true;
  uint16_t connId = pServer->getConnId();
  chunkSize = min(pServer->getPeerMTU(connId) - 3, 480);  //512
  fileChecksum = (uint8_t*)calloc(chunkSize, 1);          // Must be cleared
  ESP_LOGI(TAG, "Starting transfer with chunksize %u", chunkSize);
  delay(10000);
}

bool BLECompanionServer::serveFiles() {
  if (deviceConnected && transferring) { //Cant send if disconnected
    sendNextChunk();
  }
  return transferring || (filesPending && !deviceWasConnected);  // If transferring or we have files pending and the client has not been connected and is now disconnected
}

bool BLECompanionServer::hasFilesPending() {
  File potentialFile = getNewFile();
  if (potentialFile) {
    potentialFile.close();
    pFilePendingCharacteristic->setValue(1);
    return true;
  }
  pFilePendingCharacteristic->setValue(0);
  return false;
}

File BLECompanionServer::getNewFile() {
  File newDir = fs->open(DIRNAME_RECORDINGS);
  if (!newDir || !newDir.isDirectory()) {
    ESP_LOGE(TAG, "Error scanning new dir");
    return File();
  }
  currentFile = newDir.openNextFile();
  newDir.close();
  return currentFile;
}

void BLECompanionServer::prepareNextFile() {
  currentFile = getNewFile();
  if (!currentFile) {
    ESP_LOGI(TAG, "No files to send");
    transferring = false;
    return;
  }

  currentFilename = currentFile.name();
  fileSize = currentFile.size();
  bytesSent = 0;
  pFilenameCharacteristic->setValue(currentFilename.c_str());
  pFileSizeCharacteristic->setValue((uint32_t)fileSize);
  ESP_LOGI(TAG, "Prepared file '%s' of size %ul", currentFilename.c_str(), currentFile.size());
}

void BLECompanionServer::sendNextChunk() {
  if (!transferring || !currentFile) return;

  if (bytesSent < fileSize) {
    uint8_t buffer[chunkSize];
    size_t toRead = min(chunkSize, fileSize - bytesSent);
    size_t readBytes = currentFile.read(buffer, toRead);
    if (readBytes > 0) {
      //pDataCharacteristic->setValue(buffer, readBytes);
      //pDataCharacteristic->indicate();
      //pDataCharacteristic->notify();
      uint16_t connectionId = pServer->getConnId();
      uint16_t attributeHandle = pDataCharacteristic->getHandle();
      sendNotification(connectionId, attributeHandle, buffer, readBytes);
      for (size_t i = 0; i < readBytes; ++i) {
        fileChecksum[i] ^= buffer[i];
      }
      bytesSent += readBytes;
      delay(20);
    }
    ESP_LOGI(TAG, "Sent %lu bytes out of %lu", bytesSent, fileSize);
  } else {
    ESP_LOGI(TAG, "Finished sending file %s", currentFilename.c_str());
    finishFile();
  }
}

void BLECompanionServer::finishFile() {
  ESP_LOGI(TAG, "Closing file");
  currentFile.close();

  ESP_LOGI(TAG, "Clearing characteristics");
  pDataCharacteristic->setValue("");
  pFilenameCharacteristic->setValue("");
  ESP_LOGI(TAG, "Checksum of file %s: %u (partial)", currentFilename.c_str(), fileChecksum[0]);
  pFileHashCharacteristic->setValue(fileChecksum, chunkSize);
  free(fileChecksum);
  pFileHashCharacteristic->notify();
  transferring = false;
  hasFilesPending(); // Update if we have files pending
}

void BLECompanionServer::removeSentFile() {
  if (keepFileAfterTransfer) {
    ESP_LOGI(TAG, "Renaming file");
    //fs->rename(DIRNAME_RECORDINGS + currentFilename, "/old/" + currentFilename);
  } else {
    ESP_LOGI(TAG, "Removing file %s", currentFilename.c_str());
    fs->remove(DIRNAME_RECORDINGS + String("/") + currentFilename);
  }
  filesPending = hasFilesPending();
}

// Borrowed from https://github.com/skorokithakis/middle/
// Send a BLE notification via NimBLE's ble_gatts_notify_custom(), retrying
// when the call fails due to mbuf pool exhaustion (BLE_HS_ENOMEM) or other
// transient congestion. The Arduino BLE wrapper also calls this function
// internally, but on any non-zero return it aborts the entire transfer —
// which caused ~70% of file data to be silently lost during streaming.
bool BLECompanionServer::sendNotification(uint16_t connection_id, uint16_t attribute_handle,
                              uint8_t *data, int length) {
  for (int attempt = 0; attempt < 1000; attempt++) {
    // ble_gatts_notify_custom consumes the mbuf regardless of success or
    // failure, so we must allocate a fresh one on every attempt.
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, length);
    if (om == nullptr) {
      delay(20);
      continue;
    }

    int rc = ble_gatts_notify_custom(connection_id, attribute_handle, om);
    if (rc == 0) {
      return true;
    }
    // Non-zero means congestion (BLE_HS_ENOMEM = 6, BLE_HS_EBUSY = 15, etc).
    // Wait briefly for the BLE stack to drain and retry.
    delay(20);
  }
  return false;
}
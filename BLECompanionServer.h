#pragma once

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include "FS.h"
#include "config.h"

class BLECompanionServer {
    public:
        BLECompanionServer();
        void begin(fs::FS &fs, void(*newItemCB)(ListItemToAdd, bool), bool keepFileAfterTransfer = false);
        bool serveFiles();
        class TimeCharacteristicCallback;
        class CommandCallback;
        bool deviceConnected = false;
        bool deviceWasConnected = false;
        void onCommand(uint8_t * command);
        void setBattery(uint8_t batteryLevel = 0);
        
    private:
        void startNextFile();
        void startTransfer();
        void sendNextChunk();
        void finishFile();
        void prepareNextFile();
        void removeSentFile();
        File getNewFile();
        bool hasFilesPending();
        bool sendNotification(uint16_t connection_id, uint16_t attribute_handle,
                              uint8_t *data, int length);
        size_t chunkSize = 20; // BLE max payload, Adjust based on MTU negotiation
        uint8_t *fileChecksum;
        fs::FS *fs;
        String folder;
        File currentFile;
        String currentFilename;
        size_t fileSize;
        size_t bytesSent;
        bool keepFileAfterTransfer;
        bool transferring;
        bool filesPending;
        uint8_t batteryLevel;
        void(*newItemCallback)(ListItemToAdd, bool);

        BLEServer *pServer;
        BLECharacteristic* pFilenameCharacteristic;
        BLECharacteristic* pFileHashCharacteristic;
        BLECharacteristic* pFileSizeCharacteristic;
        BLECharacteristic* pFilePendingCharacteristic;
        BLECharacteristic* pDataCharacteristic;
        BLECharacteristic* pCommandCharacteristic;
        BLECharacteristic* pBatteryCharacteristic;
        BLECharacteristic* pTimeCharacteristic;
};

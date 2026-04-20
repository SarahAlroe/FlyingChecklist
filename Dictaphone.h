#pragma once

#include "Arduino.h"
#include "ESP_I2S.h"
#include "FS.h"
#include "config.h"

class Dictaphone {
  public:
    Dictaphone(gpio_num_t clkPin, gpio_num_t dataPin);
    bool begin(fs::FS &fs, SystemStatus &status); // Initialize I2S
    void warmup(); // Read and discard some samples (should have short execution time)
    void beginRecording(); // Begin making a recording
    void continueRecording(); // Keep this going with little to no delay while recording
    void processRecording(float limiterFactor);
    void saveRecording(String filePrefix); // Save it out to an SD card
    uint16_t getSecondsRecorded();
    uint16_t getMaxRecordingSeconds();
    //uint8_t * getBuffer();
    size_t getRecordingLength();

  private:
    const char* TAG = "Dictaphone";
    SystemStatus *status;
    fs::FS *fs;
    I2SClass i2s;
    uint8_t *wavBuffer;
    size_t bufferSize;
    size_t recordingLength;
    uint8_t *saveBuffer;
    size_t saveBufferSize;
    String savePrefix;
    static void startAsyncSaveRecording(void*);
    void asyncSaveRecording();
    // Audio recording settings
    const uint16_t RECORD_MAX_S = 60;          // 60 seconds as a low-ball. PSRAM 4 or 8 MB. Not in use when not taking pictures. at 32 samples at 16bit (2bytes), thats 62 or 124 seconds, 1 or 2 minutes of recording.
    const uint16_t RECORD_SAMPLE_RATE = 32000;  // 32khz -> High sample rate for higher clock speed. 
    //const size_t RECORD_MAX_SIZE = sizeof(uint8_t) * ((RECORD_SAMPLE_RATE * (I2S_DATA_BIT_WIDTH_16BIT / 8)) * I2S_SLOT_MODE_MONO) * RECORD_MAX_S;
    const size_t MILLISECOND_LENGTH = sizeof(char) * ((RECORD_SAMPLE_RATE / 1000 * (I2S_DATA_BIT_WIDTH_16BIT / 8)) * I2S_SLOT_MODE_MONO);
    const int16_t SAVED_SAMPLE_INTERVAL = 2; // Downsampling multiplier. Keep every nth sample. 1=Keep all, 2=Keep every other.
    const bool SAVED_BIT_DEPTH_DIVIDE = false; // Divide down bit depth on save? From 16 to 8 bit. Lower file size, worse support.
    const size_t DISCARD_START = MILLISECOND_LENGTH * 100; // How many samples to discard from the front of the recording
    const size_t DISCARD_END = MILLISECOND_LENGTH * 100; // How many samples to discard from the end of the recording

};

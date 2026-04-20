#include "Arduino.h"
#include <esp_log.h>
#include "ESP_I2S.h"
#include "wav_header.h"  //Wav header from ESP_I2S
#include "FS.h"
#include "time.h"
#include "Dictaphone.h"

Dictaphone::Dictaphone(gpio_num_t clkPin, gpio_num_t dataPin) {
  i2s.setPinsPdmRx(clkPin, dataPin);
}

bool Dictaphone::begin(fs::FS &fsRef, SystemStatus &statusRef) {
  fs = &fsRef;
  status = &statusRef;
  // Setup i2s bus
  if (!i2s.begin(I2S_MODE_PDM_RX, RECORD_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
      ESP_LOGE(TAG,"Failed to initialize I2S bus!");
    return false;
  }
  ESP_LOGI(TAG,"I2S initialized");
  return true;
}

void Dictaphone::warmup(){
  //ESP_LOGV(TAG,"Warming up mic");
  for(int i=0; i<200; i++){ // Read out a number of samples to make more or less equivalent with camera
    i2s.read();
  }
}

void Dictaphone::beginRecording(){
  ESP_LOGI(TAG,"Beginning mic recording");
  free(wavBuffer); wavBuffer = NULL; // For safety free wavBuffer if not already done
  //Reserve large psram buffer (Always guaranteed to be clear, no memory management issues, slow rw fine as limited processing)
  
  ESP_LOGI(TAG ,"Used PSRAM: %lu", ESP.getPsramSize() - ESP.getFreePsram());
  bufferSize = ESP.getFreePsram()/3;
  wavBuffer = (uint8_t *)ps_malloc(bufferSize); //Allocate a third of available PSRAM. This will ensure we always have leftover space.
  ESP_LOGI(TAG,"Used PSRAM after allocation: %lu", ESP.getPsramSize() - ESP.getFreePsram());
  ESP_LOGI(TAG,"Buffer allocated at %p", wavBuffer);
  recordingLength = 0;
}

void Dictaphone::continueRecording(){
  size_t bytesToRecord = MILLISECOND_LENGTH * 100;
  if ((recordingLength + bytesToRecord) <= bufferSize) {
    recordingLength += i2s.readBytes((char *)(wavBuffer + recordingLength), bytesToRecord);  // Record 1/10 second, offset from where we recorded previously
  }
}

void Dictaphone::processRecording(float limiterFactor){
  // Process the recording
  recordingLength = recordingLength - (DISCARD_START + DISCARD_END);  // Chop off the last bit to avoid the click (+100 ms for the first bit)
  // Get min and max of samples to offset and multiply without clipping.
  int16_t *clipSamples = (int16_t *)(wavBuffer + DISCARD_START); // Chop off the first 100ms
  uint8_t *clipSamples8 = (uint8_t *)(wavBuffer + DISCARD_START);
  int32_t minSample = INT16_MAX; //32 Bit to avoid accidental over or underflows.
  int32_t maxSample = INT16_MIN;
  int64_t averageSample = 0;
  for (int i = 0; i < recordingLength / 2; i += SAVED_SAMPLE_INTERVAL) { // Iterate over sample set (in 16 bit), potentially skipping every other sample
    minSample = min(minSample, (int32_t)clipSamples[i]);
    maxSample = max(maxSample, (int32_t)clipSamples[i]);
    averageSample += clipSamples[i];
  }
  ESP_LOGI(TAG,"Recording length: %ld", (int64_t)recordingLength);
  ESP_LOGI(TAG,"Actual sample minimum: %ld", minSample);
  ESP_LOGI(TAG,"Actual sample maximum: %ld", maxSample);
  averageSample = averageSample / ((int64_t)recordingLength / (2 * SAVED_SAMPLE_INTERVAL)); // *2 because 16 bits
  // Offset minimum and maximum samples by the average sample value to avoid clipping when offsetting and amplifying samples later
  minSample = minSample - averageSample;
  maxSample = maxSample - averageSample;
  int16_t sampleMult = 1;
  if (max(abs(minSample), abs(maxSample)) != 0){ // Avoid divide by zero
    sampleMult = INT16_MAX / max(abs(minSample), abs(maxSample));  // As offset we divide target value by current value to get multiplier.
  }
  sampleMult = max(sampleMult - 1, 1); // Subtract 1, but always keept at 1+ to avoid overflows.

  // Report statistics
  ESP_LOGI(TAG,"Sample average: %lld", averageSample);
  ESP_LOGI(TAG,"Subtracted sample minimum: %ld", minSample);
  ESP_LOGI(TAG,"Subtracted sample maximum: %ld", maxSample);
  ESP_LOGI(TAG,"Sample multiplier: %d", sampleMult);

  if (limiterFactor != 0.0) {
    for (int i = 0; i < recordingLength / 2; i += SAVED_SAMPLE_INTERVAL) {
      float sampleIntermediary = (float)((clipSamples[i] - averageSample) * sampleMult) / (float)INT16_MAX;             // We applpy sample offset and multiplier. Value between -1 and 1
      // sampleIntermediary = sampleIntermediary * (1 - limiterFactor * sqrt(abs(sampleIntermediary))) / (1 - limiterFactor);  // Apply compression
      sampleIntermediary = sampleIntermediary / (sqrt(abs(sampleIntermediary)));  // Apply compression
      if (SAVED_BIT_DEPTH_DIVIDE){
        clipSamples8[i/SAVED_SAMPLE_INTERVAL] = (uint8_t)((sampleIntermediary+0.5) * (float)UINT8_MAX); // Return to (u)int domain (for 8 bit). Save in sequence of bytes + handle skips
      }else{
        clipSamples[i/SAVED_SAMPLE_INTERVAL] = (int16_t)(sampleIntermediary * (float)INT16_MAX);// Return to int domain. Save in sequence of 16bit, no matter skips
      }
    }
  } else {
    for (int i = 0; i < recordingLength / 2; i += SAVED_SAMPLE_INTERVAL) { // Iterate over sample set in 16 bit, potentially every other sample
      int16_t normalizedSample = (clipSamples[i] - averageSample) * sampleMult; 
      if (SAVED_BIT_DEPTH_DIVIDE){
        clipSamples8[i/SAVED_SAMPLE_INTERVAL] = (uint8_t) (normalizedSample / 256)+127; // Save in sequence of bytes, offset following 8bit wav spec.
      }else{
        clipSamples[i/SAVED_SAMPLE_INTERVAL] = normalizedSample; // Save in sequence no matter skips
      }
    }
  }
}

void Dictaphone::saveRecording(String filePrefix){
  size_t savedLength = recordingLength / SAVED_SAMPLE_INTERVAL;
  if (SAVED_BIT_DEPTH_DIVIDE){
    savedLength = savedLength / 2;
  }
  ESP_LOGI(TAG,"Will be saving %d bytes of audio data", savedLength);

  //Reallocate to smaller size buffer
  saveBuffer = (uint8_t *)ps_realloc(wavBuffer, savedLength);
  if (saveBuffer == NULL){
    ESP_LOGE(TAG,"Failed to reallocate buffer!");
    free(wavBuffer); wavBuffer = NULL;
    return;
  }
  wavBuffer = NULL;
  saveBufferSize = savedLength;
  savePrefix = filePrefix;
  ESP_LOGI(TAG,"Saving buffer %p of length %d with prefix %s", (void *)saveBuffer, saveBufferSize, savePrefix.c_str());

  status->processes ++;
  xTaskCreate(this->startAsyncSaveRecording, "AsyncSaveRec", 4096, this, 1, NULL);
}

void Dictaphone::startAsyncSaveRecording(void* _this){
  ESP_LOGI("STATIC", "Async save thread started");
  ((Dictaphone*)_this)->asyncSaveRecording();
  ((Dictaphone*)_this)->status->processes --;
  vTaskDelete(NULL);
}

void Dictaphone::asyncSaveRecording(){
  // Local copies of pointers etc to avoid overwriting
  uint8_t * localSaveBuffer = saveBuffer;
  size_t localSaveBufferSize = saveBufferSize;
  String localSavePrefix = savePrefix;

  ESP_LOGI(TAG,"Saving buffer %p of length %d with prefix %s", (void *)localSaveBuffer, localSaveBufferSize, localSavePrefix.c_str());

  if (localSaveBuffer == NULL){
    return;
  }

  //File name
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  String hour = (timeinfo.tm_hour < 10 ? "0" : "") + String(timeinfo.tm_hour);
  String minute = (timeinfo.tm_min < 10 ? "0" : "") + String(timeinfo.tm_min);
  String second = (timeinfo.tm_sec < 10 ? "0" : "") + String(timeinfo.tm_sec);
  String tmpPath = FILENAME_TMP_PREFIX + minute + second + FILE_WAV_EXTENSION;
  String path = DIRNAME_RECORDINGS "/" + localSavePrefix + hour + "-" + minute + "-" + second + FILE_WAV_EXTENSION;
  ESP_LOGI(TAG,"Temporary path: %s Final path: %s", tmpPath.c_str(), path.c_str());
  File file = fs->open(tmpPath.c_str(), FILE_WRITE); // Use a temporary path while writing
  if (!file) {
    ESP_LOGE(TAG,"Failed to open file %s for writing!", path.c_str());
    free(localSaveBuffer);
    return;
  }
  ESP_LOGI(TAG,"Opened file %s for writing", path.c_str());
  
  pcm_wav_header_t wavHeader;
  if (SAVED_BIT_DEPTH_DIVIDE){
    wavHeader = PCM_WAV_HEADER_DEFAULT(recordingLength, I2S_DATA_BIT_WIDTH_8BIT, (uint32_t) (RECORD_SAMPLE_RATE / SAVED_SAMPLE_INTERVAL), I2S_SLOT_MODE_MONO);
  }else{
    wavHeader = PCM_WAV_HEADER_DEFAULT(recordingLength, I2S_DATA_BIT_WIDTH_16BIT, (uint32_t) (RECORD_SAMPLE_RATE / SAVED_SAMPLE_INTERVAL), I2S_SLOT_MODE_MONO); 
  }

  bool failedWrite = file.write((uint8_t *)&wavHeader, PCM_WAV_HEADER_SIZE) != PCM_WAV_HEADER_SIZE; // Write the audio header to the file
  failedWrite &= file.write(localSaveBuffer + DISCARD_START, localSaveBufferSize) != localSaveBufferSize; // Write the audio data to the file
  if (failedWrite) {
    ESP_LOGE(TAG,"Failed to write audio data to file!");
    free(localSaveBuffer);
    file.close();
    return;
  }

  ESP_LOGI(TAG,"Saved %d bytes header and %d bytes data", PCM_WAV_HEADER_SIZE, localSaveBufferSize);

  // Close the file
  file.close();
  free(localSaveBuffer);
  fs->rename(tmpPath.c_str(), path.c_str()); // Rename to final name for further processing.
  ESP_LOGI(TAG, "StackHighWaterMark: %u bytes", uxTaskGetStackHighWaterMark(NULL));
}

uint16_t Dictaphone::getSecondsRecorded(){
  ESP_LOGI(TAG,"Recorded %d seconds of audio.", ((recordingLength)/MILLISECOND_LENGTH)/1000);
  return ((recordingLength)/MILLISECOND_LENGTH)/1000;
}

uint16_t Dictaphone::getMaxRecordingSeconds(){
  return (bufferSize/MILLISECOND_LENGTH)/1000;
}


/*uint8_t * Dictaphone::getBuffer(){
  return wavBuffer + DISCARD_START;
}*/
size_t Dictaphone::getRecordingLength(){
  return recordingLength;
}

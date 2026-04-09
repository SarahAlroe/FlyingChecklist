/* External libraries*/
#include <ArduinoJson.h>  // https://arduinojson.org/
#include <StreamUtils.h>  // https://github.com/bblanchon/ArduinoStreamUtils/
#include <Regexp.h>       // https://github.com/nickgammon/Regexp
#define CRON_USE_LOCAL_TIME true
#include "CronAlarms.h"           // https://github.com/Martin-Laclaustra/CronAlarms
#include "ccronexpr/ccronexpr.h"  // From CronAlarms, which is not actually used.

/*System libraries*/
#include "FS.h"
#include "FFat.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <SPI.h>
#include "time.h"
#include <ESP_I2S.h>
#include "esp_sntp.h"
#include "esp32s3/ulp.h"
#include "driver/rtc_io.h"
#include "soc/rtc_io_reg.h"
#include "driver/rtc_io.h"
#include <USB.h>
#include <USBMSC.h>
#include <Update.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

/*Internal*/
#include "config.h"
#include "Whisper.h"
#include "Dictaphone.h"
#include "DisplayManager.h"
#include "NotesManager.h"
#include "BLECompanionServer.h"

/*Debug tag*/
static const char *TAG = "Main";

/*NTP*/
const char *ntpServer1 = "pool.ntp.org";
const char *ntpServer2 = "ntp.se";

/*Display constants*/
const uint16_t SCREEN_WIDTH = 240;
const uint16_t SCREEN_HEIGHT = 400;
const uint16_t NOTE_HEIGHT = 7 * 8;

/*Board features configuration*/
const uint8_t FEATURE_BUTTON_ARRAY = 0b10;   // Has a 6 button array
const uint8_t FEATURE_SCROLL_WHEEL = 0b100;  // Has a scroll wheel
const uint8_t FEATURE_DAC = 0b1000;          // Has a DAC and speaker

const uint8_t buttonPins[] = { PIN_SW_0, PIN_SW_1, PIN_SW_2, PIN_SW_3, PIN_SW_4, PIN_SW_5 };

/*Board setup CONFIG*/
#define HW_VERSION 10                                                   // [Revision][Subrevision]
const uint8_t BOARD_FEATURES = 0 | FEATURE_BUTTON_ARRAY | FEATURE_DAC;  //Features should be ORed into config.
#define HAS_USB_MSC true                                               // Disable for un-interrupted usb debugging. Should be true for general use
#define HAS_CRON true
#define FORMAT_FFAT false    // Should we reset the FFAT partition on boot? Should not be enabled for non-development
#define DO_CPU_SCALING true  // Should change CPU frequency when not doing compute intensive tasks? May cause timing inconsistencies

const uint32_t BLOCK_SIZE = 512;  //esp_partition_get_main_flash_sector_size();

WiFiMulti WiFiMulti;  // WiFi client
Whisper whisper;      // Transcription service
Dictaphone dictaphone(PIN_MIC_CLK, PIN_MIC_DATA);
JsonDocument systemConfiguration;
RTC_DATA_ATTR SystemStatus status;  // Keeps system status
RTC_DATA_ATTR SystemStatus statusMonitor;         // For monitoring changes to trigger screen updartes
SystemConfig systemConfig;
BLECompanionServer bleCompanionServer(status);

USBMSC msc;                        // USB Mass Storage Class (MSC) object
EspClass _flash;                   // Flash memory object
const esp_partition_t *Partition;  // Partition information object

/*Display setup and screen buffers*/
//SPIClass *spi = NULL;
//Adafruit_SharpMem sharpDisplay(spi, PIN_SPI_CS_SHARP, SCREEN_HEIGHT, SCREEN_WIDTH);
// Note the flipped W/H! Display is rotated 90 deg.
Adafruit_SharpMem sharpDisplay(PIN_SPI_CLK, PIN_SPI_PICO, PIN_SPI_CS_SHARP, SCREEN_HEIGHT, SCREEN_WIDTH);
GFXcanvas1 screenElement(NOTE_HEIGHT, SCREEN_WIDTH);  // TODO collapse into offScreen?
GFXcanvas1 noteField(SCREEN_WIDTH - 22, NOTE_HEIGHT - 1);
GFXcanvas1 offScreen(SCREEN_HEIGHT, SCREEN_WIDTH);

DisplayManager display(sharpDisplay, screenElement, noteField, offScreen, status, systemConfig);
NotesManager notes(FFat, display);

/*Globals*/
ListItemToAdd itemToAdd;
bool timeSynced = false;         // Has NTP sync completed?
uint32_t whenToSleep = 0;        // At which millis timestamp should go back to sleep?

/*RTC Globals, persisted across sleep*/
RTC_DATA_ATTR time_t lastNTPTimestmp = 0;           // Used to know if system time is currently usable
RTC_DATA_ATTR time_t lastCronTimestamp = 0;         // Used for firing cron events only once
RTC_DATA_ATTR time_t nextCronEvent = 0;             // Used for firing cron events only once
RTC_DATA_ATTR time_t nextTranscriptionAttempt = 0;  // Used for firing cron events only once
RTC_DATA_ATTR time_t nextBLECheckin = 0;
RTC_DATA_ATTR int8_t notebookIndex = 0;             // Current notebook on screen
RTC_DATA_ATTR int16_t notesIndex = 0;               // Current scrolll index in selected book
RTC_DATA_ATTR bool freshBoot = true;                // Is this a fresh boot? Is false on wake from sleep

I2SClass i2sDAC;
void setupSpeakerAndPlay() {
  i2s_data_bit_width_t bps = I2S_DATA_BIT_WIDTH_16BIT;
  i2s_mode_t mode = I2S_MODE_STD;
  i2s_slot_mode_t slot = I2S_SLOT_MODE_MONO;
  i2sDAC.setPins(PIN_DAC_BCLK, PIN_DAC_LRCLK, PIN_DAC_DIN);
  if (!i2sDAC.begin(I2S_MODE_STD, 22000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    ESP_LOGE(TAG, "Failed to initialize I2S DAC");
    return;
  }
  ESP_LOGV(TAG, "Initializef I2S DAC");

  File audioFile = FFat.open("/test.wav");
  size_t len = audioFile.size();
  uint8_t *wavBuffer = (uint8_t *)malloc(len);
  audioFile.read(wavBuffer, len);
  i2sDAC.playWAV(wavBuffer, len);
  free(wavBuffer);
  audioFile.close();
}

void setup(void) {
  setCPUFreq(240);
  ulp_setup();
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  esp_reset_reason_t reset_reason = esp_reset_reason();

  //spi = new SPIClass(); //TODO enable SD card?
  //spi->begin(PIN_SPI_CLK, PIN_SPI_POCI, PIN_SPI_PICO, -1);

  // Reset status of particular
  status.processes = 0;
  status.recording = false;
  status.wifi = false;
  status.sleeping = false;

  initGPIO();

  initStorage();
  initializeConfiguration();

  //Load notebook
  if (freshBoot) {
    //setupSpeakerAndPlay(); // TODO give a try
    notes.init(systemConfiguration[STR_LISTS][notebookIndex][STR_NAME], getListConfig(notebookIndex), -1);
  } else {
    notes.init(systemConfiguration[STR_LISTS][notebookIndex][STR_NAME], getListConfig(notebookIndex), notesIndex);
  }
  // Initialize the display manager
  display.init(&notes);

  bool didDebugMenu = false;
  if (digitalRead(PIN_IN_POWERED) && digitalRead(PIN_SW_LEFT) && digitalRead(PIN_SW_RIGHT)) {
    debugMenu();
    didDebugMenu = true;
  }

  // If we're connected to usb and it is enabled, go into MSC mode for updating config.
  if (HAS_USB_MSC && digitalRead(PIN_IN_POWERED) && !didDebugMenu) {  // If bottom button is held down
    display.redrawDisplay();                                          // Draw current notebook to avoid display corruption
    display.clearToLargeIcon(ICON_USB);                               // Animate into usb icon

    Partition = partition();

    msc.vendorID("ESP32");
    //msc.productID("FlyingChecklist");
    msc.productID("USB_MSC");
    msc.productRevision("1.0");
    msc.onRead(onUSBRead);
    msc.onWrite(onUSBWrite);
    msc.onStartStop(onUSBStartStop);
    msc.mediaPresent(true);
    msc.isWritable(true);
    msc.begin(Partition->size / BLOCK_SIZE, BLOCK_SIZE);

    ESP_LOGI(TAG, "Initializing USB");
    ESP_LOGI(TAG, "Sector count: %d\Sector size: %d\n", Partition->size / BLOCK_SIZE, BLOCK_SIZE);

    USB.begin();
    USB.onEvent(usbEventCallback);

    while (digitalRead(PIN_IN_POWERED)) {
      display.delayDots();
      delay(400);
    }
    display.redrawFromLargeIcon(ICON_USB);
    ESP.restart();  // Reboot to reload
  }

  dictaphone.begin();
  whisper.init(systemConfiguration[STR_WHISPER][STR_DOMAIN], systemConfiguration[STR_WHISPER][STR_PATH], systemConfiguration[STR_WHISPER][STR_MODEL], systemConfiguration[STR_WHISPER][STR_LANGUAGE], systemConfiguration[STR_WHISPER][STR_TOKEN], systemConfiguration[STR_WHISPER][STR_AUTH_TYPE]);

  String timeZone = systemConfiguration[STR_TIMEZONE] | "CET-1CEST,M3.5.0,M10.5.0/3";
  setenv("TZ", timeZone.c_str(), 1);
  tzset();

  if (freshBoot) {
    display.revealDisplay();
  } else {
    display.redrawDisplay();
  }

  freshBoot = false;                        // Initialized, can skip certain setup next time
  whenToSleep = millis() + sleepIncrement;  // Keep us awake for a while longer after initial boot

  uint8_t wakeupPin = getWakeupPin();
  if (wakeupPin == PIN_SL_1) {
    // If deliberately wake by unlock, let attempt transcribing files again.
    nextTranscriptionAttempt = 0;
    nextBLECheckin = 0;
    if (digitalRead(PIN_SW_LEFT)){
      takeScreenshot();
    }
  } else if (status.locked && (wakeupPin == PIN_SW_LEFT || wakeupPin == PIN_SW_RIGHT || wakeupPin == PIN_SW_0 || wakeupPin == PIN_SW_1 || wakeupPin == PIN_SW_2 || wakeupPin == PIN_SW_3 || wakeupPin == PIN_SW_4 || wakeupPin == PIN_SW_5) ){
    // Locked and a button was pressed. Flash locked icon, then sleep
    display.flashLock();
    goToSleep();
  } else if (wakeupPin == PIN_SW_LEFT) {
    shiftNotesRelative(-1);
  } else if (wakeupPin == PIN_SW_RIGHT) {
    shiftNotesRelative(1);
  } else if (wakeupPin == PIN_SW_0) {
    handleArrayButton(0);
  } else if (wakeupPin == PIN_SW_1) {
    handleArrayButton(1);
  } else if (wakeupPin == PIN_SW_2) {
    handleArrayButton(2);
  } else if (wakeupPin == PIN_SW_3) {
    handleArrayButton(3);
  } else if (wakeupPin == PIN_SW_4) {
    handleArrayButton(4);
  } else if (wakeupPin == PIN_SW_5) {
    handleArrayButton(5);
  }
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0 || wakeup_reason == ESP_SLEEP_WAKEUP_EXT1){
    // Reset the notify LED to stop blinking when awoke from user interaction
    RTC_SLOW_MEM[ULP_ADDR_LED_FB] = 0;
    status.hasNews = false; // Also remove the hasNews flag
  }

  xTaskCreate(
    transcribeMessagesTask, "TranscribeNow",  // A name just for humans
    16384,//8192,                                     // The stack size
    NULL,                                     // Pass reference to a variable describing the task number
    1,                                        // priority
    NULL                                      // Task handle is not used here - simply pass NULL
  );

  // Print debug stuff
  ESP_LOGD(TAG, "Timestamp: %lu", time(NULL));
  printResetReason(reset_reason);
  printWakeupReason(wakeup_reason);
}

void debugMenu() {
  sharpDisplay.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, WHITE);
  sharpDisplay.setCursor(0, 0);
  sharpDisplay.println("DEBUG MENU");
  sharpDisplay.println("If unsure, press the upper of the six list buttons.");
  sharpDisplay.println("");
  sharpDisplay.println("1: Exit and Reboot");
  sharpDisplay.println("2: Update from /update.bin");
  sharpDisplay.println("3: FW update from server");
  sharpDisplay.println("4: N/A");
  sharpDisplay.println("5: N/A");
  sharpDisplay.println("6: Factory reset");
  sharpDisplay.println("");
  sharpDisplay.println("Please leave plugged into power while in debug mode");
  sharpDisplay.refresh();

  // Avoid accidental mispress
  while (aButtonIsHeld()) {
    delay(10);
  }

  while (true) {
    if (digitalRead(PIN_SW_0)) {
      // Reboot
      ESP.restart();
    }
    if (digitalRead(PIN_SW_1)) {
      // Update from FS
      sharpDisplay.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, WHITE);
      sharpDisplay.setCursor(0, 0);
      sharpDisplay.print("Updating from /update.bin ... ");
      sharpDisplay.refresh();
      if (updateFromFS()) {
        sharpDisplay.println("Updated successfully.");
      } else {
        sharpDisplay.println("Failed to update.");
      }
      sharpDisplay.println("Restarting in 4 seconds.");
      sharpDisplay.refresh();
      delay(4000);
      ESP.restart();
    }
    if (digitalRead(PIN_SW_2)) {
      // Update from server
      sharpDisplay.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, WHITE);
      sharpDisplay.setCursor(0, 0);
      sharpDisplay.print("Connecting wifi... ");
      sharpDisplay.refresh();
      if (connectWifi()) {
        sharpDisplay.println("Connected.");
        sharpDisplay.print("Getting NTP");
        sharpDisplay.refresh();
        getNTPOverWifi();
        sharpDisplay.print("Updating");
        sharpDisplay.refresh();
        File updateCert = FFat.open(FILENAME_UPDATE_CERT);
        char *cert = (char *)malloc(updateCert.size() + 1);
        updateCert.read((uint8_t *)cert, updateCert.size());
        updateCert.close();
        NetworkClientSecure client;
        ESP_LOGV(TAG, "Cert: %s", cert);
        client.setCACert(cert);
        client.setTimeout(12000);
        httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        // Note: Following redirects require certs for all connections.
        // For github, this means certs for both github.com and release-assets.githubusercontent.com
        // Multiple certs can exist in the same file.
        t_httpUpdate_return ret = httpUpdate.update(client, systemConfiguration[STR_UPDATE][STR_URL]);
        switch (ret) {
          case HTTP_UPDATE_FAILED:
            ESP_LOGE(TAG, "HTTP_UPDATE_FAILED Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
            sharpDisplay.printf("Update Failed! Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
            break;

          case HTTP_UPDATE_NO_UPDATES:
            ESP_LOGW(TAG, "HTTP_UPDATE_NO_UPDATES");
            sharpDisplay.println("No updates to install.");
            break;

          case HTTP_UPDATE_OK: 
            ESP_LOGI(TAG, "HTTP_UPDATE_OK");
            sharpDisplay.println("Upgrade success.");
            break;
        }
      } else {
        sharpDisplay.println("Failed to connect.");
      }
      sharpDisplay.println("Restarting in 6 seconds.");
      sharpDisplay.refresh();
      delay(6000);
      ESP.restart();
    }
    if (digitalRead(PIN_SW_3)) {
      return;
    }
    if (digitalRead(PIN_SW_4)) {
      return;
    }
    if (digitalRead(PIN_SW_5)) {
      // Factory reset
      sharpDisplay.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, WHITE);
      sharpDisplay.setCursor(0, 0);
      sharpDisplay.println("Are you sure you want to factory reset the device?");
      sharpDisplay.println("1: No, exit.");
      sharpDisplay.println("2: Yes, continue.");
      sharpDisplay.refresh();
      // Avoid accidental mispress
      while (aButtonIsHeld()) {
        delay(10);
      }
      while (true) {
        if (digitalRead(PIN_SW_0)) {
          ESP.restart();
        }
        if (digitalRead(PIN_SW_1)) {
          FFat.format(FFAT_WIPE_FULL);
          ESP.restart();
        }
        delay(10);
      }
    }
    delay(10);
  }
}

void loop(void) {
  setCPUFreq(80);  // Just monitoring buttons, we don't need a lot of cpu
  status.locked = digitalRead(PIN_SL_0);
  status.charging = digitalRead(PIN_IN_POWERED);
  dictaphone.warmup();  // Keep the microphone running to avoid odd recording drift.

  // Check button and slider inputs and respond accordingly
  if (checkListButtons()) {
    // If a button has been interacted with, keep awake for longer
    whenToSleep = millis() + sleepIncrement;
  }

  // If a message has been transcribed, add it
  // TODO: Less fragile way of passing messages.
  if (itemToAdd.text != "") {
    setCPUFreq(240);
    if (display.isFullscreenNote()){
      display.animFullscreenNoteOut(); // If note is currently full-screened, minimize it before adding new note.
    }
    if (itemToAdd.listIndex == notebookIndex) {
      notes.addNewNote(itemToAdd.text, itemToAdd.isSingular);
      notes.save();
    } else {
      int8_t actualNotebookIndex = notebookIndex;
      shiftNotesAbsolute(itemToAdd.listIndex);
      notes.addNewNote(itemToAdd.text, itemToAdd.isSingular);
      notes.save();
      if (!itemToAdd.shouldFollow) {
        shiftNotesAbsolute(actualNotebookIndex);
      }
      if (itemToAdd.shouldNotify) {
        if (systemConfiguration[STR_UI][STR_DISPLAY_FEEDBACK]){
          status.hasNews = true;
        }
        if (systemConfiguration[STR_UI][STR_VIBRATION_FEEDBACK]){
          xTaskCreate(asyncPlayVibrationFeedback, "AsyncPlayVibrationFeedback", 128, NULL, 1, NULL);
        }
        if (systemConfiguration[STR_UI][STR_LED_FEEDBACK]){
          RTC_SLOW_MEM[ULP_ADDR_LED_FB] = 1;
        }
      }
    }
    itemToAdd.text = "";
    whenToSleep = millis() + sleepIncrement;
  }

  if (status != statusMonitor) {
    setCPUFreq(240);
    if (status.locked && !statusMonitor.locked) {
      display.animLock(); // Make locking animation
      // If the notebook has just been locked, clean up notes
      if(!display.isFullscreenNote()){
        notes.cleanupNotes(); // Clean up only if not fullscreen.
      }
      notes.save();  // Save notes after sorting, as some time may pass before sleep save (for power loss or crash)
    } else if (!status.locked && statusMonitor.locked){
      // If has just been unlocked, animate unlock
      // display.animUnlock(); // Uncommented as it slows down interaction. TODO make configurable
    }
    if (status.wifi && !statusMonitor.wifi) {
      // Connecting to wifi tends to corrupt screen. Redraw entire screen to minimize effect
      display.redrawDisplay();
    } else {
      display.updateHeader();  // Otherwise just update the header with new changes
    }
    statusMonitor = status;
  }

  /*if (status.processes > 0) {
    // If some other task is doing a thing, delay doing stuff
    whenToSleep = millis() + sleepIncrement;
  }*/

  if (millis() > whenToSleep && !status.charging && status.processes <= 0 && itemToAdd.text == "") {
    // If nothing has happened a while, and we're not powered
    setCPUFreq(240);
    if(!display.isFullscreenNote()){
      notes.cleanupNotes(); // Clean up only if not fullscreen.
    }
    goToSleep();
  }

  delay(2);
}

void takeScreenshot(){
  uint8_t * body = sharpDisplay.getBuffer();
  size_t bufferSize = (sharpDisplay.width() * sharpDisplay.height())/8;
  unsigned char header[62] =
  {
    0x42, 0x4d, 0xfe, 0x30, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x3e, 0x00, 0x00, 0x00, 0x28, 0x00, 
    0x00, 0x00, 0x90, 0x01, 0x00, 0x00, 0xf0, 0x00, 
    0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0xc0, 0x30, 0x00, 0x00, 0x23, 0x2e, 
    0x00, 0x00, 0x23, 0x2e, 0x00, 0x00, 0x02, 0x00, 
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 
  };

  struct tm timeinfo;
  getLocalTime(&timeinfo);
  String hour = (timeinfo.tm_hour < 10 ? "0" : "") + String(timeinfo.tm_hour);
  String minute = (timeinfo.tm_min < 10 ? "0" : "") + String(timeinfo.tm_min);
  String second = (timeinfo.tm_sec < 10 ? "0" : "") + String(timeinfo.tm_sec);
  String path = "/screenshot-" + hour + "-" + minute + "-" + second + ".bmp";
  File file = FFat.open(path.c_str(), FILE_WRITE);
  ESP_LOGI(TAG,"Opened file %s for writing", path.c_str());

  file.write(header, 62);
  // BMP row size is always a multiple of 4, hence we must pad from a 400 pixel -> 50 byte row to a 52 byte row
  while (bufferSize > 0){
    bufferSize = bufferSize - file.write(body, 50); // Write 50 bytes of screenshot
    file.write(body, 50);
    file.write(0); // Write two empty bytes of padding
    file.write(0);
  }

  file.close();
}

void asyncPlayVibrationFeedback(void *pvParameters) {
  //TODO make configurable
  digitalWrite(PIN_OUT_FB, HIGH);
  delay(200);
  digitalWrite(PIN_OUT_FB, LOW);
  delay(200);
  digitalWrite(PIN_OUT_FB, HIGH);
  delay(200);
  digitalWrite(PIN_OUT_FB, LOW);
  vTaskDelete(NULL);
}

void asyncPlayVibrationTaps(void *pvParameters) {
  uint32_t tapCount = *((uint32_t *)pvParameters);
  if (tapCount < 1 || tapCount > 16){
    tapCount = 1;
  }
  for(uint16_t i = 0; i<tapCount; i++){
    pinMode(PIN_OUT_FB, INPUT_PULLUP);
    //digitalWrite(PIN_OUT_FB, HIGH);
    delay(25);
    pinMode(PIN_OUT_FB, INPUT_PULLDOWN);
    //digitalWrite(PIN_OUT_FB, LOW);
    delay(275);
  }
  vTaskDelete(NULL);
}

void ulp_setup() {
  rtc_gpio_init(PIN_SHARP_EXTCOM);          // EXTCOM pin will be flashed regularly
  rtc_gpio_pulldown_dis(PIN_SHARP_EXTCOM);  // disable VCOM pulldown (saves 80µA). what's this about?
  rtc_gpio_pullup_dis(PIN_SHARP_EXTCOM);
  rtc_gpio_set_direction(PIN_SHARP_EXTCOM, RTC_GPIO_MODE_OUTPUT_ONLY);

  rtc_gpio_init(PIN_OUT_LED);          // LED pin may be flashed regularly
  rtc_gpio_pulldown_dis(PIN_OUT_LED);  // disable VCOM pulldown (saves 80µA). what's this about?
  rtc_gpio_pullup_dis(PIN_OUT_LED);
  rtc_gpio_set_direction(PIN_OUT_LED, RTC_GPIO_MODE_OUTPUT_ONLY);

  const ulp_insn_t ulp_prog[] = {
    I_WR_REG(RTC_GPIO_OUT_REG, PIN_SHARP_EXTCOM + RTC_GPIO_OUT_DATA_S, PIN_SHARP_EXTCOM + RTC_GPIO_OUT_DATA_S, 1),  // Turn the pin on
    I_DELAY(0xFF),                                                                                                  // Datasheet says pulse of 1us+. 17.5MHz * 1 us = 17.5 cycles. We give it 255 for good measure
    I_WR_REG(RTC_GPIO_OUT_REG, PIN_SHARP_EXTCOM + RTC_GPIO_OUT_DATA_S, PIN_SHARP_EXTCOM + RTC_GPIO_OUT_DATA_S, 0),  // Turn the pin back off
    I_MOVI(R1, ULP_ADDR_LED_FB),         // R1 <- 32
    I_LD(R1, R1, 0),        // R1 <- RTC_SLOW_MEM[R1]
    M_BL(1, 1), // if less than 1 (0), skip LED blink
    I_WR_REG(RTC_GPIO_OUT_REG, PIN_OUT_LED + RTC_GPIO_OUT_DATA_S, PIN_OUT_LED + RTC_GPIO_OUT_DATA_S, 1),  // Turn the pin on
    I_DELAY(0xCD14), // Three to not overflow 16 bit uint
    I_DELAY(0xCD14),
    I_DELAY(0xCD14), // 9ms is properly visible on LED
    I_WR_REG(RTC_GPIO_OUT_REG, PIN_OUT_LED + RTC_GPIO_OUT_DATA_S, PIN_OUT_LED + RTC_GPIO_OUT_DATA_S, 0),  // Turn the pin back off
    M_LABEL(1),
    I_HALT(),                                                                                                       // Halt program
  };

  size_t size = sizeof(ulp_prog) / sizeof(ulp_insn_t);              // Size of program
  ulp_process_macros_and_load(ULP_START_OFFSET, ulp_prog, &size);   // Load the program into memory at selected offset
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);  // Enable the power domain to run during sleep
  ulp_set_wakeup_period(0, 990 * 1000);                            // Program should run once every second
  ulp_run(ULP_START_OFFSET);                                        // Start running the program
}

void initGPIO() {
  gpio_deep_sleep_hold_dis();
  if (BOARD_FEATURES & FEATURE_BUTTON_ARRAY) {
    // If has feature button array, init as input
    rtc_gpio_deinit((gpio_num_t)PIN_SW_0);
    rtc_gpio_deinit((gpio_num_t)PIN_SW_1);
    rtc_gpio_deinit((gpio_num_t)PIN_SW_2);
    rtc_gpio_deinit((gpio_num_t)PIN_SW_3);
    rtc_gpio_deinit((gpio_num_t)PIN_SW_4);
    rtc_gpio_deinit((gpio_num_t)PIN_SW_5);

    pinMode(PIN_SW_0, INPUT);
    pinMode(PIN_SW_1, INPUT);
    pinMode(PIN_SW_2, INPUT);
    pinMode(PIN_SW_3, INPUT);
    pinMode(PIN_SW_4, INPUT);
    pinMode(PIN_SW_5, INPUT);
  }

  if (BOARD_FEATURES & FEATURE_SCROLL_WHEEL) {
    rtc_gpio_deinit((gpio_num_t)PIN_ENC_A);
    rtc_gpio_deinit((gpio_num_t)PIN_ENC_B);
    rtc_gpio_deinit((gpio_num_t)PIN_ENC_SW);

    pinMode(PIN_ENC_A, INPUT);
    pinMode(PIN_ENC_B, INPUT);
    pinMode(PIN_ENC_SW, INPUT);
  }

  if (BOARD_FEATURES & FEATURE_DAC != 0) {
    // TODO setup
    // Nothing to setup, but in the i2s.
    /*
  #define PIN_DAC_DIN 11
  #define PIN_DAC_BCLK 12
  #define PIN_DAC_LRCLK 13
  */
  }

  rtc_gpio_deinit((gpio_num_t)PIN_SW_LEFT);
  rtc_gpio_deinit((gpio_num_t)PIN_SW_RIGHT);
  rtc_gpio_deinit((gpio_num_t)PIN_SL_0);
  rtc_gpio_deinit((gpio_num_t)PIN_SL_1);
  rtc_gpio_deinit((gpio_num_t)PIN_SL_2);

  pinMode(PIN_SW_RIGHT, INPUT);
  pinMode(PIN_SW_LEFT, INPUT);

  pinMode(PIN_SL_0, INPUT_PULLDOWN);
  pinMode(PIN_SL_1, INPUT);
  pinMode(PIN_SL_2, INPUT);

  pinMode(PIN_BAT_LEVEL, INPUT);
  pinMode(PIN_IN_POWERED, INPUT);

  pinMode(PIN_OUT_FB, OUTPUT);
  digitalWrite(PIN_OUT_FB, LOW);
  gpio_hold_dis((gpio_num_t)PIN_OUT_PERIPH_PWR);
  pinMode(PIN_OUT_PERIPH_PWR, OUTPUT);

  digitalWrite(PIN_OUT_PERIPH_PWR, LOW);  // For reading voltage and enabling microphone (and DAC)
}

void initStorage() {
  // TODO Check if we have an SD card
  if (FORMAT_FFAT) {
    FFat.format(FFAT_WIPE_FULL);
    ESP_LOGI(TAG, "Formatted internal storage");
  }

  if (!FFat.begin(true)) {
    ESP_LOGE(TAG, "FFat Mount Failed");
  }

  if (!FFat.exists(DIRNAME_RECORDINGS)) {
    FFat.mkdir(DIRNAME_RECORDINGS);
    ESP_LOGI(TAG, "Created %s dir", DIRNAME_RECORDINGS);
  }
  if (!FFat.exists(DIRNAME_NOTES)) {
    FFat.mkdir(DIRNAME_NOTES);
    ESP_LOGI(TAG, "Created %s dir", DIRNAME_NOTES);
  }
  if (!FFat.exists(DIRNAME_CRON)) {
    FFat.mkdir(DIRNAME_CRON);
    ESP_LOGI(TAG, "Created %s dir", DIRNAME_CRON);
  }}

bool checkListButtons() {
  if (digitalRead(PIN_SL_2) == HIGH) {
    // If slider is in outer position, start recording
    setCPUFreq(240);
    recordMessage();
    return true;
  }

  if (digitalRead(PIN_SW_LEFT) == HIGH) {
    setCPUFreq(240);
    if (status.locked){ // If locked, anim and do nothing
      display.flashLock();
      return false;
    }
    while (digitalRead(PIN_SW_LEFT) == HIGH) {
      shiftNotesRelative(-1);
      // A short cancelleable delay before repeating shift
      long repeatActionDelay = millis() + 200;  // TODO Extract to config
      while (digitalRead(PIN_SW_RIGHT) == HIGH && repeatActionDelay > millis()) {
        delay(5);
      }
    }
    return true;
  }

  if (digitalRead(PIN_SW_RIGHT) == HIGH) {
    setCPUFreq(240);
    if (status.locked){ // If locked, anim and do nothing
      display.flashLock();
      return false;
    }
    while (digitalRead(PIN_SW_RIGHT) == HIGH) {
      shiftNotesRelative(1);
      // A short cancelleable delay before repeating shift
      long repeatActionDelay = millis() + 200;
      while (digitalRead(PIN_SW_RIGHT) == HIGH && repeatActionDelay > millis()) {
        delay(5);
      }
    }
    return true;
  }

  int8_t pressedArrayButton = -1;
  for (int i = 0; i < 6; i++) {
    if (digitalRead(buttonPins[i]) == HIGH) {
      pressedArrayButton = i;
    }
  }
  if (pressedArrayButton != -1) {
    if (status.locked){ // If locked, anim and do nothing
      display.flashLock();
      return false;
    }
    ESP_LOGV(TAG, "Scanned buttons, got %d pressed", pressedArrayButton);
    setCPUFreq(240);
    handleArrayButton(pressedArrayButton);
    return true;
  }
  return false;
}

void handleArrayButton(uint8_t pressedArrayButton){
  // Handle special cases
  if (display.isFullscreenNote()){
    display.animFullscreenNoteOut();
    while (aButtonIsHeld()){
      delay(5);
    }
    return;
  }
  if (pressedArrayButton == 0 && notes.canScrollUp()) {
    do {
      notes.scrollUp();
      delay(2);
      }
    while (digitalRead(PIN_SW_0) == HIGH);
    return;
  }
  if (pressedArrayButton == 5 && notes.canScrollDown()) {
    do {
      notes.scrollDown();
      delay(2);
    }
    while (digitalRead(PIN_SW_5) == HIGH);
    return;
  }
  if (notes.hasNoteAtDot(pressedArrayButton)) {
    // Otherwise just a regular note button clicked
    float animProgress = 0.0;
    unsigned long startTime = millis();
    unsigned long endTime = startTime + 400;
    unsigned long deltaTime = 0;
    while (digitalRead(buttonPins[pressedArrayButton]) == HIGH) {
      deltaTime = millis() - startTime;
      animProgress = ((float)deltaTime) / ((float)400);
      display.animCrossNote(pressedArrayButton, animProgress);
      if (animProgress >= 1.0) {
        // If we've completed cross anim, formalize immediately to get visual effect
        ESP_LOGD(TAG, "Completed animation, crossing note at %d.", pressedArrayButton);
        notes.crossNoteAtDot(pressedArrayButton);
        while (digitalRead(buttonPins[pressedArrayButton]) == HIGH) {
          // Then just wait around for button to be let go
          delay(5);
        }
      }
      delay(1);
    }
    if (animProgress < 1.0) {
      while (animProgress > 0.0) {
        animProgress -= 0.2;
        display.animCrossNote(pressedArrayButton, animProgress);
        delay(1);
      }
    }
    if (deltaTime < 200 && systemConfiguration[STR_UI][STR_NOTE_FULLSCREEN]){
      // Short click
      // If enabled, maximize window from this element, set global flag (to enable sleep)
      display.animFullscreenNoteIn(pressedArrayButton);
    }
  }
}

bool aButtonIsHeld(){
  return digitalRead(PIN_SW_0) || digitalRead(PIN_SW_1) ||digitalRead(PIN_SW_2) ||digitalRead(PIN_SW_3) ||digitalRead(PIN_SW_4) ||digitalRead(PIN_SW_5) ||digitalRead(PIN_SW_LEFT) ||digitalRead(PIN_SW_RIGHT);
}

void shiftNotesRelative(int8_t shift) {
  int8_t newNotebookIndex = notebookIndex + shift;
  int8_t numNotebooks = systemConfiguration[STR_LISTS].size();
  if (numNotebooks <= 1){ //If there is only one page, it makes no sense to flip between it.
    return;
  }

  // Wrap around to be existing index
  while (newNotebookIndex < 0) {
    newNotebookIndex += numNotebooks;
  }
  while (newNotebookIndex >= numNotebooks) {
    newNotebookIndex -= numNotebooks;
  }
  ESP_LOGD(TAG, "Shifting relative (%d) from notebook index %d to index %d", shift, notebookIndex, newNotebookIndex);
  notebookIndex = newNotebookIndex;
  
  if (systemConfiguration[STR_UI][STR_VIBRATION_POSITION]){
    uint32_t vibrationTaps = notebookIndex+1;
    xTaskCreate(asyncPlayVibrationTaps, "AsyncPlayVibrationTaps", 128, (void *)&vibrationTaps, 1, NULL);
  }

  notes.init(systemConfiguration[STR_LISTS][notebookIndex][STR_NAME], getListConfig(notebookIndex), -1);

  display.slideOutInNotebook(shift < 0);
}

void shiftNotesAbsolute(int8_t newIndex) {
  static bool shiftNotesAbsoluteDir = false;
  if (newIndex < 0 || newIndex >= systemConfiguration[STR_LISTS].size()) {
    ESP_LOGE(TAG, "Shift index (%d) out of range 0-%d", newIndex, systemConfiguration[STR_LISTS].size());
    return;
  }
  ESP_LOGD(TAG, "Shifting absolute from notebook index %d to index %d", notebookIndex, newIndex);
  notebookIndex = newIndex;
  notes.init(systemConfiguration[STR_LISTS][notebookIndex][STR_NAME], getListConfig(notebookIndex), -1);
  display.slideOutInNotebook(shiftNotesAbsoluteDir);
  shiftNotesAbsoluteDir = !shiftNotesAbsoluteDir;  // Just bounce directions
}

TaskHandle_t asyncAnimHandle = NULL;
void recordMessage() {
  ESP_LOGI(TAG, "recording...");
  int8_t recordingIndex = notebookIndex;                                             // Save what notebook this recording will belong to
  status.recording = true;                                                           // Update status
  xTaskCreate(asyncClearToMic, "AsyncClearToMic", 2048, NULL, 1, &asyncAnimHandle);  // Show recording screen async to start recording immediately

  // Record as long as slider is held
  dictaphone.beginRecording();
  while (digitalRead(PIN_SL_2) == HIGH) {
    dictaphone.continueRecording();
  }

  uint16_t secondsRecorded = dictaphone.getSecondsRecorded();
  ESP_LOGI(TAG, "Recording finished at %d seconds.", secondsRecorded);
  status.recording = false;
  vTaskDelete(asyncAnimHandle);  // Stop recording animation
  if (secondsRecorded < 1) {
    // If recorder was bumped, no reason to transcribe it
    // Instead animate out and return
    display.redrawFromLargeIcon(ICON_MIC);
    return;
  }
  // Animate saving recording on screen
  xTaskCreate(asyncClearToSave, "AsyncClearToSave", 2048, NULL, 1, &asyncAnimHandle);  // Show recording screen async to start recording immediately
  status.processes++;
  dictaphone.processRecording(0);
  ESP_LOGI(TAG, "Recording processed.");
  dictaphone.saveRecording(FFat, String(recordingIndex) + ", ");
  vTaskDelete(asyncAnimHandle);  // Stop save animation
  nextTranscriptionAttempt = 0; // Reset transcription timer
  status.filesWaiting ++; // Has another file for sure.
  status.processes--;
  // Animate out and return
  display.redrawFromLargeIcon(ICON_SAVE);
  while (aButtonIsHeld()){
    delay(5);
  }
}

void asyncClearToMic(void *pvParameters) {
  display.clearToLargeIcon(ICON_MIC);  // Animate screen to show recording
  while (true) {
    display.drawLargeTimer(dictaphone.getSecondsRecorded());
    delay(1000);
  }
}

void asyncClearToSave(void *pvParameters) {
  display.clearToLargeIcon(ICON_SAVE, false);
  while (true) {
    display.delayDots();
    delay(200);
  }
}

bool connectWifi() {
  if (status.wifi) {
    return true;  //If allready connected, no need to do it again.
  }
  // Connect to wifi
  WiFi.mode(WIFI_STA);
  WiFiMulti.setStrictMode(true);
  JsonArray wifiAPs = systemConfiguration[STR_WIFI];
  for (JsonVariant wifiAP : wifiAPs) {
    WiFiMulti.addAP(wifiAP[STR_SSID], wifiAP[STR_PASSWORD]);
  }

  ESP_LOGI(TAG, "Connecting Wifi...");
  sntp_set_time_sync_notification_cb(timeavailable);  // Notify when NTP sync completed
  if (WiFiMulti.run(5 * 1000) != WL_CONNECTED) {      // Try connecting for up to 5 seconds
    ESP_LOGW(TAG, "Failed to connect to WIFI");
    status.wifi = false;
    return false;
  }
  //configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
  String timeZone = systemConfiguration[STR_TIMEZONE] | "CET-1CEST,M3.5.0,M10.5.0/3";
  configTzTime(timeZone.c_str(), ntpServer1, ntpServer2);

  WiFi.setSleep(true);  // Enable modem sleep to save power

  status.wifi = true;
  ESP_LOGI(TAG, "WiFi connected. IP address: %s", WiFi.localIP().toString().c_str());
  return true;
}

void disconnectWifi() {
  if (!status.wifi) {
    status.wifi = false;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

uint8_t countRecordingsToProcess() {
  File recordingDirectory = FFat.open(DIRNAME_RECORDINGS);
  if (!recordingDirectory || !recordingDirectory.isDirectory()) {
    ESP_LOGE(TAG, "Error opening directory %s ", DIRNAME_RECORDINGS);
    return false;
  }
  uint8_t fileCount;
  File file = recordingDirectory.openNextFile();
  while (file) {
    fileCount++;
    file.close();
    file = recordingDirectory.openNextFile();
  }
  ESP_LOGV(TAG, "Has %d new files to upload", fileCount);
  recordingDirectory.close();
  status.filesWaiting = fileCount; // Technically a side effect, but never incorrect.
  return fileCount;
}

bool hasRecordingsToProcess(){
  uint8_t recordingCount = countRecordingsToProcess();
  return recordingCount >= 1;
}

void transcribeMessagesTask(void *pvParameters) {
  bool hasWifi = systemConfiguration[STR_HAS_WIFI];
  bool hasBLE = systemConfiguration[STR_HAS_BLE];
  for (;;) {
    status.processes++;
    if (status.sleeping){ // If going to sleep, stop doing things.
      status.processes--;
      vTaskDelete(NULL);
    }
    updateBatteryPercentage();
    unsigned long minimumTimeAvailable = 0;

    if(hasBLE && (time(NULL) < ANCIENT_TIME || status.filesWaiting || status.responsesWaiting || nextBLECheckin<time(NULL)) ){
      beginBLE();
      bleCompanionServer.setBattery(status.battery);
      minimumTimeAvailable = millis() + 10 * MS_TO_S_FACTOR;
      nextBLECheckin = time(NULL) + 12*S_TO_H_FACTOR; // Check in with BLE twice every day if not doing other things. //TODO Extract to config
    }

    // Update time if not synced
    if (time(NULL) < ANCIENT_TIME                                              // If time not set (in the past), start setting it,
        && (lastNTPTimestmp == 0 || (lastNTPTimestmp + 1800) < time(NULL))) {  // But only if first time this boot, or 30 mins since last attempt
      if(hasWifi){
        getNTPOverWifi();
      }else if(status.ble){
        while (time(NULL) < ANCIENT_TIME && millis() < 20 * MS_TO_S_FACTOR){
          delay(10);
        }
      }
    }

    processCron();

    hasRecordingsToProcess();
    ESP_LOGI(TAG, "Is time for next transcription attempt: %d. Has Wifi: %d. Has BLE: %d. Has recordings to process: %d", (nextTranscriptionAttempt < time(NULL)), hasWifi, hasBLE, status.filesWaiting);
    // Check for files to transcribe, then transcribe
    if (nextTranscriptionAttempt < time(NULL)) {
      String whisperDomain = systemConfiguration[STR_WHISPER][STR_DOMAIN]| "";
      bool hasWhisperConfig = whisperDomain != "";
      if(hasWifi && hasWhisperConfig){
        transcribeOverWifi();
      }

      hasRecordingsToProcess(); //TODO fix
      if (status.ble && status.filesWaiting){
        uint32_t startTime = millis();
        ESP_LOGI(TAG, "Serving files over BLE");
        bleCompanionServer.hasFilesPending();
        while (bleCompanionServer.serveFiles()) {
          delay(5);
          if (millis() - startTime > 2 * MS_TO_MIN_FACTOR ) {
            ESP_LOGW(TAG, "BLE file serving hit watchdog timeout");
            break;
          }
        }
        delay(1000);
      }

      hasRecordingsToProcess();
      ESP_LOGI(TAG, "Past serving files, has recordings to process still: %d",status.filesWaiting);

      if (status.filesWaiting) {
        // If still has recordings to process, wait for 30 minutes until trying again
        nextTranscriptionAttempt = time(NULL) + 30 * S_TO_MIN_FACTOR;
        //nextTranscriptionAttempt = time(NULL) + 2 * S_TO_MIN_FACTOR;
      }
    }

    ESP_LOGI(TAG, "Has responses waiting: %d", status.responsesWaiting);
    uint32_t responseStartTime = millis();
    while(status.ble && status.responsesWaiting && (millis() - responseStartTime) < (10 * MS_TO_S_FACTOR) ){
      bleCompanionServer.serveFiles();
      delay(10);
    }

    if (status.responsesWaiting) {
      // If still has responses waiting, wake again in 10 minutes
      nextTranscriptionAttempt = time(NULL) + 10 * S_TO_MIN_FACTOR;
    }

    while(status.ble && minimumTimeAvailable > millis()){
      // Make sure we stay online for some time to enable unexpected contact
      delay(10);
    }

    disconnectWifi();
    //disconnectBLE();
    status.processes--;

    ESP_LOGI(TAG, "Current BG processes: %d", status.processes);

    delay(2000);  // Every two seconds to check while things go as planned
  }
}

void beginBLE(){
  if (status.ble){
    return;
  }
  bleCompanionServer.begin(FFat, addNewListItem, false); //TODO load deletion from config.
  status.ble = true;
}

void disconnectBLE(){
  if (!status.ble){
    return;
  }
  btStop();
  status.ble = false;
}

void transcribeOverWifi(){
  File recordingDirectory = FFat.open(DIRNAME_RECORDINGS);
  File file = recordingDirectory.openNextFile();
  if (file) {
    // A file exists in the recording directory, and the nextTranscriptionAttemtTime is lower than now
    status.processes++;  // Adding to status processes keeps device from sleeping
    if (connectWifi()) {
      File whisperCert = FFat.open(FILENAME_WHISPER_CERT);
      while (file) {
        if (file.isDirectory()) {
          file.close();
          file = recordingDirectory.openNextFile();
          continue;
        }

        // Obtain transcription of the audio file
        uint8_t err = 0;
        String caption = whisper.transcribeFile(file, whisperCert, &err);
        String fileName = file.name();
        file.close();

        if (err == 0) {
          // Ready final transcription for insertion into notebook.
          // TODO less fragile solution
          ListItemToAdd newItem;
          newItem.listIndex = String(fileName).toInt();
          newItem.text = caption;
          addNewListItem(newItem, true);
          ESP_LOGD(TAG, "Interpreted filename '%s' as insertion index %d", fileName, newItem.listIndex);
          ESP_LOGI(TAG, "Got caption '%s', adding at list number %d", caption.c_str(), newItem.listIndex);

          FFat.remove(DIRNAME_RECORDINGS "/" + fileName);  // Delete the file after transcription
          // TODO keep for playback?
        }
        file = recordingDirectory.openNextFile();  // Proceed to any additional files
      }
      whisperCert.close();  // Close the certificate file.
    }
    file.close();
    status.processes--;
  }
  recordingDirectory.close();
}

void addNewListItem(ListItemToAdd item, bool defaultBehaviors){
  if (defaultBehaviors){
    item.isSingular = false;
    item.shouldNotify = false;
    item.shouldFollow = false;
    // TODO set by config
  }
  while (itemToAdd.text != "") {
    delay(2);
  }
  itemToAdd = item;
}

void updateBatteryPercentage(){
  // Get battery voltage
  uint32_t batteryVoltage = getBatteryVoltage();
  status.battery = batteryVoltageToPercentage(batteryVoltage);
  ESP_LOGV(TAG, "Battery: %d mV, %d %%", batteryVoltage, status.battery);
}

void getNTPOverWifi(){
  status.processes++;
  if (connectWifi()) {
    // Connecting enables sntp with DHCP
    long sntpStart = millis();
    while (!timeSynced && (sntpStart + 6000) > millis()) {
      // Wait for sync to come in
      delay(50);
    }
  }
  lastNTPTimestmp = time(NULL);  // We have now attempted NTP sync
  ESP_LOGV(TAG, "Last NTP Timestamp at %.24s", ctime(&lastNTPTimestmp));
  status.processes--;
}

void processCron(){
  // Check cron
  time_t currentTime = time(NULL);
  if (HAS_CRON && (nextCronEvent < currentTime) && (currentTime > ANCIENT_TIME)) {
    status.processes++;
    File cronDirectory = FFat.open(DIRNAME_CRON);
    if (!cronDirectory || !cronDirectory.isDirectory()) {
      ESP_LOGE(TAG, "Error scanning %s dir, stopping cron task!", DIRNAME_CRON);
      status.processes--;
      vTaskDelete(NULL);
    }

    ESP_LOGV(TAG, "Processing cron notes at time %ld. Last was at %ld", currentTime, lastCronTimestamp);

    nextCronEvent = 0;
    File cronFile = cronDirectory.openNextFile();
    while (cronFile) {
      if (cronFile.isDirectory()) {
        ESP_LOGI(TAG, "Directory in cron directory. Skipping.");
        cronFile.close();
        cronFile = cronDirectory.openNextFile();
        continue;
      }

      ESP_LOGI(TAG, "Processing cron events from file %s", cronFile.name());

      JsonDocument cron;
      ReadBufferingStream bufferedCronFile(cronFile, 128);
      DeserializationError error = deserializeJson(cron, bufferedCronFile);
      
      cronFile.close();

      if (error) {
        ESP_LOGW(TAG, "Failed to read file, continuing to next.");
        cronFile = cronDirectory.openNextFile();
        continue;
      }

      for (uint i = 0; i < cron.size(); i++) {
        const char *cronTimeString = cron[i]["time"];
        ESP_LOGI(TAG, "Processing cron event %d: '%s'", i, cronTimeString);
        cron_expr cronTime;
        const char *err = NULL;
        cron_parse_expr(cronTimeString, &cronTime, &err);
        if (!err) {
          time_t futureCronEvent = cron_next(&cronTime, currentTime);
          time_t lastCronEvent = cron_prev(&cronTime, currentTime);
          bool singularItem = cron[i][STR_SINGULAR] | false;
          bool aggressiveAdd = cron[i][STR_AGGRESSIVE] | false;
          bool followItem = cron[i][STR_FOLLOW] | false;
          bool notifyItem = cron[i][STR_NOTIFY] | false;
          String message = cron[i][STR_TEXT];
          String page = cron[i][STR_PAGE];
          ESP_LOGV(TAG, "Cron event %d was previously at %ld, next at %ld", i, lastCronEvent, futureCronEvent);
          //ESP_LOGV(TAG, "Cron event %d was previously at %ld, next at %ld Message: '%s' on page '%s'. Singular: %s. Aggressive: %s. Follow: %s. Notify: %s.", i, lastCronEvent, futureCronEvent, message.c_str(), page.c_str(), singularItem ? "true" : "false", aggressiveAdd ? "true" : "false", followItem ? "true" : "false", notifyItem ? "true" : "false");
          if ((lastCronEvent > lastCronTimestamp && lastCronTimestamp > ANCIENT_TIME) || (aggressiveAdd && lastCronTimestamp <= ANCIENT_TIME)) {
            ESP_LOGI(TAG, "Adding note '%s'", message.c_str());
            ListItemToAdd newItem;
            newItem.shouldFollow = followItem;
            newItem.shouldNotify = notifyItem;
            newItem.isSingular = singularItem;
            newItem.listIndex = getListIndex(page);
            newItem.text = message;
            addNewListItem(newItem, false);
          } else {
            ESP_LOGV(TAG, "Skipping cron note '%s', not yet time: %ld <= %ld (or last timestamp ancient)", cronTimeString, lastCronEvent, lastCronTimestamp);
          }
          // Store when a new cron will occur for later wakeup
          if (nextCronEvent == 0) {
            nextCronEvent = futureCronEvent;
          } else {
            nextCronEvent = min(nextCronEvent, futureCronEvent);
          }
        } else {
          ESP_LOGW(TAG, "Cron parse failed: '%s', err: %d", cronTimeString, err);
        }
      }

      cronFile = cronDirectory.openNextFile();
    }
    ESP_LOGV(TAG, "Updating last cron timestamp from %ld to %ld", lastCronTimestamp, currentTime);
    ESP_LOGV(TAG, "Next cron update at %ld", nextCronEvent);
    lastCronTimestamp = currentTime;
    status.processes--;
  }
}

uint8_t getListIndex(String page) {
  JsonDocument lists = systemConfiguration[STR_LISTS];
  for (int i = 0; i < lists.size(); i++) {
    if (lists[i][STR_NAME] == page) {
      return i;
    }
  }
  return 0;
}

void goToSleep() {
  ESP_LOGD(TAG, "Preparing for sleep");

  // Turn off wifi
  disconnectWifi();
  disconnectBLE();

  // Update display to show sleeping
  status.sleeping = true;

  display.redrawDisplay();  // Redraw the entire display, ready for long term sleep

  digitalWrite(PIN_OUT_PERIPH_PWR, HIGH);
  gpio_hold_en((gpio_num_t)PIN_OUT_PERIPH_PWR);
  gpio_deep_sleep_hold_en();
  // TODO turn off later for SD support (deinit first)

  notes.save();                       // Save any changes to the notes
  notesIndex = notes.getNoteIndex();  // Make sure to store in RTC memory the scroll index of the notebook, so we don't loose position on wake

  // Disable internal pulldown for power monitoring pin. Otherwise only possible to read when on, as voltage on power dips to 1.3v
  rtc_gpio_pullup_dis((gpio_num_t)PIN_IN_POWERED);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_IN_POWERED);

  rtc_gpio_pullup_dis((gpio_num_t)PIN_SW_LEFT);
  rtc_gpio_pullup_dis((gpio_num_t)PIN_SW_RIGHT);
  rtc_gpio_pullup_dis((gpio_num_t)PIN_SW_0);
  rtc_gpio_pullup_dis((gpio_num_t)PIN_SW_1);
  rtc_gpio_pullup_dis((gpio_num_t)PIN_SW_2);
  rtc_gpio_pullup_dis((gpio_num_t)PIN_SW_3);
  rtc_gpio_pullup_dis((gpio_num_t)PIN_SW_4);
  rtc_gpio_pullup_dis((gpio_num_t)PIN_SW_5);

  rtc_gpio_pulldown_en((gpio_num_t)PIN_SW_LEFT);
  rtc_gpio_pulldown_en((gpio_num_t)PIN_SW_RIGHT);
  rtc_gpio_pulldown_en((gpio_num_t)PIN_SW_0);
  rtc_gpio_pulldown_en((gpio_num_t)PIN_SW_1);
  rtc_gpio_pulldown_en((gpio_num_t)PIN_SW_2);
  rtc_gpio_pulldown_en((gpio_num_t)PIN_SW_3);
  rtc_gpio_pulldown_en((gpio_num_t)PIN_SW_4);
  rtc_gpio_pulldown_en((gpio_num_t)PIN_SW_5);

  if (status.locked) {
    esp_sleep_enable_ext1_wakeup_io(
      BUTTON_PIN_BITMASK(PIN_SL_1) | BUTTON_PIN_BITMASK(PIN_SL_2) | BUTTON_PIN_BITMASK(PIN_SW_LEFT) | BUTTON_PIN_BITMASK(PIN_SW_RIGHT) | BUTTON_PIN_BITMASK(PIN_SW_0) | BUTTON_PIN_BITMASK(PIN_SW_1) | BUTTON_PIN_BITMASK(PIN_SW_2) | BUTTON_PIN_BITMASK(PIN_SW_3) | BUTTON_PIN_BITMASK(PIN_SW_4) | BUTTON_PIN_BITMASK(PIN_SW_5) | BUTTON_PIN_BITMASK(PIN_IN_POWERED),
      ESP_EXT1_WAKEUP_ANY_HIGH);
  } else {
    esp_sleep_enable_ext1_wakeup_io(
      BUTTON_PIN_BITMASK(PIN_SL_0) | BUTTON_PIN_BITMASK(PIN_SL_2) | BUTTON_PIN_BITMASK(PIN_SW_LEFT) | BUTTON_PIN_BITMASK(PIN_SW_RIGHT) | BUTTON_PIN_BITMASK(PIN_SW_0) | BUTTON_PIN_BITMASK(PIN_SW_1) | BUTTON_PIN_BITMASK(PIN_SW_2) | BUTTON_PIN_BITMASK(PIN_SW_3) | BUTTON_PIN_BITMASK(PIN_SW_4) | BUTTON_PIN_BITMASK(PIN_SW_5) | BUTTON_PIN_BITMASK(PIN_IN_POWERED),
      ESP_EXT1_WAKEUP_ANY_HIGH);
  }

  bool hasUIClock = systemConfiguration[STR_UI][STR_SHOW_CLOCK];
  uint64_t clockUpdateTimer = systemConfiguration[STR_UI][STR_CLOCK_UPDATE_MIN];

  time_t currentTime = time(NULL);

  uint64_t nextWakeup = UINT64_MAX;
  if (nextTranscriptionAttempt > currentTime) {
    // If we hope to transcribe stuff in the future, wake then
    ESP_LOGV(TAG, "Next transcription at %ld, in %ld seconds", nextTranscriptionAttempt, nextTranscriptionAttempt - currentTime);
    nextWakeup = min(nextWakeup, (nextTranscriptionAttempt - currentTime) * uS_TO_S_FACTOR);
  }
  if (hasUIClock && clockUpdateTimer != 0) {
    // Or if the clock updater is enabled, wake then
    ESP_LOGV(TAG, "Next clock update in %ld seconds", clockUpdateTimer * S_TO_MIN_FACTOR);
    nextWakeup = min(nextWakeup, clockUpdateTimer * uS_TO_MIN_FACTOR);
  }
  if (nextCronEvent != 0) {
    // Or if there's a cron event in the future, wake then
    ESP_LOGV(TAG, "Next cron event at %ld, in %ld seconds", nextCronEvent, nextCronEvent - currentTime + 10);
    nextWakeup = min(nextWakeup, (nextCronEvent - currentTime + 10) * uS_TO_S_FACTOR);
  }
  if (nextBLECheckin != 0) {
    // Or if we wanna checkin with BLE later
    ESP_LOGV(TAG, "Next BLE checkin at %ld, in %ld seconds", nextBLECheckin, nextBLECheckin - currentTime + 10);
    nextWakeup = min(nextWakeup, (nextBLECheckin - currentTime + 10) * uS_TO_S_FACTOR);
  }

  // If there are activities happening in the future, wake up then.
  if (nextWakeup != UINT64_MAX) {
    ESP_LOGV(TAG, "Enabling timer wakeup in %ld microseconds", nextWakeup);
    esp_sleep_enable_timer_wakeup(nextWakeup);
  }

  esp_deep_sleep_disable_rom_logging();  // suppress boot messages -> https://esp32.com/viewtopic.php?t=30954

  ESP_LOGI(TAG, "Going to Deep Sleep NOW");
  delay(10);
  esp_deep_sleep_start();
}

const esp_partition_t *partition() {
  // Return the first FAT partition found (should be the only one)
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
}

static int32_t onUSBWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {

  ESP_LOGV(TAG, "Write lba: %ld\toffset: %ld\tbufsize: %ld", lba, offset, bufsize);
  /*for (int x = 0; x < bufsize / secSize; x++) {
    uint8_t blkbuffer[secSize];
    memcpy(blkbuffer, (uint8_t *)buffer + secSize * x, secSize);
    if (!SD_MMC.writeRAW(blkbuffer, lba + x)) {
      return false;
    }
  }
  return bufsize;*/
  _flash.partitionEraseRange(Partition, offset + (lba * BLOCK_SIZE), bufsize);

  // Write data to flash memory in blocks from buffer
  _flash.partitionWrite(Partition, offset + (lba * BLOCK_SIZE), (uint32_t *)buffer, bufsize);

  return bufsize;
}

static int32_t onUSBRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  /*uint32_t secSize = SD_MMC.sectorSize();
  if (!secSize) {
    return false;  // disk error
  }
  ESP_LOGV(TAG, "Read lba: %ld\toffset: %ld\tbufsize: %ld\tsector: %lu", lba, offset, bufsize, secSize);
  for (int x = 0; x < bufsize / secSize; x++) {
    if (!SD_MMC.readRAW((uint8_t *)buffer + (x * secSize), lba + x)) {
      return false;  // outside of volume boundary
    }
  }
  wasUSBPlugged = true;
  return bufsize;*/
  // Read data from flash memory in blocks and store in buffer
  _flash.partitionRead(Partition, offset + (lba * BLOCK_SIZE), (uint32_t *)buffer, bufsize);

  return bufsize;
}

static bool onUSBStartStop(uint8_t power_condition, bool start, bool load_eject) {
  ESP_LOGI(TAG, "Start/Stop power: %u\tstart: %d\teject: %d", power_condition, start, load_eject);
  // If we are stopping and ejecting the drive, sleep the device
  /*if (start == false && load_eject == true) {
    wasUSBEjected = true;
  }*/
  return true;
}

static void usbEventCallback(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == ARDUINO_USB_EVENTS) {
    arduino_usb_event_data_t *data = (arduino_usb_event_data_t *)event_data;
    switch (event_id) {
      case ARDUINO_USB_STARTED_EVENT: ESP_LOGI(TAG, "USB PLUGGED"); break;
      case ARDUINO_USB_STOPPED_EVENT: ESP_LOGI(TAG, "USB UNPLUGGED"); break;
      case ARDUINO_USB_SUSPEND_EVENT: ESP_LOGI(TAG, "USB SUSPENDED: remote_wakeup_en: %u\n", data->suspend.remote_wakeup_en); break;
      case ARDUINO_USB_RESUME_EVENT: ESP_LOGI(TAG, "USB RESUMED"); break;

      default: break;
    }
  }
}

uint8_t batteryVoltageToPercentage(uint32_t voltage) {
  // 1S LiPo voltage/capacity adapted from https://blog.ampow.com/lipo-voltage-chart/
  u_int16_t capacityVoltage[] = {
    3270,
    3610,
    3690,
    3710,
    3730,
    3750,
    3770,
    3790,
    3800,
    3820,
    3840,
    3850,
    3870,
    3910,
    3950,
    3980,
    4020,
    4080,
    4110,
    4150,
    4200
  };

  // If outside the range, return max or min
  if (voltage >= 4200) return 100;
  if (voltage <= 3270) return 0;

  for (u_int8_t i = 1; i <= 20; ++i) {
    if (voltage <= capacityVoltage[i]) {
      // Interpolate battery capacity percentage between steps of the table
      return map(voltage, capacityVoltage[i - 1], capacityVoltage[i], 5 * (i - 1), 5 * i);
    }
  }

  return 0;
}

uint32_t getBatteryVoltage() {
  //analogReadResolution(12);
  uint32_t batteryAccumulator = 0;
  uint8_t batteryReadTimes = 8;  // Average of multiple values to get better precision
  for (uint8_t i = 0; i < batteryReadTimes; i++) {
    batteryAccumulator += analogReadMilliVolts(PIN_BAT_LEVEL);
  }
  // TODO - Calibration of readings to know max and min
  return ((batteryAccumulator * 2) / batteryReadTimes) + 32;  // Multiply by 2 due to voltage divider 1/2
}

void initializeConfiguration() {
  // Only need to initialize once a boot
  static bool isConfigInitialized = false;
  if (isConfigInitialized) {
    return;
  }

  DeserializationError error;
  if (FFat.exists(FILENAME_CONFIG)) {
    // If config exists, load it
    ESP_LOGI(TAG, "Found config file '%s' - reading...", FILENAME_CONFIG);
    File configFile = FFat.open(FILENAME_CONFIG);
    // Buffering stream to improve reading speed https://arduinojson.org/v7/how-to/improve-speed/
    ReadBufferingStream bufferedFile(configFile, 128);
    error = deserializeJson(systemConfiguration, configFile);
    if (error) {
      ESP_LOGE(TAG, "Failed to read file, using default configuration");
    }
  }
  if (!FFat.exists(FILENAME_CONFIG) || error) {  // If no file or failed to read
    ESP_LOGW(TAG, "Found no config file (or error reading), creating default!");

    // Some default config values to start from
    systemConfiguration[STR_LISTS][0][STR_NAME] = "Home";
    systemConfiguration[STR_LISTS][0][STR_INSERT_STRAT] = INS_STRAT_END;
    systemConfiguration[STR_LISTS][0][STR_ORDER_INV] = false;
    systemConfiguration[STR_LISTS][0][STR_SORT] = true;
    systemConfiguration[STR_LISTS][0][STR_FOLLOW_NEW] = true;
    systemConfiguration[STR_LISTS][0][STR_FOLLOW_CHECKED] = true;
    systemConfiguration[STR_LISTS][0][STR_ALLOW_BEYOND] = false;
    systemConfiguration[STR_LISTS][0][STR_DELETION_MIN] = 1440;

    systemConfiguration[STR_LISTS][1][STR_NAME] = "Work";
    systemConfiguration[STR_LISTS][1][STR_INSERT_STRAT] = INS_STRAT_MID;
    systemConfiguration[STR_LISTS][1][STR_ORDER_INV] = false;
    systemConfiguration[STR_LISTS][1][STR_SORT] = true;
    systemConfiguration[STR_LISTS][1][STR_FOLLOW_NEW] = true;
    systemConfiguration[STR_LISTS][1][STR_FOLLOW_CHECKED] = true;
    systemConfiguration[STR_LISTS][1][STR_ALLOW_BEYOND] = false;
    systemConfiguration[STR_LISTS][1][STR_DELETION_MIN] = 10080;
    systemConfiguration[STR_HAS_WIFI] = true;
    systemConfiguration[STR_HAS_BLE] = false;

    systemConfiguration[STR_WIFI][0][STR_SSID] = "";
    systemConfiguration[STR_WIFI][0][STR_PASSWORD] = "";

    systemConfiguration[STR_WHISPER][STR_DOMAIN] = "";
    systemConfiguration[STR_WHISPER][STR_PATH] = "";
    systemConfiguration[STR_WHISPER][STR_MODEL] = "";
    systemConfiguration[STR_WHISPER][STR_LANGUAGE] = "en";
    systemConfiguration[STR_WHISPER][STR_TOKEN] = "";
    systemConfiguration[STR_WHISPER][STR_AUTH_TYPE] = "Bearer";

    systemConfiguration[STR_UI][STR_SHOW_CLOCK] = false;
    systemConfiguration[STR_UI][STR_CLOCK_UPDATE_MIN] = 0;
    systemConfiguration[STR_UI][STR_SHOW_COMPLETION_RATE] = false;
    systemConfiguration[STR_UI][STR_SHOW_MAC] = false;
    systemConfiguration[STR_UI][STR_LED_FEEDBACK] = false;
    systemConfiguration[STR_UI][STR_VIBRATION_FEEDBACK] = false;
    systemConfiguration[STR_UI][STR_DISPLAY_FEEDBACK] = false;
    systemConfiguration[STR_UI][STR_VIBRATION_POSITION] = false;
    systemConfiguration[STR_UI][STR_TEXT_MINIMUM_SIZE] = 1;
    systemConfiguration[STR_UI][STR_TEXT_CONST_SIZE] = false;
    systemConfiguration[STR_UI][STR_NOTE_FULLSCREEN] = true;

    systemConfiguration[STR_TIMEZONE] = "CET-1CEST,M3.5.0,M10.5.0/3";  //https://github.com/esp8266/Arduino/blob/master/cores/esp8266/TZ.h
    systemConfiguration[STR_UPDATE][STR_URL] = "https://github.com/SarahAlroe/FlyingChecklist/releases/latest/download/FlyingChecklist.bin";

    // Save new default config to file through buffer
    File configFile = FFat.open(FILENAME_CONFIG, FILE_WRITE, true);
    WriteBufferingStream bufferedConfigFile(configFile, 128);
    serializeJson(systemConfiguration, bufferedConfigFile);
    bufferedConfigFile.flush();
    configFile.close();
  }

  systemConfig.showClock = systemConfiguration[STR_UI][STR_SHOW_CLOCK] | true;
  systemConfig.clockUpdateRate = systemConfiguration[STR_UI][STR_CLOCK_UPDATE_MIN] | 5;
  systemConfig.showCompletionRate = systemConfiguration[STR_UI][STR_SHOW_COMPLETION_RATE] | true;
  systemConfig.showMAC = systemConfiguration[STR_UI][STR_SHOW_MAC] | true;
  systemConfig.doLEDFeedback = systemConfiguration[STR_UI][STR_LED_FEEDBACK] | false;
  systemConfig.doVibrationFeedback = systemConfiguration[STR_UI][STR_VIBRATION_FEEDBACK] | false;
  systemConfig.doDisplayFeedback = systemConfiguration[STR_UI][STR_DISPLAY_FEEDBACK] | false;
  systemConfig.minTextSize = systemConfiguration[STR_UI][STR_TEXT_MINIMUM_SIZE] | 1;
  systemConfig.constTextSize = systemConfiguration[STR_UI][STR_TEXT_CONST_SIZE] | false;
  systemConfig.doVibrationPosition = systemConfiguration[STR_UI][STR_VIBRATION_POSITION] | false;
  systemConfig.canFullscreenNotes = systemConfiguration[STR_UI][STR_NOTE_FULLSCREEN] | true;
  systemConfig.hasWifi = systemConfiguration[STR_HAS_WIFI] | true;
  systemConfig.hasBLE = systemConfiguration[STR_HAS_BLE] | false;

  // TODO Temporary fix for misspelling in config. Remove in a couple versions.e
  if (systemConfiguration[STR_TEXT_TO_SPEECH] && !systemConfiguration[STR_WHISPER]){
    systemConfiguration[STR_WHISPER] = systemConfiguration[STR_TEXT_TO_SPEECH];
  }

  isConfigInitialized = true;  // Don't initialize again later
}

ListConfig getListConfig(int8_t listIndex) {
  // Convert from JSON to struct
  ListConfig config;
  JsonDocument jsonConfig = systemConfiguration[STR_LISTS][listIndex];
  config.insertionStrategy = (InsertionStrategy)jsonConfig[STR_INSERT_STRAT];
  config.noteOrderInverted = jsonConfig[STR_ORDER_INV] | false;
  config.sortChecked = jsonConfig[STR_SORT] | true;
  config.followNew = jsonConfig[STR_FOLLOW_NEW] | true;
  config.followChecked = jsonConfig[STR_FOLLOW_CHECKED] | true;
  config.deletionMinutes = jsonConfig[STR_DELETION_MIN] | 1440;  // Default a day
  config.allowBeyondEdges = jsonConfig[STR_ALLOW_BEYOND] | false;
  return config;
}

void setCPUFreq(uint16_t frequency) {
  if (DO_CPU_SCALING) {
    static uint16_t currentFrequency;
    if (/*status.wifi && */ frequency < 80) {
      // Crashes if under 80
      frequency = 80;
    }
    if (frequency != currentFrequency) {
      setCpuFrequencyMhz(frequency);
      ESP_LOGD(TAG, "Scaled CPU! Was %dMHz, is %dMHz (xtal: %dMHZ) (APB: %dHz)", currentFrequency, getCpuFrequencyMhz(), getXtalFrequencyMhz(), getApbFrequency());
      currentFrequency = frequency;
    }
  }
}

void timeavailable(struct timeval *t) {
  timeSynced = true;
  ESP_LOGI(TAG, "Got time adjustment from NTP!");
  printLocalTime();
}

uint8_t getWakeupPin() {
  uint64_t wakeupStatus = esp_sleep_get_ext1_wakeup_status();
  return log(wakeupStatus) / log(2);
}

// Borrowed from SD_Update exampple
// perform the actual update from a given stream
void performUpdate(Stream &updateSource, size_t updateSize) {
  if (Update.begin(updateSize)) {
    size_t written = Update.writeStream(updateSource);
    if (written == updateSize) {
      ESP_LOGI(TAG, "Written: %d successfully", written);
    } else {
      ESP_LOGE(TAG, "Written only : %d / %d. Retry?", written, updateSize);
    }
    if (Update.end()) {
      ESP_LOGI(TAG, "OTA done!");
      if (Update.isFinished()) {
        ESP_LOGI(TAG, "Update successfully completed. Rebooting.");
      } else {
        ESP_LOGE(TAG, "Update not finished? Something went wrong!");
      }
    } else {
      ESP_LOGE(TAG, "Error Occurred. Error #: %d", Update.getError());
    }
  } else {
    ESP_LOGE(TAG, "Not enough space to begin OTA");
  }
}

// check given FS for valid update.bin and perform update if available
bool updateFromFS() {
  File updateBin = FFat.open("/update.bin");
  if (updateBin) {
    if (updateBin.isDirectory()) {
      ESP_LOGE(TAG, "Error, update.bin is not a file");
      updateBin.close();
      return false;
    }

    size_t updateSize = updateBin.size();

    if (updateSize > 0) {
      ESP_LOGI(TAG, "Try to start update");
      performUpdate(updateBin, updateSize);
      updateBin.close();
    } else {
      ESP_LOGE(TAG, "Error, file is empty");
      updateBin.close();
      return false;
    }

    // when finished remove the binary from sd card to indicate end of the process
    FFat.remove("/update.bin");
  } else {
    ESP_LOGE(TAG, "Could not load update.bin from sd root");
    return false;
  }
  return true;
}

void printLocalTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    ESP_LOGE(TAG, "Failed to obtain time");
    return;
  }
  ESP_LOGI(TAG, "Device time is: %d-%d-%d %d:%d:%d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

void printWakeupReason(esp_sleep_wakeup_cause_t wakeup_reason) {
  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0: ESP_LOGI(TAG, "Wakeup caused by external signal using RTC_IO"); break;
    case ESP_SLEEP_WAKEUP_EXT1: ESP_LOGI(TAG, "Wakeup caused by external signal using RTC_CNTL"); break;
    case ESP_SLEEP_WAKEUP_TIMER: ESP_LOGI(TAG, "Wakeup caused by timer"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: ESP_LOGI(TAG, "Wakeup caused by touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP: ESP_LOGI(TAG, "Wakeup caused by ULP program"); break;
    default: ESP_LOGI(TAG, "Wakeup was not caused by deep sleep: %d\n", wakeup_reason); break;
  }
}

void printResetReason(esp_reset_reason_t reset_reason) {
  switch (reset_reason) {
    case 1: ESP_LOGI(TAG, "Vbat power on reset"); break;
    case 3: ESP_LOGI(TAG, "Software reset digital core"); break;
    case 4: ESP_LOGI(TAG, "Legacy watch dog reset digital core"); break;
    case 5: ESP_LOGI(TAG, "Deep Sleep reset digital core"); break;
    case 6: ESP_LOGI(TAG, "Reset by SLC module, reset digital core"); break;
    case 7: ESP_LOGI(TAG, "Timer Group0 Watch dog reset digital core"); break;
    case 8: ESP_LOGI(TAG, "Timer Group1 Watch dog reset digital core"); break;
    case 9: ESP_LOGI(TAG, "RTC Watch dog Reset digital core"); break;
    case 10: ESP_LOGI(TAG, "Instrusion tested to reset CPU"); break;
    case 11: ESP_LOGI(TAG, "Time Group reset CPU"); break;
    case 12: ESP_LOGI(TAG, "Software reset CPU"); break;
    case 13: ESP_LOGI(TAG, "RTC Watch dog Reset CPU"); break;
    case 14: ESP_LOGI(TAG, "for APP CPU, reseted by PRO CPU"); break;
    case 15: ESP_LOGI(TAG, "Reset when the vdd voltage is not stable"); break;
    case 16: ESP_LOGI(TAG, "RTC Watch dog reset digital core and rtc module"); break;
    default: ESP_LOGI(TAG, "NO_MEAN");
  }
}

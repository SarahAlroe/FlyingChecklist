#pragma once

/*Macros and consts*/
#define BUTTON_PIN_BITMASK(GPIO) (1ULL << GPIO)  // for deep sleep wakeup config
#define uS_TO_S_FACTOR 1000000ULL
#define S_TO_MIN_FACTOR 60ULL
#define S_TO_H_FACTOR 3600ULL
#define MS_TO_S_FACTOR 1000ULL
#define MS_TO_MIN_FACTOR 60000ULL
#define uS_TO_MIN_FACTOR 60000000ULL
#define ULP_START_OFFSET 32  // Offset of ulp program in memory (randomly selected)
#define ULP_ADDR_LED_FB 4
const uint32_t sleepIncrement = 5000; // Time to wait after activity before sleeping device
const time_t ANCIENT_TIME = 1771512100;

/*Strings for JSON lookups*/
#define STR_LISTS "lists"
#define STR_NAME "name"
#define STR_INSERT_STRAT "ins_strat"
#define STR_ORDER_INV "order_inv"
#define STR_SORT "sort"
#define STR_FOLLOW_NEW "flw_new"
#define STR_FOLLOW_CHECKED "flw_chck"
#define STR_DELETION_MIN "del_mins"
#define STR_ALLOW_BEYOND "beyond"

#define STR_HAS_WIFI "has_wifi"
#define STR_HAS_BLE "has_ble"
#define STR_WIFI "wifi"
#define STR_SSID "ssid"
#define STR_PASSWORD "pass"

#define STR_WHISPER "stt"
#define STR_TEXT_TO_SPEECH "tts"
#define STR_DOMAIN "domain"
#define STR_PATH "path"
#define STR_MODEL "model"
#define STR_LANGUAGE "lang"
#define STR_TOKEN "token"
#define STR_AUTH_TYPE "auth"

#define STR_UI "ui"
#define STR_SHOW_CLOCK "clk"
#define STR_CLOCK_UPDATE_MIN "clk_upd"
#define STR_SHOW_COMPLETION_RATE "done"
#define STR_SHOW_MAC "mac"
#define STR_DISPLAY_FEEDBACK "fb_disp"
#define STR_VIBRATION_FEEDBACK "fb_vib"
#define STR_LED_FEEDBACK "fb_led"
#define STR_VIBRATION_POSITION "pos_vib"
#define STR_TEXT_MINIMUM_SIZE "txt_min"
#define STR_TEXT_CONST_SIZE "txt_const"
#define STR_NOTE_FULLSCREEN "fullscr"

#define STR_TIMEZONE "tz"

#define FILENAME_CONFIG "/config.json"  // Location of config file
#define FILENAME_WHISPER_CERT "/whisper.pem" // Location of the certificate for the whisper server
#define FILENAME_UPDATE_CERT "/update.pem" // Certificate for the update server
#define FILENAME_TMP_PREFIX "/tmp"
#define FILE_WAV_EXTENSION ".wav"
#define DIRNAME_RECORDINGS  "/rec"
#define DIRNAME_NOTES "/notes"
#define DIRNAME_CRON "/cron"

#define STR_CRON "cron"
#define STR_TEXT "txt"
#define STR_TIME "time"
#define STR_PAGE "page"
#define STR_SINGULAR "one"
#define STR_AGGRESSIVE "aggro"
#define STR_FOLLOW "follow"
#define STR_NOTIFY "notify"
#define STR_DONE "done"
#define STR_CHANGED "chng"

#define STR_UPDATE "update"
#define STR_URL "url"

/*Hardware pinout*/
#define PIN_BAT_LEVEL 1
#define PIN_IN_POWERED 21

#define PIN_OUT_FB 3
#define PIN_OUT_LED GPIO_NUM_14
#define PIN_OUT_PERIPH_PWR 48

#define PIN_SW_LEFT 2
#define PIN_SW_RIGHT 4
#define PIN_SW_0 6
#define PIN_SW_1 15
#define PIN_SW_2 17
#define PIN_SW_3 10
#define PIN_SW_5 8
#define PIN_SW_4 9
#define PIN_SL_0 16
#define PIN_SL_1 7
#define PIN_SL_2 5

#define PIN_ENC_A 17
#define PIN_ENC_B 6    // Overloaded for device variations
#define PIN_ENC_SW 15  // Overloaded for device variations

#define PIN_SPI_PICO 38
#define PIN_SPI_CLK 39
#define PIN_SPI_POCI 40

#define PIN_SPI_CS_SD 44
#define PIN_SPI_CS_SHARP 47
#define PIN_SHARP_EXTCOM GPIO_NUM_18

#define PIN_DAC_DIN 11
#define PIN_DAC_BCLK 12
#define PIN_DAC_LRCLK 13

#define PIN_MIC_DATA GPIO_NUM_41
#define PIN_MIC_CLK GPIO_NUM_42

enum InsertionStrategy { INS_STRAT_END,
                         INS_STRAT_MID,
                         INS_STRAT_CONTEXT };

struct ListConfig {
  InsertionStrategy insertionStrategy;
  bool noteOrderInverted;
  bool sortChecked;
  bool followNew;
  bool followChecked;
  int16_t deletionMinutes;
  bool allowBeyondEdges;
};

struct SystemStatus {
  bool wifi;
  bool locked;
  bool sleeping;
  bool recording;
  uint8_t processes;
  bool charging;
  uint8_t battery;
  bool ble;
  uint8_t filesWaiting;
  uint8_t responsesWaiting;
  bool hasNews;

  bool operator==(const SystemStatus& other) const {
    return wifi == other.wifi
           && locked == other.locked
           && sleeping == other.sleeping
           && recording == other.recording
           && processes == other.processes
           && charging == other.charging
           && battery == other.battery
           && ble == other.ble
           && filesWaiting == other.filesWaiting
           && responsesWaiting == other.responsesWaiting
           && hasNews == other.hasNews;
  }
};

struct SystemConfig {
  bool showClock;
  uint16_t clockUpdateRate;
  bool showCompletionRate;
  bool showMAC;
  bool doDisplayFeedback;
  bool doVibrationFeedback;
  bool doLEDFeedback;
  bool doVibrationPosition;
  uint8_t minTextSize;
  bool constTextSize;
  bool canFullscreenNotes;
  bool hasWifi;
  bool hasBLE;
};

struct ListItemToAdd{
  uint8_t listIndex;
  bool isSingular;
  bool shouldNotify;
  bool shouldFollow;
  String text;
};

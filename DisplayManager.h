#pragma once
#include "Arduino.h"
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SharpMem.h>
#include "NotesManager.h"
#include "config.h"

#define BLACK 0
#define WHITE 1

#define ICON_USB 0
#define ICON_MIC 1
#define ICON_SAVE 2
#define ICON_LOCK 3 //TODO enum
#define ICON_UNLOCK 4

enum horizontal_alignment_t { ALIGN_LEFT, ALIGN_MID, ALIGN_RIGHT };

class NotesManager;

class DisplayManager
{
public:
    DisplayManager(Adafruit_SharpMem &disp, GFXcanvas1 &screenE, GFXcanvas1 &noteFi, GFXcanvas1 &offScr, SystemStatus &status, SystemConfig &systemConfig)
        : display(disp), screenElement(screenE), noteField(noteFi), offScreen(offScr), status(status), systemConfig(systemConfig)
    {
    }
    void init(NotesManager *notesManager);
    void refresh();
    void revealDisplay();
    void redrawDisplay(bool shouldRefresh = true);
    void updateHeader(){
        drawHeader();
        display.refresh();
    }
    void slideInNote(int16_t noteIndex);
    void slideOutNote(int16_t noteIndex);
    void slideOutInNotebook(bool rightwards);
    void animCrossNote(int16_t noteScreenIndex, float animProgress);
    void openSpaceAt(int16_t noteIndex);
    void closeSpaceAt(int16_t noteIndex);
    int16_t scrollToNoteIndex(int16_t toIndex, int8_t allowedPastBorders = 0, int16_t freezeIndex = -1);
    bool indexIsOnScreen(int16_t noteIndex);
    void clearToLargeIcon(uint8_t icon, bool animate = true, unsigned long animLength = 2*SLIDE_TIME);
    void redrawFromLargeIcon(uint8_t icon, unsigned long animLength = 2*SLIDE_TIME);
    void delayDots();
    void drawLargeTimer(uint16_t seconds, uint16_t outOfSeconds);
    void animFullscreenNoteIn(int16_t noteScreenIndex);
    void animFullscreenNoteOut();
    bool isFullscreenNote();
    void animLock();
    void animUnlock();
    void flashLock();

private:
    const char* TAG = "DisplayManager";
    static const unsigned long SCROLL_TIME_BASE = 200;
    static const unsigned long SLIDE_TIME = 200;
    static const unsigned long SCREEN_SWITCH_TIME = 450;

    static const uint16_t SCREEN_WIDTH = 240;
    static const uint16_t SCREEN_HEIGHT = 400;
    static const uint16_t NOTE_HEIGHT = 7 * 8;
    static const uint16_t HEADER_HEIGHT = 8 * 8;
    static const uint16_t HEADER_MARGIN = 4;
    static const uint16_t HEADER_ICON_SIZE = 16;
    static const uint16_t HEADER_ICON_SPACING = HEADER_ICON_SIZE + HEADER_MARGIN;

    static const uint8_t BASE_CHARACTER_WIDTH = 6;
    static const uint8_t BASE_CHARACTER_HEIGHT = 8;

    Adafruit_SharpMem &display;
    GFXcanvas1 &screenElement;
    GFXcanvas1 &noteField;
    GFXcanvas1 &offScreen;
    //GFXcanvas1 screenElement(NOTE_HEIGHT, SCREEN_WIDTH);
    //GFXcanvas1 noteField(SCREEN_WIDTH - 22, NOTE_HEIGHT - 1);
    //GFXcanvas1 screenElement = GFXcanvas1(7*8, 400);
    //GFXcanvas1 noteField = GFXcanvas1(400 - 22, 7*8 - 1);
    SystemStatus &status;
    SystemConfig &systemConfig;

    NotesManager *nm;

    void drawBaseLayout();
    void drawHeader();
    void drawHeaderIcon(const unsigned char * icon, horizontal_alignment_t alignment, uint8_t index);
    void drawNoteWidget(int16_t noteNumber, int16_t screenPosition = -1);
    void prepareNoteField(int16_t noteIndex, bool crossIfChecked = true);
    void prepareScreenElement(int16_t noteIndex);
    void drawLargeIcon(uint8_t icon);
    void drawFullscreenNote();
    uint8_t getNoteTextSizeFromLength(uint32_t length);
    uint8_t getNoteLineThicknessFromLength(uint32_t length);
    uint8_t getFullscreenTextSizeFromLength(uint32_t length);
    uint16_t getMaxTextLengthFromSize(uint8_t textSize);
    static String secondsMinutesString(uint16_t seconds){
      uint8_t minutes = seconds / 60;
      seconds = seconds % 60;
      String secondsString = String(seconds);
      String minutesString = String(minutes);
      if (secondsString.length() < 2){
        secondsString = "0"+secondsString;
      }
      if (minutesString.length() < 2){
        minutesString = "0"+minutesString;
      }
      return minutesString + ":" + secondsString;
    }
    static unsigned char reverseByte(unsigned char b)
    {
        b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
        b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
        b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
        return b;
    };
    static float easeIn(float x)
    {
        return pow(x, 3);
    };
    static float easeOut(float x)
    {
        return 1 - pow(1 - x, 3);
    };
    static float easeInOut(float x)
    {
        return x < 0.5 ? 4 * x * x * x : 1 - pow(-2 * x + 2, 3) / 2;
    };
};

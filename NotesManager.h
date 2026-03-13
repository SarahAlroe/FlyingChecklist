#pragma once
#include "Arduino.h"
#include <ArduinoJson.h>
#include <StreamUtils.h>
#include "DisplayManager.h"
#include "Arduino.h"
#include "FS.h"
#include "config.h"

class DisplayManager;

class NotesManager {
public:
  NotesManager(fs::FS& fs, DisplayManager& dispm)
    : display(dispm), fs(fs) {
    screenActivityMutex = xSemaphoreCreateMutex();
  }
  void init(String noteName, ListConfig newConfig, int16_t noteIndex);
  int16_t getNoteIndex() {
    return currentNoteIndex;
  }
  int16_t getNoteIndexAtScreenPosition(int16_t position){
    return getNoteIndex() + position;
  }
  int16_t getNoteCount() {
    return (int)notes.size();
  }
  int16_t getNoteCheckedCount() {
    int16_t checkedNotes = 0;
    for (int i = 0; i<notes.size(); i++){
      if (notes[i][STR_DONE]){
        checkedNotes++;
      }
    } //TODO: Cache
    return checkedNotes;
  }
  bool getNoteChecked(uint16_t noteNumber) {
    return notes[noteNumber][STR_DONE];
  }
  String getNoteText(uint16_t noteNumber) {
    return notes[noteNumber]["txt"].as<String>();
  }
  bool canScrollUp() {
    return currentNoteIndex > 0;
  }
  bool canScrollDown() {
    return currentNoteIndex + 6 < (int)notes.size();
  }
  String getNotebookName() {
    return currentNotebook;
  }
  String getNotebookNameCP437() {
    return replaceCommonUnicode(currentNotebook);
  }

  void scrollToIndex(int16_t index, int8_t allowedPastBorders = 0);
  void scrollUp();
  void scrollDown();
  bool hasNoteAtDot(uint8_t screenIndex);
  void addNewNote(String text, bool isSingular = false);              // Causes change
  void crossNoteAtDot(uint8_t screenIndex);  // Causes change
  void cleanupNotes();                       // Potentially causes change
  void save();
private:
  const char* TAG = "NotesManager";
  JsonDocument notes;
  String currentNotebook;
  int16_t currentNoteIndex = 0;
  fs::FS& fs;
  ListConfig config;  // TODO other ref?
  DisplayManager& display;
  JsonDocument createNote(String text) {
    JsonDocument note;
    note[STR_TEXT] = text;
    note[STR_DONE] = false;
    note[STR_CHANGED] = 0;
    return note;
  }
  bool hasNotebookChanged = false;
  SemaphoreHandle_t screenActivityMutex = NULL;
  void load(String noteName);
  void addNewNoteAtIndex(uint16_t noteIndex, String text);
  void addNewNoteAtBeginning(String text);
  void addNewNoteAtEnd(String text);
  void moveInNotes(uint16_t fromIndex, uint16_t toIndex);
  void insertIntoNotesAt(uint16_t noteIndex, JsonDocument note);
  int16_t getFirstUncheckedIndex();
  int16_t getInsertionScreenPosition();
  int16_t getIndexOfNote(String text);
  void sortNotes();
  void removeOldNotes();
  String replaceCommonUnicode(String text);
};

#include "NotesManager.h"
#include "DisplayManager.h"

#include <Regexp.h>       // https://github.com/nickgammon/Regexp

RTC_DATA_ATTR bool isNotebookKnownSorted = false;  // This must exist outside of class to be in RTC memory

void NotesManager::init(String notebook, ListConfig newConfig, int16_t noteIndex) {
  config = newConfig;
  ESP_LOGI(TAG, "Initializing notebook '%s' at index %d", notebook.c_str(), noteIndex);
  load(notebook);
  if (noteIndex >= 0) {
    currentNoteIndex = noteIndex;
  } else {
    isNotebookKnownSorted = false;  // If noteindex is reset, we're loading a new notebook with unknown sorted status
    currentNoteIndex = getInsertionScreenPosition();
  }
}

void NotesManager::scrollToIndex(int16_t index, int8_t allowedPastBorders) {
  if (xSemaphoreTake(screenActivityMutex, portMAX_DELAY) == pdTRUE) {
    currentNoteIndex = display.scrollToNoteIndex(index, allowedPastBorders);
    xSemaphoreGive(screenActivityMutex);
  }
}

void NotesManager::scrollUp() {
  scrollToIndex(currentNoteIndex - 1);
}

void NotesManager::scrollDown() {
  scrollToIndex(currentNoteIndex + 1);
}

String NotesManager::replaceCommonUnicode(String text) {
  // Regex ÆØÅ replacement with printable chars
  char captionBuffer[text.length() + 100];  // Make bufffer larger than actual caption to allow for larger substitutions
  text.toCharArray(captionBuffer, text.length() + 100);
  MatchState ms(captionBuffer);  // Set up regex
  char charReplacement[] = "a";  // Placeholder char array of single character length
  // Replace chars with appropriate CP437 chars https://learn.adafruit.com/adafruit-gfx-graphics-library/graphics-primitives#extended-characters-cp437-and-a-lurking-bug-3100368
  charReplacement[0] = 0x91;
  ms.GlobalReplace("æ", charReplacement);
  charReplacement[0] = 0x92;
  ms.GlobalReplace("Æ", charReplacement);
  charReplacement[0] = 0xEC;  // No good candidates
  ms.GlobalReplace("ø", charReplacement);
  charReplacement[0] = 0xED;
  ms.GlobalReplace("Ø", charReplacement);
  charReplacement[0] = 0x86;
  ms.GlobalReplace("å", charReplacement);
  charReplacement[0] = 0x8F;
  ms.GlobalReplace("Å", charReplacement);
  ESP_LOGV(TAG, "Revised text '%s' as '%s'.", captionBuffer, text.c_str());
  return String(captionBuffer);
}

void NotesManager::addNewNote(String text, bool isSingular) {
  hasNotebookChanged = true;  // Adding a note must be saved afterwards

  text = replaceCommonUnicode(text);

  if (isSingular){
    int16_t existingNoteIndex = getIndexOfNote(text);
    if (existingNoteIndex >= 0){
      notes[existingNoteIndex][STR_DONE] = false;
      isNotebookKnownSorted = false;
      return;
    }
  }

  // Note: Notebook can never become unsorted from the addition of a new note following the same sorting rules
  ESP_LOGD(TAG, "Adding new note, determining strategy.");
  if (config.insertionStrategy == INS_STRAT_END) {
    if (config.noteOrderInverted) {
      addNewNoteAtBeginning(text);
    } else {
      addNewNoteAtEnd(text);
    }
  } else if (config.insertionStrategy == INS_STRAT_MID) {
    if (config.noteOrderInverted) {
      int16_t firstUnchecked = getFirstUncheckedIndex();
      if (firstUnchecked == notes.size() - 1) {
        addNewNoteAtEnd(text);  // If there are no checked, just insert at end,
        return;
      }
      if (config.followNew) {               // Only scroll if out of screen and config enables follow.
        scrollToIndex(firstUnchecked - 4);  //scroll to first free from bottom - 4 (to leave 2 turning to 1 free) (min 0)
      }
      addNewNoteAtIndex(firstUnchecked + 1, text);
    } else {
      int16_t firstUnchecked = getFirstUncheckedIndex();
      if (firstUnchecked == 0) {  // No checked, just insert at beginning
        addNewNoteAtBeginning(text);
        return;
      }
      if (config.followNew) {                  // Only scroll if out of screen and config enables follow.
        scrollToIndex(firstUnchecked - 1, 1);  // scroll to first free index - 1 (to show 1 checked), allow to go past borders for insertion
      }
      addNewNoteAtIndex(firstUnchecked, text);
    }
  }
}

void NotesManager::addNewNoteAtEnd(String text) {
  ESP_LOGD(TAG, "Adding note '%s' at end of notes array.", text);
  if (xSemaphoreTake(screenActivityMutex, portMAX_DELAY) == pdTRUE) {
    if (config.followNew && notes.size() > 5) {                           // Only scroll if config enables follow.
      currentNoteIndex = display.scrollToNoteIndex(notes.size() - 5, 1);  // Size is index + 1, scroll here
    }
    notes.add(createNote(text));
    display.slideInNote((int)notes.size() - 1);

    xSemaphoreGive(screenActivityMutex);
  }
}

void NotesManager::addNewNoteAtBeginning(String text) {
  //TODO semaphore
  ESP_LOGD(TAG, "Adding note '%s' at beginning of notes array.", text);
  if (config.followNew) {  // Only scroll if config enables follow.
    scrollToIndex(0);
  }
  if (notes.size() == 0) {
    notes.add(createNote(text));
    if (xSemaphoreTake(screenActivityMutex, portMAX_DELAY) == pdTRUE) {
      display.slideInNote(0);
      xSemaphoreGive(screenActivityMutex);
    }
  } else {
    addNewNoteAtIndex(0, text);
  }
}


void NotesManager::addNewNoteAtIndex(uint16_t noteIndex, String text) {
  ESP_LOGD(TAG, "Adding new note '%s' at index %d.", text.c_str(), noteIndex);
  if (xSemaphoreTake(screenActivityMutex, portMAX_DELAY) == pdTRUE) {
    display.openSpaceAt(noteIndex);                  // Animate space opening
    insertIntoNotesAt(noteIndex, createNote(text));  // Insert into datastructure
    display.slideInNote(noteIndex);                  // Animate new data in

    xSemaphoreGive(screenActivityMutex);
  } else {
    insertIntoNotesAt(noteIndex, createNote(text));  // Insert into datastructure without manipulating screen
  }
}

void NotesManager::insertIntoNotesAt(uint16_t noteIndex, JsonDocument note) {
  notes.add(notes[notes.size() - 1]);
  for (int i = notes.size() - 2; i >= noteIndex; i--) {
    notes[i + 1].set(notes[i]);  // All the way down to (including) noteindex, set the position after to current.
  }
  notes[noteIndex].set(note);
  ESP_LOGD(TAG, "Inserted into notes at index %d.", noteIndex);
}

void NotesManager::moveInNotes(uint16_t fromIndex, uint16_t toIndex) {
  if (fromIndex == toIndex) {
    return;
  }
  JsonDocument noteToMove = notes[fromIndex];
  if (fromIndex < toIndex) {  // Are we moving up or down?
    for (int i = fromIndex + 1; i >= toIndex; i++) {
      notes[i - 1].set(notes[i]);  // All the way up to (including) toIndex, set the position before to current.
    }
  } else {
    for (int i = fromIndex - 1; i >= toIndex; i--) {
      notes[i + 1].set(notes[i]);  // All the way down to (including) toIndex, set the position after to current.
    }
  }
  notes[toIndex].set(noteToMove);
  ESP_LOGD(TAG, "Shifted note from index %d to index %d.", fromIndex, toIndex);
}

void NotesManager::cleanupNotes(){
  sortNotes();
  removeOldNotes();
}

void NotesManager::removeOldNotes() {
  if (xSemaphoreTake(screenActivityMutex, portMAX_DELAY) == pdTRUE) {
    //Delete timeouted checked notes
    if (config.deletionMinutes >= 0) {  // Only delete notes if deletionMinutes config is a positive number.
      bool notesVisiblyDeleted = false;
      for (int i = 0; i < notes.size(); i++) {
        if (notes[i][STR_DONE]) {  // Only deal with checked notes
          ESP_LOGD(TAG, "Processing note %d", i);
          if (notes[i][STR_CHANGED] < ANCIENT_TIME) {  // old timestamp from desync - update
            hasNotebookChanged = true;
            ESP_LOGI(TAG, "Note %d had old timestamp %lu, now %lu", i, notes[i][STR_CHANGED], time(NULL));
            notes[i][STR_CHANGED] = time(NULL);
          } else if (time(NULL) > (notes[i][STR_CHANGED].as<unsigned long>() + (config.deletionMinutes * 60))) {  // Has been checked for deletionMinutes time, remove.
            hasNotebookChanged = true;
            notesVisiblyDeleted = true;
            ESP_LOGI(TAG, "Note %d is out of date, removing.", i);
            if (i >= currentNoteIndex) {  // if above screen index, screen anim
              display.slideOutNote(i);
              display.closeSpaceAt(i);
            } else {  // else subtract index 1
              currentNoteIndex--;
            }
            notes.remove(i);
            i--;  // We just removed the item at i - we should not skip i+1 which is now at i.
          }
        }
      }
      if (notesVisiblyDeleted){
        display.redrawDisplay(); // Redraw everything at the end for good measure if anything changed
      }
    }
    xSemaphoreGive(screenActivityMutex);
  }
}

void NotesManager::sortNotes() {
  if (!isNotebookKnownSorted) {
    if (xSemaphoreTake(screenActivityMutex, portMAX_DELAY) == pdTRUE) {
      //Rearrange checked notes
      for (int i = getFirstUncheckedIndex(); i < notes.size() && i >= 0; (config.noteOrderInverted) ? i-- : i++) {
        if (notes[i][STR_DONE]) {       // If a note exists after the first unchecked note, the list is unsorted, and this note should be moved.
          hasNotebookChanged = true;  // Moving things around should trigger a save
          int16_t newTargetIndex = getFirstUncheckedIndex(); // >1 iterations through, the first unchecked index will have moved.
          ESP_LOGD(TAG, "Sorting note moving from index %d to %d.", i, newTargetIndex);
          display.slideOutNote(i);
          display.closeSpaceAt(i);
          if (config.followChecked && (newTargetIndex < currentNoteIndex)) {
            if (newTargetIndex < i) {
              ESP_LOGD(TAG, "Target earlier in list, scrolling to %d.", newTargetIndex);
              currentNoteIndex = display.scrollToNoteIndex(newTargetIndex, 0);
            } else {
              ESP_LOGD(TAG, "Target later in list, scrolling to %d.", i, newTargetIndex - 6);
              currentNoteIndex = display.scrollToNoteIndex(newTargetIndex - 6, 0);
            }
          }
          display.openSpaceAt(newTargetIndex);
          moveInNotes(i, newTargetIndex);
          display.slideInNote(newTargetIndex);
        }
      }
      if (hasNotebookChanged){
        display.redrawDisplay(); // Redraw the display at the end if things have changed
      }
      xSemaphoreGive(screenActivityMutex);
    }
    isNotebookKnownSorted = true;
    scrollToIndex(getInsertionScreenPosition());
  }else{
    ESP_LOGD(TAG, "Notes already known sorted, skipping sort.");
  }
}

bool NotesManager::hasNoteAtDot(uint8_t screenIndex) {
  int16_t noteToCross = currentNoteIndex + screenIndex;
  return noteToCross < notes.size();
}

void NotesManager::crossNoteAtDot(uint8_t screenIndex) {
  int16_t noteToCross = currentNoteIndex + screenIndex;
  if (noteToCross < notes.size()) { // Only if the note actually exists
    notes[noteToCross][STR_DONE] = !notes[noteToCross][STR_DONE];
    notes[noteToCross][STR_CHANGED] = time(NULL);  // Was changed at now
    if (xSemaphoreTake(screenActivityMutex, portMAX_DELAY) == pdTRUE) {
      display.redrawDisplay();
      xSemaphoreGive(screenActivityMutex);
    }
    ESP_LOGI(TAG, "Crossed note at index %d.", noteToCross);
    hasNotebookChanged = true;      // Crossing a note changes the state of the notebook
    isNotebookKnownSorted = false;  // And may require sorting
  }
}

void NotesManager::load(String noteName) {
  if (currentNotebook != "") {
    save();
  }
  File noteFile = fs.open(DIRNAME_NOTES "/" + noteName + ".json", FILE_READ, false);
  ReadBufferingStream bufferedNoteFile(noteFile, 128);
  DeserializationError error = deserializeJson(notes, bufferedNoteFile);
  if (error) {
    ESP_LOGE(TAG, "Failed to read notes file. Using empty document");
    JsonDocument doc;
    notes = doc.to<JsonArray>();
  } else {
    ESP_LOGD(TAG, "Loaded noteblock %s", noteName.c_str());
  }
  noteFile.close();
  currentNotebook = noteName;
}

void NotesManager::save() {
  if (hasNotebookChanged) {  // Only save is the file has actually changed
    File noteFile = fs.open(DIRNAME_NOTES "/" + currentNotebook + ".json", FILE_WRITE, true);
    WriteBufferingStream bufferedNoteFile(noteFile, 128);
    serializeJson(notes, bufferedNoteFile);
    bufferedNoteFile.flush();
    noteFile.close();
    ESP_LOGD(TAG, "Saved noteblock %s", currentNotebook.c_str());
    hasNotebookChanged = false; // Saved, no reason to do it again without new changes
  }else{
    ESP_LOGD(TAG, "Notes have not changed since last save, skipping save.");
  }
}

int16_t NotesManager::getFirstUncheckedIndex() {
  if (config.noteOrderInverted) {
    for (int16_t i = notes.size() - 1; i >= 0; i--) {
      if (!notes[i][STR_DONE]) {
        ESP_LOGI(TAG, "Found first unchecked index at %d, inverted order.", i);
        return i;
      }
    }
    ESP_LOGI(TAG, "Found no unchecked notes, inverted order.");
    return -1;
  } else {
    for (int16_t i = 0; i < notes.size(); i++) {
      if (!notes[i][STR_DONE]) {
        ESP_LOGI(TAG, "Found first unchecked index at %d, normal order.", i);
        return i;
      }
    }
    ESP_LOGI(TAG, "Found no unchecked notes, normal order.");
    return notes.size();
  }
}

int16_t NotesManager::getInsertionScreenPosition() {
  int16_t targetPos;
  int16_t notesSize = notes.size();
  if (config.insertionStrategy == INS_STRAT_END) {
    if (config.noteOrderInverted) {
      targetPos = 0;
    } else {
      targetPos = notesSize;
    }
    ESP_LOGI(TAG, "Insertion at end, hence screen position %d", targetPos);
  } else if (config.insertionStrategy == INS_STRAT_MID) {
    if (config.noteOrderInverted) {
      targetPos = getFirstUncheckedIndex() - 6;
    } else {
      targetPos = getFirstUncheckedIndex();
    }
    ESP_LOGI(TAG, "Insertion at mid point, hence screen position %d", targetPos);
  }
  if (!config.allowBeyondEdges && targetPos > (notesSize - 6)) {
    ESP_LOGI(TAG, "Target pos %d larger than end of list at %d, now %d", targetPos, notesSize - 6, notesSize - 6);
    targetPos = notesSize - 6;
  }
  if (!config.allowBeyondEdges && targetPos < 0) {
    targetPos = 0;
    ESP_LOGI(TAG, "Target pos too small, now %d", targetPos);
  }
  ESP_LOGI(TAG, "Insertion screen index at %d", targetPos);
  return targetPos;
}

int16_t NotesManager::getIndexOfNote(String text){
  for(int16_t i = 0; i<notes.size(); i++){
    if (notes[i][STR_TEXT] == text){
      return i;
    }
  }
  return -1;
}

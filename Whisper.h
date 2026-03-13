#pragma once

#include "Arduino.h"
#include "FS.h"
class Whisper {
  public:
    Whisper();
    void init(String serverName, String serverPath, String model, String lang, String authToken = "", String authType = "");
    String transcribeFile(File fileToSend, File certificate,  uint8_t * error);

  private:
    String serverName;
    String serverPath; 
    String model;
    String lang = ""; 
    String authToken;
    String authType;
};

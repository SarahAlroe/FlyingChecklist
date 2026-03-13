#include "Arduino.h"
#include "Whisper.h"
#include <NetworkClientSecure.h>
#include "wav_header.h"
#include "ESP_I2S.h"

static const char* TAG = "Whisper";

Whisper::Whisper() {
}

void Whisper::init(String serverName, String serverPath, String model, String lang, String authToken, String authType) {
  ESP_LOGI(TAG, "Initialising Whisper at %s", serverName.c_str());
  this->serverName = serverName;
  this->serverPath = serverPath;
  this->lang = lang;
  this->authToken = authToken;
  this->model = model;
  this->authType = authType;
  if (this->authType == ""){
    this->authType = "Bearer";
  }
}

String Whisper::transcribeFile(File fileToSend, File certificate, uint8_t * error){
  fileToSend.seek(0);
  certificate.seek(0);
  char* cert = (char *)malloc(certificate.size() + 1);
  certificate.read((uint8_t*)cert, certificate.size());
  NetworkClientSecure *client = new NetworkClientSecure;
  ESP_LOGV(TAG, "Cert: %s", cert);
  client->setCACert(cert);
  int serverPort = 443;
  * error = 0;
  if (client->connect(this->serverName.c_str(), serverPort)) {
    String head = "--AutCam\r\n"
                  "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
    head +=       this->model.c_str();
                  //"deepdml/faster-whisper-large-v3-turbo-ct2"
    head +=       "\r\n--AutCam\r\n"
                  "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"
                  "text"
                  "\r\n--AutCam\r\n";
    if (this->lang != ""){
      head += "Content-Disposition: form-data; name=\"language\"\r\n\r\n";
      head += this->lang;
      head += "\r\n--AutCam\r\n";
    }
    head += "Content-Disposition: form-data; name=\"file\"; filename=\"";
    head += fileToSend.name();
    head += "\"\r\nContent-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--AutCam--\r\n\r\n";
    uint32_t imageLen = fileToSend.size();
    uint32_t extraLen = head.length() + tail.length();
    uint32_t totalLen = imageLen + extraLen;
    client->println("POST " + this->serverPath + " HTTP/1.1");
    client->println("Host: " + serverName);
    client->println("Accept: */*");
    client->println("Content-Type: multipart/form-data; boundary=AutCam");
    client->println("Authorization: "+this->authType+" " + this->authToken);
    client->println("Content-Length: " + String(totalLen));
    client->println();
    client->print(head);
  
    uint8_t buffer[1024];
    while( fileToSend.available() ) {
      size_t read_bytes = fileToSend.read( buffer, 1024 );
      client->write(buffer, read_bytes);
    }
    client->print(tail);
    unsigned long timeout = millis();
    while(client->available() == 0){
      if(millis() - timeout > 120000){ // Larger timeout
        ESP_LOGW(TAG, "Client Timeout !");
        client->stop();
        * error = 1;
        return "";
      }
      delay(2);
    }
    String firstLine = client->readStringUntil('\r');
    ESP_LOGD(TAG, "< %s", firstLine.c_str());
    firstLine = firstLine.substring(firstLine.indexOf(" ")+1, firstLine.indexOf(" ")+4); // Take the 3 character responsecode right after first space.
    bool got200Code = firstLine.toInt() < 300;
    int expectedContentLength = -1;
    while(client->available()) {
      String line = client->readStringUntil('\r');
      ESP_LOGD(TAG, "< %s", line.c_str());
      if (line.startsWith("\nContent-Length:")){
        expectedContentLength = line.substring(line.indexOf(" ")+1).toInt();
      }
      if (line == "\n"){ // Is the CR (LF) CR LF ending sequence of the header
        client -> read(); // Read the remaining LF
        break;
      }
      delay(2);
    }
    ESP_LOGD(TAG, "Expected response length: %d", expectedContentLength);
    String body = "";
    while(client->available()) {
      body += client->readStringUntil('\n');
      delay(2);
    }
    client->stop();
    if (expectedContentLength == -1 || (body == "" && expectedContentLength != 0)){
      * error = 1;
    }
    return body;
  }else{
    * error = 1;
    return ""; // Failed to connect
  }
}
#define CAMERA_MODEL_AI_THINKER

#include <WiFi.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <Base64.h>
#include "esp_camera.h"
#include <ArduinoJson.h>
#include <credentials.h>  // this file contains SSID, password, and ChatGPT key
#include "camera_pins.h"
#include "helpers.h"

const char* openai_api_url = "https://api.openai.com/v1/chat/completions";
const char* openai_api_key = apiKey;


String response = "";

#define PICTURELEN 10000

// Pin definitions
#define RELAY_PIN 2


String captureImage() {
  digitalWrite(LAMP_PIN, HIGH);  // Turn on LED
  delay(100);
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    debugMQTT("No picture");
    digitalWrite(LAMP_PIN, LOW);  // Turn off LED
    delay(200);
    esp_restart();  // Trigger a software reset
  }
  String _imageBase64 = base64::encode((const uint8_t*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  Serial.println("captured");
  // Serial.println(_imageBase64);
  digitalWrite(LAMP_PIN, LOW);  // Turn off LED
  return _imageBase64;
}

String sendQuestionToChatGPT(char question[], String imageBase64) {
  HTTPClient http;
  // Create JSON payload
  DynamicJsonDocument doc(PICTURELEN);
  doc["model"] = "gpt-4o";
  JsonArray messages = doc.createNestedArray("messages");

  JsonObject message = messages.createNestedObject();
  message["role"] = "user";

  JsonArray content = message.createNestedArray("content");

  JsonObject textObject = content.createNestedObject();
  textObject["type"] = "text";
  textObject["text"] = question;

  JsonObject imageObject = content.createNestedObject();
  imageObject["type"] = "image_url";

  JsonObject imageUrl = imageObject.createNestedObject("image_url");
  imageUrl["url"] = "data:image/jpeg;base64," + imageBase64 + "\"";

  doc["max_tokens"] = 300;

  String payload;
  serializeJson(doc, payload);
  doc.clear();

  http.begin(openai_api_url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + openai_api_key);

  // Send the POST request
  int httpResponseCode = http.POST(payload);

  // Handle the response
  String GPTresponse;
  if (httpResponseCode > 0) {
    GPTresponse = http.getString();
    //Serial.println("GPTResponse:");
    //Serial.println(GPTresponse);
  } else {
    Serial.print("Error on sending POST: ");
    Serial.println(httpResponseCode);
    debugMQTT("Error sending POST");
    GPTresponse = "0";  // in case of error water is on
  }
  http.end();
  DynamicJsonDocument responseBody(4096);
  DeserializationError error = deserializeJson(responseBody, GPTresponse);

  if (error) {
    Serial.print("JSON parse failed: ");
    Serial.println(error.c_str());
    debugMQTT("JSON not decoded");
    return "";  // Assume no cat detected on error
  }

  String answer = responseBody["choices"][0]["message"]["content"];
  answer.toLowerCase();
  Serial.print("Answer from API: ");
  debugMQTT(answer);
  Serial.println(answer);
  return (answer);
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(LAMP_PIN, OUTPUT);
  digitalWrite(LAMP_PIN, LOW);

  connectWiFi();
  setupCamera();
  defineMACaddress();
  client.setCallback(callback);
  setupOTA();
  connectMQTT();
  HA_auto_discovery();
  Serial.print("stateTopic");
  Serial.println(stateTopicName);
  debugMQTT("Connected");

  String imageBase64 = captureImage();
  if (imageBase64.isEmpty()) {
    debugMQTT("no image");
    delay(200);
    esp_restart();
  }
  debugMQTT("Image taken");
  Serial.println("1st Question-----------");
  debugMQTT("1st question");
  response = sendQuestionToChatGPT("what do you see on the image?", imageBase64);
  Serial.println("2nd Question-----------");
    debugMQTT("2nd question");
  response = sendQuestionToChatGPT("Do you recognize a cat on the image? Please answer only with yes or no", imageBase64);
  if (response != "") {
    if (response.indexOf("yes") != -1) {
      Serial.println("Cat detected!");
      client.publish(stateTopicName.c_str(), "ON");
    } else {
      Serial.println("No cat detected!");
      client.publish(stateTopicName.c_str(), "OFF");
    }
  } else {
    client.publish(stateTopicName.c_str(), "No response");
    client.publish(stateTopicName.c_str(), "ON");
  }
}

void loop() {
  ArduinoOTA.handle();
  if (!client.connected()) {
    connectMQTT();
  }
  client.loop();
}

const int mqttPort = 1883;
const char* mqttUser = "admin";
const char* mqttPassword = "admin";

String macAddr;
String uniqueID;

String stateTopicName;
String discoveryTopicName;

#define receiveTopic "Cat_PIR"
#define MESSAGELENGTH 600

WiFiClient espClient;
PubSubClient client(espClient);

void setupCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_SVGA;
  config.pixel_format = PIXFORMAT_JPEG;  // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Limit the frame size when PSRAM is not available
      Serial.println("Please switch PSRAM on in configuration");
      while (1)
        config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t* s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // flip it back
    s->set_brightness(s, 1);   // up the brightness just a bit
    s->set_saturation(s, -2);  // lower the saturation
  }
  // drop down frame size for higher initial frame rate
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

// Setup LED FLash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash(LED_GPIO_NUM);
#endif
}

void connectWiFi() {
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("WiFi Connected!");
}

void connectMQTT() {
  client.setServer(mqttServer, mqttPort);
  while (!client.connected()) {
    Serial.println("Connecting to MQTT...");
    if (client.connect("ESP32Client", mqttUser, mqttPassword)) {
      Serial.println("Connected to MQTT");
      client.subscribe(receiveTopic);
    } else {
      Serial.print("MQTT connection failed. Reset.");
      esp_restart();
    }
  }
}

void debugMQTT(String _message) {
  Serial.print("------------");
  Serial.println(_message);
  client.publish(stateTopicName.c_str(), _message.c_str());
}

void setupOTA() {
  ArduinoOTA.setHostname("Cat-Discoverer");  // Set the OTA device name
  ArduinoOTA.setPassword("admin");           // Optional: Set an OTA password
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else {  // U_SPIFFS
      type = "filesystem";
    }
    Serial.println("Start updating " + type);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });

  ArduinoOTA.begin();
  Serial.println("OTA Ready");
}


void HA_auto_discovery() {
  // Construct the autodiscovery message for the binary sensor
  StaticJsonDocument<MESSAGELENGTH> doc;  // Allocate memory for the JSON document
  doc["name"] = "Cat-Discover";
  doc["device_class"] = "occupancy";
  doc["qos"] = 0;
  doc["unique_id"] = uniqueID;  // Use MAC-based unique ID
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["state_topic"] = stateTopicName;
  /*
  // Create a nested array for availability
  JsonArray availability = doc.createNestedArray("availability");
  JsonObject availability_0 = availability.createNestedObject();
  availability_0["topic"] = String(stateTopicName) + "/availability";
  availability_0["payload_available"] = "online";
  availability_0["payload_not_available"] = "offline";
*/
  JsonObject device = doc.createNestedObject("device");
  device["ids"] = macAddr;          // Use the full MAC address as identifier
  device["name"] = "Cat-Discover";  // Device name
  device["mf"] = "Sensorsiot";      // Include supplier info
  device["mdl"] = "Notifier";
  device["sw"] = "1.0";
  device["hw"] = "0.9";

  char buffer[MESSAGELENGTH];
  serializeJson(doc, buffer);  // Serialize JSON object to buffer

  Serial.println(discoveryTopicName.c_str());
  Serial.print("AutodiscoveryMessage: ");
  Serial.println(buffer);  // Print the JSON payload to Serial Monitor
  Serial.print("Length of buffer: ");
  Serial.println(strlen(buffer));
  client.publish(discoveryTopicName.c_str(), buffer, true);  // Publish to MQTT with retain flag set
}

void callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("Message received on topic ");
  Serial.print(topic);
  Serial.print(": ");
  Serial.println(message);

  if (String(topic) == receiveTopic && message == "occupied") {
    Serial.println("Motion detected! Capturing image...");
 // detect();
  }
}

void defineMACaddress(){
    // Get the MAC address of the board
  macAddr = WiFi.macAddress();
  Serial.println(macAddr);
  String hi = macAddr;
  hi.toLowerCase();
  hi.replace(":", "");  // Remove colons from MAC address to make it topic-friendly
  // Extract the last 6 characters of the MAC address (ignoring colons)
  uniqueID = "catNotifier-" + hi.substring(hi.length() - 4);  // Use last 4 byte
  stateTopicName = "homeassistant/binary_sensor/" + uniqueID + "/state";
  discoveryTopicName = "homeassistant/binary_sensor/" + uniqueID + "/config";

  Serial.print("uniqueID: ");
  Serial.println(uniqueID);
  Serial.print("stateTopicName: ");
  Serial.println(stateTopicName);
  Serial.print("discoveryTopicName: ");
  Serial.println(discoveryTopicName);
}
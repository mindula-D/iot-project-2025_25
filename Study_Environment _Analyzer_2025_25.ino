#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Wire.h>
#include <U8g2lib.h>

// WiFi and MQTT Configuration
const char* ssid = "Dialog4G";              
const char* password = "Mindula@3210";          
const char* mqtt_server = "test.mosquitto.org";
const int mqtt_port = 1883;
const char* client_id = "ESP32_StudyAnalyzer"; // Unique client ID

// IMPORTANT: Make sure these match exactly with your Node-RED flow
const char* topic_sensor_data = "study_analyzer/sensor_data"; 
const char* topic_mongodb_data = "study_analyzer/mongodb_data"; 

// MongoDB storage interval (REDUCED FOR TESTING)
unsigned long lastMongoDBPublishTime = 0;
const long mongoDBPublishInterval = 1000;   // 30 seconds for testing (change to 300000 for 5 minutes)

// Pin Definitions
#define DHT_PIN 19
#define LDR_ANALOG_PIN 34
#define LDR_DIGITAL_PIN 25
#define SOUND_ANALOG_PIN 32
#define SOUND_DIGITAL_PIN 23
#define GREEN_LED 13
#define RED_LED 14
#define YELLOW_LED 15

// Constants
#define DHTTYPE DHT22

// MQTT client
WiFiClient espClient;
PubSubClient client(espClient);

// Initialize U8g2 for your OLED display
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// Sensor objects
DHT dht(DHT_PIN, DHTTYPE);

// Variables
unsigned long lastSensorReadTime = 0;
const long sensorReadInterval = 2000;      // Read sensors every 2 seconds
unsigned long lastMqttPublishTime = 0;
const long mqttPublishInterval = 5000;     // Publish data every 5 seconds
unsigned long lastReconnectAttempt = 0;
const long reconnectInterval = 5000;       // Try to reconnect every 5 seconds

// Connection status tracking
bool mqttConnected = false;
int mongoDbCounter = 0;                    // Count successful MongoDB publishes

// Sensor values
float temperature = 0;
float humidity = 0;
int lightAnalogValue = 0;
bool lightDigitalValue = false;
float luxValue = 0;
int soundValue = 0;
float decibelValue = 0;
bool soundThreshold = false;
int studyQualityScore = 0;
String studyQualityCategory = "";

// Light categories
String getLightCategory(float lux) {
  if (lux < 50) return "VeryDark";
  else if (lux < 200) return "Dark";
  else if (lux < 400) return "ModLight";
  else if (lux < 600) return "Bright";
  else return "VeryBrt";
}

// Sound categories
String getSoundCategory(int rawValue) {
  if (rawValue < 1700) return "VeryQuiet";
  else if (rawValue < 1900) return "Quiet";
  else if (rawValue < 2100) return "Moderate";
  else if (rawValue < 2300) return "Noisy";
  else return "VeryNoisy";
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  // Wait for connection with timeout
  unsigned long startAttemptTime = millis();
  
  while (WiFi.status() != WL_CONNECTED && 
         millis() - startAttemptTime < 20000) { // 20 second timeout
    digitalWrite(YELLOW_LED, HIGH);
    delay(250);
    digitalWrite(YELLOW_LED, LOW);
    delay(250);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("");
    Serial.println("WiFi connection FAILED");
    // Indicate connection failure with red LED
    digitalWrite(RED_LED, HIGH);
  }
}

// Callback for MQTT messages
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
}

boolean reconnect() {
  // Loop until we're reconnected
  if (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    
    // Simplified connection approach from code 2
    if (client.connect(client_id)) {
      Serial.println("connected");
      mqttConnected = true;
      // Once connected, publish an announcement
      client.publish("study_analyzer/status", "ESP32 Study Analyzer connected");
      
      // Blink green LED to indicate successful connection
      for (int i = 0; i < 3; i++) {
        digitalWrite(GREEN_LED, HIGH);
        delay(100);
        digitalWrite(GREEN_LED, LOW);
        delay(100);
      }
      return true;
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again later");
      mqttConnected = false;
      
      // Flash yellow LED to indicate connection issue
      for (int i = 0; i < 3; i++) {
        digitalWrite(YELLOW_LED, HIGH);
        delay(100);
        digitalWrite(YELLOW_LED, LOW);
        delay(100);
      }
      return false;
    }
  }
  return true;
}

void readDHTSensor() {
  // Read temperature and humidity
  humidity = dht.readHumidity();
  temperature = dht.readTemperature();
  
  // Check if reading was successful
  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("Failed to read from DHT sensor!");
    humidity = 0;
    temperature = 0;
  }
}

void readLightSensor() {
  // Read light sensor (both analog and digital)
  lightAnalogValue = analogRead(LDR_ANALOG_PIN);
  lightDigitalValue = digitalRead(LDR_DIGITAL_PIN);
  
  // Convert to approximate lux value (needs calibration)
  luxValue = calibrateLuxValue(lightAnalogValue);
}

void readSoundSensor() {
  // Read sound sensor multiple times and average to reduce noise
  soundValue = 0;
  const int numSamples = 10;
  
  for (int i = 0; i < numSamples; i++) {
    soundValue += analogRead(SOUND_ANALOG_PIN);
    delay(5); // Short delay between readings
  }
  
  // Calculate average
  soundValue = soundValue / numSamples;
  
  // Read digital threshold value
  soundThreshold = digitalRead(SOUND_DIGITAL_PIN);
  
  // Calculate approximate decibel value
  decibelValue = map(soundValue, 0, 4095, 30, 100); // Rough approximation
}

void printAllReadings() {
  Serial.println("\n----- Current Environment Readings -----");
  
  // Temperature and Humidity
  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.print(" °C | Humidity: ");
  Serial.print(humidity);
  Serial.println(" %");
  
  // Light
  Serial.print("Light Level (RAW): ");
  Serial.print(lightAnalogValue);
  Serial.print(" | Light Threshold: ");
  Serial.print(lightDigitalValue ? "HIGH (Dark)" : "LOW (Bright)");
  Serial.print(" | Estimated Illuminance: ");
  Serial.print(luxValue);
  Serial.print(" lux | Category: ");
  Serial.println(getLightCategory(luxValue));
  
  // Sound
  Serial.print("Sound Level (RAW): ");
  Serial.print(soundValue);
  Serial.print(" | Sound Threshold: ");
  Serial.print(soundThreshold ? "HIGH" : "LOW");
  Serial.print(" | Category: ");
  Serial.println(getSoundCategory(soundValue));
  
  Serial.println("-----------------------------------------");
}

void analyzeEnvironment() {
  bool temperatureOptimal = (temperature >= 20 && temperature <= 25);
  bool humidityOptimal = (humidity >= 40 && humidity <= 60);
  bool lightOptimal = (luxValue >= 300 && luxValue <= 500);
  bool soundOptimal = (soundValue < 300); // Using raw value threshold
  
  // Simple implementation of Study Quality Score
  studyQualityScore = 0;
  if (temperatureOptimal) studyQualityScore += 25;
  if (humidityOptimal) studyQualityScore += 25;
  if (lightOptimal) studyQualityScore += 25;
  if (soundOptimal) studyQualityScore += 25;
  
  Serial.print("Study Environment Quality Score: ");
  Serial.print(studyQualityScore);
  Serial.print("/100 - ");
  
  if (studyQualityScore >= 75) {
    studyQualityCategory = "OPTIMAL";
    Serial.println("OPTIMAL");
    digitalWrite(GREEN_LED, HIGH);
    digitalWrite(RED_LED, LOW);
  } else if (studyQualityScore >= 50) {
    studyQualityCategory = "ACCEPTABLE";
    Serial.println("ACCEPTABLE");
    digitalWrite(GREEN_LED, HIGH);
    digitalWrite(RED_LED, HIGH);
  } else {
    studyQualityCategory = "SUBOPTIMAL";
    Serial.println("SUBOPTIMAL");
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(RED_LED, HIGH);
  }
}

void updateDisplay() {
  u8g2.clearBuffer();
  
  // Display header
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(0, 9, "Smart Study Environment");
  u8g2.drawLine(0, 11, 128, 11);
  
  // Display temperature and humidity (top section)
  char tempStr[16], humStr[16];
  sprintf(tempStr, "T:%.1fC", temperature);
  sprintf(humStr, "H:%.1f%%", humidity);
  u8g2.drawStr(0, 21, tempStr);
  u8g2.drawStr(70, 21, humStr);
  
  // Small temperature and humidity bar indicators
  u8g2.drawFrame(45, 17, 20, 5);
  int tempFill = map(temperature, 10, 35, 0, 20);
  u8g2.drawBox(45, 17, tempFill, 5);
  
  u8g2.drawFrame(115, 17, 12, 5);
  int humFill = map(humidity, 0, 100, 0, 12);
  u8g2.drawBox(115, 17, humFill, 5);
  
  // Divider
  u8g2.drawLine(0, 23, 128, 23);
  
  // Light level (middle section)
  char luxStr[20], ldrStr[16];
  sprintf(luxStr, "Light:%.0f lx", luxValue);
  sprintf(ldrStr, "Raw:%d", lightAnalogValue);
  u8g2.drawStr(0, 33, luxStr);
  u8g2.drawStr(70, 33, ldrStr);
  
  // Get light category
  String lightCat = getLightCategory(luxValue);
  u8g2.drawStr(0, 42, lightCat.c_str());
  
  // Light level bar (inverted for your sensor)
  u8g2.drawFrame(70, 38, 58, 5);
  // For inverted sensor: higher raw value = less light, so invert the fill
  int lightFill = map(4095 - lightAnalogValue, 0, 4095, 0, 58);
  u8g2.drawBox(70, 38, lightFill, 5);
  
  // Divider
  u8g2.drawLine(0, 45, 128, 45);
  
  // Sound level (lower section)
  char soundStr[20];
  sprintf(soundStr, "Sound: Raw:%d", soundValue);
  u8g2.drawStr(0, 55, soundStr);
  
  // Get sound category based on raw value
  String soundCat = getSoundCategory(soundValue);
  u8g2.drawStr(0, 64, soundCat.c_str());
  
  // Sound level bar
  u8g2.drawFrame(70, 60, 58, 5);
  int soundFill = map(soundValue, 0, 1500, 0, 58); // Adjusted range for better visibility
  soundFill = constrain(soundFill, 0, 58);
  u8g2.drawBox(70, 60, soundFill, 5);
  
  // Display connection status in top-right corner
  char statusStr[16];
  sprintf(statusStr, "M%c DB:%d", mqttConnected ? '+' : '-', mongoDbCounter);
  u8g2.drawStr(72, 9, statusStr);
  
  u8g2.sendBuffer();
}

float calibrateLuxValue(int adcValue) {
  // Inverting the reading since the sensor gives lower values for higher light intensity
  int invertedValue = 4095 - adcValue;  // Invert the scale (assuming 12-bit ADC with range 0-4095)
  
  // Simple approximation - needs proper calibration
  // Map inverted value to lux range
  return map(invertedValue, 0, 4095, 0, 1000);
}

bool publishSensorData() {
  // Create JSON document for regular updates
  StaticJsonDocument<256> jsonDoc;
  
  jsonDoc["temperature"] = temperature;
  jsonDoc["humidity"] = humidity;
  jsonDoc["light_raw"] = lightAnalogValue;
  jsonDoc["light_lux"] = luxValue;
  jsonDoc["light_category"] = getLightCategory(luxValue);
  jsonDoc["sound_raw"] = soundValue;
  jsonDoc["sound_category"] = getSoundCategory(soundValue);
  jsonDoc["study_quality_score"] = studyQualityScore;
  jsonDoc["study_quality_category"] = studyQualityCategory;
  jsonDoc["timestamp"] = millis();
  
  // Serialize JSON to string
  char jsonBuffer[256];
  size_t n = serializeJson(jsonDoc, jsonBuffer);
  
  // Publish to MQTT broker
  Serial.print("Publishing sensor data: ");
  Serial.println(jsonBuffer);
  digitalWrite(YELLOW_LED, HIGH);
  
  bool success = client.publish(topic_sensor_data, jsonBuffer, n);
  
  if (success) {
    Serial.println("Publish successful");
  } else {
    Serial.println("Publish failed");
  }
  
  digitalWrite(YELLOW_LED, LOW);
  return success;
}

bool publishMongoDBData() {
  // Create a simpler JSON document for MongoDB storage - using code 2's approach
  StaticJsonDocument<256> mongoDoc;
  
  // Add fields with proper naming convention for MongoDB
  mongoDoc["deviceID"] = client_id;
  mongoDoc["temp"] = temperature;
  mongoDoc["humidity"] = humidity;
  mongoDoc["lightRaw"] = lightAnalogValue;
  mongoDoc["lightLux"] = luxValue;
  mongoDoc["lightCategory"] = getLightCategory(luxValue);
  mongoDoc["soundRaw"] = soundValue;
  mongoDoc["soundCategory"] = getSoundCategory(soundValue);
  mongoDoc["studyScore"] = studyQualityScore;
  mongoDoc["studyCategory"] = studyQualityCategory;
  mongoDoc["timestamp"] = millis();
  
  // Add database info for Node-RED
  mongoDoc["db_name"] = "study_environment_db";
  mongoDoc["collection_name"] = "study_env_new";
  
  // Serialize JSON to string
  char mongoBuffer[256];
  size_t n = serializeJson(mongoDoc, mongoBuffer);
  
  // Publish MongoDB data to MQTT broker using the simpler approach from code 2
  Serial.print("Publishing MongoDB data: ");
  Serial.println(mongoBuffer);
  digitalWrite(YELLOW_LED, HIGH);
  
  bool success = client.publish(topic_sensor_data, mongoBuffer, n);
  
  if (success) {
    Serial.println("MongoDB data publish successful");
    lastMongoDBPublishTime = millis(); // Update the last publish time
    mongoDbCounter++; // Increment counter for display
    for(int i=0; i<3; i++) {
        digitalWrite(GREEN_LED, HIGH);
        delay(50);
        digitalWrite(GREEN_LED, LOW);
        delay(50);
    }
  } else {
    Serial.println("MongoDB data publish failed");
  }
  
  digitalWrite(YELLOW_LED, LOW);
  return success;
}

void setup() {
  // Initialize serial communication
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n----- Smart Study Environment Analyzer -----");
  
  // Initialize pins
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(SOUND_DIGITAL_PIN, INPUT);
  pinMode(LDR_DIGITAL_PIN, INPUT);
  
  // Initialize DHT sensor
  dht.begin();
  
  // Initialize OLED display
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.setContrast(128);
  
  // Display startup message
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 12, "Smart Study");
  u8g2.drawStr(0, 24, "Environment Analyzer");
  u8g2.drawStr(0, 36, "Connecting...");
  u8g2.sendBuffer();
  
  // Setup WiFi
  setup_wifi();
  
  // Configure MQTT with simpler approach from code 2
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  
  // Update display with connection status
  u8g2.clearBuffer();
  u8g2.drawStr(0, 12, "Smart Study");
  u8g2.drawStr(0, 24, "Environment Analyzer");
  
  if (WiFi.status() == WL_CONNECTED) {
    u8g2.drawStr(0, 36, "WiFi Connected!");
    u8g2.drawStr(0, 48, WiFi.localIP().toString().c_str());
  } else {
    u8g2.drawStr(0, 36, "WiFi Failed!");
    u8g2.drawStr(0, 48, "Check settings");
  }
  u8g2.sendBuffer();
  delay(2000);
  
  // Try initial MQTT connection - simpler approach from code 2
  if (client.connect(client_id)) {
    mqttConnected = true;
    client.publish("study_analyzer/status", "ESP32 Study Analyzer started");
    
    // Send an immediate MongoDB test message
    Serial.println("Sending initial MongoDB test message...");
    publishMongoDBData();
  }
  
  // Indicate system is ready
  digitalWrite(GREEN_LED, HIGH);
  Serial.println("System initialized and ready!");
  
  // Initialize timing variables
  lastSensorReadTime = millis();
  lastMqttPublishTime = millis();
  lastMongoDBPublishTime = millis();
  lastReconnectAttempt = 0;
}

void loop() {
  unsigned long currentMillis = millis();
  
  // Check MQTT connection and reconnect if needed
  if (!client.connected()) {
    mqttConnected = false;
    // Try to reconnect every 5 seconds
    if (currentMillis - lastReconnectAttempt > reconnectInterval) {
      lastReconnectAttempt = currentMillis;
      if (reconnect()) {
        lastReconnectAttempt = 0;
      }
    }
  } else {
    // Client connected
    client.loop();
  }
  
  // Read sensors at regular intervals
  if (currentMillis - lastSensorReadTime >= sensorReadInterval) {
    lastSensorReadTime = currentMillis;
    
    // Blink yellow LED to indicate data reading in progress
    digitalWrite(YELLOW_LED, HIGH);
    
    // Read all sensor values
    // delay(5000);
    readDHTSensor();
    readLightSensor();
    readSoundSensor();
    
    // Display readings on serial monitor
    printAllReadings();
    
    // Analyze readings and determine environmental quality
    analyzeEnvironment();
    
    // Update OLED display with all readings
    updateDisplay();
    
    // Turn off yellow LED after reading is complete
    digitalWrite(YELLOW_LED, LOW);
  }
  
  // Publish data to MQTT at regular intervals if connected
  if (mqttConnected && currentMillis - lastMqttPublishTime >= mqttPublishInterval) {
    lastMqttPublishTime = currentMillis;
    publishSensorData();
  }
  
  // Publish data to MongoDB every 30 seconds if connected (testing interval)
  if (mqttConnected && currentMillis - lastMongoDBPublishTime >= mongoDBPublishInterval) {
    lastMongoDBPublishTime = currentMillis;
    publishMongoDBData();
  }
}
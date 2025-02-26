#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESPmDNS.h>

// ----------------------------
// RFID Configuration
// ----------------------------
#define RFID_SS_PIN    5     // SDA/SS Pin for RFID
#define RFID_RST_PIN   22    // RST Pin for RFID
MFRC522 mfrc522(RFID_SS_PIN, RFID_RST_PIN);

// ----------------------------
// Persistent Data
// ----------------------------
Preferences preferences;

// ----------------------------
// Global Variables & Provisioning Configuration
// ----------------------------
String savedSSID = "";          // Wi-Fi SSID (empty means not provisioned)
String savedPassword = "";      // Wi-Fi Password

// AP and captive portal settings
const char *provisionSSID = "ESP32_Provision";
const char *provisionPassword = "12345678";

// Custom captive portal domain
const char* customDomain = "myportal.local";

// DNS Server for captive portal
const byte DNS_PORT = 53;
DNSServer dnsServer;

// Async Web Server on port 80
AsyncWebServer webServer(80);

// Backend fallback URL (if mDNS discovery fails)
const char* fallbackServerURL = "http://192.168.1.36:8000/input-log/";
// Backend mDNS hostname
const char* serverMDNS = "esp-server.local";
String serverURL = "";          // Will be set via mDNS

// Wi-Fi scan results
#define MAX_NETWORKS 20
String networkList[MAX_NETWORKS];
int networkCount = 0;

// To avoid duplicate RFID scans
String lastUID = "";

// ----------------------------
// LED, Buzzer, and Vibration Pins
// ----------------------------
#define LED_RFID       25    // LED for RFID detection
#define LED_WIFI       26    // LED for Wi-Fi connection
#define LED_READY      27    // LED for System Ready
#define BUZZER_PIN     32    // Buzzer for feedback
#define VIBRATION_PIN  21    // Vibration motor

// ----------------------------
// HTML Landing Page for Provisioning (stored in flash)
// ----------------------------
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
  <title>ESP32 WiFi Provisioning</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; text-align: center; margin-top: 50px; }
    select, input { font-size: 16px; padding: 8px; }
  </style>
  </head><body>
  <h2>Select Wi-Fi Network and Enter Password</h2>
  <form method="POST" action="/save">
    <label for="ssid">Choose Network:</label><br>
    <select name="ssid" id="ssid">%OPTIONS%</select><br><br>
    Password: <input type="password" name="password"><br><br>
    <input type="submit" value="Save">
  </form>
</body></html>
)rawliteral";

// ----------------------------
// GPIO Initialization
// ----------------------------
void initGPIO() {
  Serial.println("🔧 Initializing GPIO...");
  pinMode(RFID_SS_PIN, OUTPUT);
  pinMode(LED_WIFI, OUTPUT);
  pinMode(LED_READY, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(VIBRATION_PIN, OUTPUT);
  digitalWrite(LED_WIFI, LOW);
  digitalWrite(LED_READY, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(VIBRATION_PIN, LOW);
  Serial.println("✅ GPIO Initialized!");
}

// ----------------------------
// Helper Functions: LED, Buzzer, Vibration
// ----------------------------
void blinkLED(int pin, int duration) {
  digitalWrite(pin, HIGH);
  delay(duration);
  digitalWrite(pin, LOW);
}

void beepBuzzer(int duration) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(duration);
  digitalWrite(BUZZER_PIN, LOW);
}

void vibrate(int duration) {
  digitalWrite(VIBRATION_PIN, HIGH);
  delay(duration);
  digitalWrite(VIBRATION_PIN, LOW);
}

// ----------------------------
// RFID GPIO Self-Test Function
// ----------------------------
void testRFIDGPIO() {
  Serial.println("🔧 Testing RFID GPIO outputs...");
  blinkLED(LED_READY, 300);
  beepBuzzer(300);
  vibrate(300);
  Serial.println("✅ RFID GPIO test completed.");
}

// ----------------------------
// Wi-Fi Scanning Function (Do NOT trim SSIDs)
// ----------------------------
void scanWiFiNetworks() {
  Serial.println("\n🔍 Scanning for Wi-Fi networks...");
  networkCount = WiFi.scanNetworks();
  if (networkCount == -1) {
    Serial.println("❌ Scan failed!");
    return;
  }
  if (networkCount > MAX_NETWORKS) networkCount = MAX_NETWORKS;
  for (int i = 0; i < networkCount; i++) {
    networkList[i] = WiFi.SSID(i);  // Preserve the exact SSID
  }
  Serial.printf("✅ Found %d networks:\n", networkCount);
  for (int i = 0; i < networkCount; i++) {
    Serial.println(networkList[i]);
  }
}

// ----------------------------
// Persistent Data: Save and Load Credentials
// ----------------------------
void loadCredentials() {
  preferences.begin("wifi", false);
  savedSSID = preferences.getString("ssid", "");
  savedPassword = preferences.getString("password", "");
  preferences.end();
}

void saveCredentials(String ssid, String pass) {
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", pass);
  preferences.end();
}

// ----------------------------
// Build Landing Page with Options
// ----------------------------
String buildLandingPage() {
  String options = "";
  for (int i = 0; i < networkCount; i++) {
    options += "<option value='" + networkList[i] + "'>" + networkList[i] + "</option>";
  }
  String page = FPSTR(index_html);
  page.replace("%OPTIONS%", options);
  return page;
}

// ----------------------------
// Captive Portal Handlers (AsyncWebServer)
// ----------------------------
void handleRoot(AsyncWebServerRequest *request) {
  request->send(200, "text/html", buildLandingPage());
}

void handleSave(AsyncWebServerRequest *request) {
  if (request->hasParam("ssid", true) && request->hasParam("password", true)) {
    String newSSID = request->getParam("ssid", true)->value();
    String newPassword = request->getParam("password", true)->value();
    
    saveCredentials(newSSID, newPassword);
    savedSSID = newSSID;
    savedPassword = newPassword;
    
    Serial.println("\n🔄 Received Credentials:");
    Serial.println("SSID: " + savedSSID);
    Serial.println("Password: " + savedPassword);
    request->send_P(200, "text/html", "<h3>Credentials Saved. Restarting...</h3>");
    delay(500);
    ESP.restart();
  } else {
    request->send(400, "text/html", "<h3>Invalid Data!</h3>");
  }
}

// ----------------------------
// Start Provisioning Mode: AP + Captive Portal
// ----------------------------
void startProvisioning() {
  Serial.println("\n🚀 Starting Provisioning Mode (AP Mode)...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(provisionSSID, provisionPassword);
  
  Serial.print("📡 AP IP Address: ");
  Serial.println(WiFi.softAPIP());
  
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.printf("✅ DNS Server started for domain '%s'\n", customDomain);
  
  scanWiFiNetworks();
  
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.onNotFound([](AsyncWebServerRequest *request){
    request->send(200, "text/html", buildLandingPage());
  });
  
  webServer.begin();
  Serial.println("✅ Provisioning Web Server Started!");
  Serial.printf("👉 Please visit http://%s in your browser\n", customDomain);
}

// ----------------------------
// Normal Operation: Connect using Saved Credentials
// ----------------------------
bool connectToWiFi() {
  Serial.print("🔗 Connecting to Wi-Fi: ");
  Serial.println(savedSSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(), savedPassword.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Wi-Fi Connected!");
    Serial.print("📡 IP Address: ");
    Serial.println(WiFi.localIP());
    digitalWrite(LED_WIFI, HIGH);
    return true;
  } else {
    Serial.println("\n❌ Failed to connect to Wi-Fi.");
    Serial.println("Clearing Wi-Fi credentials...");
    preferences.begin("wifi", false);
    preferences.clear();
    preferences.end();
    Serial.println("Credentials cleared. Restarting...");
    delay(500);
    ESP.restart();
    return false;
  }
}

// ----------------------------
// mDNS Setup: Discover Backend Server
// ----------------------------
void setupMDNS() {
  if (MDNS.begin("esp32")) {
    Serial.println("✅ ESP32 mDNS responder started as esp32.local");
  } else {
    Serial.println("❌ Error setting up mDNS responder.");
  }
  
  Serial.print("🔍 Querying for backend server via mDNS: ");
  Serial.println(serverMDNS);
  IPAddress serverIP = MDNS.queryHost(serverMDNS, 2000);
  if (serverIP) {
    Serial.print("✅ Found backend server IP: ");
    Serial.println(serverIP);
    serverURL = "http://" + serverIP.toString() + ":8000/input-log/";
  } else {
    Serial.println("❌ Backend server not found via mDNS.");
    
  }
}

// ----------------------------
// RFID Functions
// ----------------------------
void initRFID() {
  SPI.begin();
  mfrc522.PCD_Init();
  Serial.println("✅ RFID Scanner Ready");
  mfrc522.PCD_DumpVersionToSerial();
}

String readRFID() {
  if (!mfrc522.PICC_IsNewCardPresent()) return "";
  if (!mfrc522.PICC_ReadCardSerial()) return "";
  
  String uidStr = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    uidStr += String(mfrc522.uid.uidByte[i], HEX);
  }
  uidStr.toUpperCase();
  
  mfrc522.PICC_HaltA();
  
  Serial.println("✅ RFID Detected! UID: " + uidStr);
  blinkLED(LED_READY, 200);
  beepBuzzer(200);
  vibrate(300);
  
  return uidStr;
}

// ----------------------------
// Function to Send RFID Data to Backend (Disabled)
// ----------------------------
void sendRFIDData(String uid) {
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ Wi-Fi disconnected, can't send data!");
    return;
  }
  
  HTTPClient http;
  String fullURL = serverURL + WiFi.localIP().toString() + "/" + uid;
  Serial.print("📡 Sending request to: ");
  Serial.println(fullURL);
  
  http.begin(fullURL);
  int httpResponseCode = http.GET();
  
  if (httpResponseCode > 0) {
    Serial.print("✅ Server Response: ");
    Serial.println(http.getString());
  } else {
    Serial.print("❌ Error sending data! HTTP Code: ");
    Serial.println(httpResponseCode);
  }
  http.end();
  
}

// ----------------------------
// Serial Command: Clear Credentials
// ----------------------------
void checkSerialCommand() {
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim(); // Remove extra whitespace/newlines
    if (command.equalsIgnoreCase("clear")) {
      Serial.println("Clearing Wi-Fi credentials...");
      preferences.begin("wifi", false);
      preferences.clear();
      preferences.end();
      Serial.println("Credentials cleared. Restarting...");
      delay(500);
      ESP.restart();
    }
  }
}

// ----------------------------
// Main Setup & Loop
// ----------------------------
void setup() {
  Serial.begin(115200);
  initGPIO();
  initRFID();
  loadCredentials();
  testRFIDGPIO();
  
  // If no credentials are saved, start provisioning mode.
  if (savedSSID == "") {
    startProvisioning();
  } else {
    if (connectToWiFi()) {
      setupMDNS();
      digitalWrite(LED_READY, HIGH);
    }
  }
}

void loop() {
  // Always check for serial commands
  checkSerialCommand();
  
  // If in AP mode (provisioning), process DNS requests.
  if (WiFi.getMode() == WIFI_AP) {
    dnsServer.processNextRequest();
    return;
  }
  
  // Non-blocking reconnection: if WiFi is disconnected, check every 1 minute.
  static unsigned long lastReconnectTime = 0;
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastReconnectTime >= 60000) {  // 1 minute interval
      Serial.println("⚠️ Wi-Fi Disconnected! Attempting reconnection...");
      WiFi.disconnect();
      WiFi.begin(savedSSID.c_str(), savedPassword.c_str());
      lastReconnectTime = now;
    }
    return;
  }
  
  // Normal Operation: Scan for RFID card.
  String uid = readRFID();
  if (uid == "" || uid == lastUID) return;
  
  lastUID = uid;
  Serial.println("🔄 New Card Detected! UID: " + uid);
  
  // Data transfer is disabled; uncomment to enable backend communication.
  // sendRFIDData(uid);
  
  // Also check serial commands periodically.
  checkSerialCommand();
}

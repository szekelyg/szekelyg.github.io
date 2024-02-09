
// version 1.0.0.
// ez olvassa az adatokat a p1 portból közvetlenül, kábellel összedugva és küldi a szervernek
// ehhez használtunk egy jelinvertert és egy feszültségszint átalakítót
// uj topic struktura bevezetése
// ez már taltalmazza az inverter alapszintű lekérdezéséhez és szabolyozásához létrehozott függvényeket


#include <WiFiNINA.h>
#include <WiFiSSLClient.h>
#include <ArduinoBearSSL.h>
#include <ArduinoECCX08.h>
#include <PubSubClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Arduino.h>
#include "wiring_private.h"
#include <ArduinoRS485.h>
#include <ArduinoModbus.h>
#include <ArduinoOTA.h>

// Konstansok és Beállítások
//const char* ssid = "LANSolo";
//const char* password = "Sevenof";

Uart mySerial (&sercom3, 1, 0, SERCOM_RX_PAD_1, UART_TX_PAD_0);

const char* flexiBoxId = "MKR1010Client1"; 

const char* mqtt_server = "1d4a6d32c1c3472e8617499d19071e10.s2.eu.hivemq.cloud";
const long interval = 10000; // interval at which to send data (10 seconds)
unsigned long previousMillis = 0;
unsigned long epochTime;

unsigned long lastReconnectAttempt = 0;

char dsmrData;
String dsmrBuffer = "";
bool messageComplete = false;

String knownSSID = "LANSolo"; 
String knownPASSWORD = "Sevenof9"; 

WiFiSSLClient wifiClient;
PubSubClient client(wifiClient);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 3600, 60000);
WiFiServer server(80);


String meterSerialNumber = "";
String invSerialNumber = "";

char smartMeterTopic[100]; 
char flexiBoxStatusTopic[100]; 
char deviceCommandTopic[100];

//String smartMeterTopic = createTopic(flexiBoxId, "smartmeter", meterSerialNumber);
//String inverterTopic = createTopic(flexiBoxId, "inverter",invSerialNumber);


void setup() { 
  Serial.begin(9600);
  ModbusRTUClient.begin(9600);
  server.begin();
  timeClient.begin();
  setupWifi();
  client.setServer(mqtt_server, 8883);
  client.setCallback(callback);
  client.setKeepAlive(15);
  mySerial.begin(115200);
  // start the WiFi OTA library with internal (flash) based storage
  ArduinoOTA.begin(WiFi.localIP(), "Arduino", "alma", InternalStorage);
  pinPeripheral(1, PIO_SERCOM); //Assign RX function to pin 1
  pinPeripheral(0, PIO_SERCOM); //Assign TX function to pin 0
  ntpSync();
  createTopic(smartMeterTopic, flexiBoxId, "data", nullptr);
  createTopic(flexiBoxStatusTopic, flexiBoxId, "status", nullptr);
  createTopic(deviceCommandTopic, flexiBoxId, "command", nullptr);
}

void loop() {
  ArduinoOTA.poll();
  readDSMR();
  client.loop();
  reconnect();
}

void createTopic(char* buffer, const char* flexiBoxId, const char* category, const char* subCategory) {
  strcpy(buffer, "device/");
  strcat(buffer, flexiBoxId);
  strcat(buffer, "/");
  strcat(buffer, category);
  if (subCategory != nullptr) {
    strcat(buffer, "/");
    strcat(buffer, subCategory);
  }
}

void SERCOM3_Handler(){

  mySerial.IrqHandler();
}

// OBIS kódok angol megnevezései
const char* getEnglishNameForOBIS(const char* obisCode) {
  if (strcmp(obisCode, "1-0:32.7.0") == 0) return "instantaneous_voltage_l1";
  if (strcmp(obisCode, "1-0:52.7.0") == 0) return "instantaneous_voltage_l2";
  if (strcmp(obisCode, "1-0:72.7.0") == 0) return "instantaneous_voltage_l3";
  if (strcmp(obisCode, "1-0:31.7.0") == 0) return "instantaneous_current_c1";
  if (strcmp(obisCode, "1-0:51.7.0") == 0) return "instantaneous_current_l2";
  if (strcmp(obisCode, "1-0:71.7.0") == 0) return "instantaneous_current_c3";
  if (strcmp(obisCode, "1-0:13.7.0") == 0) return "instantaneous_power_factor";
  if (strcmp(obisCode, "1-0:33.7.0") == 0) return "instantaneous_fower_factor_l1";
  if (strcmp(obisCode, "1-0:53.7.0") == 0) return "instantaneous_power_factor_l2";
  if (strcmp(obisCode, "1-0:73.7.0") == 0) return "instantaneous_power_factor_l3";
  if (strcmp(obisCode, "1-0:14.7.0") == 0) return "frequency";
  if (strcmp(obisCode, "1-0:1.7.0") == 0) return "instantaneous_import_power";
  if (strcmp(obisCode, "1-0:2.7.0") == 0) return "instantaneous_export_power";
  return "Unknown";
}

void processLine(char* line, JsonObject &dataPoints) {
    if (strlen(line) > 0) {
        char* key = strtok(line, "(");
        char* rawValue = strtok(NULL, "\n");

        if (rawValue != NULL) {
            char* value = extractValue(rawValue);
            float floatValue = atof(value);
            int intValue = static_cast<int>(floatValue * 1000);

            String keyString = String(key);
            const char* englishName = getEnglishNameForOBIS(keyString.c_str());
            // Use the English name as the key in the data_points object

            dataPoints[englishName] = intValue;

            //Serial.print("NAME: ");
            //Serial.print(englishName);
            //Serial.print(", VALUE: ");
           // Serial.println(intValue);
        }
    }
}

char* extractValue(char* line) {
    char* end = strchr(line, '*');  // Keresse meg a '*' karaktert
    if (end == NULL) {
        end = strchr(line, ')');  // Ha nincs '*', keresse a ')' karaktert
    }
    if (end != NULL) {
        *end = '\0';  // Vágja le a stringet az elválasztó karakter előtt
    }
    return line;  // Visszaadja az értéket
}

void setupWifi() {
  Serial.print("Connecting to ");
  Serial.println(knownSSID);
  WiFi.begin(knownSSID.c_str(), knownPASSWORD.c_str());
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.println("try to connect to wifi");
    //WiFi.begin(knownSSID.c_str(), knownPASSWORD.c_str());
  }
  Serial.println("\nWiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

void handleInverterCommand(String type, String id, String cmd, String& result, String& errorMsg) {
  if (cmd == "start") {
    //set_power_on(); // Tegyük fel, hogy ez a függvény bekapcsolja az invertert
    result = "ON";
  } else if (cmd == "stop") {
    //set_power_off(); // Tegyük fel, hogy ez a függvény kikapcsolja az invertert
    result = "OFF";
  } else if (cmd == "backfeeding_start") {
    result = "Backfeeding_ON";
    /*if (setActivePowerControl(1)) {  // Aktív teljesítményszabályozás bekapcsolása
      result = "Backfeeding_ON";
    } else {
      errorMsg = "Failed to start backfeeding";
    }*/
  } else if (cmd == "backfeeding_stop") {
    result = "Backfeeding_OFF";
    /*if (setActivePowerControl(0)) {  // Aktív teljesítményszabályozás kikapcsolása
      result = "Backfeeding_OFF";
    } else {
      errorMsg = "Failed to stop backfeeding";
    }*/
  } else {
    errorMsg = "Unknown command";
  }
}


void sendResponse(String transactionId, String type, String id, String result, String errorMsg) {
  DynamicJsonDocument responseDoc(256);
  responseDoc["transaction_id"] = transactionId;
  JsonArray commands = responseDoc.createNestedArray("commands");
  JsonObject command = commands.createNestedObject();
  command["type"] = type;
  command["id"] = id;
  if (errorMsg != "") {
    command["result"] = "error";
    command["msg"] = errorMsg;
  } else {
    command["result"] = result;
  }
  String responseString;
  serializeJson(responseDoc, responseString);
  Serial.println("RESPONSE MESSAGE: " + responseString);
  //client.publish(deviceCommandTopic, responseString.c_str());
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.println("] ");
  
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  DynamicJsonDocument doc(1024);
  deserializeJson(doc, message);
  
  String transactionId = doc["transaction_id"];
  JsonArray commands = doc["commands"].as<JsonArray>();
  
  for (JsonVariant command : commands) {
    String type = command["type"];
    String id = command["id"].as<String>();
    String cmd = command["command"].as<String>();
    String result;
    String errorMsg;

    if (type == "inverter") {
      handleInverterCommand(type, id, cmd, result, errorMsg);

      //Serial.println("ezzel hivjuk meg a sendResposet trid: " + String(transactionId) + " type: " + String(type) + "id: "+ String(id) + " result: " + String(result) + " errorMsg: " + String(errorMsg));
      sendResponse(transactionId, type, id, result, errorMsg);
    }
  }
}



void mqttConnect() {
  if (WiFi.status() == WL_CONNECTED) { // Ellenőrizzük, hogy van-e internetkapcsolat
    while (!client.connected()) {
      Serial.print("Attempting MQTT connection...");
      String willMsg = "{\"status\": \"offline\" }";
      const char* willTopic = flexiBoxStatusTopic;
      const char* willMessage = willMsg.c_str();
      if (client.connect(String(flexiBoxId).c_str(), "szekelyg", "Sevenof9", willTopic, 1, false, willMessage)) {
        Serial.println("connected");
        String onlinePayload = "{ \"status\": \"online\" }";
        client.publish(flexiBoxStatusTopic, onlinePayload.c_str());
        client.subscribe(deviceCommandTopic);
      } else {
        Serial.print("failed, rc=");
        Serial.print(client.state());
        Serial.println(" try again in 5 seconds");
        delay(5000);
      }
    }
  } else {   
    Serial.println("No internet connection. Skipping MQTT connection attempt.");
    delay(5000);
  }
}

void processDSMRData(String data) {

  meterSerialNumber = extractMeterSerialNumber(data);
  
  if (meterSerialNumber=="") {
    Serial.println("Nem sikerült kinyerni a mérő gyári számát.");
    return;
  }else{
    //smartMeterTopic = createTopic(flexiBoxId, "smartmeter", meterSerialNumber);
  }
  //Serial.println(meterSerialNumber);
  DynamicJsonDocument doc(1024);

  //doc["serial_number"] = flexiBoxId;
  doc["timestamp"] = timeClient.getEpochTime();
  doc["backing_device_type"] = "smart_meter";
  doc["backing_device_serial_number"] = meterSerialNumber;

  // Létrehoz egy alatta lévő objektumot a data_points számára
  JsonObject dataPoints = doc.createNestedObject("data_point");

  int start = 0, end;
  while ((end = data.indexOf('\n', start)) != -1) {
    String line = data.substring(start, end);
    char* cLine = const_cast<char*>(line.c_str());

    // OBIS kód és érték kinyerése
    char* key = strtok(cLine, "(");
    char* rawValue = strtok(NULL, "*");

    if (key != NULL && rawValue != NULL) {
      // OBIS kód átalakítása angol nevére
      String keyString = String(key);
      const char* englishName = getEnglishNameForOBIS(keyString.c_str());

      // Csak akkor dolgozza fel, ha létezik angol név az OBIS kódhoz
      if (strcmp(englishName, "Unknown") != 0) {
        // Érték kinyerése, konvertálása és megszorzása
        rawValue = extractValue(rawValue);
        float floatValue = atof(rawValue) * 1000;
        int intValue = static_cast<int>(floatValue); // Konvertálás egész számra

        dataPoints[englishName] = intValue; // Egész szám hozzáadása a data_points objektumhoz
      }
    }
    start = end + 1;
  }

  String jsonString;
  serializeJson(doc, jsonString);
  //Serial.println(jsonString);
  client.setBufferSize(1024);

  bool result = client.publish(smartMeterTopic, jsonString.c_str());
  if (result) {
      Serial.println("MÉRT P1 port adatok sikeresen kiadva az MQTT-n.");
  } else {
      Serial.println("Hiba történt a P1 adatok kiadásakor az MQTT-n.");
  }
  
}

void ntpSync() {
  // Próbálja meg frissíteni az időt az NTP szervertől
  bool isTimeUpdated = false;
  for (int i = 0; i < 5; ++i) {  // Próbálkozik ötször
    if (timeClient.update()) {
      isTimeUpdated = true;
      break;  // Szinkronizáció sikeres
    }
    Serial.print("NTP szinkronizációs próbálkozás: ");
    Serial.println(i + 1);
    delay(1000);  // Vár 1 másodpercet a következő próbálkozás előtt
  }

  if (isTimeUpdated) {
    Serial.println("NTP szinkronizált");
    Serial.print("Epoch idő: ");
    Serial.println(timeClient.getEpochTime());
  } else {
    Serial.println("NTP szinkronizáció sikertelen");
  }
}

void reconnect() {
  // Csatlakozási kísérlet csak akkor, ha a kapcsolat megszakadt
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    // Ellenőrizzük, hogy eltelt-e már 5 perc az utolsó próbálkozás óta
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      // Próbálkozás a hálózathoz való csatlakozással
      Serial.println("Próbálkozás újra csatlakozni a Wi-Fi hálózathoz...");
      WiFi.disconnect();
      WiFi.begin(knownSSID.c_str(), knownPASSWORD.c_str());
    }
  } else {
    // Ha a csatlakozás sikeres, frissítjük az utolsó kísérlet időpontját
    lastReconnectAttempt = millis();
  }
}

void readDSMR(){
    // Adatok olvasása a szériális portról
    while (mySerial.available()) {
        char receivedChar = (char)mySerial.read();
        dsmrBuffer += receivedChar;

        // Ellenőrizzük, hogy a teljes üzenet beérkezett-e
        if (receivedChar == '!') {
            messageComplete = true;
        }
    }

    // Ha teljes üzenet érkezett
    if (messageComplete) {
        //Serial.println(dsmrBuffer);
        processDSMRData(dsmrBuffer); // Feldolgozza és elküldi az adatokat az MQTT-n keresztül
        dsmrBuffer = ""; // Puffer törlése
        messageComplete = false;
    }
  if (!client.connected()) {
    mqttConnect();
  }
}

String extractMeterSerialNumber(const String& dsmrData) {
  int start = dsmrData.indexOf('/');
  int end = dsmrData.indexOf('\n', start);
  if (start != -1 && end != -1) {
    String serialNumber = dsmrData.substring(start + 1, end);
    // Eltávolítja a '\r' karaktert, ha az a string végén van
    if(serialNumber.endsWith("\r")) {
      serialNumber.remove(serialNumber.length() - 1);
    }
    return serialNumber;
  }
  return "";
}



//
//
//
// *************** INVERTER DATA SETTERS AND GETTERS **********************
// ************************************************************************
// 
//
//
//


//SETTERS

bool set_power_off(){ 
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.holdingRegisterWrite(3, 200, 0)) {
      tryCount++;
      Serial.println("tryed " + String(tryCount) + " times");
    } else {
      Serial.println("INVERTER STOPPED");
      return true;
    }
  }
  return false;
}

bool set_power_on(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.holdingRegisterWrite(3, 200, 1)) {
      tryCount++;
      Serial.println("tryed " + String(tryCount) + " times");
    } else {
      Serial.println("INVERTER STARTED");
      return true;
    }
  }
  return false;
}  
  


bool setActivePowerControl(int value){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.holdingRegisterWrite(3, 4000, value)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      Serial.println("Power control value set successfully to: " + String(value)) ;
      return true;
    }
  }
  return false;
}


bool setPowerPn(int pn){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.holdingRegisterWrite(3, 5402, pn)) {
      tryCount++;
      Serial.println("tryed " + String(tryCount) + " times");
    } else {
      Serial.println("Power control value set successfully to: " + String(pn) + " %") ;
      return true;
    }
  }
  return false;
}


//GETTERS

int inverter_e_today(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1302, 2)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t pwr1 = ModbusRTUClient.read();
      uint16_t pwr2 = ModbusRTUClient.read();
      uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return pwr;
    }
  }
  return -1;
}

int inverter_e_total(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1304, 2)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t pwr1 = ModbusRTUClient.read();
      uint16_t pwr2 = ModbusRTUClient.read();
      uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return pwr;
    }
  }
  return -1;
}

int device_state(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1308, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t state = ModbusRTUClient.read();
      //uint16_t pwr2 = ModbusRTUClient.read();
      //uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return state;
    }
  }
  return -1;
}

int air_temperature(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1310, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t air_temperature = ModbusRTUClient.read();
      //uint16_t pwr2 = ModbusRTUClient.read();
      //uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return air_temperature;
    }
  }
  return -1;
}

int inverter_temperature_u(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1311, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t inverter_temperature_u = ModbusRTUClient.read();
      //uint16_t pwr2 = ModbusRTUClient.read();
      //uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return inverter_temperature_u;
    }
  }
  return -1;
}

int inverter_temperature_v(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1312, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t inverter_temperature_v = ModbusRTUClient.read();
      //uint16_t pwr2 = ModbusRTUClient.read();
      //uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return inverter_temperature_v;
    }
  }
  return -1;
}

int inverter_temperature_w(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1313, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t inverter_temperature_w = ModbusRTUClient.read();
      //uint16_t pwr2 = ModbusRTUClient.read();
      //uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return inverter_temperature_w;
    }
  }
  return -1;
}

int boost_temperature(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1314, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t boost_temperature = ModbusRTUClient.read();
      //uint16_t pwr2 = ModbusRTUClient.read();
      //uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return boost_temperature;
    }
  }
  return -1;
}

int bidirectional_dcdc_converter_temperature(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1315, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t bidirectional_dcdc_converter_temperature = ModbusRTUClient.read();
      //uint16_t pwr2 = ModbusRTUClient.read();
      //uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return bidirectional_dcdc_converter_temperature;
    }
  }
  return -1;
}

int voltage_l1(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1358, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t voltage_l1 = ModbusRTUClient.read();
      //uint16_t pwr2 = ModbusRTUClient.read();
      //uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return voltage_l1;
    }
  }
  return -1;
}

int current_l1(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1359, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t current_l1 = ModbusRTUClient.read();
      //uint16_t pwr2 = ModbusRTUClient.read();
      //uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return current_l1;
    }
  }
  return -1;
}

int voltage_l2(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1360, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t voltage_l2 = ModbusRTUClient.read();
      //uint16_t pwr2 = ModbusRTUClient.read();
      //uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return voltage_l2;
    }
  }
  return -1;
}

int current_l2(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1361, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t current_l2 = ModbusRTUClient.read();
      //uint16_t pwr2 = ModbusRTUClient.read();
      //uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return current_l2;
    }
  }
  return -1;
}

int voltage_l3(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1362, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t voltage_l3 = ModbusRTUClient.read();
      //uint16_t pwr2 = ModbusRTUClient.read();
      //uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return voltage_l3;
    }
  }
  return -1;
}

int current_l3(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1363, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t current_l3 = ModbusRTUClient.read();
      //uint16_t pwr2 = ModbusRTUClient.read();
      //uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return current_l3;
    }
  }
  return -1;
}

int apparent_power(){
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1368, 2)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t return_data1 = ModbusRTUClient.read();
      uint16_t return_data2 = ModbusRTUClient.read();
      uint32_t return_data = return_data1 << 16 | return_data2;
      //Serial.println("Inv E today: " + String(pwr) + "kWh");
      return return_data;
    }
  }
  return -1;
}

int active_power() {
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1370, 2)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t pwr1 = ModbusRTUClient.read();
      uint16_t pwr2 = ModbusRTUClient.read();
      uint32_t pwr = pwr1 << 16 | pwr2;
      //Serial.println("PV1 Power: " + String(pwr) + "W");
      return pwr;
    }
  }
  return -1;
}

int pw_total_power() {
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1600, 2)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t pwr1 = ModbusRTUClient.read();
      uint16_t pwr2 = ModbusRTUClient.read();
      uint32_t pw_total_power = pwr1 << 16 | pwr2;
      //Serial.println("PV1 Power: " + String(pwr) + "W");
      return pw_total_power;
    }
  }
  return -1;
}

int pw_e_today() {
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1602, 2)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t pwr1 = ModbusRTUClient.read();
      uint16_t pwr2 = ModbusRTUClient.read();
      uint32_t pw_e_today = pwr1 << 16 | pwr2;
      //Serial.println("PV1 Power: " + String(pwr) + "W");
      return pw_e_today;
    }
  }
  return -1;
}

int pw_e_total() {
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1604, 2)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t pwr1 = ModbusRTUClient.read();
      uint16_t pwr2 = ModbusRTUClient.read();
      uint32_t pw_e_total = pwr1 << 16 | pwr2;
      //Serial.println("PV1 Power: " + String(pwr) + "W");
      return pw_e_total;
    }
  }
  return -1;
}

int battery_status() {
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1607, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t ret1 = ModbusRTUClient.read();
      //uint16_t pwr2 = ModbusRTUClient.read();
      //uint32_t ret = pwr1 << 16 | pwr2;
      //Serial.println("PV1 Power: " + String(pwr) + "W");
      return ret1;
    }
  }
  return -1;
}

int battery_power() {
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1618, 2)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t ret1 = ModbusRTUClient.read();
      uint16_t ret2 = ModbusRTUClient.read();
      uint32_t ret = ret1 << 16 | ret2;
      //Serial.println("PV1 Power: " + String(pwr) + "W");
      return ret;
    }
  }
  return -1;
}

int battery_temperature() {
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1620, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t ret1 = ModbusRTUClient.read();
      //uint16_t pwr2 = ModbusRTUClient.read();
      //uint32_t ret = pwr1 << 16 | pwr2;
      //Serial.println("PV1 Power: " + String(pwr) + "W");
      return ret1;
    }
  }
  return -1;
}

int battery_soc() {
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1621, 1)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t ret1 = ModbusRTUClient.read();
      //uint16_t ret2 = ModbusRTUClient.read();
      //uint32_t ret = ret1 << 16 | ret2;
      //Serial.println("PV1 Power: " + String(pwr) + "W");
      return ret1;
    }
  }
  return -1;
}

int battery_e_charge_today() {
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1623, 2)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t ret1 = ModbusRTUClient.read();
      uint16_t ret2 = ModbusRTUClient.read();
      uint32_t ret = ret1 << 16 | ret2;
      //Serial.println("PV1 Power: " + String(pwr) + "W");
      return ret;
    }
  }
  return -1;
}

int battery_e_discharge_today() {
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1624, 2)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t ret1 = ModbusRTUClient.read();
      uint16_t ret2 = ModbusRTUClient.read();
      uint32_t ret = ret1 << 16 | ret2;
      //Serial.println("PV1 Power: " + String(pwr) + "W");
      return ret;
    }
  }
  return -1;
}

int e_consumption_today_ac_side() {
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1629, 2)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t ret1 = ModbusRTUClient.read();
      uint16_t ret2 = ModbusRTUClient.read();
      uint32_t ret = ret1 << 16 | ret2;
      //Serial.println("PV1 Power: " + String(pwr) + "W");
      return ret;
    }
  }
  return -1;
}

int e_generation_today_ac_side() {
  bool done = false;
  int tryCount = 0;
  while(!done && tryCount != 10){
    if (!ModbusRTUClient.requestFrom(3, INPUT_REGISTERS, 1631, 2)) {
      tryCount++;
      //Serial.println("tryed " + String(tryCount) + " times");
    } else {
      uint16_t ret1 = ModbusRTUClient.read();
      uint16_t ret2 = ModbusRTUClient.read();
      uint32_t ret = ret1 << 16 | ret2;
      //Serial.println("PV1 Power: " + String(pwr) + "W");
      return ret;
    }
  }
  return -1;
}


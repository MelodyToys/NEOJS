#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#ifdef ESP32
#include <FS.h>
#include <SPIFFS.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESP8266mDNS.h>
#endif
#include <ESPAsyncWebServer.h>
#include <SPIFFSEditor.h>
#include <Adafruit_NeoPixel.h>
extern "C" {
  #include <elk.h> 
}

#define DEBUG     0
#define NEOJS_VER "2.0-PRE"

typedef String (*cmd_action_t)(const char *);

typedef struct {
  const char *command;
  cmd_action_t action;
} cmd_t;

String lsCmd(const char *arg);     // list files
String pwdCmd(const char *arg);    // present working directory
String cdCmd(const char *arg);     // change directory
String rmCmd(const char *arg);     // remove file
String mvCmd(const char *arg);     // move file
String cpCmd(const char *arg);     // copy file
String catCmd(const char *arg);    // concatenate file
String rebootCmd(const char *arg); // reboot hardware reset
String reloadCmd(const char *arg); // reload virtual machine
String wifiCmd(const char *arg);   // wifi commands
String configCmd(const char *arg); // config commands
String verCmd(const char *arg);    // version
String infoCmd(const char *arg);   // info 
String helpCmd(const char *arg);   // help 

const cmd_t commands[] = {
  {"help", helpCmd},
  {"info", infoCmd},
  {"ver", verCmd},
  {"config", configCmd},
  {"wifi", wifiCmd},
  {"reload", reloadCmd},
  {"reboot", rebootCmd},
  {"reboot", rebootCmd},
  {"cat", catCmd},
  {"cp", cpCmd},
  {"mv", mvCmd},
  {"rm", rmCmd},
  {"cd", cdCmd},
  {"pwd", pwdCmd},
  {"ls", lsCmd}
};

struct Config {
  int  elk_alloc;
  int  led_pin;
  int  led_count;
  char wifi_sta_ssid[64];
  char wifi_sta_pass[64];
  char wifi_ap_ssid[64];
  char wifi_ap_pass[64];
  char hostName[64];
  char http_username[64];
  char http_password[64];
};

Config  config;
String  serial_Repl = "";
struct  js *js;
bool    js_init = false;
String  spiffs_pwd  = "";
bool    spiffs_init = false;

Adafruit_NeoPixel strip(10, 10, NEO_GRB + NEO_KHZ800); // FIXME: Hack ~ set to anything ... then redefine in setup()

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AsyncEventSource events("/events");

int WheelR(int Pos) {
  Pos = 255 - Pos;
  if(Pos < 85) { return 255 - Pos * 3; }
  if(Pos < 170) { return 0; }
  Pos -= 170;
  return Pos * 3;
}

int WheelG(int Pos) {
  Pos = 255 - Pos;
  if(Pos < 85) { return 0; }
  if(Pos < 170) { Pos -= 85; return Pos * 3; }
  Pos -= 170;
  return 255 - Pos * 3;
}

int WheelB(int Pos) {
  Pos = 255 - Pos;
  if(Pos < 85) { return Pos * 3; }
  if(Pos < 170) { Pos -= 85; return 255 - Pos * 3; }
  return 0;
}

uint32_t Wheel32(byte WheelPos) {
  WheelPos = 255 - WheelPos;
  if(WheelPos < 85) {
    return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
  }
  if(WheelPos < 170) {
    WheelPos -= 85;
    return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3);
  }
  WheelPos -= 170;
  return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}

bool elkLoad(String path) {
  jsval_t v;
  String buffer;
  char c = 0;

  if(path.endsWith(".js")) {
    if(SPIFFS.exists(path)) {
      File f = SPIFFS.open(path, "r");
      if(!f) {
        if(DEBUG) { Serial.print(F("file open failed: ")); Serial.println(path); }
        return false;
      } else {
       if(DEBUG) { Serial.print(F("reading script from file: ")); Serial.println(path); }
       while (f.available()) {
         c = f.read();
         if(DEBUG) {Serial.write(c);}
         buffer += c;
       }  
       f.close();
       if(DEBUG) { Serial.println(F("file closed")); }
       v = js_eval(js, buffer.c_str(), buffer.length());
       if(DEBUG) { Serial.print(F("js_eval() result: ")); Serial.println(js_str(js, v)); }
       buffer = "";
       return true;
      }
    } else {
      if(DEBUG) { Serial.print(F("file not found: ")); Serial.println(path);}
      return false;
    }
  } else {
    if(DEBUG) { Serial.print(F("not a javascript file: ")); Serial.println(path);}
    return false;    
  }
}

extern "C" {
  unsigned long Sys_millis() { millis(); }
  void Sys_println(jsval_t v) { 
    Serial.println(js_str(js, v));
    ws.printfAll("%s", js_str(js, v));
  }
  
  // a little hack for async type delay()'s
  void Sys_delay(unsigned long milli) {
    unsigned long targetMillis = millis() + milli;
	  unsigned long currentMillis = millis();
  
	  while(currentMillis < targetMillis) {
		  serialRepl();
		  ArduinoOTA.handle();
		  ws.cleanupClients();
		  yield();
		  currentMillis = millis();
	  }
  }
  
  void Neo_show() { strip.show(); }
  void Neo_clear() { strip.clear(); }
  void Neo_setPixelColor(uint16_t n, int r, int g, int b) { strip.setPixelColor(n, strip.Color(r, g, b)); }
  void Neo_setPixelColor32(uint16_t n, uint32_t c) { strip.setPixelColor(n, c); }  
//  uint32_t Neo_gamma32(uint32_t x) { return strip.gamma32(x); }
//  uint32_t Neo_ColorHSV(uint16_t hue, uint8_t sat, uint8_t val){ return strip.ColorHSV(hue); }
  uint32_t Neo_Wheel32(byte wheelpos) { return Wheel32(wheelpos); }
  int Neo_WheelR(int wheelpos) { return WheelR(wheelpos); }
  int Neo_WheelG(int wheelpos) { return WheelG(wheelpos); }
  int Neo_WheelB(int wheelpos) { return WheelB(wheelpos); }
  int Neo_numPixels(void) { return strip.numPixels(); }
  void js_load(const char *str) { elkLoad(String(str)); }
}

void elkInit() {
  if(DEBUG) { Serial.print(F("Starting VM (Elk ")); Serial.print(JS_VERSION); Serial.print(F(") ")); }
  js = js_create(malloc(config.elk_alloc), config.elk_alloc);
  jsval_t global = js_glob(js), serial = js_mkobj(js), pixel = js_mkobj(js);
  js_set(js, global, "Serial", serial);
  js_set(js, global, "Pixel", pixel);
  
  js_set(js, global, "millis", js_import(js, (uintptr_t) Sys_millis, "i"));
  js_set(js, global, "delay", js_import(js, (uintptr_t) Sys_delay, "vi"));
  js_set(js, serial, "println", js_import(js, (uintptr_t) Sys_println, "vj"));
  js_set(js, pixel, "show", js_import(js, (uintptr_t) Neo_show, "v"));
  js_set(js, pixel, "clear", js_import(js, (uintptr_t) Neo_clear, "v"));
  js_set(js, pixel, "setPixelColor", js_import(js, (uintptr_t) Neo_setPixelColor, "viiii"));
  js_set(js, pixel, "setPixelColor32", js_import(js, (uintptr_t) Neo_setPixelColor32, "vii"));
  js_set(js, global, "Wheel32", js_import(js, (uintptr_t) Neo_Wheel32, "ii"));
  js_set(js, global, "WheelR", js_import(js, (uintptr_t) Neo_WheelR, "ii"));
  js_set(js, global, "WheelG", js_import(js, (uintptr_t) Neo_WheelG, "ii"));
  js_set(js, global, "WheelB", js_import(js, (uintptr_t) Neo_WheelB, "ii"));
  js_set(js, global, "load", js_import(js, (uintptr_t) js_load, "vs"));
  js_set(js, pixel, "numPixels", js_import(js, (uintptr_t) Neo_numPixels, "i")); 

  if(spiffs_init) {
    if(elkLoad(String("/init.js"))) {
      if(DEBUG) { Serial.println(F("/init.js loaded")); }
      jsval_t v = js_eval(js, "typeof(setup);", ~0);
      if (String(js_str(js, v)).substring(0) == "\"function\"") {
        if(DEBUG) { Serial.println(F("setup() function found")); }
        v = js_eval(js, "setup();", ~0);
        if(DEBUG) { Serial.print(F("setup() function: ")); Serial.println(js_str(js, v)); }
      } else {
        if(DEBUG) { Serial.println(F("setup() function not found")); }
      }      
      v = js_eval(js, "typeof(loop);", ~0);
      if (String(js_str(js, v)).substring(0) == "\"function\"") {
        if(DEBUG) { Serial.println(F("loop() function found")); }
        js_init = true;
      } else {
        if(DEBUG) { Serial.println(F("loop() function not found")); }
        js_init = false;
      }
    } else {
      if(DEBUG) { Serial.println(F("/init.js failed")); }
      js_init = false;
    }
    Serial.printf("\n\r\n\rConnected to NeoJS SerialREPL (Elk %s)\n\r", JS_VERSION);
  }
}

String elkReload() {
  ws.printfAll("Connected to NeoJS WebREPL (Elk %s)", JS_VERSION);
  elkInit();
  return String("Reloading Elk VM ...");
}

void setup(){
  WiFi.persistent(false); // do not use SDK wifi settings in flash ?
    
  Serial.begin(115200);
  Serial.setDebugOutput(false); // do not use wifi debug to console
  Serial.println("");
  if(!SPIFFS.begin()) {
    if(DEBUG) { Serial.println(F("SPIFFS Initialization ... failed")); }
  } else { 
    if(DEBUG) { Serial.println(F("\nSPIFFS Initialize....ok")); }
    spiffs_init = true;
  }

  if(!loadConfig(config)) {
    if(DEBUG) { Serial.println(F("Config Initialization ... failed")); }
  } else { if(DEBUG) { Serial.println(F("Config Initialize....ok")); }}

  strip.updateLength(config.led_count);
  strip.setPin(config.led_pin);
  strip.begin();
  strip.show();
  strip.setBrightness(50);
  
  elkInit();  

  WiFi.hostname(config.hostName);
  WiFi.mode(WIFI_AP_STA);
  if((String(config.wifi_ap_pass) != "") && (String(config.wifi_ap_ssid) != "")) // make sure ssid is set too?
    WiFi.softAP(config.wifi_ap_ssid, config.wifi_ap_pass);
  else
    if(String(config.wifi_ap_ssid) != "")
      WiFi.softAP(config.wifi_ap_ssid);
    else
      WiFi.softAP(config.hostName); // always start an AP ...
    
  if(String(config.wifi_sta_ssid != "")) {
    if(String(config.wifi_sta_pass != ""))
      WiFi.begin(config.wifi_sta_ssid, config.wifi_sta_pass);
    else
      WiFi.begin(config.wifi_sta_ssid);
  }

  ArduinoOTA.onStart([]() { events.send("Update Start", "ota"); });
  ArduinoOTA.onEnd([]() { events.send("Update End", "ota"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) { char p[32]; sprintf(p, "Progress: %u%%\n\r", (progress/(total/100))); events.send(p, "ota");});
  ArduinoOTA.onError([](ota_error_t error) { if(error == OTA_AUTH_ERROR) events.send("Auth Failed", "ota"); else if(error == OTA_BEGIN_ERROR) events.send("Begin Failed", "ota"); else if(error == OTA_CONNECT_ERROR) events.send("Connect Failed", "ota"); else if(error == OTA_RECEIVE_ERROR) events.send("Recieve Failed", "ota"); else if(error == OTA_END_ERROR) events.send("End Failed", "ota");});
  ArduinoOTA.setHostname(config.hostName);
  ArduinoOTA.begin();

  MDNS.addService("http","tcp",80);
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  events.onConnect([](AsyncEventSourceClient *client){client->send("hello!",NULL,millis(),1000);});
  server.addHandler(&events);
  if(String(config.http_username) != "") {
  #ifdef ESP32
    server.addHandler(new SPIFFSEditor(SPIFFS, config.http_username,config.http_password));
  #elif defined(ESP8266)
    server.addHandler(new SPIFFSEditor(config.http_username,config.http_password));
  #endif
  } else {
  #ifdef ESP32
    server.addHandler(new SPIFFSEditor(SPIFFS));
  #elif defined(ESP8266)
    server.addHandler(new SPIFFSEditor());
  #endif
  }
  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "[";
   
    int n = WiFi.scanComplete();
    if(n == -2){

    #ifdef ESP32
      WiFi.scanNetworks(true, true);
    #elif defined(ESP8266) 
      WiFi.scanNetworks(true);
    #endif

    } else if(n){
      for (int i = 0; i < n; ++i){
        if(i) json += ",";
        json += "{";
        json += "\"rssi\":"+String(WiFi.RSSI(i));
        json += ",\"ssid\":\""+WiFi.SSID(i)+"\"";
        json += ",\"bssid\":\""+WiFi.BSSIDstr(i)+"\"";
        json += ",\"channel\":"+String(WiFi.channel(i));
        json += ",\"secure\":"+String(WiFi.encryptionType(i));
    #ifdef ESP8266
        json += ",\"hidden\":"+String(WiFi.isHidden(i)?"true":"false");
    #endif
        json += "}";
      }
      WiFi.scanDelete();
      if(WiFi.scanComplete() == -2){
        WiFi.scanNetworks(true);
      }
    }
    json += "]";
    request->send(200, "application/json", json);
    json = String();}
  );
  server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request){request->send(200, "text/plain", String(ESP.getFreeHeap()));});
  server.on("/reload", HTTP_GET, [](AsyncWebServerRequest *request){request->send(200, "text/plain", String(elkReload()));}); 
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.htm");
  server.onNotFound([](AsyncWebServerRequest *request){
    int headers = request->headers();
    int i;
    for(i=0;i<headers;i++){
      AsyncWebHeader* h = request->getHeader(i);
    }
    int params = request->params();
    for(i=0;i<params;i++){
      AsyncWebParameter* p = request->getParam(i);
    }
    request->send(404);}
  );
  server.onFileUpload([](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final){});
  server.onRequestBody([](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){});
  server.begin();
  //FIXME: figure out how to pre-scan wifi??
}

void loop() {
  serialRepl();
  ArduinoOTA.handle();
  ws.cleanupClients();
  if(js_init)
    js_eval(js, "loop();", ~0);
}

String processCommand(String cmdString) {
  jsval_t v;
  String Responce;
  cmdString.trim();
  size_t cmd_num = (sizeof(commands) / sizeof (commands[0]));
  if (cmd_num > 0) {
    const cmd_t *command = commands;
    for (size_t i = 0; i < cmd_num; ++i) {
      if (cmdString.startsWith(command->command)) {
        cmdString.remove(0, String(command->command).length());
        cmdString.trim();
        Responce = command->action(cmdString.c_str());
        return Responce;
      }
      ++command;
    }
  }
  v = js_eval(js, cmdString.c_str(), cmdString.length());
  Responce = js_str(js, v);
  return Responce;
}

String lsCmd(const char *arg) {
  String Responce = "";
  Responce += "Done: arg = " + String(arg);
  return Responce;
}

String pwdCmd(const char *arg) {
  String Responce = "";
  Responce += "Done: arg = " + String(arg);
  return Responce;
}

String cdCmd(const char *arg) {
  String Responce = "";
  Responce += "Done: arg = " + String(arg);
  return Responce;
}

String rmCmd(const char *arg) {
  String Responce = "";
  Responce += "Done: arg = " + String(arg);
  return Responce;
}

String mvCmd(const char *arg) {
  String Responce = "";
  Responce += "Done: arg = " + String(arg);
  return Responce;
}

String cpCmd(const char *arg) {
  String Responce = "";
  Responce += "Done: arg = " + String(arg);
  return Responce;
}

String catCmd(const char *arg) {
  String Responce = "";
  Responce += "Done: arg = " + String(arg);
  return Responce;
}

String rebootCmd(const char *arg) {
  String Responce = "";
  Responce += "Done: arg = " + String(arg);
  return Responce;
}

String reloadCmd(const char *arg) {
  String Responce = "";
  Responce += "Done: arg = " + String(arg);
  return Responce;
}

String wifiCmd(const char *arg) {
  String Responce = "";
  Responce += "Done: arg = " + String(arg);
  return Responce;
}

String configCmd(const char *arg) {
  String Responce = "";
  Responce += "Done: arg = " + String(arg);
  return Responce;
}

String verCmd(const char *arg) {
  String Responce = "";
  Responce += "Done: arg = " + String(arg);
  return Responce;
}

String infoCmd(const char *arg) {
  String Responce = "";
  FSInfo fs_info;
  SPIFFS.info(fs_info);

  Responce += "Host: " + WiFi.hostname() + " Ssid: " + WiFi.SSID() + " Rssi: " + String(WiFi.RSSI()) + "% Chan: " + String(WiFi.channel()) + "\n\r";
  Responce += "IP: " + WiFi.localIP().toString() + " Mask: " + WiFi.subnetMask().toString() + " GW: " + WiFi.gatewayIP().toString() + "\n\r";
#ifdef ESP32
  Responce += "Device: ESP32";
#elif defined(ESP8266)
  Responce += "Device: ESP8266";
#endif
  Responce += " ID: " + String(ESP.getChipId(), HEX) + " CPU: " + String(ESP.getCpuFreqMHz()) + "MHz" + "\n\r";
  Responce += "Flash Id: " + String(ESP.getFlashChipId(), HEX) + " Size: " + String((ESP.getFlashChipSize()/1024)/1024) + "MB Speed: " + String(ESP.getFlashChipSpeed()/1000000) +"MHz Mode: ";
  switch (ESP.getFlashChipMode()) {
    case 0: Responce += "QIO\n\r"; break;
    case 1: Responce += "QOUT\n\r"; break;
    case 2: Responce += "DIO\n\r"; break;
    case 3: Responce += "DOUT\n\r"; break;
    case 255: Responce += "???\n\r"; break;
    default: Responce += "???\n\r"; break;
  }
  Responce += "Spiffs Size: " + String(fs_info.totalBytes/1024) +"KB Block: " + String(fs_info.blockSize/1024) + "KB Page: " + String(fs_info.pageSize) + "B\n\r";
  Responce += "SDK: " + String(ESP.getSdkVersion()) + " Core: " + String(ESP.getCoreVersion()) + " Arduino: " + String(ARDUINO) + "\n\r";
  Responce += "NeoJS: " + String(NEOJS_VER) + " Size: " + String(ESP.getSketchSize()/1024) + "KB Elk: " + String(JS_VERSION) + " Heap: " + String(config.elk_alloc) + "KB\n\r";
  Responce += "MD5: " + ESP.getSketchMD5() + "\n\r";
  Responce += "(Free) VM: " + String(100 - js_usage(js)) + "% Sketch: " + String(ESP.getFreeSketchSpace()/1024) + "KB Heap: " + String(ESP.getFreeHeap()/1024) + "KB Spiffs: " + String((fs_info.totalBytes - fs_info.usedBytes)/1024) + "KB\n\r";
  return Responce;
}

String helpCmd(const char *arg) {
  String Responce = "";
  Responce += "ls <dir> (list files in dir ... or pwd if non given)\n\r";
  Responce += "pwd (prints current present working directory)\n\r";
  Responce += "cd <dir> (change to directory)\n\rrm <file> (remove file)\n\r";
  Responce += "mv <file> <file> (move or rename file)\n\r";
  Responce += "cp <file> <file> (copy file)\n\rcat <file> (concatenate file)\n\r";
  Responce += "\n\rreboot (hardware reset)\n\rreload (vm reload)\n\r";
  Responce += "\n\rwifi\n\r  scan (wifi scan)\n\r";
  Responce += "  connect <ap> <password> (wifi connect, prompted for password when needed if not given)\n\r";
  Responce += "  status (current wifi connection information) \n\r\n\rconfig\n\r";
  Responce += "  <key> <value> (set config key value pair)\n\r  <key> (get config key's value)\n\r";
  Responce += "  (prints full current config file when no key given)\n\r\n\r";
  Responce += "ver (version)\n\rinfo (information about system)\n\rhelp (list commands and usage)\n\r";
  Responce += "Done: arg = " + String(arg);
  return Responce;
}

bool loadConfig(Config &config) {
  if(spiffs_init) {
    File configFile = SPIFFS.open("/config.json", "r");
    if (!configFile) {
      if(DEBUG) { Serial.println(F("Failed to open config file")); }
    }
    size_t size = configFile.size();
    if (size > 1024) {
      if(DEBUG) { Serial.println(F("Config file size is too large")); }
    }
    StaticJsonBuffer<512> jsonBuffer;
    JsonObject &root = jsonBuffer.parseObject(configFile);
    configFile.close();
    config.led_pin = root["led_pin"] | 4;
    config.led_count = root["led_count"] | 8;
    config.elk_alloc = root["elk_alloc"] | 4096;
    strlcpy(config.wifi_sta_ssid, root["wifi_sta_ssid"] | "", sizeof(config.wifi_sta_ssid));
    strlcpy(config.wifi_sta_pass, root["wifi_sta_pass"] | "", sizeof(config.wifi_sta_pass));
    strlcpy(config.wifi_ap_ssid, root["wifi_ap_ssid"] | "", sizeof(config.wifi_ap_ssid));
    strlcpy(config.wifi_ap_pass, root["wifi_ap_pass"] | "", sizeof(config.wifi_ap_pass));
    strlcpy(config.hostName, root["hostName"] | "NeoJS", sizeof(config.hostName));
    strlcpy(config.http_username, root["http_username"] | "", sizeof(config.http_username));
    strlcpy(config.http_password, root["http_password"] | "", sizeof(config.http_password));
    if (!root.success()) {
      if(DEBUG) { Serial.println(F("Failed to read file, using default configuration")); }
      return false;
    }
  } else {
    if(DEBUG) { Serial.println(F("Failed to read file, using default configuration")); }
    config.led_pin = 4;
    config.led_count = 8;
    config.elk_alloc = 4096;
    strlcpy(config.wifi_sta_ssid, "", sizeof(config.wifi_sta_ssid));
    strlcpy(config.wifi_sta_pass, "", sizeof(config.wifi_sta_pass));
    strlcpy(config.wifi_ap_ssid, "", sizeof(config.wifi_ap_ssid));
    strlcpy(config.wifi_ap_pass, "", sizeof(config.wifi_ap_pass));
    strlcpy(config.hostName, "NeoJS", sizeof(config.hostName));
    strlcpy(config.http_username, "", sizeof(config.http_username));
    strlcpy(config.http_password, "", sizeof(config.http_password));
    return false;
  }
  return true;
}

bool saveConfiguration(Config &config) {
  if(spiffs_init) {
    SPIFFS.remove("/config.json");
    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      if(DEBUG) { Serial.println(F("Failed to create Config file")); }
      return false;
    }
    StaticJsonBuffer<256> jsonBuffer;
    JsonObject &root = jsonBuffer.createObject();
    root["led_pin"] = config.led_pin;
    root["led_count"] = config.led_count;
    root["elk_alloc"] = config.elk_alloc;
    root["wifi_sta_ssid"] = config.wifi_sta_ssid;
    root["wifi_sta_pass"] = config.wifi_sta_pass;
    root["wifi_ap_ssid"] = config.wifi_ap_ssid; 
    root["wifi_ap_pass"] = config.wifi_ap_pass;
    root["hostname"] = config.hostName;
    root["http_username"] = config.http_username;
    root["http_password"] = config.http_password;
    if (root.printTo(configFile) == 0) {
      if(DEBUG) { Serial.println(F("Failed to write to Config file")); }
      return false;
    }
    configFile.close();
    if(DEBUG) { Serial.println(F("Config File write complete")); }
    return true;
  }
}

void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  String Responce;
  if(type == WS_EVT_CONNECT){
    client->printf("Connected to NeoJS REPL (Elk %s)", JS_VERSION);
    client->ping();
  } else if(type == WS_EVT_DATA){
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    String msg = "";
    if(info->final && info->index == 0 && info->len == len){
      if(info->opcode == WS_TEXT){
        for(size_t i=0; i < info->len; i++) {
          msg += (char) data[i];
        }
      } else {
        char buff[3];
        for(size_t i=0; i < info->len; i++) {
          sprintf(buff, "%02x ", (uint8_t) data[i]);
          msg += buff ;
        }
      }
      Responce = processCommand(msg);
    } else {
      if(info->opcode == WS_TEXT){
        for(size_t i=0; i < len; i++) {
          msg += (char) data[i];
        }
      } else {
        char buff[3];
        for(size_t i=0; i < len; i++) {
          sprintf(buff, "%02x ", (uint8_t) data[i]);
          msg += buff ;
        }
      }
      Responce = processCommand(msg);
    }
    Responce.replace("\n\r", "\n"); // strip double returns ... html supports both ...
    //client->printf("%s", msg.c_str()); // echo
    client->printf("%s", Responce.c_str());
  }
}

void serialRepl() {
  char c = 0;
  String Responce;
  
  if(Serial.available() > 0) {
    c = Serial.read();
    if(c == '\n' || c == '\r') {
      if(Serial.peek() == '\r' || Serial.peek() == '\n') { //f*%k microsoft
        c = Serial.read();
      }
      Responce = processCommand(serial_Repl);
      Serial.println(Responce);
      serial_Repl = "";
      Responce = "";
    } else if((c == 0x7F) || (c == '\b')) { // backspace
        if(serial_Repl.length() > 0) {
          serial_Repl.remove(serial_Repl.length()-1);
          Serial.write(" \b", 2);
        }
    } else {
      //Serial.write(c);
      serial_Repl += c;
    }
  }
}

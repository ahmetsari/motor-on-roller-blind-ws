#include <Stepper_28BYJ_48.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <PubSubClient.h>
#include <WiFiUdp.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include "FS.h"
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include "NidayandHelper.h"
#include "index_html.h"

//--------------- CHANGE PARAMETERS ------------------
//Configure Default Settings for Access Point logon
String APid = "BlindsConnectAP";    //Name of access point
String APpw = "ahmetsari";           //Hardcoded password for access point

//----------------------------------------------------

// Version number for checking if there are new code releases and notifying the user
String version = "0.9.1";

NidayandHelper helper = NidayandHelper();

//Fixed settings for WIFI
WiFiClient espClient;
PubSubClient psclient(espClient);   //MQTT client
char mqtt_server[40];             //WIFI config: MQTT server config (optional)
char mqtt_port[6] = "1883";       //WIFI config: MQTT port config (optional)
char mqtt_uid[40];             //WIFI config: MQTT server username (optional)
char mqtt_pwd[40];             //WIFI config: MQTT server password (optional)

String outputTopic;               //MQTT topic for sending messages
String inputTopic;                //MQTT topic for listening
boolean mqttActive = true;
char config_name[40];             //WIFI config: Bonjour name of device
char config_rotation[40] = "0;0"; //WIFI config: Detault rotation is CCW

String actions[] = {"",""};                      //Action manual/auto
int paths[2] = {0,0};                       //Direction of blind (1 = down, 0 = stop, -1 = up)
int setPos[2] = {0,0};                     //The set position 0-100% by the client
long currentPositions[2] = {0,0};           //Current position of the blind
long maxPositions[2] = {2000,2000};         //Max position of the blind. Initial value
String scheduledOpen[2] = {"",""};
String scheduledClose[2] = {"",""};
boolean loadDataSuccess = false;
int saveItNow[2] = {0,0};
bool shouldSaveConfig = false;      //Used for WIFI Manager callback to save parameters
boolean initLoop = true;            //To enable actions first time the loop is run
boolean ccw[2] = {true, true};                 //Turns counter clockwise to lower the curtain

Stepper_28BYJ_48 small_stepper1(D1, D2, D3, D4); //Initiate stepper driver
Stepper_28BYJ_48 small_stepper2(D5, D6, D7, D8); //Initiate stepper driver
Stepper_28BYJ_48 small_steppers[2] = {small_stepper1, small_stepper2};

ESP8266WebServer server(80);              // TCP server at port 80 will respond to HTTP requests
WebSocketsServer webSocket = WebSocketsServer(81);

//default custom static IP
char static_ip[16] = "192.168.1.150";
char static_gw[16] = "192.168.1.1";
char static_sn[16] = "255.255.255.0";

WiFiManager wifiManager;

bool loadConfig() {
  if (!helper.loadconfig()){
    return false;
  }
  JsonVariant json = helper.getconfig();

  //Store variables locally
  currentPositions[0] = long(json["currentPosition0"]);
  currentPositions[1] = long(json["currentPosition1"]);
  maxPositions[0] = long(json["maxPosition0"]);
  maxPositions[1] = long(json["maxPosition1"]);
  scheduledOpen[0] = String(json["scheduledOpen0"]);
  scheduledOpen[1] = String(json["scheduledOpen1"]);
  scheduledClose[0] = String(json["scheduledClose0"]);
  scheduledClose[1] = String(json["scheduledClose1"]);
  //strcpy(config_name, json["config_name"]);
  //strcpy(mqtt_server, json["mqtt_server"]);
  //strcpy(mqtt_port, json["mqtt_port"]);
  //strcpy(mqtt_uid, json["mqtt_uid"]);
  //strcpy(mqtt_pwd, json["mqtt_pwd"]);
  strcpy(config_rotation, json["config_rotation"]);

  return true;
}

/**
   Save configuration data to a JSON file
   on SPIFFS
*/
bool saveConfig() {
  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["currentPosition0"] = currentPositions[0];
  json["currentPosition1"] = currentPositions[1];
  json["maxPosition0"] = maxPositions[0];
  json["maxPosition1"] = maxPositions[1];
  
  char scheduledOpen0[scheduledOpen[0].length() + 1];
  char scheduledOpen1[scheduledOpen[1].length() + 1];
  char scheduledClose0[scheduledClose[0].length() + 1];
  char scheduledClose1[scheduledClose[1].length() + 1];
  scheduledOpen[0].toCharArray(scheduledOpen0, scheduledOpen[0].length() + 1);
  scheduledOpen[1].toCharArray(scheduledOpen1, scheduledOpen[1].length() + 1);
  scheduledClose[0].toCharArray(scheduledClose0, scheduledClose[0].length() + 1);
  scheduledClose[1].toCharArray(scheduledClose1, scheduledClose[1].length() + 1);
 
  json["scheduledOpen0"] = scheduledOpen0;
  json["scheduledOpen1"] = scheduledOpen1;
  json["scheduledClose0"] = scheduledClose0;
  json["scheduledClose1"] = scheduledClose1;
  //json["config_name"] = config_name;
  //json["mqtt_server"] = mqtt_server;
  //json["mqtt_port"] = mqtt_port;
  //json["mqtt_uid"] = mqtt_uid;
  //json["mqtt_pwd"] = mqtt_pwd;
  json["config_rotation"] = config_rotation;

  return helper.saveconfig(json);
}

/*
   Connect to MQTT server and publish a message on the bus.
   Finally, close down the connection and radio
*/
void sendmsg(String topic, String payload) {
  if (!mqttActive)
    return;

  helper.mqtt_publish(psclient, topic, payload);
}


/****************************************************************************************
*/
void processMsg(String command, String value, int motor_num, uint8_t clientnum){
  /*
     Below are actions based on inbound MQTT payload
  */
  if (command == "start") {
    /*
       Store the current position as the start position
    */
      currentPositions[motor_num] = 0;
      paths[motor_num] = 0;
      saveItNow[motor_num] = 1;
      actions[motor_num] = "manual";

  } else if (command == "max") {
    /*
       Store the max position of a closed blind
    */
      maxPositions[motor_num] = currentPositions[motor_num];
      paths[motor_num] = 0;
      saveItNow[motor_num] = 1;
      actions[motor_num] = "manual";

  } else if (command == "manual" && value == "0") {
    /*
       Stop
    */
      paths[motor_num] = 0;
      saveItNow[motor_num] = 1;
      actions[motor_num] = "manual";

  } else if (command == "manual" && value == "1") {
    /*
       Move down without limit to max position
    */
      paths[motor_num] = 1;
      actions[motor_num] = "manual";


  } else if (command == "manual" && value == "-1") {
    /*
       Move up without limit to top position
    */
      paths[motor_num] = -1;
      actions[motor_num] = "manual";


  } else if (command == "update") {
    //Send position details to client
    sendPos(0,clientnum);
    sendPos(1,clientnum);

  } else if (command == "ping") {
    //Do nothing
  } else if (command == "percent") {
    /*
       Any other message will take the blind to a position
       Incoming value = 0-100
       path is now the position
    */

    Serial.println("Received position " + value);

      paths[motor_num] = maxPositions[motor_num] * value.toInt() / 100;
      setPos[motor_num] = paths[motor_num]; //Copy path for responding to updates
      actions[motor_num] = "auto";

      //Send the instruction to all connected devices
      sendPos(motor_num,-1);
  } else if (command == "schedule") {
    int i = value.indexOf(";");
    String open = value.substring(0, i);
    Serial.println("-----------");
    Serial.println(open);
    String close = value.substring(i+1);
    Serial.println("-----------");
    Serial.println(close);
    Serial.println("-----------");
    scheduledOpen[motor_num] = open;
    scheduledClose[motor_num] = close;

    paths[motor_num] = 0;
    saveItNow[motor_num] = 1;
    actions[motor_num] = "manual";
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_TEXT:
            Serial.printf("[%u] get Text: %s\n", num, payload);

            String res = (char*)payload;

            StaticJsonBuffer<100> jsonBuffer1;
            JsonObject& root = jsonBuffer1.parseObject(payload);
            if (root.success()) {
              const int motor_id = root["id"];
              String command = root["action"];
              String value = root["value"];
              //Send to common MQTT and websocket function
              processMsg(command, value, motor_id, num);
              break;
            }
            else {
              Serial.println("parseObject() failed");
            }
    }
}
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");
    String res = "";
    for (int i = 0; i < length; i++) {
      res += String((char) payload[i]);
    }
    String motor_str = topic;
    int motor_id = motor_str.charAt(motor_str.length() - 1) - 0x30;

    if (res == "update" || res == "ping") {
      processMsg(res, "", 0, 0);
    } else
      processMsg("auto", res, motor_id, NULL);
}

/**
  Turn of power to coils whenever the blind
  is not moving
*/
void stopPowerToCoils(int motor_id) {
  Serial.println("Stopping power to coils " + String(motor_id));
  if(motor_id == 0){
    digitalWrite(D1, LOW);
    digitalWrite(D2, LOW);
    digitalWrite(D3, LOW);
    digitalWrite(D4, LOW);
  }else if(motor_id == 1){
    digitalWrite(D5, LOW);
    digitalWrite(D6, LOW);
    digitalWrite(D7, LOW);
    digitalWrite(D8, LOW);
  }
}

bool shouldSavePos(){
    return (saveItNow[0] + saveItNow[1]) > 0;
}

/*
   Callback from WIFI Manager for saving configuration
*/
void saveConfigCallback () {
  shouldSaveConfig = true;
}

void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}
void handleReset() {
  server.send(200, "text/html", "Erasing settings and restarting...");
  helper.resetsettings(wifiManager);
  delay(1000);
  ESP.eraseConfig(); 
  delay(2000);
  ESP.reset(); 
}
void handleNotFound(){
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

void sendPos(int index, int clientNum){
        //Send position details to client
        int maxPos = maxPositions[index];
        if(maxPos == 0){
          maxPos=1;
        }
        int set = (setPos[index] * 100)/maxPos;
        int pos = (currentPositions[index] * 100)/maxPos;
        String payload = "{\"id\":"+String(index)+",\"set\":"+String(set)+", \"position\":"+String(pos)+"}";
        sendmsg(outputTopic, payload);
        if(clientNum == -1){
            webSocket.broadcastTXT(payload);
        }else{
            webSocket.sendTXT(clientNum, payload);
        }
        sendmsg(outputTopic, payload);
}

void extractRotationConfig(){
    int i = String(config_rotation).indexOf(";");
    ccw[0] = String(config_rotation).substring(0, i)=="0";
    ccw[1] = String(config_rotation).substring(i+1)=="0";
}

void setup(void)
{
  Serial.begin(115200);
  delay(100);
  Serial.print("Starting now\n");

  //handleReset();
  
  //Reset the action
  actions[0] = "";
  actions[1] = "";

  //Set MQTT properties
  outputTopic = helper.mqtt_gettopic("out");
  inputTopic = helper.mqtt_gettopic("in");

  //Set the WIFI hostname
  WiFi.hostname(config_name);

  //Define customer parameters for WIFI Manager
  //WiFiManagerParameter custom_config_name("Name", "Bonjour name", config_name, 40);
  WiFiManagerParameter custom_rotation("Rotation", "Clockwise rotation", config_rotation, 40);
  //WiFiManagerParameter custom_text("<p><b>Optional MQTT server parameters:</b></p>");
  //WiFiManagerParameter custom_mqtt_server("server", "MQTT server", mqtt_server, 40);
  //WiFiManagerParameter custom_mqtt_port("port", "MQTT port", mqtt_port, 6);
  //WiFiManagerParameter custom_mqtt_uid("uid", "MQTT username", mqtt_server, 40);
  //WiFiManagerParameter custom_mqtt_pwd("pwd", "MQTT password", mqtt_server, 40);
  //WiFiManagerParameter custom_text2("<script>t = document.createElement('div');t2 = document.createElement('input');t2.setAttribute('type', 'checkbox');t2.setAttribute('id', 'tmpcheck');t2.setAttribute('style', 'width:10%');t2.setAttribute('onclick', \"if(document.getElementById('Rotation').value == 'false'){document.getElementById('Rotation').value = 'true'} else {document.getElementById('Rotation').value = 'false'}\");t3 = document.createElement('label');tn = document.createTextNode('Clockwise rotation');t3.appendChild(t2);t3.appendChild(tn);t.appendChild(t3);document.getElementById('Rotation').style.display='none';document.getElementById(\"Rotation\").parentNode.insertBefore(t, document.getElementById(\"Rotation\"));</script>");
  //Setup WIFI Manager

  //set static ip
  IPAddress _ip, _gw, _sn;
  _ip.fromString(static_ip);
  _gw.fromString(static_gw);
  _sn.fromString(static_sn);

  wifiManager.setSTAStaticIPConfig(_ip, _gw, _sn);
  
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  //add all your parameters here
  //wifiManager.addParameter(&custom_config_name);
  wifiManager.addParameter(&custom_rotation);
  //wifiManager.addParameter(&custom_text);
  //wifiManager.addParameter(&custom_mqtt_server);
  //wifiManager.addParameter(&custom_mqtt_port);
  //wifiManager.addParameter(&custom_mqtt_uid);
  //wifiManager.addParameter(&custom_mqtt_pwd);
  //wifiManager.addParameter(&custom_text2);

  wifiManager.autoConnect(APid.c_str(), APpw.c_str());

  //Load config upon start
  if (!SPIFFS.begin()) {
    Serial.println("Failed to mount file system");
    return;
  }

  /* Save the config back from WIFI Manager.
      This is only called after configuration
      when in AP mode
  */
  if (shouldSaveConfig) {
    //read updated parameters
    //strcpy(config_name, custom_config_name.getValue());
    // strcpy(mqtt_server, custom_mqtt_server.getValue());
    // strcpy(mqtt_port, custom_mqtt_port.getValue());
    // strcpy(mqtt_uid, custom_mqtt_uid.getValue());
    // strcpy(mqtt_pwd, custom_mqtt_pwd.getValue());
    if(String(custom_rotation.getValue()) != ""){
      strcpy(config_rotation, custom_rotation.getValue());
    }

    //Save the data
    saveConfig();
  }

  /*
     Try to load FS data configuration every time when
     booting up. If loading does not work, set the default
     positions
  */
  loadDataSuccess = loadConfig();
  if (!loadDataSuccess) {
    currentPositions[0] = 0;
    currentPositions[1] = 0;
    maxPositions[0] = 2000;
    maxPositions[1] = 2000;
  }

  /*
    Setup multi DNS (Bonjour)
    */
  if (MDNS.begin(config_name)) {
    Serial.println("MDNS responder started");
    MDNS.addService("http", "tcp", 80);
    // MDNS.addService("ws", "tcp", 81);
  } else {
    Serial.println("Error setting up MDNS responder!");
    while(1) {
      delay(1000);
    }
  }
  Serial.print("Connect to http://"+String(config_name)+".local or http://");
  Serial.println(WiFi.localIP());

  //Start HTTP server
  server.on("/", handleRoot);
  server.on("/reset", handleReset);
  server.onNotFound(handleNotFound);
  server.begin();

  //Start websocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  /* Setup connection for MQTT and for subscribed
    messages IF a server address has been entered
  */
  if (String(mqtt_server) != ""){
    Serial.println("Registering MQTT server");
    psclient.setServer(mqtt_server, String(mqtt_port).toInt());
    psclient.setCallback(mqttCallback);

  } else {
    mqttActive = false;
    Serial.println("NOTE: No MQTT server address has been registered. Only using websockets");
  }

  /* Set rotation direction of the blinds */
  extractRotationConfig();

  //Update webpage
  INDEX_HTML.replace("{VERSION}","V"+version);
  INDEX_HTML.replace("{NAME}",String(config_name));
}

void loop(void)
{

  //Websocket listner
  webSocket.loop();

  /**
    Serving the webpage
  */
  server.handleClient();

  //MQTT client
  if (mqttActive){
    helper.mqtt_reconnect(psclient, mqtt_uid, mqtt_pwd, { inputTopic.c_str() });
  }


  /**
    Storing positioning data and turns off the power to the coils
  */

  if (shouldSavePos()) {
    saveConfig();
    saveItNow[0] = 0;
    saveItNow[1] = 0;

    /*
      If no action is required by the motor make sure to
      turn off all coils to avoid overheating and less energy
      consumption
    */
    stopPowerToCoils(0);
    stopPowerToCoils(1);
  }

  /**
    Manage actions. Steering of the blind
  */
  for(int i=0; i<2; i++){
      if (actions[i] == "auto") {
          /*
             Automatically open or close blind
          */
          if (currentPositions[i] > paths[i]){
            small_steppers[i].step(ccw[i] ? -1: 1);
            currentPositions[i] = currentPositions[i] - 1;
            saveItNow[i] = -1;
          } else if (currentPositions[i] < paths[i]){
            small_steppers[i].step(ccw[i] ? 1 : -1);
            currentPositions[i] = currentPositions[i] + 1;
            saveItNow[i] = -1;
          } else {
            paths[i] = 0;
            actions[i] = "";
            sendPos(i, -1);
            Serial.println("Stopped. Reached wanted position");
            saveItNow[i] = 1;
            stopPowerToCoils(i);
          }

       } else if (actions[i] == "manual" && paths[i] != 0) {
          /*
             Manually running the blind
          */
          small_steppers[i].step(ccw[i] ? paths[i] : -paths[i]);
          currentPositions[i] = currentPositions[i] + paths[i];
          saveItNow[i] = -1;
        }
  }


  /*
     After running setup() the motor might still have
     power on some of the coils. This is making sure that
     power is off the first time loop() has been executed
     to avoid heating the stepper motor draining
     unnecessary current
  */
  if (initLoop) {
    initLoop = false;
    stopPowerToCoils(0);
    stopPowerToCoils(1);
  }
}

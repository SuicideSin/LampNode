/*
 * Author: Sam Faull
 * Details: WiFi enabled lamp
 *          ESP8266 WiFi module & WS281B RGB LEDs
 *
 * Pin allocations:
 * NA
 */

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <FastLED.h>
#include <stdio.h>
#include <string.h>
#include "config.h" // this stores the private variables such as wifi ssid and password etc.

#define DEVICE_INFO \

/* Physical connections */
#define BUTTON                    3     // button on GPIO3 (tx)
#define BUTTON_ON_OFF_TIMEOUT     2000

/* Timers */
#define INPUT_READ_TIMEOUT        50    // check for button pressed every 50ms
#define LED_UPDATE_TIMEOUT        20    // update led every 20ms
#define RAINBOW_UPDATE_TIMEOUT    30
#define CYCLE_UPDATE_TIMEOUT      40
#define TWINKLE_UPDATE_TIMEOUT    50
#define BRIGHTNESS_UPDATE_TIMEOUT 50


WiFiClient espClient;
PubSubClient client(espClient);

const char* MQTTDeviceName       = "LampNode01";
const char* MQTTDeviceInfoInbox  = "/inbox/LampNode01/deviceInfo";
const char* MQTTDeviceInfoOutbox = "/outbox/LampNode01/deviceInfo";
const char* MQTTPowerInbox       = "/inbox/LampNode01/Power";
const char* MQTTPowerOutbox      = "/outbox/LampNode01/Power";
const char* MQTTModeInbox        = "/inbox/LampNode01/Mode";
const char* MQTTModeOutbox       = "/outbox/LampNode01/ModeSelection";
const char* MQTTColorInbox       = "/inbox/LampNode01/Color";
const char* MQTTColorOutbox      = "/outbox/LampNode01/Color";
const char* MQTTBrightnessInbox  = "/inbox/LampNode01/Brightness";
const char* MQTTBrightnessOutbox = "/outbox/LampNode01/Brightness";

// Make sure to update MQTT_MAX_PACKET_SIZE in PubSubClient.h so this monstrosity fits
const char* MQTTDeviceInfo =
  "{"
    "\"deviceInfo\": {"
      "\"name\": \"LampNode01\","
      "\"endPoints\": {"
        "\"Power\": {"
          "\"title\": \"Power\","
          "\"card-type\": \"crouton-simple-toggle\","
          "\"labels\": {"
            "\"true\": \"On\","
            "\"false\": \"Off\""
          "},"
          "\"values\": {"
            "\"value\": %s"
          "}"
        "},"
        "\"Mode\": {"
          "\"title\": \"Change Mode\","
          "\"card-type\": \"crouton-simple-button\","
          "\"values\": {"
            "\"value\": true"
          "},"
          "\"icons\": {"
            "\"icon\": \"circle\""
          "}"
        "},"
        "\"ModeSelection\": {"
          "\"title\": \"Selected Mode\","
          "\"card-type\": \"crouton-simple-text\","
          "\"values\": {"
            "\"value\": \"%s\""
          "}"
        "},"
        "\"Color\": {"
          "\"card-type\": \"crouton-rgb-slider\","
          "\"min\": 0,"
          "\"max\": 255,"
          "\"values\": {"
            "\"red\": %u,"
            "\"green\": %u,"
            "\"blue\": %u"
          "}"
        "},"
        "\"Brightness\": {"
          "\"title\": \"Brightness\","
          "\"card-type\": \"crouton-simple-slider\","
          "\"min\": 0,"
          "\"max\": 255,"
          "\"values\": {"
            "\"value\": %u"
          "}"
        "}"
      "},"
      "\"description\": \"MagicLamp\","
      "\"status\": \"good\""
    "}"
  "}";

unsigned long runTime         = 0,
              ledTimer        = 0,
              brightnessTimer = 0,
              twinkleTimer    = 0,
              rainbowTimer    = 0,
              cycleTimer      = 0,
              readTempTimer   = 0,
              readInputTimer  = 0;

unsigned long buttonDownTime = 0;
unsigned long buttonUpTime = 0;
unsigned long buttonPressLength = 0;

// Flags
bool button_pressed = false; // true if a button press has been registered
bool button_released = false; // true if a button release has been registered
bool button_short_press = false;
bool target_met = false;
bool pulse_animation = false;
int pulse_addr = 0;
int brightness = 155;

bool active = false;
bool lastActive = false;
bool overheating = false;

const uint16_t PixelCount    = 16;
const uint8_t  PixelDataPin  = 0;
const uint8_t  PixelClockPin = 2;

unsigned int target_colour[3] = {0,0,0}; // rgb value that LEDs are currently set to
unsigned int current_colour[3] = {0,0,0};  // rgb value which we aim to set the LEDs to
unsigned int transition[50][3];
unsigned int pulse[30][3];

enum Modes {COLOUR, TWINKLE, RAINBOW, CYCLE};   // various modes of operation
const uint8_t ModeCount = 4;
const char* ModeStrings[ModeCount] = {"Color", "Twinkle", "Rainbow", "Cycle"};
bool standby = false;

enum Modes Mode = COLOUR;

CRGB leds[PixelCount];

CRGB genericColour(0,255,0);


void sendCurrentDeviceState() {
  char deviceInfo[1024];
  // power, mode, rgb, brightness
  sprintf(deviceInfo, MQTTDeviceInfo,
    standby ? "false" : "true",
    ModeStrings[Mode],
    current_colour[0],
    current_colour[1],
    current_colour[2],
    brightness
  );
  client.publish(MQTTDeviceInfoOutbox, deviceInfo);
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected())
  {
    Serial.print("Attempting MQTT connection... ");
    // Attempt to connect
    if (client.connect(MQTTDeviceName, MQTTuser, MQTTpassword))
    {
      Serial.println("Connected");
      // Once connected, publish device info
      sendCurrentDeviceState();
      // And subscribe to topics
      client.subscribe(MQTTDeviceInfoInbox);
      client.subscribe(MQTTPowerInbox);
      client.subscribe(MQTTModeInbox);
      client.subscribe(MQTTColorInbox);
      client.subscribe(MQTTBrightnessInbox);
    }
    else
    {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void readInputs(void)
{
  static bool button_state, last_button_state = false; // Remembers the current and previous button states

  button_state = digitalRead(BUTTON); // read button state (active high)

  if (button_state && !last_button_state) // on a rising edge we register a button press
  {
    button_pressed = true;
    button_short_press = true;  // initially assume its gonna be a short press
  }

  if (!button_state && last_button_state) // on a falling edge we register a button press
    button_released = true;

  last_button_state = button_state;
}

void applyColour(uint8_t r, uint8_t g, uint8_t b)
{
  if (r < 256 && g < 256 && b < 256)
  {
    for (uint8_t i=0; i<PixelCount; i++)
    {
      leds[i].setRGB(r,g,b);
    }
    FastLED.show();
    // Serial.print("Whole strip set to ");
    // Serial.print(r);
    // Serial.print(",");
    // Serial.print(g);
    // Serial.print(",");
    // Serial.println(b);
  }
  else
    Serial.println("Invalid RGB value, colour not set");
}

/* pass this function a pointer to an unsigned long to store the start time for the timer */
void setTimer(unsigned long *startTime)
{
  runTime = millis();    // get time running in ms
  *startTime = runTime;  // store the current time
}

/* call this function and pass it the variable which stores the timer start time and the desired expiry time
   returns true fi timer has expired */
bool timerExpired(unsigned long startTime, unsigned long expiryTime)
{
  runTime = millis(); // get time running in ms
  if ( (runTime - startTime) >= expiryTime )
    return true;
  else
    return false;
}

void setColour(int r, int g, int b) {
  current_colour[0] = r;
  current_colour[1] = g;
  current_colour[2] = b;
  if(!standby)
    applyColour(current_colour[0], current_colour[1], current_colour[2]);
}

void setColourTransition(void) {
  for (int addr = 0; addr < 50; addr++) { // for each element in the array
    for (int i = 0; i < 3; i++) { // for each colour in turn
      transition[addr][i] = map(addr, 0, 49, current_colour[i], target_colour[i]); // compute the proportional colour value
    }
  }
}

void setColourTarget(int r, int g, int b)
{
  target_met = false;

  target_colour[0] = r;
  target_colour[1] = g;
  target_colour[2] = b;

  setColourTransition();
}

void generatePulse(void)
{
  for(int addr=0; addr<30; addr++)  // for each element in the array
  {
    for (int i=0; i<3; i++)  // for each colour in turn
    {
      pulse[addr][i] = map(addr, 0, 29, current_colour[i], current_colour[i]/5); // compute the proportional colour value
    }
    /*
    Serial.print(pulse[addr][0]);
    Serial.print(",");
    Serial.print(pulse[addr][1]);
    Serial.print(",");
    Serial.println(pulse[addr][2]);
    */
  }
}

// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
void Wheel(byte WheelPos, int *r, int *g, int *b)
{
  WheelPos = 255 - WheelPos;
  if(WheelPos < 85)
  {
   *r = 255 - WheelPos * 3;
   *g = 0;
   *b = WheelPos * 3;
  }
  else if(WheelPos < 170)
  {
    WheelPos -= 85;
   *r = 0;
   *g = WheelPos * 3;
   *b = 255 - WheelPos * 3;
  }
  else
  {
   WheelPos -= 170;
   *r = WheelPos * 3;
   *g = 255 - WheelPos * 3;
   *b = 0;
  }
}

void rainbow(void)
{
  // here we need to cycle through each led, assigning consectuve colours pulled from the Wheel function. Each time this is called all colours should shift one
  static int offset = 0;
  static int stepVal = 256/PixelCount;  // note the 256 value can be reduced to show less of the colour spectrum at once.
  int red, green, blue;

  for (int i=0; i<PixelCount; i++)
  {
    Wheel(i*stepVal+offset, &red, &green, &blue); // get our colour
    leds[i].setRGB(red, green, blue);
  }
  FastLED.show();

  if (offset >= 255)
    offset = 0;
  else
    offset++;
}

bool coinFlip(void)
{
  int coin = random(2) - 1;
  if (coin)
    return true;
  else
    return false;
}

void setTheMode(Modes temp) {
  Serial.print("mode set to: ");
  Serial.println(temp);

  switch(temp) {
    case COLOUR:
      if(!standby)
        setColourTarget(target_colour[0],target_colour[1],target_colour[2]);
    break;

    case TWINKLE:
    case RAINBOW:
    case CYCLE:
      setColour(0,0,0);
    break;

    default:
      Serial.print("UNKNOWN MODE: ");
      Serial.println(temp);
    break;
  }

  Mode = temp;

  char modeUpdate[20];
  sprintf(modeUpdate, "{\"value\":\"%s\"}", ModeStrings[Mode]);
  client.publish(MQTTModeOutbox, modeUpdate);
}

void nextMode() {
  setTheMode((Modes)((Mode + 1) % ModeCount));
}

void setStandby(bool state) {
  if (state) {
    applyColour(0,0,0);
    client.publish(MQTTPowerOutbox, "{\"value\":false}");
  } else {
    setColourTarget(target_colour[0],target_colour[1],target_colour[2]);
    client.publish(MQTTPowerOutbox, "{\"value\":true}");
  }

  standby = state;
}

// doesnt work very well
byte rgb2wheel(int R, int G, int B)
{
 return  (B & 0xE0) | ((G & 0xE0)>>3) | (R >> 6);
}

void twinkle()
{
  // here we need to cycle through each led, assigning consectuve colours pulled from the Wheel function. Each time this is called all colours should shift one
  int red, green, blue;
  int offset = random(30) - 15;
  int pix = random(PixelCount);
  int state = coinFlip();
  int val = rgb2wheel(target_colour[0],target_colour[1],target_colour[2]);
  Wheel(val+offset, &red, &green, &blue); // get our colour

  if(state)
    leds[pix].setRGB(red, green, blue);
  else
    leds[pix].setRGB(0, 0, 0);

  FastLED.show();
}

void applyBrightness(uint8_t value) {
  FastLED.setBrightness(value);
}

void set_brightness(void)
{
  static int last_brightness = 153;
  static int brightness_pulse = 153;
  static float coefficient = 1.0;
  static bool pulse_direction = 0;

  if (pulse_animation)
  {
    brightness_pulse = brightness * coefficient;

    Serial.print("pulse animation: ");
    Serial.println(brightness_pulse);

    applyBrightness(brightness_pulse);

    if (coefficient>=1.0)
      pulse_direction = 0;
    if (coefficient<=0.5)
      pulse_direction = 1;

    if (pulse_direction)
      coefficient+=0.025;
    else
      coefficient-=0.025;

    if (Mode == COLOUR) {
      FastLED.show();
    }
  }
  else
  {
    if(coefficient < 1.0)
    {
      coefficient+=0.025;
      brightness_pulse = brightness * coefficient;
      applyBrightness(brightness_pulse);
    }
    else
    {
      if (last_brightness!=brightness)
      {
        applyBrightness(brightness);
        if (Mode == COLOUR)
        {
          FastLED.show();
        }
        last_brightness = brightness;
      }
    }
  }
}

void fadeToColourTarget(void)
{
  static int addr = 0;

  if(!target_met)
  {
    setColour(transition[addr][0],transition[addr][1],transition[addr][2]);
    addr++;

    if (addr>=50)
    {
      target_met = true;
      addr = 0;
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length)
{
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");

  /* --------------- Print incoming message to serial ------------------ */
  char input[length+1];
  for (int i = 0; i < length; i++)
    input[i] = (char)payload[i];  // store payload as char array
  input[length] = '\0'; // dont forget to add a termination character

  Serial.println(input);

  if (strcmp(topic, MQTTDeviceInfoInbox) == 0) {
    Serial.println("Broadcasting device info");
    sendCurrentDeviceState();
  } else if (strcmp(topic, MQTTPowerInbox) == 0) {
    if (strstr(input,"true") != NULL) {
      setStandby(false);
      Serial.println("ON");
    } else if (strstr(input,"false") != NULL) {
      setStandby(true);
      Serial.println("OFF");
    }
  } else if (strcmp(topic, MQTTModeInbox) == 0) {
    nextMode();
  } else if (strcmp(topic, MQTTColorInbox) == 0) {
    // update looks like {"blue":65}
    unsigned int tempColor;
    char * command = strtok(input, "{\":");
    Serial.print("COLOR: ");
    Serial.print(command);
    if (command != NULL) {
      tempColor = atoi(strtok(NULL, "{\":"));
      Serial.print(", VALUE: ");
      Serial.println(tempColor);
    }
    if (strcmp(command, "red") == 0) {
      setColourTarget(tempColor, target_colour[1], target_colour[2]);
    } else if (strcmp(command, "green") == 0) {
      setColourTarget(target_colour[0], tempColor, target_colour[2]);
    } else if (strcmp(command, "blue") == 0) {
      setColourTarget(target_colour[0], target_colour[1], tempColor);
    }
    /* setColourTarget(temp[0],temp[1],temp[2]); */
  } else if (strcmp(topic, MQTTBrightnessInbox) == 0) {
    char * command = strtok(input, "{\":");
    if (command != NULL) {
      int brightness_temp = atoi(strtok(NULL, "{\":"));
      Serial.print("New Brightness: ");
      Serial.print(brightness_temp);
      if (brightness_temp >= 0 || brightness_temp < 256) {
        brightness = brightness_temp;
        sprintf(command, "{\"value\":%u}", brightness);
        client.publish(MQTTBrightnessOutbox, command);
      }
    }
  }
}

void handleInterrupt() {
  if (digitalRead(BUTTON) == HIGH) {
    buttonDownTime = millis();
  } else {
    buttonUpTime = millis();
  }
}

void setup()
{
  /* Setup I/O */
  pinMode(BUTTON, INPUT_PULLUP);  // Enables the internal pull-up resistor
  attachInterrupt(digitalPinToInterrupt(BUTTON), handleInterrupt, CHANGE);

  /* Setup serial */
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  Serial.flush();

  /* Start timers */
  setTimer(&readInputTimer);
  setTimer(&readTempTimer);
  setTimer(&ledTimer);
  setTimer(&brightnessTimer);
  setTimer(&twinkleTimer);
  setTimer(&rainbowTimer);
  setTimer(&cycleTimer);

  /* Start FastLED */
  FastLED.addLeds<LPD8806, PixelDataPin, PixelClockPin, GRB>(leds, PixelCount);
  FastLED.show();

  /* Setup MQTT */
  // Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;
  wifiManager.autoConnect(MQTTDeviceName);

  client.setServer(MQTTserver, MQTTport);
  client.setCallback(callback);
}

int cnt = 0;
void loop()
{
  /* Check WiFi Connection */
  if (!client.connected())
    reconnect();
  client.loop();

  unsigned long now = millis();  // get elapsed time

  if (!standby)
  {
    // Periodically update the brightness
    if(timerExpired(brightnessTimer, BRIGHTNESS_UPDATE_TIMEOUT))
    {
      setTimer(&brightnessTimer); // reset timer
      set_brightness();
    }

    switch (Mode)
    {
      case COLOUR:
        /* Periodically update the LEDs */
        if(timerExpired(ledTimer, LED_UPDATE_TIMEOUT))
        {
          setTimer(&ledTimer); // reset timer
          fadeToColourTarget();
        }

      break;

      case TWINKLE:
        if(timerExpired(twinkleTimer, TWINKLE_UPDATE_TIMEOUT))
        {
          setTimer(&twinkleTimer); // reset timer
          twinkle();
        }
      break;

      case RAINBOW:
        if(timerExpired(rainbowTimer, RAINBOW_UPDATE_TIMEOUT))
        {
          setTimer(&rainbowTimer); // reset timer
          rainbow();
        }
      break;

      case CYCLE:
        if(timerExpired(cycleTimer, CYCLE_UPDATE_TIMEOUT))
        {
          setTimer(&cycleTimer); // reset timer
          int r, g, b;
          Wheel(cnt++, &r, &g, &b);
          setColour(r,g,b);
          if (cnt>=256)
            cnt=0;
        }
      break;

      default:
        // unknown state - do nothing
      break;
    }
  }

  if (buttonDownTime > 0 && buttonUpTime == 0 && (now - buttonDownTime) > BUTTON_ON_OFF_TIMEOUT) {
    Serial.print("Set Standby: ");
    Serial.println(!standby);
    setStandby(!standby); // change the standby state
    buttonDownTime = 0;
  }
  if (buttonDownTime == 0 && buttonUpTime > 0) {
    // release after shutdown
    buttonUpTime = 0;
  }
  if (buttonDownTime > 0 && buttonUpTime > 0) {
    if (!standby) {
      nextMode();
    }
    buttonDownTime = buttonUpTime = 0;
  }
}

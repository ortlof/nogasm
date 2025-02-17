// Jul 2022 - Nogasm Code Ortlof
/* Drives a vibrator and uses changes in pressure of an inflatable buttplug
 * to estimate a user's closeness to orgasm, and turn off the vibrator
 * before that point.
 * A state machine updating at 60Hz creates different modes and option menus
 * that can be identified by the color of the LEDs, especially the RGB LED
 * in the central button/encoder knob.
 * 
 * [Red]    Manual Vibrator Control
 * [Blue]   Automatic vibrator edging, knob adjusts orgasm detection sensitivity
 * [Green]  Automatic vibrator edging with Pause Ramp up 0.25 sek at ever pause, knob adjusts orgasm detection sensitivity
 * Menue Colors
 * [Green]  Setting menu for maximum vibrator speed in automatic mode
 * [Blue]   Settings menu for Pause Time 0-39 sek.
 * [Red]    Settings menu for Ramp Time 0-78 sek. 
 * [White]  Debubbing menu to show data from the pressure sensor ADC
 * [Off]    While still plugged in, holding the button down for >3 seconds turns
 *          the whole device off, until the button is pressed again.
 * 
 * Settings like edging sensitivity, or maximum motor speed are stored in EEPROM,
 * so they are saved through power-cycling.
 * 
 * In the automatic edging mode, the vibrator speed will linearly ramp up to full
 * speed (set in the green menu) over 30 seconds. If a near-orgasm is detected,
 * the vibrator abruptly turns off for 15 seconds, then begins ramping up again.
 * 
 * The motor will beep during power on/off, and if the plug pressure rises above
 * the maximum the board can read - this condition could lead to a missed orgasm 
 * if unchecked. The analog gain for the sensor is adjustable via a trimpot to
 * accomidate different types of plugs that have higher/lower resting pressures.
 * 
 * Motor speed, current pressure, and average pressure are reported via USB serial
 * at 115200 baud. Timestamps can also be enabled, from the main loop.
 * 
 * There is some framework for more features like an adjustable "cool off" time 
 * other than the default 15 seconds, and options for LED brightness and enabling/
 * disabling beeps. Four DIP switches are included on the board to allow users to
 * change other software settings without reflashing code. One example use would 
 * be switch 1 toggling between the defaul ramping motor behavior, and a strict
 * ON/OFF output to the motor that could instead be used to toggle a relay for 
 * driving other toys.
 * 
 * Note - Do not set all 13 LEDs to white at full brightness at once 
 * (RGB 255,255,255) It may overheat the voltage regulator and cause the board 
 * to reset.
 */
//=======Libraries===============================
#include <Encoder.h>
#include <EEPROM.h>
#include "FastLED.h"
#include "RunningAverage.h"
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

//=======Hardware Setup===============================
//LEDs
#define NUM_LEDS 13
#define LED_PIN 17 //5V buffered pin on Teensy LC, single wire data out to WS8212Bs
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB

//LCD
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
#define OLED_RESET     4 // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

//Encoder
#define REDPIN   5 //RGB pins of the encoder LED
#define GREENPIN 4
#define BLUEPIN  3
#define ENC_SW   6 //Pushbutton on the encoder
Encoder myEnc(8, 7); //Quadrature inputs on pins 7,8

//DIP Switches
#define SW1PIN 12 //Dip switch pins, for setting software options without reflashing code
#define SW2PIN 11
#define SW3PIN 10
#define SW4PIN 9

//Motor
#define MOTPIN 23

//Pressure Sensor Analog In
#define BUTTPIN 15

//=======Software/Timing options=====================
#define FREQUENCY 60 //Update frequency in Hz
#define LONG_PRESS_MS 600 //ms requirements for a long press, to move to option menus
#define V_LONG_PRESS_MS 2500 //ms for a very long press, which turns the device off

//Update/render period
#define period (1000/FREQUENCY)
#define lcdperiod (1000/FREQUENCY)
#define longBtnCount (LONG_PRESS_MS / period)

//Running pressure average array length and update frequency
#define RA_HIST_SECONDS 25
#define RA_FREQUENCY 6
#define RA_TICK_PERIOD (FREQUENCY / RA_FREQUENCY)
RunningAverage raPressure(RA_FREQUENCY*RA_HIST_SECONDS);
int sensitivity = 0; //orgasm detection sensitivity, persists through different states

//=======State Machine Modes=========================
#define MANUAL      1
#define AUTO        2
#define AUTORAMP    3
#define OPT_SPEED   4
#define OPT_PTIME   5
#define OPT_RTIME   6
#define OPT_BRI     7
#define OPT_PRES    8

//Button states - no press, short press, long press
#define BTN_NONE   0
#define BTN_SHORT  1
#define BTN_LONG   2
#define BTN_V_LONG 3


uint8_t state = MANUAL;
//=======Global Settings=============================
//DIP switch options:
uint8_t MOT_MAX =   179; //By default, motor speed only ever reaches 179. Alternative is 255
bool SW2 =        false;
bool SW3 =        false;
bool SW4 =        false;
#define MOT_MIN 35  // Motor PWM minimum.  It needs a little more than this to start.
#define DEFAULT_RTIME 20 // Default RampTime Value is safed in Eproom
#define DEFAULT_PTIME 10 // Default Pause Time Value is safed in Eproom
#define DEFAULT_BRIGHTNESS 255 //Subject to change, limits current that the LEDs draw

CRGB leds[NUM_LEDS];

int pressure = 0;
int avgPressure = 0; //Running 25 second average pressure
int bri = DEFAULT_BRIGHTNESS; //Brightness setting
int rampTimeS = DEFAULT_RTIME; //Ramp-up time, in seconds
int rampPTime = DEFAULT_PTIME; //Pause time, in seconds
float autoRTime;
bool pflag;

#define DEFAULT_PLIMIT 600
int pLimit = DEFAULT_PLIMIT; //Limit in change of pressure before the vibrator turns off
int maxSpeed = 255; //maximum speed the motor will ramp up to in automatic mode
float motSpeed = MOT_MIN; //Motor speed, 0-255 (float to maintain smooth ramping to low speeds)
int pcounter = 0;
unsigned long Time;

//=======EEPROM Addresses============================
//128b available on teensy LC
#define BEEP_ADDR         1
#define MAX_SPEED_ADDR    2
#define SENSITIVITY_ADDR  3
#define RAMPPAUSET_ADDR   4 
#define RAMPTIME_ADDR     5
#define BRIGHTNESS_ADDR   6

//=======Setup=======================================
//Beep out tones over the motor by frequency (1047,1396,2093) may work well
void beep_motor(int f1, int f2, int f3){
  if(motSpeed > 245) analogWrite(MOTPIN, 245); //make sure the frequency is audible
  else if(motSpeed < 10) analogWrite(MOTPIN, 10);
  analogWriteFrequency(MOTPIN, f1);
  delay(250);
  analogWriteFrequency(MOTPIN, f2);
  delay(250);
  analogWriteFrequency(MOTPIN, f3);
  delay(250);
  analogWriteFrequency(MOTPIN, 440);
  analogWrite(MOTPIN,motSpeed);
}

void setup() {
   if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); // Don't proceed, loop forever
  }

  pinMode(REDPIN,   OUTPUT); //Connected to RGB LED in the encoder
  pinMode(GREENPIN, OUTPUT);
  pinMode(BLUEPIN,  OUTPUT);
  pinMode(ENC_SW,   INPUT); //Pin to read quadrature pulses from encoder

  pinMode(SW1PIN,   INPUT); //Set DIP switch pins as inputs
  pinMode(SW2PIN,   INPUT);
  pinMode(SW3PIN,   INPUT);
  pinMode(SW4PIN,   INPUT);

  digitalWrite(SW1PIN, HIGH); //Enable pullup resistors on DIP switch pins.
  digitalWrite(SW2PIN, HIGH); //They are tied to GND when switched on.
  digitalWrite(SW3PIN, HIGH);
  digitalWrite(SW4PIN, HIGH);

  pinMode(MOTPIN,OUTPUT); //Enable "analog" out (PWM)
  
  pinMode(BUTTPIN,INPUT); //default is 10 bit resolution (1024), 0-3.3
  analogReadRes(12); //changing ADC resolution to 12 bits (4095)
  analogReadAveraging(32); //To reduce noise, average 32 samples each read.
  
  raPressure.clear(); //Initialize a running pressure average

  digitalWrite(MOTPIN, LOW);//Make sure the motor is off

  delay(3000); // 3 second delay for recovery

  //If a pin reads low, the switch is enabled. Here, we read in the DIP settings
  //Right now, only SW1 is used, for enabling higher maximum motor speed.
  if(digitalRead(SW1PIN)){
    MOT_MAX = 179; //At the default low position, limit the motor speed
  }
  else{
    MOT_MAX = 255; //When SW1 is flipped high, allow higher motor speeds
  }
  SW2 = (digitalRead(SW2PIN) == LOW);
  SW3 = (digitalRead(SW3PIN) == LOW);
  SW4 = (digitalRead(SW4PIN) == LOW);

  Serial.begin(115200);
  
  //Recall saved settings from memory
  sensitivity = EEPROM.read(SENSITIVITY_ADDR);
  maxSpeed = min(EEPROM.read(MAX_SPEED_ADDR),MOT_MAX); //Obey the MOT_MAX the first power  cycle after chaning it.
  rampPTime = EEPROM.read(RAMPPAUSET_ADDR);
  rampTimeS = EEPROM.read(RAMPTIME_ADDR);
  bri = EEPROM.read(BRIGHTNESS_ADDR);

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  // limit power draw to .6A at 5v... Didn't seem to work in my FastLED version though
  //FastLED.setMaxPowerInVoltsAndMilliamps(5,DEFAULT_PLIMIT);
  FastLED.setBrightness(bri);


  beep_motor(1047,1396,2093); //Power on beep
  display.clearDisplay();
  display.stopscroll();
  display.display();
}

//=======LED Drawing Functions=================

void showKnobRGB(const CRGB& rgb)
{
  analogWrite(REDPIN,   rgb.r );
  analogWrite(GREENPIN, rgb.g );
  analogWrite(BLUEPIN,  rgb.b );
}

//Draw a "cursor", one pixel representing either a pressure or encoder position value
//C1,C2,C3 are colors for each of 3 revolutions over the 13 LEDs (39 values)
void draw_cursor_3(int pos,CRGB C1, CRGB C2, CRGB C3){
  pos = constrain(pos,0,NUM_LEDS*3-1);
  int colorNum = pos/NUM_LEDS; //revolution number
  int cursorPos = pos % NUM_LEDS; //place on circle, from 0-12
  switch(colorNum){
    case 0:
      leds[cursorPos] = C1;
      break;
    case 1:
      leds[cursorPos] = C2;
      break;
    case 2:
      leds[cursorPos] = C3;
      break;
  }
}

//Draw a "cursor", one pixel representing either a pressure or encoder position value
void draw_cursor(int pos,CRGB C1){
  pos = constrain(pos,0,NUM_LEDS-1);
  leds[pos] = C1;
}

//Draw 3 revolutions of bars around the LEDs. From 0-39, 3 colors
void draw_bars_3(int pos,CRGB C1, CRGB C2, CRGB C3){
  pos = constrain(pos,0,NUM_LEDS*3-1);
  int colorNum = pos/NUM_LEDS; //revolution number
  int barPos = pos % NUM_LEDS; //place on circle, from 0-12
  switch(colorNum){
    case 0:
      fill_gradient_RGB(leds,0,C1,barPos,C1);
      //leds[barPos] = C1;
      break;
    case 1:
      fill_gradient_RGB(leds,0,C1,barPos,C2);
      break;
    case 2:
      fill_gradient_RGB(leds,0,C2,barPos,C3);
      break;
  }
}

//Provide a limited encoder reading corresponting to tacticle clicks on the knob.
//Each click passes through 4 encoder pulses. This reduces it to 1 pulse per click
int encLimitRead(int minVal, int maxVal){
  if(myEnc.read()>maxVal*4)myEnc.write(maxVal*4);
  else if(myEnc.read()<minVal*4) myEnc.write(minVal*4);
  return constrain(myEnc.read()/4,minVal,maxVal);
}

//=======LCD Programs=========================

void lcd_auto(int presDraw, String mode){
  int motspeedp = map(motSpeed, 0, 255, 0, 100);
  display.clearDisplay();
  display.setCursor(0,0);
  display.setTextColor(WHITE); 
  display.setTextSize(1);
  display.print("Mode:");
  display.setCursor(90,0);
  display.println(mode);
  display.setCursor(0,10);
  display.print("Pressure:");
  display.setCursor(90,10);
  display.println(presDraw);
  display.setCursor(0,20);
  display.print("Vibr_Speed:");
  display.setCursor(90,20);
  display.println(motspeedp);
  display.setCursor(110,20);
  display.print("%");
  display.setCursor(0,30);
  display.print("Press Limit:");
  display.setCursor(90,30);
  display.print(pLimit);
  display.setCursor(0,40);
  display.print("Edge Counter:");
  display.setCursor(90,40);
  display.print(pcounter);
}

void lcd_settings(String setting, String tag1, int value1){
  display.clearDisplay();
  display.setCursor(0,0);
  display.setTextColor(WHITE); 
  display.setTextSize(1);
  display.print("Settings");
  display.setCursor(0,10);
  display.println(setting);
  display.setCursor(0,30);
  display.print(tag1);
  display.setCursor(90,30);
  display.println(value1);
}
//=======Program Modes/States==================

// Manual vibrator control mode (red), still shows orgasm closeness in background
void run_manual() {
  //In manual mode, only allow for 36 cursor positions, for adjusting motor speed.
  int knob = encLimitRead(0,36);
  motSpeed = map(knob, 0, 36, (float)(MOT_MIN-5), (float)MOT_MAX);
    if (motSpeed > (MOT_MIN)) {
    analogWrite(MOTPIN, (int) motSpeed);
  } else {
    analogWrite(MOTPIN, 0);
  }

  //gyrGraphDraw(avgPressure, 0, 4 * 3 * NUM_LEDS);
  int presDraw = map(constrain(pressure - avgPressure, 0, pLimit),0,pLimit,0,NUM_LEDS*3);
  draw_bars_3(presDraw, CRGB::Green,CRGB::Yellow,CRGB::Red);
  draw_cursor_3(knob, CRGB::Red,CRGB::Purple,CRGB::Blue);  // Changed to 37 motor positions and 3 color 
  int motspeedp = map(motSpeed, 0, 255, 0, 100);
  display.clearDisplay();
  display.setCursor(0,0);
  display.setTextColor(WHITE); 
  display.setTextSize(1);
  display.print("Mode: ");
  display.setCursor(90,0);
  display.println("Manual");
  display.setCursor(0,10);
  display.print("Pressure: ");
  display.setCursor(90,10);
  display.println(presDraw);
  display.setCursor(0,20);
  display.print("Vibr_Speed: ");
  display.setCursor(90,20);
  display.println(motspeedp);
  display.setCursor(110,20);
  display.print("%");
}

// Automatic edging mode, knob adjust sensitivity.
void run_auto() {
  static float motIncrement = 0.0;
  motIncrement = ((float)maxSpeed / ((float)FREQUENCY * (float)rampTimeS));

  int knob = encLimitRead(0,(3*NUM_LEDS)-1);
  sensitivity = knob*4; //Save the setting if we leave and return to this state
  //Reverse "Knob" to map it onto a pressure limit, so that it effectively adjusts sensitivity
  pLimit = map(knob, 0, 3 * (NUM_LEDS - 1), 600, 1); //set the limit of delta pressure before the vibrator turns off
  //When someone clenches harder than the pressure limit
  if (pressure - avgPressure > pLimit) {
    motSpeed = -(float)rampPTime*((float)FREQUENCY*motIncrement)+MOT_MIN;//Stay off for Pause Time
    if (pflag == false) {
      pcounter = pcounter + 1;
      pflag = true;
    }
  }
  else if (motSpeed < (float)maxSpeed) {
    motSpeed += motIncrement;
  }
  if (motSpeed > (MOT_MIN+1)) {
    analogWrite(MOTPIN, (int) motSpeed);
    pflag = false;
  } else {
    analogWrite(MOTPIN, 0);
  }

  int presDraw = map(constrain(pressure - avgPressure, 0, pLimit),0,pLimit,0,NUM_LEDS*3);
  draw_bars_3(presDraw, CRGB::Green,CRGB::Yellow,CRGB::Red);
  draw_cursor_3(knob, CRGB(50,50,200),CRGB::Blue,CRGB::Purple);
  String mode = "Auto";
  lcd_auto(presDraw, mode);
}
// Automatic edging mode, knob adjust sensitivity. With Ramping Pause time. 
void run_auto_ramp() {
  static float motIncrement = 0.0;
  motIncrement = ((float)maxSpeed / ((float)FREQUENCY * (float)rampTimeS));

  int knob = encLimitRead(0,(3*NUM_LEDS)-1);
  sensitivity = knob*4; //Save the setting if we leave and return to this state
  //Reverse "Knob" to map it onto a pressure limit, so that it effectively adjusts sensitivity
  pLimit = map(knob, 0, 3 * (NUM_LEDS - 1), 600, 1); //set the limit of delta pressure before the vibrator turns off
  //When someone clenches harder than the pressure limit
  if (pressure - avgPressure > pLimit) {
    motSpeed = -(float)autoRTime*((float)FREQUENCY*motIncrement)+MOT_MIN;//Stay off for Pause Time
    if (pflag == false) {
      autoRTime = autoRTime + 0.25;
      pcounter = pcounter + 1;
      pflag = true;
    }
    
  }
  else if (motSpeed < (float)maxSpeed) {
    motSpeed += motIncrement;
  }
  if (motSpeed > (MOT_MIN+1)) {
    analogWrite(MOTPIN, (int) motSpeed);
    pflag = false;
  } else {
    analogWrite(MOTPIN, 0);
  }

  int presDraw = map(constrain(pressure - avgPressure, 0, pLimit),0,pLimit,0,NUM_LEDS*3);
  draw_bars_3(presDraw, CRGB::Green,CRGB::Yellow,CRGB::Red);
  draw_cursor_3(knob, CRGB(50,50,200),CRGB::Blue,CRGB::Purple);
  String mode = "Ramp";
  lcd_auto(presDraw, mode);
}

//Setting menu for adjusting the maximum vibrator speed automatic mode will ramp up to
void run_opt_speed() {
  //Serial.println("speed settings");
  int knob = encLimitRead(0,12);
  motSpeed = map(knob, 0, 12, 0., (float)MOT_MAX);
  analogWrite(MOTPIN, motSpeed);
  maxSpeed = motSpeed; //Set the maximum ramp-up speed in automatic mode
  //Little animation to show ramping up on the LEDs
  static int visRamp = 0;
  if(visRamp <= FREQUENCY*NUM_LEDS-1) visRamp += 16;
  else visRamp = 0;
  draw_bars_3(map(visRamp,0,(NUM_LEDS-1)*FREQUENCY,0,knob),CRGB::Green,CRGB::Green,CRGB::Green);
  String setting = ("Speed");
  String tag1 = ("Max Speed:");
  int value1 = motSpeed;
  lcd_settings(setting, tag1, value1);
}

//Settings menu for Pause Time. IT is set in 1 seconds per led so 39 Seconds MAX
void run_opt_ptime() {
 // Serial.println("PauseTime");
 // Serial.println(rampPTime);
  int knob = encLimitRead(0,39);
  rampPTime = map(knob, 0, 39, 0, 39);
  draw_cursor_3(knob, CRGB::Red,CRGB::Blue,CRGB::Purple);
  String setting = ("Coldown Time Min");
  String tag1 = ("Seconds:");
  int value1 = rampPTime;
  lcd_settings(setting, tag1, value1);
}

//Settings menu for Ramp Time. IT is set in 2 seconds per led so 78 Seconds MAX
void run_opt_rtime() { 
 // Serial.println("RampTime");
 // Serial.println(rampTimeS);
  int knob = encLimitRead(0,39);
  rampTimeS = map(knob, 0, 39, 0, 78);
  draw_cursor_3(knob, CRGB::Red,CRGB::Blue,CRGB::Purple);
  String setting = ("Ramp Time");
  String tag1 = ("Seconds:");
  int value1 = rampTimeS;
  lcd_settings(setting, tag1, value1);
}

//Settings menu for Brightness 
void run_opt_bright() { 
  //Serial.println("Brightness");
  //Serial.println(bri);
  int knob = encLimitRead(0,39);
  bri = map(knob, 0, 39, 0, 255);
  FastLED.setBrightness(bri);
  draw_cursor(6, CRGB::White);
  String setting = ("Brightness");
  String tag1 = ("Brightness:");
  int value1 = bri;
  lcd_settings(setting, tag1, value1);
}

//Simply display the pressure analog voltage. Useful for debugging sensitivity issues.
void run_opt_pres() {
  int p = map(analogRead(BUTTPIN),0,4095,0,NUM_LEDS-1);
  draw_cursor(p,CRGB::White);
  String setting = ("Pressure Debug");
  String tag1 = ("Pressure:");
  int value1 = p;
  lcd_settings(setting, tag1, value1);
}

//Poll the knob click button, and check for long/very long presses as well
uint8_t check_button(){
  static bool lastBtn = LOW;
  static unsigned long keyDownTime = 0;
  uint8_t btnState = BTN_NONE;
  bool thisBtn = digitalRead(ENC_SW);

  //Detect single presses, no repeating, on keyup
  if(thisBtn == HIGH && lastBtn == LOW){
    keyDownTime = millis();
  }
  
  if (thisBtn == LOW && lastBtn == HIGH) { //there was a keyup
    if((millis()-keyDownTime) >= V_LONG_PRESS_MS){
      btnState = BTN_V_LONG;
    }
    else if((millis()-keyDownTime) >= LONG_PRESS_MS){
      btnState = BTN_LONG;
      }
    else{
      btnState = BTN_SHORT;
      }
    }

  lastBtn = thisBtn;
  return btnState;
}

//run the important/unique parts of each state. Also, set button LED color.
void run_state_machine(uint8_t state){
  switch (state) {
      case MANUAL:
        showKnobRGB(CRGB::Red);
        run_manual();
        break;
      case AUTO:
        showKnobRGB(CRGB::Blue);
        run_auto();
        break;
      case AUTORAMP:
        showKnobRGB(CRGB::Green);
        run_auto_ramp();
        break;
      case OPT_SPEED:
        showKnobRGB(CRGB::Green);
        run_opt_speed();
        break;
      case OPT_PTIME:
        showKnobRGB(CRGB::Blue);
        run_opt_ptime();
        break;
      case OPT_RTIME:
        showKnobRGB(CRGB::Red);
        run_opt_rtime();
        break;
      case OPT_BRI:
        showKnobRGB(CRGB::Violet);
        run_opt_bright();
        break;
      case OPT_PRES:
        showKnobRGB(CRGB::White);
        run_opt_pres();
        break;
      default:
        run_manual();
        break;
    }
}

//Switch between state machine states, and reset the encoder position as necessary
//Returns the next state to run. Very long presses will turn the system off (sort of)
uint8_t set_state(uint8_t btnState, uint8_t state){
  if(btnState == BTN_NONE){
    return state;
  }
  if(btnState == BTN_V_LONG){
    //Turn the device off until woken up by the button
    Serial.println("power off");
    fill_gradient_RGB(leds,0,CRGB::Black,NUM_LEDS-1,CRGB::Black);//Turn off LEDS
    FastLED.show();
    showKnobRGB(CRGB::Black);
    analogWrite(MOTPIN, 0);
    beep_motor(2093,1396,1047);
    analogWrite(MOTPIN, 0); //Turn Motor off
    while(!digitalRead(ENC_SW))delay(1);
    beep_motor(1047,1396,2093);
    return MANUAL ;
  }
  else if(btnState == BTN_SHORT){
    switch(state){
      case MANUAL:
        myEnc.write(sensitivity);//Whenever going into auto mode, keep the last sensitivity
        motSpeed = MOT_MIN; //Also reset the motor speed to 0
        pcounter = 0; //When going in auto Restet Pause Counter.
        return AUTO;
      case AUTO:
        myEnc.write(sensitivity);//Whenever going into autoramp mode, keep the last sensitivity
        motSpeed = MOT_MIN; //Also reset the motor speed to 0
        autoRTime = rampPTime; //When going in auto Restet Pause Counter.
        pcounter = 0;
      return AUTORAMP;
      case AUTORAMP:
        myEnc.write(0);//Whenever going into manual mode, set the speed to 0.
        motSpeed = 0;
        EEPROM.update(SENSITIVITY_ADDR, sensitivity);
        return MANUAL;
      case OPT_SPEED:
        myEnc.write(rampPTime);
        EEPROM.update(MAX_SPEED_ADDR, maxSpeed);
        motSpeed = 0;
        analogWrite(MOTPIN, motSpeed); //Turn the motor off for the white pressure monitoring mode
        return OPT_PTIME;
      case OPT_PTIME:
        EEPROM.update(RAMPPAUSET_ADDR, rampPTime);
        myEnc.write(map(rampTimeS, 0, 39, 0, (78*4)));
      return OPT_RTIME;
      case OPT_RTIME:
        EEPROM.update(RAMPTIME_ADDR, rampTimeS);
        myEnc.write(map(bri, 0, 39, 0, (255*4)));
        return OPT_BRI;
      case OPT_BRI:
        EEPROM.update(BRIGHTNESS_ADDR, bri);
        FastLED.setBrightness(bri);
        myEnc.write(0);
        return OPT_PRES;
      case OPT_PRES:
        myEnc.write(map(maxSpeed,0,255,0,4*(NUM_LEDS)));//start at saved value
        return OPT_SPEED;
    }
  }
  else if(btnState == BTN_LONG){
    switch (state) {
          case MANUAL:
            myEnc.write(map(maxSpeed,0,255,0,4*(NUM_LEDS)));//start at saved value
            return OPT_SPEED;
          case AUTO:
            myEnc.write(map(maxSpeed,0,255,0,4*(NUM_LEDS)));//start at saved value
            return OPT_SPEED;
          case AUTORAMP:
            myEnc.write(map(maxSpeed,0,255,0,4*(NUM_LEDS)));//start at saved value
            return OPT_SPEED;
          case OPT_SPEED:
            EEPROM.update(MAX_SPEED_ADDR, maxSpeed);
            myEnc.write(0);
            return MANUAL;
          case OPT_PTIME:
            EEPROM.update(RAMPPAUSET_ADDR, rampPTime);
            myEnc.write(0);
            return MANUAL;
          case OPT_RTIME:
            EEPROM.update(RAMPTIME_ADDR, rampTimeS);
            myEnc.write(0);
            return MANUAL;
          case OPT_BRI:
            EEPROM.update(BRIGHTNESS_ADDR, bri);
            myEnc.write(0);
            return MANUAL;
          case OPT_PRES:
            myEnc.write(0);
            return MANUAL;
        }
  } else return MANUAL;
}

//=======Main Loop=============================
void loop() {
  static uint8_t state = MANUAL;
  static int sampleTick = 0; //Run this section at the update frequency (default 60 Hz)
  if (millis() % period == 0) {
    delay(1);

    display.stopscroll();
    display.display();
    sampleTick++; //Add pressure samples to the running average slower than 60Hz
    if (sampleTick % RA_TICK_PERIOD == 0) {
      raPressure.addValue(pressure);
      avgPressure = raPressure.getAverage();
    }
    
    pressure = analogRead(BUTTPIN);
    fadeToBlackBy(leds,NUM_LEDS,20); //Create a fading light effect. LED buffer is not otherwise cleared
    uint8_t btnState = check_button();
    state = set_state(btnState,state); //Set the next state based on this state and button presses
    run_state_machine(state);
    FastLED.show(); //Update the physical LEDs to match the buffer in software

    //Alert that the Pressure voltage amplifier is railing, and the trim pot needs to be adjusted
    if(pressure > 4030)beep_motor(2093,2093,2093); //Three high beeps
    //Report pressure and motor data over USB for analysis / other uses. timestamps disabled by default
    //Serial.print(millis()); //Timestamp (ms)
    //Serial.print(",");
    //Serial.print(motSpeed); //Motor speed (0-255)
    //Serial.print(",");
    //Serial.print(pressure); //(Original ADC value - 12 bits, 0-4095)
    //Serial.print(",");
    //Serial.print(avgPressure); //Running average of (default last 25 seconds) pressure
   // Serial.print(",");
    //Serial.print(rampPTime);
    //Serial.print(",");
    //Serial.print(rampTimeS);
   // Serial.print(",");
   // Serial.print(autoRTime);
   // Serial.print(",");
   // Serial.print(bri);
   // Serial.println(",");
   // Serial.println(",");
  }
}

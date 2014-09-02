#include <Encoder.h>
#include <SPI.h>
#include <Adafruit_MAX31855.h>
#include <Adafruit_NeoPixel.h>
#include <ReflowDisplay.h>
#include "Reflowster.h"

#include <string.h>
#include <EEPROM.h>

const int REVISION=3;

Reflowster reflowster;

struct profile {
  profile() : soakTemp(), soakTime(), peakTemp() {}
  profile(int stp, int sti, int ptp) : soakTemp( stp ), soakTime( sti ), peakTemp( ptp ) {} 
  byte soakTemp;
  byte soakTime;
  byte peakTemp;
};

profile profile_min(0,0,0);
profile profile_max(250,250,250);

profile active;
byte activeProfile;

char * profileNames[] = {"pb leaded","-pb unleaded","custom"};
const int PROFILE_COUNT = 3;

int activeCommand = 0;
// This represents the currently active command. When a command is received, this is triggered and the busy loops handle
// adjusting the state to execute the command
const int CMD_REFLOW_START=1;
const int CMD_REFLOW_STOP=2;

// This represents the current state that the Reflowster is in, it is used by the command processing framework
// It should be considered read only in most cases
int activeMode = 0;
const int MODE_REFLOW=1;
const int MODE_MENU=2;

const int CONFIG_LOCATION = 200;
const int CONFIG_SELF_TEST = 0;
const int CONFIG_TEMP_MODE = 1;
const int CONFIG_ADVANCED_MODE = 2;

const int TEMP_MODE_C = 0;
const int TEMP_MODE_F = 1;
const int DEFAULT_TEMP_MODE = TEMP_MODE_C;

const int tones[] = {28,29,31,33,35,37,39,41,44,46,49,52,55,58,62,65,69,73,78,82,87,92,98,104,110,117,123,131,139,147,156,165,175,185,196,208,220,233,247,262,277,294,311,330,349,370,392,415,440,466,494,523,554,587,622,659,698,740,784,831,880,932,988,1047,1109,1175,1245,1319,1397,1480,1568,1661,1760,1865,1976,2093,2217,2349,2489,2637,2794,2960,3136,3322,3520,3729,3951,4186};

void setup() {
  delay(100);
  Serial.begin(9600);

  noInterrupts();
  TCCR1A = 0;
  TCCR1B = 0;

  TCNT1 = 65500;            // preload timer 65536-16MHz/256/2Hz
  TCCR1B |= (1 << CS10)|(1 << CS11);    // 64 prescaler 
  TIMSK1 |= (1 << TOIE1);   // enable timer overflow interrupt
  
  reflowster.init();
  interrupts();
  
  //We run a self test the first time the unit is powered on.. when it passes, the self test is never run again
  if (readConfig(CONFIG_SELF_TEST) != 0) {
    reflowster.selfTest();
    reflowster.getDisplay()->display("pas");
    while(1) {
      if (reflowster.getButton()) {
        factoryReset();
        break;
      }
      
      if (reflowster.getBackButton()) break;
    }
  }

  reflowster.displayTest();
  reflowster.getDisplay()->display(REVISION);
  delay(500);
  initProfile();
}

void factoryReset() {
  active = profile(130,90,225); //leaded
  saveProfile(0);
  saveProfile(2); //can we use #defines to spell out which profile is which?
  active = profile(140,90,235); //unleaded
  saveProfile(1);

  ewrite(0,0); //default active profile

  writeConfig(CONFIG_SELF_TEST,0);
  //writeConfig(CONFIG_SELF_TEST,255); //this causes the self-test to run next time you cycle power
  writeConfig(CONFIG_TEMP_MODE,DEFAULT_TEMP_MODE); //default to celsius
  writeConfig(CONFIG_ADVANCED_MODE,0); //default to disabled
}

unsigned long lastService = millis();
ISR(TIMER1_OVF_vect) {
  //TCNT1 = 65518;
  TCNT1 = 65200;
  
  //if (millis() - lastService > 1) {
    reflowster.tick(); //this updates the display
    lastService = millis();
  //}
  processCommands();
}

byte debounceButton(int b) {
  if (!digitalRead(b)) {
    while(!digitalRead(b));
    delay(100);
    return 1;
  }
  return 0;
}

const char * setCommands[] = {"setst","setsd","setpt"};
const char * setNames[] = {"soak temperature","soak duration","peak temperature"};
void processCommands() {
  char buffer[30];
  boolean recognized = false;
  char i = 0;
  while (Serial.available()) {
    buffer[i++] = Serial.read();
  }
  if (i != 0) {
    buffer[i] = 0;
    String command = String(buffer);
    int spaceAt = command.indexOf(" ");
    String arguments = command.substring(spaceAt+1);
    if (spaceAt == -1) arguments = "";
    Serial.write("> ");
    Serial.write(buffer);
    Serial.write("\n");
    if (activeMode == MODE_MENU) {
      if (command.startsWith("relay ")) {
        if (arguments.equalsIgnoreCase("on")) {
          recognized = true;
          reflowster.relayOn();
        } else if (arguments.equalsIgnoreCase("off")) {
          recognized = true;
          reflowster.relayOff();        
        } else if (arguments.equalsIgnoreCase("toggle")) {
          recognized = true;
          reflowster.relayToggle();
        }
      } else if (command.startsWith("set ")) {
        recognized = true;
        int val = arguments.toInt();
        if (val < 0 || val > PROFILE_COUNT-1) {
          Serial.println("Profile out of range!");
        } else {
          loadProfile(val);
          Serial.print("Set active profile: ");
          Serial.println(profileNames[activeProfile]);
        }
      } else if (command.startsWith("set")) {
        int l;
        int val = arguments.toInt();
        for (l=0; l<3; l++) {
          if (command.startsWith(setCommands[l])) {
            recognized = true;
            if (val < ((byte *)&profile_min)[l] || val > ((byte *)&profile_max)[l]) {
              Serial.println("Value out of range!");
            } else {
              ((byte *)&active)[l] = (byte)val;
              saveProfile(activeProfile);
              Serial.print("Set ");
              Serial.print(setNames[l]);
              Serial.print(": ");
              Serial.println(val);
            }
          }
        }
      } else if (command.equalsIgnoreCase("start")) {
        recognized = true;
        activeCommand = CMD_REFLOW_START;
      }
    } else if (activeMode == MODE_REFLOW) {
      if (command.equalsIgnoreCase("stop")) {
        recognized = true;
        activeCommand = CMD_REFLOW_STOP;
      }
    }
    
    if (command.equalsIgnoreCase("status")) {
      recognized = true;
      if (activeMode == MODE_MENU) {
        Serial.println("Status: menus");
      } else if (activeMode == MODE_REFLOW) {
        Serial.println("Status: reflow in progress");        
      } else {
        Serial.print("Status: ");
        Serial.println(activeMode);
      }
      Serial.print("Firmware version: ");
      Serial.println(REVISION);
      double tempC = reflowster.readCelsius();
    Serial.print("Current thermocouple reading: ");
      Serial.print(tempC);
      Serial.print("C ");
      Serial.print(ctof(tempC));
      Serial.println("F");
      tempC = reflowster.readInternalC();
      Serial.print("Internal junction temperature: ");
      Serial.print(tempC);
      Serial.print("C ");
      Serial.print(ctof(tempC));
      Serial.println("F");
      

      if (readConfig(CONFIG_TEMP_MODE) == TEMP_MODE_F) {
        Serial.println("Mode: Fahrenheit");
      } else {
        Serial.println("Mode: Celsius");        
      }
      if (reflowster.relayStatus()) {
        Serial.println("Relay: ON");
      } else {
        Serial.println("Relay: OFF");
      }
      Serial.print("Configuration: ");
      Serial.println(profileNames[activeProfile]);
      Serial.print("Soak Temperature (C): ");
      Serial.println(active.soakTemp);
      
      Serial.print("Soak Time (s): ");
      Serial.println(active.soakTime);
      
      Serial.print("Peak Temperature (C): ");
      Serial.println(active.peakTemp);
    } else if (command.equalsIgnoreCase("help")) {
      Serial.println("Reflowster accepts the following commands in normal mode:");
      Serial.println("relay on|off|toggle, setst deg_c, setsd time_s, setpt deg_c, start, status, help");
      Serial.println();
      Serial.println("Reflowster accepts the following commands during a reflow:");
      Serial.println("stop, status, help");
    }
    if (recognized == false) {
      Serial.println("Unrecognized or invalid command!");
    }
  }
}

void tone_error() {
  reflowster.beep(tones[28],200);
  delay(200);
  reflowster.beep(tones[27],200);
  delay(200);
}

void tone_notice() {
  reflowster.beep(tones[52],200);
  delay(200);
  delay(100);
  reflowster.beep(tones[52],200);
//  delay(200);
}

void tone_success() {
  reflowster.beep(tones[42],100);
  delay(100);
  reflowster.beep(tones[45],100);
  delay(100);
  reflowster.beep(tones[50],100);
  delay(100);
  reflowster.beep(tones[76],60);
  delay(100);
  reflowster.beep(tones[76],60);
  //  delay(100);
}

void tone_blip() {
  reflowster.beep(tones[32],20);
//  delay(100);  
}

int displayMenu(char * options[], int len, int defaultChoice) {
  activeMode = MODE_MENU;
  int menuIndex = -1;
  reflowster.setKnobPosition(defaultChoice);
  while(1) {
    //reflowster.pulseTick();
    
    if (activeCommand != 0) return -1;
    if (debounceButton(reflowster.pinConfiguration_encoderButton)) {
      reflowster.getDisplay()->clear();
      return menuIndex;
    }
    if (debounceButton(reflowster.pinConfiguration_backButton)) {
      reflowster.getDisplay()->clear();
      return -1;
    }
    
    int newIndex = reflowster.getKnobPosition();
//    Serial.println();
//    Serial.print("oldIndex: ");
//    Serial.println(menuIndex);
//    Serial.print("newIndex: ");
//    Serial.println(newIndex);
    
    if (newIndex >= len) {
      newIndex = len - 1;
      //Serial.print("setting knob: ");
      //Serial.println(newIndex);
      reflowster.setKnobPosition(newIndex);
    }
    
    if (newIndex < 0) {
      newIndex = 0;
      //Serial.print("setting knob: ");
      //Serial.println(newIndex);
      reflowster.setKnobPosition(newIndex);
    }
    
    if (newIndex != menuIndex) {
      tone_blip();
      menuIndex = newIndex;
      reflowster.getDisplay()->displayMarquee(options[menuIndex]);
    }

    delay(100);
  }
  reflowster.getDisplay()->clear();
  return menuIndex;
}

int chooseNum(int low, int high, int defaultVal) {
  int val = defaultVal;
  reflowster.setKnobPosition(val);
  while(1) {
    if (debounceButton(reflowster.pinConfiguration_encoderButton)) return val;
    if (debounceButton(reflowster.pinConfiguration_backButton)) return defaultVal;
    
    val = reflowster.getKnobPosition();
    if (val > high) {
      val = high;
      reflowster.setKnobPosition(val);
    }
    if (val < low) {
      val = low;
      reflowster.setKnobPosition(val);
    }
    
    reflowster.getDisplay()->display(val);
    delay(100);
  }
}

void initProfile() {
  activeProfile = EEPROM.read(0); //read the selected profile
  if (activeProfile > PROFILE_COUNT-1) activeProfile = 0;

  loadProfile(activeProfile); //use the leaded profile as the default
}

void loadProfile(byte profileNumber) {
  activeProfile = profileNumber;
  if (EEPROM.read(0) != activeProfile) ewrite(0,activeProfile);
  struct profile * target = &active;
  int loc = 1+profileNumber*3;
  for (byte i=0; i<3; i++) {
    byte val = EEPROM.read(loc+i);
    if (val != 255) *(((byte*)target)+i) = val; //pointer-fu to populate the profile struct
  }
}

void saveProfile(byte profileNumber) {
  int loc = 1+profileNumber*3;
  struct profile * target = &active;
  for (byte i=0; i<3; i++) {
    byte val = EEPROM.read(loc+i);
    if (val != *(((byte*)target)+i)) ewrite(loc+i,*(((byte*)target)+i)); //we only write to eeprom if the value is changed
  }
}

void writeConfig(int cfg, byte value) {
  ewrite(CONFIG_LOCATION+cfg,value);
}

byte readConfig(int cfg) {
  return EEPROM.read(CONFIG_LOCATION+cfg);
}

void ewrite(byte loc, byte val) {
  /*
  Serial.print("EEPROM WRITE ");
  Serial.print(val);
  Serial.print(" to ");
  Serial.println(loc);
  */
  EEPROM.write(loc,val);
}

void loop() { 
  mainMenu();
}

char * mainMenuItems[] = {"go","edit","open","monitor","config","hold temp"};
const int MAIN_MENU_SIZE = 6;

void mainMenu() {
  byte lastChoice = 0;
  while(1) {
    if (activeCommand == CMD_REFLOW_START) {
      activeCommand = 0;
      doReflow();
    }
    //TODO discuss this and probably move it somewhere else
    if (reflowster.readInternalC() > reflowster.MAX_ALLOWABLE_INTERNAL) { //overheat protection
      reflowster.relayOff();
    }
    int menuSize = (readConfig(CONFIG_ADVANCED_MODE) ? MAIN_MENU_SIZE : MAIN_MENU_SIZE - 1); //allow for disabling advanced features
    int choice = displayMenu(mainMenuItems,menuSize,lastChoice);
    if (choice != -1) lastChoice = choice;
    switch(choice) {
      case 0: doReflow(); break;

      case 1: 
        editProfile();
      break;

      case 2: 
        if (openProfile()) lastChoice = 0;
      break;

      case 3: doMonitor(); break;
      
      case 4: configMenu(); break;

      case 5: thermostat(); break;
    }
  }
}

void dataCollectionMode() {
  double goalTemp = 80;
  unsigned long lastReport = millis();
  
  unsigned long lastCycleStart = millis();
  float ratio = .1;
  int timeUnit = 30000;
  
  long REPORT_FREQUENCY = 5000;
  while(1) {
    double temp = 0;
    if (readConfig(CONFIG_TEMP_MODE) == TEMP_MODE_F) {
      temp = reflowster.readFahrenheit();
    } else {
      temp = reflowster.readCelsius();
    }
    if ((millis() - lastReport) > REPORT_FREQUENCY) {
      Serial.print("data: ");
      Serial.print(temp);
      Serial.print(" ");
      Serial.println(reflowster.relayStatus());
      lastReport += REPORT_FREQUENCY;
    }
    
    long current = millis();
    long cycleDuration = current - lastCycleStart;
    if (current > lastCycleStart+timeUnit) {
      lastCycleStart += timeUnit;
      cycleDuration = 0;
    } 
    double cratio = (double)cycleDuration/(double)timeUnit;
    boolean on = cratio < ratio;
    if (temp < 88) on = true;
//    Serial.print("cycleDuration: ");
//    Serial.println(cycleDuration);
//    Serial.print("cratio: ");
//    Serial.println(cratio);
    if (reflowster.relayStatus() && !on) {
      reflowster.relayOff();
    } else if (!reflowster.relayStatus() && on) {
      reflowster.relayOn();        
    }
    
    if (reflowster.getBackButton()) {
       while(reflowster.getBackButton());
       delay(1000);
       return; 
    }
    
    delay(300);
  }
}

void doReflow() {
  if (isnan(reflowster.readCelsius())) {
    reflowster.getDisplay()->displayMarquee("err no temp");
    Serial.println("Error: Thermocouple could not be read, check connection!");
    while(!reflowster.getDisplay()->marqueeComplete());
    return;
  }

  byte soakTemp = active.soakTemp;
  byte soakTime = active.soakTime;
  byte peakTemp = active.peakTemp;

  activeMode = MODE_REFLOW;
  int status = reflowImpl(soakTemp,soakTime,peakTemp);
  Serial.println(status);
  if (status == 0) {  
    reflowster.getDisplay()->displayMarquee("done");
  } else if (status == -1) {
    reflowster.getDisplay()->displayMarquee("cancelled");
    tone_error();
  } else if (status == -2) {
    reflowster.getDisplay()->displayMarquee("too hot");
    tone_error();
  }
  reflowster.setStatusColor(0,0,0);
  while(!reflowster.getDisplay()->marqueeComplete());
}

boolean openProfile() {
  while(1) {
    int choice = displayMenu(profileNames,PROFILE_COUNT,choice);
    if (choice == -1) return false;
    loadProfile(choice);
    return true;
  }
}

int chooseTemp(byte storedTemp) {
  boolean fahrenheitMode = readConfig(CONFIG_TEMP_MODE) == TEMP_MODE_F;
  if (fahrenheitMode) {
    return ftoc(chooseNum(0,ctof(255),ctof(storedTemp)));
  } else {
    return chooseNum(0,255,storedTemp);
  }
}

char * editProfileMenuItems[] = {"st-soak temp","sd-soak duration","pt-peak temp"};
const int EDIT_PROFILE_ITEMS = 3;
boolean editProfile() {
  int choice = 0;
  int val;
  byte stored;
  while(1) {
    choice = displayMenu(editProfileMenuItems,EDIT_PROFILE_ITEMS,choice);
    switch(choice) {
      case -1: return true;
      case 0:
      case 1:
      case 2:
        stored = *(((byte*)&active)+choice);
        if (choice == 0 || choice == 2) { //temp choices
          val = chooseTemp(stored);
        } else {
          val = chooseNum(0,255,stored);
        }
        *(((byte*)&active)+choice) = val;

        saveProfile(activeProfile);
      break;
    }
  }
}

void doMonitor() {
  unsigned long lastReport = millis();
  int MONITOR_FREQUENCY = 1000;
  while(e) {
    
    double temp;
    if (readConfig(CONFIG_TEMP_MODE) == TEMP_MODE_F) {
      temp = reflowster.readFahrenheit();
    } else {
      temp = reflowster.readCelsius();
    }
    reflowster.getDisplay()->display((int)temp);

    if ((millis() - lastReport) > MONITOR_FREQUENCY) {  //generate a 1000ms event period
      Serial.println(temp);
      lastReport += MONITOR_FREQUENCY;
    }
    
    if (debounceButton(reflowster.pinConfiguration_encoderButton)) return;
    if (debounceButton(reflowster.pinConfiguration_backButton)) return;

    delay(50);
  } 
}

char * configMenuItems[] = {"temp mode","adv features","factory reset"};
const int CONFIG_MENU_ITEMS = 3;

char * tempModeMenu[] = {"Cel","Fah"};
const int TEMP_MODE_ITEMS = 2;

char * advFeatMenu[] = {"no","yes"};
const int ADV_FEAT_ITEMS = 2;

void configMenu() {
  int choice = 0;
  int subChoice = 0;
  while(1) {
    choice = displayMenu(configMenuItems,CONFIG_MENU_ITEMS,choice);
    switch(choice) {
      case -1: return;
      case 0: {
        subChoice = readConfig(CONFIG_TEMP_MODE);
        subChoice = displayMenu(tempModeMenu,TEMP_MODE_ITEMS,subChoice);
        if (subChoice != -1 && subChoice != readConfig(CONFIG_TEMP_MODE)) {
          writeConfig(CONFIG_TEMP_MODE,subChoice);
        }
      }
      break;

      case 1: {
        subChoice = readConfig(CONFIG_ADVANCED_MODE);
        subChoice = displayMenu(advFeatMenu,ADV_FEAT_ITEMS,subChoice);
        if (subChoice != -1 && subChoice != readConfig(CONFIG_ADVANCED_MODE)) {
          writeConfig(CONFIG_ADVANCED_MODE,subChoice);
        }
      }
      break;

      case 2:
        reflowster.getDisplay()->displayMarquee("are you sure");
        delay(300);
        boolean cancel = true;
        while(1) {
          if (debounceButton(reflowster.pinConfiguration_encoderButton)) {
            cancel = false;
            break;
          }
          if (debounceButton(reflowster.pinConfiguration_backButton)) {
            cancel = true;
            break;
          }
        }
        if (!cancel) {
          factoryReset();
          return;
        }
      break;
    }
  }  
}

void thermostat() {
  unsigned long lastReport = millis();
  int UPDATE_PERIOD = 500;
  int setpoint = chooseNum(0,celsiusToFahrenheitIfNecessary(375),celsiusToFahrenheitIfNecessary(100));
  int hyst = 1; //degrees
  reflowster.setStatusColor(0,0,5);
  while(1) {
    double temp;
    if (readConfig(CONFIG_TEMP_MODE) == TEMP_MODE_F) {
      temp = reflowster.readFahrenheit();
    } else {
      temp = reflowster.readCelsius();
    }
    reflowster.getDisplay()->display((int)temp);

    if ((millis() - lastReport) > UPDATE_PERIOD) {  //generate an event period (ms)
      Serial.println(temp);
      lastReport += UPDATE_PERIOD;
      if ((temp < setpoint - hyst)&(not reflowster.relayStatus())) {
        reflowster.relayOn();
        reflowster.setStatusColor(35,10,0);
      } 
      if ((temp > setpoint + hyst)&(reflowster.relayStatus())) {
        reflowster.relayOff();
        reflowster.setStatusColor(0,0,5);
      }
    }
    
    if (debounceButton(reflowster.pinConfiguration_encoderButton)|debounceButton(reflowster.pinConfiguration_backButton)) {
      reflowster.relayOff();
      reflowster.setStatusColor(0,0,0);
      return;
    }
    
    delay(50);
  } 
}

double ctof(double c) {
  return ((c*9.0)/5.0)+32.0;
}

double ftoc(double f) {
  return ((f-32.0)*5.0)/9.0;
}

double celsiusToFahrenheitIfNecessary(double c) {
  if (readConfig(CONFIG_TEMP_MODE) == TEMP_MODE_C) return c;
  return ctof(c);
}

#define PHASE_PRE_SOAK 0
#define PHASE_SOAK 1
#define PHASE_SPIKE 2
#define PHASE_COOL 3
#define CANCEL_TIME 5000
int reflowImpl(byte soakTemp, byte soakTime, byte peakTemp) {
  unsigned long startTime = millis();
  unsigned long phaseStartTime = millis();
  unsigned long buttonStartTime = 0;
  unsigned long lastReport = millis();
  int phase = PHASE_PRE_SOAK;
  unsigned long MAXIMUM_OVEN_PHASE_TIME = 8*60; //if the oven is on for 8 minutes and we haven't hit the desired temp

  Serial.println("Starting Reflow: ");
  Serial.print("Soak Temp: ");
  Serial.println(soakTemp);
  Serial.print("Soak Time: ");
  Serial.println(soakTime);
  Serial.print("Peak Temp: ");
  Serial.println(peakTemp);
  
  int REPORT_INTERVAL = 200; //ms
  
  reflowster.relayOn();
  byte pulseColors = 0;
  int pulse = 0;
  while(1) {
    //reflowster.pulseTick();
    delay(50);
    double temp = reflowster.readCelsius();
    double internaltempC = reflowster.readInternalC();
    unsigned long currentPhaseSeconds = (millis() - phaseStartTime) / 1000;
    
    if ((millis() - lastReport) > REPORT_INTERVAL) {  //generate an event period
      Serial.print("data: ");
      Serial.print(temp);
      Serial.print(" ");
      Serial.println(reflowster.relayStatus());
      lastReport += REPORT_INTERVAL;
    }
    
    if (activeCommand == CMD_REFLOW_STOP) {
        Serial.println("Reflow cancelled");
        activeCommand = 0;
        reflowster.relayOff();
        return -1;
    }
    if (internaltempC > reflowster.MAX_ALLOWABLE_INTERNAL) { //overheat protection will end reflow process
        Serial.print("Error: Internal Overtemp; Too Hot ");
        Serial.print(internaltempC);
        Serial.println("C");
        activeCommand = 0;
        reflowster.relayOff();
        return -2;
    }
    if (buttonStartTime == 0) {
      reflowster.getDisplay()->display((int)celsiusToFahrenheitIfNecessary(temp));
      if (reflowster.getBackButton()) {
        reflowster.getDisplay()->displayMarquee("Hold to cancel");
        buttonStartTime = millis();
      }
    } else {
      if ((millis() - buttonStartTime) > CANCEL_TIME) {
        reflowster.relayOff();
        return -1;
      }
      if (!reflowster.getBackButton()) buttonStartTime = 0;
    }
    switch(phase) {
      case PHASE_PRE_SOAK: {
        reflowster.setStatusPulse(25,5,0);
        if (currentPhaseSeconds > MAXIMUM_OVEN_PHASE_TIME) {
          reflowster.relayOff();
          return -1;
        }
        if (temp >= soakTemp) {
          phase = PHASE_SOAK;
          phaseStartTime = millis();
          reflowster.relayOff();
        }
        break;
      }
      
      case PHASE_SOAK: {
        reflowster.setStatusPulse(25,15,0);
        if (currentPhaseSeconds > soakTime) {
          phase = PHASE_SPIKE;
          phaseStartTime = millis();
          reflowster.relayOn();
        }
        break;
      }
      
      case PHASE_SPIKE: {
        reflowster.setStatusPulse(25,0,0);
        if (temp >= peakTemp || currentPhaseSeconds > MAXIMUM_OVEN_PHASE_TIME) {
          phase = PHASE_COOL;
          phaseStartTime = millis();
          reflowster.relayOff();
          tone_success();
        }
        break;
      }
      
      case PHASE_COOL: {
        reflowster.setStatusPulse(0,0,25);
        unsigned long currentCoolSeconds = (millis() - phaseStartTime) / 1000;
        if (debounceButton(reflowster.pinConfiguration_backButton) || debounceButton(reflowster.pinConfiguration_encoderButton)) {
//        if (currentCoolSeconds > 30 || temp < 60 || debounceButton(reflowster.pinConfiguration_backButton) || debounceButton(reflowster.pinConfiguration_encoderButton)) {
          reflowster.relayOff();
          return 0;
        }
        break;
      }
    }
  }
}

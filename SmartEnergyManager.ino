/************************************************************
 SMART ENERGY MANAGER - 
************************************************************/

#define BLYNK_TEMPLATE_ID   "TMPL3PdX7Ndt5"
#define BLYNK_TEMPLATE_NAME "Smart Energy Manager"
#define BLYNK_AUTH_TOKEN    "Rt-hQKwvBongbqXLzwDEoK4oI9W4WmB4"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <PZEM004Tv30.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// WiFi
char ssid[] = "Realme 6";
char pass[] = "23456789";

// LCD
LiquidCrystal_I2C lcd(0x27, 16, 2);
int lcdPage = 0;


// PZEM
#define RXD2 16
#define TXD2 17
PZEM004Tv30 pzem(Serial2, RXD2, TXD2);

// Relays
#define R1 25
#define R2 26
#define R3 27
#define R4 32

#define RELAY_ON LOW
#define RELAY_OFF HIGH

BlynkTimer timer;

// Limits
float MAX_V = 270, RESET_V = 260;
float MAX_I = 1.1, RESET_I = 1.0;
float PF_LOW = 0.91;
float PF_LED_RESET = 0.94;
float PF_CAP_RESET = 0.99;
float MIN_LOAD = 10;

// Cost & Saving
float ENERGY_RATE = 8.0;
float totalEnergySaved = 0;
float totalMoneySaved = 0;
float lineResistance = 1.0;
float currentBeforeAPFC = 0;
float currentAfterAPFC = 0;
unsigned long lastSaveTime = 0;
unsigned long lastReportTime = 0; 

// States
bool autoMode = true;
bool killSwitch = false;
bool faultLatched = false;
bool ovFault = false;
bool ocFault = false;

// 🔴 NEW: Low PF
bool lowPFFault = false;
bool pfNotified = false;

// Notification flags
bool ovNotified = false;
bool ocNotified = false;

// Relays state
bool r1=true, r2=true, r3=false, r4=false;
int capStage = 0;

unsigned long lastCap = 0;
unsigned long CAP_DELAY = 5000;
unsigned long startupTime = 0;
bool systemReady = false;

// Readings
float V,I,P,PF,F,E;

// ---------- RELAY ----------

void setRelay(int pin, bool st){
  digitalWrite(pin, st ? RELAY_ON : RELAY_OFF);
}

void applyRelays(){

  if(killSwitch || faultLatched){
    r1=false; r2=false; r3=false; r4=false;
    capStage=0;
  }

  if(!r1 || !r2){
    r3=false; r4=false; capStage=0;
  }

  setRelay(R1,r1);
  setRelay(R2,r2);
  setRelay(R3,r3);
  setRelay(R4,r4);

  Blynk.virtualWrite(V10,r1);
  Blynk.virtualWrite(V11,r2);
  Blynk.virtualWrite(V12,r3);
  Blynk.virtualWrite(V13,r4);
}

void setCapStage(int s){
  capStage=s;
  r3 = (s==1 || s==3);
  r4 = (s==2 || s==3);
  lastCap = millis();
}

// ---------- APFC ----------

void apfc(){

  if(P < MIN_LOAD || I < 0.03){
    setCapStage(0); return;
  }

  if(PF > PF_CAP_RESET){
    if(capStage==3 && millis()-lastCap>CAP_DELAY){ setCapStage(2); return; }
    if(capStage==2 && millis()-lastCap>CAP_DELAY){ setCapStage(1); return; }
    if(capStage==1 && millis()-lastCap>CAP_DELAY){ setCapStage(0); return; }
  }

  if(PF < PF_LOW){

    if(capStage==0){

      currentBeforeAPFC = I;

      setCapStage(1);
      return;
    }

    if(capStage==1 && millis()-lastCap>CAP_DELAY){

      setCapStage(2);
      return;
    }

    if(capStage==2 && millis()-lastCap>CAP_DELAY){

      setCapStage(3);
      return;
    }
}
}

// ---------- SAVINGS ----------

void updateSavings(){

  unsigned long now = millis();

  if(lastSaveTime == 0){
    lastSaveTime = now;
    return;
  }

  float hours = (now - lastSaveTime) / 3600000.0;

  lastSaveTime = now;

  // Calculate only when APFC active
  if((r3 || r4) && P > MIN_LOAD){

    currentAfterAPFC = I;

    // I²R losses
    float lossBefore =
    currentBeforeAPFC *
    currentBeforeAPFC *
    lineResistance;

    float lossAfter =
    currentAfterAPFC *
    currentAfterAPFC *
    lineResistance;

    // Power saved
    float powerSaved =
    lossBefore - lossAfter;

    // Ignore negative values
    if(powerSaved < 0){
      powerSaved = 0;
    }

    // Energy saved in kWh
    float energySaved =
    (powerSaved * hours) / 1000.0;

    totalEnergySaved += energySaved;

    // Money saved
    totalMoneySaved =
    totalEnergySaved * ENERGY_RATE;
  }

  // Send to Blynk
  Blynk.virtualWrite(V31, totalEnergySaved);

  Blynk.virtualWrite(V32, totalMoneySaved);
}

// ---------- LCD ----------

void updateLCD(){
 if(!systemReady) return;
  lcd.clear();

  if(lcdPage==0){
    lcd.setCursor(0,0);
    lcd.print("V:"); lcd.print(V,1);
    lcd.print(" I:"); lcd.print(I,2);

    lcd.setCursor(0,1);
    lcd.print("P:"); lcd.print(P,0);
    lcd.print(" PF:"); lcd.print(PF,2);
  }

  else if(lcdPage==1){
    lcd.setCursor(0,0);
    lcd.print("F:"); lcd.print(F,1);
    lcd.print( "Hz");
    lcd.setCursor(0,1);
    lcd.print("E:"); lcd.print(E,2);
  }

  else if(lcdPage==2){
    lcd.setCursor(0,0);
    lcd.print(autoMode?"AUTO":"MAN");

    lcd.setCursor(0,1);
    lcd.print(killSwitch?"KILL":"RUN");
  }

  else if(lcdPage==3){
    lcd.setCursor(0,0);
    lcd.print("OV:"); lcd.print(ovFault?"Y":"N");

    lcd.setCursor(0,1);
    lcd.print("OC:"); lcd.print(ocFault?"Y":"N");
  }

  lcdPage++; if(lcdPage>3) lcdPage=0;
}

// ---------- MAIN ----------

void mainTask(){
 if(millis()- startupTime > 2000){
   systemReady = true;
 }
  V=pzem.voltage();
  I=pzem.current();
  P=pzem.power();
  PF=pzem.pf();
  F=pzem.frequency();
  E=pzem.energy();

  if(isnan(V)||isnan(I)||isnan(P)||isnan(PF)) return;
  //Invalid / garbage reading filter
  
  if (V < 50) {
  V = 0; I = 0; P = 0; PF = 0; // Register as a blackout/off state
  } 
  else if (V > 300) {
  return; // Only ignore impossible high spikes
  }
  if ( I <0 || I>20 ) return ;
  if ( P< 0 || P> 5000) return ;
  // FIX PF GLITCHES 
  if ( PF< 0 ||PF>1 ){
    PF = 0;
  }
  // Fault detection
  if(V>MAX_V){
    ovFault=true; faultLatched=true;
    if(!ovNotified){
      Blynk.logEvent("over_voltage", "Voltage="+String(V));
      ovNotified=true;
    }
  }

  if(I>MAX_I){
    ocFault=true; faultLatched=true;
    if(!ocNotified){
      Blynk.logEvent("over_current", "Current="+String(I));
      ocNotified=true;
    }
  }

  // 🔴 LOW PF ALERT
  if(systemReady && PF < PF_LOW && P > MIN_LOAD && I > 0.03){
    lowPFFault = true;

    if(!pfNotified){
      Blynk.logEvent("low_power_factor", "PF="+String(PF));
      pfNotified = true;
    }
  }

  if(PF > PF_LED_RESET || P < MIN_LOAD){
    lowPFFault = false;
    pfNotified = false;
  }

  // Send data
  Blynk.virtualWrite(V0,V);
  Blynk.virtualWrite(V1,I);
  Blynk.virtualWrite(V2,P);
  Blynk.virtualWrite(V3,PF);
  Blynk.virtualWrite(V4,F);
  Blynk.virtualWrite(V5,E);

  float cost = E * ENERGY_RATE;
  Blynk.virtualWrite(V30, cost);

  Blynk.virtualWrite(V21, ovFault ? 1 : 0);
  Blynk.virtualWrite(V22, ocFault ? 1 : 0);
  Blynk.virtualWrite(V23, lowPFFault ? 1 : 0); // NEW LED

  if(autoMode && !faultLatched && !killSwitch){
    apfc();
  }

  applyRelays();
  updateSavings();
  
// ---------- REPORT NOTIFICATION ----------
 if(millis() - lastReportTime >= 180000){

  String report =
  "Energy Report\n" +
  String("Energy: ") + String(E,2) + " kWh\n" +
  String("Bill: Rs ") + String(E * ENERGY_RATE,2) + "\n" +
  String("Energy Saved: ") + String(totalEnergySaved,2) + " kWh\n" +
  String("Money Saved: Rs ") + String(totalMoneySaved,2);

  Blynk.logEvent("monthly_report", report);

  lastReportTime = millis();
 }
}
// ---------- BLYNK ----------

BLYNK_WRITE(V20){ killSwitch = param.asInt(); applyRelays(); }
BLYNK_WRITE(V40){ autoMode = param.asInt(); }

// ---------- MANUAL MODE CONTROLS ----------

BLYNK_WRITE(V10){
  if(!autoMode){
    r1 = param.asInt();
    applyRelays();
  }
}

BLYNK_WRITE(V11){
  if(!autoMode){
    r2 = param.asInt();
    applyRelays();
  }
}

BLYNK_WRITE(V12){
  if(!autoMode){
    r3 = param.asInt();
    applyRelays();
  }
}

BLYNK_WRITE(V13){
  if(!autoMode){
    r4 = param.asInt();
    applyRelays();
  }
}

//------------MANUAL RESTART------------

BLYNK_WRITE(V50){
  if(param.asInt()==1){
    if(V<RESET_V && I<RESET_I){
      faultLatched=false;
      ovFault=false;
      ocFault=false;
      ovNotified=false;
      ocNotified=false;
      r1=true; r2=true;
    }
  }
  applyRelays();
}

// ---------- SETUP ----------

void setup(){

  Serial.begin(115200);
  Wire.begin();
  pinMode(R1,OUTPUT);
  pinMode(R2,OUTPUT);
  pinMode(R3,OUTPUT);
  pinMode(R4,OUTPUT);

  setRelay(R1,false);
  setRelay(R2,false);
  setRelay(R3,false);
  setRelay(R4,false);
  
  lcd.init();
  lcd.backlight();
  lcd.print("Starting...");
  startupTime = millis();
  delay(2000);

  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
  autoMode = true;
  Blynk.virtualWrite ( V40, 1);
  timer.setInterval(1000L, mainTask);
  timer.setInterval(2000L, updateLCD);
}

void loop(){
  Blynk.run();
  timer.run();
}

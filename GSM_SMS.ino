/*
  Tiny code for manage Arduino official GSM shield (base on Quectel M10 chip).
  
  This example code is in the public domain.
  Share, it's happiness !
*/

/* library */
// Arduino core
#include <SoftwareSerial.h>
#include <avr/power.h>
#include <avr/wdt.h>
#include <avr/sleep.h>
#include "Wire.h"
// Timer : must use at least v1.1 for fix millis rolover issue
// come from https://github.com/JChristensen/Timer
#include "Timer.h"
// LCD RGB shield :
// come from https://github.com/adafruit/Adafruit-RGB-LCD-Shield-Library
// change on line 119 of "Adafruit_RGBLCDShield.cpp" -> setBacklight(0x0) instead of setBacklight(0x7)
// so we start with backlight off
#include <Adafruit_MCP23017.h>
#include <Adafruit_RGBLCDShield.h>

// some const
#define SMS_CHECK_INTERVAL   30000
#define STAT_UPDATE_INTERVAL  5000
#define LCD_UPDATE_INTERVAL    300
#define M10_PWRKEY_PIN 7
#define TEST_LED 13
#define TC_PIN "1111"
#define VERSION "0.2"
#define LCD_BL_OFF    0x0
#define LCD_BL_RED    0x1
#define LCD_BL_YELLOW 0x3
#define LCD_BL_GREEN  0x2
#define LCD_BL_TEAL   0x6
#define LCD_BL_BLUE   0x4
#define LCD_BL_VIOLET 0x5
#define LCD_BL_WHITE  0x7 

// some vars
SoftwareSerial gsm_modem(2, 3); // RX, TX
Timer t;
Adafruit_RGBLCDShield lcd = Adafruit_RGBLCDShield();
int job_sms;
int job_stat;
int job_LCD;
char rx_buf[64];
char tx_buf[64];
byte rx_index = 0;
byte tx_index = 0;
int rssi = 0;  
int bar  = 0;
unsigned int sms_counter = 0;
boolean lcd_bl_on = false;
unsigned long lcd_bl_on_t;

// link stdout (printf) to Serial object
// create a FILE structure to reference our UART output function
static FILE uartout = {0};

// create a output function
// This works because Serial.write, although of
// type virtual, already exists.
static int uart_putchar (char c, FILE *stream)
{
  Serial.write(c);
  return 0;
}

// *** ISR handler ***
SIGNAL(WDT_vect) {
  wdt_disable();
  wdt_reset();
  WDTCSR &= ~_BV(WDIE);
}

void setup()
{
  // IO setup
  pinMode(M10_PWRKEY_PIN, OUTPUT);
  digitalWrite(M10_PWRKEY_PIN, LOW);
  pinMode(TEST_LED, OUTPUT);
  digitalWrite(TEST_LED, LOW);
  // memory setup
  memset(tx_buf, 0, sizeof(tx_buf));
  memset(rx_buf, 0, sizeof(rx_buf));
  // open serial communications and wait for port to open:
  Serial.begin(9600);
  // fill in the UART file descriptor with pointer to writer
  fdev_setup_stream (&uartout, uart_putchar, NULL, _FDEV_SETUP_WRITE);
  // standard output device STDOUT is uart
  stdout = &uartout ;
  // set the data rate for the SoftwareSerial port
  gsm_modem.begin(9600);
  gsm_modem.setTimeout(2000);
  // set up the LCD's number of columns and rows:
  lcd.begin(16, 2);
  // start message
  lcd_line(0, "tinyRTU V" VERSION);
  lcd_line(1, "INIT GSM CHIPSET"); 
  Serial.println(F("init gsm chipset"));
  // check if gsm chip is on
  while(1) {
    // wait GSM chipset init
    delay(600);
    gsm_modem.flush();
    gsm_modem.print(F("AT\r"));
    if (gsm_modem.find("OK")) {
      delay(1500);
      break;
    }  
    // large pulse on PWRKEY for switch Quectel M10 chip on at startup
    digitalWrite(M10_PWRKEY_PIN, HIGH);
    delay(2100);
    digitalWrite(M10_PWRKEY_PIN, LOW);
    delay(WDTO_500MS);
  }
  lcd_line(1, "INIT GSM OK");  
  Serial.println(F("init ok"));
  // set modem in text mode (instead of PDU mode)
  gsm_modem.setTimeout(8000);
  gsm_modem.flush();
  gsm_modem.print(F("AT+CMGF=1\r"));
  if (! gsm_modem.find("OK")) {
    lcd_line(1, "ERR: SMS TXT MOD");
    while(1);
  }
  // delete all SMS
  Serial.println(F("flush all pending SMS"));
  // flush serial buffer
  gsm_modem.flush();
  // delete all messages
  gsm_modem.print(F("AT+CMGD=0,4\r"));
  // check result
  if (gsm_modem.find("\r\nOK\r\n")) {
    Serial.println(F("flush ok"));
  } else {
    Serial.println(F("flush error"));
    lcd_line(1, "ERR: flush SMS");
    while(1);
  }  
  // init timer job (after instead of every for regulary call delay)
  job_sms  = t.after(SMS_CHECK_INTERVAL,   jobRxSMS);
  job_stat = t.after(STAT_UPDATE_INTERVAL, jobSTAT);
  job_LCD  = t.every(LCD_UPDATE_INTERVAL,  jobLCD);
  jobSTAT();
  jobLCD();
}

void loop()
{ 
  // do timer job
  t.update();
  // DEBUG ASCII ANALYSER
  // *** char gsm modem -> console
  while(gsm_modem.available()) {
    // check incoming char
    int c = gsm_modem.read();
    if (c != 0) {
      rx_buf[rx_index] = c; 
    }  
    // send data ?
    if (c == '\n') {
      // search notice message
      // +CMTI: "SM" for incoming SMS notice : launch sms job
      if (strncmp(rx_buf, "+CMTI: \"SM\"", 11) == 0) {
        jobRxSMS();
      } else {
        // display string like: "rx: 4f[O] 4b[K] 0d[ ] 0a[ ]"
        printf_P(PSTR("rx: "));
        byte i;
        for (i = 0; i <= rx_index; i++)
           printf_P(PSTR("%02x[%c] "), rx_buf[i], (rx_buf[i] > 0x20) ? rx_buf[i] : ' ');
         printf_P(PSTR("\n"));
      }
       rx_index = 0;
       memset(rx_buf, 0, sizeof(rx_buf));
    } else {
      // process data pointer
      rx_index++;
      if (rx_index >= sizeof(rx_buf))
        rx_index = sizeof(rx_buf)-1;
    }    
  }
  // *** char console -> gsm modem
  while(Serial.available()) {
    // check incoming char
    int c = Serial.read();
    if (c != 0) {
      tx_buf[tx_index] = c; 
    }
    // send data ?
    if (c == '\n') {
      // display string like: "tx: 41[A] 54[T] 0a[ ]"
      printf_P(PSTR("tx: "));
      byte i;
      for (i = 0; i <= tx_index; i++) {
        gsm_modem.write(tx_buf[i]);
        printf_P(PSTR("%02x[%c] "), tx_buf[i], (tx_buf[i] > 0x20) ? tx_buf[i] : ' ');
      }
      gsm_modem.write('\r');
      printf_P(PSTR("\n"));
      tx_index = 0;
      memset(tx_buf, 0, sizeof(tx_buf));
    } else {
      // process data pointer
      tx_index++;
      if (tx_index >= sizeof(tx_buf))
        tx_index = sizeof(tx_buf)-1;
    }    
  }
  // set IDLE mode
  idle_delay(WDTO_120MS);
  //idle_delay(WDTO_250MS); i avg = 29,67 ma
}

// *** jobs rotines ***

// job "stat RSSI"
void jobSTAT(void)
{
  // get RSSI, process result
  int _rssi = get_RSSI();
  // check error (rssi = 99 if data not yet available)
  if ((_rssi > -1) & (_rssi != 99)) {
    // convert to dBm (-113 dbm to -51 dbm)
    rssi = (2 * _rssi) - 113;
    // create bar indicator (0 = low level rssi to 5 = high level)
    bar = 0;
    if (rssi >= -110)
      bar = 1;
    if (rssi >= -105)
      bar = 2;
    if (rssi >= -95)
      bar = 3;
    if (rssi >= -83)
      bar = 4;
    if (rssi >= -75)
      bar = 5;
  } else {
    rssi = 0;
    bar  = 0;
  }    
  // call next time
  job_stat = t.after(STAT_UPDATE_INTERVAL, jobSTAT);
}

// job "LCD display"
void jobLCD(void)
{
  // local var
  char buf[20];
  // buttons handler
  uint8_t buttons = lcd.readButtons();
  // set backlight on/off max on time = 60s
  if (lcd_bl_on & ((millis() - lcd_bl_on_t) > 60000UL)) {
    lcd.setBacklight(LCD_BL_OFF);
    lcd_bl_on = false;    
  }
  // on key press
  if (buttons) {
    if (buttons & BUTTON_UP) {
      lcd.setBacklight(LCD_BL_WHITE);
      lcd_bl_on_t = millis();
      lcd_bl_on   = true;
    }
    if (buttons & BUTTON_DOWN) {
      lcd.setBacklight(LCD_BL_OFF);
      lcd_bl_on = false;
    }
  }
  // line 1 : RSSI level
  if (rssi != 0) {
    // display RSSI on LCD panel  
    sprintf_P(buf, PSTR("RSSI: %04d dbm %d"), rssi, bar);
    lcd_line(1, buf);
  } else {
    // if data not available  
    sprintf_P(buf, PSTR("RSSI: n/a"));
    lcd_line(1, buf);    
  }
}

// job "SMS receive polling"
void jobRxSMS(void) 
{
  // local var
   int sms_index;
  char sms_status[16];
  char sms_phonenumber[16]; 
  char sms_datetime[25];
  char sms_msg[128];
  char txt_buffer[128];
  // check SMS index
  Serial.println(F("get_last_SMS_index()"));
  sms_index = get_last_SMS_index();
  if (sms_index > 0) {
    sms_counter++;
    // read SMS
    Serial.println(F("get_SMS"));
    if (get_SMS(sms_index, sms_status, sms_phonenumber, sms_datetime, sms_msg)) {
      Serial.println(sms_index);
      Serial.println(sms_status);
      Serial.println(sms_phonenumber);
      Serial.println(sms_datetime);
      Serial.println(sms_msg);
      // decode SMS
      // sms must begin with xxxx where x is security code
      if (strncmp(sms_msg, TC_PIN, 4) == 0) {
        if (strucasestr(sms_msg, "LED")) {
          digitalWrite(TEST_LED, ! digitalRead(TEST_LED));
          sprintf_P(txt_buffer, PSTR("SMS: TC LED [%d]"), digitalRead(TEST_LED));
          Serial.println(txt_buffer);
          lcd_line(0, txt_buffer);
          sprintf_P(txt_buffer, PSTR("%s\n%s\nLED=%d\n"), "TinyRTU", "TC status", digitalRead(TEST_LED));
          send_SMS(sms_phonenumber, txt_buffer);
        } else if (strucasestr(sms_msg, "INFO")) {
          Serial.println(F("SMS: MSG INFO"));
          lcd_line(0, "SMS: MSG INFO");
          sprintf_P(txt_buffer, PSTR("%s\n%s\nup=%lu s\nrssi=%04d dbm\nsms=%d"), "TinyRTU", "Uptime (in s)", millis()/1000, rssi, sms_counter);
          send_SMS(sms_phonenumber, txt_buffer);          
        }
      } else {
        Serial.println("PIN error");
        lcd_line(0, "SMS: PIN ERR");
      }
    }
    // delete SMS
    Serial.println(F("delete SMS"));
    // flush serial buffer
    gsm_modem.flush();
    // Delete all "read" messages
    gsm_modem.print(F("AT+CMGD=0,1\r"));
    // check result
    if (gsm_modem.find("\r\nOK\r\n")) {
      Serial.println(F("delete OK"));
    } else {
      Serial.println(F("delete error"));
      lcd_line(0, "ERR: del SMS");
    }
  } else {
    Serial.println(F("no SMS"));
  }
  // call next time
  job_sms = t.after(SMS_CHECK_INTERVAL, jobRxSMS);
}

// *** misc routines ***
int send_SMS(char *phone_nb, char *sms_msg) 
{
  // flush serial buffer
  gsm_modem.flush();
  // AT+CMGS="<phone number (like +33320...)>"\r
  gsm_modem.print(F("AT+CMGS=\""));
  gsm_modem.print(phone_nb);
  gsm_modem.print(F("\"\r"));
  // sms message
  gsm_modem.print(sms_msg);
  // end with <ctrl+z>
  gsm_modem.write(0x1A);
  // check result
  if (gsm_modem.find("\r\nOK\r\n"))
    return 1;
  else   
    return 0;
}

int get_last_SMS_index(void) 
{ 
  int sms_index = 0;
  // flush serial buffer
  gsm_modem.flush();
  // list "unread" SMS
  gsm_modem.print(F("AT+CMGL=\"REC UNREAD\",1\r"));
  // parse result 
  if (! gsm_modem.findUntil("+CMGL:", "\nOK\r\n"))
    return 0; 
  sms_index = gsm_modem.parseInt();   
  // clean buffer before return
  while (gsm_modem.findUntil("+CMGL:", "OK\r\n"));
  return sms_index;
}

// read sms equal to sms_index in chip memory
// return status, phonenumber, datetime and msg.
int get_SMS(int sms_index, char *sms_status, char *sms_phonenumber, char *sms_datetime, char *sms_msg) 
{
  // local vars
  int end_index;
  // flush serial buffer
  gsm_modem.flush();
  // AT+CMGR=1\r
  gsm_modem.print(F("AT+CMGR="));
  gsm_modem.print(sms_index);
  gsm_modem.print(F("\r"));
  // parse result
  // +CMGR: "REC READ","+33123456789","","2013/06/29 11:23:35+08"
  if (! gsm_modem.findUntil("+CMGR:", "OK")) 
    return 0; 
  gsm_modem.find("\"");
  end_index = gsm_modem.readBytesUntil('"', sms_status, 16);
  sms_status[end_index] = '\0';
  gsm_modem.find(",\"");
  end_index = gsm_modem.readBytesUntil('"', sms_phonenumber, 16);
  sms_phonenumber[end_index] = '\0';
  gsm_modem.find(",\""); 
  gsm_modem.find("\",\"");
  end_index = gsm_modem.readBytesUntil('"', sms_datetime, 25);
  sms_datetime[end_index] = '\0';
  gsm_modem.find("\r\n");
  end_index = gsm_modem.readBytesUntil('\r', sms_msg, 128);
  sms_msg[end_index] = '\0';
  return 1;  
}

// use "AT+CSQ" for retrieve RSSI
// return 99 if data not yet available
//        0..31 -> -113..-51 dbm
int get_RSSI(void)
{
  // local vars
  int g_rssi;  
  int g_ber;
  // flush serial buffer
  gsm_modem.flush();
  gsm_modem.write("AT+CSQ\r");
  if (gsm_modem.find("+CSQ:")) {
    g_rssi = gsm_modem.parseInt();  
    g_ber  = gsm_modem.parseInt();
  } else {
    return -1;
  } 
  gsm_modem.find("\r\nOK\r\n");
  return g_rssi;
}

// print msg on line nb of the LCD
// pad the line with space char for ensure 16 chars wide
void lcd_line(byte line, char *msg)
{
  int i;
  lcd.setCursor(0, line);
  i = 16 - strlen(msg);
  lcd.print(msg);   
  while (i-- > 0)
    lcd.print(" ");
}

// like std c "strstr" function but not case sensitive
char *strucasestr(char *str1, char *str2)
{
  // local vars  
  char *a, *b;
  // compare str 1 and 2 (not case sensitive)
  while (*str1++) {   
    a = str1;
    b = str2;
    while((*a++ | 32) == (*b++ | 32))
      if(!*b)
        return (str1);
   }
   return 0;
}

// set uc on idle mode (for power saving) with some subsystem disable
// don't disable timer0 use for millis()
void idle_delay(uint8_t wdt_period) {
  wdt_enable(wdt_period);
  wdt_reset();
  WDTCSR |= _BV(WDIE);
  sleep_enable();
  set_sleep_mode(SLEEP_MODE_IDLE);
  power_adc_disable();
  power_spi_disable();
  power_timer1_disable();
  power_timer2_disable();
  power_twi_disable();
  sleep_mode();
  sleep_disable(); 
  power_all_enable();
  wdt_disable();
  WDTCSR &= ~_BV(WDIE);
}

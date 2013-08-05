/*
  Tiny code for manage Arduino official GSM shield (base on Quectel M10 chip).
  
  This example code is in the public domain.
  Share, it's happiness !

  Note : average consumption 35 ma on 12VDC.
  With cards : OLIMEXINO-328 (! 5V select !), official Arduino GSM shield, Adafruit RGB LCD shield.
*/

/* library */
// Arduino core
#include <SoftwareSerial.h>
#include <avr/power.h>
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
#define LCD_UPDATE_INTERVAL    100
#define M10_PWRKEY_PIN 7
#define TEST_LED 13
#define TC_PIN "1111"
#define VERSION "0.3"
#define LCD_BL_OFF    0x0
#define LCD_BL_RED    0x1
#define LCD_BL_YELLOW 0x3
#define LCD_BL_GREEN  0x2
#define LCD_BL_TEAL   0x6
#define LCD_BL_BLUE   0x4
#define LCD_BL_VIOLET 0x5
#define LCD_BL_WHITE  0x7 

// some struct
struct st_SMS {
   int index;
  char status[16];
  char phonenumber[16]; 
  char datetime[25];
  char msg[161];
};

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
byte bar  = 0;
unsigned int sms_counter = 0;
boolean lcd_bl_on = false;
unsigned long lcd_bl_on_t;
byte alive = 0;
char txt_buffer[128];
st_SMS sms;

// link stdout (printf) to Serial object
// create a FILE structure to reference our UART and LCD output function
static FILE uartout = {0};

// create a output function
// This works because Serial.write, although of
// type virtual, already exists.
static int uart_putchar (char c, FILE *stream)
{
  Serial.write(c);
  return 0;
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
  // open serial communications, link Serial to stdio lib
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
  snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("tinyRTU V%s"), VERSION);
  lcd_line(0, txt_buffer);
  printf_P(PSTR("%s\n"), txt_buffer);
  snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("INIT GSM CHIPSET"));
  lcd_line(1, txt_buffer); 
  printf_P(PSTR("%s\n"), txt_buffer);
  // check if gsm chip is on
  while(1) {
    // wait GSM chipset init
    delay_idle(600);
    gsm_modem.flush();
    gsm_modem.print(F("AT\r"));
    if (gsm_modem.find("OK")) {
      break;
    }  
    // large pulse on PWRKEY for switch Quectel M10 chip on at startup
    digitalWrite(M10_PWRKEY_PIN, HIGH);
    delay_idle(2100);
    digitalWrite(M10_PWRKEY_PIN, LOW);
    delay_idle(500);
  }
  snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("INIT GSM OK"));
  lcd_line(1, txt_buffer);  
  printf_P(PSTR("%s\n"), txt_buffer);
  delay_idle(4000);  
  // set modem in text mode (instead of PDU mode)
  gsm_modem.setTimeout(8000);
  gsm_modem.flush();
  gsm_modem.print(F("AT+CMGF=1\r"));
  if (! gsm_modem.find("OK")) {
    snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("ERR: SMS TXT MOD"));
    lcd_line(1, txt_buffer);
    printf_P("%s\n", txt_buffer);
    // loop forever
    while(1)
      cpu_idle();
  }
  // delete all SMS
  // flush serial buffer
  gsm_modem.flush();
  // delete all messages
  gsm_modem.print(F("AT+CMGD=0,4\r"));
  // check result
  if (gsm_modem.find("\r\nOK\r\n")) {
    printf_P(PSTR("FLUSH SMS OK\n"));
  } else {
    snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("ERR: flush SMS"));
    lcd_line(1, txt_buffer);
    printf_P("%s\n", txt_buffer);
    // loop forever
    while(1)
      cpu_idle();
  }
  // no echo local
  gsm_modem.flush();
  gsm_modem.print(F("ATE0\r"));
  if (gsm_modem.find("\r\nOK\r\n"))
    printf_P(PSTR("ECHO LOCAL: OFF\n"));
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
  // DEBUG : ASCII ANALYSER
  // *** char gsm modem -> console
  while(gsm_modem.available()) {
    // check incoming char
    int c = gsm_modem.read();
    if (c != 0)
      rx_buf[rx_index] = c;
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
      if (rx_index < (sizeof(rx_buf)-2))
        rx_index++;
    }
  }
  // *** char console -> gsm modem
  while(Serial.available()) {
    // check incoming char
    int c = Serial.read();
    if (c != 0)
      tx_buf[tx_index] = c; 
    // send data ?
    if (c == '\n') {
      // set "dump" command : print RAM map
      if (strncmp(tx_buf, "DUMP", 4) == 0) {
        print_RAM_map();
      // set "rssi" command : print signal level
      } else if (strncmp(tx_buf, "RSSI", 4) == 0) {
        if (rssi != 0)
          printf_P(PSTR("lvl:%04d dbm %d\n"), rssi, bar);
        else
          printf_P(PSTR("lvl: n/a\n"));
      } else {
        // display string like: "tx: 41[A] 54[T] 0a[ ]"
        printf_P(PSTR("tx: "));
        byte i;
        for (i = 0; i <= tx_index; i++) {
          gsm_modem.write(tx_buf[i]);
          printf_P(PSTR("%02x[%c] "), tx_buf[i], (tx_buf[i] > 0x20) ? tx_buf[i] : ' ');
        }
        gsm_modem.write('\r');
        printf_P(PSTR("\n"));
      }
      tx_index = 0;
      memset(tx_buf, 0, sizeof(tx_buf));    
    } else {
      // process data pointer
      if (tx_index < (sizeof(tx_buf)-2))
        tx_index++;
    }    
  }
  // set IDLE mode for 20 ms
  delay_idle(20);
}

// *** jobs rotines ***

// job "stat RSSI"
void jobSTAT(void)
{
  // get RSSI, process result
  if (rssi = get_RSSI()) {
    // create bar indicator (0 = low level rssi to 5 = high level)
    bar = 0;
    if (rssi >= -110) bar++;
    if (rssi >= -105) bar++;
    if (rssi >= -95)  bar++;
    if (rssi >= -83)  bar++;
    if (rssi >= -75)  bar++;
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
  // DEBUG alive
  if (alive++ >= 9)
    alive = 0;
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
    snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("lvl:%04d dbm %d %d"), rssi, bar, alive);
    lcd_line(1, txt_buffer);
  } else {
    // if data not available  
    snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("lvl: n/a       %d"), alive);
    lcd_line(1, txt_buffer);    
  }
}

// job "SMS receive polling"
void jobRxSMS(void) 
{
  // check SMS index
  printf_P(PSTR("get_last_SMS_index()\n"));
  sms.index = get_last_SMS_index();
  if (sms.index > 0) {
    sms_counter++;
    // read SMS
    printf_P(PSTR("get_SMS\n"));
    //if (get_SMS(sms_index, sms_status, sms_phonenumber, sms_datetime, sms_msg)) {
    if (get_SMS(sms.index)) {
      printf_P(PSTR("%s\n"), sms.index);
      printf_P(PSTR("%s\n"), sms.status);
      printf_P(PSTR("%s\n"), sms.phonenumber);
      printf_P(PSTR("%s\n"), sms.datetime);
      printf_P(PSTR("%s\n"), sms.msg);
      // decode SMS
      // sms must begin with xxxx where x is security code
      if (strncmp(sms.msg, TC_PIN, 4) == 0) {
        if (strucasestr(sms.msg, "LED")) {
          digitalWrite(TEST_LED, ! digitalRead(TEST_LED));
          snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("SMS: TC LED [%d]"), digitalRead(TEST_LED));
          printf_P(PSTR("%s\n"), txt_buffer);
          lcd_line(0, txt_buffer); 
          snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("%s\n%s\nLED=%d\n"), "TinyRTU", "TC status", digitalRead(TEST_LED));
          send_SMS(sms.phonenumber, txt_buffer);
        } else if (strucasestr(sms.msg, "INFO")) {
          snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("SMS: MSG INFO"));
          printf_P(PSTR("%s\n"), txt_buffer);
          lcd_line(0, txt_buffer);
          snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("%s\n%s\nup=%lu s\nrssi=%04d dbm\nsms=%d"), "TinyRTU", "Uptime (in s)", millis()/1000, rssi, sms_counter);
          send_SMS(sms.phonenumber, txt_buffer);          
        }
      } else {
        snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("SMS: PIN ERR"));
        printf_P(PSTR("%s\n"), txt_buffer);
        lcd_line(0, txt_buffer);
      }
    }
    // delete SMS
    printf_P(PSTR("delete SMS\n"));
    // flush serial buffer
    gsm_modem.flush();
    // Delete all "read" messages
    gsm_modem.print(F("AT+CMGD=0,1\r"));
    // check result
    if (gsm_modem.find("\r\nOK\r\n")) {
      printf_P(PSTR("delete OK\n"));
    } else {
      printf_P(PSTR("delete error\n"));
      lcd_line(0, "ERR: del SMS");
    }
  } else {
    printf_P(PSTR("no SMS\n"));
  }
  // call next time
  job_sms = t.after(SMS_CHECK_INTERVAL, jobRxSMS);
}

// *** misc routines ***
boolean send_SMS(char *phone_nb, char *sms_msg) 
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
  return gsm_modem.find("\r\nOK\r\n");
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
int get_SMS(int index) 
{
  // init args
  sms.index = index;
  memset(sms.status, 0, 16);
  memset(sms.phonenumber, 0, 16);
  memset(sms.datetime, 0, 25);
  memset(sms.msg, 0, 128);
  // flush serial buffer
  gsm_modem.flush();
  // AT+CMGR=1\r
  gsm_modem.print(F("AT+CMGR="));
  gsm_modem.print(sms.index);
  gsm_modem.print(F("\r"));
  // parse result
  // +CMGR: "REC READ","+33123456789","","2013/06/29 11:23:35+08"
  if (! gsm_modem.findUntil("+CMGR:", "OK")) 
    return 0; 
  gsm_modem.find("\"");
  gsm_modem.readBytesUntil('"', sms.status, sizeof(sms.status)-1);
  gsm_modem.find(",\"");
  gsm_modem.readBytesUntil('"', sms.phonenumber, sizeof(sms.phonenumber)-1);  
  gsm_modem.find(",\""); 
  gsm_modem.find("\",\"");
  gsm_modem.readBytesUntil('"', sms.datetime, sizeof(sms.datetime)-1);
  gsm_modem.find("\r\n");
  gsm_modem.readBytesUntil('\r', sms.msg, sizeof(sms.msg)-1);
  return 1;  
}

// use "AT+CSQ" for retrieve RSSI
// 
// return O if data not available or command fault
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
    // rssi "99" -> data not yet available
    if (g_rssi != 99) 
      // convert to dBm (0..31 -> -113..-51 dbm)
      g_rssi = (2 * g_rssi) - 113;
    else return 0; 
  } else return 0;
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
// timer0 overflow ISR occur every 256 x 64 clock cyle
// -> for 16 MHz clock every 1,024 ms, this wake up the "uc" from idle sleep
void cpu_idle(void)
{
  sleep_enable();
  set_sleep_mode(SLEEP_MODE_IDLE);
  power_adc_disable();
  power_spi_disable();
  power_timer1_disable();
  power_timer2_disable();
  power_twi_disable();
  sleep_mode(); // go sleep here
  sleep_disable(); 
  power_all_enable();
}

// set cpu in idle mode for ms milliseconds
void delay_idle(unsigned long ms)
{
  unsigned long _now = millis();
  while (millis() - _now < ms)
    cpu_idle();
}

// DEBUG
// see  http://www.nongnu.org/avr-libc/user-manual/malloc.html
void print_RAM_map(void) 
{
  // local var
  char stack = 1;
  // external symbol
  extern char *__data_start;
  extern char *__data_end;
  extern char *__bss_start;
  extern char *__bss_end;
  extern char *__heap_start;
  extern char *__heap_end;
  // sizes compute
  int data_size   = (int)&__data_end - (int)&__data_start;
  int bss_size    = (int)&__bss_end - (int)&__data_end;
  int heap_end    = (int)&stack - (int)&__malloc_margin;
  int heap_size   = heap_end - (int)&__bss_end;
  int stack_size  = RAMEND - (int)&stack + 1;
  int free_memory = (RAMEND - (int)&__data_start + 1) - (data_size + bss_size + heap_size + stack_size);
  // print MAP
  printf_P(PSTR("RAM map\n"));
  printf_P(PSTR("-------\n\n"));  
  printf_P(PSTR("+----------------+  __data_start  = %d\n"), (int)&__data_start);
  printf_P(PSTR("+      data      +\n"));
  printf_P(PSTR("+    variables   +  data_size     = %d\n"), data_size);
  printf_P(PSTR("+   (with init)  +\n"));
  printf_P(PSTR("+----------------+  __data_end    = %d\n"), (int)&__data_end);
  printf_P(PSTR("+----------------+  __bss_start   = %d\n"), (int)&__bss_start);
  printf_P(PSTR("+       bss      +\n"));
  printf_P(PSTR("+    variables   +  bss_size      = %d\n"), bss_size);
  printf_P(PSTR("+    (no init)   +\n"));
  printf_P(PSTR("+----------------+  __bss_end     = %d\n"), (int)&__bss_end);
  printf_P(PSTR("+----------------+  __heap_start  = %d\n"), (int)&__heap_start);
  printf_P(PSTR("+                +\n"));
  printf_P(PSTR("+      heap      +  heap_size     = %d\n"), heap_size);
  printf_P(PSTR("+    (dyn var)   +\n"));
  printf_P(PSTR("+----------------+  heap_end      = %d\n"), heap_end);
  printf_P(PSTR("+                +\n"));
  printf_P(PSTR("+    free mem    +  free          = %d\n"), free_memory);
  printf_P(PSTR("+                +\n"));
  printf_P(PSTR("+----------------+  Current STACK = %d\n"), (int)&stack);
  printf_P(PSTR("+      stack     +\n"));
  printf_P(PSTR("+    (sub arg,   +  stack_size    = %d\n"), stack_size);
  printf_P(PSTR("+     loc var)   +\n"));
  printf_P(PSTR("+----------------+  RAMEND        = %d\n"), RAMEND);  
  printf_P(PSTR("\n\n"));
}

/*
  Tiny code for manage SMS and RSSI on Arduino official GSM shield (base on Quectel M10 chip).
  
  - project home : https://github.com/sourceperl/arduino.gsm_sms.git
  
  This example code is in the public domain.
  Share, it's happiness !
*/

/* library */
// Arduino core
#include <SoftwareSerial.h>
#include "Wire.h"
// Timer : must use at least v1.1 for fix millis rolover issue
// come from https://github.com/JChristensen/Timer
#include "Timer.h"
#include <stdio.h>

// some const
#define M10_PWRKEY_PIN 7
#define TEST_LED 13

// some vars
SoftwareSerial gsm_modem(2, 3); // RX, TX
Timer t;
int job_rssi;
int job_sms_poll;
int job_sms_report;

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

void setup()  
{  
  // IO setup
  pinMode(M10_PWRKEY_PIN, OUTPUT);
  digitalWrite(M10_PWRKEY_PIN, LOW);
  pinMode(TEST_LED, OUTPUT);
  digitalWrite(TEST_LED, HIGH);
  // open serial communications and wait for port to open:
  Serial.begin(9600);
  // fill in the UART file descriptor with pointer to writer.
  fdev_setup_stream (&uartout, uart_putchar, NULL, _FDEV_SETUP_WRITE);
  // The uart is the standard output device STDOUT.
  stdout = &uartout;
  // set the data rate for the SoftwareSerial port
  gsm_modem.begin(9600);
  gsm_modem.setTimeout(3000);  
  // check if gsm chip is on
  while(1) {
    // wait GSM chipset init
    delay(500);
    gsm_modem.flush();
    gsm_modem.print(F("AT\r"));
    if (gsm_modem.find("OK"))
      break;
    // large pulse on PWRKEY for switch Quectel M10 chip on at startup
    Serial.println(F("chip M10 seem off : init PWRKEY pulse"));
    digitalWrite(M10_PWRKEY_PIN, HIGH);
    delay(2100);
    digitalWrite(M10_PWRKEY_PIN, LOW);
    delay(500);
  }
  Serial.println(F("init gsm ok")); 
  // set modem in text mode (instead of PDU mode)
  gsm_modem.flush();
  gsm_modem.print(F("AT+CMGF=1\r"));
  if (! gsm_modem.find("OK")) {
    Serial.println(F("error: SMS txt set mode"));
    while(1);
  }  
  // init timer job
  job_rssi       = t.every(3000, jobRSSI);
  job_sms_poll   = t.after(2000, jobRxSMS);
  //job_sms_report = t.every(3600000, jobSendSMS);
}

void loop()
{
  // do timer job
  t.update();
  // serial <-> gsm_modem
  while(gsm_modem.available()) {
    int c = gsm_modem.read();
    Serial.write(c);
  }
  while(Serial.available())
    gsm_modem.write(Serial.read());
}

void jobSendSMS(void) 
{
  send_SMS("+33123456789", "SMS hourly report message"); 
}

void jobRSSI(void)
{
  // local vars
  int rssi;
  int bar;
  // get RSSI, process result
  rssi = get_RSSI();
  // check error (rssi = 99 if data not yet available)
  if ((rssi > -1) & (rssi != 99)) {
    // convert to dBm (-113 dbm to -51 dbm)
    rssi = (2 * rssi) - 113;
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
    // display RSSI on console  
    fprintf( &uartout, "RSSI: %04d dbm %d\n", rssi, bar);
  }  
}

// job "SMS receive polling"
void jobRxSMS(void) 
{
  // local var
   int sms_index;
  char sms_status[10];
  char sms_phonenumber[16]; 
  char sms_datetime[25];
  char sms_msg[127];
  char txt_buffer[127];
  // check SMS index
  sms_index = get_last_SMS_index();
  if (sms_index > 0) {
    Serial.print("last SMS index : ");
    Serial.println(sms_index);
    // read SMS
    if (get_SMS(sms_index, sms_status, sms_phonenumber, sms_datetime, sms_msg)) {
      // decode SMS
      if (strucasestr(sms_msg, "command 1"))
        Serial.println("receive command 1");
      else if (strucasestr(sms_msg, "command 2"))
        Serial.println("receive command 2");
      else {
        Serial.println("error : not a command");
      }      
    }
    // delete SMS
    if (delete_SMS(sms_index)) {
      Serial.println("delete SMS OK");
    } else {
      Serial.println("delete SMS error");
    }
    // wait for ensure SMS remove from chip (sometime one SMS is process 2 times without this)
    delay(500);
  } else {
    Serial.println("no SMS");
  }
  // call next in 2000 ms
  job_sms_poll = t.after(2000, jobRxSMS);
}

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

int delete_SMS(int sms_index) 
{
  // flush serial buffer
  gsm_modem.flush();
  // AT+CMGD="<SMS index>"\r
  gsm_modem.print(F("AT+CMGD="));
  gsm_modem.print(sms_index);
  gsm_modem.print(F("\r"));
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
  // AT+CMGL="ALL"\r
  gsm_modem.print(F("AT+CMGL=\"ALL\"\r"));
  // parse result 
  if (! gsm_modem.findUntil("+CMGL:", "OK"))
    return 0; 
  sms_index = gsm_modem.parseInt();   
  // clean buffer before return
  while (gsm_modem.findUntil("+CMGL:", "\r\nOK\r\n"));
  return sms_index;
}

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
  end_index = gsm_modem.readBytesUntil('"', sms_status, 10);
  sms_status[end_index] = '\0';
  gsm_modem.find(",\"");
  end_index = gsm_modem.readBytesUntil('"', sms_phonenumber, 16);
  sms_phonenumber[end_index] = '\0';
  gsm_modem.find(",\""); 
  gsm_modem.find("\",\"");
  end_index = gsm_modem.readBytesUntil('"', sms_datetime, 25);
  sms_datetime[end_index] = '\0';
  gsm_modem.find("\r");
  end_index = gsm_modem.readBytesUntil('\r', sms_msg, 127);
  sms_msg[end_index] = '\0';
  return 1;  
}

int get_RSSI(void)
{
  // local vars
  int rssi;  
  int ber;
  // flush serial buffer
  gsm_modem.flush();
  gsm_modem.write("AT+CSQ\r");
  if (gsm_modem.find("+CSQ:")) {
    rssi = gsm_modem.parseInt();  
    ber  = gsm_modem.parseInt();
  } else {
    return -1;
  } 
  gsm_modem.find("\r\nOK\r\n");
  return rssi;
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

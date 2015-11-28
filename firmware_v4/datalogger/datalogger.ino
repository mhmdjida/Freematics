/*************************************************************************
* OBD-II/MEMS/GPS Data Logging Sketch for Freematics ONE
* Distributed under GPL v2.0
* Visit http://freematics.com for more information
* Developed by Stanley Huang <stanleyhuangyc@gmail.com>
*************************************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <I2Cdev.h>
#include <MPU9150.h>
#include <Narcoleptic.h>
#include "config.h"
#if ENABLE_DATA_LOG
#include <SD.h>
#endif
#include "Freematics.h"
#include "datalogger.h"

// logger states
#define STATE_SD_READY 0x1
#define STATE_OBD_READY 0x2
#define STATE_GPS_FOUND 0x4
#define STATE_GPS_READY 0x8
#define STATE_MEMS_READY 0x10
#define STATE_SLEEPING 0x20

#if VERBOSE && !ENABLE_DATA_OUT
#define SerialInfo Serial
#else
#define SerialInfo SerialRF
#endif

void(* resetFunc) (void) = 0; //declare reset function at address 0

static uint8_t lastFileSize = 0;
static uint16_t fileIndex = 0;

static const byte PROGMEM pidTier1[]= {PID_RPM, PID_SPEED, PID_ENGINE_LOAD, PID_THROTTLE};
static const byte PROGMEM pidTier2[] = {PID_COOLANT_TEMP, PID_INTAKE_TEMP, PID_DISTANCE};

#define TIER_NUM1 sizeof(pidTier1)
#define TIER_NUM2 sizeof(pidTier2)

#if USE_MPU6050 || USE_MPU9150
MPU6050 accelgyro;
#endif

static const byte PROGMEM parts[][3] = {
  {'S','D'},
  {'A','C','C'},
  {'O','B','D'},
  {'G','P','S'},
};

typedef enum {
  PART_SD = 0,
  PART_MEMS,
  PART_OBD,
  PART_GPS,
} PART_ID;

class CLogger : public COBDSPI, public CDataLogger
{
public:
    CLogger():state(0) {}
    void showStatus(byte partID, bool OK)
    {
#if ENABLE_DATA_OUT
      char buf[4];
      memcpy_P(buf, parts[partID], 3);
      buf[3] = 0;
      SerialRF.print(buf);
      SerialRF.print(' ');
      SerialRF.println(OK ? "OK" : "NO");
#endif
    }
    void setup()
    {
        bool success;
        state = 0;
        
#if ENABLE_DATA_LOG
        uint16_t volsize = initSD();
        if (volsize) {
          SerialRF.print("SD ");
          SerialRF.print(volsize);
          SerialRF.println("MB");
          success = openLogFile() != 0;
        }
        showStatus(PART_SD, success);
#endif

#if USE_MPU6050 || USE_MPU9150
        Wire.begin();
        accelgyro.initialize();
        if (success = accelgyro.testConnection()) {
          state |= STATE_MEMS_READY;
        }
        showStatus(PART_MEMS, success);
#endif

#if OBD_UART_BAUDRATE
        setBaudRate(OBD_UART_BAUDRATE);
#endif

#if USE_GPS
        if (success = initGPS(GPS_SERIAL_BAUDRATE)) {
            state |= STATE_GPS_FOUND;
        }
        showStatus(PART_GPS, success);
#endif

        if (success = init()) {
            state |= STATE_OBD_READY;
        }
        showStatus(PART_OBD, success);
        
        delay(3000);
    }
#if USE_GPS
    void logGPSData()
    {
#if LOG_GPS_NMEA_DATA
        // issue the command to get NMEA data (one line per request)
        char buf[128];
        if (sendCommand("ATGRR\r", buf, sizeof(buf))) {
            logData(buf + 4);
        }
#endif
#if LOG_GPS_PARSED_DATA
        // issue the command to get parsed GPS data
        // the return data is in following format:
        // $GPS,date,time,lat,lon,altitude,speed,course,sat
        static GPS_DATA gd = {0};
        byte mask = 0;
        byte index = -1;
        char buf[12];
        byte n = 0;
        write("ATGPS\r");
        dataTime = millis();
        for (uint32_t t = dataTime; millis() - t < GPS_DATA_TIMEOUT; ) {
            if (!available()) continue;
            char c = read();
#if VERBOSE
            logData(c);
#endif
            if (c != ',' && c != '>') {
              if (n < sizeof(buf) - 1) buf[n++] = c;
              continue;
            }
            buf[n] = 0;
            if (index == -1) {
                // need to verify header
                if (strcmp(buf + n - 4, "$GPS") == 0) {
                  index = 0;
                }
            } else {
                long v = atol(buf);
                switch (index) {
                case 0:
                    if (gd.date != v) {
                      int year = v % 100;
                      // filter out invalid date
                      if (v < 1000000 && v >= 10000 && year >= 15 && (gd.date == 0 || year - (gd.date % 100) <= 1)) {
                        gd.date = v;
                        mask |= 0x1;
                      }
                    }
                    break;
                case 1:
                    if (gd.time != v) {
                      gd.time = v;
                      mask |= 0x2;
                    }                            
                    break;
                case 2:
                    if (gd.lat != v) {
                      gd.lat = v;
                      mask |= 0x4;
                    }
                    break;
                case 3:
                    if (gd.lon != v) {
                      gd.lon = v;
                      mask |= 0x8;
                    }
                    break;
                case 4:
                    if (gd.alt != (int)v) {
                      gd.alt = (int)v;
                      mask |= 0x10;
                    }
                    break;
                case 5:
                    if (gd.speed != (byte)v) {
                      gd.speed = (byte)v;
                      mask |= 0x20;
                    }
                    break;
                case 6:
                    if (gd.heading != (int)v) {
                      gd.heading = (int)v;
                      mask |= 0x40;
                    }
                    break;
                case 7:
                    if (gd.sat != (byte)v) {
                      gd.sat = (byte)v;
                      mask |= 0x80;
                    }
                    break;
                }
                index++;
            }
            n = 0;
            if (c == '>') {
                // prompt char received, now process data
                if (mask) {
                  // something has changed
                  if (mask & 0x1) logData(PID_GPS_DATE, gd.date);
                  if (mask & 0x2) logData(PID_GPS_TIME, gd.time);
                  if (mask & 0x4) logData(PID_GPS_LATITUDE, gd.lat);
                  if (mask & 0x8) logData(PID_GPS_LONGITUDE, gd.lon);
                  if (mask & 0x10) logData(PID_GPS_ALTITUDE, gd.alt);
                  if (mask & 0x20) logData(PID_GPS_SPEED, gd.speed);
                  if (mask & 0x40) logData(PID_GPS_HEADING, gd.heading);
                  if (mask & 0x80) logData(PID_GPS_SAT_COUNT, gd.sat);
                }                        
                // discard following data if any
                while (available()) read();
                break;
            }
        }
#endif
    }
#endif
#if USE_MPU6050 || USE_MPU9150
    void logMEMSData()
    {
        if (!(state & STATE_MEMS_READY))
            return;

#if USE_MPU9150
        int16_t mems[3][3] = {0};
        int16_t mx, my, mz;
        accelgyro.getMotion9(&mems[0][0], &mems[0][1], &mems[0][2], &mems[1][0], &mems[1][1], &mems[1][2], &mems[2][0], &mems[2][1], &mems[2][2]);
        dataTime = millis();
        // assume PID_GYRO = PID_AAC + 1, PID_MAG = PID_AAC + 2
        for (byte n = 0; n < 3; n++) {
          logData(PID_ACC + n, mems[n][0] >> 4, mems[n][1] >> 4, mems[n][2] >> 4);
        }
        
#else
        int16_t mems[2][3] = {0};
        accelgyro.getMotion6(&mems[0][0], &mems[0][1], &mems[0][2], &mems[1][0], &mems[1][1], &mems[1][2]);
        dataTime = millis();
        for (byte n = 0; n < 2; n++) {
          logData(PID_ACC + n, mems[n][0] >> 4, mems[n][1] >> 4, mems[n][2] >> 4);
        }
#endif
    }
#endif
#if ENABLE_DATA_LOG
    int openLogFile()
    {
        uint16_t index = openFile();
        if (!index) {
            delay(1000);
            index = openFile();
        }
        if (index) {
            if (sdfile.println(ID_STR) > 0) {
              state |= STATE_SD_READY;
            } else {
              index = 0;
            }
        }
#if VERBOSE
        SerialInfo.print("File ID: ");
        SerialInfo.println(index);
        delay(3000);
#endif
        return index;
    }
    uint16_t initSD()
    {
        state &= ~STATE_SD_READY;
        pinMode(SS, OUTPUT);
        Sd2Card card;
        uint32_t volumesize = 0;
        if (card.init(SPI_HALF_SPEED, SD_CS_PIN)) {
#if VERBOSE
            const char* type;
            switch(card.type()) {
            case SD_CARD_TYPE_SD1:
                type = "SD1";
                break;
            case SD_CARD_TYPE_SD2:
                type = "SD2";
                break;
            case SD_CARD_TYPE_SDHC:
                type = "SDHC";
                break;
            default:
                type = "SDx";
            }

            SerialInfo.print("SD type: ");
            SerialInfo.print(type);

            SdVolume volume;
            if (!volume.init(card)) {
                SerialInfo.println(" No FAT!");
                return 0;
            }

            volumesize = volume.blocksPerCluster();
            volumesize >>= 1; // 512 bytes per block
            volumesize *= volume.clusterCount();
            volumesize /= 1000;
            SerialInfo.print(" SD size: ");
            SerialInfo.print((int)((volumesize + 511) / 1000));
            SerialInfo.println("GB");
            delay(3000);
#else
            SdVolume volume;
            if (volume.init(card)) {
              volumesize = volume.blocksPerCluster();
              volumesize >>= 1; // 512 bytes per block
              volumesize *= volume.clusterCount();
              volumesize /= 1000;
            }
#endif
        }
        if (SD.begin(SD_CS_PIN)) {
          return volumesize; 
        } else {
          return 0;
        }
    }
    void flushData()
    {
        // flush SD data every 1KB
        byte dataSizeKB = dataSize >> 10;
        if (dataSizeKB != lastFileSize) {
#if VERBOSE
            // display logged data size
            SerialInfo.print(dataSize);
            SerialInfo.println(" bytes");
#endif
            flushFile();
            lastFileSize = dataSizeKB;
#if MAX_LOG_FILE_SIZE
            if (dataSize >= 1024L * MAX_LOG_FILE_SIZE) {
              closeFile();
              if (openLogFile() == 0) {
                  state &= ~STATE_SD_READY;
              }
            }
#endif
        }
    }
#endif
    void reconnect()
    {
#if ENABLE_DATA_LOG
        closeFile();
#endif
        state &= ~STATE_OBD_READY;
#if VERBOSE
        SerialInfo.print("Retry");
#endif
        byte n = 0;
        bool toReset = false;
        while (!init()) {
#if VERBOSE
            SerialInfo.write('.');
#endif
            Narcoleptic.delay(3000);
            if (n >= 20) {
              toReset = true;
            } else {
              n++; 
            }
        }
        if (toReset) resetFunc();
    }
    bool logOBDData(byte pid)
    {
        int value;
        if (!read(pid, value)) {
            // error occurred
            recover();
            errors++;
            return false;
        }
        dataTime = millis();
        logData((uint16_t)pid | 0x100, value);
        errors = 0;
        return true;
    }
    void dataIdleLoop()
    {
      if (m_state != OBD_CONNECTED) return;
#if ENABLE_DATA_LOG
      flushData();
#endif
#if USE_MPU6050 || USE_MPU9150
      logMEMSData();
#endif
    }
    byte state;
};

static CLogger logger;

void setup()
{
#if VERBOSE
    SerialInfo.begin(STREAM_BAUDRATE);
#endif
    logger.initSender();
    logger.begin();
    logger.setup();
}

void upgradeFirmware()
{
  Serial.setTimeout(30000);
  for (;;) {
    // read data into string until '\r' encountered
    String s = Serial.readStringUntil('\r');
    if (s.length() == 0) {
      // no data received
      Serial.println("TIMEOUT");
      break;
    } else if (s == "$UPD") {
      // empty data chunk received
      Serial.println("END");
      delay(500);
      // reset 328
      resetFunc();
      break; 
    }
    // send via SPI
    logger.write(s.c_str());
    s = "";
    // receive from SPI and forward to serial
    char buffer[64];
    byte n = logger.receive(buffer, sizeof(buffer), 3000);
    if (n) Serial.write(buffer, n);
  }
}

void loop()
{
    // check serial inbound data
    if (SerialRF.available()) {
      if (Serial.read() == '#' && Serial.read() == '#') {
        Serial.println("OK");
        upgradeFirmware();
      }      
    }
    if (logger.state & STATE_OBD_READY) {
        static byte index2 = 0;
        for (byte n = 0; n < TIER_NUM1; n++) {
          byte pid = pgm_read_byte(pidTier1 + n);
          logger.logOBDData(pid);
        }
        byte pid = pgm_read_byte(pidTier2 + index2);
        logger.logOBDData(pid);
        index2 = (index2 + 1) % TIER_NUM2;
        if (logger.errors >= 10) {
            logger.reconnect();
        }
    } else if (!OBD_ATTEMPT_TIME || millis() < OBD_ATTEMPT_TIME * 1000) {
        if (logger.init()) {
            logger.state |= STATE_OBD_READY;
        }
    }

#if USE_GPS
    if (logger.state & STATE_GPS_FOUND) {
        logger.logGPSData();
    }
#endif
}
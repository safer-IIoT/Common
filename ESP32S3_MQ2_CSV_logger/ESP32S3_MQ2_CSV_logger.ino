#include <WiFi.h>
#include <time.h>
#include "config.h"
#include "FS.h"
#include "LittleFS.h"
#define LOG_FS LittleFS

constexpr size_t N_COLUMNS  = sizeof(CSV_COLUMNS) / sizeof(CSV_COLUMNS[0]);
constexpr size_t N_READINGS = 15;

constexpr size_t MIN_FREE_BYTES_FOR_LOGS = 32768;

double readings[N_READINGS];
size_t nReadings  = 0;
unsigned long lastReadMs = 0;
bool fsMounted = false;

bool timeSynced = false;
static uint32_t lastPrintTime = 0;
static uint32_t lastRetryTime = 0;

String getFormattedTimestamp();
void handleWiFi();

void writeLog(const char* level, const String& message) 
{
  Serial.printf("[%s] %s\n", level, message.c_str());

  if (!fsMounted) 
    return;

  // leave at least 32 KB free for LittleFS overhead
  if (LittleFS.totalBytes() - LittleFS.usedBytes() < 32768) 
  {
    Serial.println("[WARN] LittleFS low on space! Skipping flash log write.");
    return;
  }

  char filePath[32];
  char timePrefix[24];

  if (timeSynced) 
  {
    time_t now = time(nullptr) + GMT_OFFSET_SEC;
    struct tm t;
    gmtime_r(&now, &t);
    // Format filename: /info-yyyyMMdd.txt or /error-yyyyMMdd.txt
    snprintf(filePath, sizeof(filePath), "/%s-%04d%02d%02d.txt", level, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    // Format line timestamp: [HH:MM:SS]
    snprintf(timePrefix, sizeof(timePrefix), "[%02d:%02d:%02d]", t.tm_hour, t.tm_min, t.tm_sec);
  } 
  else 
  {
    snprintf(filePath, sizeof(filePath), "/%s-unsynced.txt", level);
    snprintf(timePrefix, sizeof(timePrefix), "[+%lums]", millis());
  }

  File file;

  if (!LittleFS.exists(filePath)) 
  {
    file = LittleFS.open(filePath, FILE_WRITE);
  } 
  else 
  {
    file = LittleFS.open(filePath, FILE_APPEND);
  }

  if (file) 
  {
    file.printf("%s %s\n", timePrefix, message.c_str());
    file.close();
  } 
  else 
  {
    Serial.printf("[FS] Failed to open %s for appending\n", filePath);
  }
}

void logInfo(const String& msg)
{ 
  writeLog("info", msg); 
}

void logError(const String& msg) 
{ 
  writeLog("error", msg); 
}

bool ensureCsvFile() 
{
  if (LOG_FS.exists(CSV_FILE_NAME)) 
  {
    File f = LOG_FS.open(CSV_FILE_NAME, FILE_READ);
    size_t size = f ? f.size() : 0;
    
    if (f) 
      f.close();

    if (size > 0) 
      return true;
  }

  File f = LOG_FS.open(CSV_FILE_NAME, FILE_WRITE);
  
  if (!f) 
    return false;

  for (size_t i = 0; i < N_COLUMNS; i++) 
  {
    if (i) 
      f.print(',');

    f.print(CSV_COLUMNS[i]);
  }

  f.print('\n');
  f.close();
  Serial.printf("Created %s with header\n", CSV_FILE_NAME);

  return true;
}

void handleWiFi() 
{
  uint32_t currentTime = millis();

  if (WiFi.status() == WL_CONNECTED)
  {
    if (currentTime - lastPrintTime > 10000)
    {
      lastPrintTime = currentTime;
    }

    return;
  }

  if (currentTime - lastPrintTime > 3000) 
  {
    logError("Wi-Fi is not available");

    lastPrintTime = currentTime;
    int currentStatus = WiFi.status();
    Serial.printf("Wi-Fi Status Code: %d\n", currentStatus);

    if (currentTime - lastRetryTime > 7000)
    {
      lastRetryTime = currentTime;
      logInfo("Re-issuing fresh Wi-Fi connect request");
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }
}

String getFormattedTimestamp() 
{
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) 
  {
    Serial.println("Failed to obtain local time from internal clock");
    return "";
  }

  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", &timeinfo);

  return String(buffer);
}

void handleNTP() 
{
  if (timeSynced || WiFi.status() != WL_CONNECTED)
    return;

  static bool     ntpStarted   = false;
  static uint32_t lastNtpCheck = 0;

  if (!ntpStarted)
  {
    configTime(GMT_OFFSET_SEC, 0, NTP_SERVER);
    ntpStarted = true;
  }

  if (time(nullptr) > 1700000000)
  {
    timeSynced = true; 
    logInfo("Clock set from NTP");
  }
  else if (millis() - lastNtpCheck > 5000)
  {
    lastNtpCheck = millis();
    Serial.println("NTP: time not set yet");
  }
}

void formatTimestamp(char* out, size_t n) 
{
  time_t now = time(nullptr);

  if (now > 1600000000)
  {
    struct tm t;
    localtime_r(&now, &t);
    strftime(out, n, "%Y-%m-%d %H:%M:%S", &t);
  } 
  else 
  {
    snprintf(out, n, "%lu", millis() / 1000);
  }
}

bool appendRow() 
{
  if (!ensureCsvFile())
    return false;

  File f = LOG_FS.open(CSV_FILE_NAME, FILE_APPEND);
  if (!f) 
    return false;

  char ts[24];
  formatTimestamp(ts, sizeof(ts));
  f.print(ts);

  for (size_t i = 0; i < N_READINGS; i++) 
  {
    f.print(',');
    f.print(readings[i], DECIMALS);
  }

  f.print(",\n");
  f.close();
  logInfo("Row written");
  return true;
}

void handleSerial() 
{
  while (Serial.available()) 
  {
    char c = Serial.read();
    
    if (c == 'p')
    {
      File f = LOG_FS.open(CSV_FILE_NAME, FILE_READ);

      if (!f) 
      { 
        Serial.println("No file"); 
        continue;
      }

      Serial.printf("----- %s (%u bytes) -----\n", CSV_FILE_NAME, (unsigned)f.size());
      
      while (f.available()) 
        Serial.write(f.read());

      f.close();
    } 
    else if (c == 'x') 
    {
      LOG_FS.remove(CSV_FILE_NAME);
      logInfo("File deleted");
      ensureCsvFile();
    }
  }
}

void connectWiFi() 
{
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void setup() 
{
  logInfo("Serial initialization started");

  Serial.begin(115200);
  delay(500);

  logInfo("Serial initialization ended");

  analogReadResolution(12);

  if (LittleFS.begin(false)) 
  {
    fsMounted = true;
    logInfo("LittleFS mounted successfully");
  } 
  else 
  {
    logError("LittleFS mount failed!");
  }
  
  if (!ensureCsvFile())
  {
    logError("Could not create the CSV file");
  }
  
  logInfo("Wi-Fi initialization started");

  connectWiFi();

  logInfo("Wi-Fi initialization ended");
  
  lastRetryTime = millis();
  lastReadMs = millis();
}

void loop() 
{  
  handleSerial();

  handleWiFi();

  handleNTP();

  if (!timeSynced)
  {
    nReadings  = 0;
    lastReadMs = millis();
    return;
  }

  if (millis() - lastReadMs < READ_INTERVAL_MS) 
    return;

  lastReadMs += READ_INTERVAL_MS;

  int raw = analogRead(GAS_PIN);
  readings[nReadings++] = constrain(raw * 100.0 / ADC_FULL_SCALE, 0.0, 100.0);

  if (nReadings == N_READINGS) 
  {
    if (!appendRow()) 
      logError("Write failed");
    nReadings = 0;
  }
}
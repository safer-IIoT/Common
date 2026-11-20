#include <WiFi.h>
#include <time.h>
#include "FS.h"
#include "LittleFS.h"
#define LOG_FS LittleFS
#include "esp_partition.h"
#include <math.h>

#include "config.h"
#include "secretconfig.h"

constexpr size_t N_COLUMNS  = sizeof(CSV_COLUMNS) / sizeof(CSV_COLUMNS[0]);
constexpr size_t N_READINGS = 15;
constexpr size_t MAX_VALUES = 32;
constexpr size_t RX_LINE_LEN = 512;

constexpr size_t MIN_FREE_BYTES_FOR_LOGS = 32768;

double readings[N_READINGS];
unsigned long lastReadMs = 0;
bool fsMounted = false;
char   lineBuffer[RX_LINE_LEN];
size_t lineLength = 0;
bool   inFrame = false;

double   rxValues[MAX_VALUES];
size_t   rxCount    = 0;
bool     rxFresh    = false;
uint32_t validFrames   = 0;
uint32_t invalidFrames  = 0;

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

bool areAllZeros(const double* arr, size_t size, double epsilon = 1e-9) 
{
    for (size_t i = 0; i < size; i++) 
    {
        if (fabs(arr[i]) > epsilon) 
        {
            return false;
        }
    }

    return true;
}

void appendRow() 
{
  bool zeroReadings = areAllZeros(readings, N_READINGS);

  if(!zeroReadings && timeSynced)
  {
    if (ensureCsvFile())
    {
      File f = LOG_FS.open(CSV_FILE_NAME, FILE_APPEND);
      
      if (!f)
        logError("Failed to open file to write sensor data");

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
    }
    else
    {
      logError("Error occurred in write sensor data");
    }      
  }
  else
  {
    logInfo("All sensor readings are zero or time is unsynced. Thus, no row was written.");
  } 
}

int hexValue(char c) 
{
  if (c >= '0' && c <= '9') 
    return c - '0';
  if (c >= 'A' && c <= 'F') 
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') 
    return c - 'a' + 10;
  return -1;
}

bool parseFrame(char* s, size_t len, double* out, size_t maxOut, size_t& count) 
{
  if (len && s[len - 1] == '\r') 
    s[--len] = '\0';

  char* star = strchr(s, '*');

  if (!star || (size_t)(s + len - star) != 3) 
    return false;

  int high = hexValue(star[1]), low = hexValue(star[2]);

  if (high < 0 || low < 0) 
    return false;

  uint8_t cs = 0;

  for (char* p = s; p < star; p++) 
    cs ^= (uint8_t)*p;

  if (cs != (uint8_t)((high << 4) | low)) 
    return false;

  *star = '\0';
  count = 0;
  char* p = s;

  while (true) 
  {
    if (count >= maxOut) 
      return false;

    char* end;
    double d = strtod(p, &end);

    if (end == p) 
      return false;

    out[count++] = d;

    if (*end == ',') 
      p = end + 1;
    else if (*end == '\0') 
      break;
    else 
      return false;
  }

  return true;
}

void onReceiveValues(const double* v, size_t n) 
{
  Serial.printf("Received values(%u):", (unsigned)n);

  for (size_t i = 0; i < n; i++)
  {
    readings[i] = v[i];
    Serial.printf(" %.4f", v[i]);
  }

  Serial.printf(" | Valid=%lu Invalid=%lu\n", (unsigned long)validFrames, (unsigned long)invalidFrames);

  appendRow();
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

void pollLink() 
{
  size_t maxReads = 128; 
  while (Serial1.available() && maxReads-- > 0) 
  {
    char c = (char)Serial1.read();

    if (c == '$') 
    { 
      inFrame = true; 
      lineLength = 0; 
      continue; 
    }

    if (!inFrame) 
      continue;

    if (c == '\n') 
    {
      inFrame = false;
      lineBuffer[lineLength] = '\0';
      double tmp[MAX_VALUES];
      size_t n = 0;

      if (parseFrame(lineBuffer, lineLength, tmp, MAX_VALUES, n)) 
      {
        memcpy(rxValues, tmp, n * sizeof(double));
        rxCount = n;
        rxFresh = true;
        validFrames++;
        onReceiveValues(rxValues, rxCount);
      } 
      else 
      {
        invalidFrames++;
      }

      continue;
    }

    if (lineLength >= RX_LINE_LEN - 1) 
    {  
      inFrame = false; 
      invalidFrames++; 
      continue; 
    }

    lineBuffer[lineLength++] = c;
  }
}

void connectWiFi() 
{
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void dumpFS(const char* tag)
{
  const esp_partition_t* p = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "spiffs");

  Serial.printf("[%s] spiffs partition: %s @0x%06x size 0x%x\n", tag,
                p ? "found" : "MISSING", p ? p->address : 0, p ? p->size : 0);
  Serial.printf("[%s] LittleFS used %u / %u bytes\n", tag,
                LittleFS.usedBytes(), LittleFS.totalBytes());

  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile())
    Serial.printf("  %s  %u\n", f.path(), f.size());
}

void setup() 
{
  logInfo("Serial initialization started");

  Serial.begin(115200);
  delay(500);

  logInfo("Serial initialization ended");

  Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
  Serial1.setRxBufferSize(1024);

  if (LittleFS.begin(false)) 
  {
    fsMounted = true;
    logInfo("LittleFS mounted successfully");
    //dumpFS("controller");
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
  pollLink();

  handleSerial();

  handleWiFi();

  handleNTP();

  if (!timeSynced)
  {
    lastReadMs = millis();
    return;
  }

  if (millis() - lastReadMs < READ_INTERVAL_MS) 
    return;

  lastReadMs += READ_INTERVAL_MS;
}
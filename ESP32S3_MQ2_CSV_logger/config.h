#pragma once

// MQ-2 Sensor
constexpr int           GAS_PIN          = 4;
constexpr unsigned long READ_INTERVAL_MS = 1000;   // one reading per second

constexpr double ADC_FULL_SCALE   = 3102.0;        // raw value that maps to score 100
constexpr int    DECIMALS         = 2;             // decimals for the scaled score

// CSV file
constexpr char CSV_FILE_NAME[] = "/mq2_dataset.csv";
constexpr const char* CSV_COLUMNS[] = {
  "timestamp",
  "reading1",  "reading2",  "reading3",  "reading4",  "reading5",
  "reading6",  "reading7",  "reading8",  "reading9",  "reading10",
  "reading11", "reading12", "reading13", "reading14", "reading15",
  "class"
};

// ---------- Timestamp ----------
constexpr char WIFI_SSID[]     = "";
constexpr char WIFI_PASSWORD[] = "";
constexpr char NTP_SERVER[]    = "pool.ntp.org";
constexpr long GMT_OFFSET_SEC  = 19800; // GMT+5:30
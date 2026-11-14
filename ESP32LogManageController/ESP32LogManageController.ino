#include <WiFi.h>
#include <WebServer.h>
#include "FS.h"
#include "LittleFS.h"

#include "secretconfig.h"

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
static uint32_t lastPrintTime = 0;
static uint32_t lastRetryTime = 0;

WebServer server(80);
File uploadFile;

void handleWiFi() 
{
  uint32_t currentTime = millis();

  if (WiFi.status() == WL_CONNECTED)
  {
    if (currentTime - lastPrintTime > 10000)
    {
      lastPrintTime = currentTime;
      Serial.print("ESP32 website in: http://");
      Serial.println(WiFi.localIP());
    }

    return;
  }

  if (currentTime - lastPrintTime > 3000) 
  {
    lastPrintTime = currentTime;
    int currentStatus = WiFi.status();

    Serial.println("Wi-Fi is not available. Status Code: " + String(currentStatus));

    if(currentStatus == 6 && (currentTime - lastRetryTime > 7000))
    {
      lastRetryTime = currentTime;
      Serial.println("[Wi-Fi] Re-issuing fresh connect request");
      WiFi.begin(ssid, password);
    }
  }
}

void appendFileRows(const char* dirname, String& html, bool& hasFiles) 
{
  File root = LittleFS.open(dirname, "r");

  if (!root || !root.isDirectory()) 
    return;

  File file = root.openNextFile();

  while (file) 
  {
    String path = String(file.path());

    if (!path.startsWith("/")) 
      path = "/" + path;

    if (file.isDirectory()) 
    {
      file.close();
      appendFileRows(path.c_str(), html, hasFiles);
    } 
    else 
    {
      hasFiles = true;
      html += "<tr>";
      html += "<td class='fw-medium'>" + path + "</td>";
      html += "<td>" + String(file.size()) + " bytes</td>";
      html += "<td class='text-end'>";
      html += "<a href='/download?file=" + path + "' class='btn btn-sm btn-success me-1'>Download</a>";
      html += "<a href='/delete?file=" + path + "' class='btn btn-sm btn-outline-danger' onclick=\"return confirm('Delete " + path + "?');\">Delete</a>";
      html += "</td></tr>";
      file.close();
    }

    file = root.openNextFile();
  }
  
  root.close();
}

void setup() 
{
  Serial.begin(115200);
  if (!LittleFS.begin(true)) 
  {
    Serial.println("LittleFS mount failed");
    return;
  }

  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  if(WiFi.status() != WL_CONNECTED) 
  {
    Serial.println("Wi-Fi is not available");
  }
  else
  {
    Serial.println("Wi-Fi available");
  }

  server.on("/assets/bootstrap.min.css", HTTP_GET, []() 
  {
    serveStaticAsset("/assets/bootstrap.min.css", "text/css");
  });

  server.on("/assets/bootstrap.bundle.min.js", HTTP_GET, []() 
  {
    serveStaticAsset("/assets/bootstrap.bundle.min.js", "application/javascript");
  });

  server.on("/", HTTP_GET, []() 
  {
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    size_t remainingBytes = totalBytes - usedBytes;
    float usedPercent = (totalBytes > 0) ? ((float)usedBytes / totalBytes) * 100.0 : 0.0;

    String barColor = "bg-primary";
    if (usedPercent > 85.0) barColor = "bg-danger";
    else if (usedPercent > 65.0) barColor = "bg-warning";

    String html = "<!DOCTYPE html><html lang='en'><head>";
    html += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>ESP32 Log Manager</title>";
    html += "<link href='/assets/bootstrap.min.css' rel='stylesheet'>";
    html += "</head><body class='bg-light'>";
    html += "<div class='container py-4' style='max-width: 800px;'>";
    html += "<h2 class='mb-4 text-primary'>ESP32 Log Manager</h2>";

    html += "<div class='card shadow-sm mb-4'><div class='card-body'>";
    html += "<h5 class='card-title'>Storage</h5>";
    html += "<div class='progress mb-2' style='height: 22px;'>";
    html += "<div class='progress-bar " + barColor + "' style='width: " + String(usedPercent, 1) + "%;'>";
    html += String(usedPercent, 1) + "%</div></div>";
    html += "<small class='text-muted'>";
    html += "<strong>Used:</strong> " + String(usedBytes/1000) + " kB &nbsp;|&nbsp; ";
    html += "<strong>Remaining:</strong> " + String(remainingBytes/1000) + " kB &nbsp;|&nbsp; ";
    html += "<strong>Total:</strong> " + String(totalBytes/1000) + " kB";
    html += "</small></div></div>";

    html += "<div class='card shadow-sm mb-4'><div class='card-body'>";
    html += "<h5 class='card-title mb-3'>Files on Flash</h5>";
    html += "<div class='table-responsive'><table class='table table-hover align-middle mb-0'>";
    html += "<thead class='table-light'><tr><th>File Name</th><th>Size</th><th class='text-end'>Actions</th></tr></thead><tbody>";

    File root = LittleFS.open("/", "r");
    File file = root.openNextFile();
    bool hasFiles = false;


    appendFileRows("/", html, hasFiles);

    if (!hasFiles) 
    {
      html += "<tr><td colspan='3' class='text-center text-muted py-3'>No files found in flash.</td></tr>";
    }

    html += "</tbody></table></div></div></div>";

    html += "<div class='card shadow-sm'><div class='card-body'>";
    html += "<h5 class='card-title mb-3'>Upload File(s) to Flash</h5>";
    html += "<form method='POST' action='/upload' enctype='multipart/form-data' class='row g-2 align-items-center'>";
    html += "<div class='col-sm-9'><input class='form-control' type='file' name='upload' multiple required></div>";
    html += "<div class='col-sm-3 d-grid'><button class='btn btn-primary' type='submit'>Upload</button></div>";
    html += "</form></div></div>";

    html += "</div>";

    html += "<script src='/assets/bootstrap.bundle.min.js'></script>";
    html += "</body></html>";

    server.send(200, "text/html", html);
  });

  server.on("/download", HTTP_GET, []() 
  {
    if (!server.hasArg("file")) {
      server.send(400, "text/plain", "Missing 'file' parameter");
      return;
    }

    String path = server.arg("file");
    if (!path.startsWith("/")) 
    {
      path = "/" + path;
    }

    File file = LittleFS.open(path, FILE_READ);
    if (!file) 
    {
      server.send(404, "text/plain", "File not found");
      return;
    }

    String downloadName = path.substring(1);
    server.sendHeader("Content-Disposition", "attachment; filename=" + downloadName);
    server.streamFile(file, "application/octet-stream");
    file.close();
  });

  server.on("/delete", HTTP_GET, []() 
  {
    if (!server.hasArg("file")) 
    {
      server.send(400, "text/plain", "Missing 'file' parameter");
      return;
    }

    String path = server.arg("file");
    if (!path.startsWith("/")) 
    {
      path = "/" + path;
    }

    if (LittleFS.exists(path)) 
    {
      LittleFS.remove(path);
    }

    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/upload", HTTP_POST, 
  []() 
  {
    server.sendHeader("Location", "/");
    server.send(303);
  },
  []() 
  {
    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) 
    {
      String filename = upload.filename;
      
      int lastSlash = filename.lastIndexOf('/');
      if (lastSlash != -1) filename = filename.substring(lastSlash + 1);
      int lastBackslash = filename.lastIndexOf('\\');
      if (lastBackslash != -1) filename = filename.substring(lastBackslash + 1);

      if (!filename.startsWith("/")) 
      {
        filename = "/" + filename;
      }

      String lowerName = filename;
      lowerName.toLowerCase();
      if (lowerName.indexOf("bootstrap") != -1) 
      {
        if (!LittleFS.exists("/assets")) 
        {
          LittleFS.mkdir("/assets");
        }
        filename = "/assets" + filename;
      }

      Serial.println("Saving upload to: " + filename);
      uploadFile = LittleFS.open(filename, FILE_WRITE, true); 
      if (!uploadFile) 
      {
        Serial.println("ERROR: Failed to open file for writing!");
      }
    } 
    else if (upload.status == UPLOAD_FILE_WRITE) 
    {
      if (uploadFile)
        uploadFile.write(upload.buf, upload.currentSize);
    } 
    else if (upload.status == UPLOAD_FILE_END) 
    {
      if (uploadFile) 
      {
        uploadFile.close();
        Serial.println("Upload finished! Total bytes: " + String(upload.totalSize));
      }
    }
  });

  server.begin();
}

void loop() 
{
  handleWiFi();
  server.handleClient();
}

void serveStaticAsset(const char* path, const char* contentType) 
{
  String gzPath = String(path) + ".gz";
  File file = LittleFS.open(gzPath, FILE_READ);

  if (!file) 
  {
    server.send(404, "text/plain", "Asset not found: " + gzPath);
    return;
  }

  server.sendHeader("Cache-Control", "max-age=86400");
  server.streamFile(file, contentType);
  file.close();
}
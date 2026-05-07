// Blynk set up
#define BLYNK_TEMPLATE_ID "your_template_id"
#define BLYNK_TEMPLATE_NAME "your_template_name"
#define BLYNK_AUTH_TOKEN "your_blynk_auth_token"
#define BLYNK_PRINT Serial

#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include "BlynkSimpleEsp32.h"

// ====== Replace with your Wi-Fi credentials ======
char auth[] = BLYNK_AUTH_TOKEN;
const char* ssid = "your_wifi_ssid";
const char* pass = "your_wifi_password";

// ====== Blynk Datastream Settings ======
// V2: Video widget on the mobile app and web dashboard (/stream)
// V3: Web Page Image Button (/jpg)
#define VPIN_VIDEO       V2
#define VPIN_SNAPSHOT    V3

// ====== Select camera module: AI Thinker ESP32-CAM ======
#define CAMERA_MODEL_AI_THINKER

// AI Thinker ESP32-CAM pin settings
#if defined(CAMERA_MODEL_AI_THINKER)
  #define PWDN_GPIO_NUM     32
  #define RESET_GPIO_NUM    -1
  #define XCLK_GPIO_NUM      0
  #define SIOD_GPIO_NUM     26
  #define SIOC_GPIO_NUM     27

  #define Y9_GPIO_NUM       35
  #define Y8_GPIO_NUM       34
  #define Y7_GPIO_NUM       39
  #define Y6_GPIO_NUM       36
  #define Y5_GPIO_NUM       21
  #define Y4_GPIO_NUM       19
  #define Y3_GPIO_NUM       18
  #define Y2_GPIO_NUM        5
  #define VSYNC_GPIO_NUM    25
  #define HREF_GPIO_NUM     23
  #define PCLK_GPIO_NUM     22
#else
  #error "目前程式只支援 CAMERA_MODEL_AI_THINKER"
#endif

WiFiServer server(80);

// ====== MJPEG Stream State (non-blocking design) ======
WiFiClient streamClient;          // Current stream client
bool streamClientActive = false;  // Whether streaming is active
unsigned long streamStartTime = 0;
unsigned long lastStreamFrameTime = 0;

const unsigned long STREAM_FRAME_INTERVAL = 40;        // One frame every 40 ms, about 25 fps
const unsigned long STREAM_MAX_DURATION   = 60UL * 1000; // Maximum duration for one stream: 60 seconds
const char* MJPEG_BOUNDARY = "frame";

// Forward declarations to avoid "not declared in this scope" errors
void startCamera();
void sendIndexHtml(WiFiClient &client);
void sendSingleJpeg(WiFiClient &client);
void sendMjpegStream(WiFiClient &client);
void handleClient(WiFiClient &client);

/*************************************************
 *  Start Camera
 *************************************************/
void startCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    // Use better image quality when PSRAM is available
    config.frame_size   = FRAMESIZE_VGA;  // 640x480
    config.jpeg_quality = 12;            // 0(best)-63(worst)
    config.fb_count     = 2;
  } else {
    // Lower the settings when PSRAM is not available
    config.frame_size   = FRAMESIZE_QVGA; // 320x240
    config.jpeg_quality = 15;
    config.fb_count     = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    // Restart repeatedly if camera initialization fails
    delay(5000);
    ESP.restart();
  } else {
    Serial.println("Camera init success.");
  }
}

/*************************************************
 *  BLYNK_CONNECTED: configure the stream URLs
 *  - V2 -> http://IP/stream
 *  - V3 -> http://IP/jpg
 *************************************************/
BLYNK_CONNECTED() {
  IPAddress ip = WiFi.localIP();
  String base = String("http://") + ip.toString();

  String urlStream = base + "/stream";
  String urlJpg    = base + "/jpg";

  Blynk.setProperty(VPIN_VIDEO,    "url", urlStream);
  Blynk.setProperty(VPIN_SNAPSHOT, "url", urlJpg);

  Serial.print("Set Blynk Video URL = ");
  Serial.println(urlStream);
  Serial.print("Set Blynk Snapshot URL = ");
  Serial.println(urlJpg);
}

/*************************************************
 *  /index or / : Simple test page
 *************************************************/
void sendIndexHtml(WiFiClient &client) {
  IPAddress ip = WiFi.localIP();
  String ipStr = ip.toString();

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head><meta charset='utf-8'>");
  client.println("<title>ESP32-CAM Smart Plant Viewer</title></head><body>");
  client.print("<h2>ESP32-CAM @ ");
  client.print(ipStr);
  client.println("</h2>");
  client.println("<h3>/stream (MJPEG)</h3>");
  client.println("<img src='/stream' style='max-width:100%;'><br>");
  client.println("<h3>/jpg (Snapshot)</h3>");
  client.println("<img src='/jpg' style='max-width:100%;'><br>");
  client.println("<p>這個頁面只是測試用，真正使用在 Blynk App / Web Dashboard。</p>");
  client.println("</body></html>");
}

/*************************************************
 *  /jpg: Single JPEG snapshot (multiple clients can request snapshots in turn)
 *************************************************/
void sendSingleJpeg(WiFiClient &client) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed (jpg)");
    client.println("HTTP/1.1 500 Internal Server Error");
    client.println("Content-Type: text/plain");
    client.println("Connection: close");
    client.println();
    client.println("Camera capture failed");
    client.stop();
    return;
  }

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: image/jpeg");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Cache-Control: no-store, no-cache, must-revalidate, max-age=0");
  client.println("Pragma: no-cache");
  client.println("Expires: 0");
  client.print("Content-Length: ");
  client.println(fb->len);
  client.println("Connection: close");
  client.println();
  client.write(fb->buf, fb->len);
  client.println();

  esp_camera_fb_return(fb);
  client.stop();
}

// Called in loop(): send one frame each time when a stream client exists
void handleStreamFrame() {
  if (!streamClientActive) {
    return;
  }

  // Close the client if it has disconnected
  if (!streamClient.connected()) {
    Serial.println("Stream client disconnected");
    streamClient.stop();
    streamClientActive = false;
    return;
  }

  unsigned long now = millis();

  // Close the stream after the maximum duration to avoid getting stuck forever
  if (now - streamStartTime > STREAM_MAX_DURATION) {
    Serial.println("Stream timeout, closing client");
    streamClient.stop();
    streamClientActive = false;
    return;
  }

  // Control FPS: do not send a new frame before FRAME_INTERVAL has passed
  if (now - lastStreamFrameTime < STREAM_FRAME_INTERVAL) {
    return;
  }
  lastStreamFrameTime = now;

  // Capture one frame
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed (stream)");
    streamClient.stop();
    streamClientActive = false;
    return;
  }

  // Write one MJPEG frame
  streamClient.print("--");
  streamClient.println(MJPEG_BOUNDARY);
  streamClient.println("Content-Type: image/jpeg");
  streamClient.print("Content-Length: ");
  streamClient.println(fb->len);
  streamClient.println();
  size_t written = streamClient.write(fb->buf, fb->len);
  streamClient.println();

  // Return the frame buffer
  esp_camera_fb_return(fb);

  if (!written) {
    Serial.println("Stream write failed, closing client");
    streamClient.stop();
    streamClientActive = false;
  }
}

/*************************************************
 *  /stream: MJPEG stream (one client at a time)
 *  Adds duration/frame limits to avoid getting stuck forever
 *************************************************/
// /stream: Start MJPEG streaming (non-blocking; sends only headers, then frames are sent in loop)
void sendMjpegStream(WiFiClient &client) {
  // Reject a new client if a stream is already active
  if (streamClientActive) {
    Serial.println("Stream already active, reject new /stream client");
    client.println("HTTP/1.1 503 Service Unavailable");
    client.println("Content-Type: text/plain");
    client.println("Connection: close");
    client.println();
    client.println("Stream already in use");
    client.stop();
    return;
  }

  // Store this client so loop() can keep sending frames to it
  streamClient = client;      // Copy WiFiClient; internally it points to the same socket
  streamClientActive = true;
  streamStartTime = millis();
  lastStreamFrameTime = 0;

  streamClient.println("HTTP/1.1 200 OK");
  streamClient.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  streamClient.println("Access-Control-Allow-Origin: *");
  streamClient.println("Connection: close");
  streamClient.println();

  Serial.println("MJPEG stream started (non-blocking)");
}

/*************************************************
 *  Handle Each HTTP Client Request
 *************************************************/
void handleClient(WiFiClient &client) {
  // Simple timeout to prevent a bad client from blocking forever
  unsigned long timeout = millis() + 2000;
  while (!client.available()) {
    if (millis() > timeout) {
      client.stop();
      return;
    }
    delay(1);
  }

  // Read the first HTTP request line, for example: GET / HTTP/1.1
  String req = client.readStringUntil('\r');
  client.readStringUntil('\n'); // Discard the remaining \n

  // Discard the remaining headers
  while (client.available()) client.read();

  Serial.print("Request: ");
  Serial.println(req);

  if (req.startsWith("GET /stream")) {
    sendMjpegStream(client);
  } else if (req.startsWith("GET /jpg")) {
    sendSingleJpeg(client);
  } else if (req.startsWith("GET / ") || req.startsWith("GET /index")) {
    sendIndexHtml(client);
    // Close the connection
    client.stop();
  } else {
    // Return 404 for other paths
    client.println("HTTP/1.1 404 Not Found");
    client.println("Content-Type: text/plain");
    client.println("Connection: close");
    client.println();
    client.println("404 Not Found");
    client.stop();
  }
}

/*************************************************
 *  setup()
 *************************************************/
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  Serial.println("ESP32-CAM Smart Plant Viewer + Blynk");

  // Start the camera
  startCamera();

  // Connect to Blynk; this also connects Wi-Fi
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  // Disable Wi-Fi power-saving sleep for better stability
  WiFi.setSleep(false);

  // Start the HTTP server
  server.begin();

  Serial.print("Local IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Test URL: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/stream");
}

/*************************************************
 *  loop()
 *************************************************/
void loop() {
  Blynk.run();

  // Handle a new HTTP client
  WiFiClient client = server.available();
  if (client) {
    Serial.println("New client");
    handleClient(client);
    Serial.println("Client handled");
  }

  // Send one frame per loop to the current /stream client
  handleStreamFrame();
}

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"

// ============================================================
// WI-FI
// ============================================================

const char* WIFI_SSID = "xxxxxxxxxxx";
const char* WIFI_PASS = "xxxxxxxxxxx";

// ============================================================
// PINOS DA ESP32-CAM AI THINKER
// ============================================================

#define PWDN_GPIO_NUM      32
#define RESET_GPIO_NUM     -1
#define XCLK_GPIO_NUM       0
#define SIOD_GPIO_NUM      26
#define SIOC_GPIO_NUM      27

#define Y9_GPIO_NUM        35
#define Y8_GPIO_NUM        34
#define Y7_GPIO_NUM        39
#define Y6_GPIO_NUM        36
#define Y5_GPIO_NUM        21
#define Y4_GPIO_NUM        19
#define Y3_GPIO_NUM        18
#define Y2_GPIO_NUM         5

#define VSYNC_GPIO_NUM     25
#define HREF_GPIO_NUM      23
#define PCLK_GPIO_NUM      22

#define FLASH_GPIO          4

// ============================================================
// SERVIDOR HTTP
// ============================================================

static httpd_handle_t httpd = nullptr;
static volatile bool streamingBusy = false;

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32-CAM</title>

  <style>
    body {
      font-family: system-ui, -apple-system, Segoe UI, Roboto, Ubuntu, sans-serif;
      margin: 0;
      background: #0b0e11;
      color: #e6e6e6;
    }

    header {
      padding: 12px 16px;
      background: #11161c;
      border-bottom: 1px solid #222;
    }

    .wrap {
      max-width: 900px;
      margin: 16px auto;
      padding: 0 12px;
    }

    .card {
      background: #141a21;
      border: 1px solid #222;
      border-radius: 12px;
      padding: 16px;
    }

    .row {
      display: flex;
      gap: 12px;
      align-items: center;
      flex-wrap: wrap;
    }

    button,
    a.btn {
      padding: 10px 16px;
      border-radius: 10px;
      border: 1px solid #2a3340;
      background: #1a2330;
      color: #e6e6e6;
      text-decoration: none;
      cursor: pointer;
    }

    img {
      max-width: 100%;
      border-radius: 10px;
    }

    small {
      opacity: 0.8;
    }
  </style>
</head>

<body>
  <header>
    <div class="wrap">
      <b>ESP32-CAM - Live</b>
    </div>
  </header>

  <div class="wrap">
    <div class="card">
      <div class="row">
        <a class="btn" href="/jpg" target="_blank">Snapshot</a>
        <button onclick="fetch('/flash?onoff=toggle')">Flash</button>
        <button onclick="fetch('/restart')">Restart</button>
        <small id="ip"></small>
      </div>
    </div>

    <div class="card" style="margin-top:16px">
      <img id="stream" src="/stream">
    </div>
  </div>

  <script>
    document.getElementById("ip").textContent = "IP: " + location.host;
  </script>
</body>
</html>
)HTML";

static const char* STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace; boundary=frame";

static const char* STREAM_BOUNDARY =
    "\r\n--frame\r\n";

static const char* STREAM_PART =
    "Content-Type: image/jpeg\r\n"
    "Content-Length: %u\r\n\r\n";

// ============================================================
// HANDLERS HTTP
// ============================================================

static esp_err_t indexHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t jpgHandler(httpd_req_t* req) {
  camera_fb_t* frame = esp_camera_fb_get();

  if (frame == nullptr) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  esp_err_t result = httpd_resp_send(
      req,
      reinterpret_cast<const char*>(frame->buf),
      frame->len
  );

  esp_camera_fb_return(frame);
  return result;
}

static esp_err_t flashHandler(httpd_req_t* req) {
  char query[64] = {};
  char value[16] = {};

  static bool flashState = false;

  size_t queryLength = httpd_req_get_url_query_len(req);

  if (queryLength > 0 && queryLength < sizeof(query)) {
    if (httpd_req_get_url_query_str(
            req,
            query,
            sizeof(query)
        ) == ESP_OK) {
      if (httpd_query_key_value(
              query,
              "onoff",
              value,
              sizeof(value)
          ) == ESP_OK) {
        String command(value);

        if (command == "on") {
          flashState = true;
        } else if (command == "off") {
          flashState = false;
        } else {
          flashState = !flashState;
        }
      }
    }
  } else {
    flashState = !flashState;
  }

  digitalWrite(FLASH_GPIO, flashState ? HIGH : LOW);

  httpd_resp_set_type(req, "text/plain");

  return httpd_resp_send(
      req,
      flashState ? "flash:on" : "flash:off",
      HTTPD_RESP_USE_STRLEN
  );
}

static esp_err_t restartHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "restarting...", HTTPD_RESP_USE_STRLEN);

  delay(200);
  ESP.restart();

  return ESP_OK;
}

static esp_err_t streamHandler(httpd_req_t* req) {
  if (streamingBusy) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "stream busy", HTTPD_RESP_USE_STRLEN);
  }

  streamingBusy = true;

  esp_err_t result = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);

  if (result != ESP_OK) {
    streamingBusy = false;
    return result;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  uint32_t lastMemoryLog = millis();
  uint8_t consecutiveFailures = 0;

  while (true) {
    camera_fb_t* frame = esp_camera_fb_get();

    if (frame == nullptr) {
      consecutiveFailures++;

      if (consecutiveFailures >= 5) {
        sensor_t* sensor = esp_camera_sensor_get();

        if (sensor != nullptr) {
          sensor->set_framesize(sensor, FRAMESIZE_QVGA);
        }

        consecutiveFailures = 0;
      }

      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    consecutiveFailures = 0;

    char partHeader[96];

    int headerLength = snprintf(
        partHeader,
        sizeof(partHeader),
        STREAM_PART,
        static_cast<unsigned int>(frame->len)
    );

    result = httpd_resp_send_chunk(
        req,
        STREAM_BOUNDARY,
        strlen(STREAM_BOUNDARY)
    );

    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(
          req,
          partHeader,
          headerLength
      );
    }

    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(
          req,
          reinterpret_cast<const char*>(frame->buf),
          frame->len
      );
    }

    esp_camera_fb_return(frame);

    if (result != ESP_OK) {
      break;
    }

    if (millis() - lastMemoryLog >= 5000) {
      lastMemoryLog = millis();

      size_t dramFree =
          heap_caps_get_free_size(MALLOC_CAP_8BIT);

      size_t psramFree =
          heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

      Serial.printf(
          "[MEM] DRAM=%u PSRAM=%u\n",
          static_cast<unsigned int>(dramFree),
          static_cast<unsigned int>(psramFree)
      );
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }

  streamingBusy = false;
  return ESP_OK;
}

// ============================================================
// INICIALIZAÇÃO DO SERVIDOR
// ============================================================

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  config.server_port = 80;
  config.max_open_sockets = 3;
  config.stack_size = 8192;
  config.recv_wait_timeout = 10;
  config.send_wait_timeout = 10;
  config.lru_purge_enable = true;

  esp_err_t result = httpd_start(&httpd, &config);

  if (result != ESP_OK) {
    Serial.printf(
        "Falha ao iniciar servidor HTTP: 0x%x\n",
        result
    );
    return;
  }

  httpd_uri_t indexUri = {};
  indexUri.uri = "/";
  indexUri.method = HTTP_GET;
  indexUri.handler = indexHandler;

  httpd_uri_t jpgUri = {};
  jpgUri.uri = "/jpg";
  jpgUri.method = HTTP_GET;
  jpgUri.handler = jpgHandler;

  httpd_uri_t streamUri = {};
  streamUri.uri = "/stream";
  streamUri.method = HTTP_GET;
  streamUri.handler = streamHandler;

  httpd_uri_t flashUri = {};
  flashUri.uri = "/flash";
  flashUri.method = HTTP_GET;
  flashUri.handler = flashHandler;

  httpd_uri_t restartUri = {};
  restartUri.uri = "/restart";
  restartUri.method = HTTP_GET;
  restartUri.handler = restartHandler;

  httpd_register_uri_handler(httpd, &indexUri);
  httpd_register_uri_handler(httpd, &jpgUri);
  httpd_register_uri_handler(httpd, &streamUri);
  httpd_register_uri_handler(httpd, &flashUri);
  httpd_register_uri_handler(httpd, &restartUri);

  Serial.println("Servidor HTTP iniciado na porta 80.");
}

// ============================================================
// INICIALIZAÇÃO DA CÂMERA
// ============================================================

bool initCamera() {
  camera_config_t config = {};

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;

  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;

  bool hasPsram = psramFound();

  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 15;
  config.fb_count = 1;

  config.fb_location =
      hasPsram
          ? CAMERA_FB_IN_PSRAM
          : CAMERA_FB_IN_DRAM;

  Serial.printf(
      "PSRAM: %s\n",
      hasPsram ? "encontrada" : "nao encontrada"
  );

  esp_err_t result = esp_camera_init(&config);

  if (result != ESP_OK) {
    Serial.printf(
        "Falha ao inicializar camera: 0x%x\n",
        result
    );
    return false;
  }

  sensor_t* sensor = esp_camera_sensor_get();

  if (sensor != nullptr) {
    sensor->set_framesize(sensor, FRAMESIZE_VGA);
    sensor->set_brightness(sensor, 0);
    sensor->set_contrast(sensor, 0);
    sensor->set_saturation(sensor, 0);

    sensor->set_hmirror(sensor, 1);
    sensor->set_vflip(sensor, 1);

    if (sensor->set_lenc != nullptr) {
      sensor->set_lenc(sensor, 1);
    }
  }

  return true;
}

// ============================================================
// CONEXÃO WI-FI
// ============================================================

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  esp_wifi_set_ps(WIFI_PS_NONE);

  Serial.printf("Conectando ao Wi-Fi \"%s\"", WIFI_SSID);

  uint32_t startTime = millis();

  while (
      WiFi.status() != WL_CONNECTED &&
      millis() - startTime < 20000
  ) {
    Serial.print(".");
    delay(500);
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi conectado.");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("Pagina: http://");
    Serial.println(WiFi.localIP());

    Serial.print("Snapshot: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/jpg");

    Serial.print("Stream: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/stream");

    return;
  }

  Serial.println("Nao foi possivel conectar ao Wi-Fi.");
  Serial.println("Criando ponto de acesso de emergencia.");

  uint64_t chipId = ESP.getEfuseMac();

  char accessPointName[32];

  snprintf(
      accessPointName,
      sizeof(accessPointName),
      "ESP32CAM-%04X",
      static_cast<uint16_t>(chipId & 0xFFFF)
  );

  WiFi.mode(WIFI_AP);
  WiFi.softAP(accessPointName);

  Serial.print("Rede criada: ");
  Serial.println(accessPointName);

  Serial.print("Acesse: http://");
  Serial.println(WiFi.softAPIP());
}

// ============================================================
// SETUP E LOOP
// ============================================================

void setup() {
  pinMode(FLASH_GPIO, OUTPUT);
  digitalWrite(FLASH_GPIO, LOW);

  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("====================================");
  Serial.println("ESP32-CAM AI THINKER - STREAMING");
  Serial.println("====================================");

  if (!initCamera()) {
    Serial.println(
        "A camera nao iniciou. Verifique a alimentacao, "
        "a placa selecionada e o cabo flat."
    );

    while (true) {
      delay(1000);
    }
  }

  connectWiFi();
  startCameraServer();

  Serial.printf(
      "[MEM] DRAM=%u PSRAM=%u\n",
      static_cast<unsigned int>(
          heap_caps_get_free_size(MALLOC_CAP_8BIT)
      ),
      static_cast<unsigned int>(
          heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
      )
  );

  Serial.println("Inicializacao concluida.");
}

void loop() {
  delay(1000);
}

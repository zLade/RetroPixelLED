#include <WiFi.h>
#include <WiFiServer.h>
#include <SPI.h>
#include <SD.h>
#include <AnimatedGIF.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <vector>
#include <algorithm>

// ====================================================================
//                  Retro Pixel LED - Recalbox dedicated build
// ====================================================================
// Single-purpose firmware:
// - Reads /config.txt from the SD card.
// - Connects to WiFi using that config.
// - Receives Recalbox events on /gif?s=<system>&g=<game>&t=<title>.
// - Shows game GIFs, rotating variants every configured interval.
// - Falls back to scrolling the game title when no matching GIF exists.
// - Shows default.gif before a game is launched.
// ====================================================================

#define FIRMWARE_VERSION "recalbox-1.0.0"
#define CONFIG_FILE "/config.txt"

// HUB75 pinout kept from the existing Retro Pixel LED hardware profile.
#define CLK_PIN       16
#define OE_PIN        15
#define LAT_PIN       4
#define A_PIN         33
#define B_PIN         32
#define C_PIN         22
#define D_PIN         17
#define E_PIN         -1
#define R1_PIN        25
#define G1_PIN        26
#define B1_PIN        27
#define R2_PIN        14
#define G2_PIN        12
#define B2_PIN        13

// SD card SPI pinout.
#define SD_CS_PIN     5
#define VSPI_MISO     19
#define VSPI_MOSI     23
#define VSPI_SCLK     18

const int PANEL_RES_X = 64;
const int PANEL_RES_Y = 32;

struct AppConfig {
  String ssid = "";
  String password = "";
  String hostname = "retropixel-recalbox";
  bool dhcp = true;
  bool hasStaticIp = false;
  IPAddress ip;
  IPAddress gateway = IPAddress(192, 168, 1, 1);
  IPAddress subnet = IPAddress(255, 255, 255, 0);
  IPAddress dns1 = IPAddress(192, 168, 1, 1);
  IPAddress dns2 = IPAddress(8, 8, 8, 8);

  uint8_t brightness = 40;
  int panelChain = 4;
  int displayXOffset = 128;
  int viewportWidth = 128;
  int gifIntervalMs = 10000;
  int textSpeedMs = 35;
  int textPauseMs = 250;
  bool showTitleBetweenGifs = true;

  String gifRoot = "/gif";
  String defaultGif = "/gif/default.gif";

  int minRefreshRate = 120;
  int latchBlanking = 1;
  int i2sSpeed = 2; // 0=8MHz, 1=10MHz, 2=16MHz, 3=20MHz.
};

struct GifVariant {
  int order = 0;
  String path = "";
};

enum DisplayMode {
  MODE_DEFAULT,
  MODE_GAME_GIFS,
  MODE_TEXT_ONLY
};

AppConfig config;
WiFiServer httpServer(80);
AnimatedGIF gif;
MatrixPanel_I2S_DMA *display = nullptr;
File gifFile;

bool sdMounted = false;
bool httpStarted = false;
unsigned long lastWifiAttempt = 0;

int gifXOffset = 0;
int gifYOffset = 0;

DisplayMode displayMode = MODE_DEFAULT;
String currentSystem = "";
String currentGame = "";
String currentTitle = "READY";
std::vector<String> currentGifs;
size_t currentGifIndex = 0;
volatile uint32_t stateVersion = 0;

// --------------------------------------------------------------------
// Utility helpers
// --------------------------------------------------------------------

String trimmed(String value) {
  value.trim();
  return value;
}

String stripMatchingQuotes(String value) {
  value.trim();
  if (value.length() >= 2) {
    char first = value.charAt(0);
    char last = value.charAt(value.length() - 1);
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      return value.substring(1, value.length() - 1);
    }
  }
  return value;
}

String upperKey(String key) {
  key.trim();
  key.toUpperCase();
  return key;
}

String lowerValue(String value) {
  value.trim();
  value.toLowerCase();
  return value;
}

bool parseBoolValue(String value, bool fallback) {
  value = lowerValue(value);
  if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
  if (value == "0" || value == "false" || value == "no" || value == "off") return false;
  return fallback;
}

int parseIntValue(String value, int fallback, int minValue, int maxValue) {
  value.trim();
  if (value.length() == 0) return fallback;
  int parsed = value.toInt();
  if (parsed < minValue) return minValue;
  if (parsed > maxValue) return maxValue;
  return parsed;
}

String normalizePath(String path) {
  path.trim();
  path.replace("\\", "/");
  while (path.indexOf("//") >= 0) path.replace("//", "/");
  if (!path.startsWith("/")) path = "/" + path;
  if (path.length() > 1 && path.endsWith("/")) path.remove(path.length() - 1);
  return path;
}

String joinPath(String dir, String name) {
  dir = normalizePath(dir);
  name.replace("\\", "/");
  while (name.startsWith("/")) name.remove(0, 1);
  if (dir == "/") return "/" + name;
  return dir + "/" + name;
}

String basenameOf(String path) {
  path.replace("\\", "/");
  int slash = path.lastIndexOf('/');
  if (slash >= 0) return path.substring(slash + 1);
  return path;
}

String dirnameOf(String path) {
  path = normalizePath(path);
  int slash = path.lastIndexOf('/');
  if (slash <= 0) return "/";
  return path.substring(0, slash);
}

String stemOf(String path) {
  String name = basenameOf(path);
  int dot = name.lastIndexOf('.');
  if (dot > 0) name = name.substring(0, dot);
  return name;
}

String cleanId(String value) {
  value.trim();
  value.replace("\\", "/");
  value = basenameOf(value);
  int dot = value.lastIndexOf('.');
  if (dot > 0) value = value.substring(0, dot);
  value.replace("/", "_");
  return value;
}

String cleanTitle(String value, const String& fallback) {
  value.trim();
  value.replace("\\", " ");
  value.replace("/", " ");
  if (value.length() == 0 || value == "null") value = fallback;
  if (value.length() > 120) value = value.substring(0, 120);
  return value;
}

bool isGifName(String name) {
  name.toLowerCase();
  return name.endsWith(".gif");
}

bool addUniqueString(std::vector<String>& list, const String& value) {
  if (value.length() == 0) return false;
  for (const String& existing : list) {
    if (existing == value) return false;
  }
  list.push_back(value);
  return true;
}

void addUniqueVariant(std::vector<GifVariant>& variants, int order, const String& path) {
  for (const GifVariant& variant : variants) {
    if (variant.path == path) return;
  }
  GifVariant variant;
  variant.order = order;
  variant.path = path;
  variants.push_back(variant);
}

int parseVariantOrder(String fileStem, String gameStem) {
  fileStem.toLowerCase();
  gameStem.toLowerCase();

  if (fileStem == gameStem) return 1;

  String prefix = gameStem + "_";
  if (!fileStem.startsWith(prefix)) return -1;

  String suffix = fileStem.substring(prefix.length());
  if (suffix.length() == 0) return -1;

  for (int i = 0; i < suffix.length(); ++i) {
    if (!isDigit((unsigned char)suffix.charAt(i))) return -1;
  }

  int order = suffix.toInt();
  return order > 1 ? order : -1;
}

String urlDecode(String value) {
  String decoded = "";
  for (int i = 0; i < value.length(); ++i) {
    char c = value.charAt(i);
    if (c == '+') {
      decoded += ' ';
    } else if (c == '%' && i + 2 < value.length()) {
      char hex[3] = { value.charAt(i + 1), value.charAt(i + 2), 0 };
      decoded += (char)strtol(hex, nullptr, 16);
      i += 2;
    } else {
      decoded += c;
    }
  }
  return decoded;
}

String queryParam(const String& query, const String& key) {
  int start = 0;
  while (start < query.length()) {
    int amp = query.indexOf('&', start);
    String part = amp >= 0 ? query.substring(start, amp) : query.substring(start);
    int eq = part.indexOf('=');
    if (eq >= 0) {
      String partKey = urlDecode(part.substring(0, eq));
      if (partKey == key) return urlDecode(part.substring(eq + 1));
    }
    if (amp < 0) break;
    start = amp + 1;
  }
  return "";
}

// --------------------------------------------------------------------
// SD config
// --------------------------------------------------------------------

void applyConfigValue(String key, String value) {
  key = upperKey(key);
  value = stripMatchingQuotes(value);

  if (key == "SSID" || key == "WIFI_SSID") {
    config.ssid = value;
  } else if (key == "PASSWORD" || key == "PASS" || key == "WIFI_PASSWORD") {
    config.password = value;
  } else if (key == "HOSTNAME" || key == "DEVICE_NAME") {
    config.hostname = value.length() ? value : config.hostname;
  } else if (key == "DHCP") {
    config.dhcp = parseBoolValue(value, config.dhcp);
  } else if (key == "IP" || key == "STATIC_IP") {
    config.hasStaticIp = config.ip.fromString(value);
    if (config.hasStaticIp) config.dhcp = false;
  } else if (key == "GATEWAY") {
    config.gateway.fromString(value);
  } else if (key == "SUBNET" || key == "NETMASK") {
    config.subnet.fromString(value);
  } else if (key == "DNS" || key == "DNS1") {
    config.dns1.fromString(value);
  } else if (key == "DNS2") {
    config.dns2.fromString(value);
  } else if (key == "BRIGHTNESS") {
    config.brightness = (uint8_t)parseIntValue(value, config.brightness, 0, 255);
  } else if (key == "PANEL_CHAIN") {
    config.panelChain = parseIntValue(value, config.panelChain, 1, 8);
  } else if (key == "DISPLAY_X_OFFSET") {
    config.displayXOffset = parseIntValue(value, config.displayXOffset, 0, 512);
  } else if (key == "VIEWPORT_WIDTH") {
    config.viewportWidth = parseIntValue(value, config.viewportWidth, 16, 512);
  } else if (key == "GIF_INTERVAL_SECONDS") {
    config.gifIntervalMs = parseIntValue(value, config.gifIntervalMs / 1000, 1, 3600) * 1000;
  } else if (key == "GIF_INTERVAL_MS") {
    config.gifIntervalMs = parseIntValue(value, config.gifIntervalMs, 500, 3600000);
  } else if (key == "TEXT_SPEED_MS") {
    config.textSpeedMs = parseIntValue(value, config.textSpeedMs, 5, 1000);
  } else if (key == "TEXT_PAUSE_MS") {
    config.textPauseMs = parseIntValue(value, config.textPauseMs, 0, 10000);
  } else if (key == "SHOW_TITLE_BETWEEN_GIFS") {
    config.showTitleBetweenGifs = parseBoolValue(value, config.showTitleBetweenGifs);
  } else if (key == "GIF_ROOT") {
    config.gifRoot = normalizePath(value);
  } else if (key == "DEFAULT_GIF") {
    config.defaultGif = normalizePath(value);
  } else if (key == "MIN_REFRESH_RATE") {
    config.minRefreshRate = parseIntValue(value, config.minRefreshRate, 30, 240);
  } else if (key == "LATCH_BLANKING") {
    config.latchBlanking = parseIntValue(value, config.latchBlanking, 1, 4);
  } else if (key == "I2S_SPEED") {
    config.i2sSpeed = parseIntValue(value, config.i2sSpeed, 0, 3);
  }
}

void loadSdConfig() {
  if (!sdMounted || !SD.exists(CONFIG_FILE)) {
    Serial.println("No /config.txt found; using firmware defaults.");
    return;
  }

  File file = SD.open(CONFIG_FILE, FILE_READ);
  if (!file) {
    Serial.println("Could not open /config.txt; using firmware defaults.");
    return;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;

    int eq = line.indexOf('=');
    if (eq <= 0) continue;

    String key = line.substring(0, eq);
    String value = line.substring(eq + 1);
    applyConfigValue(key, value);
  }

  file.close();
}

// --------------------------------------------------------------------
// Display
// --------------------------------------------------------------------

void clearViewport() {
  if (!display) return;
  display->fillRect(config.displayXOffset, 0, config.viewportWidth, PANEL_RES_Y, 0);
}

void drawViewportMessage(const String& message, uint16_t color) {
  if (!display) return;
  clearViewport();
  display->setFont(nullptr);
  display->setTextWrap(false);
  display->setTextSize(1);
  display->setTextColor(color);

  int16_t x1, y1;
  uint16_t w, h;
  display->getTextBounds(message, 0, 0, &x1, &y1, &w, &h);
  int x = config.displayXOffset + max(0, (config.viewportWidth - (int)w) / 2);
  int y = max(0, (PANEL_RES_Y - (int)h) / 2);
  display->setCursor(x, y);
  display->print(message);
  display->flipDMABuffer();
}

bool initDisplay() {
  const int matrixWidth = PANEL_RES_X * config.panelChain;

  if (config.displayXOffset >= matrixWidth) config.displayXOffset = 0;
  if (config.displayXOffset + config.viewportWidth > matrixWidth) {
    config.viewportWidth = matrixWidth - config.displayXOffset;
  }
  if (config.viewportWidth <= 0) config.viewportWidth = matrixWidth;

  HUB75_I2S_CFG::i2s_pins pins = {
    R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN,
    A_PIN, B_PIN, C_PIN, D_PIN, E_PIN,
    LAT_PIN, OE_PIN, CLK_PIN
  };

  HUB75_I2S_CFG matrixConfig(matrixWidth, PANEL_RES_Y, config.panelChain, pins);

  if (config.i2sSpeed == 0)      matrixConfig.i2sspeed = HUB75_I2S_CFG::HZ_8M;
  else if (config.i2sSpeed == 1) matrixConfig.i2sspeed = HUB75_I2S_CFG::HZ_10M;
  else if (config.i2sSpeed == 2) matrixConfig.i2sspeed = HUB75_I2S_CFG::HZ_16M;
  else if (config.i2sSpeed == 3) matrixConfig.i2sspeed = HUB75_I2S_CFG::HZ_20M;

  matrixConfig.latch_blanking = config.latchBlanking;
  matrixConfig.min_refresh_rate = config.minRefreshRate;
  matrixConfig.clkphase = false;

  display = new MatrixPanel_I2S_DMA(matrixConfig);
  if (!display) return false;

  display->begin();
  display->setBrightness8(config.brightness);
  display->fillScreen(0);
  return true;
}

// --------------------------------------------------------------------
// GIF lookup
// --------------------------------------------------------------------

void scanDirectoryForVariants(const String& dir, const String& gameId, std::vector<GifVariant>& variants) {
  if (!sdMounted || gameId.length() == 0) return;

  File root = SD.open(normalizePath(dir));
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  String gameStem = gameId;
  gameStem.toLowerCase();

  File entry = root.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      String name = basenameOf(String(entry.name()));
      if (isGifName(name)) {
        String stem = stemOf(name);
        int order = parseVariantOrder(stem, gameStem);
        if (order > 0) {
          addUniqueVariant(variants, order, joinPath(dir, name));
        }
      }
    }
    entry.close();
    entry = root.openNextFile();
  }

  root.close();
}

std::vector<String> variantsToPaths(std::vector<GifVariant>& variants) {
  std::sort(variants.begin(), variants.end(), [](const GifVariant& a, const GifVariant& b) {
    if (a.order == b.order) return a.path < b.path;
    return a.order < b.order;
  });

  std::vector<String> paths;
  for (const GifVariant& variant : variants) {
    paths.push_back(variant.path);
  }
  return paths;
}

std::vector<String> findGameGifs(const String& systemId, const String& gameId) {
  std::vector<String> dirs;
  String root = normalizePath(config.gifRoot);
  String system = cleanId(systemId);

  if (system.length() > 0) addUniqueString(dirs, joinPath(root, system));
  addUniqueString(dirs, root);
  if (root != "/gif") {
    if (system.length() > 0) addUniqueString(dirs, joinPath("/gif", system));
    addUniqueString(dirs, "/gif");
  }
  addUniqueString(dirs, "/");

  std::vector<GifVariant> variants;
  for (const String& dir : dirs) {
    scanDirectoryForVariants(dir, gameId, variants);
  }

  return variantsToPaths(variants);
}

std::vector<String> findDefaultGifs() {
  std::vector<GifVariant> variants;
  String configured = normalizePath(config.defaultGif);

  if (sdMounted && SD.exists(configured)) {
    addUniqueVariant(variants, 1, configured);
  }

  scanDirectoryForVariants(dirnameOf(configured), stemOf(configured), variants);
  scanDirectoryForVariants("/", "default", variants);
  scanDirectoryForVariants(config.gifRoot, "default", variants);
  scanDirectoryForVariants(joinPath(config.gifRoot, "default"), "default", variants);

  return variantsToPaths(variants);
}

// --------------------------------------------------------------------
// State changes
// --------------------------------------------------------------------

void bumpState() {
  stateVersion++;
  clearViewport();
}

void loadDefaultState() {
  gif.close();
  currentSystem = "";
  currentGame = "default";
  currentTitle = "READY";
  currentGifs = findDefaultGifs();
  currentGifIndex = 0;
  displayMode = currentGifs.empty() ? MODE_TEXT_ONLY : MODE_DEFAULT;
  Serial.printf("Default state: %u GIF(s)\n", (unsigned)currentGifs.size());
  bumpState();
}

void loadGameState(String systemId, String gameId, String title) {
  gif.close();
  systemId = cleanId(systemId);
  gameId = cleanId(gameId);
  title = cleanTitle(title, gameId);

  currentSystem = systemId;
  currentGame = gameId;
  currentTitle = title;
  currentGifs = findGameGifs(systemId, gameId);
  currentGifIndex = 0;
  displayMode = currentGifs.empty() ? MODE_TEXT_ONLY : MODE_GAME_GIFS;

  Serial.printf("Game state: system=%s game=%s title=%s gifs=%u\n",
                currentSystem.c_str(), currentGame.c_str(), currentTitle.c_str(),
                (unsigned)currentGifs.size());
  bumpState();
}

// --------------------------------------------------------------------
// WiFi and HTTP input
// --------------------------------------------------------------------

void beginHttpServerIfNeeded() {
  if (httpStarted || WiFi.status() != WL_CONNECTED) return;
  httpServer.begin();
  httpStarted = true;
  Serial.print("HTTP input ready at http://");
  Serial.println(WiFi.localIP());
}

void startWifiAttempt() {
  if (config.ssid.length() == 0) return;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(config.hostname.c_str());
  WiFi.setSleep(false);

  if (!config.dhcp && config.hasStaticIp) {
    WiFi.config(config.ip, config.gateway, config.subnet, config.dns1, config.dns2);
  }

  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(config.ssid);
  WiFi.begin(config.ssid.c_str(), config.password.c_str());
  lastWifiAttempt = millis();
}

void connectWifiInitial() {
  if (config.ssid.length() == 0) {
    Serial.println("WiFi SSID missing in /config.txt");
    drawViewportMessage("NO WIFI CFG", display->color565(255, 80, 0));
    return;
  }

  drawViewportMessage("WIFI...", display->color565(0, 180, 255));
  startWifiAttempt();

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(100);
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected: ");
    Serial.println(WiFi.localIP());
    beginHttpServerIfNeeded();
  } else {
    Serial.println("WiFi connection failed; firmware will retry in background.");
    drawViewportMessage("WIFI RETRY", display->color565(255, 180, 0));
  }
}

void maintainWifi() {
  if (config.ssid.length() == 0) return;

  if (WiFi.status() == WL_CONNECTED) {
    beginHttpServerIfNeeded();
    return;
  }

  httpStarted = false;
  if (millis() - lastWifiAttempt > 15000) {
    startWifiAttempt();
  }
}

void sendHttpResponse(WiFiClient& client, int status, const String& body) {
  String statusText = status == 200 ? "OK" : "Not Found";
  client.print("HTTP/1.1 " + String(status) + " " + statusText + "\r\n");
  client.print("Content-Type: text/plain\r\n");
  client.print("Connection: close\r\n");
  client.print("Cache-Control: no-store\r\n");
  client.print("Content-Length: " + String(body.length()) + "\r\n\r\n");
  client.print(body);
}

void drainHttpHeaders(WiFiClient& client) {
  unsigned long deadline = millis() + 50;
  String line = "";
  while (client.connected() && millis() < deadline) {
    while (client.available()) {
      char c = client.read();
      if (c == '\n') {
        if (line == "\r" || line.length() == 0) return;
        line = "";
      } else {
        line += c;
      }
      if (line.length() > 256) line = "";
    }
    yield();
  }
}

void handleRecalboxQuery(const String& query) {
  String systemId = queryParam(query, "s");
  String gameId = queryParam(query, "g");
  String title = queryParam(query, "t");

  String systemLower = lowerValue(systemId);
  String gameLower = lowerValue(gameId);

  if (gameLower.length() == 0 ||
      gameLower == "stop" || gameLower == "off" ||
      systemLower == "stop" || systemLower == "off") {
    loadDefaultState();
    return;
  }

  loadGameState(systemId, gameId, title);
}

void handleHttpClient() {
  if (!httpStarted) return;

  WiFiClient client = httpServer.available();
  if (!client) return;

  client.setTimeout(25);
  String requestLine = client.readStringUntil('\n');
  requestLine.trim();
  drainHttpHeaders(client);

  int firstSpace = requestLine.indexOf(' ');
  int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
  if (firstSpace < 0 || secondSpace < 0) {
    sendHttpResponse(client, 404, "BAD REQUEST\n");
    client.stop();
    return;
  }

  String target = requestLine.substring(firstSpace + 1, secondSpace);
  String path = target;
  String query = "";
  int q = target.indexOf('?');
  if (q >= 0) {
    path = target.substring(0, q);
    query = target.substring(q + 1);
  }

  if (path == "/gif") {
    handleRecalboxQuery(query);
    sendHttpResponse(client, 200, "OK\n");
  } else if (path == "/status") {
    String body = "mode=" + String(displayMode == MODE_TEXT_ONLY ? "text" : "gif") +
                  "\nsystem=" + currentSystem +
                  "\ngame=" + currentGame +
                  "\ntitle=" + currentTitle +
                  "\ngifs=" + String((unsigned)currentGifs.size()) +
                  "\nip=" + WiFi.localIP().toString() + "\n";
    sendHttpResponse(client, 200, body);
  } else {
    sendHttpResponse(client, 404, "NOT FOUND\n");
  }

  delay(1);
  client.stop();
}

void serviceNetwork() {
  maintainWifi();
  handleHttpClient();
}

bool waitWithService(unsigned long waitMs, uint32_t version) {
  unsigned long start = millis();
  while (millis() - start < waitMs) {
    serviceNetwork();
    if (version != stateVersion) return false;
    delay(1);
    yield();
  }
  return true;
}

// --------------------------------------------------------------------
// GIF callbacks and rendering
// --------------------------------------------------------------------

void drawGifPixel(int x, int y, uint16_t color) {
  if (!display) return;
  if (x < config.displayXOffset || x >= config.displayXOffset + config.viewportWidth) return;
  if (y < 0 || y >= PANEL_RES_Y) return;
  display->drawPixel(x, y, color);
}

void GIFDraw(GIFDRAW *pDraw) {
  if (!display) return;

  uint8_t *s = pDraw->pPixels;
  uint16_t *palette = pDraw->pPalette;
  uint16_t lineBuffer[320];

  int width = pDraw->iWidth;
  if (width > (int)(sizeof(lineBuffer) / sizeof(lineBuffer[0]))) {
    width = sizeof(lineBuffer) / sizeof(lineBuffer[0]);
  }

  int y = pDraw->iY + pDraw->y + gifYOffset;
  int baseX = pDraw->iX + gifXOffset;

  if (pDraw->ucHasTransparency) {
    int count = 0;
    for (int x = 0; x < width; ++x) {
      if (s[x] == pDraw->ucTransparent) {
        if (count > 0) {
          for (int i = 0; i < count; ++i) {
            drawGifPixel(baseX + x - count + i, y, lineBuffer[i]);
          }
          count = 0;
        }
      } else {
        lineBuffer[count++] = palette[s[x]];
      }
    }

    if (count > 0) {
      for (int i = 0; i < count; ++i) {
        drawGifPixel(baseX + width - count + i, y, lineBuffer[i]);
      }
    }
  } else {
    for (int x = 0; x < width; ++x) {
      drawGifPixel(baseX + x, y, palette[*s++]);
    }
  }
}

static void *GIFOpenFile(const char *filename, int32_t *size) {
  gifFile = SD.open(filename, FILE_READ);
  if (!gifFile) return nullptr;
  *size = gifFile.size();
  return (void *)&gifFile;
}

static void GIFCloseFile(void *handle) {
  File *file = static_cast<File *>(handle);
  if (file) file->close();
}

static int32_t GIFReadFile(GIFFILE *file, uint8_t *buffer, int32_t length) {
  File *handle = static_cast<File *>(file->fHandle);
  int32_t remaining = file->iSize - file->iPos;
  if (length > remaining) length = remaining;
  if (length <= 0) return 0;

  int32_t bytesRead = handle->read(buffer, length);
  file->iPos = handle->position();
  return bytesRead;
}

static int32_t GIFSeekFile(GIFFILE *file, int32_t position) {
  File *handle = static_cast<File *>(file->fHandle);
  handle->seek(position);
  file->iPos = handle->position();
  return file->iPos;
}

bool playGifForDuration(const String& path, unsigned long durationMs, uint32_t version) {
  if (!sdMounted || !display || path.length() == 0) return false;

  unsigned long start = millis();
  while (millis() - start < durationMs) {
    if (!gif.open(path.c_str(), GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw)) {
      Serial.print("Cannot open GIF: ");
      Serial.println(path);
      return false;
    }

    gifXOffset = config.displayXOffset + max(0, (config.viewportWidth - gif.getCanvasWidth()) / 2);
    gifYOffset = max(0, (PANEL_RES_Y - gif.getCanvasHeight()) / 2);
    clearViewport();

    int delayMs = 0;
    while (gif.playFrame(true, &delayMs)) {
      if (version != stateVersion) {
        gif.close();
        return true;
      }

      unsigned long elapsed = millis() - start;
      if (elapsed >= durationMs) {
        gif.close();
        return true;
      }

      unsigned long remaining = durationMs - elapsed;
      unsigned long frameWait = min((unsigned long)max(delayMs, 1), remaining);
      if (!waitWithService(frameWait, version)) {
        gif.close();
        return true;
      }
    }

    gif.close();
    serviceNetwork();
    if (version != stateVersion) return true;
  }

  return true;
}

bool scrollTextOnce(const String& text, uint32_t version) {
  if (!display) return false;

  String message = text.length() ? text : currentGame;
  display->setFont(nullptr);
  display->setTextWrap(false);
  display->setTextSize(1);

  uint16_t color = display->color565(0, 255, 80);
  display->setTextColor(color);

  int16_t x1, y1;
  uint16_t w, h;
  display->getTextBounds(message, 0, 0, &x1, &y1, &w, &h);

  int y = max(0, (PANEL_RES_Y - (int)h) / 2);
  int x = config.viewportWidth;
  int endX = -((int)w + 2);

  while (x > endX) {
    serviceNetwork();
    if (version != stateVersion) return false;

    clearViewport();
    display->setCursor(config.displayXOffset + x, y);
    display->print(message);
    display->flipDMABuffer();

    x--;
    if (!waitWithService(config.textSpeedMs, version)) return false;
  }

  return waitWithService(config.textPauseMs, version);
}

void renderCurrentState() {
  uint32_t version = stateVersion;

  if (displayMode == MODE_TEXT_ONLY || currentGifs.empty()) {
    scrollTextOnce(currentTitle, version);
    return;
  }

  if (currentGifIndex >= currentGifs.size()) currentGifIndex = 0;
  String gifPath = currentGifs[currentGifIndex];
  bool opened = playGifForDuration(gifPath, config.gifIntervalMs, version);
  if (version != stateVersion) return;

  if (!opened) {
    displayMode = MODE_TEXT_ONLY;
    currentTitle = currentTitle.length() ? currentTitle : currentGame;
    bumpState();
    return;
  }

  if (displayMode == MODE_GAME_GIFS && config.showTitleBetweenGifs && currentGifs.size() > 1) {
    scrollTextOnce(currentTitle, version);
    if (version != stateVersion) return;
  }

  currentGifIndex = (currentGifIndex + 1) % currentGifs.size();
}

// --------------------------------------------------------------------
// Arduino lifecycle
// --------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Retro Pixel LED Recalbox dedicated firmware " FIRMWARE_VERSION);

  SPI.begin(VSPI_SCLK, VSPI_MISO, VSPI_MOSI, SD_CS_PIN);
  sdMounted = SD.begin(SD_CS_PIN, SPI, 25000000);
  Serial.println(sdMounted ? "SD mounted" : "SD mount failed");

  loadSdConfig();

  if (!initDisplay()) {
    Serial.println("Display initialization failed");
    return;
  }

  if (!sdMounted) {
    drawViewportMessage("SD ERROR", display->color565(255, 0, 0));
  }

  gif.begin(LITTLE_ENDIAN_PIXELS);
  loadDefaultState();
  connectWifiInitial();
}

void loop() {
  serviceNetwork();

  if (!display) {
    delay(100);
    return;
  }

  renderCurrentState();
  yield();
}

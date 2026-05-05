#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <vector>
#include <algorithm>
#include <Update.h>       // Library for OTA functionality
#include <DNSServer.h>    // Required by WiFiManager
#include <WiFiManager.h>  // Library for WiFi management
#include <PubSubClient.h> // Library for MQTT integration with Home Assistant

// --- HARDWARE LIBRARIES ---
#include "SD.h"           // Micro SD management
#include "AnimatedGIF.h"  // GIF decoder
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h> // LED panel management

// ====================================================================
//                          CONSTANTS & FIRMWARE
// ====================================================================
#define FIRMWARE_VERSION "4.0.0" // Adds support for selecting different GIF playlists
#define PREF_NAMESPACE "pixel_config"
#define DEVICE_NAME_DEFAULT "RetroPixel-Default"
#define GIFS_BASE_PATH "/gifs" // Base directory for GIFs
#define GIF_CACHE_FILE "/gif_cache.txt" // File used to store the GIF index
#define GIF_CACHE_SIG "/gif_cache.sig" // Signature file
#define WIFI_CONFIG_FILE "/wifi_config.txt" // Optional SD WiFi bootstrap file
#define M5STACK_SD SD

// --- NEW HUB75 PIN DEFINITION ---
// These pins use a safe combination outside the SD/Flash bus.
#define CLK_PIN       16
#define OE_PIN        15
#define LAT_PIN       4
#define A_PIN         33
#define B_PIN         32
#define C_PIN         22
#define D_PIN         17
#define E_PIN         -1 // Disabled for 64x32 panels (1/16 scan)
#define R1_PIN        25
#define G1_PIN        26
#define B1_PIN        27
#define R2_PIN        14
#define G2_PIN        12
#define B2_PIN        13

// --- NEW SD SPI PIN DEFINITION (Native VSPI) ---
// These pins use the hardware VSPI bus for better speed.
#define SD_CS_PIN     5   
#define VSPI_MISO     19
#define VSPI_MOSI     23
#define VSPI_SCLK     18

// Panel definitions
const int PANEL_RES_X = 64; 
const int PANEL_RES_Y = 32;
#define MATRIX_HEIGHT PANEL_RES_Y 

// Global system variables
WebServer server(80);
WiFiServer ftpServer(21);
WiFiServer ftpDataServer(50009);
Preferences preferences;
WiFiManager wm;
AnimatedGIF gif;
MatrixPanel_I2S_DMA *display = nullptr; 
TaskHandle_t displayTaskHandle = NULL; // Handle used to stop the task during OTA


// State and playback variables
bool sdMounted = false;
bool inFileManagerMode = false;
bool interruptPlayback = false;
unsigned long gifCachePosition = 0; // Stores the cursor position in the text file
bool hasGifsInCache = false;        // Simple flag to know whether there is content
int x_offset = 0; // Offset for GIF centering
int y_offset = 0;

// Mode variables
int marqueeXPos = 0;
unsigned long lastScrollTime = 0;

// Global variable for the Batocera GIF path
String currentGame = "default";
String currentSystem = "default";
String arcadeGifPath = "/batocera/default/_default.gif";

// --- SD PATH MANAGEMENT ---
String currentPath = "/"; // Current path for the file manager
File fsUploadFile; // Global variable for file uploads
File FSGifFile;    // Global variable for GIF file handling
File currentFile; 
bool pendingGifReload = false; // Indicates whether an SD scan is pending
SemaphoreHandle_t sdMutex; // Semaphore used to protect SD and SPI bus access

// --- FTP SERVER ---
WiFiClient ftpClient;
WiFiClient ftpDataClient;
bool ftpServerRunning = false;
bool ftpLoggedIn = false;
bool ftpUserOk = false;
String ftpCurrentDir = "/";
String ftpRenameFrom = "";
String ftpCommandLine = "";
const uint16_t FTP_PASSIVE_PORT = 50009;


// --- MQTT CONFIGURATION ---
String chipID; // Stores the MAC-based ID

// Clients
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Topics for Auto-Discovery
const char* discovery_topic_bright = "homeassistant/number/retropixel/brightness/config";
const char* discovery_topic_mode = "homeassistant/select/retropixel/mode/config";
const char* discovery_topic_text = "homeassistant/text/retropixel/message/config";

// Topics (Topics)
const char* topic_cmd_mode = "retropixel/cmd/mode";   // Change mode
const char* topic_cmd_bright = "retropixel/cmd/bright"; // Change brightness
const char* topic_state = "retropixel/state";         // Report state to HA
const char* topic_cmd_power = "retropixel/cmd/power";  // Turn the switch on/off
const char* topic_state_power = "retropixel/state/power";  // Report switch state
const char* topic_cmd_text = "retropixel/cmd/text";   // Receive a new message
const char* topic_state_text = "retropixel/state/text"; // Report current message to HA
const char* topic_cmd_text_color = "retropixel/cmd/text_color"; // Receive text color
const char* topic_state_text_color = "retropixel/state/text_color"; //Report text color


// ====================================================================
//                          DATA STRUCTURE
// ====================================================================

struct Config {
    // 1. Playback controls
    bool powerState;
    int brightness = 150;
    int playMode = 0; // 0: GIFs, 1: Text, 3: Arcade
    String slidingText = "Retro Pixel LED v" + String(FIRMWARE_VERSION) + " - IP: " + WiFi.localIP().toString();
    int textSpeed = 50;
    int gifRepeats = 1;
    bool randomMode = false;
    std::vector<String> activeFolders; 
    String activeFolders_str = "/GIFS";
    String activePlaylist;
    // 2. Scrolling text mode settings
    uint32_t slidingTextColor = 0x00FF00;
    // 3. Hardware/System settings
    bool WifiOffMode; // WiFi-free mode
    int panelChain = 2; // "Number of chained LED panels"
    char device_name[40] = {0};
    // 4. Configuration MQTT Home Assistant
    bool mqtt_enabled = false;
    char mqtt_name[40] = "Retro Pixel LED"; // Friendly name for HA
    char mqtt_host[40] = "192.168.1.100";
    int mqtt_port = 1883;
    char mqtt_user[40] = "";
    char mqtt_pass[40] = "";
    // 5. FTP SD access
    bool ftp_enabled = false;
    char ftp_user[24] = "retropixel";
    char ftp_pass[24] = "retropixel";
    // 6. Advanced hardware settings
    int minRefreshRate;  // Refresh rate
    int latchBlanking;   // For ghosting
    int i2sSpeed;        // 0=8Mhz 1=10Mhz, 2=16Mhz, 3=20Mhz
};
Config config;

// List of all folders
std::vector<String> allFolders;
// List of all playlists
std::vector<String> allPlaylists;

// ====================================================================
//            BATOCERA INDEX PATH SEARCHER
// ====================================================================
String searchCache(String systemName, String gameName) {
    systemName.trim(); systemName.toLowerCase();
    gameName.trim(); gameName.toLowerCase();

    // --- LEVEL 1: SEARCH FOR THE GAME (00) ---
    Serial.printf(">> Level 1: Searching GAME [%s] -> [%s]\n", systemName.c_str(), gameName.c_str());
    String foundPath = runSearch(systemName, gameName, "00");
    
    if (foundPath != "") {
        Serial.print(">> GAME MATCH: "); Serial.println(foundPath);
        // If there is a game match, look for variants (_1, _2...) and return one
        return selectRandomVariant(foundPath);
    }

    // --- LEVEL 2: IF THERE IS NO GAME MATCH, SEARCH FOR THE SYSTEM LOGO (01) ---
    Serial.printf(">> Level 2: Game not found. Searching LOGO for [%s]\n", systemName.c_str());
    foundPath = runSearch(systemName, "default", "01"); // Reuse the variable
    
    if (foundPath != "") {
        Serial.print(">> MATCH LOGO: "); Serial.println(foundPath);
        return foundPath;
    }

    // --- LEVEL 3: FULL DEFAULT GIF ---
    Serial.println(">> Level 3: No cache match. Loading absolute default.");
    return "/batocera/default/_default.gif";
}

// Internal function to avoid repeating file-reading code
String runSearch(String systemName, String gameName, String prefix) {
    String result = "";
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000))) {
        File cache = SD.open("/batocera_cache.txt");
        if (cache) {
            while (cache.available()) {
                String line = cache.readStringUntil('\n');
                line.trim();
                
                // Optimize: skip lines that do not start with the prefix (00 or 01)
                if (!line.startsWith(prefix)) continue;

                int p1 = line.indexOf('|');
                int p2 = line.indexOf('|', p1 + 1);
                int p3 = line.lastIndexOf('|');

                if (p1 != -1 && p2 != -1 && p3 != -1) {
                    String cachedSystem = line.substring(p1 + 1, p2);
                    String cachedGame = line.substring(p2 + 1, p3);
                    
                    cachedSystem.trim(); cachedSystem.toLowerCase();
                    cachedGame.trim(); cachedGame.toLowerCase();

                    if (cachedSystem == systemName && cachedGame == gameName) {
                        result = line.substring(p3 + 1);
                        result.trim();
                        break; 
                    }
                }
            }
            cache.close();
        }
        xSemaphoreGive(sdMutex);
    }
    return result;
}

String selectRandomVariant(String originalPath) {
    // 1. Split the extension .gif
    int dotIndex = originalPath.lastIndexOf('.');
    String base = originalPath.substring(0, dotIndex); // e.g.: /batocera/neogeo/mslug
    String ext = originalPath.substring(dotIndex);     // e.g.: .gif

    // 2. Count how many variants exist
    int totalVariants = 0;
    
    // Check mslug_1.gif, mslug_2.gif... up to a maximum of 5
    for (int i = 1; i <= 5; i++) {
        String testPath = base + "_" + String(i) + ext;
        if (SD.exists(testPath)) {
            totalVariants = i;
        } else {
            break; // If _2 does not exist, stop searching
        }
    }

    // 3. If there are no variants, return the original
    if (totalVariants == 0) return originalPath;

    // 4. If variants exist, choose one randomly, including the original as option 0
    int choice = random(0, totalVariants + 1); // Random between 0 and totalVariants
    
    if (choice == 0) return originalPath;
    
    String selectedPath = base + "_" + String(choice) + ext;
    Serial.printf(">> Variant detected! Selected number %d: %s\n", choice, selectedPath.c_str());
    return selectedPath;
}

// Function declarations
void handleRoot();
void handleSave();
void handleConfig();
void handleSaveConfig();
void handleFactoryReset();
void handleRestart();
void handleOTA();
void handleOTAUpload();
void notFound();
void syncMQTTState();

// NEW DECLARATIONS FOR FILE MANAGEMENT
void handleFileManager();
void handleFileUpload();
void handleFileDelete();
String fileManagerPage();

void loadConfig();
void savePlaybackConfig();
void saveSystemConfig();
bool readSdWifiConfig(String& ssid, String& password);
bool connectFromSdWifiConfig();

// Display and mode control functions
void showMessage(const char* messageText, uint16_t color);
void listGifFiles();
void runGifMode();
void runTextMode();
void scanFolders();
void beginFtpServer();
void stopFtpServer();
void handleFtpServer();
void sendFtpResponse(const String& response);
String ftpNormalizePath(String path);
String ftpResolvePath(String path);
bool ftpOpenDataConnection();
void ftpCloseDataConnection();
void ftpSendList(String path, bool namesOnly);
void ftpStoreFile(String path);
void ftpRetrieveFile(String path);
// ====================================================================
//                      CONFIGURATION HANDLING (PREFERENCES)
// ====================================================================

void loadConfig() { 
    preferences.begin(PREF_NAMESPACE, true);

    config.powerState = preferences.getBool("powerState", true);
    config.brightness = preferences.getInt("brightness", 40); // Apply 15% default brightness
    config.playMode = preferences.getInt("playMode", 1);
    if (config.playMode == 2 || config.playMode < 0 || config.playMode > 3) config.playMode = 0;
    config.activePlaylist = preferences.getString("actPlaylist", "auto"); // "auto" is the default value
    config.slidingText = preferences.getString("slidingText", config.slidingText);
    config.textSpeed = preferences.getInt("textSpeed", 50);
    config.gifRepeats = preferences.getInt("gifRepeats", 1);
    config.randomMode = preferences.getBool("randomMode", false);
    config.mqtt_enabled = preferences.getBool("mqtt_en", false);
    String mName = preferences.getString("m_name", "Retro Pixel LED");
    config.slidingTextColor = preferences.getULong("slideColor", 0x00FF00);
    config.WifiOffMode = preferences.getBool("WifiOffMode", false);
    config.panelChain = preferences.getInt("panelChain", 2);
    // Load advanced settings ---
    // Default 90Hz, Blanking 1, Speed 1 (10MHz)
    config.minRefreshRate = preferences.getInt("minRefresh", 120);
    config.latchBlanking = preferences.getInt("latchBlank", 1);
    config.i2sSpeed = preferences.getInt("i2sSpeed", 2);
    // Configuration MQTT
    strncpy(config.mqtt_name, mName.c_str(), sizeof(config.mqtt_name));
    String mHost = preferences.getString("m_host", "192.168.1.100");
    strncpy(config.mqtt_host, mHost.c_str(), sizeof(config.mqtt_host));
    config.mqtt_port = preferences.getInt("m_port", 1883);
    String mUser = preferences.getString("m_user", "");
    strncpy(config.mqtt_user, mUser.c_str(), sizeof(config.mqtt_user));
    String mPass = preferences.getString("m_pass", "");
    strncpy(config.mqtt_pass, mPass.c_str(), sizeof(config.mqtt_pass));
    config.ftp_enabled = preferences.getBool("ftp_en", false);
    String ftpUser = preferences.getString("ftp_user", "retropixel");
    strncpy(config.ftp_user, ftpUser.c_str(), sizeof(config.ftp_user));
    String ftpPass = preferences.getString("ftp_pass", "retropixel");
    strncpy(config.ftp_pass, ftpPass.c_str(), sizeof(config.ftp_pass));
    
    // device_name: Special read for char array
    String nameStr = preferences.getString("deviceName", DEVICE_NAME_DEFAULT);
    strncpy(config.device_name, nameStr.c_str(), sizeof(config.device_name) - 1);
    config.device_name[sizeof(config.device_name) - 1] = '\0';
    
    config.activeFolders_str = preferences.getString("activeFolders", "/GIFS");
    config.activeFolders.clear();
    int start = 0;
    int end = config.activeFolders_str.indexOf(',');
    while (end != -1) {
        config.activeFolders.push_back(config.activeFolders_str.substring(start, end));
        start = end + 1;
        end = config.activeFolders_str.indexOf(',', start);
    }
    // Add the last item, or the only one if there are no commas
    if (start < config.activeFolders_str.length()) {
        config.activeFolders.push_back(config.activeFolders_str.substring(start));
    }
    
    preferences.end();
}

void savePlaybackConfig() { 
    preferences.begin(PREF_NAMESPACE, false);// write mode

    preferences.putBool("powerState", config.powerState);
    preferences.putInt("brightness", config.brightness);
    preferences.putInt("playMode", config.playMode);
    preferences.putString("slidingText", config.slidingText);
    preferences.putInt("textSpeed", config.textSpeed);
    preferences.putInt("gifRepeats", config.gifRepeats);
    preferences.putBool("randomMode", config.randomMode);
    preferences.putString("actPlaylist", config.activePlaylist);
    
    // Save folders (serializing from vector to String)
    String foldersToSave;
    for (size_t i = 0; i < config.activeFolders.size(); ++i) {
        foldersToSave += config.activeFolders[i];
        if (i < config.activeFolders.size() - 1) {               
            foldersToSave += ",";
        }
    }
    config.activeFolders_str = foldersToSave;
    preferences.putString("activeFolders", config.activeFolders_str);
    
    preferences.end();
}

void saveSystemConfig() { 
    preferences.begin(PREF_NAMESPACE, false); // 'false' means write mode
    
    preferences.putBool("powerState", config.powerState);
    const char* newKey = "slideColor";
    preferences.putULong(newKey, config.slidingTextColor);
    preferences.putInt("panelChain", config.panelChain);
    preferences.putBool("WifiOffMode", config.WifiOffMode);
    preferences.putString("deviceName", config.device_name);
    // Advanced settings
    preferences.putInt("minRefresh", config.minRefreshRate);
    preferences.putInt("latchBlank", config.latchBlanking);
    preferences.putInt("i2sSpeed", config.i2sSpeed);
    // Configuration MQTT
    preferences.putBool("mqtt_en", config.mqtt_enabled);
    preferences.putString("m_name", config.mqtt_name);
    preferences.putString("m_host", config.mqtt_host);
    preferences.putInt("m_port", config.mqtt_port);
    preferences.putString("m_user", config.mqtt_user);
    preferences.putString("m_pass", config.mqtt_pass);
    preferences.putBool("ftp_en", config.ftp_enabled);
    preferences.putString("ftp_user", config.ftp_user);
    preferences.putString("ftp_pass", config.ftp_pass);
    
    preferences.end();

}

// ====================================================================
//                      CRITICAL RESTART FUNCTION
// ====================================================================

void handleFactoryReset() {
    preferences.begin(PREF_NAMESPACE, false);
    preferences.clear(); // Deletes all saved settings
    preferences.end();
    
    wm.resetSettings();
// Deletes WiFi configuration
    
    server.send(200, "text/html", "<h2>Full Reset</h2><p>All settings, including the WiFi connection, have been deleted. The device will restart in 3 seconds and start the captive portal.</p>");
    delay(3000);
    ESP.restart();
}


bool readSdWifiConfig(String& ssid, String& password) {
    if (!sdMounted || !SD.exists(WIFI_CONFIG_FILE)) return false;

    File file = SD.open(WIFI_CONFIG_FILE, FILE_READ);
    if (!file) return false;

    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line == "" || line.startsWith("#")) continue;

        int separator = line.indexOf('=');
        if (separator < 0) separator = line.indexOf(':');
        if (separator < 0) continue;

        String key = line.substring(0, separator);
        String value = line.substring(separator + 1);
        key.trim();
        value.trim();
        key.toUpperCase();

        if ((value.startsWith("\"") && value.endsWith("\"")) || (value.startsWith("'") && value.endsWith("'"))) {
            value = value.substring(1, value.length() - 1);
        }

        if (key == "SSID" || key == "WIFI_SSID") ssid = value;
        else if (key == "PASSWORD" || key == "PASS" || key == "WIFI_PASSWORD") password = value;
    }

    file.close();
    return ssid.length() > 0;
}

bool connectFromSdWifiConfig() {
    String sdSsid;
    String sdPassword;
    if (!readSdWifiConfig(sdSsid, sdPassword)) return false;

    Serial.println("SD WiFi config found. Connecting to: " + sdSsid);
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(sdSsid.c_str(), sdPassword.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 25000) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("");

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Connected using SD WiFi config. IP: " + WiFi.localIP().toString());
        config.slidingText = "Retro Pixel LED v" + String(FIRMWARE_VERSION) + " - IP: " + WiFi.localIP().toString();
        return true;
    }

    Serial.println("SD WiFi config connection failed. Falling back to configuration portal.");
    return false;
}

// ====================================================================
//                      HTTP AND WEB HANDLERS
// ====================================================================

// --- Utility to convert HEX color to uint32_t ---
uint32_t parseHexColor(String hex) {
    if (hex.startsWith("#")) hex.remove(0, 1);
// Use 0x00FF00 (green) as the default value if the length is invalid
    if (hex.length() != 6) return 0x00FF00;
    return strtoul(hex.c_str(), NULL, 16);
}

// --- Server routes ---

void handlePower() {
    // 1. Invert current state
    config.powerState = !config.powerState;
    
    // 2. Save to flash memory (powerState is saved in saveSystemConfig)
    saveSystemConfig();
    
    // 3. Notify Home Assistant of the new state
    syncMQTTState();
    
    // 4. Redirect back to the main page
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "OK");
}

void handleSave() { 
    // 1. Read existing arguments
    int tempBrightness = server.hasArg("b") ? server.arg("b").toInt() : config.brightness;
    int tempPlayMode = server.hasArg("pm") ? server.arg("pm").toInt() : config.playMode;
    if (tempPlayMode == 2 || tempPlayMode < 0 || tempPlayMode > 3) tempPlayMode = 0;
    String tempSlidingText = server.hasArg("st") ? server.arg("st") : config.slidingText;
    int tempTextSpeed = server.hasArg("ts") ? server.arg("ts").toInt() : config.textSpeed;
    int tempGifRepeats = server.hasArg("r") ? server.arg("r").toInt() : config.gifRepeats;
    bool tempRandomMode = server.hasArg("m") ? (server.arg("m").toInt() == 1) : config.randomMode;
    
    // --- Playlist parsing with interruption ---
    bool cambioPlaylist = false;
    if (server.hasArg("pl")) {
        String newList = server.arg("pl");
        if (config.activePlaylist != newList) {
            config.activePlaylist = newList;
            interruptPlayback = true; // Enables immediate stop
            gifCachePosition = 0;           // Resets the counter for the new list
            cambioPlaylist = true;
        }
    }

    if (server.hasArg("stc")) config.slidingTextColor = parseHexColor(server.arg("stc"));

    // 2. Temporary folder management
    std::vector<String> tempFolders;
    for(size_t i = 0; i < server.args(); ++i) {
        if (server.argName(i) == "f") {
            tempFolders.push_back(server.arg(i));
        }
    }

    if (tempFolders.empty() && sdMounted) {
         tempFolders.push_back("/");
    }

    // 3. Update configuration in RAM
    config.brightness = tempBrightness;
    config.playMode = tempPlayMode;
    config.slidingText = tempSlidingText;
    config.textSpeed = tempTextSpeed;
    config.gifRepeats = tempGifRepeats;
    config.randomMode = tempRandomMode;
    config.activeFolders = tempFolders;

    if (display) display->setBrightness8(config.brightness);

    // 4. Scan management
    if (config.playMode == 0) {
        // Only schedule folder scanning when no playlist is being used
        if (config.activePlaylist == "auto") {
            pendingGifReload = true; 
            Serial.println(">> Folder scan scheduled...");
        } else {
            pendingGifReload = false;
            // Optional: call a function that counts the selected playlist lines to update config.totalGifsInPlaylist
            Serial.println(">> Using Playlist: " + config.activePlaylist);
        }
        
    }

    // 5. Save to Flash
    savePlaybackConfig();

    // 5.1 Release interruption if there was a change
    if (cambioPlaylist) {
        delay(100); // Small pause to make sure Core 1 receives the signal
        interruptPlayback = false; 
    }

    // 6. Web response
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Saved");

    // 7. Synchronize with Home Assistant
    syncMQTTState();
}

void handleSaveConfig() { 
    // 1. Reset the portal timeout to give us margin
    wm.setConfigPortalTimeout(180); 

    // 2. System and hardware settings
    if (server.hasArg("deviceName")) { 
        String nameStr = server.arg("deviceName");
        strncpy(config.device_name, nameStr.c_str(), sizeof(config.device_name) - 1);
        config.device_name[sizeof(config.device_name) - 1] = '\0';
    }
    if (server.hasArg("pc")) config.panelChain = server.arg("pc").toInt();

    // Advanced settings ---
    if (server.hasArg("i2s")) config.i2sSpeed = server.arg("i2s").toInt();
    if (server.hasArg("mrr")) config.minRefreshRate = server.arg("mrr").toInt();
    if (server.hasArg("lb"))  config.latchBlanking = server.arg("lb").toInt();

    // 3. WiFi settings (Online/Offline)
    if (server.hasArg("wifiOffMode")) {
        config.WifiOffMode = (server.arg("wifiOffMode") == "1");
    }

    // 4. Colors and text
    if (server.hasArg("stc")) config.slidingTextColor = parseHexColor(server.arg("stc"));
    if (server.hasArg("st")) { 
        config.slidingText = server.arg("st"); 
        marqueeXPos = display->width();
    }

    // 5. Configuration MQTT
    config.mqtt_enabled = server.hasArg("mqtt_en");
    if (server.hasArg("m_name")) strncpy(config.mqtt_name, server.arg("m_name").c_str(), sizeof(config.mqtt_name) - 1);
    if (server.hasArg("m_host")) strncpy(config.mqtt_host, server.arg("m_host").c_str(), sizeof(config.mqtt_host) - 1);
    if (server.hasArg("m_port")) config.mqtt_port = server.arg("m_port").toInt();
    if (server.hasArg("m_user")) strncpy(config.mqtt_user, server.arg("m_user").c_str(), sizeof(config.mqtt_user) - 1);
    if (server.hasArg("m_pass")) strncpy(config.mqtt_pass, server.arg("m_pass").c_str(), sizeof(config.mqtt_pass) - 1);

    // 6. FTP access to the SD card
    bool previousFtpEnabled = config.ftp_enabled;
    config.ftp_enabled = server.hasArg("ftp_en");
    if (server.hasArg("ftp_user")) {
        String ftpUser = server.arg("ftp_user");
        ftpUser.trim();
        strncpy(config.ftp_user, ftpUser.c_str(), sizeof(config.ftp_user) - 1);
        config.ftp_user[sizeof(config.ftp_user) - 1] = '\0';
    }
    if (server.hasArg("ftp_pass")) {
        String ftpPass = server.arg("ftp_pass");
        ftpPass.trim();
        strncpy(config.ftp_pass, ftpPass.c_str(), sizeof(config.ftp_pass) - 1);
        config.ftp_pass[sizeof(config.ftp_pass) - 1] = '\0';
    }
    if (config.ftp_user[0] == '\0') strncpy(config.ftp_user, "retropixel", sizeof(config.ftp_user) - 1);
    if (config.ftp_pass[0] == '\0') strncpy(config.ftp_pass, "retropixel", sizeof(config.ftp_pass) - 1);

    // 6. Save to FLASH
    saveSystemConfig();

    if (config.ftp_enabled != previousFtpEnabled) {
        if (config.ftp_enabled) beginFtpServer();
        else stopFtpServer();
    }
    
    // 7. MQTT synchronization 
    // Only when not offline, MQTT is enabled, and there is a real connection
    if (!config.WifiOffMode && config.mqtt_enabled && mqttClient.connected()) {
        String technicalID = "retropixel_" + chipID;
        mqttClient.publish(("retropixel/" + technicalID + "/state/text").c_str(), config.slidingText.c_str(), true);
        syncMQTTState();
        // Note: Do not disconnect so the panel keeps reporting until restart
    }

    // 8. Browser response (302 redirect)
    server.sendHeader("Location", "/config");
    server.send(302, "text/plain", "Configuration Saved");

    Serial.println(">> Configuration saved and conditional MQTT handling applied.");
}

// ====================================================================
//              UNIFIED CYBERPUNK CSS STYLE
// ====================================================================

String getStyle() {
    String s = "* { box-sizing: border-box; }";
    s += "body { font-family: 'Inter', -apple-system, sans-serif; background: radial-gradient(circle at top, #1a1c2c 0%, #0d0e14 100%); color: #fff; margin: 0; padding: 20px; display: flex; flex-direction: column; align-items: center; min-height: 100vh; }";
    s += ".c { max-width: 500px; width: 100%; }";
    s += "h1 { text-align: center; font-size: 26px; font-weight: 800; background: linear-gradient(to right, #00f2ff, #0062ff); -webkit-background-clip: text; -webkit-text-fill-color: transparent; letter-spacing: -1px; margin-bottom: 30px; text-transform: uppercase; }";
    s += "h2 { font-size: 15px; color: #00f2ff; border-bottom: 1px solid rgba(0, 242, 255, 0.3); padding-bottom: 8px; margin-top: 10px; margin-bottom: 20px; text-transform: uppercase; letter-spacing: 1px; }";
    s += ".card { background: rgba(255, 255, 255, 0.03); backdrop-filter: blur(12px); border: 1px solid rgba(255, 255, 255, 0.1); padding: 20px; border-radius: 20px; margin-bottom: 20px; box-shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.5); }";
    
    /* --- NEON BLUE BRIGHTNESS SLIDER --- */
    s += "input[type=range] { width: 100%; -webkit-appearance: none; background: transparent; margin: 15px 0; }";
    s += "input[type=range]::-webkit-slider-runnable-track { width: 100%; height: 6px; background: rgba(255,255,255,0.1); border-radius: 3px; }";
    s += "input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; height: 22px; width: 22px; border-radius: 50%; background: #00f2ff; margin-top: -8px; box-shadow: 0 0 15px #00f2ff, 0 0 5px #fff; border: 2px solid #fff; cursor: pointer; }";

    /* INPUTS Y SELECTS */
    s += "input:not([type='checkbox']):not([type='color']), select { width: 100%; padding: 12px; border-radius: 10px; background: rgba(0,0,0,0.4); border: 1px solid rgba(255,255,255,0.1); color: #fff; font-size: 15px; margin-top: 5px; }";
    s += "label { display: block; margin-top: 15px; font-size: 11px; color: #00f2ff; font-weight: bold; text-transform: uppercase; letter-spacing: 0.5px; }";
    
    /* --- CHECKBOX STYLE --- */
    s += ".cb label { font-weight: normal; text-transform: none; color: #ccc; display: flex; align-items: center; gap: 10px; margin: 10px 0; font-size: 13px; cursor: pointer; }";
    s += ".cb input[type='checkbox'] { width: 18px; height: 18px; accent-color: #00f2ff; cursor: pointer; }";

    /* --- COLOR DESIGN --- */
    s += ".dual-neon { display: grid; grid-template-columns: 1fr 1fr; gap: 15px; margin-top: 15px; }";
    s += ".color-card { background: rgba(255,255,255,0.02); border: 1px solid rgba(255,255,255,0.05); padding: 15px; border-radius: 15px; text-align: center; transition: all 0.3s; }";
    s += ".color-card:hover { border-color: rgba(0, 242, 255, 0.5); background: rgba(0, 242, 255, 0.05); }";
    
    s += "input[type='color'] { -webkit-appearance: none; border: 2px solid rgba(255,255,255,0.1); width: 90px; height: 45px; border-radius: 12px; background: none; cursor: pointer; transition: transform 0.2s; }";
    s += "input[type='color']::-webkit-color-swatch-wrapper { padding: 4px; }";
    s += "input[type='color']::-webkit-color-swatch { border-radius: 8px; border: none; }";

    s += ".info-box { background: rgba(0, 242, 255, 0.05); border-left: 3px solid #00f2ff; padding: 12px; margin-top: 15px; font-size: 11px; color: #aaa; line-height: 1.4; border-radius: 0 8px 8px 0; }";
    s += ".grid-2, .dual-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-top: 10px; }";
    s += ".grid-3 { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 10px; margin-top: 10px; }";
    
    /* NEON BUTTONS */
    s += ".btn { display: flex; align-items: center; justify-content: center; width: 100%; padding: 15px; border-radius: 12px; font-weight: bold; text-align: center; border: none; font-size: 10px; cursor: pointer; text-transform: uppercase; transition: 0.3s; text-decoration: none; min-height: 45px; }";
    s += ".save-btn { background: linear-gradient(45deg, #00b09b, #96c93d); color: #fff; box-shadow: 0 4px 15px rgba(0, 176, 155, 0.4); font-size: 13px; }";
    s += ".settings-btn, .return-btn, .back-btn { background: linear-gradient(45deg, #f39c12, #ff512f); color: #fff; box-shadow: 0 4px 15px rgba(243, 156, 18, 0.4); }";
    s += ".btn-files { background: linear-gradient(45deg, #9b59b6, #da22ff); color: #fff; box-shadow: 0 4px 15px rgba(155, 89, 182, 0.4); }";
    s += ".btn-ota, .btn-reset, .restart-btn { background: linear-gradient(45deg, #3498db, #0575E6); color: #fff; box-shadow: 0 4px 15px rgba(52, 152, 219, 0.4); }";
    s += ".reset-btn { background: linear-gradient(45deg, #e74c3c, #8e0e00); color: #fff; box-shadow: 0 4px 15px rgba(231, 76, 60, 0.4); margin-top: 20px; }";

    s += ".footer { display: flex; justify-content: space-between; font-size: 10px; color: #444; margin-top: 20px; border-top: 1px solid #222; padding-top: 10px; width:100%; }";
    return s;
}

// ====================================================================
//                  MAIN WEB INTERFACE
// ====================================================================

void handleRoot() {
    
    inFileManagerMode = false; // When entering the home page, release the panel so it can show GIFs

    // 1. Ensure maximum performance while loading the web UI
    if (getCpuFrequencyMhz() < 240) setCpuFrequencyMhz(240);

    // Scan the playlists folder to keep names up to date
    scanPlaylists();

    // 2. Tell the browser we will send chunks
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", ""); // Send headers only

    // --- VARIABLES ---
    char hexTextColor[8]; sprintf(hexTextColor, "#%06X", config.slidingTextColor); 
    int brightnessPercent = (int)(((float)config.brightness / 255.0) * 100.0);

    // --- CHUNK 1: HEADER AND STYLES ---
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>";
    html += "<title>Retro Pixel LED</title><link rel='stylesheet' href='/style.css?v=3'></head><body><div class='c'>";
    html += "<h1>Retro Pixel <span style='font-weight:200'>LED</span></h1>";
    server.sendContent(html); // Send the first chunk and free memory

    // --- CHUNK 2: PANEL STATE ---
    html = "<div class='card'><h3>Panel State</h3>";
    if (config.powerState) 
        html += "<a href='/power' class='btn' style='background:rgba(211,47,47,0.1); border:1px solid #ff2e63; color:#ff2e63; box-shadow: 0 0 15px rgba(255,46,99,0.2);'>TURN LED MATRIX OFF</a>";
    else 
        html += "<a href='/power' class='btn' style='background:rgba(56,142,60,0.1); border:1px solid #08d9d6; color:#08d9d6; box-shadow: 0 0 15px rgba(8,217,214,0.2);'>TURN LED MATRIX ON</a>";
    html += "</div><form action='/save' method='POST'>";
    server.sendContent(html);

    // --- CHUNK 3: BRIGHTNESS AND MODE ---
    html = "<div class='card'><h3>Brightness <span id='brightnessValue' style='color:#00f2ff; float:right;'>" + String(brightnessPercent) + "%</span></h3>";
    html += "<input type='range' name='b' min='0' max='255' value='" + String(config.brightness) + "' oninput='updateBrightness(this.value)'></div>";
    
    html += "<div class='card'><h3>Playback Mode</h3>";
    html += "<select name='pm' id='playModeSelect'>";
    html += String("<option value='0'") + (config.playMode == 0 ? " selected" : "") + ">📁 GIF Gallery</option>";
    html += String("<option value='1'") + (config.playMode == 1 ? " selected" : "") + ">📝 Scrolling Text</option>";
    html += String("<option value='3'") + (config.playMode == 3 ? " selected" : "") + ">🕹️ Arcade</option></select></div>";
    server.sendContent(html);

    // --- CHUNK 4: GIF CONFIGURATION ---
    html = "<div id='gifConfig' style='display:" + String(config.playMode == 0 ? "block" : "none") + ";'>";
    html += "<div class='card'><h3>Gallery Settings</h3>";
    
    // Repeats and order
    html += "<div class='grid-2'>";
    html += " <div><label>Repeats</label><input type='number' name='r' min='1' max='50' value='" + String(config.gifRepeats) + "'></div>";
    html += " <div><label>Order</label><select name='m'><option value='0'" + String(config.randomMode ? "" : " selected") + ">Sequential</option><option value='1'" + String(config.randomMode ? " selected" : "") + ">Random</option></select></div>";
    html += "</div>";
    
    // Playlist data source
    html += "<div style='margin-top:20px; background:rgba(255,255,255,0.03); padding:12px; border-radius:8px; border:1px solid rgba(255,255,255,0.05);'>";
    html += " <label style='color:#00f2ff; font-weight:bold; font-size:12px; letter-spacing:1px; text-transform:uppercase;'>GIF Source</label>";
    html += " <select name='pl' id='playlistSelect' style='width:100%; margin-top:8px; background:#111; border:1px solid #333; color:#fff; border-radius:6px; padding:8px;' onchange='toggleFolders(this.value)'>";
    
    // Auto option
    String selAuto = (config.activePlaylist == "auto") ? " selected" : "";
    html += "  <option value='auto'" + selAuto + ">✨ Auto-generate (Use folders)</option>";
    
    // Found .txt lists
    for (const String& p : allPlaylists) {
        String sel = (config.activePlaylist == p) ? " selected" : "";
        html += "  <option value='" + p + "'" + sel + ">📋 Playlist: " + p + "</option>";
    }
    html += " </select></div>";

    // Wrap folders in a div so they can be hidden when a playlist is selected
    html += "<div id='foldersBlock' style='margin-top:20px; display:" + String(config.activePlaylist == "auto" ? "block" : "none") + ";'>";
    html += "<label>SD Folders</label><div class='cb'>";
    if (sdMounted) {
        if (allFolders.empty()) {
            html += "<p style='color:#888; font-size:11px;'>There are no folders in /gifs</p>";
        } else {
            for (const String& f : allFolders) {
                bool isChecked = (std::find(config.activeFolders.begin(), config.activeFolders.end(), f) != config.activeFolders.end());
                html += "<label><input type='checkbox' name='f' value='" + f + "'" + (isChecked ? " checked" : "") + "> " + f + "</label>";
            }
        }
    } else {
        html += "<p style='color:#ff2e63; font-size:11px;'>⚠️ SD NOT DETECTED</p>";
    }
    html += "</div></div></div></div>"; // Close foldersBlock and gifConfig
    server.sendContent(html);

    // --- CHUNK 5: TEXT AND FOOTER (Cleaned) ---
    html = "<div id='textConfig' style='display:" + String(config.playMode == 1 ? "block" : "none") + ";'>";
    html += "<div class='card'><h3>Configuration Text</h3>";
    html += "<label>Custom Message</label><input type='text' name='st' value='" + config.slidingText + "' maxlength='100'>";
    html += "<div class='grid-2' style='align-items: center;'>";
    html += " <div><label>Speed (ms)</label><input type='number' name='ts' min='10' max='1000' value='" + String(config.textSpeed) + "'></div>";
    html += " <div style='text-align:center;'><label>Color</label><input type='color' name='stc' value='" + String(hexTextColor) + "'></div>";
    html += "</div></div></div>"; 

    server.sendContent(html);

    // Buttons and footer
    html = "<button type='submit' class='btn save-btn'>SAVE CHANGES</button>";
    html += "<div class='grid-3'>";
    html += " <a href='/config' class='btn settings-btn'>SETTINGS</a>";
    html += " <a href='/file_manager' class='btn btn-files' onclick='return confirm(\"The panel will stop. Continue?\")'>FILES</a>";
    html += " <a href='/ota' class='btn btn-ota'>OTA</a>";
    html += "</div></form>";
    html += "<div class='footer'><span>v" + String(FIRMWARE_VERSION) + " - fjgordillo86</span><span>IP: " + WiFi.localIP().toString() + "</span></div>";

    // --- CHUNK 6: JAVASCRIPT ---
    html += "<script>";
    html += "function toggleFolders(v){ document.getElementById('foldersBlock').style.display = (v=='auto')?'block':'none'; }";
    html += "function updateBrightness(v){ document.getElementById('brightnessValue').innerHTML=Math.round((v/255)*100)+'%'; }";
    html += "var sel = document.getElementById('playModeSelect');";
    html += "if(sel){ sel.onchange=function(){"; 
    html += " var m = this.value;";
    html += " document.getElementById('gifConfig').style.display = (m=='0')?'block':'none';";
    html += " document.getElementById('textConfig').style.display = (m=='1')?'block':'none';";
    html += "};}"; 
    html += "</script></div></body></html>";
    
    server.sendContent(html);
    
    // 3. Finish sending
    server.sendContent(""); 

}

// ====================================================================
//                  WEB CONFIGURATION UI
// ====================================================================

void handleConfig() {

    // 1. Maximum performance and chunked headers
    if (getCpuFrequencyMhz() < 240) setCpuFrequencyMhz(240);
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");

    // --- CHUNK 1: HEADER AND WIFI ---
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>";
    html += "<title>Configuration</title><link rel='stylesheet' href='/style.css?v=3'></head><body><div class='c'>";
    html += "<h1>Configuration</h1><form action='/save_config' method='POST'>";

    html += "<div class='card'><h2>1. WiFi</h2>";
    html += "<label>Operating Mode</label><select name='wifiOffMode'>";
    html += "<option value='0'" + String(!config.WifiOffMode ? " selected" : "") + ">Online (requires WiFi network)</option>";
    html += "<option value='1'" + String(config.WifiOffMode ? " selected" : "") + ">Offline (does not require WiFi network)</option>";
    html += "</select>";
    html += "<div class='info-box' style='background: #332200; border-left: 4px solid #ffaa00; padding: 10px; margin-top: 10px;'>";
    html += "⚠️ <b>Note:</b> In Offline mode the panel creates its own network. Connect to it to use Retro Pixel LED. ";
    html += "When changing modes, you must <b>Save and Restart</b>.";
    html += "<i>* If no WiFi network is configured within 3 minutes, Offline Mode is enabled automatically.</i></div><br>";
    html += "</div>";
    server.sendContent(html);

    // --- CHUNK 2: HARDWARE ---
    html = "<div class='card'><h2>2. Hardware & Panel</h2>"; 
    html += "<label>Number of Panels (Chain)</label><input type='number' name='pc' min='1' max='8' value='" + String(config.panelChain) + "'>";
    html += "<div class='grid-2'>"; 
    html += "<div><label>Speed I2S</label><select name='i2s'>";
    const char* speeds[] = {"8 MHz (Seguro)", "10 MHz (Estable)", "16 MHz (Normal)", "20 MHz (Turbo)"};
    for(int i=0; i<4; i++) {
        html += "<option value='" + String(i) + "'" + (config.i2sSpeed == i ? " selected" : "") + ">" + speeds[i] + "</option>";
    }
    html += "</select></div>";
    html += "<div><label>Min. Refresh (Hz)</label><input type='number' name='mrr' min='30' max='120' value='" + String(config.minRefreshRate) + "'></div></div>";

    html += "<label style='margin-top:10px;'>Latch Blanking (Anti-Ghosting)</label>";
    html += "<div style='display:flex; gap:10px; align-items:center;'>";
    html += "<input type='range' name='lb' min='1' max='4' step='1' value='" + String(config.latchBlanking) + "' oninput='document.getElementById(\"lbVal\").innerText=this.value'>";
    html += "<span id='lbVal' style='color:#00f2ff; font-weight:bold; font-size:14px; width:20px;'>" + String(config.latchBlanking) + "</span></div>";
    html += "<div class='info-box'>⚠️ <b>Note:</b> Increasing Latch Blanking reduces ghost brightness. A very high refresh rate can destabilize the WiFi connection. <br>Changing these values requires <b>Save and Restart</b>.</div></div>";
    server.sendContent(html);

    // --- CHUNK 3: MQTT ---
    html = "<div class='card'><h2>3. Home Assistant (MQTT)</h2>";
    html += "<div class='cb'><label><input type='checkbox' id='mqtt_en' name='mqtt_en' value='1' onchange='toggleMQTT(this.checked)'" + String(config.mqtt_enabled ? " checked" : "") + "> Enable Integration</label></div>";
    html += "<div id='mqtt_fields' style='display:" + String(config.mqtt_enabled ? "block" : "none") + "; margin-top:10px;'>";
    html += "<label>Device Name</label><input type='text' name='m_name' value='" + String(config.mqtt_name) + "'>";
    html += "<div class='grid-2'><div><label>Broker IP</label><input type='text' name='m_host' value='" + String(config.mqtt_host) + "'></div>";
    html += "<div><label>Port</label><input type='number' name='m_port' value='" + String(config.mqtt_port) + "'></div></div>";
    html += "<label>Username</label><input type='text' name='m_user' value='" + String(config.mqtt_user) + "'>";
    html += "<label>Password</label><input type='password' name='m_pass' value='" + String(config.mqtt_pass) + "'></div></div>";
    server.sendContent(html);

    // --- CHUNK 4: FTP ---
    html = "<div class='card'><h2>4. FTP SD Access</h2>";
    html += "<div class='cb'><label><input type='checkbox' id='ftp_en' name='ftp_en' value='1' onchange='toggleFTP(this.checked)'" + String(config.ftp_enabled ? " checked" : "") + "> Enable FTP for WinSCP</label></div>";
    html += "<div id='ftp_fields' style='display:" + String(config.ftp_enabled ? "block" : "none") + "; margin-top:10px;'>";
    html += "<label>FTP Username</label><input type='text' name='ftp_user' value='" + String(config.ftp_user) + "' maxlength='23'>";
    html += "<label>FTP Password</label><input type='password' name='ftp_pass' value='" + String(config.ftp_pass) + "' maxlength='23'>";
    html += "<div class='info-box'>Use WinSCP with FTP, host " + WiFi.localIP().toString() + ", port 21, passive mode enabled. FTP gives write access to the SD card on your local network.</div></div></div>";
    server.sendContent(html);

    // --- CHUNK 5: BUTTONS AND FOOTER ---
    html = "<div class='dual-grid' style='grid-template-columns: repeat(3, 1fr); margin-bottom: 20px;'>";
    html += "<button type='submit' class='btn save-btn'>SAVE</button>";
    html += "<button type='button' class='btn restart-btn' onclick=\"if(confirm('Restart?')) location.href='/restart';\">RESET</button>";
    html += "<a href='/' class='btn back-btn'>BACK</a>";
    html += "</div></form>";

    html += "<button type='button' class='btn reset-btn' onclick=\"if(confirm('DELETE EVERYTHING?')) location.href='/factory_reset';\">FACTORY RESET</button>";
    html += "<div class='footer'><span>v" + String(FIRMWARE_VERSION) + " - fjgordillo86</span><span>IP: " + WiFi.localIP().toString() + "</span></div>";
    html += "<script>function toggleMQTT(s){document.getElementById('mqtt_fields').style.display=s?'block':'none';}function toggleFTP(s){document.getElementById('ftp_fields').style.display=s?'block':'none';}</script></div></body></html>";
    
    server.sendContent(html);
    server.sendContent(""); // Finish sending
}

// ====================================================================
//                  OTA WEB INTERFACE
// ====================================================================

void handleOTA() {

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<link rel='stylesheet' href='/style.css?v=3'>";
    html += "</head><body><div class='c'>";
    html += "<h1>UPDATE <span style='color:#3498db'>OTA</span></h1>";
  
    html += "<div class='card'>";
    html += "<h3>Firmware</h3>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data' id='upload_form'>";
  
    // Styled file input
    html += "<div style='border: 2px dashed rgba(52, 152, 219, 0.5); padding: 20px; border-radius: 12px; margin-bottom: 20px; text-align: center;'>";
    html += "<input type='file' name='update' style='font-size: 12px; color: #ccc;'>";
    html += "</div>";
  
    // UPLOAD button
    html += "<button type='submit' class='btn btn-ota'>UPLOAD AND UPDATE</button>";
    html += "</form></div>";

    // BACK button 
    html += "<a href='/' class='btn settings-btn' style='margin-top: 10px;'>BACK TO PANEL</a>";

    html += "<div class='footer'>";
    html += "<span>v" + String(FIRMWARE_VERSION) + " - fjgordillo86</span>"; 
    html += "<span>IP: " + WiFi.localIP().toString() + "</span>";
    html += "</div>";

   html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleOTAUpload() {
    HTTPUpload& upload = server.upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        // 1. STOP DISPLAY TASK (prevents core conflict crash)
        if (displayTaskHandle != NULL) {
            vTaskDelete(displayTaskHandle);
            displayTaskHandle = NULL;
        }
        
        // 2. TURN OFF SCREEN AND STOP GIFS
        if (display) display->fillScreen(0);
        gif.close(); // Cerrar acceso SD
        
        Serial.printf("OTA: Starting update: %s\n", upload.filename.c_str());
        
        // 3. START UPDATE
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
        
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
        
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.println("OTA: Success. Restarting...");
            String s = "<!DOCTYPE html><html><head><link rel='stylesheet' href='/style.css?v=3'></head><body><div class='c'><div class='card'><h2>SUCCESS!</h2><p>Update complete.<br>Restarting systemName...</p></div><script>setTimeout(function(){location.href='/';},10000);</script></div></body></html>";
            server.send(200, "text/html", s);
            delay(1000); 
            ESP.restart();
        } else {
            Update.printError(Serial);
            server.send(500, "text/plain", "Error finalizing OTA");
        }
    }
}

void notFound() { server.send(404, "text/plain", "Not Found"); }

void handleRestart() { 
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    // The meta tag is a fallback in case JS fails
    html += "<meta http-equiv='refresh' content='10;url=/'>"; 
    html += "<link rel='stylesheet' href='/style.css?v=3'>";
    html += "</head><body><div class='c'>";
    
    html += "<div class='card' style='text-align:center;'>";
    html += "<h2 style='color:#00f2ff;'>RESTARTING...</h2>";
    html += "<p style='color:#eee;'>The system is restarting to apply changes.</p>";
    html += "<div class='info-box' style='border-color:#00f2ff;'>Wait a few seconds. You will be redirected to the panel automatically.</div>";
    
    // Loading animation
    html += "<div style='margin: 20px auto; width: 40px; height: 40px; border: 4px solid rgba(0, 242, 255, 0.1); border-top: 4px solid #00f2ff; border-radius: 50%; animation: spin 1s linear infinite;'></div>";
    html += "<style>@keyframes spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }</style>";
    
    html += "</div>";
    
    // Increase to 10 seconds to give the ESP32 real time to reconnect to WiFi
    html += "<script>setTimeout(function(){ window.location.href='/'; }, 10000);</script>";
    
    html += "</div></body></html>";
    
    server.send(200, "text/html", html);
    
    // CRITICAL: increase the delay to 2 seconds. 
    // 500ms is sometimes too short for the chip buffer to send all HTML before stopping.
    Serial.println("Restarting ESP32...");
    delay(2000); 
    ESP.restart();
}

// ====================================================================
//                       FILE MANAGEMENT
// ====================================================================

// --- FILE UPLOAD HANDLING (Optimized for Multi-Upload) --- 

void handleFileUpload(){ 
    if (!sdMounted) { 
        server.send(500, "text/plain", "Error: SD not mounted.");
        return; 
    } 
    
    HTTPUpload& upload = server.upload(); 
    
    // 1. File upload START (UPLOAD_FILE_START)
    if (upload.status == UPLOAD_FILE_START) { 
        // The target path is the current path (currentPath) plus the file name
        String uploadPath = currentPath + upload.filename;
        
        Serial.printf("Upload started to: %s\n", uploadPath.c_str()); 
        
        // Open the SD file for writing (overwrites if it exists)
        // The global fsUploadFile variable is used to write the content.
        fsUploadFile = SD.open(uploadPath.c_str(), FILE_WRITE);

        if (!fsUploadFile) {
            Serial.printf("ERROR: Could not open file %s for writing.\n", uploadPath.c_str());
        }
        
    // 2. Data write (UPLOAD_FILE_WRITE)
    } else if (upload.status == UPLOAD_FILE_WRITE) { 
        if(fsUploadFile) { 
            fsUploadFile.write(upload.buf, upload.currentSize);
        } 
        
    // 3. File upload END (UPLOAD_FILE_END)
    } else if (upload.status == UPLOAD_FILE_END) { 
        if (fsUploadFile) { 
            fsUploadFile.close(); 
            Serial.printf("Upload complete. Name: %s | Size: %d bytes\n", upload.filename.c_str(), upload.totalSize);
            
            // Only refresh the GIF list if the uploaded file is a GIF.
            String filename = upload.filename;
            if (filename.endsWith(".gif") || filename.endsWith(".GIF")) {
                 //listGifFiles(); No longer listing here; it now writes directly to SD because it would take too long with too many GIFs
            }
        } else {
            Serial.printf("File upload error %s: Write or open failed.\n", upload.filename.c_str());
        }
    } 
}

// --- FILE AND FOLDER DELETE HANDLING ---

void handleFileDelete() {
    if (!sdMounted) {
        server.send(500, "text/plain", "Error: SD not mounted.");
        return;
    }

    // 1. LOCK: stop the LED panel to get exclusive SD access
    inFileManagerMode = true; 

    if (server.hasArg("name") && server.hasArg("type")) {
        String filename = server.arg("name");
        String filetype = server.arg("type");
        
        // Ensure currentPath ends with '/' before adding the name
        String tempPath = currentPath;
        if (!tempPath.endsWith("/")) tempPath += "/";
        String fullPath = tempPath + filename;
        
        if (filetype == "dir") {
            // Note: rmdir only works if the folder is empty
            if (SD.rmdir(fullPath.c_str())) { 
                Serial.printf("Directory deleted: %s\n", fullPath.c_str());
            } else {
                Serial.printf("ERROR: Could not delete directory (is it empty?): %s\n", fullPath.c_str());
            }
        } else {
            if (SD.remove(fullPath.c_str())) {
                Serial.printf("File deleted: %s\n", fullPath.c_str());
            } else {
                Serial.printf("ERROR: Could not delete file: %s\n", fullPath.c_str());
            }
        }
    }

    // 2. PAUSE: Small pause so the SD updates its file table
    delay(100);

    // 3. REDIRECT: Force the browser to reload the clean file list
    server.sendHeader("Location", "/file_manager?path=" + currentPath);
    server.send(303); 
}

// --- FOLDER CREATION HANDLING ---

void handleCreateDir() {
    inFileManagerMode = true; // <--- 1. SAFETY LOCK: stop the LED panel immediately

    if (server.hasArg("name")) {
        String newDirName = server.arg("name");
        
        // Clean the folder name (remove spaces or invalid slashes)
        newDirName.trim(); 
        
        // Build the path using currentPath, which should already be clean
        String fullPath = currentPath;
        if (!fullPath.endsWith("/")) fullPath += "/";
        fullPath += newDirName;

        if (SD.mkdir(fullPath.c_str())) {
            Serial.printf("Folder created successfully: %s\n", fullPath.c_str());
        } else {
            Serial.printf("ERROR creating: %s (SD full or write-protected?)\n", fullPath.c_str());
        }
    }

    // 2. TECHNICAL PAUSE: Give the SD file table 100 ms to settle
    delay(100); 

    // 3. CLEAN REDIRECT: Use 303 (See Other) instead of 302 to force a fresh GET
    server.sendHeader("Location", "/file_manager?path=" + currentPath);
    server.send(303); 
}

// --- FILE HANDLING (DISPLAY AND NAVIGATION) ---

void handleFileManager() {
    inFileManagerMode = true; // Put the panel in "FILES MODE"

    String requestedPath = server.hasArg("path") ? server.arg("path") : "/";
    
    // Path cleanup
    if (!requestedPath.startsWith("/")) requestedPath = "/" + requestedPath;
    
    // When entering here, clear the SD file cache
    // so the next read is real.
    currentPath = requestedPath;
    
    // Send response headers that prevent browser caching
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    
    server.send(200, "text/html", fileManagerPage(currentPath));
}

String fileManagerPage(String path) {
    // 1. Initial safety check: is the SD mounted? 
    if (!sdMounted) {
        return "<!DOCTYPE html><html><head><meta charset='UTF-8'><link rel='stylesheet' href='/style.css?v=3'></head><body>"
               "<div class='c'><div class='card'><h2>⚠️ SD Not Detected</h2>"
               "<div class='info-box'>Make sure the SD card is inserted and restart the device.</div>"
               "<a href='/' class='btn back-btn'>BACK HOME</a></div></div></body></html>";
    }

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>File Manager</title><link rel='stylesheet' href='/style.css?v=3'></head><body><div class='c'>";
    html += "<h1>File <span style='font-weight:200'>Manager</span></h1>";

    // --- CARD 1: MULTIPLE UPLOAD ---
    html += "<div class='card'><h2>Upload Files</h2>";
    html += "<div class='info-box'>Uploading to: <code>" + path + "</code></div>";
    html += "<form method='POST' action='/upload' enctype='multipart/form-data' style='margin-top:15px;'>";
    html += "<input type='file' name='upload' id='file-input' multiple style='display:none;' onchange='document.getElementById(\"file-name\").innerHTML = this.files.length + \" file(s) selected\"'>";
    html += "<label for='file-input' class='btn' style='background:rgba(0,242,255,0.05); border:1px dashed #00f2ff; color:#00f2ff; margin-bottom:10px;'>📂 SELECT GIFS</label>";
    html += "<div id='file-name' style='font-size:10px; text-align:center; margin-bottom:15px; color:#666; font-family:monospace;'>No file selected</div>";
    html += "<input type='hidden' name='dir' value='" + path + "'>";
    html += "<button type='submit' class='btn save-btn'>START UPLOAD</button>";
    html += "</form></div>";

    // --- CARD 2: CREATE FOLDER ---
    html += "<div class='card'><h2>New Folder</h2>";
    html += "<form action='/create_dir' method='POST' style='margin-top:10px;'>";
    html += "<input type='text' name='name' placeholder='Folder name' required style='margin-bottom:10px;'>";
    html += "<button type='submit' class='btn btn-ota'>CREATE DIRECTORY</button>";
    html += "</form></div>";

    // --- CARD 3: EXPLORER ---
    html += "<div class='card'><h2>SD Explorer</h2>";
    html += "<div style='background:rgba(0,0,0,0.3); border-radius:15px; overflow:hidden; border:1px solid rgba(255,255,255,0.05);'>";

    // Flexbox container to align buttons on the same line
    html += "<div style='display:flex; justify-content:space-between; align-items:center; padding:10px 15px; border-bottom:1px solid rgba(255,255,255,0.1); background:rgba(255,255,255,0.02);'>";

    // 1. Back button
    if (path != "/") {
    String parentPath = path.substring(0, path.lastIndexOf('/', path.length() - 2) + 1);
    if (parentPath == "") parentPath = "/";
    html += "<a href='/file_manager?path=" + parentPath + "' style='color:#00f2ff; text-decoration:none; font-size:11px; font-weight:bold; display:flex; align-items:center;'>⬅️ UP ONE LEVEL</a>";
    } else {
    // Empty spacer to keep the refresh button on the right when at the root
    html += "<span></span>"; 
    }
    // 2. Refresh button
    // Use the same color style with a neon green accent for differentiation
    html += "<a href='/file_manager?path=" + path + "' style='color:#2ecc71; text-decoration:none; font-size:11px; font-weight:bold; display:flex; align-items:center; gap:5px;'>REFRESH 🔄</a>";
    html += "</div>"; // Close the Flexbox container

    // Directory open with a small safety retry
    File root = SD.open(path.c_str());
    if (!root && path != "/") { 
        vTaskDelay(pdMS_TO_TICKS(50));
        root = SD.open(path.c_str()); 
    }

    if (!root || !root.isDirectory()) {
        html += "<p style='padding:20px; text-align:center; color:#ff2e63;'>Error reading directory. <a href='/file_manager?path=" + path + "' style='color:#00f2ff;'>Retry</a></p>";
    } else {
        File file = root.openNextFile();
        int count = 0;
        while (file) {
            count++;
            String fileName = String(file.name());
            int lastSlash = fileName.lastIndexOf('/');
            if (lastSlash != -1) fileName = fileName.substring(lastSlash + 1);
            
            bool isDir = file.isDirectory();
            html += "<div style='display:flex; justify-content:space-between; align-items:center; padding:12px 15px; border-bottom:1px solid rgba(255,255,255,0.03);'>";
            
            if (isDir) {
                html += "<a href='/file_manager?path=" + path + fileName + "/' style='color:#f39c12; text-decoration:none; font-size:13px; display:flex; align-items:center; gap:8px;'>📁 " + fileName + "</a>";
                html += "<a href='#' onclick='postDelete(\"" + fileName + "\", \"dir\"); return false;' style='color:#ff2e63; text-decoration:none; font-size:20px;'>&times;</a>";
            } else {
                html += "<div style='display:flex; flex-direction:column;'><span style='color:#eee; font-size:13px;'>📄 " + fileName + "</span><span style='color:#555; font-size:9px;'>" + String(file.size() / 1024) + " KB</span></div>";
                html += "<a href='#' onclick='postDelete(\"" + fileName + "\", \"file\"); return false;' style='color:#ff2e63; text-decoration:none; font-size:20px;'>&times;</a>";
            }
            html += "</div>";
            
            file.close(); 
            file = root.openNextFile();
        }
        root.close();
        if (count == 0) html += "<p style='padding:30px; text-align:center; color:#444; font-size:12px;'>This folder is empty</p>";
    }
    html += "</div></div>";

    // Button to return home
    html += "<div style='text-align:center; margin-top:20px; margin-bottom:20px;'>";
    html += "<a href='/' class='btn back-btn' style='display:inline-block; width:auto; min-width:200px; max-width:90%;'>BACK TO MAIN PANEL</a>";
    html += "</div>";

    uint64_t totalBytes = SD.totalBytes();
    uint64_t freeBytes = totalBytes - SD.usedBytes();
    uint32_t totalMB = (uint32_t)(totalBytes / (1024 * 1024));
    uint32_t libreMB = (uint32_t)(freeBytes / (1024 * 1024));

    html += "<div class='footer'>";
    html += "<span>v" + String(FIRMWARE_VERSION) + " - fjgordillo86</span>"; 
    html += "<span style='color:#888;'>SD: <strong style='color:#00f2ff;'>" + String(libreMB) + " MB</strong> Free / " + String(totalMB) + " MB</span>";
    html += "<span>IP: " + WiFi.localIP().toString() + "</span>";
    html += "</div>";

    html += "</div></body></html>";

    // SCRIPT TO HANDLE DELETE THROUGH POST
    html += "<script>"
            "function postDelete(name, type) {"
            "  if(!confirm('Are you sure you want to delete ' + name + '?')) return;"
            "  var form = document.createElement('form');"
            "  form.method = 'POST';"
            "  form.action = '/delete';"
            "  var inputName = document.createElement('input');"
            "  inputName.type = 'hidden'; inputName.name = 'name'; inputName.value = name;"
            "  var inputType = document.createElement('input');"
            "  inputType.type = 'hidden'; inputType.name = 'type'; inputType.value = type;"
            "  form.appendChild(inputName);"
            "  form.appendChild(inputType);"
            "  document.body.appendChild(form);"
            "  form.submit();"
            "}"
            "</script>";

    return html;
}

// ====================================================================
//                   CORE DISPLAY FUNCTIONS
// ====================================================================

// --- 1. AnimatedGIF callback functions ---

// GIF library drawing function
void GIFDraw(GIFDRAW *pDraw)
{
    // Variables must be declared globally or as extern
    extern int x_offset; 
    extern int y_offset; 
    extern MatrixPanel_I2S_DMA *display; 

    uint8_t *s;
    uint16_t *d, *usPalette, usTemp[320];
    int x, y, iWidth;
    int iCount; // Variable for counting opaque pixels

    if (!display || interruptPlayback) return; 

    // BaseX: Frame starting point, including centering offset
    int baseX = pDraw->iX + x_offset; 
    
    // Frame height and width
    iWidth = pDraw->iWidth;
    if (iWidth > 128) 
        iWidth = 128; 
        
    usPalette = pDraw->pPalette;
    
    // Y: drawing start coordinate, including centering offset
    y = pDraw->iY + pDraw->y + y_offset; 

    s = pDraw->pPixels;

    // Logic for frames with transparency or disposal method
    if (pDraw->ucHasTransparency) { 
        
        iCount = 0;
        
        for (x = 0; x < iWidth; x++) {
            if (s[x] == pDraw->ucTransparent) {
                if (iCount) { 
                    for(int xOffset_ = 0; xOffset_ < iCount; xOffset_++ ){
                        // 🛑 FIX: ADD 128 (first line)
                        display->drawPixel(baseX + x - iCount + xOffset_ + 128, y, usTemp[xOffset_]); 
                    }
                    iCount = 0;
                }
            } else {
                usTemp[iCount++] = usPalette[s[x]];
            }
        }
        
        if (iCount) {
            for(int xOffset_ = 0; xOffset_ < iCount; xOffset_++ ){
                // 🛑 FIX: ADD 128 (second line)
                display->drawPixel(baseX + x - iCount + xOffset_ + 128, y, usTemp[xOffset_]); 
            }
        }

    } else { // No transparency (simple full-line draw)
        s = pDraw->pPixels;
        for (x=0; x<iWidth; x++)
            // 🛑 FIX: ADD 128 (third line)
            display->drawPixel(baseX + x + 128, y, usPalette[*s++]); 
    }
} /* GIFDraw() */

// SD file management functions
static void * GIFOpenFile(const char *fname, int32_t *pSize)
{
    // Use the global FSGifFile variable
    extern File FSGifFile; 
    FSGifFile = M5STACK_SD.open(fname);
    if (FSGifFile) {
        *pSize = FSGifFile.size();
        return (void *)&FSGifFile; // Return a pointer to the global variable
    }
    return NULL;
}

static void GIFCloseFile(void *pHandle)
{
    File *f = static_cast<File *>(pHandle);
    if (f != NULL)
        f->close();
}

static int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
    int32_t iBytesRead;
    iBytesRead = iLen;
    File *f = static_cast<File *>(pFile->fHandle);
    // The ugly workaround from the example
    if ((pFile->iSize - pFile->iPos) < iLen)
        iBytesRead = pFile->iSize - pFile->iPos - 1; 
    if (iBytesRead <= 0)
        return 0;
    iBytesRead = (int32_t)f->read(pBuf, iBytesRead);
    pFile->iPos = f->position();
    return iBytesRead;
}


static int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition)
{
    File *f = static_cast<File *>(pFile->fHandle);
    f->seek(iPosition);
    pFile->iPos = (int32_t)f->position();
    return pFile->iPos;
}

// --- 2. Display utility functions ---

void showMessage(const char* messageText, uint16_t color = 0xF800 ) {
    if (!display) return;
    display->fillScreen(0);
    display->setTextSize(1);
    display->setTextWrap(false);
    display->setTextColor(color);
    display->setCursor(0, MATRIX_HEIGHT / 2 - 4);
    display->print(messageText);
    display->flipDMABuffer();
}

// ====================================================================
//                  FOLDER AND LIST SCAN FUNCTION FOR THE UI
// ====================================================================

// Function that scans and lists only folders inside a base path
void scanFolders(String basePath) {
    allFolders.clear(); // Clear the global folder list
    
    // 1. Make sure the base directory exists.
    if (!SD.exists(basePath)) {
        SD.mkdir(basePath); // Create the /gifs folder if it does not exist
        Serial.printf("Base directory created: %s\n", basePath.c_str());
        return; 
    }

    File root = SD.open(basePath);
    if (!root || !root.isDirectory()) {
        Serial.printf("Error: %s is not a valid directory.\n", basePath.c_str());
        return;
    }
    
    Serial.printf("Scanning subfolders inside: %s\n", basePath.c_str());
    
    File entry = root.openNextFile();
    while(entry){
        if(entry.isDirectory()){
            String dirName = entry.name();
            // Build the full path (example: /gifs/animals)
            String fullPath = basePath + "/" + dirName; 
            
            // Add to the list shown in the web UI
            allFolders.push_back(fullPath); 
        }
        entry = root.openNextFile();
    }
    root.close();
    
    // Sort the list alphabetically for the web UI
    if (allFolders.size() > 0) {
        std::sort(allFolders.begin(), allFolders.end());
    }
}


void scanPlaylists() {
    allPlaylists.clear();
    File root = SD.open("/playlists");
    if (!root || !root.isDirectory()) {
        SD.mkdir("/playlists"); // Create it if it does not exist
        return;
    }

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String n = file.name();
            if (n.endsWith(".txt")) {
                allPlaylists.push_back(n);
            }
        }
        file = root.openNextFile();
    }
    root.close();
}

// ====================================================================
//                 GIF FILE AND CACHE MANAGEMENT
// ====================================================================
// Modified function for writing directly to the cache file
void scanGifDirectory(File &cacheFile, String path) {
    File root = SD.open(path);
    if (!root) {
        Serial.printf("Error opening directory: %s\n", path.c_str());
        return;
    }
    
    File entry = root.openNextFile();
    while(entry){
        yield(); // Allow the system to handle background processes
        if(!entry.isDirectory()){
            String fileName = entry.name();
            if (fileName.endsWith(".gif") || fileName.endsWith(".GIF")) {
                String fullPath = path + "/" + fileName;
                // Clean the double slash when the path is root (/)
                if (path == "/") fullPath = fileName;
                
                // DIRECT WRITE TO SD
                cacheFile.println(fullPath);
                hasGifsInCache = true; // Mark that at least one GIF was found
            }
        }
        entry = root.openNextFile();
    }
    root.close();
}


// Helper function that generates the current configuration signature
String generateCacheSignature() {
    String signature = "";
    // MODIFIED: Use config.activeFolders, the vector where selected folders are stored
    for (const String& folder : config.activeFolders) { 
        signature += folder + ":"; // Concatenate paths separated by :
    }
    return signature;
}

// Main GIF file listing function (uses signature validation logic)
void listGifFiles() {
    // 1. Show an informational message on the LED panel 
    if (display) {
        display->fillScreen(0);
        display->setTextColor(display->color565(0, 242, 255)); // Cyberpunk cyan
        display->setTextSize(1);
        display->setCursor(168, 7);
        display->print("LISTING");
        display->setCursor(168, 17);
        display->print("GIFS...");
        // Optional: a visual progress line
        display->drawFastHLine(160, 27, 64, display->color565(255, 0, 100)); 
    }

    // 2. Generate a signature from active folders
    String currentSignature = generateCacheSignature();
    if (currentSignature.length() == 0) { 
        hasGifsInCache = false;
        Serial.println("No folders selected.");
        return;
    }

    // 3. Check whether the signature changed to avoid unnecessary scans
    bool cacheIsValid = false;
    if (SD.exists(GIF_CACHE_SIG)) {
        File sigFile = SD.open(GIF_CACHE_SIG, FILE_READ);
        if (sigFile) {
            String savedSignature = sigFile.readStringUntil('\n');
            savedSignature.trim();
            // If the signature matches and the file exists, the cache is valid
            if (savedSignature == currentSignature && SD.exists(GIF_CACHE_FILE)) {
                cacheIsValid = true;
            }
            sigFile.close();
        }
    }

    // 4. If the cache is valid, do not scan the SD; just reset the pointer
    if (cacheIsValid) {
        Serial.println("Valid cache detected. Using existing list.");
        gifCachePosition = 0; 
        hasGifsInCache = true;
        return;
    }

    // 5. If it is not valid because folders changed or this is the first boot, regenerate it
    Serial.println("Generating a new GIF index on SD...");
    
    // Delete the previous cache to start clean
    if (SD.exists(GIF_CACHE_FILE)) SD.remove(GIF_CACHE_FILE);

    File cacheFile = SD.open(GIF_CACHE_FILE, FILE_WRITE);
    if (!cacheFile) {
        Serial.println("CRITICAL ERROR: Cannot write to SD.");
        return;
    }

    hasGifsInCache = false;
    // Scan each active folder
    for (const String& path : config.activeFolders) { 
        scanGifDirectory(cacheFile, path);
    }
    
    cacheFile.close();

    // 6. Save the new signature
    File newSigFile = SD.open(GIF_CACHE_SIG, FILE_WRITE);
    if (newSigFile) {
        newSigFile.print(currentSignature);
        newSigFile.close();
    }
    
    // Reset the read position to the start of the new file
    gifCachePosition = 0;
    Serial.println("Index generated successfully on SD.");
}

// --- Playback mode functions ---
// Global variables required by the loop
extern unsigned long lastFrameTime; // Declare this variable globally if it does not already exist
// extern WebServer server; // Assuming the server object is global

String getNextGifFromSD() {
    // 1. Determine which file will be opened
    String listPath = (config.activePlaylist == "auto") ? GIF_CACHE_FILE : "/playlists/" + config.activePlaylist;
    
    // 2. Existence check
    if (!SD.exists(listPath)) {
        Serial.println(">> Error: list not found: " + listPath);
        // If the playlist fails, try to return to auto mode
        if (config.activePlaylist != "auto") {
            listPath = GIF_CACHE_FILE;
            if (!SD.exists(listPath)) return ""; // If the cache does not exist either, abort
        } else {
            return "";
        }
    }

    File cacheFile = SD.open(listPath, FILE_READ);
    if (!cacheFile) return "";

    // --- RANDOM MODE LOGIC (Based on byte position) ---
    uint32_t fileSize = cacheFile.size();
    
    if (config.randomMode && fileSize > 20) {
        // Choose a random index in the file
        uint32_t randomPos = esp_random() % (fileSize - 10);
        cacheFile.seek(randomPos);

        // If we are not at the start of the file, advance to the next '\n'
        // to make sure a full path is read.
        if (randomPos != 0) {
            while (cacheFile.available()) {
                if (cacheFile.read() == '\n') break;
            }
        }
    }
    // --- SEQUENTIAL MODE LOGIC ---
    else {
        // If the playlist changed, gifCachePosition could be larger than the new file size
        if (gifCachePosition >= fileSize) gifCachePosition = 0;
        cacheFile.seek(gifCachePosition);
    }

    // --- COMMON READ ---
    if (!cacheFile.available()) {
        cacheFile.seek(0);
        if (!config.randomMode) gifCachePosition = 0;
    }

    String gifPath = cacheFile.readStringUntil('\n');
    gifPath.trim();

    if (!config.randomMode) {
        gifCachePosition = cacheFile.position();
    }
    
    cacheFile.close();

    // Final check: if the read line is empty, try to return the first one
    if (gifPath.length() < 3) return "";

    return gifPath;
}

void runGifMode() {
    if (!display) return; 
    
    // 1. CHANGE: check the new hasGifsInCache flag instead of the vector
    if (!sdMounted || !hasGifsInCache) {
        showMessage(!sdMounted ? "SD ERROR" : "NO GIFS");
        delay(200);
        // If the SD is mounted but no GIFs were detected, try to regenerate the list
        if (sdMounted) listGifFiles();
        return;
    }

    // 2. CHANGE: removed the vector index rotation block.
    // Instead, request the next path directly from the SD file.
    String gifPath = getNextGifFromSD(); 
    
    // If the function returns empty due to EOF or error, exit and retry on the next cycle
    if (gifPath == "") {
        return;
    }
    
    // GIF repeat loop (config.gifRepeats)
    for (int rep = 0; rep < config.gifRepeats; ++rep) { 
        // EXIT IF THE MODE CHANGES OR POWER TURNS OFF
        if (config.playMode != 0 || !config.powerState || inFileManagerMode ) return;
        
        if (gif.open(gifPath.c_str(), GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw)) {
            
            x_offset = (128 - gif.getCanvasWidth()) / 2; 
            y_offset = (32 - gif.getCanvasHeight()) / 2; 

            display->clearScreen(); 

            int delayMs;
            
            // Main frame playback loop
            while (gif.playFrame(true, &delayMs)) {
                
                // Power-off / manager mode check
                if (!config.powerState || inFileManagerMode || config.playMode != 0 || pendingGifReload || interruptPlayback) {
                    gif.close();
                    return; // Exit playback immediately
                }

                // WebServer handling and non-blocking wait
                server.handleClient(); 
                
                unsigned long targetTime = millis() + delayMs;
                while (millis() < targetTime) {
                    // Check again during the frame wait
                    if (!config.powerState || inFileManagerMode || interruptPlayback) {
                        gif.close();
                        return;
                    }
                    server.handleClient(); 
                    yield(); 
                }
            }
            gif.close();
            
        } else {
            Serial.printf("Error opening GIF: %s\n", gifPath.c_str());
            // If a specific file fails, do not stop everything; just show a brief error
            showMessage("Error GIF", display->color565(255, 255, 0));
            
            unsigned long start = millis();
            while (millis() - start < 1000) {
                 if (!config.powerState || inFileManagerMode) return;
                 server.handleClient();
                 yield();
            }
        }
    }

    // 3. CHANGE: removed currentGifIndex++; 
    // getNextGifFromSD() already advances the cursor automatically.
}

void runTextMode() {
    if (!display) return; 

    // 1. Color configuration
    uint16_t textDisplayColor = display->color565(
        (config.slidingTextColor >> 16) & 0xFF,
        (config.slidingTextColor >> 8) & 0xFF,
        config.slidingTextColor & 0xFF
    ); 

    display->setTextSize(1); 
    display->setTextWrap(false); 
    display->setTextColor(textDisplayColor); 

    // 2. Timing control for scrolling
    if (millis() - lastScrollTime > config.textSpeed) {
        lastScrollTime = millis(); 
        marqueeXPos--;

        // Calculate the real width in pixels 
        int16_t x1, y1;
        uint16_t w, h;
        display->getTextBounds(config.slidingText, 0, 0, &x1, &y1, &w, &h);

        // If the text finished scrolling
        if (marqueeXPos < -((int)w)) {
            marqueeXPos = display->width(); 
        }

        // 3. Drawing
        display->fillScreen(0); // Clean buffer
        // MATRIX_HEIGHT / 2 - 4 usually centers the standard 7-8px font well
        display->setCursor(marqueeXPos, (MATRIX_HEIGHT / 2) - (h / 2));
        display->print(config.slidingText);
        display->flipDMABuffer(); 
    }
}

void runArcadeMode() {
    // If we get here while already in manager mode, exit before opening anything
    if (inFileManagerMode) return;

    char pathChar[128];
    arcadeGifPath.toCharArray(pathChar, sizeof(pathChar));
    
    if (gif.open(pathChar, GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw)) {
        // Main GIF loop
        while (gif.playFrame(true, NULL)) {
            
            // If the server enables inFileManagerMode to read the cache,
            // close the file and exit the function immediately.
            if (inFileManagerMode || config.playMode != 3) { 
                gif.close(); 
                return; 
            }
            
            yield(); // Keep the system alive
        }
        gif.close();
    } else {
        // If it fails, return to default
        arcadeGifPath = "/batocera/default/_default.gif";
    }
}


// ====================================================================
//                                MQTT
// ====================================================================

void sendMQTTDiscovery() {
    mqttClient.setBufferSize(1024);

    // 1. DEFINE IDs
    // Use chipID (the MAC) so it is unique and stable
    String technicalID = "retropixel_" + chipID; 
    
    // The friendly name set by the user in the web UI
    String friendlyName = (String(config.mqtt_name).length() > 0) ? String(config.mqtt_name) : "Retro Pixel " + chipID;

    // 2. CREATE THE DEVICE JSON
    String deviceJSON = ",\"dev\":{";
    deviceJSON += "\"ids\":[\"" + technicalID + "\"],";
    deviceJSON += "\"name\":\"" + friendlyName + "\",";
    deviceJSON += "\"sw\":\"" + String(FIRMWARE_VERSION) + "\",";
    deviceJSON += "\"mdl\":\"Retro Pixel LED\",";
    deviceJSON += "\"mf\":\"fjgordillo86\",";
    deviceJSON += "\"cu\":\"http://" + WiFi.localIP().toString() + "/\"";
    deviceJSON += "}";

    // 3. ENTITIES (Use technicalID so topics are unique per panel)
    
    // --- MODE SELECT ---
    String modeConfig = "{\"name\":\"Mode\",\"stat_t\":\"retropixel/" + technicalID + "/state/mode\",\"cmd_t\":\"retropixel/" + technicalID + "/cmd/mode\",\"options\":[\"GIFs\",\"Text\",\"Arcade\"],\"uniq_id\":\"" + technicalID + "_mode\"" + deviceJSON + "}";
    mqttClient.publish(("homeassistant/select/" + technicalID + "/mode/config").c_str(), modeConfig.c_str(), true);

    // --- SWITCH POWER ---
    String powerConfig = "{\"name\":\"State\",\"stat_t\":\"retropixel/" + technicalID + "/state/power\",\"cmd_t\":\"retropixel/" + technicalID + "/cmd/power\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"uniq_id\":\"" + technicalID + "_power\"" + deviceJSON + "}";
    mqttClient.publish(("homeassistant/switch/" + technicalID + "/power/config").c_str(), powerConfig.c_str(), true);

    // --- BRIGHTNESS ---
    String brightConfig = "{\"name\":\"Brightness\",\"stat_t\":\"retropixel/" + technicalID + "/state/bright\",\"cmd_t\":\"retropixel/" + technicalID + "/cmd/bright\",\"min\":0,\"max\":255,\"uniq_id\":\"" + technicalID + "_bright\"" + deviceJSON + "}";
    mqttClient.publish(("homeassistant/number/" + technicalID + "/bright/config").c_str(), brightConfig.c_str(), true);

    // --- TEXTBOX FOR TEXT MODE ---
    String textConfig = "{\"name\":\"Display Text\",\"stat_t\":\"retropixel/" + technicalID + "/state/text\",\"cmd_t\":\"retropixel/" + technicalID + "/cmd/text\",\"mode\":\"text\",\"min\":1,\"max\":100,\"uniq_id\":\"" + technicalID + "_text\"" + deviceJSON + "}";
    mqttClient.publish(("homeassistant/text/" + technicalID + "/text/config").c_str(), textConfig.c_str(), true);
    // Force the state update with the current ESP32 value
    mqttClient.publish(("retropixel/" + technicalID + "/state/text").c_str(), config.slidingText.c_str(), true);

    // --- COLOR WHEEL FOR TEXT ---
    String textLightConfig = "{\"name\":\"Text Color\",\"stat_t\":\"retropixel/" + technicalID + "/state/text_color\",\"cmd_t\":\"retropixel/" + technicalID + "/cmd/text_color\",\"rgb_cmd_t\":\"retropixel/" + technicalID + "/cmd/text_color/set\",\"rgb_stat_t\":\"retropixel/" + technicalID + "/state/text_color/set\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"uniq_id\":\"" + technicalID + "_text_rgb\"" + deviceJSON + "}";    
    mqttClient.publish(("homeassistant/light/" + technicalID + "/text_color/config").c_str(), textLightConfig.c_str(), true);

    Serial.print("Discovery sent for:  ");
    Serial.println(friendlyName);
}

void reconnectMQTT() {
    // If MQTT is not enabled in the web UI, exit immediately
    if (!config.mqtt_enabled) return;

    int retries = 0;
    const int maxRetries = 5;
    String technicalID = "retropixel_" + chipID;

    while (!mqttClient.connected() && retries < maxRetries) {
        Serial.printf("Trying MQTT connection (Attempt %d/%d)...\n", retries + 1, maxRetries);
        
        // Configure the server with the current web settings
        mqttClient.setServer(config.mqtt_host, config.mqtt_port);

        // Try to connect using technicalID as the unique ClientID
        if (mqttClient.connect(technicalID.c_str(), config.mqtt_user, config.mqtt_pass)) {
            Serial.println("Connected to MQTT successfully!");
            
            // 1. DYNAMIC WILDCARD SUBSCRIPTION
            // Listen to everything HA sends to our ID
            mqttClient.subscribe(("retropixel/" + technicalID + "/cmd/#").c_str());
            Serial.println("Subscribed to: retropixel/" + technicalID + "/cmd/#");

            // 2. Send Discovery so it appears or updates in HA
            sendMQTTDiscovery();

             // 3. Synchronize the current state
            syncMQTTState();

            retries = 0; // Reset counter on connection
        } else {
            Serial.print("Connection failed, rc=");
            Serial.print(mqttClient.state());
            Serial.println(" retrying in 5 seconds...");
            
            retries++;
            
            // 5-second wait without blocking the core (Core 0)
            vTaskDelay(pdMS_TO_TICKS(5000)); 

            // If MQTT is disabled from the web UI during the wait, exit
            if (!config.mqtt_enabled) return;
        }
    }

    if (retries >= maxRetries) {
        Serial.println("MQTT: Maximum retry count reached. It will retry on the next loop cycle.");
    }
}


void callback(char* topic, byte* payload, unsigned int length) {
    // Convert the received message to String
    String message = "";
    for (int i = 0; i < length; i++) message += (char)payload[i];

    // Create the dynamic prefix to publish state to the correct topic
    String technicalID = "retropixel_" + chipID;
    String stateTopicPrefix = "retropixel/" + technicalID + "/state/";
    String strTopic = String(topic);

    Serial.println("MQTT received [" + strTopic + "]: " + message);

    // ----------------------------------------------------
    // 1. MODE CONTROL (GIFs, Text, Arcade)
    // ----------------------------------------------------
    if (strTopic.endsWith("/cmd/mode")) {
        int previousMode = config.playMode; // Store the previous mode

        if (message == "GIFs")      config.playMode = 0;
        else if (message == "Text") config.playMode = 1;
        else if (message == "Arcade") config.playMode = 3;

        if (config.playMode == 0) {
            gif.close(); 
            
            // CRITICAL: if the GIF list is empty, it must be filled
            // or GIF mode will enter and exit without drawing anything (black screen).
            if (!hasGifsInCache) {
                Serial.println("MQTT: Empty list, scanning SD...");
                listGifFiles(); 
            }

            // Make sure the index is valid after scanning
            gifCachePosition = 0;        
            Serial.println("MQTT: GIF mode enabled.");
        }

        display->fillScreen(0);
        savePlaybackConfig();
        mqttClient.publish((stateTopicPrefix + "mode").c_str(), message.c_str(), true);
        Serial.println("MQTT: Mode changed and reset to: " + message);
       
    }
    // ----------------------------------------------------
    // 2. POWER ON/OFF CONTROL
    // ----------------------------------------------------
    else if (strTopic.endsWith("/cmd/power")) {
        config.powerState = (message == "ON");
        if (!config.powerState && display) display->fillScreen(0);
        mqttClient.publish((stateTopicPrefix + "power").c_str(), message.c_str(), true);
        savePlaybackConfig();
    }
    // ----------------------------------------------------
    // 3. BRIGHTNESS CONTROL (0 - 255)
    // ----------------------------------------------------
    else if (strTopic.endsWith("/cmd/bright")) {
        config.brightness = constrain(message.toInt(), 0, 255);
        if (display) display->setBrightness8(config.brightness);
        mqttClient.publish((stateTopicPrefix + "bright").c_str(), String(config.brightness).c_str(), true);
        savePlaybackConfig();
    }
    // ----------------------------------------------------
    // 4. CUSTOM TEXT CONTROL
    // ----------------------------------------------------
    else if (strTopic.endsWith("/cmd/text")) {
        config.slidingText = message;
        marqueeXPos = display->width();
        savePlaybackConfig();
        mqttClient.publish((stateTopicPrefix + "text").c_str(), message.c_str(), true);
        Serial.println("New MQTT text (slidingText): " + message);
    }
    // ----------------------------------------------------
    // 5. TEXT COLOR CONTROL (RGB WHEEL)
    // ----------------------------------------------------
    if (strTopic.endsWith("/cmd/text_color/set")) {
        int r, g, b;
        if (sscanf(message.c_str(), "%d,%d,%d", &r, &g, &b) == 3) {
            config.slidingTextColor = (uint32_t)((r << 16) | (g << 8) | b);
            mqttClient.publish((stateTopicPrefix + "text_color/set").c_str(), message.c_str(), true);
            saveSystemConfig();
        }
    }
}

void syncMQTTState() {
    // 1. Initial check: if MQTT is not enabled or connected, exit
    if (!config.mqtt_enabled || !mqttClient.connected()) return;

    // 2. Define the variables needed to build topics
    String technicalID = "retropixel_" + chipID;
    String stateTopicPrefix = "retropixel/" + technicalID + "/state/";

    // 3. Current mode mapping
    String modeText;
    switch (config.playMode) {
        case 0:  modeText = "GIFs";   break;
        case 1:  modeText = "Text";  break;
        case 3:  modeText = "Arcade"; break;
        default: modeText = "GIFs";   break;
    }

    // 4. Helper function to convert color to R,G,B format for Home Assistant
    auto toRGBStr = [](uint32_t c) {
        return String((c >> 16) & 0xFF) + "," + String((c >> 8) & 0xFF) + "," + String(c & 0xFF);
    };

    // 5. Send basic states
    mqttClient.publish((stateTopicPrefix + "mode").c_str(), modeText.c_str(), true);
    mqttClient.publish((stateTopicPrefix + "bright").c_str(), String(config.brightness).c_str(), true);
    mqttClient.publish((stateTopicPrefix + "power").c_str(), (config.powerState ? "ON" : "OFF"), true);
    mqttClient.publish((stateTopicPrefix + "text").c_str(), config.slidingText.c_str(), true);

    // 6. Synchronize color wheel (light in HA)
    mqttClient.publish((stateTopicPrefix + "text_color").c_str(), "ON", true);
    mqttClient.publish((stateTopicPrefix + "text_color/set").c_str(), toRGBStr(config.slidingTextColor).c_str(), true);
    
    Serial.println("MQTT: State synchronization sent to HA (Mode: " + modeText + ")");
}

// ====================================================================
//                              FTP SERVER
// ====================================================================

String ftpNormalizePath(String path) {
    path.replace("\\", "/");
    if (!path.startsWith("/")) path = ftpCurrentDir + "/" + path;

    std::vector<String> parts;
    int start = 0;
    while (start < path.length()) {
        int slash = path.indexOf('/', start);
        String part = slash == -1 ? path.substring(start) : path.substring(start, slash);
        part.trim();
        if (part.length() > 0 && part != ".") {
            if (part == "..") {
                if (!parts.empty()) parts.pop_back();
            } else {
                parts.push_back(part);
            }
        }
        if (slash == -1) break;
        start = slash + 1;
    }

    String normalized = "/";
    for (size_t i = 0; i < parts.size(); i++) {
        normalized += parts[i];
        if (i < parts.size() - 1) normalized += "/";
    }
    return normalized;
}

String ftpResolvePath(String path) {
    path.trim();
    if (path == "" || path == ".") return ftpCurrentDir;
    return ftpNormalizePath(path);
}

void sendFtpResponse(const String& response) {
    if (ftpClient && ftpClient.connected()) {
        ftpClient.print(response);
        ftpClient.print("\r\n");
    }
}

void beginFtpServer() {
    if (ftpServerRunning || !config.ftp_enabled) return;
    ftpServer.begin();
    ftpDataServer.begin();
    ftpServerRunning = true;
    Serial.println("FTP server started on port 21.");
}

void stopFtpServer() {
    if (ftpClient) ftpClient.stop();
    if (ftpDataClient) ftpDataClient.stop();
    ftpLoggedIn = false;
    ftpUserOk = false;
    ftpServerRunning = false;
    inFileManagerMode = false;
    Serial.println("FTP server stopped.");
}

bool ftpOpenDataConnection() {
    unsigned long start = millis();
    while (millis() - start < 8000) {
        ftpDataClient = ftpDataServer.available();
        if (ftpDataClient && ftpDataClient.connected()) return true;
        server.handleClient();
        delay(10);
    }
    sendFtpResponse("425 Can't open data connection.");
    return false;
}

void ftpCloseDataConnection() {
    if (ftpDataClient) {
        ftpDataClient.flush();
        ftpDataClient.stop();
    }
}

void ftpSendList(String path, bool namesOnly) {
    path = ftpResolvePath(path);
    sendFtpResponse("150 Opening data connection.");
    if (!ftpOpenDataConnection()) return;

    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(8000))) {
        File dir = SD.open(path);
        if (dir && dir.isDirectory()) {
            File entry = dir.openNextFile();
            while (entry) {
                String name = String(entry.name());
                int slash = name.lastIndexOf('/');
                if (slash >= 0) name = name.substring(slash + 1);
                if (namesOnly) {
                    ftpDataClient.print(name + "\r\n");
                } else {
                    ftpDataClient.print(entry.isDirectory() ? "drwxr-xr-x 1 owner group " : "-rw-r--r-- 1 owner group ");
                    ftpDataClient.print(String(entry.size()));
                    ftpDataClient.print(" Jan 01 00:00 ");
                    ftpDataClient.print(name);
                    ftpDataClient.print("\r\n");
                }
                entry.close();
                entry = dir.openNextFile();
            }
            dir.close();
        }
        xSemaphoreGive(sdMutex);
    }

    ftpCloseDataConnection();
    sendFtpResponse("226 Transfer complete.");
}

void ftpStoreFile(String path) {
    path = ftpResolvePath(path);
    sendFtpResponse("150 Opening data connection.");
    if (!ftpOpenDataConnection()) return;

    inFileManagerMode = true;
    interruptPlayback = true;
    gif.close();

    bool ok = false;
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(8000))) {
        if (SD.exists(path)) SD.remove(path);
        File out = SD.open(path, FILE_WRITE);
        if (out) {
            uint8_t buffer[1024];
            unsigned long lastData = millis();
            while (ftpDataClient.connected() || ftpDataClient.available()) {
                int available = ftpDataClient.available();
                if (available > 0) {
                    int len = ftpDataClient.read(buffer, min(available, (int)sizeof(buffer)));
                    if (len > 0) {
                        out.write(buffer, len);
                        lastData = millis();
                    }
                } else if (millis() - lastData > 2000) {
                    break;
                } else {
                    delay(1);
                }
            }
            out.close();
            ok = true;
            pendingGifReload = true;
        }
        xSemaphoreGive(sdMutex);
    }

    ftpCloseDataConnection();
    interruptPlayback = false;
    sendFtpResponse(ok ? "226 Transfer complete." : "451 Local write error.");
}

void ftpRetrieveFile(String path) {
    path = ftpResolvePath(path);
    sendFtpResponse("150 Opening data connection.");
    if (!ftpOpenDataConnection()) return;

    bool ok = false;
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(8000))) {
        File in = SD.open(path, FILE_READ);
        if (in && !in.isDirectory()) {
            uint8_t buffer[1024];
            while (in.available() && ftpDataClient.connected()) {
                size_t len = in.read(buffer, sizeof(buffer));
                if (len > 0) ftpDataClient.write(buffer, len);
                delay(1);
            }
            ok = true;
        }
        if (in) in.close();
        xSemaphoreGive(sdMutex);
    }

    ftpCloseDataConnection();
    sendFtpResponse(ok ? "226 Transfer complete." : "550 File unavailable.");
}

void handleFtpServer() {
    if (!config.ftp_enabled) {
        if (ftpServerRunning) stopFtpServer();
        return;
    }
    if (!ftpServerRunning) beginFtpServer();

    if (!ftpClient || !ftpClient.connected()) {
        WiFiClient newClient = ftpServer.available();
        if (newClient) {
            if (ftpClient) ftpClient.stop();
            ftpClient = newClient;
            ftpLoggedIn = false;
            ftpUserOk = false;
            ftpCurrentDir = "/";
            ftpCommandLine = "";
            sendFtpResponse("220 Retro Pixel LED FTP ready.");
        }
        return;
    }

    while (ftpClient.available()) {
        char c = ftpClient.read();
        if (c == '\r') continue;
        if (c != '\n') {
            if (ftpCommandLine.length() < 256) ftpCommandLine += c;
            continue;
        }

        String line = ftpCommandLine;
        ftpCommandLine = "";
        line.trim();
        if (line == "") continue;

        int space = line.indexOf(' ');
        String command = space == -1 ? line : line.substring(0, space);
        String arg = space == -1 ? "" : line.substring(space + 1);
        command.toUpperCase();
        arg.trim();

        if (command == "USER") {
            ftpUserOk = (arg == String(config.ftp_user));
            sendFtpResponse(ftpUserOk ? "331 Password required." : "530 Invalid user.");
        } else if (command == "PASS") {
            ftpLoggedIn = ftpUserOk && (arg == String(config.ftp_pass));
            sendFtpResponse(ftpLoggedIn ? "230 Login successful." : "530 Login incorrect.");
        } else if (command == "QUIT") {
            sendFtpResponse("221 Goodbye.");
            ftpClient.stop();
        } else if (!ftpLoggedIn) {
            sendFtpResponse("530 Please login with USER and PASS.");
        } else if (command == "SYST") {
            sendFtpResponse("215 UNIX Type: L8");
        } else if (command == "FEAT") {
            sendFtpResponse("211-Features\r\n PASV\r\n SIZE\r\n211 End");
        } else if (command == "TYPE") {
            sendFtpResponse("200 Type set.");
        } else if (command == "MODE" || command == "STRU" || command == "OPTS" || command == "NOOP") {
            sendFtpResponse("200 OK.");
        } else if (command == "PWD" || command == "XPWD") {
            sendFtpResponse("257 \"" + ftpCurrentDir + "\"");
        } else if (command == "CWD" || command == "XCWD") {
            String nextDir = ftpResolvePath(arg);
            if (SD.exists(nextDir)) {
                ftpCurrentDir = nextDir;
                sendFtpResponse("250 Directory changed.");
            } else {
                sendFtpResponse("550 Directory unavailable.");
            }
        } else if (command == "CDUP") {
            ftpCurrentDir = ftpNormalizePath(ftpCurrentDir + "/..");
            sendFtpResponse("250 Directory changed.");
        } else if (command == "PASV") {
            IPAddress ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP();
            uint8_t p1 = FTP_PASSIVE_PORT / 256;
            uint8_t p2 = FTP_PASSIVE_PORT % 256;
            sendFtpResponse("227 Entering Passive Mode (" + String(ip[0]) + "," + String(ip[1]) + "," + String(ip[2]) + "," + String(ip[3]) + "," + String(p1) + "," + String(p2) + ").");
        } else if (command == "LIST") {
            ftpSendList(arg, false);
        } else if (command == "NLST") {
            ftpSendList(arg, true);
        } else if (command == "STOR") {
            ftpStoreFile(arg);
        } else if (command == "RETR") {
            ftpRetrieveFile(arg);
        } else if (command == "DELE") {
            String path = ftpResolvePath(arg);
            bool ok = false;
            if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000))) {
                ok = SD.remove(path);
                if (ok) pendingGifReload = true;
                xSemaphoreGive(sdMutex);
            }
            sendFtpResponse(ok ? "250 File deleted." : "550 Delete failed.");
        } else if (command == "MKD" || command == "XMKD") {
            String path = ftpResolvePath(arg);
            bool ok = false;
            if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000))) {
                ok = SD.mkdir(path);
                xSemaphoreGive(sdMutex);
            }
            sendFtpResponse(ok ? "257 Directory created." : "550 Create directory failed.");
        } else if (command == "RMD" || command == "XRMD") {
            String path = ftpResolvePath(arg);
            bool ok = false;
            if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000))) {
                ok = SD.rmdir(path);
                xSemaphoreGive(sdMutex);
            }
            sendFtpResponse(ok ? "250 Directory removed." : "550 Remove directory failed.");
        } else if (command == "RNFR") {
            ftpRenameFrom = ftpResolvePath(arg);
            sendFtpResponse(SD.exists(ftpRenameFrom) ? "350 Ready for RNTO." : "550 File unavailable.");
        } else if (command == "RNTO") {
            String to = ftpResolvePath(arg);
            bool ok = false;
            if (ftpRenameFrom != "" && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000))) {
                ok = SD.rename(ftpRenameFrom, to);
                if (ok) pendingGifReload = true;
                xSemaphoreGive(sdMutex);
            }
            ftpRenameFrom = "";
            sendFtpResponse(ok ? "250 Rename successful." : "550 Rename failed.");
        } else if (command == "SIZE") {
            String path = ftpResolvePath(arg);
            File f = SD.open(path, FILE_READ);
            if (f && !f.isDirectory()) sendFtpResponse("213 " + String(f.size()));
            else sendFtpResponse("550 File unavailable.");
            if (f) f.close();
        } else {
            sendFtpResponse("502 Command not implemented.");
        }
    }
}

// --- DUAL CORE TASKS ---
void TaskDisplay(void * pvParameters);

// ====================================================================
//                             SETUP AND LOOP
// ====================================================================

void setup() {
    Serial.begin(115200);

    // --- 1. GET UNIQUE ID (UNIVERSAL CORE VERSION 3.X) ---
    // Read the MAC directly from the hardware eFuses (64 bits)
    uint64_t chipid_raw = ESP.getEfuseMac(); 
    
    // Extract the last 3 bytes (the last 6 hexadecimal characters)
    // Shift 24 bits to get the final part of the MAC
    uint32_t chipid_num = (uint32_t)(chipid_raw >> 24); 

    char mac_str[7];
    sprintf(mac_str, "%06X", chipid_num); // Convert to 6-digit hexadecimal text
    chipID = String(mac_str);
    chipID.toUpperCase();
    
    Serial.print(">> Unique device ID calculated at startup: ");
    Serial.println(chipID);

    // --- 2. SEMAPHORE (MUTEX) ---
    sdMutex = xSemaphoreCreateMutex();
    if (sdMutex == NULL) {
        Serial.println("Error creating semaphore");
    }
       
    if (!SPIFFS.begin(true)) {
        Serial.println("Error mounting SPIFFS.");
    }

    loadConfig();

    // --- 3. SD INITIALIZATION --- 
    SPI.begin(VSPI_SCLK, VSPI_MISO, VSPI_MOSI, SD_CS_PIN);
    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("Error mounting SD card!");
        sdMounted = false;
        delay(100);
    } else {
        Serial.println("SD card mounted successfully.");
        sdMounted = true;
        scanFolders(GIFS_BASE_PATH);
    }   

    gif.begin(LITTLE_ENDIAN_PIXELS);

    // --- 4. WIFI CONNECTION --- 
    if (config.device_name[0] == '\0') {
        strncpy(config.device_name, DEVICE_NAME_DEFAULT, sizeof(config.device_name) - 1);
        config.device_name[sizeof(config.device_name) - 1] = '\0'; 
    }

    wm.setHostname(config.device_name);

    // WM CONFIGURATION: make the portal non-blocking
    wm.setConfigPortalBlocking(false); 
    //wm.setConfigPortalTimeout(180); // Wait 3 minutes, then continue

    bool connectedFromSd = false;
    if (!config.WifiOffMode) {
        connectedFromSd = connectFromSdWifiConfig();
    }

    if (connectedFromSd) {
        Serial.println("WiFiManager portal skipped because SD WiFi config succeeded.");
    } else if (config.WifiOffMode) {
        // SCENARIO A: Offline mode configured
        Serial.println("Offline mode enabled. Creating local network...");
        WiFi.mode(WIFI_AP);
        WiFi.softAP("Retro Pixel LED");
        config.slidingText = "Offline Mode Active - Connect to IP: 192.168.4.1 to use Retro Pixel LED";
    } else {
        // SCENARIO B: Online attempt
        WiFi.mode(WIFI_AP_STA); 
        config.slidingText = "ONLINE MODE - Searching WiFi...";

        // Configure the timeout to 3 minutes (180 sec)
        wm.setConfigPortalTimeout(180);
        wm.setConfigPortalBlocking(false);

        // Try to connect
        if (!wm.autoConnect("Retro Pixel LED")) {
            Serial.println("Portal active. Waiting for configuration or timeout...");
            config.slidingText = "Connect to the Retro Pixel LED WiFi network to configure it - IP:192.168.4.1 - If no WiFi network is configured within 3 minutes, the AP will be disabled. Restart Retro Pixel LED to enable the WiFi configuration AP again";
        } else {
            // If it connects on the first try:
            Serial.println("Connected successfully.");
            config.slidingText = "Retro Pixel LED v" + String(FIRMWARE_VERSION) + " - IP: " + WiFi.localIP().toString();
        }
    }

    // --- 5. LED MATRIX INITIALIZATION --- 
    const int FINAL_MATRIX_WIDTH = PANEL_RES_X * config.panelChain;

    HUB75_I2S_CFG::i2s_pins pin_config = {
        R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN,
        A_PIN, B_PIN, C_PIN, D_PIN, E_PIN,
        LAT_PIN, OE_PIN, CLK_PIN
    };

    HUB75_I2S_CFG matrix_config(
        FINAL_MATRIX_WIDTH, 
        MATRIX_HEIGHT,      
        config.panelChain,  
        pin_config          
    );

    // --- 6. APPLY ADVANCED SETTINGS  ---
    
    // 6.1. Speed I2S (Map index 0-3 to library constants)
    if (config.i2sSpeed == 0)      matrix_config.i2sspeed = HUB75_I2S_CFG::HZ_8M;
    else if (config.i2sSpeed == 1) matrix_config.i2sspeed = HUB75_I2S_CFG::HZ_10M;
    else if (config.i2sSpeed == 2) matrix_config.i2sspeed = HUB75_I2S_CFG::HZ_16M;
    else if (config.i2sSpeed == 3) matrix_config.i2sspeed = HUB75_I2S_CFG::HZ_20M;
    else matrix_config.i2sspeed = HUB75_I2S_CFG::HZ_10M;

    // 6.2. Latch Blanking (Anti-Ghosting)
    // Valid range 1-4.
    matrix_config.latch_blanking = config.latchBlanking;

    // 6.3. Minimum Refresh Rate
    // If 0 is received by mistake, force 60 Hz.
    if (config.minRefreshRate < 30) config.minRefreshRate = 60;
    matrix_config.min_refresh_rate = config.minRefreshRate;

    // 6.4. Double Buffer (always active for GIFs)
    //matrix_config.double_buff = true;

    // 6.5. Synchronization
    matrix_config.clkphase = false;

    // Create the Display object with the new configuration
    display = new MatrixPanel_I2S_DMA(matrix_config);

    if (display) { 
        display->begin();
        display->setBrightness8(config.brightness);
        if (!config.powerState) {
            display->fillScreen(0);
        } else {
            display->fillScreen(display->color565(0, 0, 0));
        } 

        if (!sdMounted) {
            showMessage("SD Error!", display->color565(255, 0, 0));
        } else if (WiFi.status() == WL_CONNECTED) {
            showMessage("WiFi OK!", display->color565(0, 255, 0));
        } else {
             showMessage("AP Mode", display->color565(255, 255, 0));
        }
        
        if (config.playMode == 0) {
            // Enable the flag so loop() handles it in the background.
            pendingGifReload = true; 
            Serial.println("GIF loading scheduled...");
        }
        delay(1000);
    } else {
        Serial.println("ERROR: Could not allocate memory for the LED matrix.");
    }
    
    // --- 7. WEB SERVER ROUTE CONFIGURATION ---
    server.on("/", HTTP_GET, handleRoot);
    server.on("/power", HTTP_GET, handlePower);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/config", HTTP_GET, handleConfig); 
    server.on("/save_config", HTTP_POST, handleSaveConfig); 
    server.on("/restart", HTTP_GET, handleRestart); 
    server.on("/factory_reset", HTTP_GET, handleFactoryReset);

    // CSS STYLE
    server.on("/style.css", HTTP_GET, [](){
        server.sendHeader("Cache-Control", "max-age=86400"); // 1-day cache
        server.setContentLength(CONTENT_LENGTH_UNKNOWN);     // Announce chunked sending
        server.send(200, "text/css", "");                    // Initial header
        server.sendContent(getStyle());                      // Send the CSS block
        server.sendContent("");                              // Close sending
    });
    
    // FILE MANAGEMENT ROUTES
    server.on("/file_manager", HTTP_GET, handleFileManager);
    server.on("/delete", HTTP_POST, handleFileDelete);
    server.on("/create_dir", HTTP_POST, handleCreateDir); 

    // BATOCERA ROUTE ---
    server.on("/batocera", HTTP_GET, []() {
    if (server.hasArg("s") && server.hasArg("g")) {
        // A. LOCK: pause to release the SD
        interruptPlayback = true; 
        delay(150); 

        String s = server.arg("s");
        String g = server.arg("g");
        String t = server.hasArg("t") ? server.arg("t") : "";
        s.trim(); g.trim(); t.trim();
        if (t.length() > 100) t = t.substring(0, 100);

        if (g == "OFF") {
            // --- POWER-OFF EVENT ---
            Serial.println(">>> BATOCERA OFF: Returning to GIF mode");
            config.playMode = 0; // Switch to GIF mode
        } else if (g == "STOP" || g == "") {
            // --- GAME EXIT EVENT ---
            Serial.println(">>> GAME-STOP: Loading default logo");
            arcadeGifPath = "/batocera/default/_default.gif";
            config.playMode = 3; // Keep Arcade Mode to show the logo
        } else {
            // --- GAME START EVENT ---
            Serial.printf(">>> GAME -> System: %s | Game: %s\n", s.c_str(), g.c_str());
            String gameGifPath = runSearch(s, g, "00");

            if (gameGifPath != "") {
                arcadeGifPath = selectRandomVariant(gameGifPath);
                config.playMode = 3;
                Serial.print(">>> GAME MATCH: "); Serial.println(arcadeGifPath);
            } else if (t != "") {
                Serial.printf(">>> GAME GIF MISSING: scrolling text fallback [%s]\n", t.c_str());
                config.slidingText = t;
                if (display) marqueeXPos = display->width();
                config.playMode = 1;
            } else {
                arcadeGifPath = searchCache(s, g);
                config.playMode = 3; // Keep Arcade Mode to show the fallback logo
            }
        }

        // B. RESUME
        interruptPlayback = false; 
        server.send(200, "text/plain", "OK");
        } else {
        server.send(400, "text/plain", "Missing arguments");
        }
    });

    // REDIRECT AFTER UPLOAD
    server.on("/upload", HTTP_POST, [](){ 
        // After upload finishes, redirect to the manager so the panel stays in "FILES MODE"
        server.sendHeader("Location", "/file_manager?path=" + currentPath);
        server.send(303); 
    }, handleFileUpload);
    
    // OTA ROUTES
    server.on("/ota", HTTP_GET, handleOTA);
    server.on("/ota_upload", HTTP_POST, [](){ 
        server.sendHeader("Connection", "close");
        server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        ESP.restart(); 
    }, handleOTAUpload);
    
    server.onNotFound(notFound);

    server.begin();
    if (config.ftp_enabled) beginFtpServer();

    // --- 7. MQTT ---
    if (config.mqtt_enabled) {
        mqttClient.setServer(config.mqtt_host, config.mqtt_port);
        mqttClient.setBufferSize(1024);
        mqttClient.setCallback(callback);
        Serial.println("MQTT Configured.");
    }

    // Launch the panel task on Core 1
    xTaskCreatePinnedToCore(
        TaskDisplay,   
        "TaskDisplay", 
        16384,          
        NULL,          
        2,             
        &displayTaskHandle,          
        1              
    );
    
    Serial.println("HTTP server started.");
}


bool temporaryModeActive = false;

void loop() {
    // 1. If the WiFiManager portal is running
    if (wm.getConfigPortalActive()) {
        wm.process();
        temporaryModeActive = false; // Reset flag if the portal is active
    } 
    else {
        // 2. Smart disconnection handling (when WiFi fails or the AP expires)
        if (WiFi.status() != WL_CONNECTED && !config.WifiOffMode && !temporaryModeActive) {
            
            Serial.println(">> WiFi not detected. Waiting 3s for safety...");
            delay(3000); 

            if (!config.WifiOffMode) { 
                Serial.println(">> Entering temporary offline mode.");
                config.WifiOffMode = true; 
                temporaryModeActive = true;
				config.slidingText = "The configuration AP was disabled because the WiFi network was not configured. Restart me if you want to configure WiFi.";

                // Force AP_STA mode so even if the portal closes, the web server keeps listening on the AP IP or last network IP.
                WiFi.mode(WIFI_AP_STA);
            }
        }

        // 3. Process web server
        server.handleClient();
        handleFtpServer();

        // 3.1 Automatic reconnection retry (background)
        // If temporary mode is active because the router was off, retry every 60 s
        static unsigned long lastWiFiTest = 0;
        if (temporaryModeActive && (millis() - lastWiFiTest > 60000)) {
            lastWiFiTest = millis();
            Serial.println(">> Retrying WiFi connection...");
            WiFi.begin(); // Try to connect with already-known credentials
        }


        // 3.2 Successful connection handling
        if (WiFi.status() == WL_CONNECTED) {
            // If WiFi is recovered, disable temporary mode and the lock
            if (temporaryModeActive) {
                Serial.println(">> WiFi recovered successfully.");
                temporaryModeActive = false;
            }
        
            if (config.mqtt_enabled) {
                // 1. If not connected, try to connect
                if (!mqttClient.connected()) {
                    reconnectMQTT(); 
                }
                // 2. Keep the listener loop running
                mqttClient.loop();
            }
            
        }
    }

    // 4. GIF management
    if (pendingGifReload) {
        delay(200); 
        listGifFiles();
        pendingGifReload = false; 
    }
}

// --- TASK FOR CORE 1 (LED PANEL) ---
void TaskDisplay(void * pvParameters) {
    for (;;) {
        // 1. Check whether GIFs must be listed
        // Put this first so it can interrupt any configuration change
        if (pendingGifReload) {
            if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500))) {
                listGifFiles(); // This function now clears the screen and shows "LISTING..."
                pendingGifReload = false;
                xSemaphoreGive(sdMutex);
            }
            // Do not continue here so GIF mode can run immediately after
        }

        // 2. If the panel is off (Dynamic Energy Saving)
        if (config.powerState == false) {
            display->fillScreen(0);

            // If the frequency is not 80 MHz, lower it
            if (getCpuFrequencyMhz() != 80) {
                setCpuFrequencyMhz(80);
                Serial.println(F("[POWER] Power saving mode: CPU at 80MHz (WiFi Active)"));
            }

            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        } else {
            // When turning on, restore 240 MHz
            if (getCpuFrequencyMhz() != 240) {
                setCpuFrequencyMhz(240);
                Serial.println(F("[POWER] Performance mode: CPU at 240MHz"));
            }

        }

        // 3. Safety block: file manager or silent interruption
        // Stop GIF playback and show the "FILES MODE" message
        if (inFileManagerMode) {
            display->fillScreen(0);
            display->setTextColor(display->color565(0, 242, 255)); // Cyberpunk cyan
            display->setCursor(177, 7);
            display->print("FILES");
            display->setCursor(180, 17);
            display->print("MODE");
            
            // Draw a small progress line or Cyberpunk accent
            display->drawFastHLine(160, 27, 64, display->color565(255, 0, 100)); 
            
            vTaskDelay(pdMS_TO_TICKS(500)); // Wait half a second before checking again
            continue; // Skip the rest of the code (GIFs do not run)
        }

        // Silent interruption for fast changes (Arcade)
        if (interruptPlayback) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // 4. Normal execution with semaphore
        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100))) {
            if (display) { 
        
                switch (config.playMode) {
                    case 0: runGifMode(); break;
                    case 1: runTextMode(); break;
                    case 3: runArcadeMode(); break;
                    default:
                        config.playMode = 0;
                        runGifMode();
                        break;
                }
            }
            xSemaphoreGive(sdMutex);
        }

        // A small 1 ms pause to keep the system watchdog happy
        vTaskDelay(pdMS_TO_TICKS(1)); 
    }
}

#include <Ambient.h>
#include <M5Stack.h>
#include <Preferences.h>
#include <SparkFun_SCD30_Arduino_Library.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>

#define USE_EXTERNAL_SD_FOR_CONFIG
// comment out USE_EXTERNAL_SD_FOR_CONFIG if you write config directly, and define config you use
// #define USE_AMBIENT  // comment out if you don't use Ambient for logging
// #define USE_PUSHBULLET  // comment out if you don't use PushBullet for notifying

// WiFi setting
#if defined(USE_AMBIENT) || defined(USE_PUSHBULLET)
bool use_wifi = true;
const char *ssid{"Write your ssid"};          // write your WiFi SSID (2.4GHz)
const char *password{"Write your password"};  // write your WiFi password
#else
bool use_wifi = false;
const char *ssid;      // dummy
const char *password;  // dummy
#endif

// Ambient setting
WiFiClient client;
Ambient ambient;
#ifdef USE_AMBIENT
bool use_ambient = true;
unsigned int channelId{99999};                         // write your Ambient channel ID
const char *writeKey{"Write your Ambient write key"};  // write your Ambient write key
#else
bool use_ambient = false;
unsigned int channelId;  // dummy
const char *writeKey;    // dummy
#endif

// Pushbullet setting
WiFiClientSecure secureClient;
int notify_timer_caution = 0;
int notify_timer_warning = 0;
#define NOTIFY_TIMER_S 180  // send notification when CO2 keeps exceeding threshold this second
#ifdef USE_PUSHBULLET
bool use_pushbullet = true;
String pushbullet_apikey("Write your pushbullet api key");
#else
bool use_pushbullet = false;
String pushbullet_apikey;
#endif

// sensor setting
SCD30 airSensor;
#define SENSOR_INTERVAL_S 2    // get sensor value every SENSOR_INTERVAL_S [s]
#define UPLOAD_INTERVAL_S 60   // upload data to Ambient every UPLOAD_INTERVAL_S [s]
#define BTNCHK_INTERVAL_S 0.1  // M5Stack Button check interval [s]

// TFT setting
#define MYTFT_WIDTH 320
#define MYTFT_HEIGHT 240
#define SPRITE_WIDTH 320
#define SPRITE_HEIGHT 160

// CO2 level default setting
#define CO2_MAX_RANGE 40000
#define CO2_MIN_PPM 0
#define CO2_MAX_PPM_DEFAULT 2500
#define CO2_CAUTION_PPM_DEFAULT 1000
#define CO2_WARNING_PPM_DEFAULT 2000
#define TEMPERATURE_OFFSET_MAX 999
#define TEMPERATURE_OFFSET_MIN 0
#define CO2_HYSTERESIS_PPM 100
#define CO2_SET_DELTA 100
#define TEMPERATURE_SET_DELTA 1
#define CO2_CALIBRATION_FACTOR 400  // outside fresh air
int co2_max_ppm;
int co2_caution_ppm;
int co2_warning_ppm;
int temperature_offsetx10;

// Preferences define
Preferences preferences;
#define PREF_NAMESPACE "co2-monitor"
#define KEY_CO2_MAX "max"
#define KEY_CO2_WARNING "war"
#define KEY_CO2_CAUTION "cau"

enum {
    LEVEL_NORMAL,
    LEVEL_CAUTION,
    LEVEL_WARNING
};

// variables
TFT_eSprite graph_co2 = TFT_eSprite(&M5.Lcd);
TFT_eSprite spr_values = TFT_eSprite(&M5.Lcd);
uint16_t co2_ppm;
float temperature_c, humidity_p;
int p_cau, p_war;
int elapsed_time = 0;
int co2_level_last = LEVEL_NORMAL;

// reference: https://fipsok.de/Esp32-Webserver/push-Esp32-tab
bool pushbullet(const String &message) {
    const uint16_t timeout{5000};
    const char *HOST{"api.pushbullet.com"};
    String messagebody = R"({"type": "note", "title": "CO2 Monitor", "body": ")" + message + R"("})";
    uint32_t broadcastingTime{millis()};
    if (!secureClient.connect(HOST, 443)) {
        Serial.println("Pushbullet connection failed!");
        return false;
    } else {
        secureClient.printf("POST /v2/pushes HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s\r\n", HOST, pushbullet_apikey.c_str(), messagebody.length(), messagebody.c_str());
        Serial.println("Push sent");
    }
    while (!secureClient.available()) {
        if (millis() - broadcastingTime > timeout) {
            Serial.println("Pushbullet Client Timeout !");
            secureClient.stop();
            return false;
        }
    }
    while (secureClient.available()) {
        String line = secureClient.readStringUntil('\n');
        if (line.startsWith("HTTP/1.1 200 OK")) {
            secureClient.stop();
            return true;
        }
    }
    return false;
}

int getPositionY(int ppm) {
    return SPRITE_HEIGHT - (int32_t)((float)SPRITE_HEIGHT / (co2_max_ppm - CO2_MIN_PPM) * ppm);
}

bool notifyUser(int level) {
    char body[100];

    switch (level) {
        case LEVEL_CAUTION:
            sprintf(body, "CO2 exceeded %d ppm. Ventilate please.", co2_caution_ppm);
            return pushbullet(body);

        case LEVEL_WARNING:
            sprintf(body, "CO2 exceeded %d ppm. Ventilate immediately.", co2_warning_ppm);
            return pushbullet(body);

        default:
            return false;
    }
}

uint16_t getColor(uint8_t red, uint8_t green, uint8_t blue) {
    return ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
}

void setup_with_external_SD() {
    String config_ini;
    String ssid;
    String password;
    String ambient_ch_id;
    String ambient_writekey;

    M5.Lcd.print("Open config...");
    File datfile = SD.open("/config.ini");
    if (datfile) {
        M5.Lcd.println("OK");
        while (datfile.available()) {
            config_ini = config_ini + datfile.readString();
        }
        datfile.close();
    } else {
        M5.Lcd.println("not found");
        return;
    }

    // SSID
    M5.Lcd.print("WiFi setup...");
    int ssid_index = config_ini.indexOf("#WIFI_SSID\r\n");
    if (ssid_index != -1) {
        use_wifi = true;
        config_ini = config_ini.substring(config_ini.indexOf("#WIFI_SSID\r\n") + 12);
        ssid = config_ini.substring(0, config_ini.indexOf("\r\n"));
        Serial.printf("SSID:%s\n", ssid.c_str());
        config_ini = config_ini.substring(config_ini.indexOf("#SSID_PASS\r\n") + 12);
        password = config_ini.substring(0, config_ini.indexOf("\r\n"));
        Serial.printf("Password:%s\n", password.c_str());
        // connect to Wifi
        WiFi.begin(ssid.c_str(), password.c_str());
        while (WiFi.status() != WL_CONNECTED) {
            delay(500);
            Serial.print(".");
        }
        M5.Lcd.println("OK");
        Serial.println("WiFi connected");
        Serial.println("IP address: ");
        Serial.println(WiFi.localIP());
    } else {
        M5.Lcd.println("Skip");
    }

    // Ambient
    M5.Lcd.print("Ambient setup...");
    int ambient_index = config_ini.indexOf("#AMBIENT_CH_ID\r\n");
    if (ambient_index != -1) {
        use_ambient = true;
        config_ini = config_ini.substring(config_ini.indexOf("#AMBIENT_CH_ID\r\n") + 16);
        ambient_ch_id = config_ini.substring(0, config_ini.indexOf("\r\n"));
        Serial.printf("Ambient ch ID:%ld\n", ambient_ch_id.toInt());
        config_ini = config_ini.substring(config_ini.indexOf("#AMBIENT_WRITEKEY\r\n") + 19);
        ambient_writekey = config_ini.substring(0, config_ini.indexOf("\r\n"));
        Serial.printf("Ambient write key:%s\n", ambient_writekey.c_str());
        ambient.begin(ambient_ch_id.toInt(), ambient_writekey.c_str(), &client);
        M5.Lcd.println("OK");
    } else {
        M5.Lcd.println("Skip");
    }

    // Pushbullet
    M5.Lcd.print("Pushbullet setup...");
    int pushbullet_index = config_ini.indexOf("#PUSHBULLET_APIKEY\r\n");
    if (pushbullet_index != -1) {
        use_pushbullet = true;
        config_ini = config_ini.substring(config_ini.indexOf("#PUSHBULLET_APIKEY\r\n") + 20);
        pushbullet_apikey = config_ini.substring(0, config_ini.indexOf("\r\n"));
        Serial.printf("Pushbullet api key:%s\n", pushbullet_apikey.c_str());
        M5.Lcd.println("OK");
    } else {
        M5.Lcd.println("Skip");
    }
}

void setup() {
    M5.begin();
    M5.Power.begin();
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextSize(2);
    Serial.begin(115200);

    // read preferences
    int v;
    preferences.begin(PREF_NAMESPACE, false);
    v = preferences.getInt(KEY_CO2_MAX, -1);
    if (v == -1)
        co2_max_ppm = CO2_MAX_PPM_DEFAULT;
    else
        co2_max_ppm = v;
    v = preferences.getInt(KEY_CO2_WARNING, -1);
    if (v == -1)
        co2_warning_ppm = CO2_WARNING_PPM_DEFAULT;
    else
        co2_warning_ppm = v;
    v = preferences.getInt(KEY_CO2_CAUTION, -1);
    if (v == -1)
        co2_caution_ppm = CO2_CAUTION_PPM_DEFAULT;
    else
        co2_caution_ppm = v;
    preferences.end();

#ifdef USE_EXTERNAL_SD_FOR_CONFIG
    M5.Lcd.println("Setup with external SD");
    SD.begin();
    setup_with_external_SD();
#else
    M5.Lcd.println("Setup without SD");
    // Wifi setup
    M5.Lcd.print("WiFi setup...");
    if (use_wifi) {
        WiFi.begin(ssid, password);
        while (WiFi.status() != WL_CONNECTED) {
            delay(500);
            Serial.print(".");
        }
        M5.Lcd.println("OK");
        Serial.println("WiFi connected");
        Serial.println("IP address: ");
        Serial.print(WiFi.localIP());
    } else {
        M5.Lcd.println("Skip");
    }

    // Ambient setup
    M5.Lcd.print("Ambient setup...");
    if (use_ambient) {
        ambient.begin(channelId, writeKey, &client);
        M5.Lcd.println("OK");
    } else {
        M5.Lcd.println("Skip");
    }

    // pushbullet setup
    M5.Lcd.print("Pushbullet setup...");
    if (use_pushbullet) {
        M5.Lcd.println("OK");
    } else {
        M5.Lcd.println("Skip");
    }
#endif

    // sensor setup
    M5.Lcd.print("SCD30 setup...");
    Wire.begin();
    if (airSensor.begin() == false) {
        M5.Lcd.println("fail!");
        Serial.println("Air sensor not detected. Please check wiring. Freezing...");
        while (1)
            ;
    }
    M5.Lcd.println("OK");

    Serial.println("Setup done");
    Serial.printf("use_wifi:%d\n", use_wifi);
    Serial.printf("use_ambient:%d\n", use_ambient);
    Serial.printf("use_pushbullet:%d\n", use_pushbullet);

    delay(3000);

    p_cau = getPositionY(co2_caution_ppm);
    p_war = getPositionY(co2_warning_ppm);
    M5.Lcd.fillScreen(TFT_BLACK);
    graph_co2.setColorDepth(8);
    graph_co2.createSprite(SPRITE_WIDTH, SPRITE_HEIGHT);
    graph_co2.fillSprite(TFT_BLACK);
    graph_co2.fillRect(0, 0, SPRITE_WIDTH, p_cau + 1, getColor(50, 50, 0));
    graph_co2.fillRect(0, 0, SPRITE_WIDTH, p_war + 1, getColor(50, 0, 0));

    spr_values.setColorDepth(8);
    spr_values.createSprite(SPRITE_WIDTH, MYTFT_HEIGHT - SPRITE_HEIGHT);
    spr_values.fillSprite(TFT_BLACK);
}

void updateDisplay() {
    static int y_prev = 0;
    // write value
    spr_values.setCursor(0, 0);
    spr_values.setTextSize(3);
    spr_values.fillSprite(TFT_BLACK);
    if (co2_ppm < co2_caution_ppm)
        spr_values.setTextColor(TFT_WHITE);
    else if (co2_ppm < co2_warning_ppm)
        spr_values.setTextColor(TFT_YELLOW);
    else
        spr_values.setTextColor(TFT_RED);
    spr_values.printf("CO2:  %4d ppm\n", co2_ppm);
    spr_values.setTextColor(TFT_WHITE);
    spr_values.printf("Temp: %4.1f degC\n", temperature_c);
    spr_values.printf("Humid:%4.1f %%\n", humidity_p);

    // draw lines
    p_cau = getPositionY(co2_caution_ppm);
    p_war = getPositionY(co2_warning_ppm);
    graph_co2.drawFastVLine(SPRITE_WIDTH - 1, 0, p_war + 1, getColor(50, 0, 0));
    graph_co2.drawFastVLine(SPRITE_WIDTH - 1, p_war + 1, p_cau - p_war, getColor(50, 50, 0));

    // draw co2
    int32_t y = getPositionY(co2_ppm);
    if (y > y_prev)
        graph_co2.drawFastVLine(SPRITE_WIDTH - 1, y_prev, abs(y - y_prev) + 1, TFT_WHITE);
    else
        graph_co2.drawFastVLine(SPRITE_WIDTH - 1, y, abs(y - y_prev) + 1, TFT_WHITE);
    y_prev = y;

    // update
    spr_values.pushSprite(0, 0);
    graph_co2.pushSprite(0, MYTFT_HEIGHT - SPRITE_HEIGHT);
    graph_co2.scroll(-1, 0);
}

int checkCo2Level(int level, int co2_ppm) {
    static int over_thre_timer_cau = 0;
    static int over_thre_timer_war = 0;

    Serial.printf("level:%d\n", level);
    Serial.printf("over_thre_timer_cau: %d\n", over_thre_timer_cau);
    Serial.printf("over_thre_timer_war: %d\n", over_thre_timer_war);

    switch (level) {
        case LEVEL_NORMAL:
            if (co2_ppm > co2_caution_ppm) {
                over_thre_timer_cau += SENSOR_INTERVAL_S;
                if (over_thre_timer_cau > NOTIFY_TIMER_S) {
                    over_thre_timer_cau = 0;
                    return LEVEL_CAUTION;
                }
            } else
                over_thre_timer_cau = 0;
            break;

        case LEVEL_CAUTION:
            if (co2_ppm > co2_warning_ppm) {
                over_thre_timer_war += SENSOR_INTERVAL_S;
                if (over_thre_timer_war > NOTIFY_TIMER_S) {
                    over_thre_timer_war = 0;
                    return LEVEL_WARNING;
                }
            } else
                over_thre_timer_war = 0;
            if (co2_ppm < (co2_caution_ppm - CO2_HYSTERESIS_PPM))
                return LEVEL_NORMAL;
            break;

        case LEVEL_WARNING:
            if (co2_ppm < (co2_warning_ppm - CO2_HYSTERESIS_PPM))
                return LEVEL_CAUTION;
            break;

        default:
            break;
    }

    return level;
}

void incrementValue(int selected, int delta) {
    switch (selected) {
        case 0:
            if (co2_max_ppm < CO2_MAX_RANGE)
                co2_max_ppm += delta;
            break;

        case 1:
            if (co2_warning_ppm < co2_max_ppm)
                co2_warning_ppm += delta;
            break;

        case 2:
            if (co2_caution_ppm < co2_warning_ppm)
                co2_caution_ppm += delta;
            break;

        case 3:
            if (temperature_offsetx10 < TEMPERATURE_OFFSET_MAX)
                temperature_offsetx10 += delta;
            break;

        default:
            break;
    }
}

void decrementValue(int selected, int delta) {
    switch (selected) {
        case 0:
            if (co2_warning_ppm < co2_max_ppm)
                co2_max_ppm -= delta;
            break;

        case 1:
            if (co2_caution_ppm < co2_warning_ppm)
                co2_warning_ppm -= delta;
            break;

        case 2:
            if (CO2_MIN_PPM < co2_caution_ppm)
                co2_caution_ppm -= delta;
            break;

        case 3:
            if (TEMPERATURE_OFFSET_MIN < temperature_offsetx10)
                temperature_offsetx10 -= delta;
            break;

        default:
            break;
    }
}

void calibrateCO2() {
    int selected = 1;
    while (1) {
        M5.Lcd.clear();
        M5.Lcd.setCursor(0, 0);
        M5.Lcd.print("Are you sure you want to calibrate CO2?\n");
        M5.Lcd.print("(expose to outside air for 2min before calibration)\n\n");
        if (selected == 0)
            M5.Lcd.print(">");
        else
            M5.Lcd.print(" ");
        M5.Lcd.print("Yes  ");
        if (selected == 1)
            M5.Lcd.print(">");
        else
            M5.Lcd.print(" ");
        M5.Lcd.print("No\n");

        while (1) {
            M5.update();
            if (M5.BtnA.wasPressed()) {
                selected = 0;
                break;
            }
            if (M5.BtnB.wasPressed()) {
                if (selected == 0) {
                    if (airSensor.setForcedRecalibrationFactor(CO2_CALIBRATION_FACTOR)) {
                        M5.Lcd.print("Calibration done!");
                    } else {
                        M5.Lcd.print("Calibration failed...");
                    }
                    delay(2000);
                }
                return;
            }
            if (M5.BtnC.wasPressed()) {
                selected = 1;
                break;
            }
            delay(100);
        }
    }
}

void showSetting() {
    int selected = 0;
    float delta;

    float temp_ofs = airSensor.getTemperatureOffset();
    temperature_offsetx10 = (int)(temp_ofs * 10.0);

    while (selected < 5) {
        M5.Lcd.clear();
        M5.Lcd.setCursor(0, 0);

        M5.Lcd.print("  Setting\n\n");

        if (selected == 0) {
            M5.Lcd.print("> ");
            delta = CO2_SET_DELTA;
        } else
            M5.Lcd.print("  ");
        M5.Lcd.printf("Max    :%5d [ppm]\n", co2_max_ppm);

        if (selected == 1) {
            M5.Lcd.print("> ");
            delta = CO2_SET_DELTA;
        } else
            M5.Lcd.print("  ");
        M5.Lcd.setTextColor(TFT_RED);
        M5.Lcd.printf("Warning:%5d [ppm]\n", co2_warning_ppm);
        M5.Lcd.setTextColor(TFT_WHITE);

        if (selected == 2) {
            M5.Lcd.print("> ");
            delta = CO2_SET_DELTA;
        } else
            M5.Lcd.print("  ");
        M5.Lcd.setTextColor(TFT_YELLOW);
        M5.Lcd.printf("Caution:%5d [ppm]\n", co2_caution_ppm);
        M5.Lcd.setTextColor(TFT_WHITE);

        if (selected == 3) {
            M5.Lcd.print("> ");
            delta = TEMPERATURE_SET_DELTA;
        } else
            M5.Lcd.print("  ");
        M5.Lcd.printf("TempOfs:%5.1f [degC]\n", (float)temperature_offsetx10 / 10.0);

        if (selected == 4) {
            M5.Lcd.print("> ");
            delta = 0;
        } else
            M5.Lcd.print("  ");
        M5.Lcd.printf("CO2 Cal. (press Btn A/C)\n");

        while (1) {
            M5.update();
            if (selected != 4) {
                if (M5.BtnA.wasPressed()) {
                    decrementValue(selected, delta);
                    break;
                }
                if (M5.BtnA.pressedFor(600)) {
                    decrementValue(selected, delta);
                    break;
                }
                if (M5.BtnC.wasPressed()) {
                    incrementValue(selected, delta);
                    break;
                }
                if (M5.BtnC.pressedFor(600)) {
                    incrementValue(selected, delta);
                    break;
                }
            } else {
                if (M5.BtnA.wasPressed() || M5.BtnC.wasPressed()) {
                    calibrateCO2();
                    break;
                }
            }
            if (M5.BtnB.wasPressed()) {
                selected++;
                break;
            }
            delay(100);
        }
    }
}

void saveSetting() {
    preferences.begin(PREF_NAMESPACE, false);
    preferences.putInt(KEY_CO2_MAX, co2_max_ppm);
    preferences.putInt(KEY_CO2_WARNING, co2_warning_ppm);
    preferences.putInt(KEY_CO2_CAUTION, co2_caution_ppm);
    preferences.end();

    // set temperature offset in SCD30 non-volatile memory
    float ofs = (float)temperature_offsetx10 / 10.0;
    if (airSensor.setTemperatureOffset(ofs)) {
        Serial.println("setTemperatureOffset OK");
    } else {
        Serial.println("setTemperatureOffset NG");
    }
}

void resetGraphSprite() {
    p_cau = getPositionY(co2_caution_ppm);
    p_war = getPositionY(co2_warning_ppm);
    graph_co2.fillSprite(TFT_BLACK);
    graph_co2.fillRect(0, 0, SPRITE_WIDTH, p_cau + 1, getColor(50, 50, 0));
    graph_co2.fillRect(0, 0, SPRITE_WIDTH, p_war + 1, getColor(50, 0, 0));
    graph_co2.pushSprite(0, MYTFT_HEIGHT - SPRITE_HEIGHT);
}

void loop() {
    if (airSensor.dataAvailable()) {
        // get sensor data
        co2_ppm = airSensor.getCO2();
        Serial.print("co2(ppm):");
        Serial.print(co2_ppm);

        temperature_c = airSensor.getTemperature();
        Serial.print(" temp(C):");
        Serial.print(temperature_c, 1);

        humidity_p = airSensor.getHumidity();
        Serial.print(" humidity(%):");
        Serial.print(humidity_p, 1);

        Serial.println();

        updateDisplay();

        if (use_pushbullet) {
            int co2_level_now = checkCo2Level(co2_level_last, co2_ppm);

            // notify user when co2 level exceed threshold
            if (co2_level_now > co2_level_last) {
                if (co2_level_now == LEVEL_CAUTION) {
                    if (notifyUser(co2_level_now))
                        Serial.println("notifyUser(): CAUTION");
                    else
                        Serial.println("notifyUser(): failed!");
                }
                if (co2_level_now == LEVEL_WARNING) {
                    if (notifyUser(co2_level_now))
                        Serial.println("notifyUser(): WARNING");
                    else
                        Serial.println("notifyUser(): failed!");
                }
            }

            co2_level_last = co2_level_now;
        }
    } else {
        Serial.println("Waiting for new data");
    }

    for (int i = 0; i < (float)SENSOR_INTERVAL_S / (float)BTNCHK_INTERVAL_S; ++i) {
        M5.update();
        if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) {
            showSetting();
            saveSetting();
            resetGraphSprite();
            break;
        }
        delay(BTNCHK_INTERVAL_S * 1000);
    }

    if (use_ambient) {
        elapsed_time += SENSOR_INTERVAL_S;
        Serial.printf("ambient timer: %d/%d\n", elapsed_time, UPLOAD_INTERVAL_S);
        if (elapsed_time >= UPLOAD_INTERVAL_S) {
            elapsed_time = 0;
            // upload to ambient
            Serial.print("Send data to ambient...");
            ambient.set(1, co2_ppm);
            ambient.set(2, temperature_c);
            ambient.set(3, humidity_p);
            ambient.send();
            Serial.println("done");
        }
    }
}

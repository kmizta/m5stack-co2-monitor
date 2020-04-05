#include <Wire.h>
#include <M5Stack.h>
#include <WiFi.h>
#include <Ambient.h>
#include "SparkFun_SCD30_Arduino_Library.h"
#include <WiFiClientSecure.h>
#include "envs.h"

#define USE_AMBIENT    // comment out if you don't use Ambient for logging
#define USE_PUSHBULLET // comment out if you don't use PushBullet for notifying

// WiFi setting
#if defined(USE_AMBIENT) || defined(USE_PUSHBULLET)
#define USE_WIFI
const char *ssid{WIFI_SSID};           // write your WiFi SSID (2.4GHz)
const char *password{WIFI_PASSWORD};   // write your WiFi password
#endif

// Ambient setting
#ifdef USE_AMBIENT
WiFiClient client;
Ambient ambient;
unsigned int channelId{AMBIENT_CHID};   // write your Ambient channel ID
const char *writeKey{AMBIENT_WRITEKEY};  // write your Ambient write key
#endif

// Pushbullet setting
#ifdef USE_PUSHBULLET
const char *APIKEY{PB_APIKEY};  // write your Pushbullet API KEY
WiFiClientSecure secureClient;
int notify_timer_caution = 0;
int notify_timer_warning = 0;
bool pause_notify_caution = true;
bool pause_notify_warning = true;
#define PAUSE_LENGTH 600 // do not notify PAUSE_LENGTH [s] once notified
#endif

// sensor setting
SCD30 airSensor;
#define SENSOR_INTERVAL_S 2     // get sensor value every SENSOR_INTERVAL_S [s]
#define UPLOAD_INTERVAL_S 60    // upload data to Ambient every UPLOAD_INTERVAL_S [s]

// TFT setting
#define TFT_WIDTH 320
#define TFT_HEIGHT 240
#define SPRITE_WIDTH 320
#define SPRITE_HEIGHT 160

// CO2 level setting
#define CO2_MIN_PPM 0
#define CO2_MAX_PPM 2500
#define CO2_RANGE_INT 500
#define CO2_CAUTION_PPM 1000
#define CO2_WARNING_PPM 2000

enum
{
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

#ifdef USE_PUSHBULLET
// reference: https://fipsok.de/Esp32-Webserver/push-Esp32-tab
bool pushbullet(const String &message)
{
    const uint16_t timeout{5000};
    const char *HOST{"api.pushbullet.com"};
    String messagebody = R"({"type": "note", "title": "CO2 Monitor", "body": ")" + message + R"("})";
    uint32_t broadcastingTime{millis()};
    if (!secureClient.connect(HOST, 443))
    {
        Serial.println("Pushbullet connection failed!");
        return false;
    }
    else
    {
        secureClient.printf("POST /v2/pushes HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s\r\n", HOST, APIKEY, messagebody.length(), messagebody.c_str());
        Serial.println("Push sent");
    }
    while (!secureClient.available())
    {
        if (millis() - broadcastingTime > timeout)
        {
            Serial.println("Pushbullet Client Timeout !");
            secureClient.stop();
            return false;
        }
    }
    while (secureClient.available())
    {
        String line = secureClient.readStringUntil('\n');
        if (line.startsWith("HTTP/1.1 200 OK"))
        {
            secureClient.stop();
            return true;
        }
    }
    return false;
}

bool notifyUser(int level)
{
    const char *title = "CO2 Monitor";
    char body[100];

    switch (level)
    {
    case LEVEL_CAUTION:
        sprintf(body, "CO2 exceeded %d ppm. Ventilate please.", CO2_CAUTION_PPM);
        return pushbullet(body);

    case LEVEL_WARNING:
        sprintf(body, "CO2 exceeded %d ppm. Ventilate immediately.", CO2_WARNING_PPM);
        return pushbullet(body);

    default:
        return false;
    }
}
#endif

uint16_t getColor(uint8_t red, uint8_t green, uint8_t blue)
{
    return ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
}

void setup()
{
    p_cau = getPositionY(CO2_CAUTION_PPM);
    p_war = getPositionY(CO2_WARNING_PPM);

    M5.begin();
    M5.Power.begin();
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextSize(2);
    Serial.begin(115200);

#ifdef USE_WIFI
    // Wifi setup
    M5.Lcd.print("WiFi setup...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    M5.Lcd.println("done");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.print(WiFi.localIP());
#endif
#ifdef USE_AMBIENT
    ambient.begin(channelId, writeKey, &client);
#endif

    // sensor setup
    M5.Lcd.print("SCD30 setup...");
    Wire.begin();
    if (airSensor.begin() == false)
    {
        M5.Lcd.println("fail!");
        Serial.println("Air sensor not detected. Please check wiring. Freezing...");
        while (1)
            ;
    }
    M5.Lcd.println("done");
    delay(1000);

    M5.Lcd.fillScreen(TFT_BLACK);
    graph_co2.setColorDepth(8);
    graph_co2.createSprite(SPRITE_WIDTH, SPRITE_HEIGHT);
    graph_co2.fillSprite(TFT_BLACK);
    graph_co2.fillRect(0, 0, SPRITE_WIDTH, p_cau + 1, getColor(50, 50, 0));
    graph_co2.fillRect(0, 0, SPRITE_WIDTH, p_war + 1, getColor(50, 0, 0));

    spr_values.setColorDepth(8);
    spr_values.createSprite(SPRITE_WIDTH, TFT_HEIGHT - SPRITE_HEIGHT);
    spr_values.fillSprite(TFT_BLACK);
}

int getPositionY(int ppm)
{
    return SPRITE_HEIGHT - (int32_t)((float)SPRITE_HEIGHT / (CO2_MAX_PPM - CO2_MIN_PPM) * ppm);
}

void updateDisplay()
{
    static int y_prev = 0;
    // write value
    spr_values.setCursor(0, 0);
    spr_values.setTextSize(3);
    spr_values.fillSprite(TFT_BLACK);
    if (co2_ppm < CO2_CAUTION_PPM)
        spr_values.setTextColor(TFT_WHITE);
    else if (co2_ppm < CO2_WARNING_PPM)
        spr_values.setTextColor(TFT_YELLOW);
    else
        spr_values.setTextColor(TFT_RED);
    spr_values.printf("CO2:  %4d ppm\n", co2_ppm);
    spr_values.setTextColor(TFT_WHITE);
    spr_values.printf("Temp: %4.1f deg\n", temperature_c);
    spr_values.printf("Humid:%4.1f %%\n", humidity_p);

    // draw lines
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
    graph_co2.pushSprite(0, TFT_HEIGHT - SPRITE_HEIGHT);
    graph_co2.scroll(-1, 0);
}

void loop()
{
    if (airSensor.dataAvailable())
    {
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

#ifdef USE_PUSHBULLET
        int co2_level_now;
        // check co2 level
        if (co2_ppm < CO2_CAUTION_PPM)
            co2_level_now = LEVEL_NORMAL;
        else if (co2_ppm < CO2_WARNING_PPM)
            co2_level_now = LEVEL_CAUTION;
        else
            co2_level_now = LEVEL_WARNING;

        // notify user when co2 level exceed threshold
        if (co2_level_now > co2_level_last)
        {
            if (co2_level_now == LEVEL_CAUTION && !pause_notify_caution)
            {
                if(notifyUser(co2_level_now)){
                    Serial.println("notifyUser(): CAUTION");
                }else{
                    Serial.println("notifyUser(): failed!");
                }
                pause_notify_caution = true;
            }
            if (co2_level_now == LEVEL_WARNING && !pause_notify_warning)
            {
                if(notifyUser(co2_level_now)){
                    Serial.println("notifyUser(): WARNING");
                }else{
                    Serial.println("notifyUser(): failed!");
                }                
                pause_notify_warning = true;
            }
        }

        co2_level_last = co2_level_now;
#endif
    }
    else
        Serial.println("Waiting for new data");

    delay(SENSOR_INTERVAL_S * 1000);

#ifdef USE_AMBIENT
    elapsed_time += SENSOR_INTERVAL_S;
    if (elapsed_time >= UPLOAD_INTERVAL_S)
    {
        elapsed_time = 0;
        // upload to ambient
        Serial.print("Send data to ambient...");
        ambient.set(1, co2_ppm);
        ambient.set(2, temperature_c);
        ambient.set(3, humidity_p);
        ambient.send();
        Serial.println("done");
    }
#endif

#ifdef USE_PUSHBULLET
    if (pause_notify_caution)
    {
        notify_timer_caution += SENSOR_INTERVAL_S;
        Serial.printf("notify_timer_caution: %d\n", notify_timer_caution);
        if (notify_timer_caution > PAUSE_LENGTH)
        {
            notify_timer_caution = 0;
            pause_notify_caution = false;
            Serial.println("notify_timer_caution set false");
        }
    }
    if (pause_notify_warning)
    {
        notify_timer_warning += SENSOR_INTERVAL_S;
        Serial.printf("notify_timer_warning: %d\n", notify_timer_warning);
        if (notify_timer_warning > PAUSE_LENGTH)
        {
            notify_timer_warning = 0;
            pause_notify_warning = false;
            Serial.println("notify_timer_warning set false");
        }
    }
#endif
}

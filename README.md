# M5Stack CO2 Monitor

M5StackとSCD30を使ってCO2濃度・温湿度を計測、表示します。  
オプションでAmbientによるログ保存、Pushbulletで濃度レベルに応じた通知（PC/スマホ）ができます。

 ![image1](https://pbs.twimg.com/media/EUWoylyU4AINFB8?format=jpg&name=small)

# Requirement

Hardware
* [M5Stack Basic](https://www.switch-science.com/catalog/3647/)
* [SEN-15112 (SCD30 module)](https://www.sengoku.co.jp/mod/sgk_cart/detail.php?code=EEHD-5CRY)

Software
* [SCD30 Arduino Library](https://github.com/sparkfun/SparkFun_SCD30_Arduino_Library)

Option
* [Ambient](https://ambidata.io/) account
* [Pushbullet](https://www.pushbullet.com/) account

# Usage

以下のブログを参照ください。

[換気のすゝめ　～M5StackでCO2濃度モニタを作る～](https://westgate-lab.hatenablog.com/entry/2020/04/01/224511)

また、co2monitor.inoと同じディレクトリにenvs.hを作成し、以下のように各種定数を定義してください。（ver.2のみ）
```
#define WIFI_SSID "YOUR SSID"
#define WIFI_PASSWORD "YOUR PASSWORD"
#define AMBIENT_CHID 11111
#define AMBIENT_WRITEKEY "YOUR WRITE KEY"
#define PB_APIKEY "YOUR PUSHBULLET API KEY"
```
もしWifiやAmbient、Pushbulletを使用しない場合は、空のenvs.hを作成しco2monitor.ino中の#define USE_AMBIENTおよび#define USE_PUSHBULLETをコメントアウトしてください。

# Author
 
[![Twitter URL](https://img.shields.io/twitter/url/https/twitter.com/kmizta.svg?style=social&label=Follow%20%40kmizta)](https://twitter.com/kmizta)
 
# License
 
"M5Stack CO2 Monitor" is under [MIT license](https://en.wikipedia.org/wiki/MIT_License).

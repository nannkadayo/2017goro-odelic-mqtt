#include <WiFi.h>
#include <PubSubClient.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ArduinoJson.h>
#include <Preferences.h>

/* ===== ユーザー設定 ===== */
const char* ssid               = "Buffalo-G-C670";
const char* password           = "wifispot10";
const char* mqtt_server        = "192.168.11.56";
const char* mqtt_command_topic = "ir/color";
const char* mqtt_state_topic   = "ir/color/state";
const char* mqtt_user          = "nannkadayo";
const char* mqtt_pass          = "kokodede";

/* ===== IR設定 ===== */
#define IR_PIN 4
IRsend irsend(IR_PIN);

/* ===== MQTT ===== */
WiFiClient espClient;
PubSubClient client(espClient);

/* ===== 不揮発性メモリ ===== */
Preferences preferences;

/* ===== 現在の状態 ===== */
struct {
  bool isOn        = false;
  uint8_t orange   = 10;   // 保存される基準値（0-10）
  uint8_t blue     = 10;   // 保存される基準値（0-10）
  uint8_t brightness = 10; // 明るさレベル（0-10）
  String mode      = "color";
} currentState;

/* ===== NEC互換 RAW 送信 ===== */
void sendNEC_Compatible(uint16_t address, uint16_t command) {
  uint16_t raw[100];
  int idx = 0;

  raw[idx++] = 8884;
  raw[idx++] = 4504;

  uint32_t data = irsend.encodeNEC(address, command);
  for (int i = 0; i < 32; i++) {
    raw[idx++] = 576;
    raw[idx++] = (data & 0x80000000UL) ? 1660 : 580;
    data <<= 1;
  }

  raw[idx++] = 576;
  raw[idx++] = 608;
  raw[idx++] = 550;

  irsend.sendRaw(raw, idx, 38);
}

/* ===== 色コマンド送信 ===== */
void sendColor(uint8_t orange, uint8_t blue) {
  uint16_t addr = 0xC580;
  uint8_t cmd1 = 0;
  uint8_t cmd2 = 0;

  // 暖色 (オレンジ)
  if (orange >= 1 && orange <= 10) {
    cmd1 = 0x58 + (orange - 1) * 2;
  } else if (orange == 0 && blue > 0) {
    cmd1 = 0xDA;
  }

  // 寒色 (ブルー)
  if (blue == 0) {
    cmd2 = 0xDC;
  } else if (blue >= 1 && blue <= 10) {
    cmd2 = 0x6C + (blue - 1) * 2;
  }

  if (cmd1 != 0) {
    Serial.printf("送信: Orange=%d -> 0x%02X\n", orange, cmd1);
    sendNEC_Compatible(addr, cmd1);
  }

  if (cmd2 != 0) {
    delay(40);
    Serial.printf("送信: Blue=%d -> 0x%02X\n", blue, cmd2);
    sendNEC_Compatible(addr, cmd2);
  }

  if (cmd1 == 0 && cmd2 == 0) {
    Serial.println("エラー: 無効なレベル設定");
  }
}

void sendColorWithBrightness() {
  int base_orange = currentState.orange;
  int base_blue   = currentState.blue;
  int b = currentState.brightness; // 0-10

  int actual_orange;
  int actual_blue;

  if (b <= 5) {
    // 減光ゾーン（今までと同じ考え方）
    float scale = b / 5.0;
    actual_orange = round(base_orange * scale);
    actual_blue   = round(base_blue   * scale);
  } else {
    // ブーストゾーン
    float boost = (b - 5) / 5.0; // 0.0 - 1.0

    actual_orange = base_orange + round((10 - base_orange) * boost);
    actual_blue   = base_blue   + round((10 - base_blue)   * boost);
  }

  actual_orange = constrain(actual_orange, 0, 10);
  actual_blue   = constrain(actual_blue, 0, 10);

  Serial.printf(
    "brightness=%d 基準(%d:%d) -> 送信(%d:%d)\n",
    b, base_orange, base_blue, actual_orange, actual_blue
  );

  sendColor(actual_orange, actual_blue);
}

/* ===== 設定保存 ===== */
void saveSettings() {
  preferences.begin("light", false);
  preferences.putUChar("orange", currentState.orange);
  preferences.putUChar("blue", currentState.blue);
  preferences.putUChar("brightness", currentState.brightness);
  preferences.putString("mode", currentState.mode);
  preferences.end();

  Serial.printf(
    "設定保存: orange=%d, blue=%d, brightness=%d\n",
    currentState.orange,
    currentState.blue,
    currentState.brightness
  );
}

/* ===== 設定読込 ===== */
void loadSettings() {
  preferences.begin("light", true);
  currentState.orange = preferences.getUChar("orange", 10);
  currentState.blue   = preferences.getUChar("blue", 10);
  currentState.brightness = preferences.getUChar("brightness", 10);
  currentState.mode   = preferences.getString("mode", "color");
  preferences.end();

  Serial.printf(
    "設定読込: orange=%d, blue=%d, brightness=%d\n",
    currentState.orange,
    currentState.blue,
    currentState.brightness
  );
}

/* ===== 状態送信 ===== */
void publishState() {
  StaticJsonDocument<256> doc;

  doc["state"] = currentState.isOn ? "ON" : "OFF";
  doc["color_mode"] = "color_temp";
  
  // Kelvin値を計算（基準値から）
  uint32_t kelvin = 2000 + (uint32_t)((10 - currentState.orange) * 450);
  doc["color_temp"] = kelvin;
  
  // 明るさを0-255スケールに変換（10段階 -> 255段階）
  // 10段階の中央値になるよう四捨五入
  uint8_t brightness_255 = (currentState.brightness * 255 + 5) / 10;
  doc["brightness"] = brightness_255;

  char buffer[256];
  serializeJson(doc, buffer);
  client.publish(mqtt_state_topic, buffer, true);

  Serial.print("状態送信: ");
  Serial.println(buffer);
}

/* ===== MQTT受信 ===== */
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("\n--- MQTT受信 [%s] ---\n", topic);
  for (unsigned int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println("\n------------------");

  StaticJsonDocument<200> doc;
  if (deserializeJson(doc, payload, length)) {
    Serial.println("JSON解析エラー");
    return;
  }

  // brightness が含まれている場合は先に処理
  if (doc.containsKey("brightness")) {
    // 0-255 -> 0-10 に変換（四捨五入）
    uint8_t brightness_255 = doc["brightness"];
    currentState.brightness = (brightness_255 * 10 + 128) / 255;
    currentState.brightness = constrain(currentState.brightness, 0, 10);
    
    Serial.printf("明るさ更新: %d/255 -> %d/10\n", brightness_255, currentState.brightness);
  }

  // 単純なON/OFF制御（brightness付きも含む）
  if (!doc.containsKey("mode") && !doc.containsKey("color_temp") && !doc.containsKey("color_temp_kelvin")) {
    if (doc.containsKey("state")) {
      const char* state = doc["state"];
      if (strcmp(state, "ON") == 0) {
        (currentState.mode == "night")
          ? sendNEC_Compatible(0xC580, 0x2A)
          : sendColorWithBrightness();
        currentState.isOn = true;
      } else if (strcmp(state, "OFF") == 0) {
        sendNEC_Compatible(0xC580, 0x08);
        currentState.isOn = false;
      }
      saveSettings();
      publishState();
    }
    return;
  }

  // モード処理
  const char* mode = "color";
  if (doc.containsKey("mode")) {
    mode = doc["mode"];
  }

  if (strcmp(mode, "off") == 0) {
    sendNEC_Compatible(0xC580, 0x08);
    currentState.isOn = false;

  } else if (strcmp(mode, "night") == 0) {
    sendNEC_Compatible(0xC580, 0x2A);
    currentState.isOn = true;
    currentState.mode = "night";
    saveSettings();

  } else if (strcmp(mode, "color") == 0 ||
             doc.containsKey("color_temp") ||
             doc.containsKey("color_temp_kelvin")) {

    uint32_t k = 2000;

    if (doc.containsKey("color_temp_kelvin")) {
      k = doc["color_temp_kelvin"];
      Serial.print("受信ケルビン: ");
      Serial.println(k);

    } else if (doc.containsKey("color_temp")) {
      uint32_t mired = doc["color_temp"];
      k = mired;
      Serial.print("受信mired: ");
      Serial.print(mired);
      Serial.print(" -> Kelvin: ");
      Serial.println(k);
    }

    k = constrain(k, 2000, 6500);

    // Kelvin → orange/blue 変換（基準値として保存）
    int calculated_orange = 10 - (int)((k - 2000) * 10 / 4500);
    currentState.orange = constrain(calculated_orange, 0, 10);
    currentState.blue = 10 - currentState.orange;

    Serial.printf("計算結果: Kelvin=%d, Orange=%d, Blue=%d (基準値)\n", 
                  k, currentState.orange, currentState.blue);

    // brightness が同時に指定されている場合は更新
    if (doc.containsKey("brightness")) {
      uint8_t brightness_255 = doc["brightness"];
      currentState.brightness = (brightness_255 * 10 + 128) / 255;
      currentState.brightness = constrain(currentState.brightness, 0, 10);
    }

    // IRコマンド送信（明るさ適用）
    sendColorWithBrightness();

    currentState.isOn = true;
    currentState.mode = "color";

    saveSettings();
  }

  delay(100);
  publishState();  // 四捨五入した値を返す
}

/* ===== WiFi ===== */
void setup_wifi() {
  Serial.println("\nWiFi接続中...");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi接続成功");
  Serial.print("IPアドレス: ");
  Serial.println(WiFi.localIP());
}

/* ===== MQTT再接続 ===== */
void reconnect() {
  for (int i = 0; i < 5 && !client.connected(); i++) {
    Serial.print("MQTT接続試行中...");
    if (client.connect("ESP32_IR", mqtt_user, mqtt_pass)) {
      Serial.println("成功");
      client.subscribe(mqtt_command_topic);
      publishState();
      return;
    }
    Serial.println("失敗、5秒後再試行");
    delay(5000);
  }
}

/* ===== SETUP ===== */
void setup() {
  Serial.begin(115200);
  irsend.begin();
  loadSettings();
  setup_wifi();

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  client.setBufferSize(512);
}

/* ===== LOOP ===== */
void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
}
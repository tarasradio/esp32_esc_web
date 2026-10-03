#include <ESP32Servo.h>
#include <WiFi.h>
#include <WebServer.h>

// ==================== ПИНЫ ====================
#define MOTOR_L_PIN   25
#define MOTOR_R_PIN   26
#define MOTOR_S_PIN   27
#define LED_RED_PIN   14
#define LED_BLUE_PIN  12

// ==================== ИНВЕРСИЯ МОТОРОВ ====================
// true = инвертировать направление (если колесо крутится не туда)
#define MOTOR_L_INVERTED  true
#define MOTOR_R_INVERTED  false    // <-- правое колесо было в другую сторону

// ==================== СИГНАЛЫ ESC ====================
#define US_STOP      1488
#define US_MIN       1376   // полный назад
#define US_MAX       1600   // полный вперёд

// Границы скоростей для спиннера
#define SPINNER_US_MIN 1200   // полный назад
#define SPINNER_US_MAX 1800   // полный вперёд

// ==================== FAILSAFE ====================
const unsigned long FAILSAFE_TIMEOUT_MS = 500;
unsigned long lastClientPacketTime = 0;
bool failsafeActive = true;

// ==================== СЕТЬ ====================
const char* AP_SSID = "BattleBot";
const char* AP_PASS = "Tosuffer";
WebServer server(80);

// ==================== МОТОРЫ ====================
Servo motors[3];

// Целевые значения (от веб-интерфейса)
int targetL = US_STOP;
int targetR = US_STOP;
int targetS = US_STOP;

// Текущие (для плавности)
int currentL = US_STOP;
int currentR = US_STOP;
int currentS = US_STOP;

// ==================== ВЕБ-ИНТЕРФЕЙС ====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>BattleBot Control</title>
<style>
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  body {
    margin: 0; padding: 12px; background: #111; color: #eee;
    font-family: -apple-system, Roboto, sans-serif;
    user-select: none; -webkit-user-select: none; touch-action: none;
  }
  h1 { font-size: 18px; text-align: center; margin: 4px 0 12px; }
  .status { text-align:center; font-size:14px; margin-bottom:10px; color:#888;}
  .status.on { color: #4caf50; }
  .status.off { color: #f44336; }
  .panel {
    display: flex; justify-content: space-around; align-items: center;
    gap: 20px; flex-wrap: wrap; margin-top: 10px;
  }
  .stick-wrap { text-align: center; }
  .stick-wrap .label { font-size: 14px; color:#ccc; margin-bottom: 6px; font-weight: bold;}
  .joystick {
    position: relative; width: 220px; height: 220px;
    background: #222; border-radius: 50%;
    border: 2px solid #444; touch-action: none;
  }
  .knob {
    position: absolute; width: 80px; height: 80px;
    background: #4caf50; border-radius: 50%;
    left: 70px; top: 70px; pointer-events: none;
    box-shadow: 0 0 12px rgba(76,175,80,0.7);
  }
  .spinner-wrap { text-align: center; }
  .spinner {
    position: relative; width: 80px; height: 240px;
    background: #222; border-radius: 40px;
    border: 2px solid #444; margin: 0 auto; touch-action: none;
  }
  .spinner-knob {
    position: absolute; width: 60px; height: 60px;
    background: #ff9800; border-radius: 50%;
    left: 8px; top: 90px; pointer-events: none;
    box-shadow: 0 0 12px rgba(255,152,0,0.7);
  }
  .spinner-value { font-size: 14px; color:#ff9800; margin-top: 6px; font-weight: bold;}
  .btn-stop {
    display: block; width: 100%; max-width: 320px; margin: 16px auto 0;
    padding: 14px; font-size: 16px; font-weight: bold;
    background: #d32f2f; color: #fff; border: none; border-radius: 10px;
  }
  .btn-stop:active { background: #b71c1c; }
  .hint { font-size: 12px; color:#666; text-align:center; margin-top:12px;}
</style>
</head>
<body>
  <h1>⚔️ BattleBot Control</h1>
  <div class="status" id="status">Подключение...</div>

  <div class="panel">
    <!-- ДЖОЙСТИК ДВИЖЕНИЯ -->
    <div class="stick-wrap">
      <div class="label">ДВИЖЕНИЕ</div>
      <div class="joystick" id="joyM"><div class="knob" id="knobM"></div></div>
    </div>

    <!-- СПИННЕР -->
    <div class="spinner-wrap">
      <div class="label">СПИННЕР</div>
      <div class="spinner" id="joyS"><div class="spinner-knob" id="knobS"></div></div>
      <div class="spinner-value" id="spinVal">0%</div>
    </div>
  </div>

  <button class="btn-stop" id="btnStop">🛑 СТОП (движение + спиннер)</button>
  <div class="hint">Джойстик движения возвращается в центр. Спиннер держит позицию.</div>

<script>
function clamp(v, a, b){ return Math.max(a, Math.min(b, v)); }

// ==================== СОСТОЯНИЕ ====================
let move = {x:0, y:0};   // движение: y = вперёд/назад, x = поворот
let spin = {y:0};        // спиннер: y = -1..1 (не сбрасывается)

// ==================== ДЖОЙСТИК ДВИЖЕНИЯ (возвратный) ====================
(function(){
  const el = document.getElementById('joyM');
  const knob = document.getElementById('knobM');
  let active = false;

  function setKnob(dx, dy){
    const r = el.getBoundingClientRect();
    const maxOffset = (r.width/2) - (knob.offsetWidth/2);
    knob.style.left = (r.width/2 - knob.offsetWidth/2 + dx*maxOffset) + 'px';
    knob.style.top  = (r.height/2 - knob.offsetHeight/2 + dy*maxOffset) + 'px';
  }

  function update(clientX, clientY){
    const r = el.getBoundingClientRect();
    const cx = r.left + r.width/2;
    const cy = r.top + r.height/2;
    let dx = clamp((clientX - cx) / (r.width/2), -1, 1);
    let dy = clamp((clientY - cy) / (r.height/2), -1, 1);
    setKnob(dx, dy);
    move.x = dx;
    move.y = -dy; // вверх = положительное
  }

  function center(){
    setKnob(0, 0);
    move.x = 0;
    move.y = 0;
  }

  el.addEventListener('touchstart', e => { active = true; e.preventDefault(); update(e.touches[0].clientX, e.touches[0].clientY); }, {passive:false});
  el.addEventListener('touchmove',  e => { if (active){ e.preventDefault(); update(e.touches[0].clientX, e.touches[0].clientY); } }, {passive:false});
  el.addEventListener('touchend',   e => { active = false; e.preventDefault(); center(); }, {passive:false});
  el.addEventListener('touchcancel',e => { active = false; center(); }, {passive:false});

  el.addEventListener('mousedown', e => { active = true; update(e.clientX, e.clientY); });
  window.addEventListener('mousemove', e => { if (active) update(e.clientX, e.clientY); });
  window.addEventListener('mouseup', e => { if (active){ active = false; center(); } });
})();

// ==================== СПИННЕР (НЕ возвратный) ====================
(function(){
  const el = document.getElementById('joyS');
  const knob = document.getElementById('knobS');
  const valEl = document.getElementById('spinVal');
  let active = false;

  function setKnob(dy){
    const r = el.getBoundingClientRect();
    const maxOffset = (r.height/2) - (knob.offsetHeight/2);
    knob.style.top = (r.height/2 - knob.offsetHeight/2 + dy*maxOffset) + 'px';
  }

  function update(clientY){
    const r = el.getBoundingClientRect();
    const cy = r.top + r.height/2;
    let dy = clamp((clientY - cy) / (r.height/2), -1, 1);
    setKnob(dy);
    spin.y = -dy;
    valEl.textContent = Math.round(spin.y * 100) + '%';
  }

  el.addEventListener('touchstart', e => { active = true; e.preventDefault(); update(e.touches[0].clientY); }, {passive:false});
  el.addEventListener('touchmove',  e => { if (active){ e.preventDefault(); update(e.touches[0].clientY); } }, {passive:false});
  el.addEventListener('touchend',   e => { active = false; e.preventDefault(); /* НЕ сбрасываем */ }, {passive:false});
  el.addEventListener('touchcancel',e => { active = false; /* НЕ сбрасываем */ }, {passive:false});

  el.addEventListener('mousedown', e => { active = true; update(e.clientY); });
  window.addEventListener('mousemove', e => { if (active) update(e.clientY); });
  window.addEventListener('mouseup', e => { if (active) active = false; /* НЕ сбрасываем */ });
})();

// ==================== ОТПРАВКА ====================
async function sendCommand(){
  try {
    // move.y = forward, move.x = turn
    const url = `/cmd?f=${move.y.toFixed(3)}&t=${move.x.toFixed(3)}&s=${spin.y.toFixed(3)}`;
    await fetch(url, {method:'GET', cache:'no-store'});
  } catch(e) {}
}
setInterval(sendCommand, 50);

// ==================== СТАТУС ====================
async function pollStatus(){
  try {
    const r = await fetch('/status', {cache:'no-store'});
    const j = await r.json();
    const el = document.getElementById('status');
    if (j.client) { el.textContent = '✅ Управление активно'; el.className='status on'; }
    else          { el.textContent = '⛔ Нет клиента — моторы стоп'; el.className='status off'; }
  } catch(e) {
    document.getElementById('status').textContent = '❌ Нет связи';
    document.getElementById('status').className = 'status off';
  }
}
setInterval(pollStatus, 500);

// ==================== КНОПКА СТОП ====================
document.getElementById('btnStop').addEventListener('click', () => {
  // Сброс движения
  move.x = 0; move.y = 0;
  const km = document.getElementById('knobM');
  const pm = km.parentElement;
  km.style.left = (pm.offsetWidth/2 - km.offsetWidth/2) + 'px';
  km.style.top  = (pm.offsetHeight/2 - km.offsetHeight/2) + 'px';

  // Сброс спиннера
  spin.y = 0;
  const ks = document.getElementById('knobS');
  const ps = ks.parentElement;
  ks.style.top = (ps.offsetHeight/2 - ks.offsetHeight/2) + 'px';
  document.getElementById('spinVal').textContent = '0%';

  sendCommand();
});
</script>
</body>
</html>
)rawliteral";

// ==================== ПРЕОБРАЗОВАНИЕ ====================
int axisToUsWheels(float v) {
  if (v > 1) v = 1;
  if (v < -1) v = -1;
  if (v >= 0) return (int)(US_STOP + v * (US_MAX - US_STOP));
  else        return (int)(US_STOP + v * (US_STOP - US_MIN));
}

int axisToUsSpinner(float v) {
  if (v > 1) v = 1;
  if (v < -1) v = -1;
  if (v >= 0) return (int)(US_STOP + v * (SPINNER_US_MAX - US_STOP));
  else        return (int)(US_STOP + v * (US_STOP - SPINNER_US_MIN));
}

// ==================== ОБРАБОТЧИКИ ====================
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleCmd() {
  if (server.hasArg("f") && server.hasArg("t")) {
    float fwd  = server.arg("f").toFloat();  // -1..1
    float turn = server.arg("t").toFloat();  // -1..1

    // Танковая миксация
    float left  = fwd + turn;
    float right = fwd - turn;

    // Ограничение
    if (left >  1) left  =  1;
    if (left < -1) left  = -1;
    if (right >  1) right =  1;
    if (right < -1) right = -1;

    // Инверсия моторов (если нужно)
    #if MOTOR_L_INVERTED
      left = -left;
    #endif
    #if MOTOR_R_INVERTED
      right = -right;
    #endif

    targetL = axisToUsWheels(left);
    targetR = axisToUsWheels(right);
  }

  if (server.hasArg("s")) {
    targetS = axisToUsSpinner(server.arg("s").toFloat());
  }

  lastClientPacketTime = millis();
  if (failsafeActive) {
    failsafeActive = false;
    Serial.println("Client connected — control enabled");
  }

  server.send(200, "text/plain", "ok");
}

void handleStatus() {
  String json = "{\"client\":" + String(failsafeActive ? "false" : "true") + "}";
  server.send(200, "application/json", json);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ==================== ПЛАВНОЕ ДВИЖЕНИЕ ====================
void updateMotors() {
  const int STEP = 4;

  int tL = failsafeActive ? US_STOP : targetL;
  int tR = failsafeActive ? US_STOP : targetR;
  int tS = failsafeActive ? US_STOP : targetS;

  if (currentL < tL) currentL = min(currentL + STEP, tL);
  else if (currentL > tL) currentL = max(currentL - STEP, tL);

  if (currentR < tR) currentR = min(currentR + STEP, tR);
  else if (currentR > tR) currentR = max(currentR - STEP, tR);

  if (currentS < tS) currentS = min(currentS + STEP, tS);
  else if (currentS > tS) currentS = max(currentS - STEP, tS);

  motors[0].writeMicroseconds(currentL);
  motors[1].writeMicroseconds(currentR);
  motors[2].writeMicroseconds(currentS);
}

// ==================== SETUP ====================
void setup() {
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_BLUE_PIN, OUTPUT);
  digitalWrite(LED_RED_PIN, HIGH);
  digitalWrite(LED_BLUE_PIN, LOW);

  Serial.begin(115200);
  delay(300);

  motors[0].attach(MOTOR_L_PIN, 1000, 2000);
  motors[1].attach(MOTOR_R_PIN, 1000, 2000);
  motors[2].attach(MOTOR_S_PIN, 1000, 2000);

  motors[0].writeMicroseconds(US_STOP);
  motors[1].writeMicroseconds(US_STOP);
  motors[2].writeMicroseconds(US_STOP);

  // === АРМИНГ ESC ===
  Serial.println("Arming ESCs...");
  delay(3000);

  motors[0].writeMicroseconds(US_MAX);
  motors[1].writeMicroseconds(US_MAX);
  motors[2].writeMicroseconds(US_MAX);
  delay(2000);

  motors[0].writeMicroseconds(US_STOP);
  motors[1].writeMicroseconds(US_STOP);
  motors[2].writeMicroseconds(US_STOP);
  delay(2000);
  Serial.println("Arming done");

  // === WI-FI AP ===
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/cmd", handleCmd);
  server.on("/status", handleStatus);
  server.on("/favicon.ico", [](){ server.send(204); });
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");

  lastClientPacketTime = millis();
  failsafeActive = true;
}

// ==================== LOOP ====================
void loop() {
  server.handleClient();

  if (!failsafeActive && (millis() - lastClientPacketTime > FAILSAFE_TIMEOUT_MS)) {
    failsafeActive = true;
    targetL = targetR = targetS = US_STOP;
    Serial.println("FAILSAFE — client lost");
  }

  if (failsafeActive) {
    digitalWrite(LED_RED_PIN, HIGH);
    digitalWrite(LED_BLUE_PIN, LOW);
  } else {
    digitalWrite(LED_RED_PIN, LOW);
    digitalWrite(LED_BLUE_PIN, HIGH);
  }

  updateMotors();
  delay(10);
}
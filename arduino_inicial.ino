/*
 * PROJETO: Controle Peltier (Quente/Frio) + Umidade
 * HARDWARE:
 * - Arduino Uno/Nano
 * - Sensor: DHT22 (Pino 5)
 * - Display: LCD 16x2 I2C (Endereço 0x27)
 * - Relé 1 e 2: Controle do Peltier (Ponte H a relé)
 * - Relé 3: Controle da Umidade
 * - Botões: 1x Push (Start), 2x Trava (Emergencia, Seleção), 1x Potenciômetro
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>

// --- DEFINIÇÃO DE PINOS ---
// Entradas
#define PIN_POT A0           // Potenciômetro
#define PIN_BTN_START 4      // Botão Push (Start/Pause)
#define PIN_CHAVE_SELETORA 3 // Chave: HIGH=Temp, LOW=Umidade
#define PIN_CHAVE_EMERG 2    // Chave de Emergência
#define PIN_SENSOR_DHT 5     // Dados do DHT22

// Saídas (Relés)
// Para o Peltier (Ponte H):
#define PIN_RELE_PELTIER_A 7 
#define PIN_RELE_PELTIER_B 8 
// Para a Umidade:
#define PIN_RELE_UMIDADE 9   

// Configuração
#define DHTTYPE DHT22
DHT dht(PIN_SENSOR_DHT, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2); 

// --- VARIÁVEIS DO SISTEMA ---
bool sistemaAtivo = false;      
bool emEmergencia = false;
bool ultimoEstadoBtnStart = HIGH; 
unsigned long lastDebounceTime = 0;
unsigned long delayLeitura = 0;
unsigned long delaySerial = 0;

// Setpoints Iniciais
float setpointTemp = 25.0; // Temperatura alvo
float setpointHum = 60.0;  // Umidade alvo

// Zonas Mortas (Histerese)
float histereseTemp = 1.0; // Evita oscilação térmica
float histereseHum = 3.0;  

void setup() {
  Serial.begin(9600);
  
  pinMode(PIN_BTN_START, INPUT_PULLUP);
  pinMode(PIN_CHAVE_SELETORA, INPUT_PULLUP);
  pinMode(PIN_CHAVE_EMERG, INPUT_PULLUP);
  
  pinMode(PIN_RELE_PELTIER_A, OUTPUT);
  pinMode(PIN_RELE_PELTIER_B, OUTPUT);
  pinMode(PIN_RELE_UMIDADE, OUTPUT);
  
  // Desliga tudo inicialmente
  pararTudo();

  dht.begin();
  lcd.init();
  lcd.backlight();
  
  Serial.println("--- INICIANDO SISTEMA ---");
  Serial.println("Monitoramento via Serial Ativo");
}

void loop() {
  // 1. VERIFICA EMERGÊNCIA (Prioridade Absoluta)
  if (digitalRead(PIN_CHAVE_EMERG) == LOW) { // Assumindo LOW como ativado
    if (!emEmergencia) {
      Serial.println("!!! PARADA DE EMERGENCIA ACIONADA !!!");
      emEmergencia = true;
      sistemaAtivo = false;
    }
    pararTudo();
    lcd.setCursor(0,0); lcd.print("!! EMERGENCIA !!");
    lcd.setCursor(0,1); lcd.print("SISTEMA TRAVADO ");
    delay(100);
    return; 
  } else {
    if (emEmergencia) {
      Serial.println("Emergencia liberada. Aguardando Start.");
      lcd.clear();
    }
    emEmergencia = false;
  }

  // 2. LEITURA DO BOTÃO START/PAUSE
  int leituraBtn = digitalRead(PIN_BTN_START);
  if (leituraBtn == LOW && ultimoEstadoBtnStart == HIGH && (millis() - lastDebounceTime) > 200) {
    sistemaAtivo = !sistemaAtivo;
    lastDebounceTime = millis();
    lcd.clear();
    if (sistemaAtivo) Serial.println("Status: SISTEMA INICIADO");
    else Serial.println("Status: SISTEMA PAUSADO");
  }
  ultimoEstadoBtnStart = leituraBtn;

  // 3. LEITURA SENSOR E POTENCIÔMETRO
  // Leitura com intervalo para não travar
  float tempAtual = dht.readTemperature();
  float humAtual = dht.readHumidity();

  // Tratamento de erro do sensor
  if (isnan(tempAtual) || isnan(humAtual)) {
    Serial.println("ERRO: Falha ao ler sensor DHT!");
    lcd.setCursor(0,0); lcd.print("ERRO SENSOR DHT ");
    pararTudo();
    return;
  }

  // Leitura do Potenciômetro para ajuste
  int valorPot = analogRead(PIN_POT);
  bool ajustandoTemp = (digitalRead(PIN_CHAVE_SELETORA) == HIGH);
  
  if (ajustandoTemp) {
    setpointTemp = map(valorPot, 0, 1023, 0, 50); // Ajuste 0°C a 50°C
  } else {
    setpointHum = map(valorPot, 0, 1023, 0, 100); // Ajuste 0% a 100%
  }

  // 4. LÓGICA DE CONTROLE
  if (sistemaAtivo) {
    controlarTemperatura(tempAtual);
    controlarUmidade(humAtual);
  } else {
    pararTudo();
  }

  // 5. ATUALIZA DISPLAYS (LCD e Serial)
  atualizarLCD(tempAtual, humAtual, ajustandoTemp);
  enviarSerial(tempAtual, humAtual);
}

// --- FUNÇÕES DE CONTROLE ---

void controlarTemperatura(float temp) {
  // Configuração para RESFRIAR (Padrão)
  // Relé A = LOW (GND), Relé B = HIGH (12V) -> (Exemplo, depende da fiação)
  
  // Se Temp > Alvo + Margem -> LIGA RESFRIAMENTO
  if (temp > (setpointTemp + histereseTemp)) {
    // Modo Frio
    digitalWrite(PIN_RELE_PELTIER_A, HIGH); 
    digitalWrite(PIN_RELE_PELTIER_B, LOW);
  } 
  // Se Temp < Alvo - Margem -> LIGA AQUECIMENTO (Inverte Polaridade)
  else if (temp < (setpointTemp - histereseTemp)) {
    // Modo Quente (Invertido)
    digitalWrite(PIN_RELE_PELTIER_A, LOW);
    digitalWrite(PIN_RELE_PELTIER_B, HIGH);
  } 
  // Se estiver dentro da margem ideal -> DESLIGA TUDO
  else {
    digitalWrite(PIN_RELE_PELTIER_A, HIGH); // Ambos HIGH ou Ambos LOW desligam
    digitalWrite(PIN_RELE_PELTIER_B, HIGH);
  }
}

void controlarUmidade(float hum) {
  // Se umidade baixa, liga ventoinha/umidificador
  if (hum < (setpointHum - histereseHum)) {
    digitalWrite(PIN_RELE_UMIDADE, LOW); // Liga (Relé ativo em LOW)
  } 
  else if (hum > setpointHum) {
    digitalWrite(PIN_RELE_UMIDADE, HIGH); // Desliga
  }
}

void pararTudo() {
  // Estado de desligado seguro
  digitalWrite(PIN_RELE_PELTIER_A, HIGH); // Módulos relé geralmente desligam em HIGH
  digitalWrite(PIN_RELE_PELTIER_B, HIGH);
  digitalWrite(PIN_RELE_UMIDADE, HIGH);
}

// --- FUNÇÕES DE VISUALIZAÇÃO ---

void atualizarLCD(float t, float h, bool modoTemp) {
  // Delay visual para não piscar muito o LCD
  static unsigned long lcdTimer = 0;
  if (millis() - lcdTimer < 300) return;
  lcdTimer = millis();

  lcd.setCursor(0, 0);
  if (sistemaAtivo) lcd.print("ON "); else lcd.print("OFF");
  lcd.print(" T:"); lcd.print((int)t); lcd.print("C H:"); lcd.print((int)h); lcd.print("%");

  lcd.setCursor(0, 1);
  if (modoTemp) {
    lcd.print("SetT:>"); lcd.print((int)setpointTemp); lcd.print(" SetH: "); lcd.print((int)setpointHum);
  } else {
    lcd.print("SetT: "); lcd.print((int)setpointTemp); lcd.print(" SetH:>"); lcd.print((int)setpointHum);
  }
}

void enviarSerial(float t, float h) {
  // Envia dados para o PC a cada 1 segundo
  if (millis() - delaySerial > 1000) {
    Serial.print("Estado: ");
    if (emEmergencia) Serial.print("EMERGENCIA | ");
    else if (sistemaAtivo) Serial.print("RODANDO | ");
    else Serial.print("PAUSADO | ");

    Serial.print("Temp Atual: "); Serial.print(t);
    Serial.print("C (Alvo: "); Serial.print(setpointTemp);
    
    Serial.print(") | Umid Atual: "); Serial.print(h);
    Serial.print("% (Alvo: "); Serial.print(setpointHum);
    Serial.println(")");
    
    delaySerial = millis();
  }
}

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

// --- VARIÁVEIS DE PROTEÇÃO ---
unsigned long ultimoTempoDesligado = 0;  // Marca a hora que o Peltier desligou
const unsigned long tempoSeguranca = 60000; // 60 segundos (1 minuto) de espera obrigatória
int ultimoModo = 0; // 0=Desligado, 1=Resfriando, 2=Aquecendo

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
  unsigned long agora = millis();
  
  // Definição do que o sistema QUER fazer agora
  int acaoDesejada = 0; // 0 = Ficar desligado (zona morta)
  
  if (temp > (setpointTemp + histereseTemp)) {
    acaoDesejada = 1; // Precisa RESFRIAR
  } 
  else if (temp < (setpointTemp - histereseTemp)) {
    acaoDesejada = 2; // Precisa AQUECER
  }

  // --- LÓGICA DE PROTEÇÃO ---
  
  // Se a ação desejada é igual ao que já está acontecendo, mantém.
  if (acaoDesejada == ultimoModo) {
    return; 
  }

  // Se queremos LIGAR algo (seja aquecer ou esfriar), verificamos o timer
  if (acaoDesejada != 0) {
    // Se passou pouco tempo desde que desligou, NÃO FAZ NADA (proteção)
    if (agora - ultimoTempoDesligado < tempoSeguranca) {
      // Opcional: Mostrar no Serial que está aguardando proteção
      Serial.println("Aguardando tempo de segurança do Peltier...");
      return; 
    }
    
    // Proteção extra: Se estava esfriando e quer aquecer (inversão direta), 
    // forçamos um desligamento primeiro se não tiver passado pelo estado 0.
    // Mas como a lógica acima exige passar pela histerese, naturalmente ele desliga antes.
  }

  // --- EXECUÇÃO ---

  if (acaoDesejada == 1) { // Ligar RESFRIAMENTO
    digitalWrite(PIN_RELE_PELTIER_A, HIGH); 
    digitalWrite(PIN_RELE_PELTIER_B, LOW);
    ultimoModo = 1;
  }
  else if (acaoDesejada == 2) { // Ligar AQUECIMENTO
    digitalWrite(PIN_RELE_PELTIER_A, LOW);
    digitalWrite(PIN_RELE_PELTIER_B, HIGH);
    ultimoModo = 2;
  }
  else { // DESLIGAR TUDO (acaoDesejada == 0)
    // Só atualizamos o timer se ele ESTAVA ligado antes
    if (ultimoModo != 0) {
      ultimoTempoDesligado = agora;
    }
    digitalWrite(PIN_RELE_PELTIER_A, HIGH); 
    digitalWrite(PIN_RELE_PELTIER_B, HIGH);
    ultimoModo = 0;
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

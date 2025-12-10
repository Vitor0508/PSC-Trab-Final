/*
 * PROJETO: Controle Peltier (Quente/Frio) + Umidade
 * VERSAO: Correção Botão Stop/Emergência (Umidade Travada)
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>

// --- DEFINIÇÃO DE PINOS ---
#define PIN_POT A0            
#define PIN_BTN_START 4      
#define PIN_CHAVE_SELETORA 3 
#define PIN_CHAVE_EMERG 2    
#define PIN_SENSOR_DHT 5      

#define PIN_RELE_PELTIER_A 7 
#define PIN_RELE_PELTIER_B 8 
#define PIN_RELE_UMIDADE 9    

// --- AJUSTE DE LÓGICA DOS RELÉS ---
#define RELE_LIGADO LOW
#define RELE_DESLIGADO HIGH

// Configuração
#define DHTTYPE DHT22
DHT dht(PIN_SENSOR_DHT, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 20, 4); 

// --- VARIÁVEIS DO SISTEMA ---
bool sistemaAtivo = false;       
bool emEmergencia = false;
bool ultimoEstadoBtnStart = HIGH; 
unsigned long lastDebounceTime = 0;
unsigned long delaySerial = 0;

// Setpoints
float setpointTemp = 25.0; 
float setpointHum = 60.0;  

// Zonas Mortas
float histereseTemp = 1.0; 
float histereseHum = 3.0;  

// --- VARIÁVEIS DE PROTEÇÃO ---
unsigned long ultimoTempoDesligado = 0;  
const unsigned long tempoSeguranca = 60000; // 60s
int ultimoModo = 0; // 0=Desligado, 1=Resfriando, 2=Aquecendo

void setup() {
  Serial.begin(9600);
  
  pinMode(PIN_BTN_START, INPUT_PULLUP);
  pinMode(PIN_CHAVE_SELETORA, INPUT_PULLUP);
  pinMode(PIN_CHAVE_EMERG, INPUT_PULLUP);
  
  pinMode(PIN_RELE_PELTIER_A, OUTPUT);
  pinMode(PIN_RELE_PELTIER_B, OUTPUT);
  pinMode(PIN_RELE_UMIDADE, OUTPUT);
  
  // Garante que comece tudo desligado
  digitalWrite(PIN_RELE_PELTIER_A, RELE_DESLIGADO);
  digitalWrite(PIN_RELE_PELTIER_B, RELE_DESLIGADO);
  digitalWrite(PIN_RELE_UMIDADE, LOW);
  
  // Timer inicial para permitir ligar logo
  ultimoTempoDesligado = millis() - tempoSeguranca; 

  dht.begin();
  lcd.init();
  lcd.backlight();
  
  Serial.println("--- INICIANDO SISTEMA ---");
}

void loop() {
  // 1. VERIFICA EMERGÊNCIA
  if (digitalRead(PIN_CHAVE_EMERG) == LOW) { 
    if (!emEmergencia) {
      Serial.println("!!! PARADA DE EMERGENCIA !!!");
      emEmergencia = true;
      sistemaAtivo = false;
    }
    pararTudo(); // Chama a função que agora força o desligamento de tudo
    lcd.setCursor(0,0); lcd.print("!! EMERGENCIA !!");
    lcd.setCursor(0,1); lcd.print("SISTEMA TRAVADO ");
    delay(100);
    return; 
  } else {
    if (emEmergencia) {
      lcd.clear();
      emEmergencia = false;
    }
  }

  // 2. BOTÃO START/PAUSE
  int leituraBtn = digitalRead(PIN_BTN_START);
  if (leituraBtn == LOW && ultimoEstadoBtnStart == HIGH && (millis() - lastDebounceTime) > 200) {
    sistemaAtivo = !sistemaAtivo;
    lastDebounceTime = millis();
    lcd.clear();
    
    if (!sistemaAtivo) {
       pararTudo(); 
       Serial.println("Status: SISTEMA PAUSADO PELO USUARIO");
    } else {
       Serial.println("Status: SISTEMA INICIADO");
    }
  }
  ultimoEstadoBtnStart = leituraBtn;

  // 3. LEITURA SENSOR/POT
  float tempAtual = dht.readTemperature();
  float humAtual = dht.readHumidity();

  if (isnan(tempAtual) || isnan(humAtual)) {
    lcd.setCursor(0,0); lcd.print("ERRO SENSOR DHT ");
    pararTudo();
    return;
  }

  int valorPot = analogRead(PIN_POT);
  bool ajustandoTemp = (digitalRead(PIN_CHAVE_SELETORA) == HIGH);
  
  // Range ajustado
  if (ajustandoTemp) {
      setpointTemp = map(valorPot, 0, 1023, 0, 70); 
  } else {
      setpointHum = map(valorPot, 0, 1023, 0, 100);
  }

  // 4. LÓGICA DE CONTROLE
  if (sistemaAtivo) {
    controlarTemperatura(tempAtual);
    controlarUmidade(humAtual);
  } else {
    // --- CORREÇÃO AQUI ---
    // Removemos o "if(ultimoModo != 0)". 
    // Se o sistema não está ativo, TEM que chamar pararTudo() para garantir 
    // que a umidade e os peltiers fiquem desligados.
    pararTudo(); 
  }

  // 5. ATUALIZAÇÃO VISUAL
  atualizarLCD(tempAtual, humAtual, ajustandoTemp);
  enviarSerial(tempAtual, humAtual);
}

// --- FUNÇÕES DE CONTROLE ---

void controlarTemperatura(float temp) {
  unsigned long agora = millis();
  int acaoDesejada = 0; // 0=Desligado
  
  if (temp > (setpointTemp + histereseTemp)) acaoDesejada = 1; // Resfriar
  else if (temp < (setpointTemp - histereseTemp)) acaoDesejada = 2; // Aquecer

  if (acaoDesejada == ultimoModo) return;

  // VERIFICAÇÃO DE SEGURANÇA (TIMER)
  if (acaoDesejada != 0) {
    if (agora - ultimoTempoDesligado < tempoSeguranca) {
      return; // Bloqueado pelo timer
    }
  }

  if (acaoDesejada == 1) { 
    // MODO RESFRIAR
    digitalWrite(PIN_RELE_PELTIER_A, RELE_DESLIGADO); 
    digitalWrite(PIN_RELE_PELTIER_B, RELE_LIGADO);
    ultimoModo = 1;
  }
  else if (acaoDesejada == 2) { 
    // MODO AQUECER
    digitalWrite(PIN_RELE_PELTIER_A, RELE_LIGADO);
    digitalWrite(PIN_RELE_PELTIER_B, RELE_DESLIGADO);
    ultimoModo = 2;
  }
  else { 
    pararTudo();
  }
}

void controlarUmidade(float hum) {
  // Só atua se o sistema estiver ativo (redundância de segurança)
  if (!sistemaAtivo) {
      digitalWrite(PIN_RELE_UMIDADE, LOW);
      return;
  }

  if (hum > (setpointHum - histereseHum)) {
    digitalWrite(PIN_RELE_UMIDADE, HIGH); 
  } 
  else if (hum < setpointHum) {
    digitalWrite(PIN_RELE_UMIDADE, LOW); 
  }
}

void pararTudo() {
  unsigned long agora = millis();
  
  // Só mexe no timer do Peltier se o Peltier estava ligado.
  // Isso evita reiniciar o timer se só a umidade estava ligada.
  if (ultimoModo != 0) {
    ultimoTempoDesligado = agora;
    ultimoModo = 0;
  }

  // --- FORÇA BRUTA: Desliga tudo incondicionalmente ---
  digitalWrite(PIN_RELE_PELTIER_A, RELE_DESLIGADO);
  digitalWrite(PIN_RELE_PELTIER_B, RELE_DESLIGADO);
  digitalWrite(PIN_RELE_UMIDADE, LOW); 
}

// --- VISUALIZAÇÃO ---

void atualizarLCD(float t, float h, bool modoTemp) {
  static unsigned long lcdTimer = 0;
  if (millis() - lcdTimer < 300) return;
  lcdTimer = millis();

  // Linha 1: Status
  lcd.setCursor(0, 0);
  if (sistemaAtivo) lcd.print("ON "); else lcd.print("OFF");
  
  // Sem decimais
  lcd.print(" T:"); lcd.print(t, 0); lcd.print("C U:"); lcd.print(h, 0); lcd.print("%");

  // Linha 2: Info / Timer
  lcd.setCursor(0, 1);
  
  long tempoPassado = millis() - ultimoTempoDesligado;
  long segundosRestantes = (tempoSeguranca - tempoPassado) / 1000;
  
  bool precisaAtuar = (t > setpointTemp + histereseTemp) || (t < setpointTemp - histereseTemp);
  bool bloqueado = (tempoPassado < tempoSeguranca);

  if (sistemaAtivo && bloqueado && precisaAtuar) {
    lcd.print("AGUARDE PROTECAO: "); 
    if(segundosRestantes < 10) lcd.print("0");
    lcd.print(segundosRestantes);
    lcd.print("s    "); 
  } 
  else if (sistemaAtivo && !bloqueado && precisaAtuar && ultimoModo == 0) {
     lcd.print("PREPARANDO...       ");
  }
  else {
    if (modoTemp) {
      lcd.print("SetT:>"); lcd.print(setpointTemp, 0); lcd.print(" SetU: "); lcd.print(setpointHum, 0);
    } else {
      lcd.print("SetT: "); lcd.print(setpointTemp, 0); lcd.print(" SetU:>"); lcd.print(setpointHum, 0);
    }
  }
}

void enviarSerial(float t, float h) {
  if (millis() - delaySerial > 1000) {
    Serial.print("Modo: "); Serial.print(ultimoModo);
    Serial.print(" | T: "); Serial.print(t, 1);
    Serial.print(" | U: "); Serial.println(h, 1);
    delaySerial = millis();
  }
}

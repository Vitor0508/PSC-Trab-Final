/*
 * PROJETO: Controle Peltier Hibrido (SCADA + Fisico)
 * VERSAO: Com Feedback do Rele de Umidade (Reg 6)
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <Modbusino.h>

ModbusinoSlave modbusino_slave(1);

// --- DEFINIÇÃO DE PINOS ---
#define PIN_POT A0            
#define PIN_BTN_START 4       
#define PIN_CHAVE_SELETORA 3 
#define PIN_CHAVE_EMERG 2    
#define PIN_SENSOR_DHT 5      

#define PIN_RELE_PELTIER_A 7 
#define PIN_RELE_PELTIER_B 8 
#define PIN_RELE_UMIDADE 9    

#define RELE_LIGADO LOW
#define RELE_DESLIGADO HIGH

#define DHTTYPE DHT22
DHT dht(PIN_SENSOR_DHT, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 20, 4); 

// --- VARIÁVEIS DO SISTEMA ---
bool sistemaAtivo = false;       
bool emergenciaVirtual = false; 
bool ultimoEstadoBtnStart = HIGH; 
unsigned long lastDebounceTime = 0;

// Setpoints
float setpointTemp = 25.0; 
float setpointHum = 60.0;  

// Zonas Mortas
float histereseTemp = 1.0; 
float histereseHum = 3.0;  

// Proteção
unsigned long ultimoTempoDesligado = 0;  
const unsigned long tempoSeguranca = 60000; 
int ultimoModo = 0; 

// Híbrido SCADA/POT
int ultimoEstadoChave = -1; 
bool modoSincronia = false; 
int ultimaLeituraPot = 0; 
const int limiarMovimento = 15; 

// Tabela Modbus [0..9]
uint16_t tab_reg[10];

void setup() {
  modbusino_slave.setup(9600);
  
  pinMode(PIN_BTN_START, INPUT_PULLUP);
  pinMode(PIN_CHAVE_SELETORA, INPUT_PULLUP);
  pinMode(PIN_CHAVE_EMERG, INPUT_PULLUP);
  
  pinMode(PIN_RELE_PELTIER_A, OUTPUT);
  pinMode(PIN_RELE_PELTIER_B, OUTPUT);
  pinMode(PIN_RELE_UMIDADE, OUTPUT);
  
  digitalWrite(PIN_RELE_PELTIER_A, RELE_DESLIGADO);
  digitalWrite(PIN_RELE_PELTIER_B, RELE_DESLIGADO);
  digitalWrite(PIN_RELE_UMIDADE, LOW);
  
  ultimoTempoDesligado = millis() - tempoSeguranca; 

  dht.begin();
  lcd.init();
  lcd.backlight();
  
  ultimoEstadoChave = digitalRead(PIN_CHAVE_SELETORA);
  ultimaLeituraPot = analogRead(PIN_POT); 
  
  tab_reg[2] = (uint16_t)(setpointTemp * 10);
  tab_reg[3] = (uint16_t)(setpointHum * 10);
}

void loop() {
  
  // 1. LEITURA SENSOR
  float tempAtual = dht.readTemperature();
  float humAtual = dht.readHumidity();
  if (isnan(tempAtual)) tempAtual = 0.0;
  if (isnan(humAtual)) humAtual = 0.0;

  // --- ATUALIZAÇÃO MODBUS ---
  tab_reg[0] = (uint16_t)(tempAtual * 10);      
  tab_reg[1] = (uint16_t)(humAtual * 10);       
  
  // Status Code (0=Off, 1=Resfria, 2=Aquece, 3=Standby, 4=EMERGENCIA)
  if (digitalRead(PIN_CHAVE_EMERG) == LOW || emergenciaVirtual) {
      tab_reg[4] = 4; 
  } else {
      tab_reg[4] = (uint16_t)(sistemaAtivo ? (ultimoModo == 0 ? 3 : ultimoModo) : 0); 
  }

  // --- NOVO: STATUS DO RELÉ DE UMIDADE ---
  // Lê o estado físico do pino 9. Se HIGH retorna 1, se LOW retorna 0.
  tab_reg[6] = digitalRead(PIN_RELE_UMIDADE); 

  modbusino_slave.loop(tab_reg, 10);

  // 2. COMANDOS SCADA
  switch(tab_reg[5]) {
    case 1: // LIGAR
      if (digitalRead(PIN_CHAVE_EMERG) == HIGH && !emergenciaVirtual) {
          sistemaAtivo = true;
      }
      tab_reg[5] = 0; 
      break;
    case 2: // DESLIGAR
      sistemaAtivo = false;
      pararTudo();
      tab_reg[5] = 0;
      break;
    case 3: // ACIONAR EMERGENCIA VIRTUAL
      emergenciaVirtual = true;
      tab_reg[5] = 0;
      break;
    case 4: // RESETAR EMERGENCIA VIRTUAL
      emergenciaVirtual = false;
      tab_reg[5] = 0;
      break;
  }

  // 3. EMERGÊNCIA UNIFICADA
  bool emergenciaFisica = (digitalRead(PIN_CHAVE_EMERG) == LOW);
  
  if (emergenciaFisica || emergenciaVirtual) {
      sistemaAtivo = false; 
      pararTudo();          
      
      lcd.setCursor(0,0); lcd.print("!! EMERGENCIA !!");
      lcd.setCursor(0,1);
      if (emergenciaFisica && emergenciaVirtual) lcd.print("TRAVA: FIS + VIRT");
      else if (emergenciaFisica) lcd.print("TRAVA: FISICA    ");
      else lcd.print("TRAVA: SCADA     ");

      return; 
  }

  // 4. BOTÃO START FÍSICO
  int leituraBtn = digitalRead(PIN_BTN_START);
  if (leituraBtn == LOW && ultimoEstadoBtnStart == HIGH && (millis() - lastDebounceTime) > 200) {
    sistemaAtivo = !sistemaAtivo;
    lastDebounceTime = millis();
    lcd.clear();
    if (!sistemaAtivo) pararTudo(); 
  }
  ultimoEstadoBtnStart = leituraBtn;

  if (isnan(tempAtual) || isnan(humAtual)) {
    lcd.setCursor(0,0); lcd.print("ERRO SENSOR DHT ");
    pararTudo();
    return;
  }

  // 5. SETPOINT HÍBRIDO
  float scadaTemp = (float)tab_reg[2] / 10.0;
  float scadaHum = (float)tab_reg[3] / 10.0;

  // SCADA vence
  if (abs(scadaTemp - setpointTemp) > 0.1) {
      setpointTemp = scadaTemp;
      modoSincronia = true; 
  }
  if (abs(scadaHum - setpointHum) > 0.1) {
      setpointHum = scadaHum; 
      modoSincronia = true;
  }

  // Potenciômetro
  int leituraChave = digitalRead(PIN_CHAVE_SELETORA);
  bool ajustandoTemp = (leituraChave == HIGH);
  int valorPot = analogRead(PIN_POT);
  
  if (leituraChave != ultimoEstadoChave) {
      modoSincronia = true; 
      ultimoEstadoChave = leituraChave;
      lcd.clear(); 
  }

  if (abs(valorPot - ultimaLeituraPot) > limiarMovimento) {
      ultimaLeituraPot = valorPot; 
      float valorPotMapeado;
      if (ajustandoTemp) valorPotMapeado = map(valorPot, 0, 1023, 0, 70); 
      else valorPotMapeado = map(valorPot, 0, 1023, 0, 100);

      if (modoSincronia) {
          float valorAlvo = ajustandoTemp ? setpointTemp : setpointHum;
          if (abs(valorPotMapeado - valorAlvo) < 2.0) modoSincronia = false; 
      } else {
          if (ajustandoTemp) {
              setpointTemp = valorPotMapeado;
              tab_reg[2] = (uint16_t)(setpointTemp * 10); 
          } else {
              setpointHum = valorPotMapeado;
              tab_reg[3] = (uint16_t)(setpointHum * 10); 
          }
      }
  } else {
      tab_reg[2] = (uint16_t)(setpointTemp * 10);
      tab_reg[3] = (uint16_t)(setpointHum * 10);
  }

  // 6. ATUAÇÃO
  if (sistemaAtivo) {
    controlarTemperatura(tempAtual);
    controlarUmidade(humAtual);
  } else {
    pararTudo(); 
  }

  atualizarLCD(tempAtual, humAtual, ajustandoTemp);
}

// --- FUNÇÕES AUXILIARES ---

void controlarTemperatura(float temp) {
  unsigned long agora = millis();
  int acaoDesejada = 0; 
  if (temp > (setpointTemp + histereseTemp)) acaoDesejada = 1; 
  else if (temp < (setpointTemp - histereseTemp)) acaoDesejada = 2; 

  if (acaoDesejada == ultimoModo) return;
  if (acaoDesejada != 0 && (agora - ultimoTempoDesligado < tempoSeguranca)) return; 

  if (acaoDesejada == 1) { 
    digitalWrite(PIN_RELE_PELTIER_A, RELE_DESLIGADO); 
    digitalWrite(PIN_RELE_PELTIER_B, RELE_LIGADO);
    ultimoModo = 1;
  }
  else if (acaoDesejada == 2) { 
    digitalWrite(PIN_RELE_PELTIER_A, RELE_LIGADO);
    digitalWrite(PIN_RELE_PELTIER_B, RELE_DESLIGADO);
    ultimoModo = 2;
  }
  else { 
    pararTudo();
  }
}

void controlarUmidade(float hum) {
  if (!sistemaAtivo) { digitalWrite(PIN_RELE_UMIDADE, LOW); return; }
  
  // LOGICA: Se Umidade Atual > Setpoint, LIGA o umidificador?
  // Normalmente, se for UMIDIFICADOR: Se Hum < Setpoint, LIGA.
  // Se for EXAUSTOR/DESUMIDIFICADOR: Se Hum > Setpoint, LIGA.
  // Pelo seu código original: hum > (set - hist) -> LIGA (HIGH).
  // Isso indica um comportamento de DESUMIDIFICAR ou ventilar.
  
  if (hum > (setpointHum - histereseHum)) digitalWrite(PIN_RELE_UMIDADE, HIGH); 
  else if (hum < setpointHum) digitalWrite(PIN_RELE_UMIDADE, LOW); 
}

void pararTudo() {
  unsigned long agora = millis();
  if (ultimoModo != 0) {
    ultimoTempoDesligado = agora;
    ultimoModo = 0;
  }
  digitalWrite(PIN_RELE_PELTIER_A, RELE_DESLIGADO);
  digitalWrite(PIN_RELE_PELTIER_B, RELE_DESLIGADO);
  digitalWrite(PIN_RELE_UMIDADE, LOW); 
}

void atualizarLCD(float t, float h, bool modoTemp) {
  static unsigned long lcdTimer = 0;
  if (millis() - lcdTimer < 250) return; 
  lcdTimer = millis();

  lcd.setCursor(0, 0);
  if (sistemaAtivo) lcd.print("ON  "); else lcd.print("OFF ");
  lcd.print("T:"); lcd.print(t, 0); lcd.print(" U:"); lcd.print(h, 0); 

  lcd.setCursor(0, 1);
  char indicador = modoSincronia ? '*' : '>';

  if (modoTemp) {
     lcd.print("SetT:"); lcd.print(indicador); lcd.print(setpointTemp, 0); 
     lcd.print(" SetU: "); lcd.print(setpointHum, 0);
  } else {
     lcd.print("SetT: "); lcd.print(setpointTemp, 0); 
     lcd.print(" SetU:"); lcd.print(indicador); lcd.print(setpointHum, 0);
  }
}

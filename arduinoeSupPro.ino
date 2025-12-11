/*
 * PROJETO: Controle Peltier Hibrido (SCADA + Fisico)
 * ATUALIZADO: Logica de prioridade para Setpoints e Comandos
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <Modbusino.h>

// ID do escravo Modbus = 1
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
bool emEmergencia = false;
bool ultimoEstadoBtnStart = HIGH; 
unsigned long lastDebounceTime = 0;

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

// --- VARIÁVEIS PARA LOGICA HÍBRIDA (SCADA vs POT) ---
int ultimoEstadoChave = -1; 
bool modoSincronia = false; 
int ultimaLeituraPot = 0; // Para detectar movimento físico
const int limiarMovimento = 15; // Sensibilidade para considerar que mexeu no pot

// Tabela Modbus (10 registradores)
// [0]=Temp, [1]=Umid, [2]=SetT, [3]=SetU, [4]=Status, [5]=Comando
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
  ultimaLeituraPot = analogRead(PIN_POT); // Inicia leitura anterior
  
  // Inicializa registradores com valores padrão para o SCADA não ler zero na largada
  tab_reg[2] = (uint16_t)(setpointTemp * 10);
  tab_reg[3] = (uint16_t)(setpointHum * 10);
}

void loop() {
  
  // 1. LEITURA SENSOR E COMUNICAÇÃO
  float tempAtual = dht.readTemperature();
  float humAtual = dht.readHumidity();

  if (isnan(tempAtual)) tempAtual = 0.0;
  if (isnan(humAtual)) humAtual = 0.0;

  // Atualiza Leituras para o SCADA (Registradores 0, 1 e 4)
  tab_reg[0] = (uint16_t)(tempAtual * 10);      
  tab_reg[1] = (uint16_t)(humAtual * 10);       
  tab_reg[4] = (uint16_t)(sistemaAtivo ? (ultimoModo == 0 ? 3 : ultimoModo) : 0); 
  // Status: 0=Off, 1=Resfria, 2=Aquece, 3=Ligado/Standby

  // --- PROCESSA MODBUS ---
  modbusino_slave.loop(tab_reg, 10);

  // --- 2. LÓGICA DE COMANDOS VIA SCADA (REGISTRADOR 5) ---
  // O Elipse escreve 1 para Ligar, 2 para Parar, 3 para Emergência
  switch(tab_reg[5]) {
    case 1: // Comando LIGAR
      if (!emEmergencia) sistemaAtivo = true;
      tab_reg[5] = 0; // Reseta comando para não ficar loopando
      break;
    case 2: // Comando DESLIGAR
      sistemaAtivo = false;
      pararTudo();
      tab_reg[5] = 0;
      break;
    case 3: // Comando EMERGENCIA SCADA
      if (!emEmergencia) {
         emEmergencia = true;
         sistemaAtivo = false;
         pararTudo();
      }
      tab_reg[5] = 0;
      break;
    case 4: // Reset Emergencia (Opcional via SCADA)
       emEmergencia = false;
       tab_reg[5] = 0;
       break;
    default:
      // Nada a fazer
      break;
  }

  // --- 3. VERIFICA EMERGÊNCIA FÍSICA ---
  if (digitalRead(PIN_CHAVE_EMERG) == LOW) { 
    if (!emEmergencia) {
      emEmergencia = true;
      sistemaAtivo = false;
    }
    pararTudo(); 
    lcd.setCursor(0,0); lcd.print("!! EMERGENCIA !!");
    lcd.setCursor(0,1); lcd.print("SISTEMA TRAVADO ");
    delay(100);
    return; 
  } else {
    // Se soltou o botão físico, mas não foi comando de SCADA, libera
    // Se quiser que o SCADA precise dar reset, remova o 'else' abaixo
    if (emEmergencia && tab_reg[5] != 3) { 
       // Verifica se foi apenas físico ou comando. Aqui simplificado: soltou botão, libera.
       emEmergencia = false; 
       lcd.clear();
    }
  }

  // --- 4. BOTÃO START FÍSICO ---
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

  // --- 5. LÓGICA DE SETPOINT HÍBRIDO (REGISTRADORES 2 e 3) ---
  
  // A) Verifica se o SCADA mandou valor novo (Modbus -> Arduino)
  float scadaTemp = (float)tab_reg[2] / 10.0;
  float scadaHum = (float)tab_reg[3] / 10.0;

  // Se o valor no registrador for diferente do setpoint atual, o SCADA mandou mudar
  if (abs(scadaTemp - setpointTemp) > 0.1) {
      setpointTemp = scadaTemp; // SCADA venceu
      // Atualiza o modoSincronia para evitar salto no potenciômetro
      modoSincronia = true; 
  }
  if (abs(scadaHum - setpointHum) > 0.1) {
      setpointHum = scadaHum; // SCADA venceu
      modoSincronia = true;
  }

  // B) Verifica Potenciômetro (Arduino -> Modbus)
  int leituraChave = digitalRead(PIN_CHAVE_SELETORA);
  bool ajustandoTemp = (leituraChave == HIGH);
  int valorPot = analogRead(PIN_POT);
  
  if (leituraChave != ultimoEstadoChave) {
      modoSincronia = true; 
      ultimoEstadoChave = leituraChave;
      lcd.clear(); 
  }

  // Só processa o potenciômetro se houver movimento significativo (Histerese de ruído)
  if (abs(valorPot - ultimaLeituraPot) > limiarMovimento) {
      
      ultimaLeituraPot = valorPot; // Atualiza referência
      
      float valorPotMapeado;
      if (ajustandoTemp) {
          valorPotMapeado = map(valorPot, 0, 1023, 0, 70); 
      } else {
          valorPotMapeado = map(valorPot, 0, 1023, 0, 100);
      }

      // Lógica de Soft Pickup (Evitar pulos bruscos)
      if (modoSincronia) {
          float valorAlvo = ajustandoTemp ? setpointTemp : setpointHum;
          if (abs(valorPotMapeado - valorAlvo) < 2.0) {
              modoSincronia = false; // Destravou, agora o pot controla
          }
      } else {
          // Potenciômetro assumiu o controle
          if (ajustandoTemp) {
              setpointTemp = valorPotMapeado;
              tab_reg[2] = (uint16_t)(setpointTemp * 10); // Atualiza SCADA
          } else {
              setpointHum = valorPotMapeado;
              tab_reg[3] = (uint16_t)(setpointHum * 10); // Atualiza SCADA
          }
      }
  } 
  // Se não mexeu no pot, mantemos o valor do tab_reg atualizado com a variável interna
  // Isso garante que se o SCADA mudou, o tab_reg não volta pro valor antigo do pot
  else {
      tab_reg[2] = (uint16_t)(setpointTemp * 10);
      tab_reg[3] = (uint16_t)(setpointHum * 10);
  }

  // --- 6. ATUAÇÃO ---
  if (sistemaAtivo) {
    controlarTemperatura(tempAtual);
    controlarUmidade(humAtual);
  } else {
    pararTudo(); 
  }

  atualizarLCD(tempAtual, humAtual, ajustandoTemp, map(valorPot, 0, 1023, ajustandoTemp?70:100, 0));
}

// --- MANTIVE AS FUNÇÕES AUXILIARES IGUAIS, APENAS COPIANDO O RESTANTE ---

void controlarTemperatura(float temp) {
  unsigned long agora = millis();
  int acaoDesejada = 0; 
  
  if (temp > (setpointTemp + histereseTemp)) acaoDesejada = 1; 
  else if (temp < (setpointTemp - histereseTemp)) acaoDesejada = 2; 

  if (acaoDesejada == ultimoModo) return;

  if (acaoDesejada != 0) {
    if (agora - ultimoTempoDesligado < tempoSeguranca) {
      return; 
    }
  }

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
  if (ultimoModo != 0) {
    ultimoTempoDesligado = agora;
    ultimoModo = 0;
  }
  digitalWrite(PIN_RELE_PELTIER_A, RELE_DESLIGADO);
  digitalWrite(PIN_RELE_PELTIER_B, RELE_DESLIGADO);
  digitalWrite(PIN_RELE_UMIDADE, LOW); 
}

void atualizarLCD(float t, float h, bool modoTemp, float potFantasma) {
  static unsigned long lcdTimer = 0;
  if (millis() - lcdTimer < 250) return; 
  lcdTimer = millis();

  if (modoSincronia) {
      lcd.setCursor(0, 0);
      lcd.print(">> SINCRONIA << ");
      lcd.setCursor(0, 1);
      lcd.print("Gire Pot -> Alvo");
      return; 
  }

  lcd.setCursor(0, 0);
  if (emEmergencia) lcd.print("EMG ");
  else if (sistemaAtivo) lcd.print("ON  "); 
  else lcd.print("OFF ");
  
  lcd.print("T:"); lcd.print(t, 0); lcd.print(" U:"); lcd.print(h, 0); 

  lcd.setCursor(0, 1);
  if (modoTemp) {
     lcd.print("SetT:>"); lcd.print(setpointTemp, 0); lcd.print(" SetU: "); lcd.print(setpointHum, 0);
  } else {
     lcd.print("SetT: "); lcd.print(setpointTemp, 0); lcd.print(" SetU:>"); lcd.print(setpointHum, 0);
  }
}

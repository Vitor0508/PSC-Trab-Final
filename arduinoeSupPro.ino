/*
 * PROJETO: Controle Peltier (Quente/Frio) + Umidade + SCADA (MODBUS)
 * VERSAO: Clean Serial (Sem conflito de porta)
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

// --- VARIÁVEIS PARA LOGICA DO POTENCIOMETRO ---
int ultimoEstadoChave = -1; 
bool modoSincronia = false; 

// Tabela Modbus (10 registradores)
uint16_t tab_reg[10];

void setup() {
  // Configura a Serial para Modbus (9600 baud)
  modbusino_slave.setup(9600);
  
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
  
  // Inicializa estado da chave
  ultimoEstadoChave = digitalRead(PIN_CHAVE_SELETORA);

  // Serial.println("--- INICIANDO SISTEMA ---"); // COMENTADO: CONFLITO MODBUS
}

void loop() {
  
  // 1. LEITURA SENSOR
  float tempAtual = dht.readTemperature();
  float humAtual = dht.readHumidity();

  // Se der erro no sensor, zera valores para segurança
  if (isnan(tempAtual)) tempAtual = 0.0;
  if (isnan(humAtual)) humAtual = 0.0;

  // --- ATUALIZAÇÃO TABELA MODBUS (SCADA) ---
  // Multiplicamos por 10 para enviar decimais como inteiro (Ex: 25.5 vira 255)
  tab_reg[0] = (uint16_t)(tempAtual * 10);      // Temp Real
  tab_reg[1] = (uint16_t)(humAtual * 10);       // Umid Real
  tab_reg[2] = (uint16_t)(setpointTemp * 10);   // Setpoint T
  tab_reg[3] = (uint16_t)(setpointHum * 10);    // Setpoint U
  tab_reg[4] = (uint16_t)ultimoModo;            // Status (0, 1 ou 2)
  
  // Processa requisições do Elipse
  modbusino_slave.loop(tab_reg, 10);

  // 2. VERIFICA EMERGÊNCIA
  if (digitalRead(PIN_CHAVE_EMERG) == LOW) { 
    if (!emEmergencia) {
      // Serial.println("!!! PARADA DE EMERGENCIA !!!"); // COMENTADO
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
      lcd.clear();
      emEmergencia = false;
    }
  }

  // 3. BOTÃO START/PAUSE
  int leituraBtn = digitalRead(PIN_BTN_START);
  if (leituraBtn == LOW && ultimoEstadoBtnStart == HIGH && (millis() - lastDebounceTime) > 200) {
    sistemaAtivo = !sistemaAtivo;
    lastDebounceTime = millis();
    lcd.clear();
    
    if (!sistemaAtivo) {
       pararTudo(); 
       // Serial.println("Status: SISTEMA PAUSADO"); // COMENTADO
    } else {
       // Serial.println("Status: SISTEMA INICIADO"); // COMENTADO
    }
  }
  ultimoEstadoBtnStart = leituraBtn;

  // VERIFICAÇÃO DE ERRO CRÍTICO SENSOR
  if (isnan(tempAtual) || isnan(humAtual)) {
    lcd.setCursor(0,0); lcd.print("ERRO SENSOR DHT ");
    pararTudo();
    return;
  }

  // --- LÓGICA SOFT PICKUP (POTENCIÔMETRO) ---
  int leituraChave = digitalRead(PIN_CHAVE_SELETORA);
  bool ajustandoTemp = (leituraChave == HIGH);
  int valorPot = analogRead(PIN_POT);
  
  if (leituraChave != ultimoEstadoChave) {
      modoSincronia = true; 
      ultimoEstadoChave = leituraChave;
      lcd.clear(); 
  }

  float valorPotMapeado;
  if (ajustandoTemp) {
      valorPotMapeado = map(valorPot, 0, 1023, 0, 70); 
  } else {
      valorPotMapeado = map(valorPot, 0, 1023, 0, 100);
  }

  if (modoSincronia) {
      float valorAlvo = ajustandoTemp ? setpointTemp : setpointHum;
      if (abs(valorPotMapeado - valorAlvo) < 2.0) {
          modoSincronia = false; 
      }
  } else {
      if (ajustandoTemp) {
          setpointTemp = valorPotMapeado;
      } else {
          setpointHum = valorPotMapeado;
      }
  }
  // --------------------------

  // 4. LÓGICA DE CONTROLE
  if (sistemaAtivo) {
    controlarTemperatura(tempAtual);
    controlarUmidade(humAtual);
  } else {
    pararTudo(); 
  }

  // 5. ATUALIZAÇÃO VISUAL (LCD)
  atualizarLCD(tempAtual, humAtual, ajustandoTemp, valorPotMapeado);
  
  // enviarSerial(tempAtual, humAtual); // COMENTADO: A função inteira foi desativada
}

// --- FUNÇÕES DE CONTROLE ---

void controlarTemperatura(float temp) {
  unsigned long agora = millis();
  int acaoDesejada = 0; // 0=Desligado
  
  if (temp > (setpointTemp + histereseTemp)) acaoDesejada = 1; // Resfriar
  else if (temp < (setpointTemp - histereseTemp)) acaoDesejada = 2; // Aquecer

  if (acaoDesejada == ultimoModo) return;

  // TIMER PROTEÇÃO
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

// --- VISUALIZAÇÃO ATUALIZADA ---

void atualizarLCD(float t, float h, bool modoTemp, float potFantasma) {
  static unsigned long lcdTimer = 0;
  if (millis() - lcdTimer < 250) return; 
  lcdTimer = millis();

  if (modoSincronia) {
      lcd.setCursor(0, 0);
      lcd.print(">> TRAVA DE SEGUR <<");
      lcd.setCursor(0, 1);
      lcd.print("Gire p/ "); 
      if(modoTemp) lcd.print(setpointTemp, 0); else lcd.print(setpointHum, 0);
      
      lcd.print(" (Pot:"); 
      lcd.print(potFantasma, 0); 
      lcd.print(")");
      return; 
  }

  lcd.setCursor(0, 0);
  if (sistemaAtivo) lcd.print("ON "); else lcd.print("OFF");
  
  lcd.print(" T:"); lcd.print(t, 0); lcd.print("C U:"); lcd.print(h, 0); lcd.print("%");

  lcd.setCursor(0, 1);
  
  long tempoPassado = millis() - ultimoTempoDesligado;
  long segundosRestantes = (tempoSeguranca - tempoPassado) / 1000;
  
  bool precisaAtuar = (t > setpointTemp + histereseTemp) || (t < setpointTemp - histereseTemp);
  bool bloqueado = (tempoPassado < tempoSeguranca);

  if (sistemaAtivo && bloqueado && precisaAtuar) {
    lcd.print("AGUARDE PROT: "); 
    if(segundosRestantes < 10) lcd.print("0");
    lcd.print(segundosRestantes);
    lcd.print("s   "); 
  } 
  else {
    if (modoTemp) {
      lcd.print("SetT:>"); lcd.print(setpointTemp, 0); lcd.print(" SetU: "); lcd.print(setpointHum, 0);
    } else {
      lcd.print("SetT: "); lcd.print(setpointTemp, 0); lcd.print(" SetU:>"); lcd.print(setpointHum, 0);
    }
  }
}

/*
void enviarSerial(float t, float h) {
  // FUNÇÃO DESATIVADA PARA NÃO GERAR CONFLITO COM SCADA
}
*/

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "TFLiteMicro_ArduinoESP32S3.h"
#include "model.h"

// ── PINES (Ya establecidos, no modificar :D ) ──────────────
const int PIN_SENSOR     = 12;
const int PIN_CALEFACTOR = 14;  // MOSFET IRLZ44N

// ── LÍMITES TEMPERATURA
const float TEMP_ON  = 37.0;    
const float TEMP_OFF = 40.0;    // 

// ── NORMALIZACIÓN ──────────────────────────────────────────
// ⚠️ Valores de la celda del notebook de Colab
const float TEMP_MIN_NORM = 18.00f;
const float TEMP_MAX_NORM = 50.00f;

// ── Config. librería TFLite ────────────────────────────────
const int   NUM_OPS    = 3;
const int   ARENA_SIZE = 8 * 1024;

// Función resolver: declara las operaciones que usa el modelo
tflite::MicroMutableOpResolver<NUM_OPS> crearResolver() {
  tflite::MicroMutableOpResolver<NUM_OPS> resolver;
  resolver.AddFullyConnected();
  resolver.AddRelu();
  resolver.AddLogistic();
  return resolver;
}

// ── Variables sensor
OneWire oneWire(PIN_SENSOR);
DallasTemperature sensors(&oneWire);

bool calefactorEncendido = false;
unsigned long ultimaLectura = 0;
const unsigned long INTERVALO_LECTURA = 30000; // 30 segundos
unsigned long startTime;

// ── CONFIGURACIÓN BLE (Perfil Nordic UART) ─────────────────
#define SERVICE_UUID           "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_UUID_RX "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  
#define CHARACTERISTIC_UUID_TX "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  

BLECharacteristic *pTxCharacteristic;
bool conectado = false;

// Callbacks para la conexión/desconexión del servidor
class MisCallbacksServidor : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) { conectado = true; }
  void onDisconnect(BLEServer* pServer) { conectado = false; pServer->startAdvertising(); }
};

// ── Cálculo que toma temp actual y la normaliza ────────────
float normalizarTemp(float temp) {
  return (temp - TEMP_MIN_NORM) / (TEMP_MAX_NORM - TEMP_MIN_NORM);
}

// ── Función IA ─────────────────────────────────────────────
int predecirIA(float temperatura, bool calefactorActual) {
  TFLMinput->data.f[0] = normalizarTemp(temperatura);
  TFLMinput->data.f[1] = calefactorActual ? 1.0f : 0.0f;

  bool ok = TFLMpredict();
  if (!ok) return -1;

  float prediccion = TFLMoutput->data.f[0];
  //Serial.print("[IA] Prediccion: ");
  //Serial.println(prediccion, 3);

  if (prediccion >= 0.70f) return 1;
  if (prediccion <= 0.30f) return 0;
  return calefactorActual ? 1 : 0;
}

void setup() {
  Serial.begin(115200);
  delay(100);

  sensors.begin();
  pinMode(PIN_CALEFACTOR, OUTPUT);
  digitalWrite(PIN_CALEFACTOR, LOW);
  startTime = millis();

  // ── Inicializar TFLite con la librería ───────────────────
  TFLMinterpreter = TFLMsetupModel<NUM_OPS, ARENA_SIZE>(
    model,
    crearResolver,  // función resolver definida arriba
    false           // true = imprime debug en Serial (ponlo false cuando todo funcione)
  );

  if (TFLMinterpreter == nullptr) {
    Serial.println("[IA] ERROR: no se pudo inicializar el modelo");
    while (true); // detiene el programa
  }

  Serial.println("[IA] Modelo cargado correctamente");
  Serial.print("[IA] Inputs esperados: ");
  Serial.println(TFLMinput->dims->data[1]); // debe imprimir 2

  // ── Inicializar Bluetooth BLE ────────────────────────────
  BLEDevice::init("ESP32_Termostato");
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MisCallbacksServidor());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());
  pService->start();
  BLEDevice::getAdvertising()->start();

  Serial.println("Sistema listo. Conectate desde la App.");
  Serial.println("tiempo_seg,temperatura_C,heater_state,fuente");
}

void loop() {
  if (millis() - ultimaLectura >= INTERVALO_LECTURA) {
    ultimaLectura = millis();

    sensors.requestTemperatures();
    float temperatura = sensors.getTempCByIndex(0);
    unsigned long tiempoSeg = (millis() - startTime) / 1000;
    String mensajeClaro = "";
    
    // ── ESCENARIO 1: SENSOR NO CONECTADO / ERROR ───────────
    if (temperatura < -100.0 || temperatura == 85.0 || temperatura == -127.0) {
      // Por seguridad, si el sensor falla apagamos el calefactor
      digitalWrite(PIN_CALEFACTOR, LOW);
      calefactorEncendido = false;

      // Mensaje amigable para la App (BLE)
      String mensajeClaro = "tiempo:" + String(tiempoSeg) + "s, temp:ERR, estado:APAGADO\n";

      // Salida CSV (Monitor Serie)
      Serial.print(tiempoSeg);
      Serial.println(",ERROR,0,ERROR");

      // Enviar por Bluetooth
      if (conectado) {
        pTxCharacteristic->setValue(mensajeClaro.c_str());
        pTxCharacteristic->notify();
      }
      return;
    }

    // ── ESCENARIO 2: SENSOR LEYENDO BIEN ───────────────────
    
    bool debeEncender;
    String fuente;

    int resultadoIA = predecirIA(temperatura, calefactorEncendido);

    if (resultadoIA != -1) {
      debeEncender = (resultadoIA == 1);
      fuente = "IA";
    } else {
      // Fallo del modelo → control de respaldo por reglas
      if (!calefactorEncendido && temperatura < TEMP_ON) {
        debeEncender = true;
      } else if (calefactorEncendido && temperatura > TEMP_OFF) {
        debeEncender = false;
      } else {
        debeEncender = calefactorEncendido;
      }
      fuente = "REGLA";
    }

    if (debeEncender != calefactorEncendido) {
      calefactorEncendido = debeEncender;
      digitalWrite(PIN_CALEFACTOR, calefactorEncendido ? HIGH : LOW);
    }

    // ── Salida CSV (Monitor Serie) ─────────────────────────
    Serial.print(tiempoSeg);
    Serial.print(",");
    Serial.print(temperatura);
    Serial.print(",");
    Serial.print(calefactorEncendido ? "1" : "0");
    Serial.print(",");
    Serial.println(fuente);

    // ── Mensaje amigable + envío por Bluetooth (Celular) ───
    String estadoTexto = calefactorEncendido ? "ENCENDIDO" : "APAGADO";
    String mensajeClaro = "tiempo:" + String(tiempoSeg) + "s, temp:" + String(temperatura) +
                          "C, estado:" + estadoTexto + ", fuente:" + fuente + "\n";

    if (conectado) {
      pTxCharacteristic->setValue(mensajeClaro.c_str());
      pTxCharacteristic->notify();
    }
  }
}

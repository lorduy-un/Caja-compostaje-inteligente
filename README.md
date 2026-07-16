# Compostaje de PLA

## Autoras
* Linda Orduy
* Paola Bello

## Descripción del proyecto
Sistema autónomo de monitoreo y control de temperatura para una caja de pruebas de compostaje de PLA, con el objetivo de mantener el entorno entre 58 °C y 70 °C. Por un lado, el núcleo de nuestro sistema embebido es un microcontrolador ESP32‑S3‑WROOM‑1, que lee un sensor de temperatura DS18B20 y controla un actuador, una resistencia calefactora de fibra de carbono. 
Para controlar la potencia de la resistencia se empleo un MOSFET IRLZ44N que actua como un ON/OFF, alimentado a través de un convertidor DC‑DC elevador que eleva los 12 V de una fuente a 35 V, obteniendo una potencia de calefacción de aproximadamente 15 W. Nuestra diseño de tarjeta integra el circuito de potencia por lo que a esta se conecta el sensor y la resistencia mientras que el elevador se encuentra de forma externa.

## Diseño PCB 
Desafios de desarollo, componentes (link compra, footprint) 
| Referencia | Cantidad | Descripcion | Footprint |
|------------|----------|-------------|-----------|
| Raspberry Pi Zero 2 W | — |

### Esquematico 
Esquematico tarjeta kicad y para conexcion con elevador y resistencia 

### Desafios de diseño
Se obtuvieron diversos diseños debido a errores que se fueron presentaron o actualizaciones a implementar en el diseño
* Inicialmnete, la primer PCB diseñada presentó el siguiente error: islas en la zona de cobre de tierra (GND) que dejaban partes del circuito eléctricamente desconectadas. A partir de aca, se empezo a probar en protoboard de manera funcional para avanzar paralelamente tanto en el diseño como en el codigo y toma de datos.
* SOlder mask, prototipado

## Consideraciones importantes resistencia calefactora
Para esta entrega, se tiene una resistencia de 2.4 metros de longitud, lo primero que se hizo fue tomar sus extremos y medir la resistencia con un multimetro para proceder con los siguientes calculos

**Resistencia por metro**<br />
$$R_{\text{por metro}} = \frac{79.1\ \Omega}{2.385\ \text{m}} = 33.165\ \Omega/\text{m}$$

El fabricante nos da un limite de potencia por metro, asi que calculamos la potencia total para la fuente que queremos implementar
**Potencia a 35 V**<br />
$$P = \frac{V^2}{R} = \frac{34^2}{79.1} = \frac{1156}{79.1} \approx 14.614\ \text{W}$$

Y para saber si esta potencia es segura, hacemos el siguiente calculo. De igual forma, conociendo este limite fisico de 25W/m, bajamos un poco el umbral a 21W/m y procedemos tambien a calcular la tension max que podemos aplicar para no exceder la potencia que soporta.

**Potencia máxima segura por metro (límite del fabricante)**<br />
$$P_{\text{máx total}} = 25\ \text{W/m} \times 2.385\ \text{m} = 59.625\ \text{W}$$
Por seguridad, limitamos a **50 W**.
$$P_{\text{por metro}} = \frac{50\ \text{W}}{2.385\ \text{m}} \approx 20.96\ \text{W/m}$$

**Voltaje necesario para 21 W por metro (referencia de diseño)**<br />
$$V = \sqrt{P \cdot R} = \sqrt{21 \cdot 79.1} \approx 40\ \text{a}\ 45\ \text{V}$$


| Parámetro        | Valor       |
|------------------|-------------|
| Resistencia total | 79.1 Ω      |
| Longitud         | 2.4 m    |
| Resistencia/metro| 33.2 Ω/m |
| Potencia máx. recomendada por el fabricante| 25 W/m |
| Potencia máx. segura total (calculada) | 50 W |

## Entrenamiento de IA
Inicialmente, para la recoleccion de los datos, el ESP32 envía por puerto serie las lecturas de temperatura y el estado del calefactor luego un script de Python en la computadora captura esas líneas y las guarda automáticamente en archivos .csv, con columnas de tiempo, temperatura y estado del calefactor (1=ON, 0=OFF). Se generaron dos conjuntos de datos: uno solo con el sensor en temperatura ambiente y otro con el actuador funcionando solo en un rango determinado: encender debajo de 37 °C, apagar sobre 40 °C. Aunque el objetivo en un comienzo era manejar un umbral de 60-70 ºC, para esta primera entrega por limitaciones de tension de la fuente y longitud de la resitencia calefactora no se alcanzo esta temperatura, asi que para la otra entrega se va a optar por una resistencia mas larga y una fuente de tension mas alta.
Volviendo al entrenamiento, ambos conjuntos de datos fueron combinados para aumentar la cantidad de ejemplos de entrenamiento, tras limpiarlos de valores nulos y atípicos (como la medición dentro de la nevera) ya con los datos limpios se procedio a definir * Entradas: Temperatura y estado actual del calefactor
* Label de salida: etiqueta_futura del calefactor (1=ON, 0=OFF)
Acto seguido se procedio a aplicar un normalizacion a los datos de temperatura, debido a que las redes neuronales aprenden ajustando números muy pequeños llamados pesos y ese proceso de ajuste funciona mal cuando los datos de entrada tienen escalas muy diferentes o muy grandes; si le pasas al modelo temperaturas crudas como 28.0 o 55.0, esos números dominan el cálculo y el entrenamiento se vuelve inestable o muy lento. Con valores entre 0 y 1 todos los inputs están en la misma escala y la red aprende de forma mucho más eficiente.
Entonces se define un dato de temperatura minimo como 0 y un maximo como 1 y lo que pasa a partir de esto es que se reescala cualquier número al rango [0, 1]

IMAGEN VISUALIZACION DE DATOS

Con los datos recopilados se entrenó un modelo de red neuronal en Google Colab utilizando TensorFlow, el modelo es de clasificación binaria: predice si el calefactor debe estar encendido o apagado basándose en los últimos 10 valores de temperatura. La arquitectura empleada es una red secuencial con dos capas ocultas de 16 neuronas, activación ReLU y una salida sigmoide, tambien se implementó una división: 60 % entrenamiento, 20 % validación, 20 % prueba. Finalmenten, el modelo entrenado se convirtió a TensorFlow Lite y se generó el archivo modelo_calefactor.h, listo para ser integrado en el firmware del ESP32 mediante la librería 

IMAGEN DIVISION DATOS

Con esto claro, el programa al arrancar inicializa el sensor DS18B20 y el pin del calefactor se configura como salida y se pone en LOW para garantizar que la resistencia calefactora empieza apagada. Al mismo tiempo, el modelo de inteligencia artificial se carga en memoria usando TFLMsetupModel, que toma el array de bytes del archivo model.h
Una vez en loop(), el programa no usa delay() sino que revisa constantemente si han pasado 30 segundos desde la última lectura comparando millis() con ultimaLectura. Cuando ese tiempo se cumple, le pide la temperatura al sensor y valida que no sea -127°C, que es el código de error del DS18B20 cuando hay un problema de conexión y si la lectura es válida, el programa pasa a la fase de decisión.
La fase de decisión tiene dos caminos. 
* El primero es la inteligencia artificial: la función predecirIA toma la temperatura actual y el estado actual del calefactor, normaliza los datos de temperatura, los escribe en las dos ranuras de entrada del modelo y llama a TFLMpredict(). El modelo ejecuta sus cálculos internos y deposita un número entre 0 y 1 en la ranura de salida, si ese número es mayor o igual a 0.5 el modelo está diciendo "en el próximo minuto la temperatura va a necesitar calefactor", y la función retorna 1. Por otro lado, si es menor retorna 0.
* El segundo camino se activa solo si el modelo falla, es decir si el metodo anterior retorna un error. En ese caso el sistema cae al control clásico: si la temperatura bajó de 37°C enciende, si subió de 40°C apaga, y si está entre los dos valores mantiene el estado que tenía. Este respaldo garantiza que el sistema nunca se queda sin control aunque haya un problema con el modelo.

IAMGEN ENTRENAMIENTO 

## Conclusiones
* FSDFS



#include <Arduino.h>

#include <pines.h>
#include <dispositivos.h>
#include <adc.h>
#include <lmic.h>
#include <hal\hal.h>
#include <RocketScream_LowPowerAVRZero.h>
#include <CayenneLPP.h>

#define DEBUG

#define MINUTOS_ENVIO 1 // Cada X minutos realizamos un envío.

unsigned long tiempo;
int lectura, hora, minutos, segundos, milisegundos;
volatile byte contador;

// Tamaño de los datos enviados a Cayenne.
// Cada sensor añade al payload 2 bytes + los bytes de sus datos.
CayenneLPP lpp(4 + 4); // Térmometro + Lectura de batería

// Declaración de funciones.
void wakeUp(void);
void pulsador(void);
void do_send(osjob_t *j);

// Hay un envío en curso, no poner el microcontrolador a dormir.
volatile boolean envioEnCurso = true;

// Pines del ATmega4808 no utilizados en la aplicación
// se ponen aquí para reducir el consumo de corriente.
#if defined(DEBUG)
const uint8_t unusedPins[] = {PIN_PD0, PIN_PD1, PIN_PD2, PIN_PD3, PIN_PD4, PIN_PD5, PIN_PD7, PIN_PA1, PIN_PA2, PIN_PA3};
#else
const uint8_t unusedPins[] = {PIN_PD0, PIN_PD1, PIN_PD2, PIN_PD3, PIN_PD4, PIN_PD5, PIN_PD7, PIN_PA1, PIN_PA2, PIN_PA3, PIN_PF0, PIN_PF1};
#endif

// Device EUI. Al copiar los datos de la consola de Helium hay que indicarle que lo haga en formato "lsb"
static const u1_t PROGMEM DEVEUI[8] = {DEVEUI_DISPOSITIVO_01};
void os_getDevEui(u1_t *buf) { memcpy_P(buf, DEVEUI, 8); }

// App EUI. Al copiar los datos de la consola de Helium hay que indicarle que lo haga en formato "lsb"
static const u1_t PROGMEM APPEUI[8] = {APPEUI_DISPOSITIVO_01};
void os_getArtEui(u1_t *buf) { memcpy_P(buf, APPEUI, 8); }

// App Key. Al copiar los datos de la consola de Helium hay que indicarle que lo haga en formato "msb"
static const u1_t PROGMEM APPKEY[16] = {APPKEY_DISPOSITIVO_01};
void os_getDevKey(u1_t *buf) { memcpy_P(buf, APPKEY, 16); }

static osjob_t sendjob;

// Al compilar aparece un aviso de que no tiene el pinmap de la placa y que debemos utilizar un propio.
// Configuramos el pinmap de la placa.
const lmic_pinmap lmic_pins = {
    .nss = RFM95_SS,
    .rxtx = LMIC_UNUSED_PIN,
    .rst = RFM95_RST,
    .dio = {RFM95_DIO0, RFM95_DIO1, LMIC_UNUSED_PIN},
};

// Imprime información en hexadecimal en el monitor serial al producirse el evento "Joined"
// (Se ha unido correctamente)
void printHex2(unsigned v)
{
    v &= 0xff;
    if (v < 16)
        Serial2.print('0');
    Serial2.print(v, HEX);
}

// Función declarada en la libreria lmic. Se ejecuta cada vez que se produce un evento.
// Aquí podemos poner código de control.
void onEvent(ev_t ev)
{
#if defined(DEBUG)
    Serial2.print(os_getTime());
    Serial2.print(": ");
#endif
    switch (ev)
    {
    case EV_SCAN_TIMEOUT:
#if defined(DEBUG)
        Serial2.println(F("EV_SCAN_TIMEOUT"));
#endif
        break;
    case EV_BEACON_FOUND:
#if defined(DEBUG)
        Serial2.println(F("EV_BEACON_FOUND"));
#endif
        break;
    case EV_BEACON_MISSED:
#if defined(DEBUG)
        Serial2.println(F("EV_BEACON_MISSED"));
#endif
        break;
    case EV_BEACON_TRACKED:
#if defined(DEBUG)
        Serial2.println(F("EV_BEACON_TRACKED"));
#endif
        break;
    case EV_JOINING:
#if defined(DEBUG)
        Serial2.println(F("EV_JOINING"));
#endif
        break;
    case EV_JOINED:
#if defined(DEBUG)
        Serial2.println(F("EV_JOINED"));
        // #endif
        {
            u4_t netid = 0;
            devaddr_t devaddr = 0;
            u1_t nwkKey[16];
            u1_t artKey[16];
            LMIC_getSessionKeys(&netid, &devaddr, nwkKey, artKey);
        }
#endif
        // Deshabilitar la validación de comprobación de enlaces (habilitada automáticamente durante la unión,
        //  pero no es compatible con TTN en este momento).
        LMIC_setLinkCheckMode(0); // 0
        break;
    case EV_RFU1:
#if defined(DEBUG)
        Serial2.println(F("EV_RFU1"));
#endif
        break;
    case EV_JOIN_FAILED:
#if defined(DEBUG)
        Serial2.println(F("EV_JOIN_FAILED"));
#endif
        break;
    case EV_REJOIN_FAILED:
#if defined(DEBUG)
        Serial2.println(F("EV_REJOIN_FAILED"));
#endif
        break;
    case EV_TXCOMPLETE:
#if defined(DEBUG)
        Serial2.println(F("EV_TXCOMPLETE (includes waiting for RX windows)"));
        if (LMIC.txrxFlags & TXRX_ACK)
            Serial2.println(F("Received ack"));
        if (LMIC.dataLen)
        {
            Serial2.println(F("Received "));
            Serial2.println(LMIC.dataLen);
            Serial2.println(F(" bytes of payload"));
        }
#endif
        // Programar la próxima transmisión.
        // os_setTimedCallback(&sendjob, os_getTime()+sec2osticks(TX_INTERVAL), do_send);
        envioEnCurso = false; // Finaliza el envío.
        break;
    case EV_LOST_TSYNC:
#if defined(DEBUG)
        Serial2.println(F("EV_LOST_TSYNC"));
#endif
        break;
    case EV_RESET:
#if defined(DEBUG)
        Serial2.println(F("EV_RESET"));
#endif
        break;
    case EV_RXCOMPLETE:
#if defined(DEBUG)
        // data received in ping slot
        Serial2.println(F("EV_RXCOMPLETE"));
#endif
        break;
    case EV_LINK_DEAD:
#if defined(DEBUG)
        Serial2.println(F("EV_LINK_DEAD"));
#endif
        break;
    case EV_LINK_ALIVE:
#if defined(DEBUG)
        Serial2.println(F("EV_LINK_ALIVE"));
#endif
        break;
    case EV_TXSTART:
#if defined(DEBUG)
        Serial2.println(F("EV_TXSTART"));
#endif
        break;
    case EV_TXCANCELED:
#if defined(DEBUG)
        Serial2.println(F("EV_TXCANCELED"));
#endif
        break;
    case EV_RXSTART:
        /* No utilizar el monitor serial en este evento. */
        break;
    case EV_JOIN_TXCOMPLETE:
#if defined(DEBUG)
        Serial2.println(F("EV_JOIN_TXCOMPLETE: no JoinAccept"));
#endif
        break;
    default:
#if defined(DEBUG)
        Serial2.println(F("Unknown event"));
        Serial2.println((unsigned)ev);
#endif
        break;
    }
}

// Lanza la transmisión de los datos.
void do_send(osjob_t *j)
{
    // Comprueba si hay un trabajo TX/RX actual en ejecución
    if (LMIC.opmode & OP_TXRXPEND)
    {
#if defined(DEBUG)
        Serial2.println(F("OP_TXRXPEND, not sending"));
#endif
    }
    else
    {
        // Prepara el envío de datos en el próximo momento posible.
        // Enviamos los datos en el formato de Cayenne.
        lpp.reset();
        lpp.addAnalogInput(2, ADC_BateriaLeerVoltaje() / 1000.F); // Batería en Voltios.
        lpp.addAnalogInput(1, 19.1);                              // Añadimos el sensor de temperatura.
        LMIC_setTxData2(1, lpp.getBuffer(), lpp.getSize(), 0);
#if defined(DEBUG)
        Serial2.println(F("Packet queued"));
#endif
    }
}

void setup()
{
    // put your setup code here, to run once:
    // Reduce el consumo de los pines que no se van a usar en el ATmega4808
    uint8_t index;
    for (index = 0; index < sizeof(unusedPins); index++)
    {
        pinMode(unusedPins[index], INPUT_PULLUP);
        LowPower.disablePinISC(unusedPins[index]);
    }

// Configuramos la interrupción en el ATmega4808 que produce el pin de Wake del TPL5010
#if defined(VERSION_1)
    attachInterrupt(digitalPinToInterrupt(TPL5010_WAKE), wakeUp, CHANGE);
#else
    attachInterrupt(digitalPinToInterrupt(TPL5010_WAKE), wakeUp, RISING);
#endif

#if defined(DEBUG)
    Serial2.begin(115200);
    Serial2.println(F("Starting"));
#endif

    // Inicializamos el ADC.
    ADC_BateriaInicializarADC();
    delay(1000); // Espera inicialización ADC

    // LMIC init
    os_init();

    // Restablece el estado MAC. Se descartarán las transferencias de datos de sesión y pendientes.
    LMIC_reset();

    // allow much more clock error than the X/1000 default. See:
    // https://github.com/mcci-catena/arduino-lorawan/issues/74#issuecomment-462171974
    // https://github.com/mcci-catena/arduino-lmic/commit/42da75b56#diff-16d75524a9920f5d043fe731a27cf85aL633
    // the X/1000 means an error rate of 0.1%; the above issue discusses using
    // values up to 10%. so, values from 10 (10% error, the most lax) to 1000
    // (0.1% error, the most strict) can be used.
    LMIC_setClockError(MAX_CLOCK_ERROR * 10 / 100);

    // Sub-band 2 - Helium Network
    LMIC_setLinkCheckMode(0);

    // Spread Factor y potencia de emisión de la radio.
    LMIC_setDrTxpow(DR_SF7, 14);

    // Iniciar un trabajo (el envío también inicia automáticamente OTAA)
    do_send(&sendjob);

    // Configuración de pines.
    pinMode(LED_BUILTIN, OUTPUT);  // LED
    pinMode(TPL5010_WAKE, INPUT);  // TPL5010 WAKE
    pinMode(TPL5010_DONE, OUTPUT); // TPL5010 DONE

    // Generamos el pulso de DONE del TPL5010
    // El TPL5010 comienza a funcionar.
    digitalWrite(TPL5010_DONE, HIGH);
    delay(1);
    digitalWrite(TPL5010_DONE, LOW);

    // Parpadeo al inicio o en el Reset
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);
    digitalWrite(LED_BUILTIN, LOW);
}

void loop()
{
    // put your main code here, to run repeatedly:
    // Realiza el envío en curso si lo hay.
    os_runloop_once();
    // Ponemos el microcontrolador a dormir.
    if (!envioEnCurso)
    {
#if defined(DEBUG)
        Serial2.println(F("Microcontrolador a dormir\n"));
        delay(500);
#endif
        // Si el micro se pone a dormir antes de que termine la comunicación
        // serie, mirar si afecta al consumo de corriente cuando duerme.
        // comparar consumo con delay de 200
        LowPower.powerDown();
// Se despierta aquí después de atender a la interrupción del timer.
#if defined(DEBUG)
        Serial2.println(F("Microcontrolador despierto"));
        tiempo = millis();
        milisegundos = tiempo % 1000;
        tiempo /= 1000;
        segundos = tiempo % 60;
        tiempo /= 60;
        minutos = tiempo % 60;
        tiempo /= 60;
        hora = tiempo;

        Serial2.print(++contador);
        Serial2.print(" | ");
        Serial2.printf("%02u:%02u:%02u.%03u", hora, minutos, segundos, milisegundos);
        Serial2.print(" | ");
        Serial2.println();
#endif
    }
}

// TIC Timer R=18k -> 36s, R=20k -> 60s.
void wakeUp(void)
{
    // Generamos la señal de Done para el timer.
    digitalWrite(TPL5010_DONE, HIGH);
    // Añadimos un delay para el tiempo de señal en alto de DONE del TPL5010.
    for (char i = 0; i < 5; i++)
    {
    } // Ancho de pulso mínimo 100 ns. 8.5 us con i < 3.
    digitalWrite(TPL5010_DONE, LOW);

    // Variable para enviar cada x minutos.
    static unsigned char contador = MINUTOS_ENVIO - 1;
    contador++; // Un incremento de esta variable es igual a 1 minuto.
    if (contador == MINUTOS_ENVIO)
    {
        contador = 0;
        // Preparamos un envío a Helium.
        envioEnCurso = true; // Se pone en false cuando se finaliza el envío.
        os_setCallback(&sendjob, do_send);
    }
}

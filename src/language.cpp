#include "language.h"

namespace {
uint8_t g_lang = lang::EN;

// [language][string], same order as lang::Str. nullptr = use English. Keep Spanish about as short as English: sprites have fixed widths.
const char* const EN_T[lang::S_COUNT] = {
  /* SETUP_L1 */ "Continue",                      // "Continue your setup" screen: headline line 1 (X36 bold, column 272 px)
  /* SETUP_L2 */ "your setup",                    // headline line 2
  /* SETUP_WAIT */ "on the CubiNav Connect app",  // third line (S19); the id keeps its old name
  /* DISCONNECTED */ "DISCONNECTED",
  /* CONN_LOST */ "Connection to phone lost.",
  /* SEARCHING */ "SEARCHING",
  /* NEXT_TRIP */ "Your next trip starts in the app.",
  /* CONNECTED */ "CONNECTED",
  /* PICK_DEST */ "Pick a destination to start.",
  /* ARRIVED */ "Arrived",
  /* REROUTING */ "Calculating a new route",
  /* REROUTE_BIG */ "Rerouting",
  /* START */ "START",
  /* PAIR_TAG */ "PAIRING",
  /* PAIR_LABEL */ "PAIRING CODE",
  /* PAIR_H1 */ "Enter this code on your phone",
  /* PAIR_H2 */ "The code is new every time you connect.",
  /* NO_GPS */ "NO GPS SIGNAL",
  /* FW_UPDATE */ "FIRMWARE UPDATE",
  /* UPD_FAILED */ "Update failed",
  /* FW_KEPT */ "Old firmware kept. Try again in the app.",
  /* KEEP_OPEN */ "Keep the app open. Do not power off.",
  /* UPD_CANCELLED */ "Update cancelled.",
  /* RESTARTING */ "Restarting...",
  /* VERIFYING */ "Verifying...",
  /* BOOT_LOADING */ "Loading interface...",
  /* BOOT_BATTERY */ "Checking battery...",
  /* BOOT_BT */ "Starting Bluetooth...",
  /* BOOT_READY */ "Ready",
  /* SU_TAG */ "SETUP",
  /* SU_L1 */ "GET THE APP",
  /* SU_L2 */ "PICK THIS DEVICE",
  /* SU_L3 */ "ENTER THE CODE",
  /* SU_H1 */ "Tap Add device in the app.",
  /* SU_H2 */ "It will show up on this screen.",
  /* DIR0 */ "N",
  /* DIR1 */ "NE",
  /* DIR2 */ "E",
  /* DIR3 */ "SE",
  /* DIR4 */ "S",
  /* DIR5 */ "SW",
  /* DIR6 */ "W",
  /* DIR7 */ "NW",
  /* OTA_E_STATE */ "Device busy",
  /* OTA_E_SIZE */ "Image too large",
  /* OTA_E_GAP */ "Transfer out of sync",
  /* OTA_E_FLASH */ "Flash write error",
  /* OTA_E_MD5 */ "Checksum mismatch",
  /* OTA_E_BATTERY */ "Battery too low",
  /* OTA_E_TIMEOUT */ "Phone stopped sending",
  /* OTA_E_ARGS */ "Bad request",
  /* OTA_E_ABORTED */ "Connection lost",
  /* OTA_E_NOT_SECURE */ "Pairing required",
  /* OTA_E_IMAGE */ "Not a valid firmware image",
  /* OTA_E_UNKNOWN */ "Unknown error"
};
const char* const ES_T[lang::S_COUNT] = {
  /* SETUP_L1 */ "Continua",
  /* SETUP_L2 */ "tu configuracion",
  /* SETUP_WAIT */ "en la app CubiNav Connect",
  /* DISCONNECTED */ "DESCONECTADO",
  /* CONN_LOST */ "Sin conexion con el telefono.",
  /* SEARCHING */ "BUSCANDO",
  /* NEXT_TRIP */ "Tu proximo viaje empieza en la app.",
  /* CONNECTED */ "CONECTADO",
  /* PICK_DEST */ "Elige un destino para empezar.",
  /* ARRIVED */ "Llegaste",
  /* REROUTING */ "Calculando nueva ruta",
  /* REROUTE_BIG */ "Recalculando",
  /* START */ "INICIO",
  /* PAIR_TAG */ "VINCULANDO",
  /* PAIR_LABEL */ "CODIGO DE VINCULACION",
  /* PAIR_H1 */ "Ingresa este codigo en tu telefono",
  /* PAIR_H2 */ "El codigo cambia en cada conexion.",
  /* NO_GPS */ "SIN SENAL GPS",
  /* FW_UPDATE */ "ACTUALIZACION DE FIRMWARE",
  /* UPD_FAILED */ "Error al actualizar",
  /* FW_KEPT */ "Se conserva el firmware anterior. Reintenta en la app.",
  /* KEEP_OPEN */ "Deja la app abierta. No apagues el equipo.",
  /* UPD_CANCELLED */ "Actualizacion cancelada.",
  /* RESTARTING */ "Reiniciando...",
  /* VERIFYING */ "Verificando...",
  /* BOOT_LOADING */ "Cargando interfaz...",
  /* BOOT_BATTERY */ "Comprobando bateria...",
  /* BOOT_BT */ "Iniciando Bluetooth...",
  /* BOOT_READY */ "Listo",
  /* SU_TAG */ "CONFIG",
  /* SU_L1 */ "OBTEN LA APP",
  /* SU_L2 */ "ELIGE ESTE EQUIPO",
  /* SU_L3 */ "INGRESA EL CODIGO",
  /* SU_H1 */ "Toca Agregar dispositivo en la app.",
  /* SU_H2 */ "Aparecera en esta pantalla.",
  /* DIR0 */ "N",
  /* DIR1 */ "NE",
  /* DIR2 */ "E",
  /* DIR3 */ "SE",
  /* DIR4 */ "S",
  /* DIR5 */ "SO",
  /* DIR6 */ "O",
  /* DIR7 */ "NO",
  /* OTA_E_STATE */ "Equipo ocupado",
  /* OTA_E_SIZE */ "Imagen demasiado grande",
  /* OTA_E_GAP */ "Transferencia desincronizada",
  /* OTA_E_FLASH */ "Error al escribir en flash",
  /* OTA_E_MD5 */ "Suma de control invalida",
  /* OTA_E_BATTERY */ "Bateria demasiado baja",
  /* OTA_E_TIMEOUT */ "El telefono dejo de enviar",
  /* OTA_E_ARGS */ "Solicitud invalida",
  /* OTA_E_ABORTED */ "Conexion perdida",
  /* OTA_E_NOT_SECURE */ "Se requiere vincular",
  /* OTA_E_IMAGE */ "Firmware no valido",
  /* OTA_E_UNKNOWN */ "Error desconocido"
};
const char* const* const TABLES[lang::COUNT] = { EN_T, ES_T, nullptr, nullptr, nullptr };   // PT, FR, DE: not translated yet -> English
}  // namespace

namespace lang {
void    set(uint8_t id) { g_lang = valid(id) ? id : EN; }
uint8_t get()           { return g_lang; }
const char* tr(Str s) {
  if (s >= S_COUNT) return "";
  const char* const* t = TABLES[g_lang];
  const char* v = t ? t[s] : nullptr;
  return v ? v : EN_T[s];
}
const char* cardinal(int i) { return tr((Str)(S_DIR0 + (i & 7))); }
}  // namespace lang
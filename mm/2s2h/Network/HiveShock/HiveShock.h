#ifndef HIVESHOCK_H
#define HIVESHOCK_H

#include "ultra64.h"

struct Actor;

#ifdef __cplusplus
#include <string>

// HiveShock: TCP command server for the HiveShock companion app.
// Protocol: one JSON object per line on 0.0.0.0:<Port> (default 43000); 127.0.0.1 works on the same PC.
class HiveShock {
  public:
    static HiveShock* Instance;

    // Enemy counters for this session (only enemies: fairies and bombchus are not counted).
    struct Stats {
        int total = 0;        // received so far (waiting + spawned)
        int spawned = 0;      // have entered the game
        int defeated = 0;     // spawned and no longer alive
        int alive = 0;        // alive right now
        int waiting = 0;      // still in the queue
        int eliteBank = 0;    // elite points in use inside the elite bank
        int eliteBankMax = 0; // size of the elite bank (0 = no bank)
        int eliteWaiting = 0; // elite enemies still in the queue
    };
    Stats GetStats() const;
    // Starts counting from now: what is alive or waiting stays counted, the rest is forgotten.
    void ResetStats();

    void Enable();
    void Disable();
    bool IsEnabled() const;
    bool IsListening() const;
    bool HasClient() const;
    // "a.b.c.d:port" of the LAN interface (or 127.0.0.1 when unknown).
    std::string GetListenEndpoint() const;
};
#endif // __cplusplus

#ifdef __cplusplus
extern "C" {
#endif

// Invencible: los golpes ocurren, pero Health_ChangeBy no baja HP.
// ApplyInstaKill escribe health=0 directo y no pasa por aquí.
int HiveShock_ShouldBlockHealthLoss(void);

// colorRgba8 = Color_RGBA8*. 1 si se sustituye el tinte de buff.
int HiveShock_OverrideColorFilter(struct Actor* actor, void* colorRgba8);

// Endpoint para HiveShock (app / LAN). Escribe "a.b.c.d:43000" o "127.0.0.1:43000".
// Devuelve el puerto. out puede ser NULL si solo quieres el puerto.
int HiveShock_GetListenEndpoint(char* out, int outSize);

// Estado en vivo del presupuesto de entidades HiveShock. Cualquier puntero puede ser NULL.
// used = carga viva actual, max = presupuesto actual (CVar), queued = regalos esperando cupo.
void HiveShock_GetCapacityStatus(int* used, int* max, int* queued);

#ifdef __cplusplus
}
#endif

#endif // HIVESHOCK_H

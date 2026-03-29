#ifndef COMMON_H
#define COMMON_H

#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    MOVE_DOWN = 0,
    MOVE_LEFT = 1,
    MOVE_UP = 2,
    MOVE_RIGHT = 3
} MoveDirection;

typedef struct {
    uint8_t direction;               // Dirección solicitada por el jugador
} MoveRequest;

typedef struct {
    char name[16];                    // Nombre del jugador
    unsigned int score;               // Puntaje
    unsigned int invalid_mov;         // Solicitudes de movimientos inválidas
    unsigned int valid_mov;           // Solicitudes de movimientos válidas
    unsigned short x, y;              // Coordenadas en el tablero
    pid_t pid;                        // Identificador de proceso
    bool blocked;                     // Si el jugador está bloqueado
} Player;

typedef struct {
    unsigned short width;              // Ancho del tablero
    unsigned short height;             // Alto del tablero
    unsigned char numPlayers;          // Cantidad de jugadores
    Player players[9];                 // Lista de jugadores
    bool finished;                     // Si el juego se ha terminado
    char board[];                      // Arreglo que representa el tablero
} GameState;

typedef struct {
    sem_t canPrint;                   // Máster → Vista: hay cambios por imprimir
    sem_t completedPrint;             // Vista → Máster: terminó de imprimir
    // La sincronización entre el máster y la vista involucra los semáforos canPrint y completedPrint 
    // siguiendo un patrón simple de señalización bidireccional.

    sem_t mutexWriter;               // Mutex: previene inanición del máster
    sem_t mutexStatus;               // Mutex: protege el estado del juego
    sem_t mutexReaders;              // Mutex: protege el contador de lectores
    unsigned int playersReading;     // Cantidad de jugadores leyendo el estado
    // La sincronización entre el máster y los jugadores involucra los
    // semáforos mutexWriter, mutexStatus y mutexReaders y la variable playersReading

    sem_t allowed_Mov[9];            // Semáforo individual para cada jugador
    // Cada jugador, deberá esperar a que el máster procese su movimiento para poder
    // enviar otro, lo cual se coordina mediante un semáforo único para cada jugador en el arreglo allowed_Mov.
} Sync;

#endif

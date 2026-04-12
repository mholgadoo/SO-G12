# ChompChamps — TP1: Inter Process Communication

Trabajo Práctico Nº 1 de Sistemas Operativos (ITBA). Implementación del juego multijugador **ChompChamps** utilizando mecanismos de IPC de POSIX: memoria compartida, semáforos anónimos y pipes anónimos.

---

## Compilación y ejecución

### Requisito previo: imagen Docker de la cátedra

El desarrollo, compilación y ejecución deben realizarse dentro del contenedor provisto:

```bash
docker pull agodio/itba-so-multiarch:3.1
docker run -it --rm -v "$(pwd)":/app -w /app agodio/itba-so-multiarch:3.1
```

### Compilación

Dentro del contenedor, desde la raíz del repositorio:

```bash
make
```

Esto genera los binarios `master`, `vista` y `jugador` en el directorio raíz.

Para limpiar los binarios:

```bash
make clean
```

### Ejecución

El punto de entrada es el proceso `master`. Los parámetros entre corchetes son opcionales:

```bash
./master [-w width] [-h height] [-d delay] [-t timeout] [-s seed] [-v vista] -p jugador1 [jugador2 ...]
```

**Parámetros:**

| Parámetro | Descripción | Default / Mínimo |
|-----------|-------------|-----------------|
| `-w width` | Ancho del tablero | Default y mínimo: 10 |
| `-h height` | Alto del tablero | Default y mínimo: 10 |
| `-d delay` | Milisegundos de espera entre impresiones del estado | Default: 200 |
| `-t timeout` | Timeout en segundos sin movimientos válidos para finalizar | Default: 10 |
| `-s seed` | Semilla para la generación del tablero | Default: `time(NULL)` |
| `-v view` | Ruta al binario de la vista | Default: sin vista |
| `-p player1 [player2...]` | Ruta/s a los binarios de los jugadores (mínimo 1, máximo 9) | — |

**Ejemplo con un jugador y vista:**

```bash
./master -w 20 -h 15 -d 100 -t 30 -v ./vista -p ./jugador
```

**Ejemplo con múltiples jugadores y sin vista:**

```bash
./master -p ./jugador ./jugador ./jugador
```

**Ejemplo usando el master provisto por la cátedra:**

```bash
./ChompChamps -v ./vista -p ./jugador ./jugador
```

### Rutas relativas para el torneo

- **Vista:** `./vista`
- **Jugador:** `./jugador`

---

## Decisiones de diseño

### Arquitectura general

El sistema está compuesto por tres procesos independientes que se comunican mediante IPC POSIX:

- **Master:** coordina el juego, valida movimientos y gestiona el estado compartido.
- **Vista:** lee el estado compartido y lo imprime en terminal.
- **Jugador:** lee el estado compartido y envía solicitudes de movimiento al master mediante un pipe anónimo.

### Memoria compartida

Se utilizan dos memorias compartidas:

- `/game_state`: contiene el estado del juego (tablero, jugadores, puntajes, posiciones, etc.).
- `/game_sync`: contiene los semáforos de sincronización entre procesos.

### Sincronización

Se implementó el problema clásico de **lectores y escritores** con **prevención de inanición del escritor** (master). El esquema de semáforos es:

- `mutexWriter` (C): evita la inanición del master al escribir — bloquea el ingreso de nuevos lectores.
- `mutexStatus` (D): mutex del estado del juego — garantiza acceso exclusivo al escritor.
- `mutexReaders` (E): protege el contador de lectores activos.
- `playersReading` (F): cantidad de jugadores actualmente leyendo el estado.
- `viewReady` (A): el master notifica a la vista que hay cambios por imprimir.
- `viewDone` (B): la vista notifica al master que terminó de imprimir.
- `playerReady[9]` (G): el master notifica a cada jugador que su movimiento fue procesado y puede enviar el siguiente.

### Atención de jugadores (round-robin)

El master atiende las solicitudes de movimiento usando `select(2)` sobre los pipes de todos los jugadores. Mantiene un índice rotativo para recorrer los jugadores con solicitudes pendientes en orden circular, evitando sesgo sistemático hacia algún jugador en particular.

### Estrategia del jugador

El jugador implementa una estrategia automática. Consulta el estado del tablero de forma sincronizada y elige el movimiento a enviar. El comportamiento base es aleatorio entre las 8 direcciones posibles.

### Distribución inicial de jugadores

El master distribuye los jugadores de forma determinística en el tablero, garantizando un margen de movimiento similar para cada uno.

---

## Limitaciones

- El jugador implementa una estrategia principalmente aleatoria; no realiza planificación de caminos.
- La vista imprime el estado en texto plano por terminal, sin interfaz gráfica avanzada.
- No se implementan los chequeos de consistencia del struct de sincronización (solo presentes en el master provisto).

---

## Problemas encontrados y soluciones

**Inanición del master (escritor):** En la primera versión, el master no podía escribir porque los jugadores leían el estado continuamente, manteniendo el mutex tomado indefinidamente. Se resolvió incorporando el semáforo `mutexWriter` que bloquea el ingreso de nuevos lectores en cuanto el master quiere escribir, implementando así la variante del problema lectores-escritores que previene la inanición del escritor.

---

## Citas de fragmentos de código / Uso de IA

- La estructura del problema de lectores-escritores fue consultada en el material teórico de la cátedra y en *Operating Systems: Three Easy Pieces* (Arpaci-Dusseau).
- Se utilizó IA (Claude) como asistente para completar este README a partir del enunciado del TP.

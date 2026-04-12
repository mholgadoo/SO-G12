# ChompChamps — TP1: Inter Process Communication

Trabajo Práctico Nº 1 de Sistemas Operativos (ITBA). Implementación del juego multijugador **ChompChamps** utilizando mecanismos de IPC de POSIX: memoria compartida, semáforos anónimos y pipes anónimos.

---

### Compilación y ejecución

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

### Decisiones de diseño

- Para evitar que un proceso acorralado sature el sistema enviando movimientos inválidos en un bucle infinito, se dotó al proceso Jugador de una evaluación de entorno espacial. Antes de emitir un movimiento, el jugador escanea sus celdas adyacentes (matriz de 3x3). Si determina que no existen rutas válidas , el proceso finaliza su ejecución de forma voluntaria, cerrando su descriptor de archivo. Esto genera un End of File (EOF) en el pipe, lo que permite al proceso Máster detectar la rendición instantáneamente (read() == 0) y marcar al jugador como bloqueado.

- Para prevenir interbloqueos causados por algoritmos deficientes o procesos colgados, el Máster monitorea la lectura de los pipes mediante la llamada al sistema select(). Si el temporizador de select() expira antes de recibir una solicitud de movimiento, se asume inactividad severa y el jugador es descalificado y bloqueado automáticamente.

- Adicionalmente, el Máster utiliza la función difftime() para medir el tiempo real transcurrido desde el último movimiento válido registrado en todo el tablero. La ejecución del bucle principal del juego se interrumpe de forma absoluta bajo dos condiciones concurrentes: cuando el arreglo completo de jugadores alcanza el estado de bloqueado, o cuando se supera el tiempo máximo de inactividad global sin progreso en el tablero.

### Arquitectura general

El sistema está compuesto por tres procesos independientes que se comunican mediante IPC POSIX:

- **Master:** Proceso orquestador. Inicializa IPCs, genera procesos hijos, rutea la comunicación vía pipes y dicta el avance de los turnos.
- **Vista:** Proceso monitor. Consume el estado mediante un esquema de semáforos binarios sincronizados con el Máster.
- **Jugador:** Proceso cliente. Aplica lógica espacial de detección de encierro y emite solicitudes aleatorias hacia el Máster.
- **Common.h** Contiene las estructuras de datos, el layout de la memoria compartida (GameState y Sync) y enumeradores compartidos por todos los módulos.

### Memoria compartida

Se utilizan dos memorias compartidas:

- `/game_state`: Contiene el estado del juego (tablero, jugadores, puntajes, posiciones, etc.).
- `/game_sync`: Contiene los semáforos de sincronización entre procesos.

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

### Limitaciones

- El jugador implementa una estrategia principalmente aleatoria; no realiza planificación de caminos.
- La vista imprime el estado en texto plano por terminal, sin interfaz gráfica avanzada.
- No se implementan los chequeos de consistencia del struct de sincronización (solo presentes en el master provisto).

---

### Gestión de Recursos

- Al finalizar la partida (sea por victoria o por timeout global), el Máster se asegura de:

1. Destrabar a los jugadores restantes mediante sem_post(&sync->allowed_Mov[i]) para que puedan salir de   sus bucles.

2. Esperar la finalización de la Vista y de todos los Jugadores capturando sus estados de salida mediante wait().

3. Destruir todos los semáforos POSIX (sem_destroy).

4. Desmapear la memoria (munmap) y desenlazar los objetos de memoria compartida (shm_unlink) para no dejar basura en /dev/shm.

---

### Problemas encontrados y soluciones

**Inanición del master (escritor):** En la primera versión, el master no podía escribir porque los jugadores leían el estado continuamente, manteniendo el mutex tomado indefinidamente. Se resolvió incorporando el semáforo `mutexWriter` que bloquea el ingreso de nuevos lectores en cuanto el master quiere escribir, implementando así la variante del problema lectores-escritores que previene la inanición del escritor.

---

### Citas de fragmentos de código / Uso de IA

- La estructura del problema de lectores-escritores fue consultada en el material teórico de la cátedra.

- En lugar de enviar el descriptor de archivo (File Descriptor) del extremo de escritura del pipe como un argumento por línea de comandos (lo cual expone la implementación interna y acopla los procesos), el proceso Máster realiza una redirección antes de ejecutar la llamada execv. La transición hacia este modelo de comunicación fue el resultado de una sesión de revisión de código asistida por Inteligencia Artificial (Gemini). La justificación arquitectónica detrás de este cambio se basó en el siguiente concepto analizado durante el desarrollo: "El uso de dup2 no es una forma de pasar el parámetro, sino de estandarizar la salida. Al hacer dup2(pipe_escritura, 1), se desconecta lógicamente el flujo hacia la terminal y se conecta, en su lugar, el flujo hacia el Pipe del Máster. Esto hace que el jugador no necesite saber por qué número de pipe habla; simplemente escribe en su salida estándar." De esta manera, se garantiza la compatibilidad universal del binario del jugador con cualquier proceso maestro que respete el estándar de capturar la salida estándar (FD 1), tal como lo requiere el enunciado del proyecto.

- durante el desarrollo se noto que que los jugadores con IDs mayores a 1 no eran reconocidos por la vista y no podían capturar celdas. Esto ocurrió porque en procesadores ARM (a diferencia de x86), el tipo char es unsigned por defecto. Al guardar un ID negativo (ej. -1), se generaba un underflow guardando el valor 255. La solución fue forzar el casteo explícito a signed char al momento de leer el tablero tanto en master.c como en jugador.c y vista.c, garantizando la portabilidad del código en cualquier hardware.



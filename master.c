#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <sys/mman.h>
#include "common.h" 
#include <sys/wait.h>

static void aplicar_movimiento(GameState *state, Sync *sync, int player_index, MoveDirection dir) {
    Player *p = &state->players[player_index];
    int new_x = (int)p->x;
    int new_y = (int)p->y;

    switch (dir) {
        case MOVE_RIGHT: new_x++; break;
        case MOVE_LEFT:  new_x--; break;
        case MOVE_UP:    new_y--; break;
        case MOVE_DOWN:  new_y++; break;
    }

    bool dentro = (new_x >= 0 && new_x < (int)state->width && new_y >= 0 && new_y < (int)state->height);
    // Leemos qué hay en la celda destino
    char cell_dest = 0;
    if (dentro) {
        cell_dest = state->board[new_y * state->width + new_x];
    }
    // Una celda es "pisable" si está libre (0) o si tiene una recompensa (no es un jugador del 1 al 9)
    bool celda_libre = dentro && (cell_dest < 1 || cell_dest > 9);

    // FIX race condition: antes solo haciamos sem_wait(mutexStatus) directo,
    // pero eso esta incompleto. El protocolo correcto del escritor es:
    //   1. tomar mutexWriter primero -> bloquea nuevos lectores que quieran entrar
    //   2. recien ahi esperar mutexStatus -> espera que los lectores activos terminen
    // Sin el paso 1, si los jugadores leen continuamente el master nunca puede
    // escribir porque siempre hay alguien con mutexStatus (inanicion del escritor).
    if (sem_wait(&sync->mutexWriter) == -1) {
        perror("Error en sem_wait de mutexWriter (escritor)");
        return;
    }
    if (sem_wait(&sync->mutexStatus) == -1) {
        perror("Error en sem_wait de mutexStatus");
        sem_post(&sync->mutexWriter); // rollback si falla
        return;
    }

    // Si corresponde sumamos el puntaje de la recompensa
    if (dentro && celda_libre) {
        if (cell_dest != 0) {
            switch(cell_dest) {
                case '&': p->score += 1; break;
                case '@': p->score += 3; break;
                case '%': p->score += 5; break;
                case '#': p->score += 7; break;
                case '$': p->score += 9; break;
            }
        }

        state->board[p->y * state->width + p->x] = 0; // Vaciamos celda vieja
        p->x = (unsigned short)new_x;
        p->y = (unsigned short)new_y;
        state->board[new_y * state->width + new_x] = (char)(player_index + 1); // Llenamos nueva
        p->valid_mov++;
    } else {
        p->invalid_mov++;
    }

    // liberamos en orden inverso al que tomamos
    if (sem_post(&sync->mutexStatus) == -1) {
        perror("Error en sem_post de mutexStatus");
    }
    if (sem_post(&sync->mutexWriter) == -1) {
        perror("Error en sem_post de mutexWriter (escritor)");
    }
}

int main(int argc, char *argv[]) {
    // ### RECEPCION Y PROCESAMIENTO DE PARÁMETROS ### // 
    // 1. Variables de configuración con sus valores por defecto
    int width = 10;                 // Mínimo y default: 10
    int height = 10;                // Mínimo y default: 10
    int delay = 200;                // Default: 200 ms
    int timeout = 10;               // Default: 10 segundos
    int seed = time(NULL);          // Default: time(NULL)
    char *view_path = NULL;         // Default: Sin vista
    char *player_paths[9];          // Máximo 9 jugadores
    int num_players = 0;            // Contador de jugadores ingresados

    int opt;
    // 2. Bucle de getopt. Los dos puntos ':' indican que la bandera requiere un valor.
    while ((opt = getopt(argc, argv, "w:h:d:t:s:v:p:")) != -1) {
        switch (opt) {
            case 'w':
                width = atoi(optarg);
                if (width < 10) width = 10; // Validación de mínimo
                break;
            case 'h':
                height = atoi(optarg);
                if (height < 10) height = 10; // Validación de mínimo
                break;
            case 'd':
                delay = atoi(optarg);
                break;
            case 't':
                timeout = atoi(optarg);
                break;
            case 's':
                seed = atoi(optarg);
                break;
            case 'v':
                view_path = optarg;
                break;
            case 'p':
                // Primero guardamos el argumento que getopt ya capturó en optarg
                if (num_players < 9) {
                    player_paths[num_players++] = optarg;
                }
                // Revisamos manualmente los siguientes argumentos hasta encontrar otra bandera ('-')
                // Si no hacemos este while, solo se capturará el primer jugador y los demás serán ignorados por getopt
                while (optind < argc && argv[optind][0] != '-') {
                    if (num_players < 9) {
                        player_paths[num_players++] = argv[optind];
                    }
                    optind++; // Avanzamos el índice manualmente
                }
                break;
            default:
                fprintf(stderr, "Uso incorrecto de parámetros.\n");
                exit(EXIT_FAILURE);
        }
    }

    // 3. Validación de jugarores
    if (num_players == 0) {
        fprintf(stderr, "Error: Se requiere al menos un jugador.\n");
        exit(EXIT_FAILURE);
    }

    // Solo para comprobar que todo cargo bien 
    srand((unsigned int)seed); // <-- AGREGAR ESTO PARA QUE SEAN DISTINTAS PARTIDAS
    printf("Master iniciado con Tablero %dx%d, Delay: %d, Timeout: %d, Semilla: %d\n", 
           width, height, delay, timeout, seed);
    printf("Jugadores detectados: %d\n", num_players);

    // ### LÓGICA DE MEMORIA ### // 

    // 1. Limpieza preventiva de recursos compartidos anteriores
    shm_unlink("/game_state");
    shm_unlink("/game_sync");

    // 2. Cálculo de tamaños
    size_t board_bytes = width * height * sizeof(char);
    size_t state_size = sizeof(GameState) + board_bytes;  // Sumamos el tamaño estático del struct más el tamaño dinámico del tablero
    size_t sync_size = sizeof(Sync);

    // CREACIÓN DE MEMORIA COMPARTIDA: /game_state //
    int fd_state = shm_open("/game_state", O_CREAT | O_EXCL | O_RDWR, 0666); // Creamos la memoria compartida
    if (fd_state == -1) {
        perror("Error creando /game_state");
        exit(EXIT_FAILURE);
    }

    if (ftruncate(fd_state, state_size) == -1) { // Ajustamos el tamaño de la memoria compartida
        perror("Error en ftruncate de /game_state");
        exit(EXIT_FAILURE);
    }

    GameState *state = mmap(NULL, state_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_state, 0); // Mapeamos la memoria compartida a nuestro espacio de direcciones
    if (state == MAP_FAILED) {
        perror("Error en mmap de /game_state");
        exit(EXIT_FAILURE);
    }
    // Una vez mapeada la memoria, podemos cerrar el descriptor de archivo ya que no lo vamos a necesitar más 
    // El espacio de memoria seguirá existiendo y lo vamos a poder acceder a través del puntero state
    close(fd_state); 

    
    // CREACIÓN DE MEMORIA COMPARTIDA: /game_sync // 
    int fd_sync = shm_open("/game_sync", O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd_sync == -1) {
        perror("Error creando /game_sync");
        exit(EXIT_FAILURE);
    }

    if (ftruncate(fd_sync, sync_size) == -1) {
        perror("Error en ftruncate de /game_sync");
        exit(EXIT_FAILURE);
    }

    Sync *sync = mmap(NULL, sync_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_sync, 0);
    if (sync == MAP_FAILED) {
        perror("Error en mmap de /game_sync");
        exit(EXIT_FAILURE);
    }
    close(fd_sync);

    // ### INICIALIZACIÓN DE DATOS Y SINCRONIZACIÓN ### //
    
    // Inicializamos las variables estáticas del juego
    state->width = width;
    state->height = height;
    state->numPlayers = num_players;
    state->finished = false;
    // Limpiamos el tablero 
    memset(state->board, 0, board_bytes);
    // Limpiamos el array de jugadores
    memset(state->players, 0, sizeof(state->players));

    // Inicializamos los jugadores
    for (int i = 0; i < num_players; i++) {
        Player *player = &state->players[i];
        // Obtenemos el nombre del jugador
        const char *base_name = strrchr(player_paths[i], '/');

        if (base_name != NULL) {
            // Si el nombre del jugador tiene una barra, lo ignoramos
            base_name++;
        } else {
            base_name = player_paths[i];
        }
        // Guardamos el nombre del jugador
        snprintf(player->name, sizeof(player->name), "%s", base_name);
        // Inicializamos las variables del jugador
        player->score = 0;
        player->invalid_mov = 0;
        player->valid_mov = 0;
        // Inicializamos las coordenadas del jugador
        /* ponemos en width y height porque el tablero va de 0 a width-1 y de 0 a height-1 entonces
        *  esto significa que el jugador no tiene una posicion asignada todavia
        */
        //player->x = (unsigned short)width;
        //player->y = (unsigned short)height;
        
        // Generamos posiciones aleatorias hasta encontrar una celda vacía (0)
        unsigned short rand_x, rand_y;
        do {
            rand_x = (unsigned short)(rand() % width);
            rand_y = (unsigned short)(rand() % height);
        } while (state->board[rand_y * state->width + rand_x] != 0);

        player->x = rand_x;
        player->y = rand_y;
        // Marcamos la celda como ocupada (1, 2 o 3...)
        state->board[player->y * state->width + player->x] = (char)(i + 1);

        player->pid = 0;
        player->blocked = false;
    }

    // ### SEMBRADO DE RECOMPENSAS ### // 
    char tipos_recompensas[] = {'&', '@', '%', '#', '$'};
    int cantidad_por_tipo = 10; // Vamos a poner 10 de cada una (50 en total)

    for (int r = 0; r < 5; r++) {
        for (int k = 0; k < cantidad_por_tipo; k++) {
            unsigned short rand_x, rand_y;
            // Buscamos una celda que esté completamente vacía (0)
            do {
                rand_x = (unsigned short)(rand() % width);
                rand_y = (unsigned short)(rand() % height);
            } while (state->board[rand_y * state->width + rand_x] != 0);

            // Plantamos la recompensa
            state->board[rand_y * state->width + rand_x] = tipos_recompensas[r];
        }
    }

    // ### INICIALIZACIÓN DE SEMÁFOROS ### //

    // Master <-> Vista: 
    // arrancan en 0 (bloqueados) porque nadie tiene que imprimir todavia
    // el master hace post(canPrint) para avisar, la vista hace post(completedPrint) para confirmar
    if (sem_init(&sync->canPrint, 1, 0) == -1) {
        perror("Error en sem_init de canPrint");
        exit(EXIT_FAILURE);
    }
    if (sem_init(&sync->completedPrint, 1, 0) == -1) {
        perror("Error en sem_init de completedPrint");
        exit(EXIT_FAILURE);
    }

    // Master <-> Jugadores: patron lectores-escritores con molinete
    // arrancan en 1 (libres) porque son mutexes binarios
    // mutexWriter = molinete para evitar que el master se muera de hambre
    // mutexStatus = protege el GameState (lo toma el primer lector, lo suelta el ultimo)
    // mutexReaders = protege solo el contador playersReading
    if (sem_init(&sync->mutexWriter, 1, 1) == -1) {
        perror("Error en sem_init de mutexWriter");
        exit(EXIT_FAILURE);
    }
    if (sem_init(&sync->mutexStatus, 1, 1) == -1) {
        perror("Error en sem_init de mutexStatus");
        exit(EXIT_FAILURE);
    }
    if (sem_init(&sync->mutexReaders, 1, 1) == -1) {
        perror("Error en sem_init de mutexReaders");
        exit(EXIT_FAILURE);
    }
    sync->playersReading = 0;

    // Control de flujo individual por jugador
    // arrancan en 0 porque el jugador tiene que esperar a que el master le de el turno
    for (int i = 0; i < num_players; i++) {
        if (sem_init(&sync->allowed_Mov[i], 1, 0) == -1) {
            perror("Error en sem_init de allowed_Mov");
            exit(EXIT_FAILURE);
        }
    }

    // ### CREACIÓN DE PIPES ### //

    int pipes[9][2]; // pipes[i][0] = lectura (master), pipes[i][1] = escritura (jugador)
    for (int i = 0; i < num_players; i++) {
        if (pipe(pipes[i]) == -1) {
            perror("Error creando pipe");
            exit(EXIT_FAILURE);
        }
    }

    // ### CREACIÓN DEL PROCESO HIJO VISTA ### // 
    if (view_path != NULL) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("Error creando proceso vista");
            exit(EXIT_FAILURE);
        }
        // Proceso hijo: como la vista no es un jugador, no necesitamos los pipes -> los cerramos
        if (pid == 0) {
            for (int i = 0; i < num_players; i++) {
                close(pipes[i][0]);
                close(pipes[i][1]);
            }

            // En execv armamos manualmente el argv del nuevo programa.
            // char *args[] = { arg0, arg1, arg2, NULL };
            // execv(ruta, args);
            // "ruta" es el ejecutable a cargar.
            // args[0] pasa a ser argv[0] del nuevo programa.
            // El NULL final marca dónde termina el arreglo de argumentos.
            char *view_argv[] = { view_path, NULL };
            execv(view_path, view_argv);

            // Si llego aca, hubo un error en execv
            perror("Error ejecutando vista");
            _exit(EXIT_FAILURE);
        }
        // Proceso padre: sigue la ejecucion de master
    }

    // ### CREACIÓN DE PROCESOS HIJOS JUGADORES ### // 
    for (int i = 0; i < num_players; i++) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("Error creando proceso jugador");
            exit(EXIT_FAILURE);
        }
        // Proceso hijo: ejecuta el jugador
        if (pid == 0) {
            char index_str[12];
            char fd_str[12];
            // Cerramos los pipes que no son del jugador actual
            for (int j = 0; j < num_players; j++) {
                // Cerramos todos los extremos de lectura porque un jugador no lee de ningun pipe
                close(pipes[j][0]);

                if (j != i) {
                    // cerramos el pipe de escritura de todos los otros jugadores
                    close(pipes[j][1]);
                }
            }
        // Hasta aca dejamos al jugador con un solo FD util: pipes[i][1] (escritura)
            
        
            // Esas dos lineas convierten enteros a texto, porque execv solo pasa strings en argv
            snprintf(index_str, sizeof(index_str), "%d", i);
            snprintf(fd_str, sizeof(fd_str), "%d", pipes[i][1]);

            //char *player_argv[] = { player_paths[i], index_str, fd_str, NULL };
            //execv(player_paths[i], player_argv);

            // Alternamos: pares son random, impares son fijos
            char *behavior = (i % 2 == 0) ? "random" : "fijo"; 
            
            // Ahora sí pasamos el argumento "algo" al proceso jugador
            char *player_argv[] = { player_paths[i], index_str, fd_str, behavior, NULL };
            execv(player_paths[i], player_argv);

            // Si llego aca, hubo un error en execv
            perror("Error ejecutando jugador");
            _exit(EXIT_FAILURE);
        } 
        // Proceso padre: sigue la ejecucion de master
        state->players[i].pid = pid;
        close(pipes[i][1]); 
    }

    // si hay vista, hacemos una primera "foto" del estado inicial
    // la idea es: el master avisa que ya hay algo coherente para mostrar
    // y despues espera hasta que la vista confirme que termino de imprimir
    if (view_path != NULL) {
        if (sem_post(&sync->canPrint) == -1) {
            perror("Error avisando a la vista");
            exit(EXIT_FAILURE);
        }

        if (sem_wait(&sync->completedPrint) == -1) {
            perror("Error esperando a la vista");
            exit(EXIT_FAILURE);
        }
    }

    // ### BUCLE PRINCIPAL DEL JUEGO ### // 
    int round = 0;
    int winner_index = -1;
    bool all_dead = false;

    while (!state->finished) {
        for (int offset = 0; offset < num_players; offset++) {
            int player_index = (round + offset) % num_players;
            Player *p = &state->players[player_index];

            // 1. Si el jugador ya fue penalizado/bloqueado antes, lo saltamos
            if (p->blocked) {
                continue; 
            }

            // 2. Le damos luz verde para que juegue
            if (sem_post(&sync->allowed_Mov[player_index]) == -1) {
                perror("Error habilitando movimiento del jugador");
                state->finished = true;
                break;
            }

            // 3. PREPARAMOS SELECT()
            fd_set read_fds;
            FD_ZERO(&read_fds); // Vaciamos la "caja"
            
            int pipe_lectura = pipes[player_index][0];
            FD_SET(pipe_lectura, &read_fds); // Metemos el pipe del jugador actual
            
            // select requiere que el primer parámetro sea el fd más alto + 1
            int nfds = pipe_lectura + 1;

            // Configuramos el temporizador con el parámetro que recibimos por getopt
            struct timeval tv;
            tv.tv_sec = timeout; // Segundos (por defecto 10, o lo que pase el usuario)
            tv.tv_usec = 0;      // Microsegundos

            // 4. EJECUTAMOS SELECT (Nos quedamos esperando aquí)
            int select_result = select(nfds, &read_fds, NULL, NULL, &tv);

            if (select_result == -1) {
                perror("Error en select");
                state->finished = true;
                break;
            } 
            else if (select_result == 0) {
                // TIMEOUT: El tiempo se agotó.
                fprintf(stderr, "TIMEOUT: El jugador %d tardo mas de %d segs. Ha sido bloqueado.\n", 
                        player_index, timeout);
                p->blocked = true;
                
                // Forzamos a la vista a dibujar este bloqueo
                if (view_path != NULL) {
                    sem_post(&sync->canPrint);
                    sem_wait(&sync->completedPrint);
                }
                continue; // Pasamos al siguiente jugador sin hacer read()
            } 
            else {
                // ÉXITO: El jugador mandó su movimiento a tiempo
                if (FD_ISSET(pipe_lectura, &read_fds)) {
                    MoveRequest request;
                    ssize_t bytes_read = read(pipe_lectura, &request, sizeof(request));

                    if (bytes_read <= 0 || bytes_read != (ssize_t)sizeof(request)) {
                        fprintf(stderr, "El jugador %d envio datos corruptos o cerro el pipe. Bloqueado.\n", player_index);
                        p->blocked = true;
                        continue;
                    }

                    // Aplicamos el movimiento real en el tablero
                    aplicar_movimiento(state, sync, player_index, (MoveDirection)request.direction);

                    // Avisamos a la vista que el tablero cambió
                    if (view_path != NULL) {
                        sem_post(&sync->canPrint);
                        sem_wait(&sync->completedPrint);
                        usleep(delay * 5000); 
                    }
                }
            }
        }
        
       // 5. EVALUACIÓN SILENCIOSA DE FIN DE JUEGO
        bool all_blocked = true;
        for (int i = 0; i < num_players; i++) {
            if (state->players[i].score >= 30) {
                winner_index = i;
                state->finished = true;
                break;
            }
            if (!state->players[i].blocked) {
                all_blocked = false;
            }
        }
        
        if (!state->finished && all_blocked) {
            all_dead = true;
            state->finished = true;
        }

        round++;
    }
        
    // --- EL MOMENTO CLAVE ---
    // Primero hacemos la última señal a la vista para que dibuje el tablero final.
    // La Vista hará su última limpieza de pantalla aquí.
    if (view_path != NULL) {
        sem_post(&sync->canPrint);
        sem_wait(&sync->completedPrint);
    }

    // AHORA QUE LA VISTA TERMINÓ Y YA NO LIMPIARÁ LA PANTALLA, EL MÁSTER IMPRIME
    if (winner_index != -1) {
        printf("\n🏆 ¡EL JUGADOR %d (%s) HA GANADO LA PARTIDA CON %u PUNTOS! 🏆\n", 
               winner_index + 1, state->players[winner_index].name, state->players[winner_index].score);
    } 
    else if (all_dead) {
        printf("\n💀 TODOS LOS JUGADORES ESTÁN BLOQUEADOS. FIN DEL JUEGO. 💀\n");
    }

    // Destrabamos a los jugadores restantes para que salgan
    for (int i = 0; i < num_players; i++) {
        sem_post(&sync->allowed_Mov[i]);
    }
    
    // aca destrabamos a todos porque cada uno puede haber quedado esperando su proximo turno
    for (int i = 0; i < num_players; i++) {
        sem_post(&sync->allowed_Mov[i]);
    }

    // ### CLEANUP: LIMPIEZA FINAL DE RECURSOS ### //
    printf("\nIniciando limpieza de recursos...\n");

    // 1. Esperar a que TODOS los hijos (Vista y Jugadores) mueran pacíficamente
    // wait(NULL) suspende al master hasta que un hijo termina. 
    // Cuando devuelve -1, significa que ya no quedan más hijos vivos.
    while (wait(NULL) > 0);
    printf("[-] Todos los procesos hijos finalizados (Zombies eliminados).\n");

    // 2. Cerrar todos los pipes de lectura que el Master seguía escuchando
    for (int i = 0; i < num_players; i++) {
        close(pipes[i][0]);
    }
    printf("[-] Pipes cerrados.\n");

    // 3. Destruir los semáforos POSIX
    sem_destroy(&sync->canPrint);
    sem_destroy(&sync->completedPrint);
    sem_destroy(&sync->mutexWriter);
    sem_destroy(&sync->mutexStatus);
    sem_destroy(&sync->mutexReaders);
    for (int i = 0; i < num_players; i++) {
        sem_destroy(&sync->allowed_Mov[i]);
    }
    printf("[-] Semáforos destruidos.\n");

    // 4. Desmapear la memoria virtual
    munmap(state, state_size);
    munmap(sync, sync_size);
    printf("[-] Memoria desmapeada.\n");

    // 5. Borrar los archivos de Memoria Compartida del Sistema Operativo
    shm_unlink("/game_state");
    shm_unlink("/game_sync");
    printf("[-] Archivos de memoria compartida eliminados\n");

    printf("\n✅ JUEGO CERRADO CORRECTAMENTE. ¡HASTA LA PRÓXIMA!\n");
    return 0;
}
   

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <indice_jugador> <pipe_fd>\n", argv[0]);
        return 1;
    }

    int player_index = atoi(argv[1]);
    int pipe_fd = atoi(argv[2]);

    // En esta etapa solo dejamos parseados los argumentos que el master
    // pasa por execv. Más adelante los usaremos para sincronización y envío.
    (void)player_index;
    (void)pipe_fd;

    return 0;
}

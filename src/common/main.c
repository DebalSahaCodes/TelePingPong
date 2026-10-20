#include <stdio.h>
#include <string.h>

int server_main(int argc, char **argv);
int client_main(int argc, char **argv);

#define VERSION_STR "1.0.0"

int main(int argc, char **argv) {
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("pingpong %s\n", VERSION_STR);
        return 0;
    }
    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("pingpong %s - terminal ping pong over the internet\n\n"
               "  pingpong                       play (asks which server to join)\n"
               "  pingpong -s HOST[:PORT] -n NAME   play, skipping the questions\n"
               "  pingpong server [-p PORT]      run a lobby/match server (default port 7777)\n\n"
               "Server options: --win-score N  --max-matches N  --max-per-ip N\n"
               "Environment: PINGPONG_SERVER=host[:port] sets the default server.\n", VERSION_STR);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "server")) return server_main(argc - 2, argv + 2);
    return client_main(argc - 1, argv + 1);
}

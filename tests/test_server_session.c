#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define main linuxchat_server_main
#include "../server/server.c"
#undef main

static void reset_client_state(void) {
    memset(clients, 0, sizeof(clients));
    client_count = 0;
}

int main(void) {
    pthread_mutex_init(&clients_mutex, NULL);
    reset_client_state();

    clients[0].sockfd = 11;
    clients[0].user_id = -1;
    clients[1].sockfd = 12;
    clients[1].user_id = -1;
    client_count = 2;

    update_client_session(12, 7, "alice", "Alice");

    assert(clients[0].user_id == -1);
    assert(clients[0].username[0] == '\0');
    assert(clients[0].nickname[0] == '\0');

    assert(clients[1].user_id == 7);
    assert(strcmp(clients[1].username, "alice") == 0);
    assert(strcmp(clients[1].nickname, "Alice") == 0);

    update_client_session(99, 8, "bob", "Bob");

    assert(clients[1].user_id == 7);
    assert(strcmp(clients[1].username, "alice") == 0);
    assert(strcmp(clients[1].nickname, "Alice") == 0);

    char long_username[128];
    char long_nickname[128];
    memset(long_username, 'u', sizeof(long_username) - 1);
    memset(long_nickname, 'n', sizeof(long_nickname) - 1);
    long_username[sizeof(long_username) - 1] = '\0';
    long_nickname[sizeof(long_nickname) - 1] = '\0';

    update_client_session(12, 9, long_username, long_nickname);

    assert(clients[1].user_id == 9);
    assert(clients[1].username[sizeof(clients[1].username) - 1] == '\0');
    assert(clients[1].nickname[sizeof(clients[1].nickname) - 1] == '\0');
    assert(strlen(clients[1].username) == sizeof(clients[1].username) - 1);
    assert(strlen(clients[1].nickname) == sizeof(clients[1].nickname) - 1);

    pthread_mutex_destroy(&clients_mutex);
    puts("server session tests passed");
    return 0;
}

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define main linuxchat_server_main
#include "../server/server.c"
#undef main

static void reset_client_state(void) {
    memset(clients, 0, sizeof(clients));
    client_count = 0;
}

static void test_protocol_record_bounds(void) {
    char output[96] = "";

    assert(append_protocol_record(output, sizeof(output), "1", "alice", "Alice") == 0);
    assert(strcmp(output, "1:alice:Alice;") == 0);

    assert(append_protocol_record(output, sizeof(output), NULL, "empty-id", "Name") == 0);
    assert(strcmp(output, "1:alice:Alice;:empty-id:Name;") == 0);

    char tiny[8] = "";
    assert(append_protocol_record(tiny, sizeof(tiny), "abcdef", "ghijkl", "mnopqr") == -1);
    assert(tiny[sizeof(tiny) - 1] == '\0');

    assert(append_protocol_record(tiny, 0, "a", "b", "c") == -1);
}

static void test_online_session_update(void) {
    reset_client_state();

    clients[0].sockfd = 31;
    clients[0].user_id = -1;
    clients[1].sockfd = 32;
    clients[1].user_id = -1;
    client_count = 2;

    update_client_session(32, 7, "alice", "Alice");

    assert(clients[0].user_id == -1);
    assert(clients[1].user_id == 7);
    assert(strcmp(clients[1].username, "alice") == 0);
    assert(strcmp(clients[1].nickname, "Alice") == 0);

    update_client_session(999, 8, "bob", "Bob");
    assert(clients[1].user_id == 7);
    assert(strcmp(clients[1].username, "alice") == 0);
}

static void test_online_lookup_uses_session_state(void) {
    reset_client_state();

    clients[0].sockfd = 41;
    clients[0].user_id = 10;
    clients[1].sockfd = 42;
    clients[1].user_id = -1;
    client_count = 2;

    assert(is_user_online(10) == 1);
    assert(is_user_online(11) == 0);
    assert(is_user_online(0) == 0);
}

static void test_shutdown_signal_marks_server_stopped(void) {
    server_running = 1;
    handle_shutdown_signal(SIGINT);
    assert(server_running == 0);

    server_running = 1;
    handle_shutdown_signal(SIGTERM);
    assert(server_running == 0);

    server_running = 1;
}

static void test_send_to_user_targets_only_matching_user(void) {
    int alice_pair[2];
    int bob_pair[2];
    char buffer[32] = "";

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, alice_pair) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, bob_pair) == 0);

    reset_client_state();
    clients[0].sockfd = alice_pair[0];
    clients[0].user_id = 1;
    clients[1].sockfd = bob_pair[0];
    clients[1].user_id = 2;
    client_count = 2;

    send_to_user(2, "hello");

    ssize_t bytes = recv(bob_pair[1], buffer, sizeof(buffer) - 1, 0);
    assert(bytes == 5);
    assert(strcmp(buffer, "hello") == 0);

    errno = 0;
    send_to_user(999, "nobody");

    close(alice_pair[0]);
    close(alice_pair[1]);
    close(bob_pair[0]);
    close(bob_pair[1]);
}

int main(void) {
    pthread_mutex_init(&clients_mutex, NULL);
    pthread_mutex_init(&db_mutex, NULL);

    test_protocol_record_bounds();
    test_online_session_update();
    test_online_lookup_uses_session_state();
    test_shutdown_signal_marks_server_stopped();
    test_send_to_user_targets_only_matching_user();

    pthread_mutex_destroy(&db_mutex);
    pthread_mutex_destroy(&clients_mutex);

    puts("server core tests passed");
    return 0;
}

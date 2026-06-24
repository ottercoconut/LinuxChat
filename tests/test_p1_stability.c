#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define main linuxchat_server_main
#include "../server/server.c"
#undef main

int main(void) {
    pthread_mutex_init(&clients_mutex, NULL);
    pthread_mutex_init(&db_mutex, NULL);

    char record[128] = "";
    assert(append_protocol_record(record, sizeof(record),
                                  "hello", "2026-06-23 15-30-45", "Alice") == 0);
    assert(strcmp(record, "hello:2026-06-23 15-30-45:Alice;") == 0);
    assert(strchr("2026-06-23 15-30-45", ':') == NULL);

    assert(append_protocol_record(record, sizeof(record),
                                  "second", "2026-06-23 15-31-01", "Bob") == 0);
    assert(strcmp(record,
                  "hello:2026-06-23 15-30-45:Alice;"
                  "second:2026-06-23 15-31-01:Bob;") == 0);

    char small[24] = "";
    assert(append_protocol_record(small, sizeof(small),
                                  "1234567890", "abcdefghij", "klmnopqrst") == -1);
    assert(small[sizeof(small) - 1] == '\0');

    pthread_mutex_destroy(&db_mutex);
    pthread_mutex_destroy(&clients_mutex);

    puts("P1 stability tests passed");
    return 0;
}

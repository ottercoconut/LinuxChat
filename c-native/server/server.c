#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <mysql.h>

#define PORT 8888
#define MAX_CLIENTS 100
#define BUFFER_SIZE 1024

MYSQL *db_conn;
pthread_mutex_t db_mutex;

typedef struct {
    int sockfd;
    struct sockaddr_in addr;
    int user_id;
    char username[50];
    char nickname[50];
} Client;

Client clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex;

int client_count = 0;

int is_protocol_safe_text(const char *text, int allow_comma) {
    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    for (const unsigned char *ptr = (const unsigned char *)text; *ptr != '\0'; ptr++) {
        if (*ptr < 32 || *ptr == ':' || *ptr == ';' || (!allow_comma && *ptr == ',')) {
            return 0;
        }
    }

    return 1;
}

static void bind_string_param(MYSQL_BIND *bind, const char *value, unsigned long *length) {
    *length = (unsigned long)strlen(value);
    bind->buffer_type = MYSQL_TYPE_STRING;
    bind->buffer = (char *)value;
    bind->buffer_length = *length;
    bind->length = length;
}

static void bind_int_param(MYSQL_BIND *bind, int *value) {
    bind->buffer_type = MYSQL_TYPE_LONG;
    bind->buffer = value;
}

int append_protocol_record(char *result, size_t result_size,
                           const char *field1, const char *field2, const char *field3) {
    if (result_size == 0) {
        return -1;
    }

    size_t used = strlen(result);

    if (used >= result_size) {
        return -1;
    }

    int written = snprintf(result + used, result_size - used, "%s:%s:%s;",
                           field1 ? field1 : "", field2 ? field2 : "", field3 ? field3 : "");
    if (written < 0 || (size_t)written >= result_size - used) {
        result[result_size - 1] = '\0';
        return -1;
    }

    return 0;
}

void update_client_session(int sockfd, int user_id, const char *username, const char *nickname) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].sockfd == sockfd) {
            clients[i].user_id = user_id;
            strncpy(clients[i].username, username, sizeof(clients[i].username) - 1);
            clients[i].username[sizeof(clients[i].username) - 1] = '\0';
            strncpy(clients[i].nickname, nickname, sizeof(clients[i].nickname) - 1);
            clients[i].nickname[sizeof(clients[i].nickname) - 1] = '\0';
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void init_database(void) {
    db_conn = mysql_init(NULL);
    if (!mysql_real_connect(db_conn, "localhost", "chat_user", "chat_password", "chat_db", 0, NULL, 0)) {
        fprintf(stderr, "数据库连接失败: %s\n", mysql_error(db_conn));
        exit(1);
    }
    printf("数据库连接成功\n");
}

void create_tables(void) {
    char *create_users = "CREATE TABLE IF NOT EXISTS users ("
                         "id INT AUTO_INCREMENT PRIMARY KEY,"
                         "username VARCHAR(50) UNIQUE NOT NULL,"
                         "password VARCHAR(100) NOT NULL,"
                         "nickname VARCHAR(50) NOT NULL,"
                         "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)";
    
    char *create_messages = "CREATE TABLE IF NOT EXISTS messages ("
                            "id INT AUTO_INCREMENT PRIMARY KEY,"
                            "sender_id INT NOT NULL,"
                            "receiver_id INT NOT NULL,"
                            "content TEXT NOT NULL,"
                            "timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                            "FOREIGN KEY(sender_id) REFERENCES users(id) ON DELETE CASCADE,"
                            "FOREIGN KEY(receiver_id) REFERENCES users(id) ON DELETE CASCADE)";
    
    char *create_friends = "CREATE TABLE IF NOT EXISTS friends ("
                           "id INT AUTO_INCREMENT PRIMARY KEY,"
                           "user_id INT NOT NULL,"
                           "friend_id INT NOT NULL,"
                           "status INT DEFAULT 0,"
                           "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                           "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
                           "FOREIGN KEY(friend_id) REFERENCES users(id) ON DELETE CASCADE,"
                           "UNIQUE KEY unique_friend (user_id, friend_id))";
    
    pthread_mutex_lock(&db_mutex);
    if (mysql_query(db_conn, create_users) != 0) {
        fprintf(stderr, "创建users表失败: %s\n", mysql_error(db_conn));
    }
    if (mysql_query(db_conn, create_messages) != 0) {
        fprintf(stderr, "创建messages表失败: %s\n", mysql_error(db_conn));
    }
    if (mysql_query(db_conn, create_friends) != 0) {
        fprintf(stderr, "创建friends表失败: %s\n", mysql_error(db_conn));
    }
    pthread_mutex_unlock(&db_mutex);
    printf("数据库表初始化完成\n");
}

int register_user(const char *username, const char *password, const char *nickname) {
    const char *statement =
        "INSERT INTO users (username, password, nickname) VALUES (?, SHA2(?, 256), ?)";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[3];
    unsigned long lengths[3];
    int user_id = -1;

    if (!is_protocol_safe_text(username, 0) ||
        !is_protocol_safe_text(password, 0) ||
        !is_protocol_safe_text(nickname, 0)) {
        return -1;
    }

    memset(params, 0, sizeof(params));
    bind_string_param(&params[0], username, &lengths[0]);
    bind_string_param(&params[1], password, &lengths[1]);
    bind_string_param(&params[2], nickname, &lengths[2]);

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化注册语句失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0) {
        fprintf(stderr, "注册失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    user_id = (int)mysql_insert_id(db_conn);

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return user_id;
}

int login_user(const char *username, const char *password, char *nickname, int *user_id) {
    const char *statement =
        "SELECT id, nickname FROM users WHERE username = ? AND password = SHA2(?, 256)";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[2];
    MYSQL_BIND results[2];
    unsigned long param_lengths[2];
    unsigned long nickname_length = 0;
    char nickname_buffer[50] = "";
    int found_user_id = 0;
    int status = -1;

    if (!is_protocol_safe_text(username, 0) ||
        !is_protocol_safe_text(password, 0) ||
        nickname == NULL ||
        user_id == NULL) {
        return -1;
    }

    memset(params, 0, sizeof(params));
    bind_string_param(&params[0], username, &param_lengths[0]);
    bind_string_param(&params[1], password, &param_lengths[1]);

    memset(results, 0, sizeof(results));
    results[0].buffer_type = MYSQL_TYPE_LONG;
    results[0].buffer = &found_user_id;
    results[1].buffer_type = MYSQL_TYPE_STRING;
    results[1].buffer = nickname_buffer;
    results[1].buffer_length = sizeof(nickname_buffer) - 1;
    results[1].length = &nickname_length;

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化登录语句失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_bind_result(stmt, results) != 0) {
        fprintf(stderr, "登录查询失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    int fetch_status = mysql_stmt_fetch(stmt);
    if (fetch_status == 0 || fetch_status == MYSQL_DATA_TRUNCATED) {
        size_t copy_length = nickname_length < 49 ? nickname_length : 49;
        nickname_buffer[copy_length] = '\0';
        if (!is_protocol_safe_text(nickname_buffer, 0)) {
            goto cleanup;
        }

        *user_id = found_user_id;
        strncpy(nickname, nickname_buffer, 49);
        nickname[49] = '\0';
        status = 0;
    }

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

int add_friend(int user_id, int friend_id) {
    char query[500];

    if (user_id <= 0 || friend_id <= 0 || user_id == friend_id) {
        return -1;
    }

    snprintf(query, sizeof(query), "INSERT INTO friends (user_id, friend_id, status) VALUES (%d, %d, 1)",
             user_id, friend_id);
    
    pthread_mutex_lock(&db_mutex);
    if (mysql_query(db_conn, "START TRANSACTION") != 0) {
        fprintf(stderr, "开始好友事务失败: %s\n", mysql_error(db_conn));
        pthread_mutex_unlock(&db_mutex);
        return -1;
    }

    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "添加好友失败: %s\n", mysql_error(db_conn));
        mysql_query(db_conn, "ROLLBACK");
        pthread_mutex_unlock(&db_mutex);
        return -1;
    }
    
    snprintf(query, sizeof(query), "INSERT INTO friends (user_id, friend_id, status) VALUES (%d, %d, 1)",
             friend_id, user_id);
    
    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "添加好友失败: %s\n", mysql_error(db_conn));
        mysql_query(db_conn, "ROLLBACK");
        pthread_mutex_unlock(&db_mutex);
        return -1;
    }

    if (mysql_query(db_conn, "COMMIT") != 0) {
        fprintf(stderr, "提交好友事务失败: %s\n", mysql_error(db_conn));
        mysql_query(db_conn, "ROLLBACK");
        pthread_mutex_unlock(&db_mutex);
        return -1;
    }

    pthread_mutex_unlock(&db_mutex);
    return 0;
}

void get_friends(int user_id, char *result, size_t result_size) {
    char query[500];
    snprintf(query, sizeof(query), "SELECT u.id, u.username, u.nickname FROM friends f "
                                   "JOIN users u ON f.friend_id = u.id WHERE f.user_id = %d", user_id);

    if (result_size == 0) {
        return;
    }
    result[0] = '\0';
    if (user_id <= 0) {
        return;
    }

    pthread_mutex_lock(&db_mutex);
    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "获取好友列表失败: %s\n", mysql_error(db_conn));
        pthread_mutex_unlock(&db_mutex);
        return;
    }
    
    MYSQL_RES *res = mysql_store_result(db_conn);
    if (res == NULL) {
        pthread_mutex_unlock(&db_mutex);
        return;
    }
    
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != NULL) {
        if (append_protocol_record(result, result_size, row[0], row[1], row[2]) != 0) {
            fprintf(stderr, "好友列表结果过长，已截断\n");
            break;
        }
    }
    mysql_free_result(res);
    pthread_mutex_unlock(&db_mutex);
}

int are_friends(int user_id, int friend_id) {
    char query[300];
    int result = 0;

    if (user_id <= 0 || friend_id <= 0 || user_id == friend_id) {
        return 0;
    }

    snprintf(query, sizeof(query),
             "SELECT COUNT(*) FROM friends WHERE user_id = %d AND friend_id = %d AND status = 1",
             user_id, friend_id);

    pthread_mutex_lock(&db_mutex);
    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "好友关系查询失败: %s\n", mysql_error(db_conn));
        pthread_mutex_unlock(&db_mutex);
        return 0;
    }

    MYSQL_RES *res = mysql_store_result(db_conn);
    if (res != NULL) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row != NULL && row[0] != NULL) {
            result = atoi(row[0]) > 0;
        }
        mysql_free_result(res);
    }

    pthread_mutex_unlock(&db_mutex);
    return result;
}

void get_messages(int user_id, int friend_id, char *result, size_t result_size) {
    char query[500];
    snprintf(query, sizeof(query), "SELECT m.content, DATE_FORMAT(m.timestamp, '%%Y-%%m-%%d %%H-%%i-%%s'), u.nickname FROM messages m "
                                   "JOIN users u ON m.sender_id = u.id WHERE "
                                   "(m.sender_id = %d AND m.receiver_id = %d) OR "
                                   "(m.sender_id = %d AND m.receiver_id = %d) ORDER BY m.timestamp",
             user_id, friend_id, friend_id, user_id);

    if (result_size == 0) {
        return;
    }
    result[0] = '\0';
    if (user_id <= 0 || friend_id <= 0) {
        return;
    }

    pthread_mutex_lock(&db_mutex);
    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "获取消息失败: %s\n", mysql_error(db_conn));
        pthread_mutex_unlock(&db_mutex);
        return;
    }
    
    MYSQL_RES *res = mysql_store_result(db_conn);
    if (res == NULL) {
        pthread_mutex_unlock(&db_mutex);
        return;
    }
    
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != NULL) {
        if (append_protocol_record(result, result_size, row[0], row[1], row[2]) != 0) {
            fprintf(stderr, "消息列表结果过长，已截断\n");
            break;
        }
    }
    mysql_free_result(res);
    pthread_mutex_unlock(&db_mutex);
}

int save_message(int sender_id, int receiver_id, const char *content) {
    const char *statement =
        "INSERT INTO messages (sender_id, receiver_id, content) VALUES (?, ?, ?)";
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND params[3];
    unsigned long content_length;
    int status = -1;
    int bound_sender_id = sender_id;
    int bound_receiver_id = receiver_id;

    if (sender_id <= 0 ||
        receiver_id <= 0 ||
        !is_protocol_safe_text(content, 1)) {
        return -1;
    }

    memset(params, 0, sizeof(params));
    bind_int_param(&params[0], &bound_sender_id);
    bind_int_param(&params[1], &bound_receiver_id);
    bind_string_param(&params[2], content, &content_length);

    pthread_mutex_lock(&db_mutex);
    stmt = mysql_stmt_init(db_conn);
    if (stmt == NULL) {
        fprintf(stderr, "初始化消息语句失败: %s\n", mysql_error(db_conn));
        goto cleanup;
    }

    if (mysql_stmt_prepare(stmt, statement, strlen(statement)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_execute(stmt) != 0) {
        fprintf(stderr, "保存消息失败: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    status = 0;

cleanup:
    if (stmt != NULL) {
        mysql_stmt_close(stmt);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

void broadcast_message(int sender_id, const char *message) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].user_id == sender_id) {
            continue;
        }
        send(clients[i].sockfd, message, strlen(message), 0);
    }
    pthread_mutex_unlock(&clients_mutex);
}

void send_to_user(int user_id, const char *message) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].user_id == user_id) {
            send(clients[i].sockfd, message, strlen(message), 0);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void *handle_client(void *arg) {
    Client *client = (Client *)arg;
    char buffer[BUFFER_SIZE];
    char response[BUFFER_SIZE];
    
    printf("新客户端连接: %s:%d\n", inet_ntoa(client->addr.sin_addr), ntohs(client->addr.sin_port));
    
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_read = recv(client->sockfd, buffer, BUFFER_SIZE - 1, 0);
        
        if (bytes_read <= 0) {
            printf("客户端断开连接: %s:%d\n", inet_ntoa(client->addr.sin_addr), ntohs(client->addr.sin_port));
            break;
        }
        
        printf("收到消息: %s\n", buffer);
        
        if (strncmp(buffer, "REGISTER:", 9) == 0) {
            char username[50], password[50], nickname[50];
            if (sscanf(buffer + 9, "%49[^,],%49[^,],%49[^\n]", username, password, nickname) == 3) {
                int user_id = register_user(username, password, nickname);
                if (user_id > 0) {
                    snprintf(response, sizeof(response), "REGISTER_SUCCESS:%d", user_id);
                } else {
                    snprintf(response, sizeof(response), "REGISTER_FAILED");
                }
            } else {
                snprintf(response, sizeof(response), "REGISTER_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (strncmp(buffer, "LOGIN:", 6) == 0) {
            char username[50], password[50], nickname[50];
            int user_id;

            if (sscanf(buffer + 6, "%49[^,],%49[^\n]", username, password) == 2 &&
                login_user(username, password, nickname, &user_id) == 0) {
                client->user_id = user_id;
                strncpy(client->username, username, sizeof(client->username) - 1);
                client->username[sizeof(client->username) - 1] = '\0';
                strncpy(client->nickname, nickname, sizeof(client->nickname) - 1);
                client->nickname[sizeof(client->nickname) - 1] = '\0';
                update_client_session(client->sockfd, user_id, username, nickname);
                snprintf(response, sizeof(response), "LOGIN_SUCCESS:%d:%s:%s", user_id, username, nickname);
            } else {
                snprintf(response, sizeof(response), "LOGIN_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (strncmp(buffer, "ADDFRIEND:", 10) == 0) {
            int requested_user_id, friend_id;
            if (client->user_id > 0 &&
                sscanf(buffer + 10, "%d,%d", &requested_user_id, &friend_id) == 2 &&
                add_friend(client->user_id, friend_id) == 0) {
                if (requested_user_id != client->user_id) {
                    printf("忽略客户端声明的好友操作用户ID: %d，使用登录会话ID: %d\n",
                           requested_user_id, client->user_id);
                }
                snprintf(response, sizeof(response), "ADDFRIEND_SUCCESS");
            } else {
                snprintf(response, sizeof(response), "ADDFRIEND_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (strncmp(buffer, "FRIENDS:", 8) == 0) {
            int requested_user_id;
            if (client->user_id > 0 && sscanf(buffer + 8, "%d", &requested_user_id) == 1) {
                char friends[BUFFER_SIZE];
                if (requested_user_id != client->user_id) {
                    printf("忽略客户端声明的好友列表用户ID: %d，使用登录会话ID: %d\n",
                           requested_user_id, client->user_id);
                }
                get_friends(client->user_id, friends, sizeof(friends));
                snprintf(response, sizeof(response), "FRIENDS_LIST:%s", friends);
            } else {
                snprintf(response, sizeof(response), "FRIENDS_LIST:");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (strncmp(buffer, "MESSAGES:", 9) == 0) {
            int requested_user_id, friend_id;
            if (client->user_id > 0 &&
                sscanf(buffer + 9, "%d,%d", &requested_user_id, &friend_id) == 2 &&
                are_friends(client->user_id, friend_id)) {
                char messages[BUFFER_SIZE];
                if (requested_user_id != client->user_id) {
                    printf("忽略客户端声明的消息查询用户ID: %d，使用登录会话ID: %d\n",
                           requested_user_id, client->user_id);
                }
                get_messages(client->user_id, friend_id, messages, sizeof(messages) - strlen("MESSAGES_LIST:"));
                snprintf(response, sizeof(response), "MESSAGES_LIST:%s", messages);
            } else {
                snprintf(response, sizeof(response), "MESSAGES_LIST:");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (strncmp(buffer, "SEND:", 5) == 0) {
            int sender_id, requested_sender_id, receiver_id;
            char content[BUFFER_SIZE];

            if (client->user_id <= 0) {
                snprintf(response, sizeof(response), "SEND_FAILED");
                send(client->sockfd, response, strlen(response), 0);
                continue;
            }

            if (sscanf(buffer + 5, "%d,%d,%1023[^\n]", &requested_sender_id, &receiver_id, content) != 3) {
                snprintf(response, sizeof(response), "SEND_FAILED");
                send(client->sockfd, response, strlen(response), 0);
                continue;
            }

            sender_id = client->user_id;
            if (requested_sender_id != sender_id) {
                printf("忽略客户端声明的发送者ID: %d，使用登录会话ID: %d\n",
                       requested_sender_id, sender_id);
            }
            if (!are_friends(sender_id, receiver_id) || save_message(sender_id, receiver_id, content) != 0) {
                snprintf(response, sizeof(response), "SEND_FAILED");
                send(client->sockfd, response, strlen(response), 0);
                continue;
            }

            snprintf(response, sizeof(response), "NEW_MESSAGE:%d:%s:%s", sender_id, client->nickname, content);
            send_to_user(receiver_id, response);
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (strcmp(buffer, "QUIT") == 0) {
            break;
        }
    }
    
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].sockfd == client->sockfd) {
            for (int j = i; j < client_count - 1; j++) {
                clients[j] = clients[j + 1];
            }
            client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    close(client->sockfd);
    free(client);
    return NULL;
}

int main(void) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    
    pthread_mutex_init(&clients_mutex, NULL);
    pthread_mutex_init(&db_mutex, NULL);
    
    init_database();
    create_tables();
    
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt SO_REUSEADDR");
        exit(EXIT_FAILURE);
    }

#ifdef SO_REUSEPORT
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt SO_REUSEPORT");
        exit(EXIT_FAILURE);
    }
#endif
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    
    printf("服务器启动成功，监听端口 %d...\n", PORT);
    
    while (1) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept");
            continue;
        }
        
        if (client_count >= MAX_CLIENTS) {
            printf("客户端数量已达上限\n");
            close(new_socket);
            continue;
        }
        
        Client *client = (Client *)malloc(sizeof(Client));
        client->sockfd = new_socket;
        client->addr = address;
        client->user_id = -1;
        client->username[0] = '\0';
        client->nickname[0] = '\0';
        
        pthread_mutex_lock(&clients_mutex);
        clients[client_count++] = *client;
        pthread_mutex_unlock(&clients_mutex);
        
        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_client, client) != 0) {
            perror("pthread_create");
            free(client);
        }
        pthread_detach(thread);
    }
    
    mysql_close(db_conn);
    return 0;
}

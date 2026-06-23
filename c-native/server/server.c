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
                            "FOREIGN KEY(sender_id) REFERENCES users(id),"
                            "FOREIGN KEY(receiver_id) REFERENCES users(id))";
    
    char *create_friends = "CREATE TABLE IF NOT EXISTS friends ("
                           "id INT AUTO_INCREMENT PRIMARY KEY,"
                           "user_id INT NOT NULL,"
                           "friend_id INT NOT NULL,"
                           "status INT DEFAULT 0,"
                           "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
                           "FOREIGN KEY(user_id) REFERENCES users(id),"
                           "FOREIGN KEY(friend_id) REFERENCES users(id))";
    
    if (mysql_query(db_conn, create_users) != 0) {
        fprintf(stderr, "创建users表失败: %s\n", mysql_error(db_conn));
    }
    if (mysql_query(db_conn, create_messages) != 0) {
        fprintf(stderr, "创建messages表失败: %s\n", mysql_error(db_conn));
    }
    if (mysql_query(db_conn, create_friends) != 0) {
        fprintf(stderr, "创建friends表失败: %s\n", mysql_error(db_conn));
    }
    printf("数据库表初始化完成\n");
}

int register_user(const char *username, const char *password, const char *nickname) {
    char query[500];
    sprintf(query, "INSERT INTO users (username, password, nickname) VALUES ('%s', '%s', '%s')",
            username, password, nickname);
    
    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "注册失败: %s\n", mysql_error(db_conn));
        return -1;
    }
    return mysql_insert_id(db_conn);
}

int login_user(const char *username, const char *password, char *nickname, int *user_id) {
    char query[500];
    sprintf(query, "SELECT id, nickname FROM users WHERE username='%s' AND password='%s'",
            username, password);
    
    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "登录查询失败: %s\n", mysql_error(db_conn));
        return -1;
    }
    
    MYSQL_RES *result = mysql_store_result(db_conn);
    if (result == NULL) {
        fprintf(stderr, "获取结果失败: %s\n", mysql_error(db_conn));
        return -1;
    }
    
    MYSQL_ROW row = mysql_fetch_row(result);
    if (row != NULL) {
        *user_id = atoi(row[0]);
        strcpy(nickname, row[1]);
        mysql_free_result(result);
        return 0;
    }
    
    mysql_free_result(result);
    return -1;
}

int add_friend(int user_id, int friend_id) {
    char query[500];
    sprintf(query, "INSERT INTO friends (user_id, friend_id, status) VALUES (%d, %d, 1)",
            user_id, friend_id);
    
    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "添加好友失败: %s\n", mysql_error(db_conn));
        return -1;
    }
    
    sprintf(query, "INSERT INTO friends (user_id, friend_id, status) VALUES (%d, %d, 1)",
            friend_id, user_id);
    
    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "添加好友失败: %s\n", mysql_error(db_conn));
        return -1;
    }
    return 0;
}

void get_friends(int user_id, char *result) {
    char query[500];
    sprintf(query, "SELECT u.id, u.username, u.nickname FROM friends f "
                   "JOIN users u ON f.friend_id = u.id WHERE f.user_id = %d", user_id);
    
    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "获取好友列表失败: %s\n", mysql_error(db_conn));
        return;
    }
    
    MYSQL_RES *res = mysql_store_result(db_conn);
    if (res == NULL) return;
    
    MYSQL_ROW row;
    strcpy(result, "");
    while ((row = mysql_fetch_row(res)) != NULL) {
        strcat(result, row[0]);
        strcat(result, ":");
        strcat(result, row[1]);
        strcat(result, ":");
        strcat(result, row[2]);
        strcat(result, ";");
    }
    mysql_free_result(res);
}

void get_messages(int user_id, int friend_id, char *result) {
    char query[500];
    sprintf(query, "SELECT m.content, m.timestamp, u.nickname FROM messages m "
                   "JOIN users u ON m.sender_id = u.id WHERE "
                   "(m.sender_id = %d AND m.receiver_id = %d) OR "
                   "(m.sender_id = %d AND m.receiver_id = %d) ORDER BY m.timestamp",
                   user_id, friend_id, friend_id, user_id);
    
    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "获取消息失败: %s\n", mysql_error(db_conn));
        return;
    }
    
    MYSQL_RES *res = mysql_store_result(db_conn);
    if (res == NULL) return;
    
    MYSQL_ROW row;
    strcpy(result, "");
    while ((row = mysql_fetch_row(res)) != NULL) {
        strcat(result, row[0]);
        strcat(result, ":");
        strcat(result, row[1]);
        strcat(result, ":");
        strcat(result, row[2]);
        strcat(result, ";");
    }
    mysql_free_result(res);
}

void save_message(int sender_id, int receiver_id, const char *content) {
    char query[1000];
    sprintf(query, "INSERT INTO messages (sender_id, receiver_id, content) VALUES (%d, %d, '%s')",
            sender_id, receiver_id, content);
    
    if (mysql_query(db_conn, query) != 0) {
        fprintf(stderr, "保存消息失败: %s\n", mysql_error(db_conn));
    }
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
            sscanf(buffer + 9, "%[^,],%[^,],%s", username, password, nickname);
            
            int user_id = register_user(username, password, nickname);
            if (user_id > 0) {
                sprintf(response, "REGISTER_SUCCESS:%d", user_id);
            } else {
                strcpy(response, "REGISTER_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (strncmp(buffer, "LOGIN:", 6) == 0) {
            char username[50], password[50], nickname[50];
            int user_id;
            sscanf(buffer + 6, "%[^,],%s", username, password);
            
            if (login_user(username, password, nickname, &user_id) == 0) {
                client->user_id = user_id;
                strncpy(client->username, username, sizeof(client->username) - 1);
                client->username[sizeof(client->username) - 1] = '\0';
                strncpy(client->nickname, nickname, sizeof(client->nickname) - 1);
                client->nickname[sizeof(client->nickname) - 1] = '\0';
                update_client_session(client->sockfd, user_id, username, nickname);
                sprintf(response, "LOGIN_SUCCESS:%d:%s:%s", user_id, username, nickname);
            } else {
                strcpy(response, "LOGIN_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (strncmp(buffer, "ADDFRIEND:", 10) == 0) {
            int user_id, friend_id;
            sscanf(buffer + 10, "%d,%d", &user_id, &friend_id);
            
            if (add_friend(user_id, friend_id) == 0) {
                strcpy(response, "ADDFRIEND_SUCCESS");
            } else {
                strcpy(response, "ADDFRIEND_FAILED");
            }
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (strncmp(buffer, "FRIENDS:", 8) == 0) {
            int user_id;
            sscanf(buffer + 8, "%d", &user_id);
            
            char friends[BUFFER_SIZE];
            get_friends(user_id, friends);
            sprintf(response, "FRIENDS_LIST:%s", friends);
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (strncmp(buffer, "MESSAGES:", 9) == 0) {
            int user_id, friend_id;
            sscanf(buffer + 9, "%d,%d", &user_id, &friend_id);
            
            char messages[BUFFER_SIZE * 10];
            get_messages(user_id, friend_id, messages);
            sprintf(response, "MESSAGES_LIST:%s", messages);
            send(client->sockfd, response, strlen(response), 0);
        }
        else if (strncmp(buffer, "SEND:", 5) == 0) {
            int sender_id, requested_sender_id, receiver_id;
            char content[BUFFER_SIZE];

            if (client->user_id <= 0) {
                strcpy(response, "SEND_FAILED");
                send(client->sockfd, response, strlen(response), 0);
                continue;
            }

            if (sscanf(buffer + 5, "%d,%d,%[^\n]", &requested_sender_id, &receiver_id, content) != 3) {
                strcpy(response, "SEND_FAILED");
                send(client->sockfd, response, strlen(response), 0);
                continue;
            }

            sender_id = client->user_id;
            if (requested_sender_id != sender_id) {
                printf("忽略客户端声明的发送者ID: %d，使用登录会话ID: %d\n",
                       requested_sender_id, sender_id);
            }
            save_message(sender_id, receiver_id, content);

            sprintf(response, "NEW_MESSAGE:%d:%s:%s", sender_id, client->nickname, content);
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
    
    init_database();
    create_tables();
    
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
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

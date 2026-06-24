#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <gtk/gtk.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8888
#define BUFFER_SIZE 1024

int sockfd = -1;
int current_user_id = -1;
char current_username[50];
char current_nickname[50];
int selected_friend_id = -1;
char selected_friend_username[50];
int selected_group_id = -1;
gboolean selected_is_group = FALSE;

GtkWidget *window;
GtkWidget *main_stack;
GtkWidget *login_window;
GtkWidget *chat_window;
GtkWidget *message_view;
GtkWidget *message_entry;
GtkWidget *friends_list;
GtkWidget *groups_list;
GtkWidget *members_list;
GtkWidget *status_label;

GList *friend_items = NULL;
pthread_t recv_thread;
int recv_thread_running = 0;

void send_message_to_server(const char *msg) {
    if (sockfd >= 0) {
        send(sockfd, msg, strlen(msg), 0);
    }
}

gboolean is_client_protocol_safe(const char *text, gboolean allow_comma) {
    if (text == NULL || text[0] == '\0') {
        return FALSE;
    }

    for (const unsigned char *ptr = (const unsigned char *)text; *ptr != '\0'; ptr++) {
        if (*ptr < 32 || *ptr == ':' || *ptr == ';' || (!allow_comma && *ptr == ',')) {
            return FALSE;
        }
    }

    return TRUE;
}

void connect_to_server(void) {
    struct sockaddr_in server_addr;

    if (sockfd >= 0) {
        close(sockfd);
        sockfd = -1;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        return;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("invalid address");
        close(sockfd);
        sockfd = -1;
        return;
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connection failed");
        close(sockfd);
        sockfd = -1;
        return;
    }

    printf("Connected to server\n");
}

void on_register_clicked(GtkButton *button, gpointer user_data) {
    (void)button;

    GtkEntry *username_entry = GTK_ENTRY(g_object_get_data(G_OBJECT(user_data), "username_entry"));
    GtkEntry *password_entry = GTK_ENTRY(g_object_get_data(G_OBJECT(user_data), "password_entry"));
    GtkEntry *nickname_entry = GTK_ENTRY(g_object_get_data(G_OBJECT(user_data), "nickname_entry"));

    const char *username = gtk_entry_get_text(username_entry);
    const char *password = gtk_entry_get_text(password_entry);
    const char *nickname = gtk_entry_get_text(nickname_entry);

    if (strlen(username) == 0 || strlen(password) == 0) {
        gtk_label_set_text(GTK_LABEL(status_label), "请填写用户名和密码");
        return;
    }
    if (strlen(username) >= 50 || strlen(password) >= 50 || strlen(nickname) >= 50) {
        gtk_label_set_text(GTK_LABEL(status_label), "用户名、密码和昵称不能超过49字节");
        return;
    }
    if (!is_client_protocol_safe(username, FALSE) ||
        !is_client_protocol_safe(password, FALSE) ||
        (strlen(nickname) > 0 && !is_client_protocol_safe(nickname, FALSE))) {
        gtk_label_set_text(GTK_LABEL(status_label), "输入不能包含逗号、冒号、分号或换行");
        return;
    }

    connect_to_server();
    if (sockfd < 0) {
        gtk_label_set_text(GTK_LABEL(status_label), "无法连接服务器");
        return;
    }

    char msg[BUFFER_SIZE];
    if (snprintf(msg, sizeof(msg), "REGISTER:%s,%s,%s", username, password, nickname) >= (int)sizeof(msg)) {
        gtk_label_set_text(GTK_LABEL(status_label), "注册信息过长");
        close(sockfd);
        sockfd = -1;
        return;
    }
    send_message_to_server(msg);

    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);
    int bytes_read = recv(sockfd, buffer, BUFFER_SIZE - 1, 0);

    if (bytes_read > 0 &&
        strncmp(buffer, "REGISTER_SUCCESS:", strlen("REGISTER_SUCCESS:")) == 0) {
        gtk_label_set_text(GTK_LABEL(status_label), "注册成功，请登录");
        gtk_entry_set_text(username_entry, "");
        gtk_entry_set_text(password_entry, "");
        gtk_entry_set_text(nickname_entry, "");
    } else if (strcmp(buffer, "REGISTER_FAILED_DUPLICATE_USERNAME") == 0) {
        gtk_label_set_text(GTK_LABEL(status_label), "注册失败，用户名已存在");
    } else if (strcmp(buffer, "REGISTER_FAILED_INVALID_INPUT") == 0) {
        gtk_label_set_text(GTK_LABEL(status_label), "注册失败，输入格式不正确");
    } else if (strcmp(buffer, "REGISTER_FAILED_SERVER_ERROR") == 0 ||
               strcmp(buffer, "REGISTER_FAILED") == 0) {
        gtk_label_set_text(GTK_LABEL(status_label), "注册失败，请稍后重试");
    } else {
        gtk_label_set_text(GTK_LABEL(status_label), "注册失败，服务器无响应");
    }

    close(sockfd);
    sockfd = -1;
}

void *receive_messages(void *arg);

int start_receive_thread(void) {
    if (recv_thread_running) {
        return 0;
    }

    recv_thread_running = 1;
    if (pthread_create(&recv_thread, NULL, receive_messages, NULL) != 0) {
        perror("pthread_create receive thread");
        recv_thread_running = 0;
        return -1;
    }

    pthread_detach(recv_thread);
    return 0;
}

void on_login_clicked(GtkButton *button, gpointer user_data) {
    (void)button;

    GtkEntry *username_entry = GTK_ENTRY(g_object_get_data(G_OBJECT(user_data), "username_entry"));
    GtkEntry *password_entry = GTK_ENTRY(g_object_get_data(G_OBJECT(user_data), "password_entry"));

    const char *username = gtk_entry_get_text(username_entry);
    const char *password = gtk_entry_get_text(password_entry);

    if (strlen(username) == 0 || strlen(password) == 0) {
        gtk_label_set_text(GTK_LABEL(status_label), "请填写用户名和密码");
        return;
    }
    if (strlen(username) >= 50 || strlen(password) >= 50) {
        gtk_label_set_text(GTK_LABEL(status_label), "用户名和密码不能超过49字节");
        return;
    }
    if (!is_client_protocol_safe(username, FALSE) ||
        !is_client_protocol_safe(password, FALSE)) {
        gtk_label_set_text(GTK_LABEL(status_label), "用户名和密码不能包含逗号、冒号、分号或换行");
        return;
    }

    connect_to_server();
    if (sockfd < 0) {
        gtk_label_set_text(GTK_LABEL(status_label), "无法连接服务器");
        return;
    }

    char msg[BUFFER_SIZE];
    if (snprintf(msg, sizeof(msg), "LOGIN:%s,%s", username, password) >= (int)sizeof(msg)) {
        gtk_label_set_text(GTK_LABEL(status_label), "登录信息过长");
        close(sockfd);
        sockfd = -1;
        return;
    }
    send_message_to_server(msg);

    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);
    recv(sockfd, buffer, BUFFER_SIZE - 1, 0);

    if (strncmp(buffer, "LOGIN_SUCCESS:", 14) == 0 &&
        sscanf(buffer + 14, "%d:%49[^:]:%49[^\n]", &current_user_id, current_username, current_nickname) == 3) {
        gtk_label_set_text(GTK_LABEL(status_label), "登录成功");

        if (start_receive_thread() != 0) {
            gtk_label_set_text(GTK_LABEL(status_label), "接收线程启动失败");
            close(sockfd);
            sockfd = -1;
            return;
        }

        gtk_stack_set_visible_child(GTK_STACK(main_stack), chat_window);

        char title[100];
        snprintf(title, sizeof(title), "Linux聊天工具 - %s", current_nickname);
        gtk_window_set_title(GTK_WINDOW(window), title);

        snprintf(msg, sizeof(msg), "FRIENDS:%d", current_user_id);
        send_message_to_server(msg);
    } else {
        gtk_label_set_text(GTK_LABEL(status_label), "登录失败，用户名或密码错误");
        close(sockfd);
        sockfd = -1;
    }
}

void on_send_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    const char *text = gtk_entry_get_text(GTK_ENTRY(message_entry));

    if (strlen(text) == 0 || (!selected_is_group && selected_friend_id < 0) ||
        (selected_is_group && selected_group_id < 0)) {
        return;
    }
    if (strlen(text) >= 900) {
        gtk_label_set_text(GTK_LABEL(status_label), "消息内容过长");
        return;
    }
    if (!is_client_protocol_safe(text, TRUE)) {
        gtk_label_set_text(GTK_LABEL(status_label), "消息不能包含冒号、分号或换行");
        return;
    }

    char msg[BUFFER_SIZE];
    int written;
    if (selected_is_group) {
        written = snprintf(msg, sizeof(msg), "SEND_GROUP:%d,%d,%s",
                           current_user_id, selected_group_id, text);
    } else {
        written = snprintf(msg, sizeof(msg), "SEND:%d,%d,%s",
                           current_user_id, selected_friend_id, text);
    }
    if (written >= (int)sizeof(msg)) {
        gtk_label_set_text(GTK_LABEL(status_label), "消息内容过长");
        return;
    }
    send_message_to_server(msg);

    gtk_entry_set_text(GTK_ENTRY(message_entry), "");
}

void on_friend_selected(GtkTreeView *tree_view, gpointer user_data) {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(tree_view);
    GtkTreeModel *model;
    GtkTreeIter iter;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gint friend_id;
        gchar *nickname;
        gchar *username;

        gtk_tree_model_get(model, &iter, 0, &friend_id, 1, &nickname, 2, &username, -1);

        selected_friend_id = friend_id;
        snprintf(selected_friend_username, sizeof(selected_friend_username), "%s", username);
        selected_group_id = -1;
        selected_is_group = FALSE;

        char title[100];
        snprintf(title, sizeof(title), "与 %s 聊天", nickname);
        gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(user_data), "chat_title")), title);

        g_free(nickname);
        g_free(username);

        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(message_view)), "", -1);

        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "MESSAGES:%d,%d", current_user_id, selected_friend_id);
        send_message_to_server(msg);
    }
}

void on_group_selected(GtkTreeView *tree_view, gpointer user_data) {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(tree_view);
    GtkTreeModel *model;
    GtkTreeIter iter;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gint group_id;
        gchar *group_name;

        gtk_tree_model_get(model, &iter, 0, &group_id, 1, &group_name, -1);

        selected_group_id = group_id;
        selected_friend_id = -1;
        selected_friend_username[0] = '\0';
        selected_is_group = TRUE;

        char title[120];
        snprintf(title, sizeof(title), "群聊：%s", group_name);
        gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(user_data), "chat_title")), title);

        g_free(group_name);

        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(message_view)), "", -1);

        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "GROUP_MESSAGES:%d", selected_group_id);
        send_message_to_server(msg);
        snprintf(msg, sizeof(msg), "GROUP_MEMBERS:%d", selected_group_id);
        send_message_to_server(msg);
    }
}

void on_add_friend_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons("添加好友", GTK_WINDOW(window),
                                                    GTK_DIALOG_MODAL, "确定", GTK_RESPONSE_OK,
                                                    "取消", GTK_RESPONSE_CANCEL, NULL);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *label = gtk_label_new("请输入好友用户名:");
    GtkWidget *entry = gtk_entry_new();

    gtk_container_add(GTK_CONTAINER(content_area), label);
    gtk_container_add(GTK_CONTAINER(content_area), entry);

    gtk_widget_show_all(content_area);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));

    if (response == GTK_RESPONSE_OK) {
        const char *friend_username = gtk_entry_get_text(GTK_ENTRY(entry));

        if (strlen(friend_username) > 0 &&
            strlen(friend_username) < 50 &&
            strcmp(friend_username, current_username) != 0 &&
            is_client_protocol_safe(friend_username, FALSE)) {
            char msg[BUFFER_SIZE];
            if (snprintf(msg, sizeof(msg), "ADDFRIEND_USERNAME:%s", friend_username) < (int)sizeof(msg)) {
                send_message_to_server(msg);
            }
        } else {
            gtk_label_set_text(GTK_LABEL(status_label), "请输入有效的好友用户名");
        }
    }

    gtk_widget_destroy(dialog);
}

void on_create_group_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons("创建群聊", GTK_WINDOW(window),
                                                    GTK_DIALOG_MODAL, "确定", GTK_RESPONSE_OK,
                                                    "取消", GTK_RESPONSE_CANCEL, NULL);
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *name_label = gtk_label_new("群名称:");
    GtkWidget *name_entry = gtk_entry_new();
    GtkWidget *members_label = gtk_label_new("初始成员ID（逗号分隔，可留空）:");
    GtkWidget *members_entry = gtk_entry_new();

    gtk_container_add(GTK_CONTAINER(content_area), name_label);
    gtk_container_add(GTK_CONTAINER(content_area), name_entry);
    gtk_container_add(GTK_CONTAINER(content_area), members_label);
    gtk_container_add(GTK_CONTAINER(content_area), members_entry);
    gtk_widget_show_all(content_area);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(name_entry));
        const char *members = gtk_entry_get_text(GTK_ENTRY(members_entry));

        if (strlen(name) > 0 && strlen(name) < 80 &&
            is_client_protocol_safe(name, FALSE) &&
            (strlen(members) == 0 || is_client_protocol_safe(members, TRUE))) {
            char msg[BUFFER_SIZE];
            int written;
            if (strlen(members) > 0) {
                written = snprintf(msg, sizeof(msg), "CREATE_GROUP:%s,%s", name, members);
            } else {
                written = snprintf(msg, sizeof(msg), "CREATE_GROUP:%s", name);
            }
            if (written < (int)sizeof(msg)) {
                send_message_to_server(msg);
            }
        } else {
            gtk_label_set_text(GTK_LABEL(status_label), "群名称或成员ID格式不正确");
        }
    }

    gtk_widget_destroy(dialog);
}

void on_add_group_member_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    if (selected_group_id <= 0) {
        gtk_label_set_text(GTK_LABEL(status_label), "请先选择群聊");
        return;
    }

    GtkWidget *dialog = gtk_dialog_new_with_buttons("添加群成员", GTK_WINDOW(window),
                                                    GTK_DIALOG_MODAL, "确定", GTK_RESPONSE_OK,
                                                    "取消", GTK_RESPONSE_CANCEL, NULL);
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *label = gtk_label_new("请输入成员用户名:");
    GtkWidget *entry = gtk_entry_new();

    gtk_container_add(GTK_CONTAINER(content_area), label);
    gtk_container_add(GTK_CONTAINER(content_area), entry);
    gtk_widget_show_all(content_area);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const char *member_username = gtk_entry_get_text(GTK_ENTRY(entry));
        if (strlen(member_username) > 0 &&
            strlen(member_username) < 50 &&
            is_client_protocol_safe(member_username, FALSE)) {
            char msg[BUFFER_SIZE];
            if (snprintf(msg, sizeof(msg), "ADD_GROUP_MEMBER_USERNAME:%d,%s",
                         selected_group_id, member_username) < (int)sizeof(msg)) {
                send_message_to_server(msg);
            }
        } else {
            gtk_label_set_text(GTK_LABEL(status_label), "请输入有效的成员用户名");
        }
    }

    gtk_widget_destroy(dialog);
}

void on_block_user_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    if (selected_friend_id <= 0 || selected_friend_username[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(status_label), "请先选择要屏蔽的好友");
        return;
    }

    char msg[BUFFER_SIZE];
    if (snprintf(msg, sizeof(msg), "BLOCK_USER_USERNAME:%s", selected_friend_username) < (int)sizeof(msg)) {
        send_message_to_server(msg);
    }
}

void on_unblock_user_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons("解除屏蔽", GTK_WINDOW(window),
                                                    GTK_DIALOG_MODAL, "确定", GTK_RESPONSE_OK,
                                                    "取消", GTK_RESPONSE_CANCEL, NULL);
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *label = gtk_label_new("请输入用户名:");
    GtkWidget *entry = gtk_entry_new();

    gtk_container_add(GTK_CONTAINER(content_area), label);
    gtk_container_add(GTK_CONTAINER(content_area), entry);
    gtk_widget_show_all(content_area);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const char *blocked_username = gtk_entry_get_text(GTK_ENTRY(entry));
        if (strlen(blocked_username) > 0 &&
            strlen(blocked_username) < 50 &&
            is_client_protocol_safe(blocked_username, FALSE)) {
            char msg[BUFFER_SIZE];
            if (snprintf(msg, sizeof(msg), "UNBLOCK_USER_USERNAME:%s", blocked_username) < (int)sizeof(msg)) {
                send_message_to_server(msg);
            }
        } else {
            gtk_label_set_text(GTK_LABEL(status_label), "请输入有效的用户名");
        }
    }

    gtk_widget_destroy(dialog);
}

gboolean parse_friends_list(gpointer data) {
    char *buffer = (char *)data;
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(friends_list)));
    gtk_list_store_clear(store);

    const char *ptr = buffer + 14;
    char friend_id[10], username[50], nickname[50];

    while (sscanf(ptr, "%9[^:]:%49[^:]:%49[^;];", friend_id, username, nickname) == 3) {
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, atoi(friend_id), 1, nickname, 2, username, -1);

        ptr += strlen(friend_id) + strlen(username) + strlen(nickname) + 3;
    }

    g_free(buffer);
    return G_SOURCE_REMOVE;
}

gboolean parse_groups_list(gpointer data) {
    char *buffer = (char *)data;
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(groups_list)));
    gtk_list_store_clear(store);

    const char *ptr = buffer + 12;
    char group_id[10], group_name[80], owner_nickname[50];

    while (sscanf(ptr, "%9[^:]:%79[^:]:%49[^;];", group_id, group_name, owner_nickname) == 3) {
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, atoi(group_id), 1, group_name, -1);

        ptr += strlen(group_id) + strlen(group_name) + strlen(owner_nickname) + 3;
    }

    g_free(buffer);
    return G_SOURCE_REMOVE;
}

gboolean parse_group_members_list(gpointer data) {
    char *buffer = (char *)data;
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(members_list)));
    gtk_list_store_clear(store);

    const char *ptr = buffer + 19;
    char member_id[10], username[50], nickname[50];

    while (sscanf(ptr, "%9[^:]:%49[^:]:%49[^;];", member_id, username, nickname) == 3) {
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, 0, atoi(member_id), 1, nickname, 2, member_id, -1);

        ptr += strlen(member_id) + strlen(username) + strlen(nickname) + 3;
    }

    g_free(buffer);
    return G_SOURCE_REMOVE;
}

gboolean parse_messages_list(gpointer data) {
    char *buffer = (char *)data;
    GtkTextBuffer *text_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(message_view));
    gtk_text_buffer_set_text(text_buffer, "", -1);

    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(text_buffer, &iter);

    const char *ptr = buffer + 15;
    char content[BUFFER_SIZE], timestamp[50], nickname[50];

    while (sscanf(ptr, "%1023[^:]:%49[^:]:%49[^;];", content, timestamp, nickname) == 3) {
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "%s [%s]: %s\n", nickname, timestamp, content);

        gtk_text_buffer_insert(text_buffer, &iter, msg, -1);

        ptr += strlen(content) + strlen(timestamp) + strlen(nickname) + 3;
    }

    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(message_view),
                                  gtk_text_buffer_get_insert(text_buffer), 0.0, FALSE, 0.0, 0.0);

    g_free(buffer);
    return G_SOURCE_REMOVE;
}

gboolean parse_group_messages_list(gpointer data) {
    char *buffer = (char *)data;
    GtkTextBuffer *text_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(message_view));
    gtk_text_buffer_set_text(text_buffer, "", -1);

    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(text_buffer, &iter);

    const char *ptr = buffer + 20;
    char content[BUFFER_SIZE], timestamp[50], nickname[50];

    while (sscanf(ptr, "%1023[^:]:%49[^:]:%49[^;];", content, timestamp, nickname) == 3) {
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "%s [%s]: %s\n", nickname, timestamp, content);

        gtk_text_buffer_insert(text_buffer, &iter, msg, -1);

        ptr += strlen(content) + strlen(timestamp) + strlen(nickname) + 3;
    }

    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(message_view),
                                  gtk_text_buffer_get_insert(text_buffer), 0.0, FALSE, 0.0, 0.0);

    g_free(buffer);
    return G_SOURCE_REMOVE;
}

gboolean parse_new_message(gpointer data) {
    char *buffer = (char *)data;
    int sender_id;
    char sender_nickname[50], content[BUFFER_SIZE];

    if (sscanf(buffer + 12, "%d:%49[^:]:%1023[^\n]", &sender_id, sender_nickname, content) == 3) {
        if (sender_id == selected_friend_id || sender_id == current_user_id) {
            GtkTextBuffer *text_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(message_view));
            GtkTextIter iter;
            gtk_text_buffer_get_end_iter(text_buffer, &iter);

            char msg[BUFFER_SIZE];
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            char time_str[50];
            strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

            snprintf(msg, sizeof(msg), "%s [%s]: %s\n", sender_nickname, time_str, content);
            gtk_text_buffer_insert(text_buffer, &iter, msg, -1);

            gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(message_view),
                                          gtk_text_buffer_get_insert(text_buffer), 0.0, FALSE, 0.0, 0.0);
        }
    }

    g_free(buffer);
    return G_SOURCE_REMOVE;
}

gboolean parse_new_group_message(gpointer data) {
    char *buffer = (char *)data;
    int group_id, sender_id;
    char sender_nickname[50], content[BUFFER_SIZE];

    if (sscanf(buffer + 18, "%d:%d:%49[^:]:%1023[^\n]",
               &group_id, &sender_id, sender_nickname, content) == 4) {
        if (selected_is_group && group_id == selected_group_id) {
            GtkTextBuffer *text_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(message_view));
            GtkTextIter iter;
            gtk_text_buffer_get_end_iter(text_buffer, &iter);

            char msg[BUFFER_SIZE];
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            char time_str[50];
            strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

            snprintf(msg, sizeof(msg), "%s [%s]: %s\n", sender_nickname, time_str, content);
            gtk_text_buffer_insert(text_buffer, &iter, msg, -1);

            gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(message_view),
                                          gtk_text_buffer_get_insert(text_buffer), 0.0, FALSE, 0.0, 0.0);
        } else {
            char notice[120];
            snprintf(notice, sizeof(notice), "群聊 %d 有新消息", group_id);
            gtk_label_set_text(GTK_LABEL(status_label), notice);
        }
    }

    (void)sender_id;
    g_free(buffer);
    return G_SOURCE_REMOVE;
}

gboolean parse_friend_status(gpointer data) {
    char *buffer = (char *)data;
    int user_id;
    char nickname[50];
    char notice[120];

    if (strncmp(buffer, "FRIEND_ONLINE:", 14) == 0 &&
        sscanf(buffer + 14, "%d:%49[^\n]", &user_id, nickname) == 2) {
        snprintf(notice, sizeof(notice), "%s 已上线", nickname);
        gtk_label_set_text(GTK_LABEL(status_label), notice);
    } else if (strncmp(buffer, "FRIEND_OFFLINE:", 15) == 0 &&
               sscanf(buffer + 15, "%d:%49[^\n]", &user_id, nickname) == 2) {
        snprintf(notice, sizeof(notice), "%s 已离线", nickname);
        gtk_label_set_text(GTK_LABEL(status_label), notice);
    }

    (void)user_id;
    g_free(buffer);
    return G_SOURCE_REMOVE;
}

gboolean parse_offline_messages(gpointer data) {
    char *buffer = (char *)data;
    const char *ptr = buffer + 22;
    char type[16], related_id[16], count[16];
    int total = 0;
    char notice[160] = "没有未读消息";

    while (sscanf(ptr, "%15[^:]:%15[^:]:%15[^;];", type, related_id, count) == 3) {
        total += atoi(count);
        ptr += strlen(type) + strlen(related_id) + strlen(count) + 3;
    }

    if (total > 0) {
        snprintf(notice, sizeof(notice), "你有 %d 条未读消息，可打开好友或群聊历史查看", total);
    }
    gtk_label_set_text(GTK_LABEL(status_label), notice);

    g_free(buffer);
    return G_SOURCE_REMOVE;
}

gboolean refresh_friends_after_add(gpointer data) {
    (void)data;

    if (current_user_id > 0) {
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "FRIENDS:%d", current_user_id);
        send_message_to_server(msg);
    }

    return G_SOURCE_REMOVE;
}

gboolean refresh_groups_after_change(gpointer data) {
    (void)data;

    if (current_user_id > 0) {
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "GROUPS:%d", current_user_id);
        send_message_to_server(msg);
        if (selected_group_id > 0) {
            snprintf(msg, sizeof(msg), "GROUP_MEMBERS:%d", selected_group_id);
            send_message_to_server(msg);
        }
    }

    return G_SOURCE_REMOVE;
}

gboolean show_block_status(gpointer data) {
    (void)data;

    gtk_label_set_text(GTK_LABEL(status_label), "屏蔽设置已更新");
    return G_SOURCE_REMOVE;
}

void *receive_messages(void *arg) {
    (void)arg;
    char buffer[BUFFER_SIZE];

    while (sockfd >= 0) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_read = recv(sockfd, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_read <= 0) {
            printf("服务器断开连接\n");
            break;
        }

        printf("收到消息: %s\n", buffer);

        if (strncmp(buffer, "FRIENDS_LIST:", 14) == 0) {
            gdk_threads_add_idle(parse_friends_list, g_strdup(buffer));
            if (current_user_id > 0) {
                char msg[BUFFER_SIZE];
                snprintf(msg, sizeof(msg), "GROUPS:%d", current_user_id);
                send_message_to_server(msg);
            }
        } else if (strncmp(buffer, "GROUPS_LIST:", 12) == 0) {
            gdk_threads_add_idle(parse_groups_list, g_strdup(buffer));
            if (current_user_id > 0) {
                char msg[BUFFER_SIZE];
                snprintf(msg, sizeof(msg), "OFFLINE_MESSAGES:%d", current_user_id);
                send_message_to_server(msg);
            }
        } else if (strncmp(buffer, "GROUP_MEMBERS_LIST:", 19) == 0) {
            gdk_threads_add_idle(parse_group_members_list, g_strdup(buffer));
        } else if (strncmp(buffer, "MESSAGES_LIST:", 15) == 0) {
            gdk_threads_add_idle(parse_messages_list, g_strdup(buffer));
        } else if (strncmp(buffer, "GROUP_MESSAGES_LIST:", 20) == 0) {
            gdk_threads_add_idle(parse_group_messages_list, g_strdup(buffer));
        } else if (strncmp(buffer, "NEW_MESSAGE:", 12) == 0) {
            gdk_threads_add_idle(parse_new_message, g_strdup(buffer));
        } else if (strncmp(buffer, "NEW_GROUP_MESSAGE:", 18) == 0) {
            gdk_threads_add_idle(parse_new_group_message, g_strdup(buffer));
        } else if (strncmp(buffer, "ADDFRIEND_SUCCESS", 17) == 0) {
            gdk_threads_add_idle(refresh_friends_after_add, NULL);
        } else if (strncmp(buffer, "CREATE_GROUP_SUCCESS", 20) == 0 ||
                   strncmp(buffer, "ADD_GROUP_MEMBER_SUCCESS", 24) == 0) {
            gdk_threads_add_idle(refresh_groups_after_change, NULL);
        } else if (strncmp(buffer, "BLOCK_USER_SUCCESS", 18) == 0) {
            gdk_threads_add_idle(show_block_status, NULL);
        } else if (strncmp(buffer, "UNBLOCK_USER_SUCCESS", 20) == 0) {
            gdk_threads_add_idle(show_block_status, NULL);
        } else if (strncmp(buffer, "FRIEND_ONLINE:", 14) == 0 ||
                   strncmp(buffer, "FRIEND_OFFLINE:", 15) == 0) {
            gdk_threads_add_idle(parse_friend_status, g_strdup(buffer));
        } else if (strncmp(buffer, "OFFLINE_MESSAGES_LIST:", 22) == 0) {
            gdk_threads_add_idle(parse_offline_messages, g_strdup(buffer));
        }
    }

    recv_thread_running = 0;
    return NULL;
}

void build_login_window(void) {
    login_window = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    GtkWidget *vbox = GTK_WIDGET(login_window);

    GtkWidget *username_label = gtk_label_new("用户名:");
    GtkWidget *username_entry = gtk_entry_new();

    GtkWidget *password_label = gtk_label_new("密码:");
    GtkWidget *password_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(password_entry), FALSE);

    GtkWidget *nickname_label = gtk_label_new("昵称:");
    GtkWidget *nickname_entry = gtk_entry_new();

    status_label = gtk_label_new("");

    GtkWidget *register_button = gtk_button_new_with_label("注册");
    GtkWidget *login_button = gtk_button_new_with_label("登录");

    gtk_container_add(GTK_CONTAINER(vbox), username_label);
    gtk_container_add(GTK_CONTAINER(vbox), username_entry);
    gtk_container_add(GTK_CONTAINER(vbox), password_label);
    gtk_container_add(GTK_CONTAINER(vbox), password_entry);
    gtk_container_add(GTK_CONTAINER(vbox), nickname_label);
    gtk_container_add(GTK_CONTAINER(vbox), nickname_entry);
    gtk_container_add(GTK_CONTAINER(vbox), status_label);
    gtk_container_add(GTK_CONTAINER(vbox), register_button);
    gtk_container_add(GTK_CONTAINER(vbox), login_button);

    g_object_set_data(G_OBJECT(login_window), "username_entry", username_entry);
    g_object_set_data(G_OBJECT(login_window), "password_entry", password_entry);
    g_object_set_data(G_OBJECT(login_window), "nickname_entry", nickname_entry);

    g_signal_connect(register_button, "clicked", G_CALLBACK(on_register_clicked), login_window);
    g_signal_connect(login_button, "clicked", G_CALLBACK(on_login_clicked), login_window);
}

void build_chat_window(void) {
    chat_window = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_hexpand(chat_window, TRUE);
    gtk_widget_set_vexpand(chat_window, TRUE);

    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_size_request(sidebar, 220, -1);
    gtk_widget_set_vexpand(sidebar, TRUE);

    GtkWidget *user_info = gtk_frame_new("用户信息");
    GtkWidget *user_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *user_label = gtk_label_new("");
    GtkWidget *add_friend_button = gtk_button_new_with_label("添加好友");
    GtkWidget *create_group_button = gtk_button_new_with_label("创建群聊");
    GtkWidget *add_group_member_button = gtk_button_new_with_label("添加群成员");
    GtkWidget *block_user_button = gtk_button_new_with_label("屏蔽好友");
    GtkWidget *unblock_user_button = gtk_button_new_with_label("解除屏蔽");
    gtk_container_add(GTK_CONTAINER(user_vbox), user_label);
    gtk_container_add(GTK_CONTAINER(user_vbox), add_friend_button);
    gtk_container_add(GTK_CONTAINER(user_vbox), create_group_button);
    gtk_container_add(GTK_CONTAINER(user_vbox), add_group_member_button);
    gtk_container_add(GTK_CONTAINER(user_vbox), block_user_button);
    gtk_container_add(GTK_CONTAINER(user_vbox), unblock_user_button);
    gtk_container_add(GTK_CONTAINER(user_info), user_vbox);

    GtkWidget *friends_frame = gtk_frame_new("好友列表");

    GtkListStore *store = gtk_list_store_new(3, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    friends_list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(friends_list), FALSE);

    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("好友", renderer,
                                                                          "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(friends_list), column);

    GtkWidget *friends_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(friends_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(friends_scroll), 100);
    gtk_widget_set_vexpand(friends_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(friends_scroll), friends_list);
    gtk_container_add(GTK_CONTAINER(friends_frame), friends_scroll);

    GtkWidget *groups_frame = gtk_frame_new("群聊列表");
    GtkListStore *groups_store = gtk_list_store_new(2, G_TYPE_INT, G_TYPE_STRING);
    groups_list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(groups_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(groups_list), FALSE);
    GtkCellRenderer *group_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *group_column = gtk_tree_view_column_new_with_attributes("群聊", group_renderer,
                                                                                "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(groups_list), group_column);
    GtkWidget *groups_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(groups_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(groups_scroll), 120);
    gtk_widget_set_vexpand(groups_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(groups_scroll), groups_list);
    gtk_container_add(GTK_CONTAINER(groups_frame), groups_scroll);

    GtkWidget *members_frame = gtk_frame_new("群成员");
    GtkListStore *members_store = gtk_list_store_new(3, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    members_list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(members_store));
    GtkCellRenderer *member_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *member_column = gtk_tree_view_column_new_with_attributes("成员", member_renderer,
                                                                                "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(members_list), member_column);
    GtkCellRenderer *member_id_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *member_id_column = gtk_tree_view_column_new_with_attributes("ID", member_id_renderer,
                                                                                   "text", 2, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(members_list), member_id_column);
    GtkWidget *members_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(members_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(members_scroll), 120);
    gtk_widget_set_vexpand(members_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(members_scroll), members_list);
    gtk_container_add(GTK_CONTAINER(members_frame), members_scroll);

    gtk_box_pack_start(GTK_BOX(sidebar), user_info, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar), friends_frame, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar), groups_frame, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar), members_frame, TRUE, TRUE, 0);

    GtkWidget *chat_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_hexpand(chat_area, TRUE);
    gtk_widget_set_vexpand(chat_area, TRUE);

    GtkWidget *chat_header = gtk_frame_new("聊天");
    GtkWidget *chat_title = gtk_label_new("选择好友开始聊天");
    gtk_container_add(GTK_CONTAINER(chat_header), chat_title);
    gtk_widget_set_hexpand(chat_header, TRUE);

    message_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(message_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(message_view), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_hexpand(message_view, TRUE);
    gtk_widget_set_vexpand(message_view, TRUE);
    GtkWidget *message_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(message_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(message_scroll, TRUE);
    gtk_widget_set_vexpand(message_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(message_scroll), message_view);

    GtkWidget *entry_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_hexpand(entry_box, TRUE);
    message_entry = gtk_entry_new();
    gtk_widget_set_hexpand(message_entry, TRUE);
    GtkWidget *send_button = gtk_button_new_with_label("发送");
    gtk_widget_set_size_request(send_button, 90, -1);

    gtk_box_pack_start(GTK_BOX(entry_box), message_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(entry_box), send_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(chat_area), chat_header, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(chat_area), message_scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(chat_area), entry_box, FALSE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(chat_window), sidebar, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(chat_window), chat_area, TRUE, TRUE, 0);

    g_object_set_data(G_OBJECT(chat_window), "chat_title", chat_title);

    g_signal_connect(add_friend_button, "clicked", G_CALLBACK(on_add_friend_clicked), chat_window);
    g_signal_connect(create_group_button, "clicked", G_CALLBACK(on_create_group_clicked), chat_window);
    g_signal_connect(add_group_member_button, "clicked", G_CALLBACK(on_add_group_member_clicked), chat_window);
    g_signal_connect(block_user_button, "clicked", G_CALLBACK(on_block_user_clicked), chat_window);
    g_signal_connect(unblock_user_button, "clicked", G_CALLBACK(on_unblock_user_clicked), chat_window);
    g_signal_connect(friends_list, "cursor-changed", G_CALLBACK(on_friend_selected), chat_window);
    g_signal_connect(groups_list, "cursor-changed", G_CALLBACK(on_group_selected), chat_window);
    g_signal_connect(send_button, "clicked", G_CALLBACK(on_send_clicked), chat_window);
    g_signal_connect(message_entry, "activate", G_CALLBACK(on_send_clicked), chat_window);

}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Linux聊天工具");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);

    main_stack = gtk_stack_new();
    build_login_window();
    build_chat_window();

    gtk_stack_add_named(GTK_STACK(main_stack), login_window, "login");
    gtk_stack_add_named(GTK_STACK(main_stack), chat_window, "chat");
    gtk_container_add(GTK_CONTAINER(window), main_stack);

    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(window);

    gtk_stack_set_visible_child(GTK_STACK(main_stack), login_window);

    gtk_main();

    if (sockfd >= 0) {
        close(sockfd);
    }

    return 0;
}

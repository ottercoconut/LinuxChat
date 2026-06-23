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

GtkWidget *window;
GtkWidget *main_stack;
GtkWidget *login_window;
GtkWidget *chat_window;
GtkWidget *message_view;
GtkWidget *message_entry;
GtkWidget *friends_list;
GtkWidget *status_label;

GList *friend_items = NULL;
pthread_t recv_thread;
int recv_thread_running = 0;

void send_message_to_server(const char *msg) {
    if (sockfd >= 0) {
        send(sockfd, msg, strlen(msg), 0);
    }
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
    
    if (strlen(username) == 0 || strlen(password) == 0 || strlen(nickname) == 0) {
        gtk_label_set_text(GTK_LABEL(status_label), "请填写完整信息");
        return;
    }
    
    connect_to_server();
    if (sockfd < 0) {
        gtk_label_set_text(GTK_LABEL(status_label), "无法连接服务器");
        return;
    }
    
    char msg[BUFFER_SIZE];
    sprintf(msg, "REGISTER:%s,%s,%s", username, password, nickname);
    send_message_to_server(msg);
    
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);
    recv(sockfd, buffer, BUFFER_SIZE - 1, 0);
    
    if (strncmp(buffer, "REGISTER_SUCCESS:", 18) == 0) {
        gtk_label_set_text(GTK_LABEL(status_label), "注册成功，请登录");
        close(sockfd);
        sockfd = -1;
    } else {
        gtk_label_set_text(GTK_LABEL(status_label), "注册失败，用户名已存在");
        close(sockfd);
        sockfd = -1;
    }
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
    
    connect_to_server();
    if (sockfd < 0) {
        gtk_label_set_text(GTK_LABEL(status_label), "无法连接服务器");
        return;
    }
    
    char msg[BUFFER_SIZE];
    sprintf(msg, "LOGIN:%s,%s", username, password);
    send_message_to_server(msg);
    
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);
    recv(sockfd, buffer, BUFFER_SIZE - 1, 0);
    
    if (strncmp(buffer, "LOGIN_SUCCESS:", 14) == 0) {
        sscanf(buffer + 14, "%d:%[^:]:%s", &current_user_id, current_username, current_nickname);
        gtk_label_set_text(GTK_LABEL(status_label), "登录成功");

        if (start_receive_thread() != 0) {
            gtk_label_set_text(GTK_LABEL(status_label), "接收线程启动失败");
            close(sockfd);
            sockfd = -1;
            return;
        }

        gtk_stack_set_visible_child(GTK_STACK(main_stack), chat_window);
        
        char title[100];
        sprintf(title, "Linux聊天工具 - %s", current_nickname);
        gtk_window_set_title(GTK_WINDOW(window), title);
        
        sprintf(msg, "FRIENDS:%d", current_user_id);
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
    
    if (strlen(text) == 0 || selected_friend_id < 0) {
        return;
    }
    
    char msg[BUFFER_SIZE];
    sprintf(msg, "SEND:%d,%d,%s", current_user_id, selected_friend_id, text);
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
        
        gtk_tree_model_get(model, &iter, 0, &friend_id, 1, &nickname, -1);
        
        selected_friend_id = friend_id;
        
        char title[100];
        sprintf(title, "与 %s 聊天", nickname);
        gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(user_data), "chat_title")), title);
        
        g_free(nickname);
        
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(message_view)), "", -1);
        
        char msg[BUFFER_SIZE];
        sprintf(msg, "MESSAGES:%d,%d", current_user_id, selected_friend_id);
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
    GtkWidget *label = gtk_label_new("请输入好友ID:");
    GtkWidget *entry = gtk_entry_new();
    
    gtk_container_add(GTK_CONTAINER(content_area), label);
    gtk_container_add(GTK_CONTAINER(content_area), entry);
    
    gtk_widget_show_all(content_area);
    
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    
    if (response == GTK_RESPONSE_OK) {
        const char *friend_id_str = gtk_entry_get_text(GTK_ENTRY(entry));
        int friend_id = atoi(friend_id_str);
        
        if (friend_id > 0 && friend_id != current_user_id) {
            char msg[BUFFER_SIZE];
            sprintf(msg, "ADDFRIEND:%d,%d", current_user_id, friend_id);
            send_message_to_server(msg);
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
        gtk_list_store_set(store, &iter, 0, atoi(friend_id), 1, nickname, -1);
        
        ptr += strlen(friend_id) + strlen(username) + strlen(nickname) + 3;
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

gboolean refresh_friends_after_add(gpointer data) {
    (void)data;

    if (current_user_id > 0) {
        char msg[BUFFER_SIZE];
        sprintf(msg, "FRIENDS:%d", current_user_id);
        send_message_to_server(msg);
    }

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
        } else if (strncmp(buffer, "MESSAGES_LIST:", 15) == 0) {
            gdk_threads_add_idle(parse_messages_list, g_strdup(buffer));
        } else if (strncmp(buffer, "NEW_MESSAGE:", 12) == 0) {
            gdk_threads_add_idle(parse_new_message, g_strdup(buffer));
        } else if (strncmp(buffer, "ADDFRIEND_SUCCESS", 17) == 0) {
            gdk_threads_add_idle(refresh_friends_after_add, NULL);
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
    
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    
    GtkWidget *user_info = gtk_frame_new("用户信息");
    GtkWidget *user_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *user_label = gtk_label_new("");
    GtkWidget *add_friend_button = gtk_button_new_with_label("添加好友");
    gtk_container_add(GTK_CONTAINER(user_vbox), user_label);
    gtk_container_add(GTK_CONTAINER(user_vbox), add_friend_button);
    gtk_container_add(GTK_CONTAINER(user_info), user_vbox);
    
    GtkWidget *friends_frame = gtk_frame_new("好友列表");
    
    GtkListStore *store = gtk_list_store_new(2, G_TYPE_INT, G_TYPE_STRING);
    friends_list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("好友", renderer,
                                                                          "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(friends_list), column);
    
    gtk_container_add(GTK_CONTAINER(friends_frame), friends_list);
    
    gtk_container_add(GTK_CONTAINER(sidebar), user_info);
    gtk_container_add(GTK_CONTAINER(sidebar), friends_frame);
    
    GtkWidget *chat_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    
    GtkWidget *chat_header = gtk_frame_new("聊天");
    GtkWidget *chat_title = gtk_label_new("选择好友开始聊天");
    gtk_container_add(GTK_CONTAINER(chat_header), chat_title);
    
    message_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(message_view), FALSE);
    
    GtkWidget *entry_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    message_entry = gtk_entry_new();
    GtkWidget *send_button = gtk_button_new_with_label("发送");
    
    gtk_container_add(GTK_CONTAINER(entry_box), message_entry);
    gtk_container_add(GTK_CONTAINER(entry_box), send_button);
    
    gtk_container_add(GTK_CONTAINER(chat_area), chat_header);
    gtk_container_add(GTK_CONTAINER(chat_area), message_view);
    gtk_container_add(GTK_CONTAINER(chat_area), entry_box);
    
    gtk_container_add(GTK_CONTAINER(chat_window), sidebar);
    gtk_container_add(GTK_CONTAINER(chat_window), chat_area);
    
    g_object_set_data(G_OBJECT(chat_window), "chat_title", chat_title);
    
    g_signal_connect(add_friend_button, "clicked", G_CALLBACK(on_add_friend_clicked), chat_window);
    g_signal_connect(friends_list, "cursor-changed", G_CALLBACK(on_friend_selected), chat_window);
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

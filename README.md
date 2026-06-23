# LinuxChat

LinuxChat 是一个 Linux 程序设计课程实践项目，也是一个基于 Linux 平台的 C 语言网络即时通信工具。项目采用 Client/Server 架构实现，包含 TCP Socket 通信、多线程服务端、MySQL 数据持久化和 GTK3 图形客户端。

## 功能特性

- 用户注册与登录
- 好友添加与好友列表查询
- 一对一实时文本聊天
- 聊天记录持久化存储
- GTK3 图形客户端
- 多线程 TCP 服务端

## 技术栈

| 模块 | 技术 |
| --- | --- |
| 服务端 | C, POSIX Socket, pthread |
| 客户端 | C, GTK3 |
| 数据库 | MySQL, MySQL C API |
| 通信方式 | TCP 文本协议 |
| 默认端口 | 8888 |

## 目录结构

```text
.
├── README.md
├── c-native/
│   ├── client/
│   │   └── client.c
│   ├── database/
│   │   └── init.sql
│   ├── docs/
│   │   ├── 操作文档.md
│   │   └── 课程设计报告.md
│   └── server/
│       └── server.c
└── .gitignore
```

## 环境依赖

Ubuntu/Debian:

```bash
sudo apt update
sudo apt install gcc make mysql-server libmysqlclient-dev libgtk-3-dev pkg-config
```

macOS:

```bash
brew install mysql gtk+3 pkg-config
brew services start mysql
```

## 数据库初始化

创建数据库和用户：

```bash
mysql -u root -p
```

```sql
CREATE DATABASE chat_db CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER 'chat_user'@'localhost' IDENTIFIED BY 'chat_password';
GRANT ALL PRIVILEGES ON chat_db.* TO 'chat_user'@'localhost';
FLUSH PRIVILEGES;
```

导入初始化脚本：

```bash
mysql -u chat_user -p chat_db < c-native/database/init.sql
```

然后根据本机数据库配置，修改 `c-native/server/server.c` 中的 MySQL 连接参数：

```c
mysql_real_connect(db_conn, "localhost", "chat_user", "chat_password", "chat_db", 0, NULL, 0)
```

macOS 使用 Homebrew 安装 MySQL 后，也可以通过同样的 `mysql` 命令初始化数据库。如果本地 MySQL root 账号未设置密码，可先直接运行 `mysql -u root`，再按上面的 SQL 创建项目数据库和用户。

## 编译运行

编译服务端：

```bash
cd c-native/server
gcc server.c -o server -lmysqlclient -lpthread
```

macOS 如果找不到 MySQL 头文件或链接库，可以改用：

```bash
cd c-native/server
gcc server.c -o server $(mysql_config --cflags --libs) -lpthread
```

编译客户端：

```bash
cd c-native/client
gcc client.c -o client `pkg-config --cflags --libs gtk+-3.0` -lpthread
```

启动服务端：

```bash
cd c-native/server
./server
```

启动客户端：

```bash
cd c-native/client
./client
```

## 通信协议

客户端与服务端使用简单文本协议通信，基本格式为：

```text
COMMAND:param1,param2,param3
```

常用命令：

| 命令 | 参数 | 说明 |
| --- | --- | --- |
| REGISTER | username,password,nickname | 用户注册 |
| LOGIN | username,password | 用户登录 |
| ADDFRIEND | user_id,friend_id | 添加好友 |
| FRIENDS | user_id | 获取好友列表 |
| MESSAGES | user_id,friend_id | 获取历史消息 |
| SEND | sender_id,receiver_id,content | 发送消息 |
| QUIT | 无 | 断开连接 |

## 数据表

项目使用三张 MySQL 表：

- `users`：用户账号、密码、昵称和创建时间
- `friends`：用户之间的好友关系
- `messages`：聊天消息内容、发送方、接收方和时间戳

完整建表脚本见 `c-native/database/init.sql`。

## 文档

- `c-native/docs/操作文档.md`：环境安装、数据库配置、编译运行和使用说明
- `c-native/docs/课程设计报告.md`：项目目标、模块划分、设计实现和测试总结

## 当前状态

当前版本是课程设计/学习用途实现，已覆盖主要功能链路。后续可以继续完善：

- 服务端在线用户数组保存的是 `Client` 副本，登录后线程内状态没有同步回数组，实时转发可能找不到接收方
- 客户端接收线程在连接建立前启动，可能因 `sockfd = -1` 直接退出，导致登录后无法持续接收消息
- GTK 主窗口只添加了登录页，聊天页创建后没有正确加入主容器，登录后的页面切换存在显示问题
- 服务端 `SEND` 分支通过空密码重新调用登录函数获取昵称，容易导致昵称和发送者信息异常
- SQL 使用字符串拼接，存在注入和缓冲区风险，后续应改为 MySQL 预处理语句
- 文本协议依赖 `:`, `,`, `;` 分隔，消息内容包含这些字符时可能解析错误
- 密码明文保存，后续应增加密码哈希和更完整的输入校验

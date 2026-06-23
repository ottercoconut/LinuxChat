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

如果使用 MySQL 官网安装包且 `mysql_config` 不在 `PATH` 中，可以使用完整路径：

```bash
cd c-native/server
gcc server.c -o server $(/usr/local/mysql/bin/mysql_config --cflags --libs) -lpthread
```

如果运行时提示找不到 `libssl.3.dylib` 或 `libcrypto.3.dylib`，需要为本地生成的 `server` 修正动态库路径：

```bash
install_name_tool -add_rpath /usr/local/mysql/lib server
install_name_tool -change libssl.3.dylib /opt/homebrew/lib/libssl.3.dylib server
install_name_tool -change libcrypto.3.dylib /opt/homebrew/lib/libcrypto.3.dylib server
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

## 测试验证

项目提供了完整测试入口，用于验证客户端和服务端可以编译，并覆盖服务端状态、协议边界、源码契约和可选的真实数据库一致性检查：

```bash
./tests/run_all_tests.sh
```

默认测试不要求运行 MySQL 服务。它覆盖：

- 客户端和服务端在当前依赖下通过 `-Wall -Wextra -Wpedantic` 编译
- 服务端在线会话状态同步和定向发送
- 协议记录拼接的边界和截断行为
- 客户端 GTK idle 回调、页面切换和限长解析契约
- 服务端数据库互斥、事务、级联外键、唯一约束和响应构造契约
- `init.sql` 的关键约束和可重复导入特性

如果需要运行真实 MySQL 集成测试，需要准备一个可被测试销毁的数据库，数据库名必须包含 `test`：

```bash
LINUXCHAT_RUN_DB_TESTS=1 \
LINUXCHAT_TEST_DB_HOST=localhost \
LINUXCHAT_TEST_DB_USER=chat_test_user \
LINUXCHAT_TEST_DB_PASSWORD=chat_test_password \
LINUXCHAT_TEST_DB_NAME=linuxchat_test \
./tests/run_all_tests.sh --with-db
```

数据库集成测试会删除并重建测试库中的 `users`、`messages`、`friends` 三张表，用于检查：

- 重复用户名被拒绝
- 无效外键不会写入好友或消息
- 好友关系双向插入具备事务一致性
- 半边好友关系不会在失败重试时变成不一致的双边状态
- 历史消息按时间排序，且时间戳不会破坏协议分隔
- 删除用户会级联清理相关好友和消息

脚本会自动探测 `mysql_config`。如果本机 MySQL 安装路径不在常见位置，可以显式指定：

```bash
MYSQL_CONFIG=/path/to/mysql_config ./tests/run_all_tests.sh
```

原有 P0/P1 回归入口仍然保留：

```bash
./tests/run_p0_tests.sh
./tests/run_p1_tests.sh
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

当前版本是课程设计/学习用途实现，已覆盖主要功能链路。issue #3 已修复登录后聊天链路的 P0 阻断问题，包括客户端接收线程启动时机、聊天页切换、服务端在线用户状态同步和实时消息昵称获取。issue #5 已修复 P1 稳定性和数据正确性问题，包括历史消息时间戳解析、历史响应缓冲区、socket 失败判断和全局 MySQL 连接并发访问保护。

后续可以继续完善：

- SQL 仍使用字符串拼接，存在注入风险，后续应改为 MySQL 预处理语句
- 文本协议仍依赖 `:`, `,`, `;` 分隔，消息内容包含这些字符时可能解析错误
- 密码明文保存，后续应增加密码哈希和更完整的输入校验
- TCP 文本协议尚未处理拆包/粘包，后续可增加明确的消息边界

# LinuxChat

LinuxChat 是一个 Linux 程序设计课程实践项目，也是一个基于 Linux 平台的 C 语言网络即时通信工具。项目采用 Client/Server 架构实现，包含 TCP Socket 通信、多线程服务端、MySQL 数据持久化和 GTK3 图形客户端。

## 功能特性

- 用户注册与登录
- 好友添加与好友列表查询
- 一对一实时文本聊天
- 群聊创建、群列表、群成员查看和群聊文本消息
- 好友上线/离线通知、用户屏蔽和解除屏蔽
- 离线私聊/群聊未读提示
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
├── client/
│   └── client.c
├── database/
│   └── init.sql
├── docs/
│   ├── 操作文档.md
│   └── 课程设计报告.md
├── server/
│   └── server.c
├── tests/
│   ├── run_p0_tests.sh
│   └── run_p1_tests.sh
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
mysql -u chat_user -p chat_db < database/init.sql
```

然后根据本机数据库配置，修改 `server/server.c` 中的 MySQL 连接参数：

```c
mysql_real_connect(db_conn, "localhost", "chat_user", "chat_password", "chat_db", 0, NULL, 0)
```

macOS 使用 Homebrew 安装 MySQL 后，也可以通过同样的 `mysql` 命令初始化数据库。如果本地 MySQL root 账号未设置密码，可先直接运行 `mysql -u root`，再按上面的 SQL 创建项目数据库和用户。

## 编译运行

编译服务端：

```bash
cd server
gcc server.c -o server -lmysqlclient -lpthread
```

macOS 如果找不到 MySQL 头文件或链接库，可以改用：

```bash
cd server
MYSQL_LIBDIR="$(mysql_config --variable=pkglibdir)"
gcc server.c -o server $(mysql_config --cflags --libs) -Wl,-rpath,"$MYSQL_LIBDIR" -lpthread
install_name_tool -change libssl.3.dylib @rpath/libssl.3.dylib server
install_name_tool -change libcrypto.3.dylib @rpath/libcrypto.3.dylib server
```

如果使用 MySQL 官网安装包且 `mysql_config` 不在 `PATH` 中，可以使用完整路径：

```bash
cd server
MYSQL_CONFIG_BIN=/usr/local/mysql/bin/mysql_config
MYSQL_LIBDIR="$($MYSQL_CONFIG_BIN --variable=pkglibdir)"
gcc server.c -o server $($MYSQL_CONFIG_BIN --cflags --libs) -Wl,-rpath,"$MYSQL_LIBDIR" -lpthread
install_name_tool -change libssl.3.dylib @rpath/libssl.3.dylib server
install_name_tool -change libcrypto.3.dylib @rpath/libcrypto.3.dylib server
```

如果已经按旧命令编译，运行时提示找不到 `@rpath/libmysqlclient.24.dylib`、`libssl.3.dylib` 或 `libcrypto.3.dylib`，可以不重新编译，直接为本地生成的 `server` 修正动态库路径：

```bash
cd server
MYSQL_LIBDIR="$(mysql_config --variable=pkglibdir)"
install_name_tool -add_rpath "$MYSQL_LIBDIR" server
install_name_tool -change libssl.3.dylib @rpath/libssl.3.dylib server
install_name_tool -change libcrypto.3.dylib @rpath/libcrypto.3.dylib server
```

编译客户端：

```bash
cd client
gcc client.c -o client `pkg-config --cflags --libs gtk+-3.0` -lpthread
```

启动服务端：

```bash
cd server
./server
```

服务端运行后会在终端持续监听连接；需要停止时，在该终端按 `Ctrl+C`。

启动客户端：

```bash
cd client
./client
```

## 测试验证

项目提供了完整测试入口，用于验证客户端和服务端可以编译，并覆盖服务端状态、协议边界、源码检查项和可选的真实数据库一致性检查：

```bash
./tests/run_all_tests.sh
```

默认测试不要求运行 MySQL 服务。它覆盖：

- 客户端和服务端在当前依赖下通过 `-Wall -Wextra -Wpedantic` 编译
- 服务端在线会话状态同步和定向发送
- 协议记录拼接的边界和截断行为
- 客户端 GTK idle 回调、页面切换和限长解析检查
- 服务端数据库互斥、事务、级联外键、唯一约束、预处理语句和会话授权检查
- 安全加固检查，包括协议分隔符拒绝、密码哈希、SQL 注入防护和客户端 `snprintf` 构造
- 用户名解析路径，包括 username 查找、找不到用户、自我添加拒绝和群成员权限仍以登录会话用户为准
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

数据库集成测试会删除并重建测试库中的 `users`、`messages`、`friends`、`friend_blocks`、`chat_groups`、`group_members`、`group_messages` 和 `group_message_deliveries` 等表，用于检查：

- 重复用户名被拒绝
- 密码以 `SHA2(..., 256)` 哈希形式写入，不再明文落库
- 注入型用户名/密码不能绕过登录，带单引号的合法字段通过预处理语句安全处理
- 无效外键不会写入好友或消息
- 好友关系双向插入具备事务一致性
- 半边好友关系不会在失败重试时变成不一致的双边状态
- 历史消息按时间排序，且时间戳不会破坏协议分隔
- 含协议分隔符的消息内容会被拒绝，不会写入数据库
- 删除用户会级联清理相关好友和消息
- 群聊创建、重复群成员处理、非成员访问拒绝和群消息持久化
- 屏蔽关系会阻止私聊发送判定
- 私聊和群聊离线消息统计会在查看历史后清除

脚本会自动探测 `mysql_config`。如果本机 MySQL 安装路径不在常见位置，可以显式指定：

```bash
MYSQL_CONFIG=/path/to/mysql_config ./tests/run_all_tests.sh
```

另外还保留了两个旧专项测试脚本，分别用于快速验证登录后聊天链路和聊天稳定性相关修复。

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
| ADDFRIEND_USERNAME | username | 添加好友，客户端默认使用用户名 |
| ADDFRIEND | user_id,friend_id | 添加好友，旧 ID 协议兼容 |
| FRIENDS | user_id | 获取好友列表 |
| MESSAGES | user_id,friend_id | 获取历史消息 |
| SEND | sender_id,receiver_id,content | 发送消息 |
| CREATE_GROUP | group_name,member_id... | 创建群聊，成员 ID 用逗号分隔 |
| GROUPS | user_id | 获取当前用户加入的群聊 |
| GROUP_MEMBERS | group_id | 获取群成员 |
| ADD_GROUP_MEMBER_USERNAME | group_id,username | 添加群成员，客户端默认使用用户名 |
| ADD_GROUP_MEMBER | group_id,user_id | 添加群成员，旧 ID 协议兼容 |
| GROUP_MESSAGES | group_id | 获取群聊历史 |
| SEND_GROUP | sender_id,group_id,content | 发送群消息 |
| BLOCK_USER_USERNAME | username | 屏蔽用户，客户端默认使用用户名 |
| BLOCK_USER | user_id,blocked_id | 屏蔽用户，旧 ID 协议兼容 |
| UNBLOCK_USER_USERNAME | username | 解除屏蔽，客户端默认使用用户名 |
| UNBLOCK_USER | user_id,blocked_id | 解除屏蔽，旧 ID 协议兼容 |
| OFFLINE_MESSAGES | user_id | 获取未读消息摘要 |
| QUIT | 无 | 断开连接 |

普通用户交互优先使用 `username`，服务端通过预处理语句将 `users.username` 解析为内部 `users.id`。`users.id` 是数据库主键和内部实现细节；`nickname` 只用于展示，不能作为唯一查找依据。为兼容现有客户端，`ADDFRIEND`、`FRIENDS`、`MESSAGES`、`SEND`、`GROUPS`、`SEND_GROUP`、`BLOCK_USER`、`UNBLOCK_USER` 和 `OFFLINE_MESSAGES` 仍可携带 user_id/sender_id 字段；服务端实际执行时只信任登录会话中的用户 ID。用户名、密码、昵称和群名不能包含 `,`、`:`、`;` 或换行，消息内容不能包含 `:`、`;` 或换行。

## 数据表

项目主要使用下面几类 MySQL 表：

- `users`：内部数字主键、唯一用户名、SHA-256 密码哈希、昵称和创建时间
- `friends`：用户之间的好友关系
- `friend_blocks`：用户屏蔽关系
- `messages`：私聊消息内容、发送方、接收方、未读状态和时间戳
- `chat_groups`：群聊基本信息和创建者
- `group_members`：群成员关系，使用唯一约束避免重复成员
- `group_messages`：群聊消息内容、发送方和时间戳
- `group_message_deliveries`：群消息按用户记录未读/已读状态

完整建表脚本见 `database/init.sql`。

## 文档

- `docs/操作文档.md`：环境安装、数据库配置、编译运行和使用说明
- `docs/课程设计报告.md`：项目目标、模块划分、设计实现和测试总结

## 当前状态

当前版本是课程设计/学习用途实现，已覆盖注册登录、好友、私聊、群聊、屏蔽、在线状态通知和离线消息提示等主要功能链路。前期已修复登录后聊天链路无法正常工作的关键问题，包括客户端接收线程启动时机、聊天页切换、服务端在线用户状态同步和实时消息昵称获取。随后又补充了聊天稳定性和数据正确性修复，包括历史消息时间戳解析、历史响应缓冲区、socket 失败判断和全局 MySQL 连接并发访问保护。安全加固部分已完成 SQL 预处理语句、密码哈希、协议分隔符校验、客户端有界格式化，以及好友/群聊/消息操作基于登录会话授权。

后续可以继续完善：

- 文本协议仍依赖 `:`, `,`, `;` 分隔，目前通过拒绝分隔符规避解析错位，后续可升级为转义或长度前缀协议
- 密码哈希目前使用 MySQL `SHA2(..., 256)`，后续可升级为带盐的慢哈希方案
- TCP 文本协议尚未处理拆包/粘包，后续可增加明确的消息边界

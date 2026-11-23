# dmcmd —— 核心的命令行（详设 2.14）

老 cmddm 重写：命令不再写死，来自核心的接口清单（`dm.iface.list`），参数按清单里的 JSON Schema 解析。
阻塞式：一个线程、普通的 socket，不用 libuv。用 dmc 收发。

    dmcmd [--dir 数据目录] [--role 角色] [--json] 命令 [参数...]
      help [接口]            这条连接能调的接口 / 一个接口的说明和参数
      version | stat | log [--n N]
      watch <类>             持续看推送，Ctrl-C 退出
      mcp                    stdio 和核心的 /mcp 之间转一道（令牌从环境变量 DMCMD_MCP_TOKEN 读）
      <接口名> [--名字 值...]
    不带命令：交互模式，一条连接，一行一条命令

- 从数据目录的 `dm.run` 拿端口和口令；没给 --dir 用环境变量 DMCMD_DIR，再没有用当前目录
- 退出码：0 成功；1 核心没在跑 / 连接断了；2 命令或参数不对（不发请求）；3 核心回了错误
- 冒烟测试（对着演示核心 test/support/demo-core）：`python build.py test smoke`

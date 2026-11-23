# dmc —— 客户端库（详设 2.14）

老 linkdm / plug 按新协议重写。只是协议引擎：封帧、切帧、按请求号把应答对回去、收推送、答核心的反向调用；
不带事件循环、不碰 libuv、不链 libdm，搬字节的事交给宿主（`dmc_host_st` 的几个回调）。

- 头文件：`include/dmc.h`；帧头用 `<dm/frame.h>` 的 16 字节，和核心、WebSocket 同一份
- 输出：`libdmc.a`；用它的程序只要 dm 的 include 路径，链接时不要 libdm、libuv（T-DMC-09）
- 错误码 -120000 起（详设 1.4）；核心回的错误码原样交给回调
- 用的人：dmcmd。壳和页面是 JavaScript，走 WebSocket，另写一份（详设第 9 章）
- 测试：`python build.py test dmc`

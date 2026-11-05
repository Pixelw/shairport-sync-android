/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
#ifndef LX_IPC_CHANNEL_H
#define LX_IPC_CHANNEL_H
#include <stdint.h>
#include "smi_abi.h"
#ifndef LX_IPC_DIRECTORY
/* 设备端必须由后续获授权的部署步骤预建；程序不创建目录。 */
#define LX_IPC_DIRECTORY "/dev/lx04-ap2-ipc"
#endif
/* 唯一允许的单客户端协议标识，不作为路径使用。 */
#define LX_IPC_CLIENT "/lx04"
/* 获取固定目录单写者锁；默认拒绝旧 smi，显式 reset 才清理已校验旧对象。 */
int lx_ipc_writer_start(int reset);
/* 首个 T 后创建固定 smi11；保留 fd 以参与文件锁协议。 */
int lx_ipc_writer_create(void);
/* 发布完整双副本；锁竞争返回失败，由后续事件循环更新重试。 */
int lx_ipc_writer_publish(const struct shm_structure *);
/* 使当前映射失效，删除本进程持有的当前 inode 并释放 fd。 */
int lx_ipc_writer_drop(void);
/* 在所有客户端清理后释放单写者锁。 */
void lx_ipc_writer_finish(void);
/* 打开只读映射，保持 fd；不创建对象。 */
int lx_ipc_reader_open(void);
/* T 发送前保存旧 inode；只接受生产者处理 T 后重新创建的对象，拒绝旧世代。 */
void lx_ipc_reader_begin(void);
/* E 发送前立即阻止本地时钟读取，避免等待控制报文往返时沿用旧 clock。 */
void lx_ipc_reader_pause(void);
/* 持共享锁读取并验证 inode、版本、双副本、时效和 master 生命周期。 */
int lx_ipc_reader_snapshot(struct shm_structure *, uint64_t now_raw_ns);
/* 关闭只读映射；每次 E/T 或会话切换需重新建立会话上下文。 */
int lx_ipc_reader_close(void);
#endif

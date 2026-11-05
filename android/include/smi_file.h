/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
#ifndef LX_SMI_FILE_H
#define LX_SMI_FILE_H
/* 原型只支持同一受信 UID 和预建 0700 目录；调用方负责映射和关闭。 */
/* 打开并校验私有目录，不创建目录，也不清理旧对象。 */
int lx_smi_open_directory(const char *path);
/* 排他创建固定 0600、88 字节对象，拒绝复用已有对象。 */
int lx_smi_create(int directory_fd);
/* 以只读方式打开并校验对象；映射必须使用 PROT_READ。 */
int lx_smi_open_reader(int directory_fd);
/* 每次读前检查命名对象是否仍指向当前打开的 inode。 */
int lx_smi_is_current(int directory_fd, int object_fd);
struct shm_structure;
/* 两端必须独立 open 同一 inode 并遵守 flock，不能与原版 SPS 混用。
 * 映射与 value 不得重叠；持非阻塞排他锁，先版本 0、后副本、最后版本 11。
 * 失败返回 -1；锁竞争应由协作事件循环重试，不能忙等。
 * 调用方仍须校验 inode、时效和会话世代，崩溃后不自动沿用旧时钟。 */
int lx_smi_publish(int object_fd, struct shm_structure *mapped,
                   const struct shm_structure *value);
/* 持非阻塞共享锁读取；版本或副本不一致时返回 -1/EAGAIN。
 * 调用前必须检查 inode；成功只表示结构完整，不代表时钟仍然有效。 */
int lx_smi_snapshot(int object_fd, const struct shm_structure *mapped,
                    struct shm_structure *value);
/* 目录下唯一允许的固定对象名，不接受网络报文传入路径。 */
#define LX_SMI_LEAF "smi11"
#endif

/**
* @file arkrtos_fdt_overlay.h
* @brief 最小子集的 dtb overlay：把一段 fragment 合并进一棵树，产出新的 blob
* @details 设备树的 overlay 在 Linux 上是一族语法：target-path / target 句柄、
*          __overlay__ 容器、__fixups__ / __symbols__ / __local_fixups__ 三段
*          标签解析、加上"删属性/删节点"。**这里只做其中一支，并且明确拒绝
*          其余部分**：
*
*            - 只认 target-path（绝对路径）。target = <&label> 需要先解析
*              __fixups__，那是另一套机制，不在本子集里；
*            - __overlay__ 里的属性覆盖同名属性、新属性追加到节点；
*            - __overlay__ 里的子节点作为新子节点追加，**同名子节点直接拒绝**
*              （"合并两棵子树"是另一个语义，这里不猜）；
*            - 任何 __fixups__ / __symbols__ / __local_fixups__ 节点一律拒绝；
*            - 没有删除语义：删属性 / 删节点在这里做不到，也不假装做到。
*
*          实现方式是把整棵树**重新拼一遍**：解析仍复用内核那套只读视图，
*          overlay 只负责"读基树 + 读 fragment + 写出新的 struct/strings 段"。
*          输出落在调用方给的 arena 里；成功之后调用方可以
*          arkrtos_fdt_load(arena, cap, arena, out_len) 换成这棵新树。
*
*          失败时**不修改调用方的树**（本模块根本不碰内核那份视图），
*          也**不承诺 arena 内容有效**——只有返回 OK 时 out_len 才有意义。
*
*          开关 ARKRTOS_USE_FDT_OVERLAY 默认 0，且依赖 ARKRTOS_USE_FDT。
*
* @author xw
*/

#ifndef __ARKRTOS_FDT_OVERLAY_H__
#define __ARKRTOS_FDT_OVERLAY_H__

#include "arkrtos_types.h"

/* Overlay 的错误码都落在 -11..-15：不与解析器的 -1..-10 混用，
 * 这样日志里的一个数字就能说清"是基树坏了还是 overlay 不支持"。 */
#define ARKRTOS_FDT_ERR_OVERLAY        (-11) /* fragment 形状不认识 */
#define ARKRTOS_FDT_ERR_OVERLAY_TARGET (-12) /* target-path 不存在 / 用了句柄形式 */
#define ARKRTOS_FDT_ERR_OVERLAY_DUP    (-13) /* 新子节点与已有子节点同名 */
#define ARKRTOS_FDT_ERR_OVERLAY_SIZE   (-14) /* 输出 arena 不够大 */
#define ARKRTOS_FDT_ERR_OVERLAY_TAG    (-15) /* 出现 __fixups__ / __symbols__ 等标签段 */

/* 输出 arena 至少要能放下：基树 struct + fragment struct + 两边 strings */
#define ARKRTOS_FDT_OVERLAY_MIN_ARENA  4096u

/**
* @brief 把 fragment 合并进 base，写出一个新的 flattened blob。
* @param dst      输出缓冲区（4 字节对齐，容量 cap）
* @param cap      输出缓冲区字节数
* @param base     基树 blob（含 magic 的完整 dtb）
* @param base_len 基树字节数
* @param frag     fragment blob（同上）
* @param frag_len fragment 字节数
* @param out_len  成功时写入新 blob 的字节数（含 header）
* @return ARKRTOS_FDT_OK / -1..-10（基树或 fragment 本身不合法）/
*         -11..-15（overlay 语义不支持，见上面的错误码）
*/
int arkrtos_fdt_overlay(void *dst, uint32 cap,
                        const void *base, uint32 base_len,
                        const void *frag, uint32 frag_len,
                        uint32 *out_len);

/** @brief 错误码的文字解释（只用于日志，不参与判定）。 */
const char *arkrtos_fdt_overlay_reason(int rc);

#endif /* __ARKRTOS_FDT_OVERLAY_H__ */

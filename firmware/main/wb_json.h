// main/wb_json.h —— 零依赖的极简 JSON 解析器(固定节点池,不分配内存)。
//
// 为什么不用 cJSON:
//   1. 设备只解析主机下发的 7 类帧,每帧 ≤512B,结构固定,通用解析器的大部分
//      能力(深嵌套、科学计数、Unicode 转义表)在这里都用不上;
//   2. 本仓库的校验门禁(tools/validate.sh)用 cc 直接编译纯 C 测试,引入 cJSON
//      就得在门禁里额外链接一份源码。这里保持"任何 C 编译器都能编译、能测试"。
//
// 设计取舍:
//   · 节点池固定 WB_JSON_MAX_NODES 个,超限即 parse 失败(帧超长本来就要丢);
//   · 字符串用"源串切片 + 转义解码"表示,不复制;取用时由调用方给缓冲区;
//   · 对象里每个键值对占两个节点(key 节点、value 节点),key.next 指向 value;
//   · 不支持 \uXXXX 转义解码(主机侧 json.dumps(ensure_ascii=False),中文是
//     原样 UTF-8;真遇到 \u 输出 '?',不猜测)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 本工程的其余代码是 C++;这些是纯 C 模块,需要 C 链接。
#ifdef __cplusplus
extern "C" {
#endif

#define WB_JSON_MAX_NODES 64
#define WB_JSON_MAX_DEPTH 6

typedef enum {
    WB_JSON_NULL = 0,
    WB_JSON_FALSE,
    WB_JSON_TRUE,
    WB_JSON_NUM,
    WB_JSON_STR,
    WB_JSON_ARR,
    WB_JSON_OBJ,
} wb_json_type_t;

typedef struct {
    uint8_t type;
    uint8_t kids;    // arr 元素数 / obj 键值对数
    uint16_t pos;    // 字符串/数字在源串中的起始偏移
    uint16_t len;    // 字符串/数字的原始长度(字符串不含两侧引号)
    int16_t first;   // 首个子节点,无则 -1
    int16_t next;    // 下一个兄弟节点,无则 -1
} wb_json_node_t;

typedef struct {
    wb_json_node_t node[WB_JSON_MAX_NODES];
    int16_t used;
    const char *src;
    uint16_t src_len;
} wb_json_t;

// 解析整段文本。成功返回 true;语法错误、节点溢出、超深嵌套均返回 false。
bool wb_json_parse(wb_json_t *j, const char *text, size_t len);

// 根节点下标;解析失败返回 -1。
int wb_json_root(const wb_json_t *j);

// 取值辅助。入参节点非法或键不存在时返回 -1,调用方按"缺键"处理。
int wb_json_obj_get(const wb_json_t *j, int obj, const char *key);
int wb_json_arr_at(const wb_json_t *j, int arr, int index);
int wb_json_arr_len(const wb_json_t *j, int arr);

// 字符串取值:按 UTF-8 边界截断到 cap 内(留 1 字节给 '\0')。
// 返回实际写入的字节数(不含 '\0');节点不是字符串时写空串并返回 0。
size_t wb_json_str(const wb_json_t *j, int n, char *out, size_t cap);

// 数字取值。节点不是数字时返回 dflt。
double wb_json_num(const wb_json_t *j, int n, double dflt);
int wb_json_int(const wb_json_t *j, int n, int dflt);
bool wb_json_bool(const wb_json_t *j, int n, bool dflt);

// 类型查询。节点非法时返回 WB_JSON_NULL。
wb_json_type_t wb_json_type(const wb_json_t *j, int n);
bool wb_json_is_obj(const wb_json_t *j, int n);
bool wb_json_is_arr(const wb_json_t *j, int n);

#ifdef __cplusplus
}  // extern "C"
#endif

//
// Created by zr on 23-1-13.
//
#include "huffman.h"
#include "internal/pqueue.h"
#include "internal/bitstream.h"
#include <stdlib.h>
#include <string.h>
#include "marker.h"

static int encode(comp_huffman_ctx_t* huff, comp_bitstream_t* in, comp_bitstream_t* out);
static int decode(comp_huffman_ctx_t* huff, comp_bitstream_t* in, comp_bitstream_t* out);

/* 优先队列比较函数 */
static inline int huffman_node_pri_cmp(const void* a, const void* b)
{
    return ((const comp_huffman_node_t*) a)->freq > ((const comp_huffman_node_t*) b)->freq;
}

/* 范式huffman编码排序比较函数 */
static inline int canonical_symbol_cmp(const void* a, const void* b)
{
    const comp_huffman_symbol_t* s1 = a;
    const comp_huffman_symbol_t* s2 = b;
    if(s1->symbol_code_len < s2->symbol_code_len)
        return 1;
    if(s1->symbol_code_len == s2->symbol_code_len)
        return s1->symbol < s2->symbol;
    return 0;
}

static comp_huffman_node_t* huffman_node_new(u_char c, u_int32_t freq, int is_leaf,
                                             comp_huffman_node_t* lchild, comp_huffman_node_t* rchild)
{
    comp_huffman_node_t* node = (comp_huffman_node_t*) malloc(sizeof(comp_huffman_node_t));
    if(!node) return NULL;
    node->c = c;
    node->freq = freq;
    node->left = lchild;
    node->right = rchild;
    node->is_leaf = is_leaf;
    return node;
}

static comp_huffman_symbol_t* huffman_symbol_new(u_char sym, size_t code_len)
{
    comp_huffman_symbol_t* huff_symbol = (comp_huffman_symbol_t*) malloc(sizeof(comp_huffman_symbol_t));
    if(!huff_symbol) return NULL;
    huff_symbol->symbol_code_len = code_len;
    huff_symbol->symbol = sym;
    return huff_symbol;
}

comp_huffman_ctx_t* comp_huffman_init(comp_progress_bar* bar)
{
    comp_huffman_ctx_t* huff = (comp_huffman_ctx_t*) malloc(sizeof(comp_huffman_ctx_t));
    if(!huff) return NULL;
    memset(huff->freq, 0, HUFFMAN_MAX_SYMBOL);
    for(int i = 0; i < HUFFMAN_MAX_SYMBOL; i++)
        huff->symbol_code_table[i] = comp_str_empty();
    huff->symbols = comp_vec_init(64);
    huff->root = NULL;
    huff->padding = 0;
    huff->content_len = 0;
    huff->disable = 0;
    huff->huffman_encode = encode;
    huff->huffman_decode = decode;
    huff->bar = bar;
    return huff;
}

/* 建立普通的huffman树，将所有出现过的符号插入优先队列，
 * 每次从中取两个频数最低的符号生成子树，新父节点频数为两个子节点之和，
 * 将新的父节点插入优先队列 */
static int huffman_build_tree(comp_huffman_ctx_t* huff)
{
    comp_pqueue_t* pq = comp_pqueue_init(64, huffman_node_pri_cmp);
    if(!pq) return -1;
    for(int c = 0; c < HUFFMAN_MAX_SYMBOL; c++)
        if(huff->freq[c] > 0)
        {
            comp_huffman_node_t* node = huffman_node_new((u_char)c, huff->freq[c], 1, NULL, NULL);
            if(!node)
            {
                comp_pqueue_destroy(pq);
                return -1;
            }
            comp_pqueue_insert(pq, node);
        }
    while(comp_pqueue_size(pq) > 1)
    {
        comp_huffman_node_t* x = comp_pqueue_pop(pq);
        comp_huffman_node_t* y = comp_pqueue_pop(pq);
        comp_huffman_node_t* parent = huffman_node_new(0, x->freq + y->freq, 0, x, y);
        comp_pqueue_insert(pq, parent);
    }
    huff->root = comp_pqueue_pop(pq);
    comp_pqueue_destroy(pq);
    return 0;
}

/* 遍历huffman树，计算每个符号的编码长度，为生成范式huffman编码作准备 */
static void get_code_len(comp_huffman_ctx_t* huff, comp_huffman_node_t* root, size_t code_len)
{
    if(root->is_leaf)
    {
        comp_huffman_symbol_t* huff_symbol = huffman_symbol_new(root->c, code_len);
        comp_vec_push_back(huff->symbols, huff_symbol);
        return;
    }
    get_code_len(huff, root->left, code_len + 1);
    get_code_len(huff, root->right, code_len + 1);
}

/* 为sym->symbol分配编码，码值是code，码长是sym->symbol_code_len，结合码值，码长可以唯一确定一个编码 */
static void huffman_assign_code(comp_huffman_ctx_t* huff, int code, comp_huffman_symbol_t* sym)
{
    comp_str_t code_str = comp_str_parse_int(code, 2);
    comp_str_t symbol_code = huff->symbol_code_table[sym->symbol];
    size_t code_str_len = comp_str_len(code_str);
    if(sym->symbol_code_len == code_str_len)
        symbol_code = comp_str_assign(symbol_code, code_str);
    else
    {
        for(int i = 0; i < sym->symbol_code_len - code_str_len; i++)
            symbol_code = comp_str_append_char(symbol_code, '0');
        symbol_code = comp_str_append_str(symbol_code, code_str);
    }
    //编码插入到编译表中
    huff->symbol_code_table[sym->symbol] = symbol_code;
    comp_str_free(code_str);
}

/* 生成范式huffman编码 */
static void huffman_build_code(comp_huffman_ctx_t* huff)
{
    //计算每个符号的编码长度
    get_code_len(huff, huff->root, 0);
    //按照编码长度排序
    comp_vec_sort(huff->symbols, 0, comp_vec_len(huff->symbols) - 1, canonical_symbol_cmp);
    int cnt = 0;
    int code = 0; int pre_min_code = 0;
    size_t pre_code_len = 1;
    u_int32_t remain = 0;
    //为每个符号重新分配编码
    for(int i = 0; i < comp_vec_len(huff->symbols); i++)
    {
        comp_huffman_symbol_t* sym = comp_vec_get(huff->symbols, i);
        if(sym->symbol_code_len == 0 && comp_vec_len(huff->symbols) == 1)
            sym->symbol_code_len += 1;
        remain += (sym->symbol_code_len * huff->freq[sym->symbol]) % 8;
        huff->content_len += huff->freq[sym->symbol];
        //同长度编码的码值是递增的
        if(sym->symbol_code_len == pre_code_len)
        {
            //将code分配给sym，因为code是整数，11和011在整数范畴是相等的，但是完全不同的编码，
            //所以需要将code转为二进制字符串，再根据 sym->symbol_code_len 在前边补'0'
            huffman_assign_code(huff, code, sym);
            code++;
            cnt++;
        }
        else
        {
            //遇到了更长的编码，先将上一个长度的编码中码值的最小值左移，再加一，就是当前长度的第一个编码
            size_t gap = sym->symbol_code_len - pre_code_len;
            code = pre_min_code = (pre_min_code + cnt) << gap;
            huffman_assign_code(huff, code, sym);
            code++;
            pre_code_len = sym->symbol_code_len;
            cnt = 1;
        }
    }
    huff->padding = 8 - remain % 8;
    if(huff->padding == 8)
        huff->padding = 0;
}

/* 向输出流中写huffman头
 * 正常的huffman头结构如下
 +----------+------------+------------------------+
 |  标识符  | 0x48       |    huffman头部标识     |
 +----------+------------+------------------------+
 | 头部长度 | u_short    |                        |
 +----------+------------+------------------------+
 | 内容长度 | uint_32_t  |                        |
 +----------+------------+------------------------+
 |  长度表  | u_char[17] | 每个长度编码的符号个数 |
 +----------+------------+------------------------+
 |  符号表  | u_char[N]  |                        |
 +----------+------------+------------------------+
 | 填充长度 | u_char     | 字节对齐所需填充bit个数|
 +----------+------------+------------------------+
 因为最终使用的编码是范式huffman编码，所以解码器根据长度表和符号表就能反推出符号对应编码

 如果huffman编码被disable, 头部结构如下
 +----------+------------+------------------------+
 |  标识符  | 0x4E       |                无编码               |
 +----------+------------+------------------------+
 | 内容长度 | uint_32_t  |                        |
 +----------+------------+------------------------+
*/

void huffman_write_header(comp_huffman_ctx_t* huff, size_t header_len, comp_bitstream_t* out_stream)
{
    if(huff->disable)
    {
        comp_bitstream_write_char(out_stream, NONE_COMPRESS_MARKER);
        comp_bitstream_write_int(out_stream, (int) huff->content_len);
        return;
    }
    comp_bitstream_write_char(out_stream, HUFFMAN_HEADER_MARKER);
    u_char header_len_high = (header_len >> 8) & 0xFF;
    u_char header_len_low = header_len & 0xFF;
    comp_bitstream_write_char(out_stream, (char) header_len_high);
    comp_bitstream_write_char(out_stream, (char) header_len_low);
    comp_bitstream_write_int(out_stream, (int) huff->content_len);
    u_char num[17] = {0};
    for(int i = 0; i < comp_vec_len(huff->symbols); i++)
        num[HUFFMAN_GET_SYMBOL_LEN(i)]++;
    comp_bitstream_write(out_stream, (char*) (num + 1), 16);
    for(int i = 0; i < comp_vec_len(huff->symbols); i++)
        comp_bitstream_write_char(out_stream, HUFFMAN_GET_SYMBOL(i));
#ifdef DEBUG
    HUFFMAN_DEBUG("padding = %d", huff->padding);
#endif
    comp_bitstream_write_char(out_stream, (char) huff->padding);
}

/* 释放huffman树 */
static void huffman_free_tree(comp_huffman_node_t* root)
{
    if(!root) return;
    if(root->is_leaf)
    {
        free(root);
        return;
    }
    huffman_free_tree(root->left);
    huffman_free_tree(root->right);
    free(root);
}

/* 重置huffman编解码器的状态 */
static void huffman_ctx_cleanup(comp_huffman_ctx_t* huff)
{
    // 统计的词频清零
    memset(huff->freq, 0, HUFFMAN_MAX_SYMBOL * sizeof(u_int32_t));
    // 清空编译表
    for(int i = 0; i < HUFFMAN_MAX_SYMBOL; i++)
        comp_str_clear(huff->symbol_code_table[i]);
    for(int i = 0; i < comp_vec_len(huff->symbols); i++)
        free(comp_vec_get(huff->symbols, i));
    comp_vec_clear(huff->symbols);
    // 释放huffman树
    if(huff->root)
        huffman_free_tree(huff->root);
    huff->padding = 0;
    huff->content_len = 0;
    huff->disable = 0;
    huff->root = NULL;
}

/* 编码文件内容 */
static void huffman_encode_content(comp_huffman_ctx_t* huff, comp_bitstream_t* in_stream, comp_bitstream_t* out_stream)
{
    char input;
    //如果启用huffman编码，就不断从输入读入符号，在编译表中查找对应编码，写入输出
    if(!huff->disable)
    {
        // 填充 huff->padding 个bit，字节对齐
        for(int i = 0; i < huff->padding; i++)
            comp_bitstream_write_bit(out_stream, 0);
        while(1)
        {
            comp_bitstream_read_char(in_stream, &input);
            if(comp_bitstream_eof(in_stream))
                break;
            comp_str_t code = huff->symbol_code_table[(u_char) input];
            for(int i = 0; i < comp_str_len(code); i++)
                comp_bitstream_write_bit(out_stream, comp_str_at(code, i) - '0');
#ifndef DEBUG
            comp_bar_add(huff->bar, 1);
#endif
        }
    }
    //如果禁用了huffman编码，就把输入原封不动复制到输出
    else
    {
        while(1)
        {
            comp_bitstream_read_char(in_stream, &input);
            if(comp_bitstream_eof(in_stream))
                break;
            comp_bitstream_write_char(out_stream, input);
#ifndef DEBUG
            comp_bar_add(huff->bar, 1);
#endif
        }
    }
    //清输出缓冲
    comp_bitstream_flush(out_stream);
}

/* 检查是否可以启用huffman编码
 * 有以下两种情况需要禁用huffman编码
 * 1. 最长编码超过16位，目前的实现不支持16位以上的huffman编码，这里可以优化
 * 2. 256种符号全部出现并且编码长度全相等，这时huffman编码没有意义，而且会引发错误(u_char溢出)*/
void huffman_check_disable_condition(comp_huffman_ctx_t* huff)
{
    if(HUFFMAN_GET_SYMBOL_LEN(comp_vec_len(huff->symbols) - 1) > 16)
    {
        huff->disable = 1;
        return;
    }
    if(comp_vec_len(huff->symbols) < 256)
        return;
    comp_huffman_symbol_t* sym = comp_vec_front(huff->symbols);
    for (int i = 1; i < comp_vec_len(huff->symbols); ++i)
        if(HUFFMAN_GET_SYMBOL_LEN(i) != sym->symbol_code_len)
            return;
    huff->disable = 1;
}

/* 编码函数 */
int encode(comp_huffman_ctx_t* huff, comp_bitstream_t* in_stream, comp_bitstream_t* out_stream)
{
    if(!in_stream || !out_stream) return -1;
    char c;
    //这里 huffman_header_len_min 是huffman头部长度的固定长度部分
    // 2 bytes header_len + 4 bytes content_len + 16 bytes symbol num + 1 byte padding_len
    size_t huffman_header_len_min = 2 + 4 + 16 + 1;
    size_t huffman_header_len = huffman_header_len_min;
    while(1)
    {
        //统计词频
        comp_bitstream_read_char(in_stream, &c);
        if(comp_bitstream_eof(in_stream))
            break;
        if(huff->freq[(u_char) c]++ == 0)
            huffman_header_len++;
    }
    if(huffman_header_len == huffman_header_len_min)
    {
        //这里判断的是空文件的情况
        comp_bitstream_write_char(out_stream, HUFFMAN_HEADER_MARKER);
        comp_bitstream_write_short(out_stream, 2);
        huffman_ctx_cleanup(huff);
        return 0;
    }
    //建huffman树
    huffman_build_tree(huff);
    //生成范式huffman编码
    huffman_build_code(huff);
    //检查是否可以启用huffman编码
    huffman_check_disable_condition(huff);
#ifdef DEBUG
    if(!huff->disable)
    {
        printf("\n");
        for(int i = 0; i < comp_vec_len(huff->symbols); i++)
        {
            comp_huffman_symbol_t* sym = comp_vec_get(huff->symbols, i);
            HUFFMAN_DEBUG("%x: %s", sym->symbol, huff->symbol_code_table[sym->symbol]);
        }
    }
#endif
    //写huffman头
    huffman_write_header(huff, huffman_header_len, out_stream);
    //统计词频以后文件指针已经到末尾了，要重置到文件头
    comp_bitstream_reset(in_stream);
    huffman_encode_content(huff, in_stream, out_stream);
    huffman_ctx_cleanup(huff);
    return 0;
}

/* 读取huffman头，最主要工作是建立 symbol->码长 的关系，保存在huff->symbols中，
 * 根据 symbol->码长 的信息就可以还原出范式huffman树 */
static int huffman_read_header(comp_huffman_ctx_t* huff, comp_bitstream_t* in_stream)
{
    char input;
    comp_bitstream_read_char(in_stream, &input);
    comp_bar_add(huff->bar, 1);
    if(input == NONE_COMPRESS_MARKER)
    {
        comp_bitstream_read_int(in_stream, (int*)(&huff->content_len));
        comp_bar_add(huff->bar, 4);
        huff->disable = 1;
        return 0;
    }
    if(input != HUFFMAN_HEADER_MARKER)
        return -1;
    char hdr_high, hdr_low;
    comp_bitstream_read_char(in_stream, &hdr_high);
    comp_bitstream_read_char(in_stream, &hdr_low);
    size_t huffman_hdr_len = (u_char)hdr_high << 8 | (u_char)hdr_low;
    comp_bar_add(huff->bar, huffman_hdr_len);
    huffman_hdr_len -= 2;
    if(huffman_hdr_len == 0)
    {
        huff->content_len = 0;
        return 0;
    }
    comp_bitstream_read_int(in_stream, (int*)(&huff->content_len));
    huffman_hdr_len -= 4;
    u_char num[17] = {0};
    for(int i = 1; i <= 16; i++)
        if(comp_bitstream_read_char(in_stream, (char*)(num + i)) < 0)
            return -1;
    huffman_hdr_len -= 16;
    for(int i = 1; i <= 16; i++)
        for(int j = 0; j < num[i]; j++)
        {
            comp_bitstream_read_char(in_stream, &input);
            comp_huffman_symbol_t* symbol = huffman_symbol_new(input, i);
            comp_vec_push_back(huff->symbols, symbol);
            huffman_hdr_len -= 1;
        }
    comp_bitstream_read_char(in_stream, &input);
    huff->padding = input;
    huffman_hdr_len -= 1;
    return huffman_hdr_len == 0 ? 0 : -1;
}

/* 解码文件内容 */
static void huffman_decode_content(comp_huffman_ctx_t* huff, comp_bitstream_t* in_stream, comp_bitstream_t* out_stream)
{
    char cnt = 0;
    for(int i = 0; i < huff->padding; i++)
    {
        comp_bitstream_read_bit(in_stream, NULL);
        cnt++;
    }
    int bit;
    comp_huffman_node_t* huff_node = huff->root;
    u_int32_t len = 0;
    while(1)
    {
        comp_bitstream_read_bit(in_stream, &bit);
        if(++cnt == 8)
        {
            cnt = 0;
            comp_bar_add(huff->bar, 1);
        }
        if(!bit) huff_node = huff_node->left;
        else huff_node = huff_node->right;
        if(huff_node->is_leaf)
        {
            comp_bitstream_write_char(out_stream, (char) huff_node->c);
            huff_node = huff->root;
            if(++len == huff->content_len)
                break;
        }
    }
    comp_bitstream_flush(out_stream);
}

static int assign_symbol(comp_huffman_node_t* root, comp_huffman_symbol_t* sym, size_t len)
{
    if(len == sym->symbol_code_len)
    {
        if(!root->left)
        {
            root->left = huffman_node_new(sym->symbol, 0, 1, NULL, NULL);
            return 0;
        }
        if(!root->right)
        {
            root->right = huffman_node_new(sym->symbol, 0, 1, NULL, NULL);
            return 0;
        }
        return -1;
    }
    if(!root->left)
        root->left = huffman_node_new(0, 0, 0, NULL, NULL);
    int assigned = -1;
    if(!root->left->is_leaf)
        assigned = assign_symbol(root->left, sym, len + 1);
    if(assigned < 0)
    {
        if(!root->right)
            root->right = huffman_node_new(0, 0, 0, NULL, NULL);
        if(!root->right->is_leaf)
            return assign_symbol(root->right, sym, len + 1);
        return -1;
    }
    return 0;
}

/* 使用 symbol->码长 信息还原范式huffman树 */
static int huffman_rebuild_tree(comp_huffman_ctx_t* huff)
{
    huff->root = huffman_node_new(0, 0, 0, NULL, NULL);
    for(int i = 0; i < comp_vec_len(huff->symbols); i++)
        if(assign_symbol(huff->root, (comp_huffman_symbol_t*) comp_vec_get(huff->symbols, i), 1) < 0)
            return -1;
    return 0;
}

// for debugging
static void print(comp_huffman_node_t* root, comp_str_t code)
{
    if(root->is_leaf)
    {
        printf("%x ==> %s\n", root->c, code);
        comp_str_free(code);
        return;
    }
    comp_str_t left_code = comp_str_new(code);
    comp_str_t right_code = comp_str_new(code);
    left_code = comp_str_append_char(left_code, '0');
    right_code = comp_str_append_char(right_code, '1');
    comp_str_free(code);
    print(root->left, left_code);
    print(root->right, right_code);
}
// for debugging
void huffman_print(comp_huffman_ctx_t* huff)
{
    printf("\n");
    print(huff->root, comp_str_empty());
}

/* 解码函数 */
int decode(comp_huffman_ctx_t* huff, comp_bitstream_t* in_stream, comp_bitstream_t* out_stream)
{
    if(!in_stream || !out_stream) return -1;
    int err = 0;
    if(huffman_read_header(huff, in_stream) < 0)
    {
        err = -1;
        goto end;
    }
    //原文件是空文件
    if(huff->content_len == 0)
        goto end;
    if(huff->disable)
    {
        char input;
        u_int32_t len = 0;
        while(1)
        {
            comp_bitstream_read_char(in_stream, &input);
            comp_bitstream_write_char(out_stream, input);
            comp_bar_add(huff->bar, 1);
            if(++len == huff->content_len)
                break;
        }
        goto end;
    }
    if(huffman_rebuild_tree(huff) < 0)
    {
#ifdef DEBUG
        HUFFMAN_DEBUG("%s", "rebuild huffman tree fail");
#endif
        err = -1;
        goto end;
    }
#ifdef DEBUG
    huffman_print(huff);
#endif
    huffman_decode_content(huff, in_stream, out_stream);

end:
    huffman_ctx_cleanup(huff);
    return err;
}

void comp_huffman_free(comp_huffman_ctx_t* huff)
{
    for(int i = 0; i < 256; i++)
        comp_str_free(huff->symbol_code_table[i]);
    comp_vec_free(huff->symbols);
    free(huff);
}

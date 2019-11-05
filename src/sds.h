/* SDSLib, A C dynamic strings library
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __SDS_H
#define __SDS_H

/*
 * 最大预分配长度
 */
#define SDS_MAX_PREALLOC (1024*1024)

#include <sys/types.h>
#include <stdarg.h>

/*
 * 类型别名，用于指向 sdshdr 的 buf 属性
 */
typedef char *sds;

/*
 * 保存字符串对象的结构
 */
struct sdshdr {
    
    // buf 中已占用空间的长度
    int len;

    // buf 中剩余可用空间的长度
    int free;

    // 数据空间
    char buf[];
};

/*
 * 返回 sds 实际保存的字符串的长度
 *
 * T = O(1)
 */
static inline size_t sdslen(const sds s) {
    /*
     * 作为一种特殊的情况下，与一个以上的命名构件的结构的最后一个元件可以具有一个不完整的阵列类型;这就是所谓的柔性阵列构件。在大多数情况下，该柔性阵列构件被忽略。特别地，该结构的大小是因为如果柔性阵列构件，不同的是它可以具有更多拖尾填充比遗漏将意味着省略。
     * 然而，当一个.(或 ->）运算符具有左操作数是（指针）与柔性阵列构件和右操作数名该成员，它的行为就像该成员用最长的阵列替代（具有相同的元素类型的结构），不会使结构不是被访问的对象大;所述阵列的偏移应保持该柔性阵列构件的，即使这会从该替换阵列的不同。如果这个数组就没有的元素，它的行为就好像它有一个元素，但如果任何企图访问作出元素或产生一个指向一个过去它的行为是不明确的。
     * 
     * 在这里 sdshdr中的buf就是柔性阵列构件，所以在进行sizeof(struct sdshdr)的时候是只计算 len和free的大小的，也就是说 sizeof(struct sdshdr) = sizeof(unsigned int) + sizeof(unsigned int)
     * 所以利用buf的内存地址减去len和free的地址就可以的到sdshdr这个结构体的位置，然后将此转化为一个sdshdr结构体
     */
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
    return sh->len;
}

/*
 * 返回 sds 可用空间的长度
 *
 * T = O(1)
 */
static inline size_t sdsavail(const sds s) {
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
    return sh->free;
}

sds sdsnewlen(const void *init, size_t initlen);
sds sdsnew(const char *init);
sds sdsempty(void);
size_t sdslen(const sds s);
sds sdsdup(const sds s);
void sdsfree(sds s);
size_t sdsavail(const sds s);
sds sdsgrowzero(sds s, size_t len);
sds sdscatlen(sds s, const void *t, size_t len);
sds sdscat(sds s, const char *t);
sds sdscatsds(sds s, const sds t);
sds sdscpylen(sds s, const char *t, size_t len);
sds sdscpy(sds s, const char *t);

sds sdscatvprintf(sds s, const char *fmt, va_list ap);
#ifdef __GNUC__
sds sdscatprintf(sds s, const char *fmt, ...)
    /*
     * 这句主要作用是提示编译器，对这个函数的调用需要像printf一样，用对应的format字符串来check可变参数的数据类型。
     * 例如：
     *  extern int my_printf (void *my_object, const char *my_format, ...)
     *  __attribute__ ((format (printf, 2, 3)));
     *  format (printf, 2, 3)告诉编译器，my_format相当于printf的format，而可变参数是从my_printf的第3个参数开始。
     *  这样编译器就会在编译时用和printf一样的check法则来确认可变参数是否正确了
     */
    __attribute__((format(printf, 2, 3)));
#else
sds sdscatprintf(sds s, const char *fmt, ...);
#endif

sds sdscatfmt(sds s, char const *fmt, ...);
sds sdstrim(sds s, const char *cset);
void sdsrange(sds s, int start, int end);
void sdsupdatelen(sds s);
void sdsclear(sds s);
int sdscmp(const sds s1, const sds s2);
sds *sdssplitlen(const char *s, int len, const char *sep, int seplen, int *count);
void sdsfreesplitres(sds *tokens, int count);
void sdstolower(sds s);
void sdstoupper(sds s);
sds sdsfromlonglong(long long value);
sds sdscatrepr(sds s, const char *p, size_t len);
sds *sdssplitargs(const char *line, int *argc);
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen);
sds sdsjoin(char **argv, int argc, char *sep);

/* Low level functions exposed to the user API */
sds sdsMakeRoomFor(sds s, size_t addlen);
void sdsIncrLen(sds s, int incr);
sds sdsRemoveFreeSpace(sds s);
size_t sdsAllocSize(sds s);

#endif

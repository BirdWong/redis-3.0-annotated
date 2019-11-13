/* zmalloc - total amount of allocated memory aware version of malloc()
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * 以下中文注解参考于:<a href="https://blog.csdn.net/guodongxiaren/article/details/44747719">果冻虾仁</a>

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
 *
 *  CPU一次性能读取数据的二进制位数称为字长，也就是我们通常所说的32位系统（字长4个字节）、
 *  64位系统（字长8个字节）的由来。所谓的8字节对齐，就是指变量的起始地址是8的倍数。
 *  比如程序运行时（CPU）在读取long型数据的时候，只需要一个总线周期，时间更短，
 *  如果不是8字节对齐的则需要两个总线周期才能读完数据。
 *
 *  以下的8字节对齐是针对64位系统而言的，如果是32位系统那么就是4字节对齐。
 *  实际上Redis源码中的字节对齐是软编码，而非硬编码。里面多用sizeof(long)或sizeof(size_t)来表示。
 *  size_t（gcc中其值为long unsigned int）和long的长度是一样的，
 *  long的长度就是计算机的字长。这样在未来的系统中如果字长（long的大小）不是8个字节了，
 *  该段代码依然能保证相应代码可用。
 */

#include <stdio.h>
#include <stdlib.h>

/* This function provide us access to the original libc free(). This is useful
 * for instance to free results obtained by backtrace_symbols(). We need
 * to define this function before including zmalloc.h that may shadow the
 * free implementation if we use jemalloc or another non standard allocator. */
void zlibc_free(void *ptr) {
    free(ptr);
}

#include <string.h>
#include <pthread.h>
#include "config.h"
#include "zmalloc.h"

#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
#else
#if defined(__sun) || defined(__sparc) || defined(__sparc__)
#define PREFIX_SIZE (sizeof(long long))
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif
#endif

/* Explicitly override malloc/free etc when using tcmalloc. */
#if defined(USE_TCMALLOC)
#define malloc(size) tc_malloc(size)
#define calloc(count,size) tc_calloc(count,size)
#define realloc(ptr,size) tc_realloc(ptr,size)
#define free(ptr) tc_free(ptr)
#elif defined(USE_JEMALLOC)
#define malloc(size) je_malloc(size)
#define calloc(count,size) je_calloc(count,size)
#define realloc(ptr,size) je_realloc(ptr,size)
#define free(ptr) je_free(ptr)
#endif

#ifdef HAVE_ATOMIC
#define update_zmalloc_stat_add(__n) __sync_add_and_fetch(&used_memory, (__n))
#define update_zmalloc_stat_sub(__n) __sync_sub_and_fetch(&used_memory, (__n))
#else
/*
 *    pthread_mutex_lock()和pthread_mutex_unlock()
 *    使用互斥锁（mutex）来实现线程同步，前者表示加锁，后者
 *    表示解锁，它们是POSIX定义的线程同步函数。当加锁以后它
 *    后面的代码在多线程同时执行这段代码的时候就只会执行一次，
 *    也就是实现了线程安全
 */

#define update_zmalloc_stat_add(__n) do { \
    pthread_mutex_lock(&used_memory_mutex); \
    used_memory += (__n); \
    pthread_mutex_unlock(&used_memory_mutex); \
} while(0)

#define update_zmalloc_stat_sub(__n) do { \
    pthread_mutex_lock(&used_memory_mutex); \
    used_memory -= (__n); \
    pthread_mutex_unlock(&used_memory_mutex); \
} while(0)

#endif

/*
 *  使用do while循环的好处:http://www.spongeliu.com/415.html
 *
 *
 *  malloc()本身能够保证所分配的内存是8字节对齐的：如果你要分配的内存不是8的倍数，
 *  那么malloc就会多分配一点，来凑成8的倍数。所以update_zmalloc_stat_alloc函
 *  数（或者说zmalloc()相对malloc()而言）真正要实现的功能并不是进行8字节对
 *  齐（malloc已经保证了），它的真正目的是使变量used_memory精确的维护实际已分配内存的大小
 */
#define update_zmalloc_stat_alloc(__n) do { \
    size_t _n = (__n); \
    /* \
     * 这段代码就是判断分配的内存空间的大小是不是8的倍数。   \
     * 如果内存大小不是8的倍数，就加上相应的偏移量使之变成8的倍数。  \
     * _n&7 在功能上等价于 _n%8，不过位操作的效率显然更高。      \
     */ \
    if (_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1)); \
    /*                      \
     *   第2个if的条件是一个整型变量zmalloc_thread_safe。    \
     *   顾名思义，它的值表示操作是否是线程安全的，如果不是线程    \
     *   安全的（else），就给变量used_memory加上n。used_memory   \
     *   是zmalloc.c文件中定义的全局静态变量，表示已分配内存的大小。 \
     *   如果是内存安全的就使用update_zmalloc_stat_add来给   \
     *   used_memory加上n。    \
     */ \
    if (zmalloc_thread_safe) { \
        update_zmalloc_stat_add(_n); \
    } else { \
        used_memory += _n; \
    } \
} while(0)
/*
 * 利用互斥线程减少used_memory大小
 */
#define update_zmalloc_stat_free(__n) do { \
    size_t _n = (__n); \
    if (_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1)); \
    if (zmalloc_thread_safe) { \
        update_zmalloc_stat_sub(_n); \
    } else { \
        used_memory -= _n; \
    } \
} while(0)

static size_t used_memory = 0;
static int zmalloc_thread_safe = 0;
pthread_mutex_t used_memory_mutex = PTHREAD_MUTEX_INITIALIZER;

// oom是out of memory（内存不足）的意思
static void zmalloc_default_oom(size_t size) {
    fprintf(stderr, "zmalloc: Out of memory trying to allocate %zu bytes\n",
        size);
    fflush(stderr);
    abort();
}

static void (*zmalloc_oom_handler)(size_t) = zmalloc_default_oom;


/*
 *
 * 参数size是我们需要分配的内存大小。
 *
 * 辅助的函数：
 *
 * malloc()
 * zmalloc_oom_handler【函数指针】
 * zmalloc_default_oom()【被上面的函数指针所指向】
 * update_zmalloc_stat_alloc()【宏函数】
 * update_zmalloc_stat_add()【宏函数】
 */
void *zmalloc(size_t size) {
    /*
     * 实际上我们调用malloc实际分配的大小是size+PREFIX_SIZE。
     * PREFIX_SIZE是一个条件编译的宏，不同的平台有不同的结果，
     * 在Linux中其值是sizeof(size_t)，所以我们多分配了一个字
     * 长(8个字节)的空间（后面代码可以看到多分配8个字节的目的是用于储存size的值）。
     */
    void *ptr = malloc(size+PREFIX_SIZE);

    /*
     * 如果ptr指针为NULL（内存分配失败），调用zmalloc_oom_handler（size）。
     * 该函数实际上是一个函数指针指向函数zmalloc_default_oom，其主要功能就是打印错误信息并终止程序。
     */
    if (!ptr) zmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_alloc(zmalloc_size(ptr));
    return ptr;
#else
    /*
     * 第一行就是在已分配空间的第一个字长（前8个字节）处存储需要分配的字节大小（size）。
     *
     * 第二行调用了update_zmalloc_stat_alloc()【宏函数】，它的功能是更新全局变量
     * used_memory（已分配内存的大小）的值。
     *
     * 第三行返回的（char *）ptr+PREFIX_SIZE。就是将已分配内存的起始地址向右偏移
     * PREFIX_SIZE * sizeof(char)的长度（即8个字节），此时得到的新指针指向的内存
     * 空间的大小就等于size了。
     */
    *((size_t*)ptr) = size;
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);
    return (char*)ptr+PREFIX_SIZE;
#endif
}

/*
 *  calloc()的功能是也是分配内存空间，与malloc()的不同之处有两点：
 *      1、它分配的空间大小是 size * nmemb。比如calloc(10,sizoef(char)); // 分配10个字节
 *      2、calloc()会对分配的空间做初始化工作（初始化为0），而malloc()不会
 */
void *zcalloc(size_t size) {
    //  zcalloc()中没有calloc()的第一个函数nmemb。
    //  因为它每次调用calloc(),其第一个参数都是1。
    //  也就是说zcalloc()功能是每次分配 size+PREFIX_SIZE 的空间，并初始化
    void *ptr = calloc(1, size+PREFIX_SIZE);

    if (!ptr) zmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_alloc(zmalloc_size(ptr));
    return ptr;
#else
    *((size_t*)ptr) = size;
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);
    return (char*)ptr+PREFIX_SIZE;
#endif
}

/*
 * realloc()要完成的功能是给首地址ptr的内存空间，重新分配大小。
 * 如果失败了，则在其它位置新建一块大小为size字节的空间，
 * 将原先的数据复制到新的内存空间，并返回这段内存首地址【原内存会被系统自然释放】
 */
void *zrealloc(void *ptr, size_t size) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
#endif
    size_t oldsize;
    void *newptr;
    // 如果原地址不存在，说明没有要复制的数据， 直接初始化一个规定大小的空间即可
    if (ptr == NULL) return zmalloc(size);
#ifdef HAVE_MALLOC_SIZE
    oldsize = zmalloc_size(ptr);
    newptr = realloc(ptr,size);
    if (!newptr) zmalloc_oom_handler(size);

    update_zmalloc_stat_free(oldsize);
    update_zmalloc_stat_alloc(zmalloc_size(newptr));
    return newptr;
#else
    // 获取到真正分配的地址位置
    realptr = (char*)ptr-PREFIX_SIZE;
    // 获取数据块分配的地址的大小
    oldsize = *((size_t*)realptr);
    // 在原地址位置重新分配需要的大小（申请的大小+存储数据块大小的变量空间）
    newptr = realloc(realptr,size+PREFIX_SIZE);
    // 如果申请失败报错
    if (!newptr) zmalloc_oom_handler(size);

    // 申请成功则记录新的数据块地址大小
    *((size_t*)newptr) = size;
    // 减去之前数据块的大小
    update_zmalloc_stat_free(oldsize);
    // 加上新申请的数据块大小
    update_zmalloc_stat_alloc(size);
    // 右移，返回数据块的地址
    return (char*)newptr+PREFIX_SIZE;
#endif
}

/* Provide zmalloc_size() for systems where this function is not provided by
 * malloc itself, given that in that case we store a header with this
 * information as the first bytes of every allocation. */
#ifndef HAVE_MALLOC_SIZE
size_t zmalloc_size(void *ptr) {
    void *realptr = (char*)ptr-PREFIX_SIZE;
    size_t size = *((size_t*)realptr);
    /* Assume at least that all the allocations are padded at sizeof(long) by
     * the underlying allocator. */
    if (size&(sizeof(long)-1)) size += sizeof(long)-(size&(sizeof(long)-1));
    return size+PREFIX_SIZE;
}
#endif

/*
 * zfree函数的实现，它需要释放的空间起始地址要视库的支持能力决定。
 * 如果库不支持获取区块大小，则需要将传入的指针前移PREFIX_SIZE，然后释放该起始地址的空间。
 */ 
void zfree(void *ptr) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL) return;
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(zmalloc_size(ptr));
    free(ptr);
#else
    // 表示的是ptr指针向前偏移8个字节的长度，即回退到最初malloc返回的地址，这里称为realptr。
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    // zmalloc中update_zmalloc_stat_alloc()大致相同，唯一不同之处是此函数是给变量used_memory减去分配的空间
    update_zmalloc_stat_free(oldsize+PREFIX_SIZE);
    free(realptr);
#endif
}

/*
 * 复制字符串s的内容，到新的内存空间，构造新的字符串【堆区】。并将这段新的字符串地址返回
 */
char *zstrdup(const char *s) {
    // 获取字符串长度 加1是为了保存\0
    size_t l = strlen(s)+1;
    // 然后调用zmalloc()来分配足够的空间，首地址为p
    char *p = zmalloc(l);

    // 调用memcpy来完成复制
    memcpy(p,s,l);
    // 返回p
    return p;
}

size_t zmalloc_used_memory(void) {
    size_t um;

    if (zmalloc_thread_safe) {
#ifdef HAVE_ATOMIC
        um = __sync_add_and_fetch(&used_memory, 0);
#else
        pthread_mutex_lock(&used_memory_mutex);
        um = used_memory;
        pthread_mutex_unlock(&used_memory_mutex);
#endif
    }
    else {
        um = used_memory;
    }

    return um;
}

void zmalloc_enable_thread_safeness(void) {
    zmalloc_thread_safe = 1;
}

void zmalloc_set_oom_handler(void (*oom_handler)(size_t)) {
    zmalloc_oom_handler = oom_handler;
}

/* Get the RSS information in an OS-specific way.
 *
 * WARNING: the function zmalloc_get_rss() is not designed to be fast
 * and may not be called in the busy loops where Redis tries to release
 * memory expiring or swapping out objects.
 *
 * For this kind of "fast RSS reporting" usages use instead the
 * function RedisEstimateRSS() that is a much faster (and less precise)
 * version of the function. */

#if defined(HAVE_PROC_STAT)
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

size_t zmalloc_get_rss(void) {
    int page = sysconf(_SC_PAGESIZE);
    size_t rss;
    char buf[4096];
    char filename[256];
    int fd, count;
    char *p, *x;

    snprintf(filename,256,"/proc/%d/stat",getpid());
    if ((fd = open(filename,O_RDONLY)) == -1) return 0;
    if (read(fd,buf,4096) <= 0) {
        close(fd);
        return 0;
    }
    close(fd);

    p = buf;
    count = 23; /* RSS is the 24th field in /proc/<pid>/stat */
    while(p && count--) {
        p = strchr(p,' ');
        if (p) p++;
    }
    if (!p) return 0;
    x = strchr(p,' ');
    if (!x) return 0;
    *x = '\0';

    rss = strtoll(p,NULL,10);
    rss *= page;
    return rss;
}
#elif defined(HAVE_TASKINFO)
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/task.h>
#include <mach/mach_init.h>

size_t zmalloc_get_rss(void) {
    task_t task = MACH_PORT_NULL;
    struct task_basic_info t_info;
    mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

    if (task_for_pid(current_task(), getpid(), &task) != KERN_SUCCESS)
        return 0;
    task_info(task, TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count);

    return t_info.resident_size;
}
#else
size_t zmalloc_get_rss(void) {
    /* If we can't get the RSS in an OS-specific way for this system just
     * return the memory usage we estimated in zmalloc()..
     *
     * Fragmentation will appear to be always 1 (no fragmentation)
     * of course... */
    return zmalloc_used_memory();
}
#endif

/* Fragmentation = RSS / allocated-bytes */
float zmalloc_get_fragmentation_ratio(size_t rss) {
    return (float)rss/zmalloc_used_memory();
}

#if defined(HAVE_PROC_SMAPS)
size_t zmalloc_get_private_dirty(void) {
    char line[1024];
    size_t pd = 0;
    FILE *fp = fopen("/proc/self/smaps","r");

    if (!fp) return 0;
    while(fgets(line,sizeof(line),fp) != NULL) {
        if (strncmp(line,"Private_Dirty:",14) == 0) {
            char *p = strchr(line,'k');
            if (p) {
                *p = '\0';
                pd += strtol(line+14,NULL,10) * 1024;
            }
        }
    }
    fclose(fp);
    return pd;
}
#else
size_t zmalloc_get_private_dirty(void) {
    return 0;
}
#endif

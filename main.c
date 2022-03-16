#define _GNU_SOURCE             /* See feature_test_macros(7) */
#include <fcntl.h>              /* Obtain O_* constant definitions */
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <time.h> 
#include <sys/mman.h>
#include <errno.h>
#include <stdbool.h>
#include <x86intrin.h>
#include <inttypes.h>

int main(int argc, char *argv[])
{
    int listenfd = 0, connfd = 0;
    struct sockaddr_in serv_addr; 

    char sendBuff[1025];
    time_t ticks; 

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&serv_addr, '0', sizeof(serv_addr));
    memset(sendBuff, '0', sizeof(sendBuff)); 

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(5000); 

    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0)
    {
        fprintf(stderr, "setsockopt(SO_REUSEADDR) failed\n");
        return 1;
    }

    if (bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1)
    {
        fprintf(stderr, "bind error %s\n", strerror(errno));
        return 1;
    }

    if (listen(listenfd, 10) == -1)
    {
        fprintf(stderr, "listen error %s\n", strerror(errno));
        return 1;
    }

    {
        connfd = accept(listenfd, (struct sockaddr*)NULL, NULL);
        if (connfd == -1)
        {
            fprintf(stderr, "accept error %s\n", strerror(errno));
            return 1;
        }

        fprintf(stderr, "accepted\n");

        union
        {
            int buf_fd_[2];
            struct
            {
                int buf_rfd_;
                int buf_wfd_;
            };
        } pipe;
        
        if (pipe2(&pipe.buf_fd_[0], O_CLOEXEC))
        {
            fprintf(stderr, "pipe2 error %s\n", strerror(errno));
            return 1;
        }

        if (fcntl(pipe.buf_wfd_, F_SETPIPE_SZ, (int)(1 << 20)) == -1)
        {
            fprintf(stderr, "fcntl on pipe error %s\n", strerror(errno));
            return 1;
        }

        const int WRITE = false;
        const int LARGE_MMAP = false;
        const int LARGE_VECIO = false;

        #define VEC_COUNT_ORDER 6
        #define VEC_COUNT (1 << VEC_COUNT_ORDER)
        struct iovec iov[VEC_COUNT];
        const int bufsize = 1 << (20 - VEC_COUNT_ORDER);
        
        long long memset_ticks = 0;
            
        ssize_t total_memsets = 0;
            
        if (WRITE)
        {
            for (int i = 0; i < VEC_COUNT; ++i)
            {
                iov[i].iov_base = mmap(NULL, bufsize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                iov[i].iov_len = bufsize;
                if (iov[i].iov_base == MAP_FAILED)
                {
                    fprintf(stderr, "mmap[%d] error %s\n", i, strerror(errno));
                    return 1;
                }
            }
            
            for (int iteration = 0; iteration < 20000; ++iteration)
            {
                if (iteration % 1000 == 999)
                    fprintf(stderr, "Iteration %d\n", iteration + 1);

                for (int vec = 0; vec < VEC_COUNT; vec++)
                {
                    long long start = __rdtsc();
                    memset(iov[vec].iov_base, '0' + ((vec + iteration) & 32), bufsize);
                    memset_ticks += __rdtsc() - start;
                    total_memsets += bufsize;

                    ssize_t written = write(connfd, iov[vec].iov_base, iov[vec].iov_len);
                    if (written == -1)
                    {
                        fprintf(stderr, "write[%d] error %s\n", vec, strerror(errno));
                        return 1;
                    }
                    if (written != iov[vec].iov_len)
                    {
                        fprintf(stderr, "failed to write the full buffer of %zd bytes\n", iov[vec].iov_len);
                        return 1;
                    }
                }
            }

            for (int i = 0; i < VEC_COUNT; ++i)
                munmap(iov[i].iov_base, bufsize);
        }
        else
        {
            for (int iteration = 0; iteration < 20000; ++iteration)
            {
                if (LARGE_MMAP)
                {
                    char *base = mmap(NULL, VEC_COUNT * bufsize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                    if (base == MAP_FAILED)
                    {
                        fprintf(stderr, "large mmap error %s\n", strerror(errno));
                        return 1;
                    }
                    for (int i = 0; i < VEC_COUNT; ++i)
                    {
                        iov[i].iov_base = base + i * bufsize;
                        iov[i].iov_len = bufsize;
                        long long start = __rdtsc();
                        memset(iov[i].iov_base, '0' + ((i + iteration) & 32), bufsize);
                        memset_ticks += __rdtsc() - start;
                        total_memsets += bufsize;
                    }
                }
                else
                {
                    for (int i = 0; i < VEC_COUNT; ++i)
                    {
                        iov[i].iov_base = mmap(NULL, bufsize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                        iov[i].iov_len = bufsize;
                        if (iov[i].iov_base == MAP_FAILED)
                        {
                            fprintf(stderr, "mmap[%d] error %s\n", i, strerror(errno));
                            return 1;
                        }
                        long long start = __rdtsc();
                        memset(iov[i].iov_base, '0' + ((i + iteration) & 32), bufsize);
                        memset_ticks += __rdtsc() - start;
                        total_memsets += bufsize;
                    }
                }

                if (LARGE_VECIO)
                {
                    if (iteration % 1000 == 999)
                        fprintf(stderr, "Iteration %d\n", iteration + 1);
                    ssize_t written = vmsplice(pipe.buf_wfd_, iov, VEC_COUNT, SPLICE_F_GIFT);
                    if (written == -1)
                    {
                        fprintf(stderr, "vmsplice error %s\n", strerror(errno));
                        return 1;
                    }

                    off64_t zero_off = 0;
                    ssize_t written2 = splice(pipe.buf_rfd_, NULL, connfd, NULL,
                                              bufsize * VEC_COUNT, SPLICE_F_MOVE);
                    if (written2 == -1)
                    {
                        fprintf(stderr, "splice error %s\n", strerror(errno));
                        return 1;
                    }
                }
                else
                {
                    if (iteration % 1000 == 999)
                        fprintf(stderr, "Iteration %d\n", iteration + 1);

                    for (int vec = 0; vec < VEC_COUNT; vec++)
                    {
                        ssize_t written = vmsplice(pipe.buf_wfd_, &iov[vec], 1, SPLICE_F_GIFT);
                        if (written == -1)
                        {
                            fprintf(stderr, "vmsplice error %s\n", strerror(errno));
                            return 1;
                        }

                        off64_t zero_off = 0;
                        ssize_t written2 = splice(pipe.buf_rfd_, NULL, connfd, NULL,
                                                  bufsize * VEC_COUNT, SPLICE_F_MOVE);

                        if (written2 == -1)
                        {
                            fprintf(stderr, "splice error %s\n", strerror(errno));
                            return 1;
                        }
                    }
                }

                if (LARGE_MMAP)
                {
                    munmap(iov[0].iov_base, bufsize * VEC_COUNT);
                }
                else
                {
                    for (int i = 0; i < VEC_COUNT; ++i)
                        munmap(iov[i].iov_base, bufsize);
                }
            }
        }
        fprintf(stderr, "total memory filled: %td bytes\n", total_memsets);
        /* https://stackoverflow.com/questions/6400180/how-to-printf-long-long */
        fprintf(stderr, "cycles spent filling memory: %" PRId64 " millions\n", memset_ticks / 1000000);

        close(connfd);
     }
     
     return 0;
}


#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <linux/types.h>

#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif

#define SCHED_DEADLINE 6
#define SCHED_LOW_IDLE 7

#ifdef __x86_64__
#define __NR_sched_setattr 314
#define __NR_sched_getattr 315
#elif defined(__i386__)
#define __NR_sched_setattr 351
#define __NR_sched_getattr 352
#elif defined(__aarch64__)
#define __NR_sched_setattr 274
#define __NR_sched_getattr 275
#elif defined(__arm__)
#define __NR_sched_setattr 380
#define __NR_sched_getattr 381
#else
#error "Arquitetura nao suportada para sched_setattr"
#endif

#define MAX_THREADS 26
#define RT_PRIORITY 1
#define LINE_WIDTH 64
#define RAW_PRINT_MAX 20000
#define FIFO_YIELD_INTERVAL 1000
#define WRITE_CHUNK_SIZE 256

#define DEADLINE_RUNTIME (10ULL * 1000 * 1000)
#define DEADLINE_PERIOD  (30ULL * 1000 * 1000)

static char *buffer;
static size_t buffer_size;
static size_t next_index;
static sem_t ready;

struct sched_attr {
    __u32 size;
    __u32 sched_policy;
    __u64 sched_flags;
    __s32 sched_nice;
    __u32 sched_priority;
    __u64 sched_runtime;
    __u64 sched_deadline;
    __u64 sched_period;
};

struct thread_arg {
    int id;
    char ch;
    int policy;
    int err;
    pid_t tid;
    sem_t start;
    pthread_t thread;
};

static struct thread_arg targs[MAX_THREADS];

struct policy_name {
    const char *name;
    int value;
};

static const struct policy_name policies[] = {
    { "SCHED_LOW_IDLE", SCHED_LOW_IDLE },
    { "SCHED_IDLE", SCHED_IDLE },
    { "SCHED_FIFO", SCHED_FIFO },
    { "SCHED_RR", SCHED_RR },
    { "SCHED_OTHER", SCHED_OTHER },
    { "SCHED_DEADLINE", SCHED_DEADLINE }
};

#define NPOLICIES ((int)(sizeof(policies) / sizeof(policies[0])))

static int parse_policy(const char *s)
{
    int i;

    for (i = 0; i < NPOLICIES; i++) {
        const char *short_name = policies[i].name + 6;

        if (strcasecmp(s, policies[i].name) == 0 ||
            strcasecmp(s, short_name) == 0)
            return policies[i].value;
    }

    return -1;
}

static const char *policy_str(int policy)
{
    int i;

    for (i = 0; i < NPOLICIES; i++) {
        if (policies[i].value == policy)
            return policies[i].name;
    }

    return "unknown";
}

static int priority_for(int policy)
{
    return (policy == SCHED_FIFO || policy == SCHED_RR)
           ? RT_PRIORITY : 0;
}

static void print_sched(int policy)
{
    int priority_min;
    int priority_max;

    printf("%s", policy_str(policy));

    priority_min = sched_get_priority_min(policy);
    priority_max = sched_get_priority_max(policy);

    printf(" PRI_MIN: %d PRI_MAX: %d\n",
           priority_min, priority_max);
}

static int set_thread_policy(pthread_t *thr,
                             int newpolicy,
                             int newpriority)
{
    int policy;
    int ret;
    int priority_min;
    int priority_max;

    struct sched_param param;

    priority_min = sched_get_priority_min(newpolicy);
    priority_max = sched_get_priority_max(newpolicy);

    if (priority_min == -1 || priority_max == -1) {
        fprintf(stderr, "Politica invalida: %s\n",
                policy_str(newpolicy));
        return errno ? errno : EINVAL;
    }

    if (newpriority > priority_max || newpriority < priority_min) {
        fprintf(stderr,
                "Invalid priority: MIN: %d, MAX: %d\n",
                priority_min, priority_max);
        return EINVAL;
    }

    ret = pthread_getschedparam(*thr, &policy, &param);

    if (ret != 0) {
        fprintf(stderr, "pthread_getschedparam: %s\n",
                strerror(ret));
        return ret;
    }

    printf("current: ");
    print_sched(policy);

    param.sched_priority = newpriority;

    ret = pthread_setschedparam(*thr, newpolicy, &param);

    if (ret != 0) {
        fprintf(stderr, "pthread_setschedparam(%s): %s\n",
                policy_str(newpolicy), strerror(ret));
        return ret;
    }

    ret = pthread_getschedparam(*thr, &policy, &param);

    if (ret != 0) {
        fprintf(stderr, "pthread_getschedparam: %s\n",
                strerror(ret));
        return ret;
    }

    printf("new: ");
    print_sched(policy);

    if (policy != newpolicy) {
        fprintf(stderr, "Politica aplicada diferente da solicitada.\n");
        return EINVAL;
    }

    return 0;
}

static int sched_setattr(pid_t pid,
                         const struct sched_attr *attr,
                         unsigned int flags)
{
    return syscall(__NR_sched_setattr, pid, attr, flags);
}

static int sched_getattr(pid_t pid,
                         struct sched_attr *attr,
                         unsigned int size,
                         unsigned int flags)
{
    return syscall(__NR_sched_getattr, pid, attr, size, flags);
}

static int set_deadline(pid_t tid)
{
    struct sched_attr attr;
    struct sched_attr current;
    unsigned int flags = 0;

    int ret;

    ret = sched_getattr(tid, &current, sizeof(current), 0);

    if (ret < 0) {
        fprintf(stderr, "sched_getattr: %s\n", strerror(errno));
        return errno;
    }

    printf("current: %s\n", policy_str(current.sched_policy));

    memset(&attr, 0, sizeof(attr));

    attr.size = sizeof(attr);
    attr.sched_flags = 0;
    attr.sched_nice = 0;
    attr.sched_priority = 0;

    attr.sched_policy = SCHED_DEADLINE;
    attr.sched_runtime = DEADLINE_RUNTIME;
    attr.sched_deadline = DEADLINE_PERIOD;
    attr.sched_period = DEADLINE_PERIOD;

    ret = sched_setattr(tid, &attr, flags);

    if (ret < 0) {
        fprintf(stderr, "sched_setattr: %s\n", strerror(errno));
        return errno;
    }

    ret = sched_getattr(tid, &current, sizeof(current), 0);

    if (ret < 0) {
        fprintf(stderr, "sched_getattr: %s\n", strerror(errno));
        return errno;
    }

    printf("new: %s\n", policy_str(current.sched_policy));
    printf("runtime: %llu ns\n",
           (unsigned long long)current.sched_runtime);
    printf("deadline: %llu ns\n",
           (unsigned long long)current.sched_deadline);
    printf("period: %llu ns\n",
           (unsigned long long)current.sched_period);

    if (current.sched_policy != SCHED_DEADLINE)
        return EINVAL;

    return 0;
}

static void *worker(void *arg)
{
    struct thread_arg *t = arg;
    unsigned int writes_since_yield = 0;

    t->tid = (pid_t)syscall(SYS_gettid);

    sem_post(&ready);

    sem_wait(&t->start);

    if (t->err != 0)
        return NULL;

    for (;;) {
        size_t start = __sync_fetch_and_add(&next_index,
                                            WRITE_CHUNK_SIZE);
        size_t end;

        if (start >= buffer_size)
            break;

        end = start + WRITE_CHUNK_SIZE;
        if (end > buffer_size)
            end = buffer_size;

        memset(buffer + start, t->ch, end - start);

        if (t->policy == SCHED_FIFO) {
            writes_since_yield += end - start;
            if (writes_since_yield >= FIFO_YIELD_INTERVAL) {
                writes_since_yield -= FIFO_YIELD_INTERVAL;
                sched_yield();
            }
        }
    }

    return NULL;
}

static void print_wrapped(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        putchar(s[i]);

        if ((i + 1) % LINE_WIDTH == 0)
            putchar('\n');
    }

    if (n % LINE_WIDTH != 0)
        putchar('\n');
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s <tam_buffer> <num_threads> <politica>\n"
        "  politica: SCHED_LOW_IDLE | SCHED_IDLE | SCHED_FIFO\n"
        "            SCHED_RR | SCHED_OTHER | SCHED_DEADLINE\n"
        "  Tambem aceita lista separada por virgula.\n"
        "Exemplo: %s 10000 4 SCHED_RR\n"
        "Exemplo: %s 10000 3 SCHED_RR,SCHED_FIFO,SCHED_DEADLINE\n",
        prog, prog, prog);
}

int main(int argc, char **argv)
{
    long size;
    long nthreads;

    int list[MAX_THREADS];
    int nlist = 0;

    int i;
    int rc;
    int failed = 0;
    struct sched_param main_param;

    char *plist;
    char *tok;
    char *save = NULL;

    char *summary;

    size_t slen = 0;

    long counts[MAX_THREADS];
    long blocks[MAX_THREADS];

    char last;

    if (argc != 4) {
        usage(argv[0]);
        return 1;
    }

    size = atol(argv[1]);
    nthreads = atol(argv[2]);

    if (size <= 0 || nthreads <= 0 || nthreads > MAX_THREADS) {
        fprintf(stderr,
                "Erro: tam_buffer > 0 e 1 <= num_threads <= %d\n",
                MAX_THREADS);
        return 1;
    }

    plist = strdup(argv[3]);

    if (!plist) {
        perror("strdup");
        return 1;
    }

    for (tok = strtok_r(plist, ",", &save);
         tok && nlist < MAX_THREADS;
         tok = strtok_r(NULL, ",", &save)) {

        int p = parse_policy(tok);

        if (p < 0) {
            fprintf(stderr, "Erro: politica desconhecida '%s'\n", tok);
            free(plist);
            usage(argv[0]);
            return 1;
        }

        list[nlist++] = p;
    }

    free(plist);

    if (nlist == 0) {
        usage(argv[0]);
        return 1;
    }

    buffer = calloc((size_t)size, 1);

    if (!buffer) {
        perror("calloc");
        return 1;
    }

    buffer_size = (size_t)size;
    next_index = 0;
    sem_init(&ready, 0, 0);

    for (i = 0; i < nthreads; i++) {
        targs[i].id = i;
        targs[i].ch = 'A' + i;
        targs[i].policy = list[i % nlist];
        targs[i].err = 0;
        targs[i].tid = 0;

        sem_init(&targs[i].start, 0, 0);

        rc = pthread_create(&targs[i].thread,
                            NULL,
                            worker,
                            &targs[i]);

        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            return 1;
        }
    }

    for (i = 0; i < nthreads; i++)
        sem_wait(&ready);

    main_param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &main_param);
    if (rc != 0)
        fprintf(stderr, "Nao foi possivel priorizar a thread principal: %s\n",
                strerror(rc));

    for (i = 0; i < nthreads; i++) {

        printf("\n--- Thread %c ---\n", targs[i].ch);

        if (targs[i].policy == SCHED_DEADLINE) {
            rc = set_deadline(targs[i].tid);
        } else {
            rc = set_thread_policy(&targs[i].thread,
                                   targs[i].policy,
                                   priority_for(targs[i].policy));
        }

        if (rc != 0) {
            targs[i].err = rc;
            failed++;

            if (rc == EPERM) {
                fprintf(stderr,
                        "Thread %c: permissao insuficiente. "
                        "Pode ser necessario executar como root.\n",
                        targs[i].ch);
            }
        }
    }

    for (i = 0; i < nthreads; i++)
        sem_post(&targs[i].start);

    for (i = 0; i < nthreads; i++)
        pthread_join(targs[i].thread, NULL);

    if (failed) {
        fprintf(stderr,
                "\nExecucao invalida: %d thread(s) sem a politica pedida.\n",
                failed);
        free(buffer);
        return 1;
    }

    printf("\nParametros: tam_buffer=%ld threads=%ld politica=%s\n",
           size, nthreads, argv[3]);

    printf("\n--- Buffer (sem pos-processamento) ---\n");

    if (size <= RAW_PRINT_MAX) {
        print_wrapped(buffer, (size_t)size);
    } else {
        print_wrapped(buffer, RAW_PRINT_MAX);

        printf("[... mostrando as primeiras %d de %ld posicoes ...]\n",
               RAW_PRINT_MAX, size);
    }

    summary = malloc((size_t)size + 1);

    if (!summary) {
        perror("malloc");
        free(buffer);
        return 1;
    }

    memset(counts, 0, sizeof(counts));
    memset(blocks, 0, sizeof(blocks));

    last = 0;

    for (i = 0; i < size; i++) {
        char c = buffer[i];

        if (c == 0)
            continue;

        counts[c - 'A']++;

        if (c != last) {
            summary[slen++] = c;
            blocks[c - 'A']++;
            last = c;
        }
    }

    summary[slen] = '\0';

    printf("\n--- Apos pos-processamento ---\n");

    print_wrapped(summary, slen);

    printf("\n--- Blocos consecutivos e caracteres escritos ---\n");

    for (i = 0; i < nthreads; i++) {
        printf("%c = %ld blocos, %ld caracteres\n",
               targs[i].ch, blocks[i], counts[i]);
    }

    free(summary);
    free(buffer);

    return 0;
}
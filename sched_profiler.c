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

#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif

/* Politica criada no Tutorial 3.0 (nao existe nos headers da toolchain) */
#define SCHED_LOW_IDLE 7

#define MAX_THREADS    26
#define RT_PRIORITY    10
#define LINE_WIDTH     64
#define RAW_PRINT_MAX  20000

static char *buffer;
static size_t buffer_size;
static size_t next_index;     /* ponteiro global: proxima posicao livre */
static sem_t mutex;           /* semaforo binario da secao critica */
static sem_t ready;           /* threads avisam que estao prontas */

struct thread_arg {
    char ch;
    int policy;
    int err;
    sem_t start;              /* largada sincronizada */
    pthread_t thread;
};

static struct thread_arg targs[MAX_THREADS];

struct policy_name {
    const char *name;
    int value;
};

static const struct policy_name policies[] = {
    { "SCHED_LOW_IDLE", SCHED_LOW_IDLE },
    { "SCHED_IDLE",     SCHED_IDLE },
    { "SCHED_FIFO",     SCHED_FIFO },
    { "SCHED_RR",       SCHED_RR },
    { "SCHED_OTHER",    SCHED_OTHER }
};

#define NPOLICIES ((int)(sizeof(policies) / sizeof(policies[0])))

static int parse_policy(const char *s)
{
    int i;

    for (i = 0; i < NPOLICIES; i++) {
        const char *short_name = policies[i].name + 6; /* sem "SCHED_" */

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
    return (policy == SCHED_FIFO || policy == SCHED_RR) ? RT_PRIORITY : 0;
}

static void print_sched(int policy)
{
    printf("%s PRI_MIN: %d PRI_MAX: %d\n",
           policy_str(policy),
           sched_get_priority_min(policy),
           sched_get_priority_max(policy));
}

static int set_thread_policy(pthread_t *thr, int newpolicy, int newpriority)
{
    int policy;
    int ret;
    int priority_min = sched_get_priority_min(newpolicy);
    int priority_max = sched_get_priority_max(newpolicy);
    struct sched_param param;

    if (priority_min == -1 || priority_max == -1) {
        fprintf(stderr, "Politica invalida: %s\n", policy_str(newpolicy));
        return errno ? errno : EINVAL;
    }

    if (newpriority > priority_max || newpriority < priority_min) {
        fprintf(stderr, "Invalid priority: MIN: %d, MAX: %d\n",
                priority_min, priority_max);
        return EINVAL;
    }

    ret = pthread_getschedparam(*thr, &policy, &param);
    if (ret != 0) {
        fprintf(stderr, "pthread_getschedparam: %s\n", strerror(ret));
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
        fprintf(stderr, "pthread_getschedparam: %s\n", strerror(ret));
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

static void *worker(void *arg)
{
    struct thread_arg *t = arg;

    sem_post(&ready);      /* "estou pronta" */
    sem_wait(&t->start);   /* espera a largada */

    if (t->err != 0)
        return NULL;

    for (;;) {
        sem_wait(&mutex);              /* entra na secao critica */

        if (next_index >= buffer_size) {
            sem_post(&mutex);
            break;
        }

        buffer[next_index] = t->ch;    /* escreve na posicao corrente */
        next_index++;                  /* incrementa o ponteiro global */

        sem_post(&mutex);              /* sai da secao critica */
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
        "            SCHED_RR | SCHED_OTHER\n"
        "  Tambem aceita lista separada por virgula (uma por thread).\n"
        "Exemplo: %s 5000000 4 SCHED_RR\n"
        "Exemplo: %s 5000000 2 SCHED_LOW_IDLE,SCHED_IDLE\n",
        prog, prog, prog);
}

int main(int argc, char **argv)
{
    long size;
    long nthreads;
    long i;
    int rc;
    int failed = 0;

    int list[MAX_THREADS];
    int nlist = 0;

    struct sched_param main_param;

    char *plist;
    char *tok;
    char *save = NULL;

    long counts[MAX_THREADS];
    long blocks[MAX_THREADS];
    long col = 0;
    char last;

    if (argc != 4) {
        usage(argv[0]);
        return 1;
    }

    size = atol(argv[1]);
    nthreads = atol(argv[2]);

    if (size <= 0 || nthreads <= 0 || nthreads > MAX_THREADS) {
        fprintf(stderr, "Erro: tam_buffer > 0 e 1 <= num_threads <= %d\n",
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
    sem_init(&mutex, 0, 1);            /* semaforo binario */

    /* cria as threads; todas ficam bloqueadas em t->start */
    for (i = 0; i < nthreads; i++) {
        targs[i].ch = 'A' + (char)i;
        targs[i].policy = list[i % nlist];
        targs[i].err = 0;

        sem_init(&targs[i].start, 0, 0);

        rc = pthread_create(&targs[i].thread, NULL, worker, &targs[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            return 1;
        }
    }

    for (i = 0; i < nthreads; i++)
        sem_wait(&ready);

    /* main sobe para FIFO 99 para configurar todas antes da largada */
    main_param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &main_param);
    if (rc != 0)
        fprintf(stderr, "Nao foi possivel priorizar a thread principal: %s\n",
                strerror(rc));

    for (i = 0; i < nthreads; i++) {
        printf("\n--- Thread %c ---\n", targs[i].ch);

        rc = set_thread_policy(&targs[i].thread,
                               targs[i].policy,
                               priority_for(targs[i].policy));

        if (rc != 0) {
            targs[i].err = rc;
            failed++;

            if (rc == EPERM)
                fprintf(stderr,
                        "Thread %c: permissao insuficiente. "
                        "Pode ser necessario executar como root.\n",
                        targs[i].ch);
        }
    }

    /* largada */
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

    memset(counts, 0, sizeof(counts));
    memset(blocks, 0, sizeof(blocks));
    last = 0;

    printf("\n--- Apos pos-processamento ---\n");

    for (i = 0; i < size; i++) {
        char c = buffer[i];

        if (c == 0)
            continue;

        counts[c - 'A']++;

        if (c != last) {
            putchar(c);
            if (++col % LINE_WIDTH == 0)
                putchar('\n');
            blocks[c - 'A']++;
            last = c;
        }
    }

    if (col % LINE_WIDTH != 0)
        putchar('\n');

    printf("\n--- Blocos consecutivos e caracteres escritos ---\n");

    for (i = 0; i < nthreads; i++)
        printf("%c = %ld blocos, %ld caracteres\n",
               targs[i].ch, blocks[i], counts[i]);

    free(buffer);

    return 0;
}
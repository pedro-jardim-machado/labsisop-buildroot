#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <time.h>

#define SCHED_IDLE 5

#define SCHED_LOW_IDLE 7

#define MAX_THREADS    26
#define LINE_WIDTH     64
#define MAX_PRINT  10000

static char *buffer;
static size_t buffer_size;
static size_t next_index;     
static sem_t mutex;          
static sem_t ready;         

struct thread_arg {
    char ch;
    int policy;
    sem_t start;                   /* largada sincronizada */
    pthread_t thread;
};

static struct thread_arg targs[MAX_THREADS];

static const struct {
    const char *name;
    int value;
} policies[] = {
    { "SCHED_LOW_IDLE", SCHED_LOW_IDLE },
    { "SCHED_IDLE",     SCHED_IDLE },
    { "SCHED_FIFO",     SCHED_FIFO },
    { "SCHED_RR",       SCHED_RR }
};

#define NPOLICIES ((int)(sizeof(policies) / sizeof(policies[0]))) // length

static int parse_policy(const char *s)
{
    int i;

    for (i = 0; i < NPOLICIES; i++)
        if (strcmp(s, policies[i].name) == 0)
            return policies[i].value;

    return -1;
}

static const char *policy_str(int policy)
{
    int i;

    for (i = 0; i < NPOLICIES; i++)
        if (policies[i].value == policy)
            return policies[i].name;

    return "unknown";
}

// aplica a politica 
static int set_thread_policy(pthread_t thr, int policy)
{
    struct sched_param param;
    int applied;
    int ret;

    param.sched_priority =
        (policy == SCHED_FIFO || policy == SCHED_RR) ? 10 : 0;

    ret = pthread_setschedparam(thr, policy, &param);
    if (ret != 0) {
        fprintf(stderr, "pthread_setschedparam(%s): %s\n",
                policy_str(policy), strerror(ret));
        return ret;
    }

    pthread_getschedparam(thr, &applied, &param);
    printf("new: %s prioridade=%d\n",
           policy_str(applied), param.sched_priority);

    return 0;
}

static void *worker(void *arg)
{
    struct thread_arg *t = arg;

    sem_post(&ready);
    sem_wait(&t->start);

    for (;;) {
        sem_wait(&mutex);              /* secao critica */

        if (next_index >= buffer_size) {
            sem_post(&mutex);
            break;
        }

        buffer[next_index] = t->ch;    /* escreve na posicao corrente */
        next_index++;                  

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
        "  politica: SCHED_LOW_IDLE | SCHED_IDLE | SCHED_FIFO | SCHED_RR\n"
        "Exemplo: %s 5000000 4 SCHED_RR\n",
        prog, prog, prog);
}

int main(int argc, char **argv)
{
    long size, nthreads, i;
    int rc;
    struct timespec start_time, end_time;
    double elapsed;

    int list[MAX_THREADS];
    int nlist = 0;

    struct sched_param main_param;
    char *plist, *tok, *save = NULL;

    long counts[MAX_THREADS] = { 0 };
    long blocks[MAX_THREADS] = { 0 };
    long col = 0;
    char last = 0;

    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        perror("clock_gettime");
        return 1;
    }

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
            usage(argv[0]);
            return 1;
        }

        list[nlist++] = p;
    }

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
    sem_init(&ready, 0, 0);
    sem_init(&mutex, 0, 1);            /* semaforo binario */

    /* cria as thread, todas ficam bloqueadas */
    for (i = 0; i < nthreads; i++) {
        targs[i].ch = 'A' + (char)i;
        targs[i].policy = list[i % nlist];
        sem_init(&targs[i].start, 0, 0);

        rc = pthread_create(&targs[i].thread, NULL, worker, &targs[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            return 1;
        }
    }

    for (i = 0; i < nthreads; i++)
        sem_wait(&ready);

    /* main sobe para FIFO 99 para configurar todas */
    main_param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &main_param);
    if (rc != 0)
        fprintf(stderr, "Nao foi possivel priorizar a thread principal: %s\n",
                strerror(rc));

    for (i = 0; i < nthreads; i++) {
        printf("\n--- Thread %c ---\n", targs[i].ch);

        rc = set_thread_policy(targs[i].thread, targs[i].policy);
        if (rc != 0) {
            if (rc == EPERM)
                fprintf(stderr, "Permissao insuficiente: execute como root.\n");
            fprintf(stderr, "Execucao invalida: thread %c sem a politica pedida.\n",
                    targs[i].ch);
            return 1;   /* as threads bloqueadas  */
        }
    }

    /* largada */
    for (i = 0; i < nthreads; i++)
        sem_post(&targs[i].start);

    for (i = 0; i < nthreads; i++)
        pthread_join(targs[i].thread, NULL);

    printf("\nParametros: tam_buffer=%ld threads=%ld politica=%s\n",
           size, nthreads, argv[3]);

    printf("\n--- Buffer ---\n");

    if (size <= MAX_PRINT) {
        print_wrapped(buffer, (size_t)size);
    } else {
        print_wrapped(buffer, MAX_PRINT);
        printf("[... mostrando as primeiras %d de %ld posicoes ...]\n",
               MAX_PRINT, size);
    }

    printf("\n--- Sequencia compactada ---\n");

    for (i = 0; i < size; i++) {
        char c = buffer[i];

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

    printf("\n--- Resumo por thread ---\n");
    for (i = 0; i < nthreads; i++)
        printf("%c = %ld, %ld caracteres\n",
               targs[i].ch, blocks[i], counts[i]);

    if (clock_gettime(CLOCK_MONOTONIC, &end_time) != 0) {
        perror("clock_gettime");
        return 1;
    }

    elapsed = (end_time.tv_sec - start_time.tv_sec) +
              (end_time.tv_nsec - start_time.tv_nsec) / 1e9;
    printf("Tempo total do processo: %.3f segundos\n", elapsed);

    return 0;
}
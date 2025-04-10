#include <arpa/inet.h>
#include <bits/getopt_core.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include "io_helper.h"
#include "request.h"

#define MAXBUF 8192

char default_root[] = ".";
int default_port = 10000;
int default_threads = 1;
int default_buffers = 1;
char default_schedalg[] = "FIFO";
int default_sff_delay_ms = 200; 

typedef enum { FIFO_SCHED, SFF_SCHED } sched_alg_t;
sched_alg_t sched = FIFO_SCHED;

unsigned long seq_counter = 0;

typedef struct {
    int conn_fd;
    int filesize;
    unsigned long seq;
} request_t;

request_t *request_heap = NULL;
int heap_size = 0;
int buffer_capacity = 0;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;

int request_cmp(const request_t *a, const request_t *b) {
    if (sched == FIFO_SCHED) {
        if (a->seq < b->seq) return -1;
        else if (a->seq > b->seq) return 1;
        else return 0;
    } else {
        if (a->filesize < b->filesize) return -1;
        else if (a->filesize > b->filesize) return 1;
        else {
            if (a->seq < b->seq) return -1;
            else if (a->seq > b->seq) return 1;
            else return 0;
        }
    }
}

void swap(request_t *a, request_t *b) {
    request_t temp = *a;
    *a = *b;
    *b = temp;
}

void heapify_up(int index) {
    while (index > 0) {
        int parent = (index - 1) / 2;
        if (request_cmp(&request_heap[index], &request_heap[parent]) < 0) {
            swap(&request_heap[index], &request_heap[parent]);
            index = parent;
        } else
            break;
    }
}

void heapify_down(int index) {
    while (1) {
        int smallest = index;
        int left = 2 * index + 1;
        int right = 2 * index + 2;
        if (left < heap_size && request_cmp(&request_heap[left], &request_heap[smallest]) < 0)
            smallest = left;
        if (right < heap_size && request_cmp(&request_heap[right], &request_heap[smallest]) < 0)
            smallest = right;
        if (smallest != index) {
            swap(&request_heap[index], &request_heap[smallest]);
            index = smallest;
        } else
            break;
    }
}

void heap_insert(request_t req) {
    if (heap_size >= buffer_capacity) {
        fprintf(stderr, "Heap is full while inserting request\n");
        exit(1);
    }
    request_heap[heap_size] = req;
    heapify_up(heap_size);
    heap_size++;
}

request_t heap_extract_min() {
    if (heap_size <= 0) {
        fprintf(stderr, "Heap is empty on extract\n");
        exit(1);
    }
    request_t min_req = request_heap[0];
    heap_size--;
    if (heap_size > 0) {
        request_heap[0] = request_heap[heap_size];
        heapify_down(0);
    }
    return min_req;
}

void *worker_thread(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&mutex);
        while (heap_size == 0)
            pthread_cond_wait(&not_empty, &mutex);

        if (sched == SFF_SCHED && heap_size == 1) {
            pthread_mutex_unlock(&mutex);
            usleep(default_sff_delay_ms * 1000);
            pthread_mutex_lock(&mutex);
        }

        request_t req = heap_extract_min();
        pthread_cond_signal(&not_full);
        pthread_mutex_unlock(&mutex);

        request_handle(req.conn_fd);
        close_or_die(req.conn_fd);
    }
    return NULL;
}

int main(int argc, char *argv[]){
    int c;
    char *root_dir = default_root;
    int port = default_port;
    int num_threads = default_threads;
    int num_buffers = default_buffers;
    char *schedalg_str = default_schedalg;
    int sff_delay_ms = default_sff_delay_ms;

    while((c = getopt(argc, argv, "d:p:t:b:s:w:")) != -1)
        switch(c) {
            case 'd':
                root_dir = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                break;
            case 't':
                num_threads = atoi(optarg);
                break;
            case 'b':
                num_buffers = atoi(optarg);
                break;
            case 's':
                schedalg_str = optarg;
                break;
            case 'w':
                sff_delay_ms = atoi(optarg);
                break;
            default:
                fprintf(stderr, "usage: pserver [-d basedir] [-p port] [-t threads] [-b buffers] [-s schedalg] [-w sff_delay_ms]\n");
                exit(1);
        }

    if (port < 1024 || port > 65535) {
        fprintf(stderr, "Invalid port number: %d. Port must be between 1024 and 65535.\n", port);
        exit(1);
    }

    if (strcasecmp(schedalg_str, "SFF") == 0)
        sched = SFF_SCHED;
    else
        sched = FIFO_SCHED;

    default_sff_delay_ms = sff_delay_ms;
    chdir_or_die(root_dir);

    buffer_capacity = num_buffers;
    request_heap = malloc(sizeof(request_t) * buffer_capacity);
    if (request_heap == NULL) {
        perror("malloc");
        exit(1);
    }
    heap_size = 0;

    pthread_t *thread_ids = malloc(sizeof(pthread_t) * num_threads);
    if (thread_ids == NULL) {
        perror("malloc");
        free(request_heap);
        exit(1);
    }
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&thread_ids[i], NULL, worker_thread, NULL) != 0) {
            perror("pthread_create");
            free(request_heap);
            free(thread_ids);
            exit(1);
        }
        if (pthread_detach(thread_ids[i]) != 0) {
            perror("pthread_detach");
            free(request_heap);
            free(thread_ids);
            exit(1);
        }
    }

    int listen_fd = open_listen_fd_or_die(port);
    printf("Server listening... on port %d\n", port);
    while(1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int conn_fd = accept_or_die(listen_fd, (sockaddr_t *)&client_addr, (socklen_t *)&client_len);

        int filesize = 0;
        char peek_buf[MAXBUF];

        if (sched == SFF_SCHED) {
            int n = recv(conn_fd, peek_buf, MAXBUF, MSG_PEEK);
            if (n <= 0) {
                close_or_die(conn_fd);
                continue;
            }
            char line[MAXBUF];
            int i = 0;
            while (i < n && i < MAXBUF - 1 && peek_buf[i] != '\n') {
                line[i] = peek_buf[i];
                i++;
            }
            line[i] = '\0';

            char method[16], uri[256], version[16];
            if (sscanf(line, "%s %s %s", method, uri, version) != 3) {
                close_or_die(conn_fd);
                continue;
            }

            char filename_buf[MAXBUF];
            char cgiargs[MAXBUF];
            if (!strstr(uri, "cgi")) {
                strcpy(cgiargs, "");
                snprintf(filename_buf, sizeof(filename_buf), ".%s", uri);
                if(uri[strlen(uri) - 1] == '/')
                    strcat(filename_buf, "index.html");
            } else {
                char *ptr = strchr(uri, '?');
                if (ptr) {
                    strcpy(cgiargs, ptr + 1);
                    *ptr = '\0';
                } else
                    strcpy(cgiargs, "");
                snprintf(filename_buf, sizeof(filename_buf), ".%s", uri);
            }

            struct stat sbuf;
            if (stat(filename_buf, &sbuf) < 0)
                filesize = INT_MAX;
            else
                filesize = sbuf.st_size;
        }

        request_t req;
        req.conn_fd = conn_fd;
        req.filesize = filesize;
        req.seq = seq_counter++;

        pthread_mutex_lock(&mutex);
        while (heap_size == buffer_capacity)
            pthread_cond_wait(&not_full, &mutex);
        heap_insert(req);
        pthread_cond_signal(&not_empty);
        pthread_mutex_unlock(&mutex);
    }
    free(request_heap);
    free(thread_ids);
    return 0;
}

#include <arpa/inet.h>
#include <bits/getopt_core.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "io_helper.h"
#include "request.h"

#define MAXBUF 8192

char default_root[] = ".";
int default_port = 10000;
int default_threads = 1;
int default_buffers = 1;
char default_schedalg[] = "FIFO";

//
// ./pserver [-d <basedir>] [-p <portnum>] 
// 
typedef enum { FIFO_SCHED, SFF_SCHED } sched_alg_t;
sched_alg_t sched = FIFO_SCHED;

typedef struct {
	int conn_fd;
	int filesize;
} request_t;

request_t *request_buffer = NULL;
int buffer_capacity = 0;
int request_count = 0;
int head = 0;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;

void *worker_thread(void *arg) {
    (void)arg;
    while(1){
        pthread_mutex_lock(&mutex);
        while(request_count == 0)
            pthread_cond_wait(&not_empty, &mutex);
        request_t req;
        if(sched == FIFO_SCHED){
            req = request_buffer[head];
            head = (head + 1) % buffer_capacity;
            request_count--;
        } else {
            int min_relative = 0;
            int min_index = (head + 0) % buffer_capacity;
            for(int i = 1; i < request_count; i++){
                int idx = (head + i) % buffer_capacity;
                if(request_buffer[idx].filesize < request_buffer[min_index].filesize){
                    min_relative = i;
                    min_index = idx;
                }
            }
            req = request_buffer[min_index];
            int last_index = (head + request_count - 1) % buffer_capacity;
            request_buffer[min_index] = request_buffer[last_index];
            request_count--;
            if(min_index == head)
                head = (head + 1) % buffer_capacity;
        }
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

	while((c = getopt(argc, argv, "d:p:t:b:s:")) != -1)
		switch(c){
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
		default:
			fprintf(stderr, "usage: pserver [-d basedir] [-p port] [-t threads] [-b buffers] [-s schedalg]\n");
			exit(1);
		}

	// run out of this directory
	if (port < 1024 || port > 65535) {
		fprintf(stderr, "Invalid port number: %d. Port must be between 1024 and 65535.\n", port);
		exit(1);
	}

	if(strcmp(schedalg_str, "SFF") == 0)
		sched = SFF_SCHED;
	else
		sched = FIFO_SCHED;

	chdir_or_die(root_dir);

	// now, get to work
	buffer_capacity = num_buffers;
	request_buffer = malloc(sizeof(request_t) * buffer_capacity);
	if(request_buffer == NULL){
		perror("malloc");
		exit(1);
	}

	pthread_t *thread_ids = malloc(sizeof(pthread_t) * num_threads);
	if (thread_ids == NULL) {
		perror("malloc");
		free(request_buffer);
		exit(1);
	}
	for(int i = 0; i < num_threads; i++){
		if(pthread_create(&thread_ids[i], NULL, worker_thread, NULL) != 0){
			perror("pthread_create");
			free(request_buffer);
			free(thread_ids);
			exit(1);
		}
		if(pthread_detach(thread_ids[i]) != 0){
			perror("pthread_detach");
			free(request_buffer);
			free(thread_ids);
			exit(1);
		}
	}	

	int listen_fd = open_listen_fd_or_die(port);
	printf("Server listening... on port %d\n", port);
	while(1){
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		int conn_fd = accept_or_die(listen_fd, (sockaddr_t *)&client_addr, (socklen_t *)&client_len);

		int filesize = 0;

		if(sched == SFF_SCHED){
			char peek_buf[MAXBUF];
			int n = recv(conn_fd, peek_buf, MAXBUF, MSG_PEEK);
			if(n <= 0){
				close_or_die(conn_fd);
				continue;
			}
			char line[MAXBUF];
			int i = 0;
			while(i < n && i < MAXBUF - 1 && peek_buf[i] != '\n'){
				line[i] = peek_buf[i];
				i++;
			}
			line[i] = '\0';

			char method[16], uri[256], version[16];
			if(sscanf(line, "%s %s %s", method, uri, version) != 3){
				close_or_die(conn_fd);
				continue;
			}

			char filename_buf[MAXBUF];
			char cgiargs[MAXBUF];
			if(!strstr(uri, "cgi")){
				strcpy(cgiargs, "");
				snprintf(filename_buf, sizeof(filename_buf), ".%s", uri);
				if(uri[strlen(uri) - 1] == '/')
					strcat(filename_buf, "index.html");
			} else {
				char *ptr = strchr(uri, '?');
				if(ptr){
					strcpy(cgiargs, ptr + 1);
					*ptr = '\0';
				} else
					strcpy(cgiargs, "");
				snprintf(filename_buf, sizeof(filename_buf), ".%s", uri);
			}

			struct stat sbuf;
			if(stat(filename_buf, &sbuf) < 0)
				filesize = INT_MAX;
			else
				filesize = sbuf.st_size;
		}

		request_t req;
		req.conn_fd = conn_fd;
		req.filesize = filesize;

		pthread_mutex_lock(&mutex);
		while(request_count == buffer_capacity)
			pthread_cond_wait(&not_full, &mutex);
		int tail = (head + request_count) % buffer_capacity;
		request_buffer[tail] = req;
		request_count++;
		pthread_cond_signal(&not_empty);
		pthread_mutex_unlock(&mutex);
	}
	free(request_buffer);
	free(thread_ids);
	return 0;
}

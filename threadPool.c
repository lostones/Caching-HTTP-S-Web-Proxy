/* 
 * A simple HTTP server with thread pool
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <pthread.h>
#include <syscall.h>
#include <signal.h>

#define BUFSIZE 1024
#define PORTNAME 3000
#define POOLSIZE 10

typedef struct stack {
	int fd;
	struct stack * next;
} STACK;

STACK * requests;
pthread_t ** threads;
/* 
 * error - wrapper for perror
 */
void error(char *msg) {
    perror(msg);
    exit(0);
}

pthread_mutex_t stackMutex;
pthread_cond_t stackCond;
pthread_mutex_t exitMutex;
pthread_cond_t exitCond;

int running = 1;
int exited = 0;
int serviced = 0;

int listenfd, portno, n;

int stackEmpty() {
	return requests != NULL && requests->fd ? 0 : 1;
}

STACK * pop() {
	if (stackEmpty()) return NULL;
	STACK * tos = requests;
	requests = requests->next;
	return tos;
}

// Only called by main thread whose on main method is not synchronized, therefore needs locking here
void push(int fd) {
	pthread_mutex_lock(&stackMutex);
	STACK * newReq = (STACK*) malloc(sizeof(STACK));
	newReq->fd = fd;
	newReq->next = requests;
	requests = newReq;
	//pthread_cond_broadcast(&stackCond);
	pthread_cond_signal(&stackCond);
	pthread_mutex_unlock(&stackMutex);	
}

void incrExit(int reqsServiced) {
	pthread_mutex_lock(&exitMutex);
	long threadId = syscall(SYS_gettid);
	if (++exited == POOLSIZE) {
		printf("Last thread %ld exiting, about to signal main thread\n", threadId);
		pthread_cond_signal(&exitCond);
	}
	printf("Thread %ld is %d to exit\n", threadId, exited);
	serviced += reqsServiced;
	pthread_mutex_unlock(&exitMutex);
}

void * serviceRequest() {
	long threadId = syscall(SYS_gettid);
	printf("Thread %ld created\n", threadId);
	int requestsServiced = 0;
	while(running) {
		pthread_mutex_lock(&stackMutex);
		while(stackEmpty() && running) {
			printf("Thread %ld checking status of stack\n", threadId);
			pthread_cond_wait(&stackCond, &stackMutex);
			printf("Thread %ld returned from wait\n", threadId);
		}
		if (running) {
			STACK * tos = pop();
			int connFd = tos->fd;
			requestsServiced ++;
			printf("Thread %ld servicing request using socket fd %d\n", threadId, connFd);
			free(tos);
			char response[23] = "Booyakasha Bounty!\r\n\r\n\0";
        		int n = write(connFd, response, strlen(response));
        		if (n < 0) {
                		printf("Thread id %ld unable to write to client\n", threadId);
        		}
        		close(connFd);
		}
		pthread_mutex_unlock(&stackMutex);
	}
	printf("Thread %ld exiting after serving %d requests\n", threadId, requestsServiced);
	incrExit(requestsServiced);
}

void spawnThreads() {
	threads = (pthread_t**) malloc(POOLSIZE * sizeof(pthread_t*));
	for (int thread = 0; thread < POOLSIZE; thread ++) {
		*(threads + thread) = (pthread_t*) malloc(sizeof(pthread_t));
		pthread_create(*(threads + thread), NULL, serviceRequest, NULL);
	}
}

void freeAll() {
	STACK * current = requests;
	STACK * next;
	while(current != NULL) {
		next = current->next;
		free(current);
		current = next;
	}
	for (int thread = 0; thread < POOLSIZE; thread ++) {
		free(*(threads + thread));	
	}
	free(threads);
}

void quit() {
	pthread_mutex_lock(&exitMutex);
	pthread_mutex_lock(&stackMutex);
	running = 0;
	pthread_cond_broadcast(&stackCond);
	pthread_mutex_unlock(&stackMutex);
	puts("Main thread waiting for pool threads to exit");
	pthread_cond_wait(&exitCond, &exitMutex);
	printf("Main thread about to exit\n");
	freeAll();
	close(listenfd);
	pthread_mutex_unlock(&exitMutex);
	pthread_mutex_destroy(&stackMutex);
	pthread_mutex_destroy(&exitMutex);
	pthread_cond_destroy(&stackCond);
	pthread_cond_destroy(&exitCond);
	puts("Freed all resources");
	printf("Total requests serviced : %d\n", serviced);
	exit(0);
}

int main(int argc, char **argv) {
    pthread_mutex_init(&stackMutex, NULL);
    pthread_cond_init(&stackCond, NULL);
    pthread_mutex_init(&exitMutex, NULL);
    printf("Main thread of id %ld about to spawn pool threads\n", syscall(SYS_gettid));
    signal(SIGINT, quit);
    puts("Registered keyboard-interrupt signal-handler");
    spawnThreads();
    struct sockaddr_in serveraddr, cliaddr;
    char *hostname;
    char buf[BUFSIZE];

    socklen_t socklen;

    portno = PORTNAME;
    /* socket: create the socket */
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) 
        error("ERROR opening listening socket");

    /* build the server's Internet address */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons(portno);
    if(bind(listenfd, (struct sockaddr_in *) &serveraddr, (socklen_t)sizeof(serveraddr))) {
	error("Unable to bind");
    }
 
    int connfd;
    char response[23] = "Booyakasha Bounty!\r\n\r\n\0";
    socklen_t len = (socklen_t) sizeof(cliaddr);
    listen(listenfd, 10);
    printf("Listening on socket of fd %d\n", listenfd);
    while (running) {
	if ((connfd = accept(listenfd, &cliaddr, &len)) == -1) {
		error("Unable to accept connection");
	}
	printf("Accepted new connection on socket %d\n", connfd);
	push(connfd);
	printf("Pushed connection %d onto stack\n", connfd);
    }
    
    close(listenfd);
    return 0;
}
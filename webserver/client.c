/* Generic */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <semaphore.h>
#include <pthread.h>

/* Network */
#include <netdb.h>
#include <sys/socket.h>

#define BUF_SIZE 100
#define VERSION 23
#define BUFSIZE 8096
#define ERROR      42
#define LOG        44
#define FORBIDDEN 403
#define NOTFOUND  404

typedef struct files
{
  char *file_one;
  char *file_two;
}files;

typedef struct connection
{
  int id;
  int file_flag;
  char* hostname;
  char* port;
  char* file_one;
  char* file_two;
}connection;

int size;
pthread_t *thread_pool;
sem_t semaphore;
pthread_barrier_t barrier;
pthread_mutex_t mutex;
int scheduling_flag; // scheduling_flag = 0 for CONCUR. scheduling_flag = 1 for FIFO


// Get host information (used to establishConnection)
struct addrinfo *getHostInfo(char* host, char* port) {
  int r;
  struct addrinfo hints, *getaddrinfo_res;
  // Setup hints
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if ((r = getaddrinfo(host, port, &hints, &getaddrinfo_res))) {
    fprintf(stderr, "[getHostInfo:21:getaddrinfo] %s\n", gai_strerror(r));
    return NULL;
  }

  return getaddrinfo_res;
}

// Establish connection with host
int establishConnection(struct addrinfo *info) {
  if (info == NULL) return -1;

  int clientfd;
  for (;info != NULL; info = info->ai_next) {
    if ((clientfd = socket(info->ai_family,
                           info->ai_socktype,
                           info->ai_protocol)) < 0) {
      perror("[establishConnection:35:socket]");
      continue;
    }

    if (connect(clientfd, info->ai_addr, info->ai_addrlen) < 0) {
      close(clientfd);
      perror("[establishConnection:42:connect]");
      continue;
    }

    freeaddrinfo(info);
    return clientfd;
  }

  freeaddrinfo(info);
  return -1;
}

void Ignore(int ignore){
	return;
}

// Send GET request
void GET(int clientfd, char *path) {
  char req[1000] = {0};
  sprintf(req, "GET %s HTTP/1.0\r\n\r\n", path);
  send(clientfd, req, strlen(req), 0);
}

void concur_routine(connection *con)
{
  while(1)
  {
    int clientfd;
    pthread_mutex_lock(&mutex);
    // Establish connection with <hostname>:<port>
    clientfd = establishConnection(getHostInfo(con->hostname, con->port));
    if (clientfd == -1) {
      fprintf(stderr,
              "[main] Failed to connect to: %s:%s \n",
              con->hostname, con->port);
      exit(1);
    }
    char buffer[BUF_SIZE];
    char *file = (con->file_flag == 0) ? con->file_one : con->file_two;
    // Send first GET request > stdout
    GET(clientfd, file);
    pthread_mutex_unlock(&mutex);
    while (recv(clientfd, buffer, BUF_SIZE, 0) > 0) {
      fputs(buffer, stdout);
      memset(buffer, 0, BUF_SIZE);
    }
    if(pthread_barrier_wait(&barrier) == PTHREAD_BARRIER_SERIAL_THREAD){
      con->file_flag = (con->file_flag == 0) ? 1 : 0;
    }
  }
}

void fifo_routine(connection *con)
{
  while(1)
  {
    int clientfd;
    sem_wait(&semaphore);
    // Establish connection with <hostname>:<port>
    clientfd = establishConnection(getHostInfo(con->hostname, con->port));
    if (clientfd == -1) {
      fprintf(stderr,
              "[main] Failed to connect to: %s:%s \n",
              con->hostname, con->port);
      exit(1);
    }
    char buffer[BUF_SIZE];
    // Send first GET request > stdout
    char *file = (con->file_flag == 0) ? con->file_one : con->file_two;
    GET(clientfd, file);
    sem_post(&semaphore);
    while (recv(clientfd, buffer, BUF_SIZE, 0) > 0) {
      fputs(buffer, stdout);
      memset(buffer, 0, BUF_SIZE);
    }
    if(pthread_barrier_wait(&barrier) == PTHREAD_BARRIER_SERIAL_THREAD){
      con->file_flag = (con->file_flag == 0) ? 1 : 0;
    }
  }
}

void* routine(void *args)
{
  if(scheduling_flag == 0) { // if CONCUR scheduling policy
    concur_routine(args);
  }
  else { // if FIFO scheduling policy
    fifo_routine(args);
  }
  return NULL;
}


void Pthread_create(int i, char* file_one, char* file_two, char* hostname, char* socketfd){
  int *id = (int *)malloc(sizeof(int));
  *id = i;
  connection* con;
  con = (connection*)malloc(sizeof(connection));
  con->file_flag = 0;
  con->file_one = file_one;
  con->file_two = file_two;
  con->hostname = hostname;
  con->id = i;
  con->port = socketfd;
	if(pthread_create(&thread_pool[i], NULL, &routine, con) != 0)
	{
		perror("Error: failed to create thread.\n");		
	}
}

void get_arguements(int argc, char **argv, int *num_threads, char **schedalg){
  //make sure we have the correct amount of inputs 
  if ((argc != 6) && (argc != 7)) {
    fprintf(stderr, "USAGE: ./httpclient <hostname> <port> <threads> <schedule algorithm> <filename 1> <filename 2>\n");
    exit(1);
  }

  // Get the threads available
  if((*num_threads = atoi(argv[3])) < 1){
    fprintf(stderr, "Number of threads must be a positive integer\n");
    exit(1);
  }

  // Get the schedule algorithm "CONCUR" or "FIFO"
  *schedalg = argv[4];
  if(!strcmp(*schedalg,"CONCUR") && !strcmp(*schedalg,"FIFO")) {
		(void)printf("ERROR: Invalid scheduling algorithm: %s, see nweb -?\n",*schedalg);
		exit(1);
	}
}

int main(int argc, char **argv) {
  int  num_threads;
  int i;
  char* schedalg; 
  files contents;
  char* hostname;
  char* port;

  get_arguements(argc, argv, &num_threads, &schedalg);

  scheduling_flag = strcmp(schedalg, "FIFO") == 0 ? 1 : 0; // set scheduling flag to 1 if FIFO or 0 if CONCUR
  pthread_barrier_init(&barrier, NULL, num_threads);

  if(scheduling_flag == 0) { // Concur use mutex lock and barrier
    pthread_mutex_init(&mutex, NULL);
  }
  else {
    sem_init(&semaphore, 0, 1); // FIFO use the semaphore
  }

  contents = *(files*)malloc(sizeof(files));
  hostname = argv[1];
  port = argv[2];
  contents.file_one = argv[5];
  contents.file_two = (argv[6] == NULL) ? argv[5] : argv[6];
  
  size = num_threads;
  // Initialize the thread pool with the available worker threads 
	thread_pool = (pthread_t*)malloc(num_threads * sizeof(pthread_t));	
	
	// Run through the pool
	for (i = 0; i < num_threads; i++)
	{
		// Create the pthread with the routine that it will have to run
		Pthread_create(i, contents.file_one, contents.file_two, hostname, port);
	}		
  // logger(LOG, "DEBUG", "Created Thread Pool", num_threads);
  while(1){

  }

  if(scheduling_flag == 0) {
    pthread_mutex_destroy(&mutex);
    pthread_barrier_destroy(&barrier);
  }
  else {
    sem_destroy(&semaphore);
  }
  return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define VERSION 23
#define BUFSIZE 8096
#define ERROR      42
#define LOG        44
#define FORBIDDEN 403
#define NOTFOUND  404


/************************
 *	     Structs        *
 ************************/

typedef struct thread_t
{
	pthread_t pthread;
	int thread_id;
	int thread_count;
	int html_count;
	int image_count;
} thread_t;

typedef struct connection{
	int port;
	int hit;
	int num_before;
	// statistics
	int arrival_count;
	int arrival_time;
	int dispatch_count;
	int dispatch_time;
	int complete_count;
	int complete_time;
	int num_arrived_before;
	int num_dispatched_before;
	int age;

	char *fileType;
	long readReturn; //read();
	char buffer[BUFSIZE+1]; 
}connection;

typedef struct queue
{
    connection **elements; //Array of connection pointers
    int max_capacity;
    int head;      
    int tail;      
    int count;     
} queue;

struct {
	char *ext;
	char *filetype;
} extensions [] = {
	{"gif", "image/gif" },  
	{"jpg", "image/jpg" }, 
	{"jpeg","image/jpeg"},
	{"png", "image/png" },  
	{"ico", "image/ico" },  
	{"zip", "image/zip" },  
	{"gz",  "image/gz"  },  
	{"tar", "image/tar" },  
	{"htm", "text/html" },  
	{"html","text/html" },  
	{0,0} };



/************************
 *        Methods       *
 ************************/


void logger(int type, char *s1, char *s2, int socket_fd);
void web(connection* con, thread_t* thread);
void * concur_routine(thread_t thread);
void * priority_routine(thread_t thread);
void * routine(void *args);
void Pthread_create(int i);
void Socket(int *listenfd);
void get_arguements(int argc, char **argv, int *num_threads, int *buffers, char **schedalg);
void connect_to_port(char **argv, int *port);
char* read_file_type(char *buffer, int ret);
void Ignore(int ignore);
void set_schedule_policy(char * schedalg);

queue* newqueue(int size);
int count(queue *q);
int isEmpty(queue *q);
int isFull(queue *q);
int areFilled(queue *q1, queue *q2);
int enqueue(queue *q, connection *x);
connection* dequeue(queue *q);
void free_q(queue *q);

int getTime();



/************************
 *		Variables	    * 
 ************************/


int hp_flag; //flag indicating whether the scheduling algorithm is either HPIC or HPHC. 0 if neither, 1 if HPIC, -1 if HPHC.
int requests_counter;
thread_t *thread_pool;
pthread_mutex_t stat_mutex;
pthread_mutex_t mutex;
pthread_cond_t condNotFull;
pthread_cond_t condNotEmpty;
queue *queue_priority;
queue *queue_non_priority;

//Variables to store statistics shared across all threads
// Time in milliseconds denoting the start time of web server creation.
static int start_time; 
struct timeval time_clock;
//The number of requests that arrived before this request arrived.
static int xStatReqArrivalCount = 0;
//The number of requests that completed before this request completed.
static int xStatReqCompletedCount = 0; 
static int xStatReqDispatchCount = 0;



/*****************************************
 * 				   MAIN					 *
 *****************************************/

int main(int argc, char **argv)
{
	int i, port, is_html=0;
	int listenfd, socketfd, hit;
	int num_threads, buffers;
	char *schedalg; //*file_type;
	socklen_t length;
	static struct sockaddr_in cli_addr; /* static = initialised to zeros */
	static struct sockaddr_in serv_addr; /* static = initialised to zeros */

	get_arguements(argc, argv, &num_threads, &buffers, &schedalg); 
	set_schedule_policy(schedalg); 

	/* Become deamon + unstopable and no zombies children (= no wait()) */
	if(fork() != 0)
		return 0; /* parent returns OK to shell */
	(void)signal(SIGCHLD, SIG_IGN); /* ignore child death */
	(void)signal(SIGHUP, SIG_IGN); /* ignore terminal hangups */
	for(i=0;i<32;i++)
		(void)close(i);		/* close open files */
	(void)setpgrp();	
	
	logger(LOG,"nweb starting",argv[1],getpid());
	
	logger(LOG,"Start time"," in millis",start_time);

	// Initialize the thread pool with the available worker threads 
	thread_pool = (thread_t*)malloc(num_threads * sizeof(thread_t));
	
	pthread_mutex_init(&stat_mutex, NULL);
	pthread_mutex_init(&mutex, NULL);
	pthread_cond_init(&condNotFull, NULL);
	pthread_cond_init(&condNotEmpty, NULL);

	// Initialize queue with the allowed size 
	queue_priority = newqueue(buffers);
	if (hp_flag != 0)
	{
		queue_non_priority = newqueue(buffers);	
	}
	
	// Run through the pool
	for (i = 0; i < num_threads; i++)
	{
		Pthread_create(i); // Create the pthread with the routine that it will have to run
	}		

	/* setup the network socket */
	// Safely connect to a socket 
	Socket(&listenfd);

	// Try to connect to the port given 
	connect_to_port(argv, &port);

	// Set the server information
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(port);

	// Connect the server struct to the port 
	if(bind(listenfd, (struct sockaddr *)&serv_addr,sizeof(serv_addr)) <0)
		logger(ERROR,"system call","bind",0);
	
	// Open the port for request connections 
	if(listen(listenfd,64) <0) //64 --> Buffers
		logger(ERROR,"system call","listen",0);
	
	// Allow requests and execute them 
	for(hit=1; /*run*/ ;hit++) {
		
		// Get the size of the client 
		length = sizeof(cli_addr);

		// Socket accepts their request
		if((socketfd = accept(listenfd, (struct sockaddr *)&cli_addr, &length)) < 0) {
			logger(ERROR,"system call","accept",0);
		}
		connection* con;
		con = (connection*)malloc(sizeof(connection));
		
		con->hit = hit;
		con->port = socketfd;
		long ret = read(socketfd, con->buffer, BUFSIZE);
		if(ret == 0 || ret == -1) {	/* read failure stop now */
			logger(FORBIDDEN,"failed to read browser request","",socketfd);
		}
		con->fileType = read_file_type(con->buffer, ret);
		con->readReturn = ret;

		logger(LOG, "ACCEPTED", "FD=", socketfd);

		if (hp_flag == 0) {
			pthread_mutex_lock(&mutex);
			con->arrival_count = xStatReqArrivalCount++;
			con->arrival_time = getTime() - start_time;
			while (isFull(queue_priority))
			{
				pthread_cond_wait(&condNotFull, &mutex);
			}
			logger(LOG,"enqueue","client id", socketfd);
			// con->num_before = count(queue_priority);
			if (enqueue(queue_priority, con) < 0) 
			{
				perror("error adding to the queue.\n");	
			}
			logger(LOG, "WAITING","QUEUE GOT AN ELEMENT OPEN THE CONDITION", 0);
			pthread_cond_signal(&condNotEmpty);
			pthread_mutex_unlock(&mutex);
			
		}
		else {
			pthread_mutex_lock(&mutex);
			con->arrival_count = xStatReqArrivalCount++;
			con->arrival_time = getTime() - start_time;
			//find out what type of file the request is for.
			//based on the policy put it in the proper queue based on the file types priority.
			while (areFilled(queue_priority, queue_non_priority))
			{
				pthread_cond_wait(&condNotFull, &mutex);
			}

			is_html = (strcmp(con->fileType, "text/html") == 0) ? 1 : 0;

			//enque into priority
			if((is_html && hp_flag == -1) || (!is_html && hp_flag == 1)) {
				enqueue(queue_priority, con);
			}
			else { //enque into non-priority queue
				con->num_arrived_before = count(queue_non_priority);
				con->num_dispatched_before = xStatReqDispatchCount;
				enqueue(queue_non_priority, con);
			}
			pthread_cond_signal(&condNotEmpty);
			pthread_mutex_unlock(&mutex);
		}
	}

	pthread_mutex_destroy(&stat_mutex);
	pthread_mutex_destroy(&mutex);
	pthread_cond_destroy(&condNotEmpty);
	pthread_cond_destroy(&condNotFull);

	// Free up queue and thread pool
	free_q(queue_priority);
	if (hp_flag != 0) {
		free_q(queue_non_priority);
	}
	free(thread_pool);
}



/*********************
 *  Main Functions   *
 *********************/

// Parses the command line arguements 
void get_arguements(int argc, char **argv, int *num_threads, int *buffers, char **schedalg){
	int i;
	if( argc < 3  || argc > 6 || !strcmp(argv[1], "-?") ) {
		(void)printf("hint: nweb Port-Number Top-Directory\t\tversion %d\n\n"
	"\tnweb is a small and very safe mini web server\n"
	"\tnweb only servers out file/web pages with extensions named below\n"
	"\t and only from the named directory or its sub-directories.\n"
	"\tThere is no fancy features = safe and secure.\n\n"
	"\tExample: nweb 8181 /home/nwebdir &\n\n"
	"\tOnly Supports:", VERSION);
		for(i=0;extensions[i].ext != 0;i++)
			(void)printf(" %s",extensions[i].ext);

		(void)printf("\n\tNot Supported: URLs including \"..\", Java, Javascript, CGI\n"
	"\tNot Supported: directories / /etc /bin /lib /tmp /usr /dev /sbin \n"
	"\tNo warranty given or implied\n\tNigel Griffiths nag@uk.ibm.com\n"  );
		exit(0);
	}
	if( !strncmp(argv[2],"/"   , 2) || !strncmp(argv[2],"/etc", 5) ||
	    !strncmp(argv[2],"/bin", 5) || !strncmp(argv[2],"/lib", 5) ||
	    !strncmp(argv[2],"/tmp", 5) || !strncmp(argv[2],"/usr", 5) ||
	    !strncmp(argv[2],"/dev", 5) || !strncmp(argv[2],"/sbin", 6) ){
		(void)printf("ERROR: Bad top directory %s, see nweb -?\n",argv[2]);
		exit(3);
	}
	if(chdir(argv[2]) == -1){ 
		(void)printf("ERROR: Can't Change to directory %s\n",argv[2]);
		exit(4);
	}
	// Get number of threads available 
	*num_threads = atoi(argv[3]);
	if (*num_threads < 0)
	{
		(void)printf("ERROR: Number of woker threads must be a positive integr\n");
		exit(5);
	}
	//get the amount of request connections allowed 
	*buffers = atoi(argv[4]);
	if (*buffers < 0)
	{
		(void)printf("ERROR: Number of request connections must be a positive integr\n");
		exit(6);
	}
	// Get the schedule policy
	*schedalg = argv[5];

	//check that the schedule algorithm is valid
	if( !strcmp(*schedalg,"ANY") && !strcmp(*schedalg,"FIFO") && !strcmp(*schedalg,"HPIC") && !strcmp(*schedalg,"HPHC")) {
		(void)printf("ERROR: Invalid scheduling algorithm: %s, see nweb -?\n",*schedalg);
		exit(7);
	}
	
}

void connect_to_port(char **argv, int *port){
	*port = atoi(argv[1]);
	if(*port < 0 || *port >60000)
	{
		logger(ERROR,"Invalid port number (try 1->60000)",argv[1],0);
	}
}

char* read_file_type(char *buffer, int ret) {
	int j, buflen;
	long i, len;
	char *fstr;

	for(i=0;i<ret;i++)	/* remove CF and LF characters */
		if(buffer[i] == '\r' || buffer[i] == '\n')
			buffer[i]='*';
	for(i=4;i<BUFSIZE;i++) { /* null terminate after the second space to ignore extra stuff */
		if(buffer[i] == ' ') { /* string is "GET URL " +lots of other stuff */
			buffer[i] = 0;
			break;
		}
	}
	for(j=0;j<i-1;j++) 	/* check for illegal parent directory use .. */
		if(buffer[j] == '.' && buffer[j+1] == '.') {
			logger(FORBIDDEN,"Parent directory (..) path names not supported",buffer,ret);
		}
	if( !strncmp(&buffer[0],"GET /\0",6) || !strncmp(&buffer[0],"get /\0",6) ) /* convert no filename to index file */
		(void)strcpy(buffer,"GET /index.html");

	/* work out the file type and check we support it */
	buflen=strlen(buffer);
	fstr = (char *)0;
	for(i=0;extensions[i].ext != 0;i++) {
		len = strlen(extensions[i].ext);
		if( !strncmp(&buffer[buflen-len], extensions[i].ext, len)) {
			fstr = extensions[i].filetype;
			break;
		}
	}
	if(fstr == 0) logger(FORBIDDEN,"file extension type not supported",buffer,ret);
	return fstr;
}

void set_schedule_policy(char * schedalg){
	if (strcmp(schedalg, "HPIC") == 0){
		hp_flag = 1; // hp_flag set to 1 if Scheduling Policy is: Highest Priority to Image Content
	} 
	else if (strcmp(schedalg, "HPHC") == 0) {
		hp_flag = -1; // hp_flag set to 1 if Scheduling Policy is: Highest Priority to HTML Content
	} 
	else {
		hp_flag = 0; // hp_flag set to 0 if Scheduling Policy is ANY or FIFO
	}
}

/*************************
 *   Helper Functions 	 *
 *************************/

void Pthread_create(int i){
	int *id = (int *)malloc(sizeof(int));
	*id = i;
	thread_t th = thread_pool[i];
	th.thread_id = i;
	if(pthread_create(&th.pthread, NULL, &routine, (void *) id) != 0)
	{
		perror("Error: failed to create thread.\n");		
	}
	logger(LOG,"Thread Create","thread id",th.thread_id);
}

void Socket(int *listenfd)
{
	if((*listenfd = socket(AF_INET, SOCK_STREAM,0)) <0)
	{
		logger(ERROR, "system call","socket",0);
	}
}

int getTime()
{
	gettimeofday(&time_clock, NULL);
	return time_clock.tv_usec;
}

/*************************
 *   Thread Functions 	 *
 *************************/

void * concur_routine(thread_t thread) {
	connection* client_addr;
	thread.thread_count = 0;
	thread.html_count = 0;
	thread.image_count = 0;
	int threadID = thread.thread_id;

	while (1) // thread keep on working never stop working
	{
		pthread_mutex_lock(&mutex);
		while (isEmpty(queue_priority)) { 
			logger(LOG, "WAITING", "QUEUE IS EMPTY: ", threadID);
			pthread_cond_wait(&condNotEmpty, &mutex);
		}
		client_addr = dequeue(queue_priority);
		
		if (client_addr == NULL)
		{
			perror("error dequeing.\n");
		}
		client_addr->dispatch_count = xStatReqDispatchCount++;
		client_addr->dispatch_time =  getTime() - start_time; 
		logger(LOG,"dequeue", "client_addr port", client_addr->port);
		logger(LOG,"ThreadID", " ", threadID);
		thread.thread_count++;
		logger(LOG,"Thread", "count", thread.thread_count);
		web(client_addr, &thread);
		pthread_cond_signal(&condNotFull);
		pthread_mutex_unlock(&mutex); 
	}
}

void * priority_routine(thread_t thread) {
	connection* client_addr;
	thread.thread_count = 0;
	thread.html_count = 0;
	thread.image_count = 0;
	int threadID = thread.thread_id;
	int temp;
	while (1) // thread keep on working never stop working
	{	
		pthread_mutex_lock(&mutex);
		
		while (isEmpty(queue_priority) && isEmpty(queue_non_priority)) {
			pthread_cond_wait(&condNotEmpty, &mutex);
		}
	
		if (!isEmpty(queue_priority)) {
			client_addr = dequeue(queue_priority);
			client_addr->age = 0;
		}	
		else { 
			client_addr = dequeue(queue_non_priority);
			temp = xStatReqDispatchCount;
			client_addr->age = temp - (client_addr->num_arrived_before - client_addr->num_dispatched_before);
		}
		
		if (client_addr == NULL)
		{
			perror("error dequeing.\n");
		}
		client_addr->dispatch_count = xStatReqDispatchCount++;
		client_addr->dispatch_time =  getTime() - start_time; 

		logger(LOG,"dequeue", "client_addr port", client_addr->port);
		logger(LOG,"ThreadID", " ", threadID);
		thread.thread_count++;
		logger(LOG,"Thread", "count", thread.thread_count);
		web(client_addr,&thread);
		pthread_cond_signal(&condNotFull);
		pthread_mutex_unlock(&mutex); 
	}
}

void * routine(void *args) {
	int i = *(int *)args;
	thread_t thread = thread_pool[i];
	thread.thread_id = i;
	if (hp_flag == 0) { // if there is no HP policy, and the start concur routine	
		concur_routine(thread);
	}
	else { // start priority routine
		priority_routine(thread);
	}
	return NULL;
}




/*******************
 *   Queue Class   *
 *******************/ 

// initialize a queue and return a pointer to the queue
queue* newqueue(int size)
{
    queue *q;
    q = (queue*)malloc(sizeof(queue));
    q->elements = (connection**)malloc(size * sizeof(connection*));
    q->max_capacity = size;
    q->head = 0;
    q->tail = -1;
    q->count = 0;
    return q;
}
 
// returns the count of the queue
int count(queue *q) 
{
	if(q == NULL)
	{
		return 0;
	}
    return q->count;
}
 
// checks if queue is empty
int isEmpty(queue *q) 
{
    return !count(q);
}

int isFull(queue *q) 
{
	return q->count == q->max_capacity; 
}

int areFilled(queue *q1, queue *q2)
{
	return (q1->count + q2->count) == q1->max_capacity; 
}
 
int enqueue(queue *q, connection *x)
{
    if (count(q) == q->max_capacity)
    {
        return -1; // error unable to enqueue: queue is full
    } 
    q->tail++;
	q->tail = q->tail % q->max_capacity;    // wrap-around circular array
    q->elements[q->tail] = x;
    q->count++;
    return 0;
}
 
connection* dequeue(queue *q)
{
    int head;
    
    if (isEmpty(q))
    {
        return NULL; // error unable to dequeue: queue is empty
    }
    head = q->head; 
    q->head = (q->head + 1) % q->max_capacity;  // wrap-around circular array
    q->count--;
    return q->elements[head];
}

void free_q(queue *q)
{
	free(q->elements);
	free(q);  
}

void Ignore(int ignore){
	return;
}



/******************
 *     Logger     *
 ******************/

void logger(int type, char *s1, char *s2, int socket_fd)
{
	int fd, ignore;
	char logbuffer[BUFSIZE*2];

	switch (type) {
	case ERROR: (void)sprintf(logbuffer,"ERROR: %s:%s Errno=%d exiting pid=%d",s1, s2, errno,getpid()); 
		break;
	case FORBIDDEN: 
		ignore = write(socket_fd, "HTTP/1.1 403 Forbidden\nContent-Length: 185\nConnection: close\nContent-Type: text/html\n\n<html><head>\n<title>403 Forbidden</title>\n</head><body>\n<h1>Forbidden</h1>\nThe requested URL, file type or operation is not allowed on this simple static file webserver.\n</body></html>\n",271);
		(void)sprintf(logbuffer,"FORBIDDEN: %s:%s",s1, s2); 
		break;
	case NOTFOUND: 
		ignore = write(socket_fd, "HTTP/1.1 404 Not Found\nContent-Length: 136\nConnection: close\nContent-Type: text/html\n\n<html><head>\n<title>404 Not Found</title>\n</head><body>\n<h1>Not Found</h1>\nThe requested URL was not found on this server.\n</body></html>\n",224);
		(void)sprintf(logbuffer,"NOT FOUND: %s:%s",s1, s2); 
		break;
	case LOG: (void)sprintf(logbuffer," INFO: %s:%s:%d",s1, s2,socket_fd); break;
	}	
	/* No checks here, nothing can be done with a failure anyway */
	if((fd = open("nweb.log", O_CREAT| O_WRONLY | O_APPEND,0644)) >= 0) {
		ignore = write(fd,logbuffer,strlen(logbuffer)); 
		ignore = write(fd,"\n",1);      
		(void)close(fd);
	}
	Ignore(ignore);
	if(type == ERROR || type == NOTFOUND || type == FORBIDDEN) exit(3);
}


/******************
 *      Web       *
 ******************/

/* this is a child web server process, so we can exit on errors */
void web(connection* con, thread_t* thread)
{
	int j, file_fd, buflen, ignore;
	long i, ret, len;
	char * fstr;
	int fd = con->port;
	char* buffer = con->buffer;
	int hit = con->hit;
	ret = con->readReturn;
	if(ret > 0 && ret < BUFSIZE)	/* return code is valid chars */
		buffer[ret]=0;		/* terminate the buffer */
	else buffer[0]=0;
	for(i=0;i<ret;i++)	/* remove CF and LF characters */
		if(buffer[i] == '\r' || buffer[i] == '\n')
			buffer[i]='*';
	
	logger(LOG,"request",buffer,hit);
	
	if( strncmp(buffer,"GET ",4) && strncmp(buffer,"get ",4) ) {
		logger(FORBIDDEN,"Only simple GET operation supported",buffer,fd);
	}
	for(i=4;i<BUFSIZE;i++) { /* null terminate after the second space to ignore extra stuff */
		if(buffer[i] == ' ') { /* string is "GET URL " +lots of other stuff */
			buffer[i] = 0;
			break;
		}
	}
	for(j=0;j<i-1;j++) 	/* check for illegal parent directory use .. */
		if(buffer[j] == '.' && buffer[j+1] == '.') {
			logger(FORBIDDEN,"Parent directory (..) path names not supported",buffer,fd);
		}
	if( !strncmp(&buffer[0],"GET /\0",6) || !strncmp(&buffer[0],"get /\0",6) ) /* convert no filename to index file */
		(void)strcpy(buffer,"GET /index.html");

	/* work out the file type and check we support it */
	buflen=strlen(buffer);
	fstr = (char *)0;
	for(i=0;extensions[i].ext != 0;i++) {
		len = strlen(extensions[i].ext);
		if( !strncmp(&buffer[buflen-len], extensions[i].ext, len)) {
			fstr = extensions[i].filetype;
			break;
		}
	}
	if(fstr == 0) logger(FORBIDDEN,"file extension type not supported",buffer,fd);

	if (strcmp(fstr, "text/html") == 0)
	{
		thread->html_count++;
	}
	else
	{
		thread->image_count++;
	}
	
	if(( file_fd = open(&buffer[5],O_RDONLY)) == -1) {  /* open the file for reading */
		logger(NOTFOUND, "failed to open file",&buffer[5],fd);
	}
	logger(LOG,"SEND",&buffer[5],hit);
	len = (long)lseek(file_fd, (off_t)0, SEEK_END); /* lseek to the file end to find the length */
	      (void)lseek(file_fd, (off_t)0, SEEK_SET); /* lseek back to the file start ready for reading */
          (void)sprintf(buffer,"HTTP/1.1 200 OK\nServer: nweb/%d.0\nContent-Length: %ld\nConnection: close\nContent-Type: %s\n\n", VERSION, len, fstr); /* Header + a blank line */
	logger(LOG,"Header",buffer,hit);
	ignore = write(fd,buffer,strlen(buffer));
	
    /* Send the statistical headers described in the paper, example below */
	con->complete_count = xStatReqCompletedCount++;
	con->complete_time =  getTime() - start_time; 	

    ignore = sprintf(buffer,"X-stat-req-arrival-count: %d\r\n", con->arrival_count);
	ignore = write(fd,buffer,strlen(buffer));

	ignore = sprintf(buffer,"X-stat-req-arrival-time: %d\r\n", con->arrival_time);
	ignore = write(fd,buffer,strlen(buffer));
	
    ignore = sprintf(buffer,"X-stat-req-dispatch-count: %d\r\n", con->dispatch_count);
	ignore = write(fd,buffer,strlen(buffer));

	ignore = sprintf(buffer,"X-stat-req-dispatch-time: %d\r\n", con->dispatch_time);
	ignore = write(fd,buffer,strlen(buffer));

	ignore = sprintf(buffer,"X-stat-req-complete-count: %d\r\n", con->complete_count);
	ignore = write(fd,buffer,strlen(buffer));

	ignore = sprintf(buffer,"X-stat-req-complete-time: %d\r\n", con->complete_time);
	ignore = write(fd,buffer,strlen(buffer));

	ignore = sprintf(buffer,"X-stat-req-age: %d\r\n", con->age);
	ignore = write(fd,buffer,strlen(buffer));

	/******************
 	 *  Thread-Stats  *
 	 ******************/

	ignore = sprintf(buffer,"X-stat-thread-id: %d\r\n", thread->thread_id);
	ignore = write(fd,buffer,strlen(buffer));

	ignore = sprintf(buffer,"X-stat-thread-count: %d\r\n", thread->thread_count);
	ignore = write(fd,buffer,strlen(buffer));

	ignore = sprintf(buffer,"X-stat-thread-html: %d\r\n", thread->html_count);
	ignore = write(fd,buffer,strlen(buffer));

	ignore = sprintf(buffer,"X-stat-thread-image: %d\r\n", thread->image_count);
	ignore = write(fd,buffer,strlen(buffer));

    /* send file in 8KB block - last block may be smaller */
	while (	(ret = read(file_fd, buffer, BUFSIZE)) > 0 ) {
		ignore = write(fd,buffer,ret);
	}
	Ignore(ignore);
	logger(LOG, "CLOSING", "CLIENT FILE", fd);
	sleep(1);	/* allow socket to drain before signalling the socket is closed */
	close(fd);
}



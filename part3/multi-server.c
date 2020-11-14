/*
 * multi-server.c
 */

#include <stdio.h>      /* for printf() and fprintf() */
#include <sys/socket.h> /* for socket(), bind(), and connect() */
#include <arpa/inet.h>  /* for sockaddr_in and inet_ntoa() */
#include <stdlib.h>     /* for atoi() and exit() */
#include <string.h>     /* for memset() */
#include <unistd.h>     /* for close() */
#include <time.h>       /* for time() */
#include <netdb.h>      /* for gethostbyname() */
#include <signal.h>     /* for signal() */
#include <sys/stat.h>   /* for stat() */
#include <semaphore.h> 
#include <sys/wait.h>   /* for waitpid() */
#include <sys/mman.h>   /* for mmap() */


#define MAXPENDING 5    /* Maximum outstanding connection requests */

#define DISK_IO_BUF_SIZE 4096

struct stats{
	sem_t semp;
	unsigned int totalreq;
	unsigned int req_2xx;
	unsigned int req_3xx;
	unsigned int req_4xx;
	unsigned int req_5xx;
};

static struct stats *Statistics = NULL;

static void die(const char *message)
{
    perror(message);
    exit(1); 
}

/*
 * Create a listening socket bound to the given port.
 */
static int createServerSocket(unsigned short port)
{
    int servSock;
    struct sockaddr_in servAddr;

    /* Create socket for incoming connections */
    if ((servSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
        die("socket() failed");
      
    /* Construct local address structure */
    memset(&servAddr, 0, sizeof(servAddr));       /* Zero out structure */
    servAddr.sin_family = AF_INET;                /* Internet address family */
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY); /* Any incoming interface */
    servAddr.sin_port = htons(port);              /* Local port */

    /* Bind to the local address */
    if (bind(servSock, (struct sockaddr *)&servAddr, sizeof(servAddr)) < 0)
        die("bind() failed");

    /* Mark the socket so it will listen for incoming connections */
    if (listen(servSock, MAXPENDING) < 0)
        die("listen() failed");

    return servSock;
}

/*
 * A wrapper around send() that does error checking and logging.
 * Returns -1 on failure.
 * 
 * This function assumes that buf is a null-terminated string, so
 * don't use this function to send binary data.
 */
ssize_t Send(int sock, const char *buf)
{
    size_t len = strlen(buf);
    ssize_t res = send(sock, buf, len, 0);
    if (res != len) {
        perror("send() failed");
        return -1;
    }
    else 
        return res;
}

/*
 * HTTP/1.0 status codes and the corresponding reason phrases.
 */

static struct {
    int status;
    char *reason;
} HTTP_StatusCodes[] = {
    { 200, "OK" },
    { 201, "Created" },
    { 202, "Accepted" },
    { 204, "No Content" },
    { 301, "Moved Permanently" },
    { 302, "Moved Temporarily" },
    { 304, "Not Modified" },
    { 400, "Bad Request" },
    { 401, "Unauthorized" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 500, "Internal Server Error" },
    { 501, "Not Implemented" },
    { 502, "Bad Gateway" },
    { 503, "Service Unavailable" },
    { 0, NULL } // marks the end of the list
};

static inline const char *getReasonPhrase(int statusCode)
{
    int i = 0;
    while (HTTP_StatusCodes[i].status > 0) {
        if (HTTP_StatusCodes[i].status == statusCode)
            return HTTP_StatusCodes[i].reason;
        i++;
    }
    return "Unknown Status Code";
}


/*
 * Send HTTP status line followed by a blank line.
 */
static void sendStatusLine(int clntSock, int statusCode)
{
    char buf[1000];
    const char *reasonPhrase = getReasonPhrase(statusCode);

    // print the status line into the buffer
    sprintf(buf, "HTTP/1.0 %d ", statusCode);
    strcat(buf, reasonPhrase);
    strcat(buf, "\r\n");

    // We don't send any HTTP header in this simple server.
    // We need to send a blank line to signal the end of headers.
    strcat(buf, "\r\n");

    // For non-200 status, format the status line as an HTML content
    // so that browers can display it.
    if (statusCode != 200) {
        char body[1000];
        sprintf(body, 
                "<html><body>\n"
                "<h1>%d %s</h1>\n"
                "</body></html>\n",
                statusCode, reasonPhrase);
        strcat(buf, body);
    }

    // lock the semaphore
    sem_wait(&Statistics->semp);
    
    // update Statistics
    Statistics->totalreq++;
    if(statusCode >= 200 && statusCode < 300){
	    Statistics->req_2xx++;
    }else if(statusCode >= 300 && statusCode < 400){
	    Statistics->req_3xx++;
    }else if(statusCode >= 400 && statusCode < 500){
	    Statistics->req_4xx++;
    }else if(statusCode >= 500 && statusCode < 600){
	    Statistics->req_5xx++;
    }
    
    // unlock the semaphore
    sem_post(&Statistics->semp);

    // send the buffer to the browser
    Send(clntSock, buf);
}

// Handle the Statistics requests
static int
handleStatRequest(int clntSock){
	static char buf[4096];

	sendStatusLine(clntSock, 200);

	sem_wait(&Statistics->semp);

	snprintf(buf, sizeof(buf), "<html><body>\n"
			"<h1>Server Statistics</h1>\n"
			"<tr><td>total requests: </td><td>%d</td></tr>\n"
			"<tr><td>2xx requests: </td><td>%d</td></tr>\n"
			"<tr><td>3xx requests: </td><td>%d</td></tr>\n"
			"<tr><td>4xx requests: </td><td>%d</td></tr>\n"
			"<tr><td>5xx requests: </td><td>%d</td></tr>\n"
			"</body></html>\n",/*" the total request: %d",*/
			Statistics->totalreq, Statistics->req_2xx, Statistics->req_3xx, Statistics->req_4xx, Statistics->req_5xx);
	sem_post(&Statistics->semp);

	Send(clntSock, buf);
	return 200;
}

// Handle the directory requests
static int
handleDirRequest(char *file, int clntSock){
	pid_t pid;
	int statusCode, dirpipe[2];
	char *buf[512];
	ssize_t mesize;

	// create a pipe
	// pipe returns 0 if success
	if(pipe(dirpipe) != 0)
		die("pipe error\n");

	// create a child process
	pid = fork();

	if(pid == -1){
		die("fork error\n");
	}else if(pid > 0){
		statusCode = 200;
		sendStatusLine(clntSock, statusCode);
		// we are now in the parent process
		// we don't need to write into the pipe, only read from it
		// close the write end
		close(dirpipe[1]);
		do{
			mesize = read(dirpipe[0], buf, sizeof(buf)); 
			if(mesize > 0){
				if(send(clntSock, buf, mesize, 0) != mesize){
					perror("send message error\n");
					break;
				}
			}
		}while(mesize>0);
		
		// we are done reading
		close(dirpipe[0]);

		return statusCode;
	}

	// we are now in the child process
	// close the read end and standard in, out, error file descriptors
	close(dirpipe[0]);
	close(STDIN_FILENO);
	close(clntSock);

	// redirect the standard output and error to the pipe
	if(dup2(dirpipe[1], STDOUT_FILENO) != STDOUT_FILENO)
		die("standard output redirection failed\n");
	if(dup2(dirpipe[1], STDERR_FILENO) != STDERR_FILENO)
		die("standard error redirection failed\n");

	close(dirpipe[1]);
	
	char *argv[] = {"ls", "-a", file};
	if(execv("/bin/ls", argv) == -1){
		perror("ls execution error\n");
	}
	return 0;
}

/*
 * Handle static file requests.
 * Returns the HTTP status code that was sent to the browser.
 */
static int handleFileRequest(
        const char *webRoot, const char *requestURI, int clntSock)
{
    int statusCode;
    FILE *fp = NULL;

    // Compose the file path from webRoot and requestURI.
    // If requestURI ends with '/', append "index.html".
    
    char *file = (char *)malloc(strlen(webRoot) + strlen(requestURI) + 100);
    if (file == NULL)
        die("malloc failed");
    strcpy(file, webRoot);
    strcat(file, requestURI);
    if (file[strlen(file)-1] == '/') {
        strcat(file, "index.html");
    }

    // See if the requested file is a directory.
    // Our server does not support directory listing.

    struct stat st;
    if (stat(file, &st) == 0 && S_ISDIR(st.st_mode)) {
        //statusCode = 403; // "Forbidden"
        //sendStatusLine(clntSock, statusCode);
	statusCode = handleDirRequest(file, clntSock);
        goto func_end;
    }

    // If unable to open the file, send "404 Not Found".

    fp = fopen(file, "rb");
    if (fp == NULL) {
        statusCode = 404; // "Not Found"
        sendStatusLine(clntSock, statusCode);
        goto func_end;
    }

    // Otherwise, send "200 OK" followed by the file content.

    statusCode = 200; // "OK"
    sendStatusLine(clntSock, statusCode);

    // send the file 
    size_t n;
    char buf[DISK_IO_BUF_SIZE];
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (send(clntSock, buf, n, 0) != n) {
            // send() failed.
            // We log the failure, break out of the loop,
            // and let the server continue on with the next request.
            perror("\nsend() failed");
            break;
        }
    }
    // fread() returns 0 both on EOF and on error.
    // Let's check if there was an error.
    if (ferror(fp))
        perror("fread failed");

func_end:

    // clean up
    free(file);
    if (fp)
        fclose(fp);

    return statusCode;
}

static void
chldHandler(int signo){
	waitpid(-1, NULL, WNOHANG);
}



int main(int argc, char *argv[])
{
    // Ignore SIGPIPE so that we don't terminate when we call
    // send() on a disconnected socket.
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        die("signal() failed\n");

    if (signal(SIGCHLD, chldHandler) == SIG_ERR)
	    die("chldHandler failed\n");

    if (argc != 3) {
        fprintf(stderr, "usage: %s <server_port> <web_root>\n", argv[0]);
        exit(1);
    }

    unsigned short servPort = atoi(argv[1]);
    const char *webRoot = argv[2];
    fprintf(stderr, "webRoot: %s\n", webRoot);

    int servSock = createServerSocket(servPort);

    char line[1000];
    char requestLine[1000];
    int statusCode;
    struct sockaddr_in clntAddr;
    pid_t pid;

    // anonymous memory mapping
    if((Statistics = mmap(0, sizeof(struct stats), PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0)) == MAP_FAILED)
	    die("anonymous memory mapping failed\n");
    // allocate a memory segment to Statistics
    //if((Statistics = malloc(sizeof(struct stats))) == NULL)
	    //die("memory allocation to Statistics failed!\n");

    memset(Statistics, 0, sizeof(struct stats));

    // create an unnamed semaphore
    if(sem_init(&Statistics->semp, 1, 1) != 0)
	    die(" semaphore initialization failed \n");

    for (;;) {

        /*
         * wait for a client to connect
         */
	
	fprintf(stderr, "the total requests is : %d\n", Statistics->totalreq);

        // initialize the in-out parameter
        unsigned int clntLen = sizeof(clntAddr); 
        int clntSock = accept(servSock, (struct sockaddr *)&clntAddr, &clntLen);
        if (clntSock < 0)
            die("accept() failed");
	
	// create a new process - child process
	pid = fork();
	if (pid == -1){
		die("fork failed\n");
	}else if(pid > 0){
		close(clntSock);
		continue;
	}

	close(servSock);


        FILE *clntFp = fdopen(clntSock, "r");
        if (clntFp == NULL)
            die("fdopen failed");

        /*
         * Let's parse the request line.
         */

        char *method      = "";
        char *requestURI  = "";
        char *httpVersion = "";

        if (fgets(requestLine, sizeof(requestLine), clntFp) == NULL) {
            // socket closed - there isn't much we can do
            statusCode = 400; // "Bad Request"
            goto loop_end;
        }

        char *token_separators = "\t \r\n"; // tab, space, new line
        method = strtok(requestLine, token_separators);
        requestURI = strtok(NULL, token_separators);
	fprintf(stderr, "requestURI: %s\n", requestURI);
        httpVersion = strtok(NULL, token_separators);
        char *extraThingsOnRequestLine = strtok(NULL, token_separators);

        // check if we have 3 (and only 3) things in the request line
        if (!method || !requestURI || !httpVersion || 
                extraThingsOnRequestLine) {
            statusCode = 501; // "Not Implemented"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }

        // we only support GET method 
        if (strcmp(method, "GET") != 0) {
            statusCode = 501; // "Not Implemented"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }

        // we only support HTTP/1.0 and HTTP/1.1
        if (strcmp(httpVersion, "HTTP/1.0") != 0 && 
            strcmp(httpVersion, "HTTP/1.1") != 0) {
            statusCode = 501; // "Not Implemented"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }
        
        // requestURI must begin with "/"
        if (!requestURI || *requestURI != '/') {
            statusCode = 400; // "Bad Request"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }

        // make sure that the requestURI does not contain "/../" and 
        // does not end with "/..", which would be a big security hole!
        int len = strlen(requestURI);
        if (len >= 3) {
            char *tail = requestURI + (len - 3);
            if (strcmp(tail, "/..") == 0 || 
                    strstr(requestURI, "/../") != NULL)
            {
                statusCode = 400; // "Bad Request"
                sendStatusLine(clntSock, statusCode);
                goto loop_end;
            }
        }

        /*
         * Now let's skip all headers.
         */

        while (1) {
            if (fgets(line, sizeof(line), clntFp) == NULL) {
                // socket closed prematurely - there isn't much we can do
                statusCode = 400; // "Bad Request"
                goto loop_end;
            }
            if (strcmp("\r\n", line) == 0 || strcmp("\n", line) == 0) {
                // This marks the end of headers.  
                // Break out of the while loop.
                break;
            }
        }

        /*
         * At this point, we have a well-formed HTTP GET request.
         * Let's handle it.
         */
	if (strcmp(requestURI,"/Statistics") == 0){
		statusCode = handleStatRequest(clntSock);
	}
	else{
        statusCode = handleFileRequest(webRoot, requestURI, clntSock);
	}

loop_end:

        /*
         * Done with client request.
         * Log it, close the client socket, and go back to accepting
         * connection.
         */
        
        fprintf(stderr, "%s \"%s %s %s\" %d %s\n",
                inet_ntoa(clntAddr.sin_addr),
                method,
                requestURI,
                httpVersion,
                statusCode,
                getReasonPhrase(statusCode));

        // close the client socket 
        fclose(clntFp);

	//fprintf(stderr, " the total request: %d \n", Statistics->totalreq);

	exit(1);

    } // for (;;)

    sem_destroy(&Statistics->semp);
    munmap(Statistics, sizeof(struct stats));

    return 0;
}


#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <netdb.h>

////////////////////// Constants and Struct Declarations ///////////////////////////////

#define MAX_REQUEST_LEN (5000)
#define MAX_CONCURRENT_REQ (200)
#define MAX_BACK_LOG (30)
#define MAX_CONCURRENT_CONNECTIONS (5)
#define MAX_CONTENT_LEN (2000000)
#define REC_BUFFER_SIZE (4096)
#define BYTES_PER_MB (1000000)
#define WEBSERVER_PORT "http"
#define GET_REQUEST_FIELD "GET"
#define HOST_FIELD "Host: "

typedef struct cache_element {
	struct cache_element* next;
	struct cache_element* prev;
	int numResponseBytes;
	char* response;
	char* request;
} cache_element;

typedef struct cache {
	cache_element* MR;
	cache_element* LR;
	int bytes;
	int max_bytes;
} cache;


/////////////////////////// Global State ///////////////////////////////////


cache* LRU_cache;
int browserProxySock;
int numActiveThreads;
pthread_mutex_t count_mutex;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;


/////////////////////////// Cache Functions ///////////////////////////////////



void initializeCache(int port, int cache_size) {
	LRU_cache = (cache*) malloc(sizeof(cache));
	LRU_cache->max_bytes = cache_size * BYTES_PER_MB;
	LRU_cache->LR = LRU_cache->MR = NULL;
	LRU_cache->bytes = 0;
}

cache_element* scanCacheForElement(char* request) {
	cache_element* temp;
	temp = LRU_cache->MR;
	while (temp != NULL) {
		if (strcmp(request, temp->request) == 0) {
			return temp;
		}
		temp = temp->next;
	}
	return NULL;
}


void removeElementFromCache(cache_element* el) {
	if (el->prev) {
		(el->prev)->next = el->next;
	}
	if (el->next) {
		(el->next)->prev = el->prev;
	}
	LRU_cache->bytes -= el->numResponseBytes;

	if (el == LRU_cache->MR) {
		LRU_cache->MR = el->next;
	}
	if (el == LRU_cache->LR) {
		LRU_cache->LR = el->prev;
	}
}

void removeAndFreeLRElementFromCache() {
	cache_element* el = LRU_cache->LR;
	removeElementFromCache(el);
	free(el->request);
	free(el->response);
	free(el);
}

void addElementToCache(cache_element* MRelement) {
	if (MRelement->numResponseBytes > LRU_cache->max_bytes) {
		return;
	}

	while (LRU_cache->bytes + MRelement->numResponseBytes > LRU_cache->max_bytes) {
		removeAndFreeLRElementFromCache();
	}

	MRelement->prev = NULL;

	if (LRU_cache->LR == NULL) {
		LRU_cache->LR = LRU_cache->MR = MRelement;
	} else {
		MRelement->next = LRU_cache->MR;
		(LRU_cache->MR)->prev = MRelement;
		LRU_cache->MR = MRelement;
	}
	LRU_cache->bytes += MRelement->numResponseBytes;
}

void updateCacheMR(cache_element* MRelement) {
	removeElementFromCache(MRelement);
	addElementToCache(MRelement);
}


cache_element* buildCacheElement(char* parsedRequest, int response_bytes,
		char* response) {
	cache_element* el;
	el = (cache_element*) malloc(sizeof(cache_element));
	memset(el, 0, sizeof(cache_element));

	el->request = (char*) malloc(strlen(parsedRequest) + 1);
	memcpy(el->request, parsedRequest, strlen(parsedRequest) + 1);

	el->response = (char*) malloc(response_bytes);
	memcpy(el->response, response, response_bytes);

	el->numResponseBytes = response_bytes;
	el->next = NULL;
	el->prev = NULL;
	return el;
}

/////////////////////////// Helper Functions ///////////////////////////////////


int timeoutOccurred() {
	return errno == EAGAIN || errno == EWOULDBLOCK;
}


int isPostRequest(char* request) {
	return strstr(request, "GET") != request;
}

char* parseFieldFromRequest(char* request, char* field) {
	int offset = (field == GET_REQUEST_FIELD) ? 0 : strlen(field);
	char* requestStart = strstr(request, field) + offset;
	char* requestEnd = strstr(requestStart, "\r");
	int parsedRequestLen = requestEnd - requestStart;
	char* parsedRequest = (char*) malloc(parsedRequestLen + 1);
	memcpy(parsedRequest, requestStart, parsedRequestLen);
	parsedRequest[parsedRequestLen] = '\0';
	return parsedRequest;
}

void sig_handler(int sig) {
	if (browserProxySock) {
		close(browserProxySock);
		printf("Socket was closed.\n");
	}
	exit(0);
}

void sendContentToClient(int sock, cache_element* element) {
	send(sock, element->response, (size_t) element->numResponseBytes, 0);
}


/////////////////////////// Proxy Functionality ///////////////////////////////////


cache_element* readFromSocketAndBuildCacheElement(int outgoing_sock,
		char* request, char* parsedRequest) {

	struct timeval timeout;
	timeout.tv_sec = 1;
	timeout.tv_usec = 0;

	if (setsockopt(outgoing_sock, SOL_SOCKET, SO_RCVTIMEO, (char *) &timeout,
			sizeof(timeout)) < 0) {
		error("setsockopt failed\n");
	}

	int totalBytesRead = 0;
	int recv_len = 0;

	char* receivedContent;
	receivedContent = (char*) malloc(MAX_CONTENT_LEN);
	memset(receivedContent, 0, MAX_CONTENT_LEN);

	while(1) {
		recv_len = read(outgoing_sock, receivedContent + totalBytesRead,
				MAX_CONTENT_LEN - totalBytesRead);

		if (recv_len <= 0) {
			if ((timeoutOccurred() || recv_len==0) && totalBytesRead > 0) {
				break;
			}
			free(receivedContent);
			return NULL;
		}

		totalBytesRead += recv_len;
	}

	cache_element* newElement = buildCacheElement(parsedRequest, totalBytesRead,
			receivedContent);
	free(receivedContent);
	return newElement;
}

cache_element* makeRequestAndBuildCacheElement(char* request,
		char* parsedRequest, int outgoing_sock) {

	//connection is correct so now send request to web server
	if (send(outgoing_sock, request, strlen(request), 0) < 0) {
		perror("Send error");
		printf("Failed Request: \n%s\n", request);
		return NULL;
	}

	return readFromSocketAndBuildCacheElement(outgoing_sock, request,
			parsedRequest);
}

int handleRequest(int browser_sock, int server_sock, char* rawRequest_buf) {

	char* parsedRequest = parseFieldFromRequest(rawRequest_buf,
			GET_REQUEST_FIELD);

	pthread_mutex_lock(&count_mutex);
	cache_element* cache_element = scanCacheForElement(parsedRequest);
	pthread_mutex_unlock(&count_mutex);

	if (cache_element) {
		printf("Cache HIT... Cache size: %d\n", LRU_cache->bytes);
		pthread_mutex_lock(&count_mutex);
		updateCacheMR(cache_element);
		pthread_mutex_unlock(&count_mutex);
	} else {
		cache_element = makeRequestAndBuildCacheElement(rawRequest_buf,
				parsedRequest, server_sock);
		if (!cache_element) {
			free(parsedRequest);
			return -1;
		}
		printf("Cache MISS... Cache size: %d\n", LRU_cache->bytes);
		pthread_mutex_lock(&count_mutex);
		addElementToCache(cache_element);
		pthread_mutex_unlock(&count_mutex);
	}

	sendContentToClient(browser_sock, cache_element);
	free(parsedRequest);
	return 0;
}

void threadExit() {
	pthread_mutex_lock(&mutex);
	numActiveThreads--;
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&mutex);
}

void handlePersistentConnection(void* browser_accepted_sock) {

	pthread_mutex_lock(&mutex);
	while(numActiveThreads > MAX_CONCURRENT_CONNECTIONS) {
		pthread_cond_wait(&cond, &mutex);
	}
	pthread_mutex_unlock(&mutex);

	char* rawRequest_buf;
	int recv_len, server_sock, browser_sock;
	int firstRequestReceived = 0;

	browser_sock = (int) browser_accepted_sock;

	struct timeval timeout;
	timeout.tv_sec = 1;
	timeout.tv_usec = 0;

	if (setsockopt(browser_sock, SOL_SOCKET, SO_RCVTIMEO, (char *) &timeout,
			sizeof(timeout)) < 0) {
		error("setsockopt failed\n");
	}

	while (1) {

		rawRequest_buf = (char*) malloc(MAX_REQUEST_LEN);
		memset(rawRequest_buf, 0, MAX_REQUEST_LEN);

		recv_len = recv(browser_sock, rawRequest_buf, MAX_REQUEST_LEN, 0);

		if (recv_len <= 0 || timeoutOccurred() || isPostRequest(rawRequest_buf)) {
			close(browser_sock);
			free(rawRequest_buf);
			return;
		}

		if (!firstRequestReceived) {
			firstRequestReceived = 1;
			struct addrinfo hints, *servinfo, *p;

			server_sock = -1;

			char * host;
			host = parseFieldFromRequest(rawRequest_buf, HOST_FIELD);

			memset(&hints, 0, sizeof(hints));

			hints.ai_family = PF_UNSPEC;
			hints.ai_socktype = SOCK_STREAM;
			int s = getaddrinfo(host, WEBSERVER_PORT, &hints, &servinfo);

			free(host);

			if (s != 0) {
				perror("getaddrinfo");
				close(browser_sock);
				free(rawRequest_buf);
				threadExit();
				return;
			}

			for (p = servinfo; p != NULL; p = p->ai_next) {
				if ((server_sock = socket(p->ai_family, p->ai_socktype,
						p->ai_protocol)) == -1) {
					perror("socket creation");
					continue;
				}

				if (connect(server_sock, p->ai_addr, p->ai_addrlen) == -1) {
					close(server_sock);
					perror("connect");
					continue;
				}

				break;
			}

			if (server_sock == -1) {
				close(server_sock);
				close(browser_sock);
				free(rawRequest_buf);
				threadExit();
				return;
			}
		}

		if (handleRequest(browser_sock, server_sock, rawRequest_buf) < 0) {
			close(server_sock);
			close(browser_sock);
			free(rawRequest_buf);
			threadExit();
			return;
		}
		free(rawRequest_buf);
	}
}

int start_proxy_server(int port, int cache_size) {

	struct sockaddr_in client_addr;

	int numThreads = 0;
	pthread_t threads[MAX_CONCURRENT_REQ];

	signal(SIGINT, sig_handler);
	signal(SIGPIPE, SIG_IGN);

	initializeCache(port, cache_size);

	struct sockaddr_in server_addr;

	if ((browserProxySock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("Create socket error");
		return -1;
	}

	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);

	if (bind(browserProxySock, (struct sockaddr *) &server_addr,
			sizeof(server_addr)) < 0) {
		perror("Binding socket error");
		return 1;
	}

	listen(browserProxySock, MAX_BACK_LOG);

	socklen_t client_addr_len = sizeof(client_addr);

	while (1) {

		int accepted_sock = accept(browserProxySock,
				(struct sockaddr *) &client_addr, &client_addr_len);

		if (accepted_sock < 0) {
			perror("Accepting connection error");
			return -1;
		}

		printf("Accepted browser persistent connection.\n");

		memset(&threads[numThreads], 0, sizeof threads[numThreads]);

		pthread_create(&threads[numThreads], NULL,
				(void *) &handlePersistentConnection, (void *) accepted_sock);

		numThreads = (numThreads < MAX_CONCURRENT_REQ) ? numThreads + 1 : 0;
	}

	return 0;
}


////////////////////////////////// MAIN ///////////////////////////////////


int main(int argc, char ** argv) {
	if (argc != 3) {
		printf("Command should be: myprog <port> <cache size in MB>\n");
		return 1;
	}
	int port = atoi(argv[1]);
	if (port < 1024 || port > 65535) {
		printf(
				"Port number should be equal to or larger than 1024 and smaller than 65535\n");
		return 1;
	}
	int cache_size = atoi(argv[2]);
	if (cache_size < 1 || cache_size > 100) {
		printf("Cache size must be between 1 MB and 100 MB");
		return 1;
	}
	start_proxy_server(port, cache_size);
	return 0;
}

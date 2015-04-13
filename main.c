#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <netdb.h>

int start_proxy_server(uint16_t port, uint16_t cache_size);
void sig_handler(int sig);
void handleRequest(void* request);

#define MAX_MSG_LENGTH (512)
#define MAX_REQUEST_LEN (500)
#define MAX_BACK_LOG (5)
#define MAX_CONTENT_LEN (1000000)
#define BYTES_PER_MB (1000000)
#define WEBSERVER_PORT "http"
#define GET_REQUEST_FIELD "GET"
#define FIELD_HOST "Host: "
#define FIELD_USER_AGENT "User-Agent: "
#define FIELD_ACCEPT "Accept: "
#define FIELD_ACCEPT_LANGUAGE "Accept-Language: "
#define FIELD_ACCEPT_ENCODING "Accept-Encoding: "
#define FIELD_CONNECTION "Connection: "
#define SPACE " /"
#define NEW_LINE "\n"

typedef struct cache_element {
	struct cache_element* next;
	struct cache_element* prev;
	uint32_t bytes;
	char* content;
	char* request;
} cache_element;

typedef struct cache {
	cache_element* MR;	//Most recent add
	cache_element* LR; //Least recent add
	uint32_t bytes;
	uint32_t max_bytes;
} cache;

int sock, server_accepted_sock;
cache* LRU_cache;


/**
 * Scans the cache starting at the most recently added element
 * for a cache element with the input request. If such an element
 * exists, a pointer to it is returned. Otherwise, return NULL
 */
cache_element* scanCacheForElement(char* request) {
	cache_element* temp = LRU_cache->MR;
	while(temp != NULL) {
		if(strcmp(request, temp->request) == 0) {
			return temp;
		}
		temp = temp->next;
	}
	return NULL;
}

/**
 * Removes input cache element from cache and updates the
 * bytes field of the cache.
 */
void removeElementFromCache(cache_element* el) {
	cache_element* prevEl = el->prev;
	cache_element* nextEl = el->next;
	if(prevEl) {
		prevEl->next = nextEl;
	}
	if(nextEl) {
		nextEl->prev = prevEl;
	}
	LRU_cache->bytes -= el->bytes;
	free(el->request);
	free(el->content);
	free(el);
}

/**
 * Adds most recently accessed element to the front of the cache Linked list.
 */
void addElementToCache(cache_element* MRelement) {

	if(MRelement->bytes > LRU_cache->max_bytes) {
		return;
	}

	while(LRU_cache->bytes + MRelement->bytes > LRU_cache->max_bytes) {
		removeElementFromCache(LRU_cache->LR);
	}

	//Update Cache with MRU
	MRelement->prev = NULL;
	MRelement->next = LRU_cache->MR;
	LRU_cache->MR = MRelement;
	if(LRU_cache->LR == NULL) {
		LRU_cache->LR = MRelement;
	}
}



/**
 * Given an element in the cache that was recently fetched, the element
 * is made to be the most recently accessed element in the cache.
 */
void updateCacheMR(cache_element* MRelement) {
	removeElementFromCache(MRelement);
	addElementToCache(MRelement);
}





int main(int argc, char ** argv)
{
	if (argc != 3) {
		printf("Command should be: myprog <port> <cache size in MB>\n");
		return 1;
	}
	uint16_t port = atoi(argv[1]);
	if (port < 1024 || port > 65535) {
		printf("Port number should be equal to or larger than 1024 and smaller than 65535\n");
		return 1;
	}
	uint16_t cache_size = atoi(argv[2]);
	if(cache_size < 1 || cache_size > 100) {
		printf("Cache size must be between 1 MB and 100 MB");
		return 1;
	}
	start_proxy_server(port, cache_size);
	return 0;
}

/**
 * Given the title of a field in the request body such as Host: , this function
 * returns the field.
 */
char* parseFieldFromRequest(char* request, char* field) {
	int offset = (field == GET_REQUEST_FIELD) ? 0 : strlen(field);
	char* requestStart = strstr(request, field) + offset;
	char* requestEnd = strstr(requestStart, "\r");
	int parsedRequestLen = requestEnd-requestStart;
	char* parsedRequest = (char*) malloc(parsedRequestLen);
	memset(parsedRequest, 0, parsedRequestLen);
	memcpy(parsedRequest, requestStart, parsedRequestLen);
	return parsedRequest;
}

char receivedContent[MAX_CONTENT_LEN];

pthread_mutex_t mutexsum;


//take raw request and fix fields
/*
get this to look like format char* from main2.c
strstr on user-agent



*/
char* formatOutgoingRequest(char* request, char* urlSuffix) {
//RYAN
	char* outgoingRequest = (char*) malloc(strlen(request)+15); //TODO: change back to 5?
	int size = 0;
	memcpy(outgoingRequest, GET_REQUEST_FIELD, strlen(GET_REQUEST_FIELD));
	size += strlen(GET_REQUEST_FIELD);
	memcpy(outgoingRequest + size , SPACE, strlen(SPACE));
	size+= strlen(SPACE);
	memcpy(outgoingRequest + size , urlSuffix, strlen(urlSuffix));
	size += strlen(urlSuffix);
	//return to new line
	memcpy(outgoingRequest + size, NEW_LINE, strlen(NEW_LINE));
	size+= strlen(NEW_LINE);

	//HOST
	memcpy(outgoingRequest + size, FIELD_HOST, strlen(FIELD_HOST));
	size+= strlen(FIELD_HOST);
	char* temp_host = "dartmouth.edu";
	memcpy(outgoingRequest + size, temp_host, strlen(temp_host));
	size+= strlen(temp_host);
	memcpy(outgoingRequest + size, NEW_LINE, strlen(NEW_LINE));
	size+= strlen(NEW_LINE);

	//User-Agent
	memcpy(outgoingRequest + size, FIELD_USER_AGENT, strlen(FIELD_USER_AGENT));
	size+= strlen(FIELD_USER_AGENT);
	char* temp_userAgent = "Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:23.0) Gecko/20100101 Firefox/23.0";
	//char* temp_userAgent = "runscope/0.1";
	memcpy(outgoingRequest + size, temp_userAgent, strlen(temp_userAgent));
	size+= strlen(temp_userAgent);
	memcpy(outgoingRequest + size, NEW_LINE, strlen(NEW_LINE));
	size+= strlen(NEW_LINE);




	//Accept
	memcpy(outgoingRequest + size, FIELD_ACCEPT, strlen(FIELD_ACCEPT));
	size+= strlen(FIELD_ACCEPT);
	char* temp_accept = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";
	//char* temp_accept = "*/*";
	memcpy(outgoingRequest + size, temp_accept, strlen(temp_accept));
	size+= strlen(temp_accept);
	memcpy(outgoingRequest + size, NEW_LINE, strlen(NEW_LINE));
	size+= strlen(NEW_LINE);

	//Accept-Language
	memcpy(outgoingRequest + size, FIELD_ACCEPT_LANGUAGE, strlen(FIELD_ACCEPT_LANGUAGE));
	size+= strlen(FIELD_ACCEPT_LANGUAGE);
	char* temp_accept_language = "en-US,en;q=0.5";
	memcpy(outgoingRequest + size, temp_accept_language, strlen(temp_accept_language));
	size+= strlen(temp_accept_language);
	memcpy(outgoingRequest + size, NEW_LINE, strlen(NEW_LINE));
	size+= strlen(NEW_LINE);





	//Accept-Encoding
	memcpy(outgoingRequest + size, FIELD_ACCEPT_ENCODING, strlen(FIELD_ACCEPT_ENCODING));
	size+= strlen(FIELD_ACCEPT_ENCODING);
	char* temp_accept_encoding = "gzip, deflate";
	memcpy(outgoingRequest + size, temp_accept_encoding, strlen(temp_accept_encoding));
	size+= strlen(temp_accept_encoding);

	memcpy(outgoingRequest + size, NEW_LINE, strlen(NEW_LINE));
	size+= strlen(NEW_LINE);





	//Connection
	memcpy(outgoingRequest + size, FIELD_CONNECTION, strlen(FIELD_CONNECTION));
	size+= strlen(FIELD_CONNECTION);
	char* temp_connection = "keep-alive";
	memcpy(outgoingRequest + size, temp_connection, strlen(temp_connection));
	size+= strlen(temp_connection);

	memcpy(outgoingRequest + size, NEW_LINE, strlen(NEW_LINE));
	size+= strlen(NEW_LINE);

	memcpy(outgoingRequest + size, "\r\n\r\n\0", 5);


	//memcpy(outgoingRequest + strlen(GET_REQUEST_FIELD) + strlen(urlSuffix) , temp, strlen(temp));


	//memcpy(outgoingRequest, request, strlen(request));
	//memcpy(outgoingRequest + strlen(GET_REQUEST_FIELD) + strlen(urlSuffix) + strlen(temp), "\r\n\r\n\0", 5);
	return outgoingRequest;
//	char* oldUserAgent = strstr(request, "User-Agent:");
//	char* fieldsAfterAgent = strstr(oldUserAgent,"\n") + 1;
//	char* newUserAgent = "User-Agent: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:23.0) Gecko/20100101 Firefox/23.0\r\n\r\n";

//	int beforeAgentLen = oldUserAgent - request;
//
//	int outgoingRequestLen = beforeAgentLen + strlen(newUserAgent);
//	char* outgoingRequest = (char*) malloc(outgoingRequestLen);
//
//	memcpy(outgoingRequest, request, beforeAgentLen);
//	memcpy(outgoingRequest + beforeAgentLen, newUserAgent, strlen(newUserAgent));
//	return outgoingRequest;


//	int outgoingRequestLen = strlen(request) - (fieldsAfterAgent-oldUserAgent) + strlen(newUserAgent);
//	char* outgoingRequest = (char*) malloc(outgoingRequestLen);
//
//	memcpy(outgoingRequest, request, beforeAgentLen);
//	memcpy(outgoingRequest + beforeAgentLen, newUserAgent, strlen(newUserAgent));
//	memcpy(outgoingRequest + beforeAgentLen + strlen(newUserAgent), fieldsAfterAgent, outgoingRequestLen - beforeAgentLen - strlen(newUserAgent));
//	return outgoingRequest;
}


char* makeRequest(char* request) {
	printf("**************************Initial Input Request:****************** \n %s ----------END OF REQUST----------\n", request);
	int outgoing_sock = -1;
	struct addrinfo hints, *servinfo, *p;

	memset(&hints, 0, sizeof(hints));
	memset(&servinfo, 0, sizeof(servinfo));
	memset(&p, 0, sizeof(p));
	memset(receivedContent, 0, MAX_CONTENT_LEN);

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	char* hostName = parseFieldFromRequest(request, FIELD_HOST);
	char* urlSuffix = parseFieldFromRequest(request, hostName);
	//printf("host name = %s -----\n", hostName);
	//printf("SUFFIX name = %s -----\n", urlSuffix);


	getaddrinfo(hostName, WEBSERVER_PORT, &hints, &servinfo);
	free(hostName);

	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((outgoing_sock = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			printf("In here swaggin\n");
			//			perror("socket");
		}

		if (connect(outgoing_sock, p->ai_addr, p->ai_addrlen) == -1) {
			close(outgoing_sock);
			perror("connect");
			continue;
		}

		break;
	}

	char* outgoingRequest = formatOutgoingRequest(request, urlSuffix);
	//char* outgoingRequest = request;
	printf("********NEW OUTGOING REQUEST********: \n%s\n*************", outgoingRequest);
//	char* outgoingRequest = request;

	if (send(outgoing_sock, outgoingRequest, strlen(outgoingRequest), 0) < 0) {
		perror("Send error:");
		printf("Failed Request: \n%s\n", outgoingRequest);
		close(outgoing_sock);
		free(outgoingRequest); //error could be trying to "request" twice
		return NULL;
	}

	int recv_len = 0;
	int totalBytesRead = 0;

	do {
		recv_len = read (outgoing_sock, receivedContent + totalBytesRead, MAX_CONTENT_LEN - totalBytesRead);
		if (recv_len < 0) { // recv 0 bytes so you are done
			perror("Recv error:");
			close(outgoing_sock);
			free(outgoingRequest);
			return NULL;
		}
		totalBytesRead += recv_len;
	} while(recv_len > 0 && totalBytesRead < MAX_CONTENT_LEN);

	char *content = (char*) malloc(totalBytesRead);
	memcpy(content, receivedContent, totalBytesRead);


	free(outgoingRequest);
	close(outgoing_sock);

	return content;
}


void sendContentToClient(char* content) {
	send(server_accepted_sock, content, strlen(content), 0);
}

void handleRequest(void* request) {

	char* requestMsg = (char*) request;

//	printf("Request:\n%s\n", requestMsg);

	if(strlen(requestMsg) <= 0 || strstr(request, "GET") != request) {
		printf("Request was not a GET\n");
		return;
	}

	char* parsedRequest = parseFieldFromRequest(requestMsg, GET_REQUEST_FIELD);

	cache_element* foundElement = scanCacheForElement(parsedRequest);

	char* content;

	if(foundElement == NULL) {

		content = makeRequest(requestMsg);

		if(content == NULL) {
			printf("Failed to generate content\n");
			return;
		}

		cache_element* newElement = (cache_element*) malloc(sizeof(cache_element));
		memset(newElement, 0, sizeof(cache_element));
		newElement->request = parsedRequest;
		newElement->content = content;
		newElement->bytes = strlen(content);
		addElementToCache(newElement);

	}
	else {
		updateCacheMR(foundElement);
		content = foundElement->content;
	}
	sendContentToClient(content);
//	free(request);
}

void initializeCache(uint16_t port, uint16_t cache_size) {
	LRU_cache = (cache*) malloc(sizeof(cache));
	memset(LRU_cache, 0, sizeof(cache));
	LRU_cache->max_bytes = cache_size*BYTES_PER_MB;
}


int start_proxy_server(uint16_t port, uint16_t cache_size) {

	int msg_len;
	struct sockaddr_in server_addr, client_addr;

	int numThreads = 0;
	pthread_t threads[20];

	signal(SIGINT, sig_handler);	//close socket on kill signal
	signal(SIGPIPE,SIG_IGN);		//ignore SIG_PIPE signal

	initializeCache(port, cache_size);

	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("Create socket error:");
		return 1;
	}

	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);

	if (bind(sock, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
		perror("Binding socket error");
		return 1;
	}
	printf("Binding Socket...\n");

	listen(sock, MAX_BACK_LOG);
	printf("Listening...\n");

	socklen_t client_addr_len = sizeof(client_addr);

	//while(1) {

		printf("Waiting for connection...\n");

		server_accepted_sock = accept(sock, (struct sockaddr *) &client_addr, &client_addr_len);

		if(server_accepted_sock < 0) {
			perror("Accepting connection error");
			return -1;
		}

		printf("Accepted connection...\n");

		char* rawRequest = (char*) malloc(500);
		memset(rawRequest, 0, 500);

		msg_len = recv(server_accepted_sock, rawRequest, MAX_MSG_LENGTH, 0);

		//pthread_create(&threads[numThreads], NULL, (void *) &handleRequest, (void*) rawRequest);
		//numThreads++;
		handleRequest(rawRequest);
	//}
	return 0;
}


void sig_handler(int sig) {
	if(server_accepted_sock) {
		close(server_accepted_sock);
		printf("Socket was closed.\n");
	}
	exit(0);
}

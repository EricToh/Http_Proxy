/*
 *  proxy.c
 *  By Eric Toh and Collin Geary, 12/1/2020
 *  etoh01
 *  Comp 112: Networks Final Project
 *
 *  A Chat Server 
 *
 * 
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <math.h>

// CONSTANTS
const int OBJECT_MAX = 10000000;
const int CACHE_SIZE = 10;
const int GET = 1;
const int CON = 2;
const int TABLE_SIZE = 15;
const int TABLE_MAX_BYTES = 10000;


//STRUCTURES

struct Node{
    char key[1000];
    char * object;
    int time;
    int age;
    int port;
    int size;
    struct Node* next;
    struct Node*prev;
};

struct Node_Size{
    char key[1000];
    int size;
    struct Node * node;
    struct Node_Size * prev;
    struct Node_Size * next;
};

struct connection {
    int client_sock;
    int server_sock;
    struct connection * next;
    struct connection * prev;
};

struct Cache{
    int num_elements;
    int num_bytes;
    int capacity;
    struct Node ** table;
    struct Node_Size * largest;
};

struct Connections {
    struct connection * head;
    int num_connections;
};



// FUNCTION DECLARATIONS
void client_message(struct Cache * cache, struct Connections * connections,
                    int curr_socket, char * buffer, fd_set * master_set, int numbytes, int * fdmax);
void proxy_http(struct Cache * cache, int curr_socket, char * buffer,
                fd_set * master_set,  int numbytes, int webport,
                char * host_name, char * headerGET, char * headerHost);
void proxy_https(int curr_socket,char * buffer, int numbytes, int webport, char * host_name, char * headerHost, struct Connections * connections, fd_set * master_set, int * fdmax);
void secure_stream(int curr_socket, int server_con, char * buffer, int numbytes);
void print_list(struct Node * head);
void print_connection(struct connection * head);
void print_cache(struct Cache * cache);
void remove_connection(struct Connections * connections, int socket);
int has_connection(struct Connections * connections, int socket);
void prepend_connection(int client_sock, int server_sock, struct Connections * connections);
void remove_cache_node(struct Cache * cache, struct Node * node);
void prepend_cache_node(struct Cache * cache, struct Node * node);
void chain_front(struct Cache * cache, struct Node * node);
void remove_stale(struct Cache * cache);
bool is_stale(struct Node * node);
void add_to_size_list(struct Cache * cache, struct Node * node);
void remove_from_size_list(struct Cache * cache, char * key);
struct Node_Size * pop_largest(struct Cache * cache);
struct Cache * create_cache();

// SMALL HELPER FUNCTIONS
void error(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

int max(int num1,int num2) {
    if(num1 >= num2) {
        return num1;
    }else return num2;
}

// djb2 hash function by Dan Bernstein, hash function for a string
// Found on https://stackoverflow.com/questions/7666509/hash-function-for-string
unsigned long hash(char *str) {
    unsigned long hash = 5381;
    int c;

    while ((c = *str++)){
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }

    return hash;
}

//MAIN
int main (int argc, char *argv[]) {
    int master_sock, client_sock, server_port, curr_socket, numbytes;
    int size = 0;
    struct sockaddr_in server_address, client_address;
    socklen_t clilen;
    char * buffer = malloc(OBJECT_MAX);
    // Head of Cache
    struct Cache * cache = create_cache();
    // Head of client structure
    struct Connections * connections = malloc(sizeof(struct Connections));
    connections->head = NULL;
    connections->num_connections = 0;

    // Get port number to bind to
    if (argc != 2) {
        fprintf(stderr, "Wrong number of arguments\n");
        exit(EXIT_FAILURE);
    }
    server_port = atoi(argv[1]);

    printf("Creating socket\n");
    // Create Main Socket using socket()
    master_sock = socket(AF_INET,SOCK_STREAM, 0);
    if (master_sock < 0) {
        error("Error opening main socket");
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 50000;
    setsockopt(master_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    bzero((char *) &server_address, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(server_port);

    //Bind Socket to our port
    printf("Binding socket\n");
    if (bind(master_sock, (struct sockaddr *) &server_address, sizeof(server_address)) < 0) {
        error("Error on binding");
    }

    //Listen on port 
    if(listen(master_sock, 1) < 0) {
        error("Error on listen");
    }

    // SELECT INITIALIZATION STUFF
    fd_set master_set; 
    fd_set temp_set;

    FD_ZERO(&master_set);
    FD_ZERO(&temp_set);

    // Add master socket to set and save the fd
    FD_SET(master_sock, &master_set);
    int * fdmax = malloc(sizeof(int));
    *fdmax = master_sock;

    //Main Loop for select and accepting clients
    printf("Entering main loop for select\n");
    while(1) {
        temp_set = master_set;
        printf("select\n");
        select(*fdmax + 1, &temp_set, NULL, NULL, NULL); 

        // If main socket is expecting new connection
        if(FD_ISSET(master_sock, &temp_set)) {
            printf("\nNew socket connection from client on socket");
            clilen = (size_t)sizeof(client_address);
            client_sock = accept(master_sock, (struct sockaddr *) &client_address, &clilen);
            if (client_sock < 0) {
                error ("accept");
            }
            printf("%d\n\n", client_sock);
            FD_SET(client_sock, &master_set);
            *fdmax = max(client_sock,*fdmax);
        } else {

            for(curr_socket = 0; curr_socket <= *fdmax; curr_socket++) {
                // printf("Current check: %d\n", curr_socket);
                if(FD_ISSET(curr_socket, &temp_set)) {
                    print_cache(cache);
                    printf("\nSocket %d is set\n",curr_socket);
                    numbytes = recv(curr_socket, buffer, OBJECT_MAX-1, 0);
                    if (numbytes < 0) {
                        printf("Less than 0 read from socket %d\n", curr_socket);
                    }else if (numbytes == 0) {
                        printf("Client %d has left in orderly conduct\n", curr_socket);
                        remove_connection(connections, curr_socket);
                        FD_CLR(curr_socket, &master_set);
                        //remove_client(clients, curr_socket, &master_set);
                    }else{
                        printf("Recieved client message of size %d from socket %d\n", numbytes, curr_socket);
                        client_message(cache, connections, curr_socket, buffer, &master_set, numbytes, fdmax);
                        printf("Return from client message\n");
                    }
                }
            }
        }
    }
}

// Function to deal with the client's message
void client_message(struct Cache * cache, struct Connections * connections,
                    int curr_socket, char * buffer, fd_set * master_set, int numbytes, int * fdmax){
    // printf("Entered client message\n");
    int partner_con = has_connection(connections, curr_socket);
    // printf("server con\n");
    if(partner_con != -1) {
        secure_stream(curr_socket, partner_con, buffer, numbytes);
        printf("return from secure_stream\n");
        return;
    }

    char cpyhost[100];
    char headerGET[1000];
    char headerHost[1000];
    int webport;
    char *curr_line = NULL;
    char *host_name = NULL;
    char delim[] = "\n";
    char * newbuffer = malloc(sizeof(char) * OBJECT_MAX);
     
    //Find the GET field and the Host field and separate them
    printf("Finding Get field and Host field\n");
    bzero(newbuffer, OBJECT_MAX);
    memcpy(newbuffer, buffer, numbytes);
    curr_line = strtok(newbuffer,delim);
        
    int status = 0;
    printf("If statement\n");
    if(curr_line[0] == 'G'&& curr_line[1] == 'E' && curr_line[2] == 'T'){
        printf("curr_line: %s\n", curr_line);
        strcpy(headerGET, curr_line);
        printf("STRCPY COMPLETE\n");
        status = GET;
    }
    if(curr_line[0] == 'C'&& curr_line[1] == 'O' && curr_line[2] == 'N'){
        printf("CON\n");
        strcpy(headerGET, curr_line);
        status = CON;
    }
    if(curr_line[0] == 'H' && curr_line[1] == 'o' && curr_line[2] == 's'){
        printf("HOST\n");
        strcpy(headerHost, curr_line);
    }
    printf("while statement\n");
    while((curr_line = strtok(NULL, delim)) != NULL){
        printf("Currline: %s\n",curr_line);
        if(curr_line[0] == 'G' && curr_line[1] == 'E' && 
            curr_line[2] == 'T'){
            strcpy(headerGET, curr_line);
            status = GET;
        }
        if(curr_line[0] == 'C' && curr_line[1] == 'O' && 
            curr_line[2] == 'N'){
            strcpy(headerGET, curr_line);
            status = CON;
        }
        if(curr_line[0] == 'H' && curr_line[1] == 'o' && 
            curr_line[2] == 's'){
            strcpy(headerHost, curr_line);
        }
    }

    printf("Copying over host\n");
    strcpy(cpyhost, headerHost);
    host_name = strtok(cpyhost, ":");
    host_name = strtok(NULL, ":");
    host_name = strtok(NULL, ":");
    if(host_name != NULL){
        webport = atoi(host_name);
    }else{
        webport = 80;
    }
    if(status == CON){
        proxy_https(curr_socket, buffer, numbytes, webport, host_name, headerHost, connections, master_set, fdmax);
        return;
    }else if(status == GET){
        proxy_http(cache, curr_socket, buffer, master_set, numbytes, webport,
                     host_name, headerGET, headerHost);
        close(curr_socket);
        FD_CLR(curr_socket, master_set);
    }else {
        printf("Message is not a get or connect and does not come from and established connection\n");
        close(curr_socket);
        FD_CLR(curr_socket, master_set);
    }
}

struct Node * search_chain(struct Node * head, char * key) {
    if(head == NULL) {
        printf("Chain is empty\n");
        return NULL;
    }
    struct Node * temp = head;
    while(temp != NULL) {
        printf("Object key: %s\n", temp->key);
        if(strcmp(key, temp->key) == 0) {
            printf("Found in chain\n");
            return temp;
        }
        temp = temp->next;
    }
    return NULL;
}

struct Node * search_cache(struct Cache * cache, char * key) {
    printf("Searching cache for headerGet: %s\n", key);
    int hash_val = (int)(hash(key)%TABLE_SIZE);
    printf("Searching chain index %d\n", hash_val);
    struct Node * object = search_chain(cache->table[hash_val], key);
    if(object == NULL) {
        return NULL;
    }else {
        return object;
    }
}


void proxy_http(struct Cache * cache, int curr_socket, char * buffer,
                fd_set * master_set, int numbytes, int webport,
                char * host_name, char * headerGET, char * headerHost) {
    printf("Entered proxy http\n");
    struct hostent *server;
    struct sockaddr_in serveraddr;
    time_t rawtime;
    //Check if already in cache
    rawtime = time (NULL);
    int update = 0;
    printf("cache Size: %d\n", cache->num_elements);
    struct Node * cache_hit = search_cache(cache, headerGET);
    if(cache_hit != NULL) {
        // Checks if it is correct port and if it has expired
        if((cache_hit->age + cache_hit->time) > rawtime &&
        cache_hit->port == webport){
            printf("Normal Cache hit\n");
            chain_front(cache, cache_hit);
            printf("Writing stored data\n");
            //write(curr_socket, objcopy, cache_hit->size + age_len + 1);
            write(curr_socket, cache_hit->object, cache_hit->size);
            return;
        }else {
            printf("Cache hit, but expired or wrong port\n");
            // Remove expired object?
            if((cache_hit->age + cache_hit->time) <= rawtime) {
                printf("Removing expired object");
                remove_from_size_list(cache, cache_hit->key);
                remove_cache_node(cache, cache_hit);
            }
        }
    }
    printf("No cache hit\n");
    printf("Creating new cache object\n");
    //Create a node;
    struct Node * temp = (struct Node*)malloc(sizeof(struct Node));
    strcpy(temp->key, headerGET);
    //Create socket for web server
    int clientfd;
    clientfd = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    setsockopt(clientfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    //get hostbyname
    host_name =  strtok(headerHost, " ");
    host_name = strtok(NULL, " ");
    char tempname[100];
    int i = 0;
    while(host_name[i] != '\0' && host_name[i] != '\n' 
          && host_name[i] != ':'){
        tempname[i] = host_name[i];
        i++;
    }
    if(host_name[i] == ':'){
        tempname[i] = '\0';
    }else{
        tempname[i-1] = '\0';
    }
    server = gethostbyname(tempname);
    if(server == NULL) {
        fprintf(stderr, "ERROR, no such host as %s\n", tempname);
        close(clientfd);
        exit(0);
    }
    temp->port = webport;
  
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *)&serveraddr.sin_addr.s_addr,
           server->h_length);
    serveraddr.sin_port = htons(webport);

    //connect to webserver
    if(connect(clientfd, (struct sockaddr*) &serveraddr, 
       sizeof(serveraddr)) < 0){
        fprintf(stderr, "ERROR connecting");
    }

    //Forward Client request to Webserver
    write(clientfd, buffer, strlen(buffer));

    //Read Webserver's reply
    printf("Reading server reply\n");
    char * bigbuf = (char *) malloc(10000000);
    char * bufchunk = (char *) malloc(10000000);
    
    bzero(buffer, OBJECT_MAX);
    bzero(bigbuf, 10000000);
    int count , header_size;
    char * end_head;
    char content_len [15] = "Content-Length:";
    int contentsize = 0;
    char delim[] = "\n";

    // Read first chunk
    count = recv(clientfd, bufchunk, OBJECT_MAX, 0);
    memcpy(bigbuf + contentsize, bufchunk, count);
    contentsize += count;
    printf("Read first chunk of size %d\n", count);
    // Read until the entire header has been read
    while((end_head = strstr(bigbuf,"\r\n\r\n")) == NULL) {
        printf("Portion read so far: %s\n",bigbuf);
        count = recv(clientfd, bufchunk, OBJECT_MAX, 0);
        if(count >= 0) {
            memcpy(bigbuf + contentsize, bufchunk, count);
            contentsize += count;
        }
        printf("Read a HTTP server reply of size %d\n", count);
    }
    header_size = end_head - bigbuf + 4;
    printf("Header Size: %d\n", header_size);
    //Extract content_size
    int cont_size = 0;
    char * search_buf =  (char *) malloc(10000000);
    memcpy(search_buf,bigbuf,contentsize);
    char * curr_line = strtok(search_buf, delim);
    while((curr_line = strtok(NULL, delim)) != NULL){
        if((memcmp(&curr_line[0],&content_len[0],15)) == 0){
            char c;
            int start_i = 16;
            while((c =curr_line[start_i]) != ' ' && curr_line[start_i] != '\n' && curr_line[start_i] != '\r') {
                cont_size *= 10;
                cont_size += (c - 48);
                start_i++;
            }
        }
    }
    printf("Extracted content size %d\n", cont_size);
    int total_bytes =  header_size + cont_size;
    // Read until all bytes have been read
    while(contentsize < total_bytes) {
        count = recv(clientfd, bufchunk, OBJECT_MAX, 0);
        if(count >= 0) {
            memcpy(bigbuf + contentsize, bufchunk, count);
            contentsize += count;
        }
        printf("Read a HTTP server reply of size %d, total read so far: %d\n", count, contentsize);

    }
    printf("Read a HTTP server reply of size %d\n", contentsize);
    printf("According to header it was supposed to be size %d\n", total_bytes);

    close(clientfd);

    //Send reply to Client
    int n = write(curr_socket, bigbuf, contentsize);
    printf("Write of size %d to client\n", n);


    temp->object = malloc(sizeof(char) *contentsize);
    memcpy(temp->object, bigbuf, contentsize);
    temp->size = contentsize;

    //Add time and age to new object
    temp->time = rawtime;
    char *timebuf = malloc(10000000 * sizeof(char));
    strcpy(timebuf, bigbuf);
    char searchString[50] = "Cache-Control: max-age=";
    char *result;
    result = strstr(timebuf, searchString);
    if(result == NULL){
        temp->age = 3600;
    }else{
        curr_line = strtok(result, "=");
        curr_line = strtok(NULL, "=");
        temp->age = atoi(curr_line);
    }
        
    //Add node to cache

    printf("Adding to cache\n");
    if(cache->num_bytes > ((double)cache->capacity)/2){
        remove_stale(cache);
    }
    if(temp->size > cache->capacity) {
        write(curr_socket, bigbuf, contentsize);
        free(bigbuf);
        free(bufchunk);
        free (timebuf);
        free(temp->object);
        free(temp);
        return;
    }
    while((cache->num_bytes + temp->size) > cache->capacity){
        printf("Adding this object would exceed MAX BYTES, removing largest\n");
        struct Node_Size * remove = pop_largest(cache);
        remove_cache_node(cache, remove->node);
        free(remove);
        printf("New Cache Size: %d\n", cache->num_bytes);
    }
    prepend_cache_node(cache,temp);
    add_to_size_list(cache, temp);

    free(bigbuf);
    free(bufchunk);
    free (timebuf);
    free(search_buf);
}

void proxy_https(int curr_socket,char * buffer, int numbytes, int webport, char * host_name, char * headerHost, struct Connections * connections, fd_set * master_set, int * fdmax){
    printf("HTTPS\n");
    int server_sock;
    struct hostent *server;
    struct sockaddr_in serveraddr;
    char * buf = malloc(OBJECT_MAX);

    server_sock = socket(AF_INET, SOCK_STREAM, 0);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    setsockopt(server_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        
    //get hostbyname
    host_name =  strtok(headerHost, " ");
    host_name = strtok(NULL, " ");
    char tempname[100];
    int i = 0;
    while(host_name[i] != '\0' && host_name[i] != '\n' 
          && host_name[i] != ':'){
        tempname[i] = host_name[i];
        i++;
    }
    if(host_name[i] == ':'){
        tempname[i] = '\0';
    }else{
        tempname[i-1] = '\0';
    }
    server = gethostbyname(tempname);
    if(server == NULL) {
        fprintf(stderr, "ERROR, no such host as %s\n", tempname);
        close(server_sock);
        exit(0);
    }

    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *)&serveraddr.sin_addr.s_addr,
           server->h_length);
    serveraddr.sin_port = htons(webport);

    //connect to webserver
    if(connect(server_sock, (struct sockaddr*) &serveraddr, 
       sizeof(serveraddr)) < 0){
        fprintf(stderr, "ERROR connecting");
    }

    //Forward Client request to Webserver
    printf("Client Request: \n");
    printf("%s\n", buffer);
    //write(server_sock, buffer, strlen(buffer));

    // Creating connection success message
    char bigbuf[100] = "HTTP/1.1 200 Connection established\r\n\r\n\0";
    printf("Server Response: \n");
    printf("-------CONNECTION ESTABLISHED--------\nClient Socket: %d\nServer socket: %d\n------------------------------\n",curr_socket,server_sock);

    printf("Sending %s to %d\n", bigbuf, curr_socket);
    int n = write(curr_socket, bigbuf, strlen(bigbuf));
    printf("Sent msg of size %d\n", n);

    prepend_connection(curr_socket, server_sock, connections);

    *fdmax = max(server_sock,*fdmax);
    FD_SET(server_sock, master_set);
}

void secure_stream(int socket, int socket_partner, char * buffer, int numbytes){
    printf("Found existing connection, sending %d msg to %d\n",numbytes,socket_partner);
    int n = write(socket_partner, buffer, numbytes);
    printf("Sent msg of size %d\n", n);
}


void print_connection(struct connection * head) {
    printf("\nPrinting connection list\n");
    if(head == NULL) {
        printf("This specific list is empty\n");
        return;
    }
    struct connection * temp = head;
    while(temp != NULL) {
        printf("Connection between client socket %d and server socket %d\n", temp->client_sock, temp->server_sock);
    }
    printf("\n");
}


int has_connection(struct Connections * connections, int socket){
    printf("Searching for existing connection, Socket: %d\n", socket);
    struct connection * head = connections->head;
    if(head == NULL) {
        printf("Connections is empty\n");
        return -1;
    }
    struct connection * temp = head;
    while(temp != NULL) {
        int client_sock = temp->client_sock;
        int server_sock = temp->server_sock;
        printf("Client: %d, Server: %d\n",client_sock, server_sock);
        if(client_sock == socket) {
            printf("Connection found, is a client\n");
            remove_connection(connections, client_sock);
            prepend_connection(client_sock, server_sock, connections);
            return server_sock;
        }else if (server_sock == socket) {
            printf("Connection found, is a server\n");
            remove_connection(connections, client_sock);
            prepend_connection(client_sock, server_sock, connections);
            return client_sock;
        }
        temp = temp->next;
    }
    return -1;
}

void remove_connection(struct Connections * connections, int socket) {
    printf("Removing connection of client %d\n", socket);
    struct connection * head = connections->head;
    if(head == NULL) {
        return;
    }
    struct connection * temp = head;
    while(temp != NULL) {
        if(temp->client_sock == socket || temp->server_sock == socket) {
            if(temp->prev != NULL) {
                temp->prev->next = temp->next;
            }else {
                connections->head = temp->next;
            }
            if(temp->next != NULL) {
                temp->next->prev = temp->prev;
            }
            free(temp);
            return;
        }
        temp = temp->next;
    }
}


void prepend_connection(int client_sock, int server_sock, struct Connections * connections) {
    printf("Prepending connection of client %d\n", client_sock);
    struct connection * new_connection = malloc(sizeof(struct connection));
    new_connection->client_sock = client_sock;
    new_connection->server_sock = server_sock;
    if(connections->head == NULL) {
        printf("Successful prepend on empty list\n");
        connections->head = new_connection;
        new_connection->next = NULL;
        new_connection->prev = NULL;
        connections->num_connections = 1;
        return;
    }
    struct connection * temp = connections->head;
    temp->prev = new_connection;
    new_connection->next = temp;
    new_connection->prev = NULL;
    connections->head = new_connection;
    connections->num_connections += 1;
    printf("Successful prepend\n");
}


void remove_cache_node(struct Cache * cache, struct Node * node) {
    if(cache == NULL || cache->table == NULL || node == NULL) {
        return;
    }
    struct Node * prev = node->prev;
    struct Node * next = node->next;
    if(prev == NULL) {
        cache->table[hash(node->key)%TABLE_SIZE] = next;
        if(next != NULL) 
            next->prev = NULL;
    }else {
        prev->next = next;
        if(next != NULL) 
            next->prev = prev;
    }
    cache->num_bytes -= node->size;
    free(node);
    cache->num_elements -= 1;
}

// Prepends a cache node in its correct cache slot

void prepend_cache_node(struct Cache * cache, struct Node * node) {
    printf("Attempting to prepend cache node\n");
    if(cache == NULL || cache->table == NULL || node == NULL) {
        return;
    }
    int hash_val = (int)(hash(node->key)%TABLE_SIZE);
    struct Node * temp = cache->table[hash_val];
    if(temp == NULL) {
        cache->table[hash_val] = node;
        node->prev = NULL;
        node->next = NULL;
        printf("Successfully prepended node, first node in this chain\n");
        cache->num_bytes += node->size;
        cache->num_elements += 1;
        return;
    }
    cache->table[hash_val] = node;
    temp->prev = node;
    node->next = temp;
    node->prev = NULL;
    cache->num_elements += 1;
    cache->num_bytes += node->size;
    printf("Successfully prepended node\n");
}

// Moves a cache node to the front of its chain
void chain_front(struct Cache * cache, struct Node * node) {
    printf("Moving node to front of chain\n");
    if(cache == NULL || cache->table == NULL || node == NULL) {
        return;
    }
    int hash_val = (int)(hash(node->key)%TABLE_SIZE);
    struct Node * temp = cache->table[hash_val];
    if(temp == node) {
        printf("Node already at front\n");
        return;
    }
    struct Node * prev = node->prev;
    struct Node * next = node->next;
    if(prev != NULL) {
        prev->next = next;
        if(next != NULL)
            next->prev = prev;
    }
    cache->table[hash_val] = node;
    node->next = temp;
    temp->prev = node;
    printf("Node position updated\n");
}

// Removes all stale objects in the cache
void remove_stale(struct Cache * cache) {
    printf("Removing stale elements\n");
    if(cache == NULL || cache->table == NULL){
        return;
    }
    for(int i = 0; i < TABLE_SIZE; i++) {
       struct Node * temp = cache->table[i];
       if(temp != NULL) {
           while(temp != NULL) {
               if(is_stale(temp)) {
                   struct Node * next = temp->next;
                   remove_from_size_list(cache, temp->key);
                   remove_cache_node(cache, temp);
                   temp = next;
               }else {
                    temp = temp->next;
               }
           }
       }
    }
}

// Checks if a cache object is stale
bool is_stale(struct Node * node) {
    time_t rawtime;
    //Check if already in cache
    rawtime = time (NULL);
    if(node != NULL && (node->age + node->time) > rawtime) {
        return false;
    }
    return true;
}

// Creates cache data structure
struct Cache * create_cache() {
    struct Cache * cache = malloc(sizeof(struct Cache));
    cache->num_elements = 0;
    cache->num_bytes = 0;
    cache->largest = NULL;
    cache->capacity = TABLE_MAX_BYTES;
    cache->table = malloc(TABLE_SIZE * sizeof(struct Node *));
    for(int i = 0; i < TABLE_SIZE; i++) {
        cache->table[i] = NULL;
    }
    return cache;
}


void print_cache(struct Cache * cache) {
    printf("Printing cache of %d elements. Total size: %d\n", cache->num_elements, cache->num_bytes);

    if(cache == NULL || cache->table == NULL){
        return;
    }
    struct Node ** table = cache->table;
    for(int i = 0; i < TABLE_SIZE; i++) {
        if(table[i] == NULL) {
            printf("Table index %d is empty\n", i);
        }else {
            printf("Table index %d: \n", i);
            struct Node * temp = table[i];
            while(temp != NULL) {
                printf("SIZE: %d   KEY: %s\n", temp->size, temp->key);
                temp = temp->next;
            }
        }
    }
    printf("\n\n");
}

void add_to_size_list(struct Cache * cache, struct Node * node) {
    if(cache == NULL || node == NULL) {
        return;
    }
    printf("Adding %s to size list\n", node->key);

    struct Node_Size * new = malloc(sizeof(struct Node_Size));
    strcpy(new->key, node->key);

    new->size = node->size;
    new->node = node;

    if(cache->largest == NULL) {
        printf("First element in size list\n");
        cache->largest = new;
        new->prev = NULL;
        new->next = NULL;
        return;
    }
    struct Node_Size * temp = cache->largest;
    while(temp->next != NULL && temp->next->size >= new->size) {
        temp = temp->next;
    }
    struct Node_Size * next = temp->next; 
    temp->next = new;
    new->prev = temp;
    new->next = next;
    if(next != NULL) {
        next->prev = new;
    }
}

void remove_from_size_list(struct Cache * cache, char * key) {
    if(cache == NULL ||  cache->largest == NULL) {
        return;
    }
    struct Node_Size * temp = cache->largest;
    while(temp != NULL) {
        if(strcmp(temp->key, key) == 0) {
            if(temp->prev != NULL) {
                temp->prev->next = temp->next;
            }else {
                cache->largest = temp->next;
            }
            if(temp->next != NULL) {
                temp->next->prev = temp->prev;
            }
            free(temp);
            return;
        }
        temp = temp->next;
    }
}

struct Node_Size * pop_largest(struct Cache * cache) {
    if(cache == NULL || cache->largest == NULL) {
        return NULL;
    }
    struct Node_Size * temp = cache->largest;
    cache->largest = temp->next;
    return temp;
}

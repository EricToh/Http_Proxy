
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
#include<sys/time.h>

// CONSTANTS
const int OBJECT_MAX = 10000000;
const int GET = 1;
const int CON = 2;
const int TABLE_SIZE = 15;
const int TABLE_MAX_BYTES = 10000000;
const int BPS = 50000;


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

struct Bucket{
    int tokens;
    time_t last_updated;
};

struct connection {
    bool secure;
    int client_sock;
    struct Bucket client_bucket;
    int server_sock;
    struct Bucket server_bucket;
    char * buffer;
    char key[1000];
    int port;
    int buf_size;
    int final_size;
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

/*
- Bucket starts with LIMIT_NUM tokens
- Access bucket, add tokens equal to rate*time since last update, if tokens exceed max set to max
- Update last_updated time
- Read from client equal to number of tokens in bucket
- Subtract bytes read from number of tokens
*/



// FUNCTION DECLARATIONS
void client_message(struct Cache * cache, struct Connections * connections,
                    int curr_socket, char * buffer, fd_set * master_set, int numbytes, int * fdmax);
void proxy_http(struct Cache * cache, int curr_socket, char * buffer,
                fd_set * master_set,  int numbytes, int webport,
                char * host_name, char * headerGET, char * headerHost,
                int * fdmax, struct Connections * connections);
void proxy_https(int curr_socket,char * buffer, int numbytes, int webport, char * host_name, char * headerHost, struct Connections * connections, fd_set * master_set, int * fdmax);

void secure_stream(int curr_socket, int server_con, char * buffer, int numbytes);

//Conection functions
void print_connection(struct connection * head);
struct connection * remove_connection(struct Connections * connections, int client_sock, int server_sock);
struct connection * has_connection(struct Connections * connections, int socket);
void prepend_connection(struct connection *, struct Connections * connections);
struct connection * new_connection(int client_sock, int server_sock,
bool secure);
bool add_msg_buf(struct connection * con, char * buffer, int numbytes);

//Rate Limit functions
int add_tokens(struct Bucket * b);
int rate_limit(struct connection * con, int socket);

// Cache functions
void print_cache(struct Cache * cache);
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

int min(int num1,int num2) {
    if(num1 >= num2) {
        return num2;
    }else return num1;
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

   /* struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 50000;
    setsockopt(master_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv); */
    int optval = 1;
    setsockopt(master_sock, SOL_SOCKET, SO_REUSEADDR,
             (const void *)&optval , sizeof(int));
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
                    // print_connection(connections->head);
                    // print_cache(cache);
                    printf("\nSocket %d is set\n",curr_socket);
                    
                    struct connection * con = has_connection(connections, curr_socket);
                    int tokens;
                    printf("adding tokens\n");
                    if(con != NULL){
                        if(con->client_sock == curr_socket){
                            tokens = add_tokens(&(con->client_bucket));
                        }else{
                            tokens = add_tokens(&(con->server_bucket));
                        }
                        if(tokens == 0){
                            continue;
                        }
                        printf("tokens added: %d\n", tokens);
                        numbytes = recv(curr_socket, buffer, tokens ,0);
                        printf("data recieved\n");
                        if(con->client_sock == curr_socket){
                            con->client_bucket.tokens -= numbytes;
                        }else{
                            con->server_bucket.tokens -= numbytes;
                        }
                    }else{
                        numbytes = recv(curr_socket, buffer, OBJECT_MAX ,0);
                    }
                    printf("tokens removed\n");
                    if (numbytes < 0) {
                        printf("Less than 0 read from socket %d\n", curr_socket);
                        struct connection * con = has_connection(connections, curr_socket);
                        if(con != NULL) {
                            remove_connection(connections, con->client_sock, con->server_sock);
                            close(con->client_sock);
                            close(con->server_sock);
                            FD_CLR(con->client_sock,&master_set);
                            FD_CLR(con->server_sock,&master_set);
                        }else {
                            close(curr_socket);
                            FD_CLR(curr_socket, &master_set);
                        }
                    }else if (numbytes == 0) {
                        printf("Client %d has left in orderly conduct\n", curr_socket);
                        struct connection * con = has_connection(connections, curr_socket);
                        if(con != NULL)
                            remove_connection(connections, con->client_sock, con->server_sock);
                        close(curr_socket);
                        FD_CLR(curr_socket, &master_set);
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
    struct connection * con = has_connection(connections, curr_socket);
    // printf("server con\n");
    if(con != NULL) {
        int partner_con;
        if(con->client_sock == curr_socket){
            partner_con = con->server_sock;
        }else{
            partner_con = con->client_sock;
        }
        if(!con->secure){
        //HTTP FORWARD/ADD TO BUFF
            if(add_msg_buf(con,buffer,numbytes)){
            // IF ENTIRE BUFFER IS READ
                int rawtime = time (NULL);
                char *curr_line;
                //Add to cache and close connections and FD Set stuff
                struct Node * temp = (struct Node*)malloc(sizeof(struct Node));
                strcpy(temp->key, (char*)con->key);
                temp->port = con->port;
                temp->size = con->buf_size;
                temp->object = malloc(sizeof(char) * con->buf_size);
                memcpy(temp->object, con->buffer, con->buf_size);
                //Add time and age to new object
                printf("Set cache time\n");
                temp->time = rawtime;
                char *timebuf = malloc(10000000 * sizeof(char));
                strcpy(timebuf, temp->object);
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
                    printf("Removing stale\n");
                    remove_stale(cache);
                }
                if(temp->size > cache->capacity) {
                    printf("Larger than cache capacity \n");
                    write(partner_con, con->buffer, con->buf_size);
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
                int n = write(partner_con, con->buffer, con->buf_size);
                printf("Wrote %d bytes to client %d\n", con->buf_size, partner_con);
                remove_connection(connections, con->client_sock,con->server_sock);
                close(con->client_sock);
                close(con->server_sock);
                FD_CLR(con->server_sock, master_set);
                FD_CLR(con->client_sock,master_set);
                free(con->buffer);
                free(con);
            }
            return;
        }else{ 
            //HTTPS Forward
            secure_stream(curr_socket, partner_con, buffer, numbytes);
            printf("return from secure_stream\n");
            return;
        }
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
                     host_name, headerGET, headerHost, fdmax, connections);
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
                char * host_name, char * headerGET, char * headerHost, 
                int * fdmax, struct Connections * connections) {
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
            close(curr_socket);
            FD_CLR(curr_socket, master_set);
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
    //struct Node * temp = (struct Node*)malloc(sizeof(struct Node));
    // printf("Set cache key\n");
    // strcpy(temp->key, headerGET);


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
        //exit(0);
    }
    // printf("Set cache port\n");
    // temp->port = webport;
  
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
    printf("Connected to webserver\n");


    //Forward Client request to Webserver
    write(clientfd, buffer, strlen(buffer));

    struct connection * con= new_connection(curr_socket, clientfd, false);
    strcpy((char *)con->key, headerGET);
    con->port = webport;
    prepend_connection(con, connections);
    *fdmax = max(clientfd,*fdmax);
    FD_SET(clientfd, master_set);

     /* COMMENT */
    //Read Webserver's reply
    /*
    bzero(buffer, OBJECT_MAX);
    int count , header_size;
    char * end_head;
    char content_len [15] = "Content-Length:";
    int contentsize = 0;
    char delim[] = "\n";
    printf("pre bus error\n");
    printf("Set cache object\n");
    temp->object = malloc(sizeof(char) *contentsize);
    memcpy(temp->object, bigbuf, contentsize);
    printf("Set cache size\n");
    temp->size = contentsize;
    //Add time and age to new object
    printf("Set cache time\n");
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
        printf("Removing stale\n");
        remove_stale(cache);
    }
    if(temp->size > cache->capacity) {
        printf("Larger than cache capacity \n");
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
    */
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
        //exit(0);
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

    struct connection * new_con = new_connection(curr_socket, server_sock, true);

    prepend_connection(new_con, connections);

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
        printf("Secure?: %d, Connection between client socket %d and server socket %d\n", temp->secure, temp->client_sock, temp->server_sock);
        temp = temp->next;
    }
    printf("\n");
}


struct connection * has_connection(struct Connections * connections, int socket){
    printf("Searching for existing connection, Socket: %d\n", socket);
    struct connection * head = connections->head;
    if(head == NULL) {
        printf("Connections is empty\n");
        return NULL;
    }
    struct connection * temp = head;
    while(temp != NULL) {
        int client_sock = temp->client_sock;
        int server_sock = temp->server_sock;
        printf("Client: %d, Server: %d\n",client_sock, server_sock);
        if(client_sock == socket) {
            printf("Connection found, is a client\n");
            prepend_connection(remove_connection(connections, client_sock, server_sock), connections);
            return temp;
        }else if (server_sock == socket) {
            printf("Connection found, is a server\n");
            prepend_connection(remove_connection(connections, client_sock, server_sock), connections);
            return temp;
        }
        temp = temp->next;
    }
    return NULL;
}

struct connection * remove_connection(struct Connections * connections,
int client_sock, int server_sock){
    if(connections == NULL) {
        return NULL;
    }
    printf("Removing connection of client %d, server %d\n", client_sock, server_sock);
    if(connections->head == NULL) {
        return NULL;
    }
    struct connection * temp = connections->head;
    while(temp != NULL) {
        if(temp->client_sock == client_sock && temp->server_sock == server_sock) {
            if(temp->prev != NULL) {
                temp->prev->next = temp->next;
            }else {
                connections->head = temp->next;
            }
            if(temp->next != NULL) {
                temp->next->prev = temp->prev;
            }
            return temp;
        }
        temp = temp->next;
    }
    return NULL;
}


void prepend_connection(struct connection * con, struct Connections * connections) {
    if(con == NULL || connections == NULL) {
        printf("You can't prepend Nothing\n");
        return;
    }
    printf("Prepending connection of client %d, server %d\n", con->client_sock, con->server_sock);
    if(connections->head == NULL) {
        printf("Successful prepend on empty list\n");
        connections->head = con;
        con->next = NULL;
        con->prev = NULL;
        connections->num_connections = 1;
        return;
    }
    struct connection * temp = connections->head;
    temp->prev = con;
    con->next = temp;
    con->prev = NULL;
    connections->head = con;
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

int add_tokens(struct Bucket * b){
    //struct timeval rawtime;
    //gettimeofday(&rawtime, NULL);
    time_t rawtime;
    rawtime = time(NULL); 
    int bucket_time = (rawtime - b->last_updated);
    int tokens = (BPS*bucket_time);
    tokens += b->tokens;
    if(tokens > BPS){
        tokens = BPS;
    }
    b->tokens = tokens;
    return tokens;
}
/*
int rate_limit(struct connection * con, int socket) {
    if(con == NULL) {
        return BPS;
    }else {
        time_t curr_time;
        curr_time = time(NULL);        
        int time_elapsed;
        if(socket == con->client_sock) {
            time_elapsed = curr_time - con->client_bucket.last_updated;
            con->client_bucket.tokens = min(BPS, con->client_bucket.tokens + time_elapsed * BPS);
            con->client_bucket.last_updated = curr_time;
            return con->client_bucket.tokens;
        }else if(socket == con->server_sock) {
            time_elapsed = curr_time - con->server_bucket.last_updated;
            con->server_bucket.tokens = min(BPS, con->server_bucket.tokens + time_elapsed * BPS);
            con->server_bucket.last_updated = curr_time;
            return con->client_bucket.tokens;
        }
    }
    return BPS;
} */

bool add_msg_buf(struct connection * con, char * buffer, int numbytes) {

    if(con == NULL || buffer == NULL || con->secure) {
        return false;
    }
    if(con->buffer == NULL) {
        con->buffer = malloc(sizeof(char) * numbytes);
        memcpy(con->buffer, buffer, numbytes);
        con->buf_size = numbytes;
        con->final_size = 0;
        
        //Search for header end
        char * end_head;
        char content_len [15] = "Content-Length:";
        char delim[] = "\n";

        if((end_head = strstr(con->buffer,"\r\n\r\n")) != NULL || 
                (end_head = strstr(con->buffer,"\n\n")) != NULL) {
            int header_size = end_head - con->buffer + 4;
            printf("Header Size: %d\n", header_size);
            //Extract content_size
            int cont_size = 0;
            char * search_buf =  (char *) malloc(10000000);
            memcpy(search_buf,con->buffer,con->buf_size);
            char * curr_line = strtok(search_buf, delim);
            while((curr_line = strtok(NULL, delim)) != NULL){
                printf("Search currline: %s\n", curr_line);
                if((memcmp(&curr_line[0],&content_len[0],15)) == 0){
                    char c;
                    int start_i = 16;
                    while((c =curr_line[start_i]) != ' ' && curr_line[start_i] != '\n' && curr_line[start_i] != '\r') {
                        cont_size *= 10;
                        cont_size += (c - 48);
                        start_i++;
                        printf("%d\n", cont_size);
                    }
                }
            }
            con->final_size =  header_size + cont_size;
            free(search_buf);
        }
    }else {
        con->buffer = realloc(con->buffer,con->buf_size + numbytes);
        memcpy(con->buffer + con->buf_size, buffer, numbytes);
        con->buf_size += numbytes;
        if(con->final_size == 0) {
            char * end_head;
            char content_len [15] = "Content-Length:";
            char delim[] = "\n";
            if((end_head = strstr(con->buffer,"\r\n\r\n")) != NULL || 
                (end_head = strstr(con->buffer,"\n\n")) != NULL) {
                int header_size = end_head - con->buffer + 4;
                printf("Header Size: %d\n", header_size);
                //Extract content_size
                int cont_size = 0;
                char * search_buf =  (char *) malloc(10000000);
                memcpy(search_buf,con->buffer,con->buf_size);
                char * curr_line = strtok(search_buf, delim);
                while((curr_line = strtok(NULL, delim)) != NULL){
                    printf("Search currline: %s\n", curr_line);
                    if((memcmp(&curr_line[0],&content_len[0],15)) == 0){
                        char c;
                        int start_i = 16;
                        while((c =curr_line[start_i]) != ' ' && curr_line[start_i] != '\n' && curr_line[start_i] != '\r') {
                            cont_size *= 10;
                            cont_size += (c - 48);
                            start_i++;
                            printf("%d\n", cont_size);

                        }
                    }
                }
                con->final_size =  header_size + cont_size;
                free(search_buf);
            }
        }
    }
    printf("Adding %d bytes to msg buf client %d\n", numbytes, con->client_sock);
    printf("Total so far: %d/%d\n", con->buf_size, con->final_size);
    if(con->buf_size >= con->final_size) {
        printf("Finished loading buffer of size %d\n", con->buf_size);
        return true;
    }else return false;
}

struct connection * new_connection(int client_sock, int server_sock,
bool secure) {
    //struct timeval now;
    //gettimeofday(&now, NULL);
    time_t curr_time;
    curr_time = time(NULL); 
    struct connection * temp = malloc(sizeof(struct connection));
    temp->client_sock = client_sock;
    temp->server_sock = server_sock;
    temp->buffer = NULL;
    temp->buf_size = 0;
    temp->final_size = 0;
    temp->next = NULL;
    temp->prev = NULL;
    temp->client_bucket.tokens = BPS;
    temp->client_bucket.last_updated = curr_time;
    temp->server_bucket.tokens = BPS;
    temp->server_bucket.last_updated = curr_time;

    if(secure) {
        temp->secure = true;
    }else temp->secure = false;
    return temp;
}
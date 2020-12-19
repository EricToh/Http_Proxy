/*
 *  proxy.c
 *  By Eric Toh and Collin Geary, 12/18/2020
 *  etoh01 & cgeary02
 *  Comp 112: Networks Final Project
 *
 *  Web Proxy 
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
const int GET = 1;
const int CON = 2;
const int TABLE_SIZE = 30;
const int TABLE_MAX_BYTES = 2000000;
const int BPS = 750000; //Bytes per Second
const int BLOCKED_SIZE = 10000;
const int BLOCKED_MAX = 100000;


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

struct blockedTable{
    int num_elements;
    int capacity;
    struct blockedNode ** table;
};

struct blockedNode{
    char host[1000];
    struct blockedNode* next;
    struct blockedNode*prev;
};



// FUNCTION DECLARATIONS
void client_message(struct Cache * cache, struct Connections * connections,
                    int curr_socket, char * buffer, fd_set * master_set, int numbytes, int * fdmax,
                    struct blockedTable * blockList);
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

//Rate limit functions
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

//Content Filter functions
void prepend_blocked_node(struct blockedTable * blockList, struct blockedNode * node);
struct blockedTable * create_blockList();
int search_blockList(struct blockedTable * blockList, char * host);
int search_blockedChain(struct blockedNode * head, char * host);

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
    //Head of block list
    struct blockedTable * blockList = create_blockList();
    if(argc == 3){
        //struct blockedTable * blockList = create_blockList();
        FILE *ptr;
        ptr = fopen(argv[2], "rb");
        int i = 0;
        while(1 == fread(buffer+i,1,1,ptr)){
            i++;
        }
        buffer[i] = '\0';
        char delim[] = "\n";
        char *curr_line = NULL;
        curr_line = strtok(buffer,delim);
        struct blockedNode * temp = (struct blockedNode*)malloc(sizeof(struct blockedNode));
        strcpy(temp->host, curr_line);
        prepend_blocked_node(blockList, temp);
        while((curr_line = strtok(NULL, delim)) != NULL){
            struct blockedNode * temp = (struct blockedNode*)malloc(sizeof(struct blockedNode));
            strcpy(temp->host, curr_line);
            prepend_blocked_node(blockList, temp);
        }
    }else{
        blockList = NULL;
    }


    // Get port number to bind to
    if (argc != 3 && argc != 2) {
        fprintf(stderr, "Wrong number of arguments\n");
        exit(EXIT_FAILURE);
    }
    server_port = atoi(argv[1]);

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
    while(1) {
        temp_set = master_set;
        select(*fdmax + 1, &temp_set, NULL, NULL, NULL); 

        // If main socket is expecting new connection
        if(FD_ISSET(master_sock, &temp_set)) {
            clilen = (size_t)sizeof(client_address);
            client_sock = accept(master_sock, (struct sockaddr *) &client_address, &clilen);
            if (client_sock < 0) {
                error ("accept");
            }
            FD_SET(client_sock, &master_set);
            *fdmax = max(client_sock,*fdmax);
        } else {

            for(curr_socket = 0; curr_socket <= *fdmax; curr_socket++) {
                if(FD_ISSET(curr_socket, &temp_set)) {
                    //Rate Limit
                    struct connection * con = has_connection(connections, curr_socket);
                    int tokens = rate_limit(con, curr_socket);

                    if(tokens == 0) {
                        continue;
                    }
                    
                    numbytes = recv(curr_socket, buffer, tokens, 0);
                    if(con != NULL) {
                        if(con->client_sock == curr_socket) {
                            con->client_bucket.tokens -= numbytes;
                        }else {
                            con->server_bucket.tokens -= numbytes;
                        }
                    }
                    if (numbytes < 0) {
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
                        struct connection * con = has_connection(connections, curr_socket);
                        if(con != NULL) {
                            remove_connection(connections, con->client_sock, con->server_sock);
                            close(con->client_sock);
                            close(con->server_sock);
                            FD_CLR(con->client_sock,&master_set);
                            FD_CLR(con->server_sock,&master_set);
                        } else {
                            close(curr_socket);
                            FD_CLR(curr_socket, &master_set); 
                        }
                    }else{
                        client_message(cache, connections, curr_socket, buffer, &master_set, numbytes, fdmax, blockList);
                    }
                }
            }
        }
    }
}

// Function to deal with the client's message
void client_message(struct Cache * cache, struct Connections * connections,
                    int curr_socket, char * buffer, fd_set * master_set, int numbytes, int * fdmax,
                    struct blockedTable * blockList){
    struct connection * con = has_connection(connections, curr_socket);
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
                if(cache->num_bytes > ((double)cache->capacity)/2){
                    remove_stale(cache);
                }
                if(temp->size > cache->capacity) {
                    write(partner_con, con->buffer, con->buf_size);
                    free (timebuf);
                    free(temp->object);
                    free(temp);
                    return;
                }
                while((cache->num_bytes + temp->size) > cache->capacity){
                    struct Node_Size * remove = pop_largest(cache);
                    remove_cache_node(cache, remove->node);
                    free(remove);
                }
                prepend_cache_node(cache,temp);
                add_to_size_list(cache, temp);
                int n = write(partner_con, con->buffer, con->buf_size);
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
    bzero(newbuffer, OBJECT_MAX);
    memcpy(newbuffer, buffer, numbytes);
    curr_line = strtok(newbuffer,delim);
        
    int status = 0;
    int host_status = 0;
    if(curr_line[0] == 'G'&& curr_line[1] == 'E' && curr_line[2] == 'T'){
        strcpy(headerGET, curr_line);
        status = GET;
    }
    if(curr_line[0] == 'C'&& curr_line[1] == 'O' && curr_line[2] == 'N'){
        strcpy(headerGET, curr_line);
        status = CON;
    }
    if(curr_line[0] == 'H' && curr_line[1] == 'o' && curr_line[2] == 's'){
        host_status = 1;
        strcpy(headerHost, curr_line);
    }
    while((curr_line = strtok(NULL, delim)) != NULL){
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
                host_status = 1;
            strcpy(headerHost, curr_line);
        }
    }

    if(host_status == 1){
        strcpy(cpyhost, headerHost);
        host_name = strtok(cpyhost, ":");
        host_name = strtok(NULL, ":");
        host_name = strtok(NULL, ":");
        if(host_name != NULL){
            webport = atoi(host_name);
        }else{
            webport = 80;
        }

        bzero(cpyhost, 100);
        strcpy(cpyhost, headerHost);
        host_name =  strtok(cpyhost, " ");
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

        if(search_blockList(blockList, tempname) == 1){
            close(curr_socket);
            FD_CLR(curr_socket, master_set);
            return;
        }
    }

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
        close(curr_socket);
        FD_CLR(curr_socket, master_set);
    }
}

struct Node * search_chain(struct Node * head, char * key) {
    if(head == NULL) {
        return NULL;
    }
    struct Node * temp = head;
    while(temp != NULL) {
        if(strcmp(key, temp->key) == 0) {
            return temp;
        }
        temp = temp->next;
    }
    return NULL;
}

struct Node * search_cache(struct Cache * cache, char * key) {
    int hash_val = (int)(hash(key)%TABLE_SIZE);
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
    struct hostent *server;
    struct sockaddr_in serveraddr;
    time_t rawtime;
    //Check if already in cache
    rawtime = time (NULL);
    int update = 0;
    struct Node * cache_hit = search_cache(cache, headerGET);
    if(cache_hit != NULL) {
        // Checks if it is correct port and if it has expired
        if((cache_hit->age + cache_hit->time) > rawtime &&
        cache_hit->port == webport){
            chain_front(cache, cache_hit);
            write(curr_socket, cache_hit->object, cache_hit->size);
            close(curr_socket);
            FD_CLR(curr_socket, master_set);
            return;
        }else {
            // Remove expired object
            if((cache_hit->age + cache_hit->time) <= rawtime) {
                remove_from_size_list(cache, cache_hit->key);
                remove_cache_node(cache, cache_hit);
            }
        }
    }

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
        return;
    }
  
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

    struct connection * con= new_connection(curr_socket, clientfd, false);
    strcpy((char *)con->key, headerGET);
    con->port = webport;
    prepend_connection(con, connections);
    *fdmax = max(clientfd,*fdmax);
    FD_SET(clientfd, master_set);
 
}

void proxy_https(int curr_socket,char * buffer, int numbytes, int webport, char * host_name, 
                 char * headerHost, struct Connections * connections, fd_set * master_set, int * fdmax){
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
        return;
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

    // Creating connection success message
    char bigbuf[100] = "HTTP/1.1 200 Connection established\r\n\r\n\0";
    int n = write(curr_socket, bigbuf, strlen(bigbuf));

    struct connection * new_con = new_connection(curr_socket, server_sock, true);

    prepend_connection(new_con, connections);

    *fdmax = max(server_sock,*fdmax);
    FD_SET(server_sock, master_set);
}

void secure_stream(int socket, int socket_partner, char * buffer, int numbytes){
    int n = write(socket_partner, buffer, numbytes);
}


void print_connection(struct connection * head) {
    if(head == NULL) {
        return;
    }
    struct connection * temp = head;
    while(temp != NULL) {
        temp = temp->next;
    }
}


struct connection * has_connection(struct Connections * connections, int socket){
    struct connection * head = connections->head;
    if(head == NULL) {
        return NULL;
    }
    struct connection * temp = head;
    while(temp != NULL) {
        int client_sock = temp->client_sock;
        int server_sock = temp->server_sock;
        if(client_sock == socket) {
            prepend_connection(remove_connection(connections, client_sock, server_sock), connections);
            return temp;
        }else if (server_sock == socket) {
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
            connections->num_connections -= 1;
            return temp;
        }
        temp = temp->next;
    }
    return NULL;
}


void prepend_connection(struct connection * con, struct Connections * connections) {
    if(con == NULL || connections == NULL) {
        return;
    }
    if(connections->head == NULL) {
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
    if(cache == NULL || cache->table == NULL || node == NULL) {
        return;
    }
    int hash_val = (int)(hash(node->key)%TABLE_SIZE);
    struct Node * temp = cache->table[hash_val];
    if(temp == NULL) {
        cache->table[hash_val] = node;
        node->prev = NULL;
        node->next = NULL;
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
}

// Moves a cache node to the front of its chain
void chain_front(struct Cache * cache, struct Node * node) {
    if(cache == NULL || cache->table == NULL || node == NULL) {
        return;
    }
    int hash_val = (int)(hash(node->key)%TABLE_SIZE);
    struct Node * temp = cache->table[hash_val];
    if(temp == node) {
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
}

// Removes all stale objects in the cache
void remove_stale(struct Cache * cache) {
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

    struct Node_Size * new = malloc(sizeof(struct Node_Size));
    strcpy(new->key, node->key);

    new->size = node->size;
    new->node = node;

    if(cache->largest == NULL) {
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
            //Extract content_size
            int cont_size = 0;
            char * search_buf =  (char *) malloc(10000000);
            memcpy(search_buf,con->buffer,con->buf_size);
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
                //Extract content_size
                int cont_size = 0;
                char * search_buf =  (char *) malloc(10000000);
                memcpy(search_buf,con->buffer,con->buf_size);
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
                con->final_size =  header_size + cont_size;
                free(search_buf);
            }
        }
    }
    if(con->buf_size >= con->final_size) {
        return true;
    }else return false;
}

struct connection * new_connection(int client_sock, int server_sock,
bool secure) {
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

int rate_limit(struct connection * con, int socket) {
    if(con == NULL) {
        return BPS;
    }else {
        time_t curr_time;
        curr_time = time(NULL);        
        int time_elapsed;
        if(socket == con->client_sock) {
            time_elapsed = ((int)curr_time) - (int)(con->client_bucket.last_updated);
            con->client_bucket.tokens = min(BPS, con->client_bucket.tokens + (time_elapsed * BPS));
            con->client_bucket.last_updated = curr_time;
            return con->client_bucket.tokens;
        }else if(socket == con->server_sock) {
            time_elapsed = ((int)curr_time) - (int)(con->server_bucket.last_updated);
            con->server_bucket.tokens = min(BPS, con->server_bucket.tokens + time_elapsed * BPS);
            con->server_bucket.last_updated = curr_time;
            return con->server_bucket.tokens;
        }
    }
    return BPS;
}

// Creates block list data structure
struct blockedTable * create_blockList() {
    struct blockedTable * blockList = malloc(sizeof(struct blockedTable));
    blockList->num_elements = 0;
    blockList->capacity = BLOCKED_MAX;
    blockList->table = malloc(BLOCKED_SIZE * sizeof(struct blockedNode *));
    for(int i = 0; i < BLOCKED_SIZE; i++) {
        blockList->table[i] = NULL;
    }
    return blockList;
}

// Prepends a blockednode in its correct cache slot
void prepend_blocked_node(struct blockedTable * blockList, struct blockedNode * node) {
    if(blockList == NULL || blockList->table == NULL || node == NULL) {
        return;
    }
    int hash_val = (int)(hash(node->host)%BLOCKED_SIZE);
    struct blockedNode * temp = blockList->table[hash_val];
    if(temp == NULL) {
        blockList->table[hash_val] = node;
        node->prev = NULL;
        node->next = NULL;
        blockList->num_elements += 1;
        return;
    }
    blockList->table[hash_val] = node;
    temp->prev = node;
    node->next = temp;
    node->prev = NULL;
    blockList->num_elements += 1;
}

int search_blockList(struct blockedTable * blockList, char * host) {
    if(blockList == NULL){
        return 0;
    }
    int hash_val = (int)(hash(host)%BLOCKED_SIZE);
    int found = search_blockedChain(blockList->table[hash_val], host);
    if(found == 1) {
        return 1;
    }else {
        return 0;
    }
}

int search_blockedChain(struct blockedNode * head, char * host) {
    if(head == NULL) {
        return 0;
    }
    struct blockedNode * temp = head;
    while(temp != NULL) {
        if(strcmp(host, temp->host) == 0) {
            return 1;
        }
        temp = temp->next;
    }
    return 0;
}
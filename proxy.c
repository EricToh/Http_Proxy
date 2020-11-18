/*
 *  proxy.c
 *  By Eric Toh and Collin Geary, 11/17/2020
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
const int OBJECT_MAX = 10000;
const int CACHE_SIZE = 10;


//STRUCTURES
struct Node{
    char key[100];
    char object[10000000];
    int time;
    int age;
    int port;
    int size;
    struct Node* next;
};

struct connection {
    int client_sock;
    int server_sock;
    struct connection * next;
};

// FUNCTION DECLARATIONS
void client_message(struct Node ** head, struct connection ** c_head,
                    int curr_socket, char * buffer, fd_set * master_set, 
                    int numbytes, int * size);
void proxy_http(struct Node ** head, int curr_socket, char * buffer,
                fd_set * master_set, int numbytes, int webport,
                char * host_name, char * headerGET, char * headerHost,
                int * size);
void proxy_https(int curr_socket,char * buffer, int numbytes, int webport, char * host_name, 
                 char * headerHost, struct connection ** c_head);
void print_list(struct Node * head);
int has_connection(struct connection ** c_head, int socket);
void secure_stream(int curr_socket, int server_con, char * buffer, int numbytes);


//void proxy_https();

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

//MAIN
int main (int argc, char *argv[]) {
    fprintf(stderr, "START\n");
    int master_sock, client_sock, server_port, curr_socket, numbytes;
    int size = 0;
    struct sockaddr_in server_address, client_address;
    socklen_t clilen;
    printf("BEFORE BUFFER\n");
    char * buffer = malloc(OBJECT_MAX);
    struct Node* head = NULL;

    // Create Connections data structure
    struct connection * connections_head = NULL;

    // Create Cache

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

    int optval = 1;
    setsockopt(master_sock, SOL_SOCKET, SO_REUSEADDR,
               (const void *)&optval , sizeof(int));
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
    int fdmax = master_sock;

    //Main Loop for select and accepting clients
    printf("Entering main loop for select\n");
    while(1) {
        if(head == NULL)
            printf("HEAD is NULL\n");
        if(head != NULL)
            printf("HEAD is NOT NULL\n");
        temp_set = master_set;
        select(fdmax + 1, &temp_set, NULL, NULL, NULL); 

        // If main socket is expecting new connection
        if(FD_ISSET(master_sock, &temp_set)) {
            printf("New connection from client\n");
            clilen = (size_t)sizeof(client_address);
            client_sock = accept(master_sock, (struct sockaddr *) &client_address, &clilen);
            if (client_sock < 0) {
                error ("accept");
            }
            FD_SET(client_sock, &master_set);
            fdmax = max(client_sock,fdmax);
        } else {
            // Data from already connected socket
            printf("Data from already connected client\n");

            for(curr_socket = 0; curr_socket <= fdmax; curr_socket++) {
                if(FD_ISSET(curr_socket, &temp_set)) {
                    // printf("Socket %d is set\n",curr_socket);
                    numbytes = recv(curr_socket, buffer, OBJECT_MAX-1, 0);
                    if (numbytes < 0) {
                        FD_CLR(curr_socket, &master_set);
                    }else if (numbytes == 0) {
                        printf("Client has left in orderly conduct\n");
                        FD_CLR(curr_socket, &master_set);
                        //remove_client(clients, curr_socket, &master_set);
                    }else{
                        // printf("\nRecieved client message of size %d from socket %d\n", numbytes, curr_socket);
                        client_message(&(head), &connections_head, curr_socket, buffer, &master_set, numbytes, &size);
                    }
                }
            }
        }
    }
}


// Function to deal with the client's message
void client_message(struct Node ** head, struct connection ** c_head,
                    int curr_socket, char * buffer, fd_set * master_set, 
                    int numbytes, int * size){
    printf("Entered client message\n");

    int server_con = has_connection(c_head, curr_socket);
    if(server_con != -1) {
        secure_stream(curr_socket, server_con, buffer, numbytes);
        printf("return from secure_stream\n");
        return;
    }

    char cpyhost[100];
    char headerGET[100];
    char headerHost[100];
    int webport;
    char *curr_line = NULL;
    char *host_name = NULL;
    char delim[] = "\n";
    char * newbuffer = malloc(sizeof(char) * OBJECT_MAX);
     
    //Find the GET field and the Host field and separate them
    bzero(newbuffer, OBJECT_MAX);
    memcpy(newbuffer, buffer, numbytes);
    curr_line = strtok(newbuffer,delim);
        
    int connectR = 0;
    if(curr_line[0] == 'G'&& curr_line[1] == 'E' && curr_line[2] == 'T'){
        strcpy(headerGET, curr_line);
    }
    if(curr_line[0] == 'C'&& curr_line[1] == 'O' && curr_line[2] == 'N'){
        strcpy(headerGET, curr_line);
        connectR = 1;
    }
    if(curr_line[0] == 'H' && curr_line[1] == 'o' && curr_line[2] == 's'){
        strcpy(headerHost, curr_line);
    }
    while((curr_line = strtok(NULL, delim)) != NULL){
        if(curr_line[0] == 'G' && curr_line[1] == 'E' && 
            curr_line[2] == 'T'){
            strcpy(headerGET, curr_line);
        }
        if(curr_line[0] == 'C' && curr_line[1] == 'O' && 
            curr_line[2] == 'N'){
            strcpy(headerGET, curr_line);
            connectR = 1;
            }
        if(curr_line[0] == 'H' && curr_line[1] == 'o' && 
            curr_line[2] == 's'){
            strcpy(headerHost, curr_line);
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
    if(connectR == 1){
        proxy_https(curr_socket, buffer, numbytes, webport, host_name, headerHost, c_head);
        return;
    }else{
        proxy_http(head, curr_socket, buffer, master_set, numbytes, webport,
                     host_name, headerGET, headerHost, size);
        close(curr_socket);
        FD_CLR(curr_socket, master_set);
    }
}

void proxy_http(struct Node ** head, int curr_socket, char * buffer,
                fd_set * master_set, int numbytes, int webport,
                char * host_name, char * headerGET, char * headerHost, 
                int * size) {
    printf("Entered proxy http\n");
    struct Node* temp = NULL;
    struct Node* iterate = NULL;
    struct Node* before_iterate = NULL;
    char *curr_line;
    struct hostent *server;
    struct sockaddr_in serveraddr;
    time_t rawtime;
    //Check if already in cache
    rawtime = time (NULL);
    iterate = *(head);
    int update = 0;
    printf("Searching cache\n");
    printf("cache Size: %d\n", *(size));
    for(int i=0; i<*(size); i++){
        if(iterate == NULL)
            printf("FUCK\n");
        printf("cache for loop\n");
        printf("headerGET: %s\n", headerGET);
        printf("iterate->key: %s\n", iterate->key);
        if(strcmp(headerGET, iterate->key) == 0 && 
            (iterate->age + iterate->time) > rawtime &&
            iterate->port == webport){
            printf("Found in cache\n");
            char * objcopy = (char *) malloc(10000000);
            char agestring[100] = "Age: ";
            char agenumber[95];
            sprintf(agenumber, "%ld", (rawtime - iterate->time));
            strcat(agestring, agenumber);
                
            //finds where to insert the age
            char *placeholder;
            placeholder = strchr(iterate->object, '\n');
            int placement_index = placeholder - iterate->object;
            int age_len = strlen(agestring);

            //Inserts it in
            memcpy(objcopy, iterate->object, placement_index+1);
            memcpy(objcopy + placement_index + 1, agestring, age_len);
            memcpy(objcopy + placement_index + age_len + 1,
                   iterate->object + placement_index, 
                   iterate->size - placement_index);
            update = 1;

            if(i == 0){
                //If first item in cache
                write(curr_socket, objcopy, iterate->size + age_len + 1);
            }else{
                before_iterate->next = iterate->next;
                iterate->next = *(head);
                *(head) = iterate;
                write(curr_socket, objcopy, iterate->size + age_len + 1);
            }
            break;
        }
        before_iterate = iterate;
        iterate = iterate->next;
    }
    printf("Done Searching cache\n");

        
    //Skips Contacting the webserver if request already in Cache
    if(update == 1)
        return;
    printf("Creating new cache object\n");
    //Create a node;
    temp = (struct Node*)malloc(sizeof(struct Node));
    strcpy(temp->key, headerGET);
    //Create socket for web server
    int clientfd;
    clientfd = socket(AF_INET, SOCK_STREAM, 0);
    int optval = 1;
    setsockopt(clientfd, SOL_SOCKET, SO_REUSEADDR, 
	            (const void *)&optval , sizeof(int));
        
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
    int count;
    int contentsize = 0;
    while((count = read(clientfd, bufchunk, 10000000)) > 0){
        memcpy(bigbuf + contentsize, bufchunk, count);
        contentsize = contentsize + count;
    }
    printf("Finished reading from server\n");

    close(clientfd);
    memcpy(temp->object, bigbuf, contentsize);
    temp->size = contentsize;

    //Add time and age to cache
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
    if(*(size) < 10){
        printf("checking for copy");
        //Check for stale copy
        iterate = *(head);
        for(int i=0; i< *(size); i++){
            if(strcmp(temp->key, iterate->key) == 0 &&
                temp->port == iterate->port){
                before_iterate->next = iterate->next;
                free(iterate);
                *(size) -= 1;
                break;
            }
            before_iterate = iterate;
            iterate = iterate->next;
        }
        temp->next = *(head);
        *(head) = temp;
        if(*(head) != NULL)
            printf("GOOD SHIT\n");
        *(size) += 1;
    }else{
        int stale = 0;
        iterate = *(head);
        //Check for any stale info
        printf("Checking for stale info\n");
        for(int i=0; i< *(size); i++){
            if((iterate->age + iterate->time) < rawtime){
                if(i == 0){
                    *(head) = temp;
                    temp->next = iterate->next;
                    free(iterate);
                }else{
                    before_iterate->next = iterate->next;
                    temp->next = *(head);
                    *(head) = temp;
                    free(iterate);
                }
                stale = 1;
                break;
            }
            before_iterate = iterate;
            iterate = iterate->next;
        }
        //If no stale elements
        if(stale == 0){
            printf("Removing oldest\n");
            iterate = *(head);
            while(iterate->next != NULL){
                before_iterate = iterate;
                iterate = iterate->next;
            }
            before_iterate->next = NULL;
            free(iterate);
            temp->next = *(head);
            *(head) = temp;
        }
    }
        

    //Send reply to Client
    write(curr_socket, bigbuf, contentsize);
    free(bigbuf);
}

void proxy_https(int curr_socket,char * buffer, int numbytes, int webport, char * host_name, 
                 char * headerHost, struct connection ** c_head){
    printf("HTTPS\n");
    int clientfd;
    struct hostent *server;
    struct sockaddr_in serveraddr;

    clientfd = socket(AF_INET, SOCK_STREAM, 0);
    /*int optval = 1;
    setsockopt(clientfd, SOL_SOCKET, SO_REUSEADDR, 
	            (const void *)&optval , sizeof(int));*/
        
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
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(clientfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

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
    printf("Client Request: \n");
    printf("%s\n", buffer);
    //write(clientfd, buffer, strlen(buffer));

    //Read Server Reply
    /*printf("Reading server reply\n");
    char * bigbuf = (char *) malloc(10000000);
    char * bufchunk = (char *) malloc(10000000);
    
    bzero(buffer, OBJECT_MAX);
    bzero(bigbuf, 10000000);
    int count;
    int contentsize = 0;
    while((count = read(clientfd, bufchunk, 10000000)) > 0){
        memcpy(bigbuf + contentsize, bufchunk, count);
        contentsize = contentsize + count;
    } */

    char bigbuf[100] = "HTTP/1.1 200 Connection established\r\n\r\n\0";
    printf("Server Response: \n");
    printf("%s\n", bigbuf);
    write(curr_socket, bigbuf, strlen(bigbuf));

    struct connection* temp = NULL;
    temp = (struct connection*)malloc(sizeof(struct connection));
    temp->client_sock = curr_socket;
    temp->server_sock = clientfd;
    temp->next = *(c_head);
    *(c_head) = temp;
}

void print_list(struct Node * head) {
    if(head == NULL) {
        printf("This specific list is empty\n");
        return;
    }
    struct Node * temp = head;
    while(temp != NULL) {
        printf(" -> ");
        printf("%s", temp->key);
    }
    printf("\n");
}

int has_connection(struct connection ** c_head, int socket) {
    if(*(c_head) == NULL) {
        return -1;
    }
    struct connection * temp = *(c_head);
    while(temp != NULL) {
        if(temp->client_sock == socket) {
            return temp->server_sock;
        }
    }
    return -1;
}

void secure_stream(int curr_socket, int server_con, char * buffer, int numbytes){
    printf("Secure Stream\n");
    write(server_con, buffer, numbytes);
    char * bigbuf = (char *) malloc(10000000);
    char * bufchunk = (char *) malloc(10000000);
    
    bzero(buffer, OBJECT_MAX);
    bzero(bigbuf, 10000000);
    int count;
    int contentsize = 0;
    while((count = read(server_con, bufchunk, 10000000)) > 0){
        printf("reading from server\n");
        memcpy(bigbuf + contentsize, bufchunk, count);
        contentsize = contentsize + count;
        printf("COUNT: %d\n", count);
        bzero(bufchunk, 10000000);
    }
    printf("out of while\n");
    write(curr_socket, bigbuf, contentsize);
    free(bigbuf);
    printf("done with ss\n");
}

/*
void append_connection(int client_sock, int server_sock, struct connection ** c_head) {
    struct connection * new_connection = malloc(sizeof(struct connection));
    new_connection->client_sock = client_sock;
    new_connection->server_sock = server_sock;
    if(*(c_head) == NULL) {
        *chead = new_connection;
        
    }
} */
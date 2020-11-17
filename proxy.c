/*
 *  Collin Geary
 *  Advanced HTTP Proxy
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>

#define MAXLINE 8192
#define MAXBUF 8192

struct Node{
    char key[100];
    char object[10000000];
    int time;
    int age;
    int port;
    int size;
    struct Node* next;
};

int main(int argc, char *argv[])
{
    int port, webport;
    char buffer[MAXBUF];
    char newbuffer[MAXBUF];
    char headerGET[100];
    char headerHost[100];
    char *curr_line = NULL;
    char *host_name = NULL;
    char delim[] = "\n";
    struct hostent *server;
    struct sockaddr_in serveraddr;
    struct Node* head = NULL;
    struct Node* temp = NULL;
    struct Node* iterate = NULL;
    struct Node* before_iterate = NULL;
    int size = 0;
    time_t rawtime;

    port = atoi(argv[1]);
    //create the server socket
    int server_socket;
    server_socket = socket(AF_INET, SOCK_STREAM,0);
     
    int optval = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, 
	     (const void *)&optval , sizeof(int));
    // define the server address
    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    server_address.sin_addr.s_addr = INADDR_ANY;

    //bind
    bind(server_socket, (struct sockaddr *) &server_address, 
         sizeof(server_address));
    
    listen(server_socket, 5);
    
    while(1){
        bzero(buffer, MAXLINE);
        struct sockaddr_in client_address;
        socklen_t clilen = sizeof(client_address);
        int newsocket;
        newsocket = accept(server_socket, (struct sockaddr *) &client_address, 
                       &clilen);
        //Read Client Header
        int n = -1;
        int yeet = 0;
        //while(n != 0){
            n = read(newsocket, buffer+yeet, MAXBUF);
            yeet = n + yeet;
        //}
        printf("%s\n", buffer);
        
        //Find the GET field and the Host field and separate them
        bzero(newbuffer, MAXLINE);
        strcpy(newbuffer, buffer);
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
        
        char cpyhost[100];
        strcpy(cpyhost, headerHost);
        host_name = strtok(cpyhost, ":");
        host_name = strtok(NULL, ":");
        host_name = strtok(NULL, ":");
        if(host_name != NULL){
            webport = atoi(host_name);
        }else{
            webport = 80;
        }
        //Check if already in cache
        rawtime = time (NULL);
        iterate = head;
        int update = 0;
        for(int i=0; i<size; i++){
            if(strcmp(headerGET, iterate->key) == 0 && 
                      (iterate->age + iterate->time) > rawtime &&
                       iterate->port == webport){
                char * objcopy = (char *) malloc(10000000);
                char agestring[100] = "Age: ";
                char agenumber[95];
                sprintf(agenumber, "%d", (rawtime - iterate->time));
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
                    write(newsocket, objcopy, iterate->size + age_len + 1);
                    close(newsocket);
                }else{
                    before_iterate->next = iterate->next;
                    iterate->next = head;
                    head = iterate;
                    write(newsocket, objcopy, iterate->size + age_len + 1);
                    close(newsocket);
                }
                    break;
            }
            before_iterate = iterate;
            iterate = iterate->next;
        }
        
        //Skips Contacting the webserver if request already in Cache
        if(update == 1)
            continue;
        //Create a node;
        temp = (struct Node*)malloc(sizeof(struct Node));
        strcpy(temp->key, headerGET);
        //Create socket for web server
        int clientfd;
        clientfd = socket(AF_INET, SOCK_STREAM, 0);
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
        char * bigbuf = (char *) malloc(10000000);
        char * bufchunk = (char *) malloc(10000000);
        //char tempbuf[MAXBUF];
        bzero(buffer, MAXBUF);
        bzero(bigbuf, 10000000);
        int count;
        int contentsize = 0;
        while((count = read(clientfd, bufchunk, 10000000)) > 0){
            memcpy(bigbuf + contentsize, bufchunk, count);
            contentsize = contentsize + count;
        }
        
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

        if(connectR == 0){
        //Add node to cache
        if(size < 10){
            //Check for stale copy
            iterate = head;
            for(int i=0; i<size; i++){
                if(strcmp(temp->key, iterate->key) == 0 &&
                    temp->port == iterate->port){
                    before_iterate->next = iterate->next;
                    free(iterate);
                    size--;
                    break;
                }
                before_iterate = iterate;
                iterate = iterate->next;
            }
            temp->next = head;
            head = temp;
            size++;
        }else{
            int stale = 0;
            iterate = head;
            //Check for any stale info
            for(int i=0; i<size; i++){
                if((iterate->age + iterate->time) < rawtime){
                    if(i == 0){
                        head = temp;
                        temp->next = iterate->next;
                        free(iterate);
                    }else{
                        before_iterate->next = iterate->next;
                        temp->next = head;
                        head = temp;
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
                iterate = head;
                while(iterate->next != NULL){
                     before_iterate = iterate;
                     iterate = iterate->next;
                }
                before_iterate->next = NULL;
                free(iterate);
                temp->next = head;
                head = temp;
            }
        }
        }

        //Send reply to Client
        write(newsocket, bigbuf, contentsize);
        close(newsocket);
        free(bigbuf);
    }
}

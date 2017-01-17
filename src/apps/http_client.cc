#include "minet_socket.h"
#include <stdlib.h>
#include <ctype.h>
#include "my_error.h"

#define BUFSIZE 1024
#define LENGTH_KEY "Content-Length: "
#define LENGTH_KEY_LEN sizeof(LENGTH_KEY)
#define END_HEADERS "\r\n\r\n"
#define END_HEADERS_LEN sizeof(END_HEADERS)
#define READ_MORE_DATA -1
#define LENGTH_NOT_FOUND -2

int write_n_bytes(int fd, char * buf, int count);
int read_n_bytes(int fd, char * buf, int count);
int check_status_line(char* buf);
void build_get_request(char *buf, char *server_path, char* port_no);
int parse_headers(char *buf, int *buf_pos, int datalen);
int read_length(char* buf, int* bodylen_acc, int* buf_pos, int datalen);
void flush_buffer(FILE* wheretoprint, char * buf, int* buf_pos, int* datalen);

/* TODOS:
 * Do I need to fix the byte order on what I get back from read?
 * Surely not... How can I know where the data boundaries are?
 */

int main(int argc, char * argv[]) {
        char * server_name = NULL;
        int server_port = 0;
        char * server_path = NULL;

        int sock = 0;
        int datalen = 0;
        int bodylen = 0;
        int bodylen_acc = 0;
        int length_pos = 0;
        struct sockaddr_in sa;
        struct hostent *site;
        FILE * wheretoprint = stdout;
        int status = -1;
        int rc = 0;

        char buf[BUFSIZE + 1];
        buf[BUFSIZE] = NULL;
        int buf_pos = 0;
        struct timeval timeout;
        fd_set set;

        /*parse args */
        if (argc != 5) {
                fprintf(stderr, "usage: http_client k|u server port path\n");
                exit(-1);}

        server_name = argv[2];
        server_port = atoi(argv[3]);
        printf("PORT IS: %d\n", server_port);
        server_path = argv[4];
        printf("SERVER IS: %s\n", server_name);


        /* initialize minet */
        if(toupper(*(argv[1])) == 'K') {
                minet_init(MINET_KERNEL);
        } else if (toupper(*(argv[1])) == 'U'){
                minet_init(MINET_USER);
        } else{
                fprintf(stderr, "First argument must be k or u\n");
                exit(-1);
        }
        if((sock = minet_socket(SOCK_STREAM)) < 0){
                my_error_at_line(sock, 0, "minet_socket", __FILE__, __LINE__);
                goto bad;
        }

        printf("BOOTED MINET\n");

        // Do DNS lookup
        /* Hint: use gethostbyname() */
        if((site = gethostbyname(server_name)) == 0){
                status = errno;
                my_error_at_line(status, 1, "gethostbyname", __FILE__, __LINE__);
                goto bad;
        }
        printf("DNS LOOKUP WENT OK\n");

        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(server_port);
        sa.sin_addr.s_addr = * ((unsigned long *) site->h_addr_list[0]);

        printf("WAS ABLE TO PACK STRUCT\n");

        printf("CONNECTING\n\n");

        /* connect socket */
        if((status = minet_connect(sock, &sa)) < 0){
                printf("WHAT THE ACTUAL FUCK\n");
                status = errno;
                my_error_at_line(status, 1, "minet_connect", __FILE__, __LINE__);
                exit(-1); // no need to close socket, never allocated.
        }
        printf("CONNECTED");

        // Wait for socket to be writable
        FD_ZERO(&set);
        FD_SET(sock, &set);

        timeout.tv_sec = 5;
        timeout.tv_usec = 0;

        while(!FD_ISSET(sock, &set)){
                if(minet_select(sock + 1, NULL, &set, NULL, &timeout) < 0){
                        my_error_at_line(status, 1, "minet_select", __FILE__, __LINE__);
                        goto bad;
                }
        }

        // Build and send request
        build_get_request(buf, argv[3], server_path);
        if((minet_write(sock, buf, strlen(buf))) < 0){
                my_error_at_line(status, 1, "minet_write", __FILE__, __LINE__);
                goto bad;}

        /* wait till socket can be read */
        /* Hint: use select(), and ignore timeout for now. */
        FD_ZERO(&set);
        FD_SET(sock, &set);


        while(!FD_ISSET(sock, &set)){
                if(minet_select(sock + 1, &set, NULL, NULL, &timeout) < 0){
                        my_error_at_line(status, 1, "minet_select", __FILE__, __LINE__);
                        goto bad ;
                }
        }

        // get to headers
        buf_pos = 1; // offset for headers
        datalen = 0;
        while(1){
                datalen += read_n_bytes(sock, buf, BUFSIZE - datalen);

                while(datalen >= 2 && buf_pos <  datalen){
                        if((buf[buf_pos - 1] == '\r' &&
                            buf[buf_pos]     == '\n')){
                                break;
                        }
                        buf_pos++;
                }
        }
        if(check_status_line(buf) < 0){
                wheretoprint = stderr;
                rc = -1;
        }

        // move buf_pos to beginning of first header line
        buf_pos++;
        length_pos = READ_MORE_DATA;
        // parse headers
        while(1 &&
              length_pos == READ_MORE_DATA){
                printf("somehow down here: %d\n\n", datalen);

                datalen += read_n_bytes(sock, buf + datalen, BUFSIZE - datalen);

                if(datalen >= (int) LENGTH_KEY_LEN){
                        length_pos = parse_headers(buf, &buf_pos, datalen);
                }

                if(datalen >= BUFSIZE){
                        flush_buffer(wheretoprint, buf, &buf_pos, &datalen);
                }
        }

        if(length_pos != LENGTH_NOT_FOUND){
                my_error_at_line(status, 1, "Didn't find length", __FILE__, __LINE__);
                goto bad; // XXX: What should I do here?
        }
        // parse in length field
        else{
                bodylen_acc = 0;
                bodylen = READ_MORE_DATA;
                buf_pos = length_pos + 1;
                while(1 &&
                      bodylen == READ_MORE_DATA){

                        datalen += read_n_bytes(sock, buf + datalen, BUFSIZE - datalen);

                        bodylen = read_length(buf, &bodylen_acc, &buf_pos, datalen);

                        if(datalen >= BUFSIZE){
                                flush_buffer(wheretoprint, buf, &buf_pos, &datalen);
                        }
                }
        }

        flush_buffer(wheretoprint, buf, &buf_pos, &datalen);

        // print until one bufferful left
        // buf_pos is irrelevant, we're done parsing (YAY!)
        while(datalen < bodylen - BUFSIZE){
                datalen += read_n_bytes(sock, buf + datalen, BUFSIZE - datalen);
                if(datalen >= BUFSIZE){
                        flush_buffer(wheretoprint, buf, &buf_pos, &datalen);

                }
        }
        flush_buffer(wheretoprint, buf, &buf_pos, &datalen);

        while(datalen < BUFSIZE){
                datalen += read_n_bytes(sock, buf + datalen, BUFSIZE - datalen);
        }

        /*close socket and deinitialize */
        minet_close(sock);


        return rc;

 bad:
        minet_close(sock);
        return -1;
}

int read_length(char* buf, int* bodylen_acc, int* buf_pos, int datalen){
        while(1){
                if(*buf_pos >= datalen){
                        return READ_MORE_DATA;
                }

                *bodylen_acc += (*bodylen_acc) * 10 + ((int) (buf[*buf_pos] - '0'));
                *buf_pos++;

                if(strncmp(END_HEADERS, buf + *buf_pos - END_HEADERS_LEN, END_HEADERS_LEN)){
                        return *bodylen_acc;
                }
        }
}

void flush_buffer(FILE* wheretoprint, char * buf, int* buf_pos, int* datalen){
        fprintf(wheretoprint, "%s", buf);
        *buf_pos = 0;
        *datalen = 0;
}

// Loads a get request into buf for server_path on port_no
// request is null terminated.
void build_get_request(char *buf, char *server_port, char *server_path){
        strcat(buf, "GET ");

        strcat(buf, server_path);
        strcat(buf, " ");

        strcat(buf, server_port);

        strcat(buf, "\r\n");
}


#define STATUS_LENGTH 4
#define HTTP_SUCCESS 200
int check_status_line(char* buf){
        char status_buf[STATUS_LENGTH];
        memset(status_buf, '\0', STATUS_LENGTH);

        int i = 0;
        for(int i = 0; buf[i] != ' '; i++);
        i += 1; //gets us onto the status code
        for(int j = 0; j < STATUS_LENGTH - 1; j++){ // preserve null byte
                status_buf[j] = buf[i++];
        }

        if(atoi(status_buf) == HTTP_SUCCESS){
                return 0;
        }else{
                return -1;
        }

}

// returns the offset to the first byte after a crlf sequence
int parse_headers(char *buf, int *buf_pos, int datalen){
        *buf_pos = (*buf_pos < (int) LENGTH_KEY_LEN) ?
                LENGTH_KEY_LEN :
                *buf_pos;

        while(*buf_pos < datalen){

                if(strncmp(END_HEADERS, buf + *buf_pos - END_HEADERS_LEN, END_HEADERS_LEN)){
                        return LENGTH_NOT_FOUND;
                }
                *buf_pos++;
                if(!strncmp(LENGTH_KEY, buf + *buf_pos - LENGTH_KEY_LEN, LENGTH_KEY_LEN)){
                        return *buf_pos;
                }

        }

        return READ_MORE_DATA;
}

int io_n_bytes(int fd, char * buf, int count, int (*minet_io_fun)(int, char*, int)){
        int rc = 0;
        int totalio = 0;
        while((rc = (*minet_io_fun)(fd, buf + totalio, count - totalio)) > 0){
                totalio += rc;
        }
        if(rc < 0)
                return -1;
        else {
                return totalio;
        }
}


int write_n_bytes(int fd, char *buf, int count){
        return io_n_bytes(fd, buf, count, minet_write);
}

int read_n_bytes(int fd, char *buf, int count){
        return io_n_bytes(fd, buf, count, minet_read);
}

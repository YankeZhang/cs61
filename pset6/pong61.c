#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>
#include <pthread.h>
#include "serverinfo.h"
#include <stdbool.h>

static const char *pong_host = PONG_HOST;
static const char *pong_port = PONG_PORT;
static const char *pong_user = PONG_USER;
static struct addrinfo *pong_addr;

// Mutex vars
pthread_mutex_t freezeTime;
pthread_mutex_t mutex;
pthread_cond_t cond;

// TIME HELPERS
double start_time = 0;

// tstamp()
//    Return the current absolute time as a real number of seconds.
double tstamp(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec + now.tv_nsec * 1e-9;
}

// elapsed()
//    Return the number of seconds that have elapsed since `start_time`.
double elapsed(void)
{
    return tstamp() - start_time;
}

// HTTP CONNECTION MANAGEMENT

// `http_connection::cstate` values
typedef enum http_connection_state {
    cstate_idle = 0,    // Waiting to send request
    cstate_waiting = 1, // Sent request, waiting to receive response
    cstate_headers = 2, // Receiving headers
    cstate_body = 3,    // Receiving body
    cstate_closed = -1, // Body complete, connection closed
    cstate_broken = -2  // Parse error
} http_connection_state;

// http_connection
//    This object represents an open HTTP connection to a server.
typedef struct http_connection http_connection;
struct http_connection
{
    int fd; // Socket file descriptor

    http_connection_state cstate; // Connection state (see above)
    int status_code;              // Response status code (e.g., 200, 402)
    size_t content_length;        // Content-Length value
    int has_content_length;       // 1 iff Content-Length was provided
    int eof;                      // 1 iff connection EOF has been reached

    char buf[BUFSIZ];             // Response buffer
    size_t len;                   // Length of response buffer
    struct http_connection *next; // Singly connection linkedlist
};

// Head of connection linked list
http_connection *head = NULL;

// helper functions
char *http_truncate_response(http_connection *conn);
static int http_process_response_headers(http_connection *conn);
static int http_check_response_body(http_connection *conn);

static void usage(void);

// http_connect(ai)
//    Open a new connection to the server described by `ai`. Returns a new
//    `http_connection` object for that server connection. Exits with an
//    error message if the connection fails.
http_connection *http_connect(const struct addrinfo *ai)
{
    // connect to the server
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        exit(1);
    }

    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

    int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (r < 0)
    {
        perror("connect");
        exit(1);
    }

    // construct an http_connection object for this connection
    http_connection *conn =
        (http_connection *)malloc(sizeof(http_connection));
    conn->fd = fd;
    conn->cstate = cstate_idle;
    conn->eof = 0;
    conn->next = NULL; // for connection linkedlist
    return conn;
}

// http_close(conn)
//    Close the HTTP connection `conn` and free its resources.
void http_close(http_connection *conn)
{
    close(conn->fd);
    free(conn);
}

// http_send_request(conn, uri)
//    Send an HTTP POST request for `uri` to connection `conn`.
//    Exit on error.
void http_send_request(http_connection *conn, const char *uri)
{
    assert(conn->cstate == cstate_idle);

    // prepare and write the request
    char reqbuf[BUFSIZ];
    size_t reqsz = snprintf(reqbuf, sizeof(reqbuf),
                            "POST /%s/%s HTTP/1.0\r\n"
                            "Host: %s\r\n"
                            "Connection: keep-alive\r\n"
                            "\r\n",
                            pong_user, uri, pong_host);
    assert(reqsz < sizeof(reqbuf));

    size_t pos = 0;
    while (pos < reqsz)
    {
        ssize_t nw = write(conn->fd, &reqbuf[pos], reqsz - pos);
        if (nw == 0)
        {
            break;
        }
        else if (nw == -1 && errno != EINTR && errno != EAGAIN)
        {
            perror("write");
            exit(1);
        }
        else if (nw != -1)
        {
            pos += nw;
        }
    }

    if (pos != reqsz)
    {
        fprintf(stderr, "%.3f sec: connection closed prematurely\n",
                elapsed());
        exit(1);
    }

    // clear response information
    conn->cstate = cstate_waiting;
    conn->status_code = -1;
    conn->content_length = 0;
    conn->has_content_length = 0;
    conn->len = 0;
}

// http_receive_response_headers(conn)
//    Read the server's response headers and set `conn->status_code`
//    to the server's status code. If the connection terminates
//    prematurely, `conn->status_code` is -1.
void http_receive_response_headers(http_connection *conn)
{
    assert(conn->cstate != cstate_idle);
    if (conn->cstate < 0)
        return;

    // read & parse data until told `http_process_response_headers`
    // tells us to stop
    while (http_process_response_headers(conn))
    {

        if (conn->len != 0)
        {
            conn->len = 0;
        }

        ssize_t nr = read(conn->fd, &conn->buf[conn->len], BUFSIZ);
        if (nr == 0)
            conn->eof = 1;
        else if (nr == -1 && errno != EINTR && errno != EAGAIN)
        {
            perror("read");
            exit(1);
        }
        else if (nr != -1)
            conn->len += nr;
    }

    // Status codes >= 500 mean we are overloading the server
    // and should exit.
    if (conn->status_code >= 500)
    {
        fprintf(stderr, "%.3f sec: exiting because of "
                        "server status %d (%s)\n",
                elapsed(),
                conn->status_code, http_truncate_response(conn));
        exit(1);
    }
}

// http_receive_response_body(conn)
//    Read the server's response body. On return, `conn->buf` holds the
//    response body, which is `conn->len` bytes long and has been
//    null-terminated.
void http_receive_response_body(http_connection *conn)
{
    assert(conn->cstate < 0 || conn->cstate == cstate_body);
    if (conn->cstate < 0)
    {
        return;
    }

    // read response body (http_check_response_body tells us when to stop)
    while (http_check_response_body(conn))
    {
        ssize_t nr = read(conn->fd, &conn->buf[conn->len], BUFSIZ);
        if (nr == 0)
        {
            conn->eof = 1;
        }
        else if (nr == -1 && errno != EINTR && errno != EAGAIN)
        {
            perror("read");
            exit(1);
        }
        else if (nr != -1)
        {
            conn->len += nr;
        }
    }
    conn->buf[conn->len] = 0; // null-terminate
}

// http_truncate_response(conn)
//    Truncate the `conn` response text to a manageable length and return
//    that truncated text. Useful for error messages.
char *http_truncate_response(http_connection *conn)
{
    char *eol = strchr(conn->buf, '\n');
    if (eol)
    {
        *eol = 0;
    }
    if (strnlen(conn->buf, 100) >= 100)
    {
        conn->buf[100] = 0;
    }
    return conn->buf;
}

// verify_connection(connListLen)
//    Verify the `connListLen` linkedlist for valid connections and place as a new node
http_connection *verify_connection(int connListLen)
{
    http_connection *conn = NULL;

    // First connection
    if (head == NULL)
    {
        conn = http_connect(pong_addr);
        head = conn;
    }
    // Everything after first successful init connection
    else
    {
        http_connection *temp = head;

        // If HEAD connection is available use it
        if (temp->cstate == cstate_closed || temp->cstate == cstate_idle)
        {
            conn = temp;
        }
        // If connection after HEAD is available, create a new connection and update linked list
        else if (temp->next == NULL)
        {
            conn = http_connect(pong_addr);
            temp->next = conn;
        }
        // Find next available node in linked list
        else
        {
            while (temp->next != NULL)
            {
                // Update linked list
                connListLen++;
                http_connection *oldConnTemp = temp;
                temp = temp->next;

                // waiting to send request
                if (temp->cstate == cstate_idle)
                {
                    conn = temp;
                    return conn;
                }
                // Parse error
                else if (temp->cstate == cstate_broken)
                {
                    http_connection *connTemp2 = http_connect(pong_addr);
                    connTemp2->next = temp->next;
                    oldConnTemp->next = connTemp2;
                    http_close(temp);
                    conn = connTemp2;
                    return conn;
                }
                else
                {
                    if (temp->next == NULL)
                    {
                        // reset linked list
                        if (connListLen > 30)
                        {
                            temp = head;
                            connListLen = 0;
                        }
                        else
                        {
                            conn = http_connect(pong_addr);
                            temp->next = conn;
                            return conn;
                        }
                    }
                }
            } // End of While
        }
    }
    return conn;
}

// MAIN PROGRAM

typedef struct pong_args
{
    int x;
    int y;
} pong_args;

pong_args pa;

// pong_thread(threadarg)
//    Connect to the server at the position indicated by `threadarg`
//    (which is a pointer to a `pong_args` structure).
void *pong_thread(void *threadarg)
{
    pthread_detach(pthread_self());
    (void)(threadarg);

    char url[256];
    http_connection *conn;
    int napTime = 1;
    int breakWhile = false;
    int connListLen = 0;

    // lock the prints
    pthread_mutex_lock(&freezeTime);
    snprintf(url, sizeof(url), "move?x=%d&y=%d&style=on",
             pa.x, pa.y);
    pthread_mutex_unlock(&freezeTime);

    // Recursive loop until you connect successfully
    while (true)
    {
        // Connect/Reconnect
        conn = verify_connection(connListLen);
        http_send_request(conn, url);
        http_receive_response_headers(conn);

        switch (conn->status_code)
        {
        case -1: // fails connection: keep trying to reconnect!
        {
            usleep(napTime * 100000);
            napTime *= 2;
            break;
        }
        case 200: // Connects successfully
        {
            pthread_cond_signal(&cond);
            http_receive_response_body(conn);
            breakWhile = true;

            // Check for Red Stop Sign
            int result = strncmp("0 OK", conn->buf, 4);
            if (result != 0)
            {
                // LOCKDOWN!!
                pthread_mutex_lock(&freezeTime);

                // Parse the time to stop
                char *napTimeString = &(conn->buf[1]);
                int i = 0;
                while (napTimeString[i] != ' ')
                {
                    i++;
                }
                napTimeString[i] = '\0';
                napTime = strtol(napTimeString, NULL, 10);
                usleep(napTime * 1000);

                // OPEN THE GATES!
                pthread_mutex_unlock(&freezeTime);
            }
            break;
        }
        default: // Didn't return with 200
        {
            fprintf(stderr, "%.3f sec: warning: %d,%d: "
                            "server returned status %d (expected 200)\n",
                    elapsed(), pa.x, pa.y, conn->status_code);
        }
        }
        if (breakWhile == true) // Break while loop
            break;
    } // End of SWITCH

    // Tell main thread to unblock
    if (breakWhile == false)
    {
        pthread_cond_signal(&cond);
    }

    // Exit
    pthread_exit(NULL);
}

// usage()
//    Explain how pong61 should be run.
static void usage(void)
{
    fprintf(stderr, "Usage: ./pong61 [-h HOST] [-p PORT] [USER]\n");
    exit(1);
}

// main(argc, argv)
//    The main loop.
int main(int argc, char **argv)
{
    // parse arguments
    int ch, nocheck = 0, fast = 0;
    while ((ch = getopt(argc, argv, "nfh:p:u:")) != -1)
    {
        if (ch == 'h')
        {
            pong_host = optarg;
        }
        else if (ch == 'p')
        {
            pong_port = optarg;
        }
        else if (ch == 'u')
        {
            pong_user = optarg;
        }
        else if (ch == 'n')
        {
            nocheck = 1;
        }
        else if (ch == 'f')
        {
            fast = 1;
        }
        else
        {
            usage();
        }
    }
    if (optind == argc - 1)
    {
        pong_user = argv[optind];
    }
    else if (optind != argc)
    {
        usage();
    }

    // look up network address of pong server
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    int r = getaddrinfo(pong_host, pong_port, &hints, &pong_addr);
    if (r != 0)
    {
        fprintf(stderr, "problem looking up %s: %s\n",
                pong_host, gai_strerror(r));
        exit(1);
    }

    // reset pong board and get its dimensions
    int width, height, delay = 100000;
    {
        http_connection *conn = http_connect(pong_addr);
        if (!nocheck && !fast)
        {
            http_send_request(conn, "reset");
        }
        else
        {
            char buf[256];
            sprintf(buf, "reset?nocheck=%d&fast=%d", nocheck, fast);
            http_send_request(conn, buf);
        }
        http_receive_response_headers(conn);
        http_receive_response_body(conn);
        int nchars;
        if (conn->status_code != 200 || sscanf(conn->buf, "%d %d %n", &width, &height, &nchars) < 2 || width <= 0 || height <= 0)
        {
            fprintf(stderr, "bad response to \"reset\" RPC: %d %s\n",
                    conn->status_code, http_truncate_response(conn));
            exit(1);
        }
        (void)sscanf(conn->buf + nchars, "%d", &delay);
        http_close(conn);
    }
    // measure future times relative to this moment
    start_time = tstamp();

    // print display URL
    printf("Display: http://%s:%s/%s/%s\n",
           pong_host, pong_port, pong_user,
           nocheck ? " (NOCHECK mode)" : "");

    // Init Synchronization
    pthread_mutex_init(&mutex, NULL);
    pthread_mutex_init(&freezeTime, NULL);
    pthread_cond_init(&cond, NULL);

    // play game
    int x = 0, y = 0, dx = 1, dy = 1;

    while (1)
    {
        pthread_t pt;
        if (pthread_create(&pt, NULL, pong_thread, NULL))
        {
            fprintf(stderr, "%.3f sec: pthread_create: %s\n",
                    elapsed(), strerror(r));
            exit(1);
        }

        pthread_mutex_lock(&mutex);
        pthread_cond_wait(&cond, &mutex);
        pthread_mutex_unlock(&mutex);

        // update position
        x += dx;
        y += dy;
        if (x < 0 || x >= width)
        {
            dx = -dx;
            x += 2 * dx;
        }
        if (y < 0 || y >= height)
        {
            dy = -dy;
            y += 2 * dy;
        }
        pa.x = x;
        pa.y = y;

        // wait 0.1sec
        usleep(delay);
    }
}

// HTTP PARSING

// http_process_response_headers(conn)
//    Parse the response represented by `conn->buf`. Returns 1
//    if more header data remains to be read, 0 if all headers
//    have been consumed.
static int http_process_response_headers(http_connection *conn)
{
    size_t i = 0;
    while ((conn->cstate == cstate_waiting || conn->cstate == cstate_headers) && i + 2 <= conn->len)
    {
        if (conn->buf[i] == '\r' && conn->buf[i + 1] == '\n')
        {
            conn->buf[i] = 0;
            if (conn->cstate == cstate_waiting)
            {
                int minor;
                if (sscanf(conn->buf, "HTTP/1.%d %d",
                           &minor, &conn->status_code) == 2)
                {
                    conn->cstate = cstate_headers;
                }
                else
                {
                    conn->cstate = cstate_broken;
                }
            }
            else if (i == 0)
            {
                conn->cstate = cstate_body;
            }
            else if (strncasecmp(conn->buf, "Content-Length: ", 16) == 0)
            {
                conn->content_length = strtoul(conn->buf + 16, NULL, 0);
                conn->has_content_length = 1;
            }
            // We just consumed a header line (i+2) chars long.
            // Move the rest of the data down, including terminating null.
            memmove(conn->buf, conn->buf + i + 2, conn->len - (i + 2) + 1);
            conn->len -= i + 2;
            i = 0;
        }
        else
        {
            ++i;
        }
    }

    if (conn->eof)
    {
        conn->cstate = cstate_broken;
    }
    return conn->cstate == cstate_waiting || conn->cstate == cstate_headers;
}

// http_check_response_body(conn)
//    Returns 1 if more response data should be read into `conn->buf`,
//    0 if the connection is broken or the response is complete.
static int http_check_response_body(http_connection *conn)
{
    if (conn->cstate == cstate_body && (conn->has_content_length || conn->eof) && conn->len >= conn->content_length)
    {
        conn->cstate = cstate_idle;
    }
    if (conn->eof && conn->cstate == cstate_idle)
    {
        conn->cstate = cstate_closed;
    }
    else if (conn->eof)
    {
        conn->cstate = cstate_broken;
    }
    return conn->cstate == cstate_body;
}

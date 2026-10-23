#include <arpa/inet.h>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std;

struct addrinfo sa, *result;
struct sockaddr sa_creat;

/** 
 * Env overrides let the test harness drive the timeout and size-limit paths          *
 * without waiting 10s or writing a 100 MiB file; unset or invalid keeps the default. *
**/
static long envLong(const char *name, long fallback)
{
    const char *value = getenv(name);
    if (value == nullptr || *value == '\0')
    {
        return fallback;
    }

    char *endptr;
    long parsed = strtol(value, &endptr, 10);
    if (*endptr != '\0' || parsed <= 0)
    {
        return fallback;
    }
    return parsed;
}

int createSocket()
{
    memset(&sa_creat, 0, sizeof(sa_creat));
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (client_socket < 0)
    {
        perror("socket");
        exit(1);
    }

    return client_socket;
}

void handleFile(const char *file, int client_socket)
{
    /* 100MiB default */
    const long MAX_FILE_SIZE = envLong("MAX_FILE_SIZE", 100L * 1024 * 1024);

    ifstream infile(file, ios::binary);
    if (!infile)
    {
        cerr << "ERROR: Cannot open file" << endl;
        exit(1);
    }

    long file_size = static_cast<long>(filesystem::file_size(file));
    cout << "File size: " << file_size << "bytes" << endl;

    /* refuse oversized files before any bytes hit the wire */
    if (file_size > MAX_FILE_SIZE)
    {
        cerr << "ERROR: File size is too large for the server to save (max 100MiB)" << endl;
        exit(1);
    }

    long timeout_seconds = envLong("TIMEOUT_SEC", 10);

    struct timeval send_timeout;
    send_timeout.tv_sec = timeout_seconds;
    send_timeout.tv_usec = 0;

    /* give socket the ability to timeout */
    if (setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) < 0)
    {
        perror("setsockopt SO_SNDTIMEO");
    }

    /**
     * metadata header: 2-byte filename length, the filename, then the
     * 8-byte file size in big-endian order
     * + the filename lets the server save the transfer under its original name
    **/
    string filename = filesystem::path(file).filename().string();
    if (filename.empty() || filename.size() > 255)
    {
        cerr << "ERROR: Invalid filename" << endl;
        exit(1);
    }

    uint64_t declared_size = static_cast<uint64_t>(file_size);
    string header;
    header.push_back(static_cast<char>((filename.size() >> 8) & 0xFF));
    header.push_back(static_cast<char>(filename.size() & 0xFF));
    header += filename;
    for (int i = 7; i >= 0; i--)
    {
        header.push_back(static_cast<char>((declared_size >> (8 * i)) & 0xFF));
    }

    size_t header_len = header.size();
    size_t header_sent = 0;
    while (header_sent < header_len)
    {
        char *tail = &header[header_sent];
        size_t tail_len = header_len - header_sent;
        ssize_t bytes_sent = send(client_socket, tail, tail_len, 0);

        if (bytes_sent < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                cerr << "ERROR: Send timeout - unable to send data for " << timeout_seconds
                     << " seconds" << endl;
            }
            else if (errno == EPIPE || errno == ECONNRESET)
            {
                cerr << "ERROR: Connection lost" << endl;
            }
            else
            {
                cerr << "ERROR: Send failed: " << strerror(errno) << endl;
            }
            close(client_socket);
            exit(1);
        }

        if (bytes_sent == 0)
        {
            cerr << "ERROR: Connection closed by server" << endl;
            close(client_socket);
            exit(1);
        }

        header_sent += bytes_sent;
    }

    char buffer[4096];
    long total_sent = 0;

    /**
     * while the infile is read in the buffer and the characters
     * extracted are greater than zero: 
    **/
    while (infile.read(buffer, sizeof(buffer)) || infile.gcount() > 0)
    {
        size_t bytes_read = infile.gcount();
        size_t chunks_sent = 0;

        /**
         * send() may accept only part of the chunk, so keep sending into the
         * socket until every byte read from the outer loop has been processed
        **/
        while (chunks_sent < bytes_read)
        {
            char *tail = buffer + chunks_sent;
            size_t tail_len = bytes_read - chunks_sent;
            ssize_t bytes_sent = send(client_socket, tail, tail_len, 0);

            if (bytes_sent < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    cerr << "ERROR: Send timeout - unable to send data for " << timeout_seconds
                         << " seconds" << endl;
                }
                else if (errno == EPIPE || errno == ECONNRESET)
                {
                    cerr << "ERROR: Connection lost" << endl;
                }
                else
                {
                    cerr << "ERROR: Send failed: " << strerror(errno) << endl;
                }
                close(client_socket);
                exit(1);
            }

            if (bytes_sent == 0)
            {
                cerr << "ERROR: Connection closed by server" << endl;
                close(client_socket);
                exit(1);
            }

            chunks_sent += bytes_sent;
            total_sent += bytes_sent;
            cout << "Sent " << bytes_sent << " bytes" << endl;
        }
    }

    cout << "Sent " << total_sent << " bytes total" << endl;
    infile.close();
}

/** 
 * this function uses fcntl file control to make it to where the socket doesn't block 
 * during connect() by changing socket properties, enabling the 10 second idle timeout
 * instead of the default OS timeout period 
**/
int connectionTimeout(int client_socket, struct sockaddr *sa2, socklen_t addrlen, int timeout_sec)
{
    /* supposed to be in blocking mode with a return value of 0 */
    int flags = fcntl(client_socket, F_GETFL, 0);

    if (flags < 0)
    {
        perror("fcntl F_GETFL");
        return -1;
    }

    /* set socket to non blocking mode */
    if (fcntl(client_socket, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        perror("fcntl F_SETFL");
        return -1;
    }
    
    int result = connect(client_socket, sa2, addrlen); 

    /* if a connection is made immediately,
     * then restore blocking mode          */
    if (result == 0)
    {
        fcntl(client_socket, F_SETFL, flags);
        return 0;
    }

    struct timeval send_timeout;

    /* monitor the socket for any errors in order to send the timeout */
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(client_socket, &write_fds);

    /* will get specified as 10 later on */
    send_timeout.tv_sec = timeout_sec;
    send_timeout.tv_usec = 0;

    /* monitors for when the socket is ready for writing */
    result = select(client_socket + 1, NULL, &write_fds, NULL, &send_timeout);

    if (result == 0)
    {
        errno = ETIMEDOUT;
        return -1;
    }

    if (result < 0)
    {
        perror("select");
        return -1;
    }

    /**
     * Check if connection actually succeeded and give the socket
     * the ability to support socket error coverage
    **/
    int sock_error;
    socklen_t len = sizeof(sock_error);
    if (getsockopt(client_socket, SOL_SOCKET, SO_ERROR, &sock_error, &len) < 0)
    {
        perror("getsockopt SO_ERROR");
        return -1;
    }

    if (sock_error != 0)
    {
        errno = sock_error;
        return -1;
    }

    /* Restore blocking mode if connection succeeds */
    fcntl(client_socket, F_SETFL, flags);

    return 0;
}

void createConnection(int client_socket, char *argv[])
{
    const char *hostname = argv[1];
    const char *port = argv[2];
    const char *file = argv[3];

    char *endptr;
    long port_long = strtol(port, &endptr, 10);

    /* Check if the port is a valid number */
    if (*endptr != '\0' || endptr == argv[2])
    {
        cerr << "ERROR: Invalid port number" << std::endl;
        exit(1);
    }

    /* check if the port is in a range between 1024 and 65535 */
    if (port_long < 1024 || port_long > 65535)
    {
        cerr << "ERROR: port must be between 1024 and 65535" << std::endl;
        exit(1);
    }

    /* bind socket using addrinfo in order to support the
     * usages of localhost and hostnames for CLI arguments */
    sa.ai_family = AF_INET;
    sa.ai_socktype = SOCK_STREAM;

    /** 
     * 3rd paramenter is used to fill in specifications for 
     * the address, and result is where getaddrinfo stores 
     * the answer with said specifications 
     **/
    if (getaddrinfo(hostname, port, &sa, &result) != 0)
    {
        cerr << "ERROR: Could not resolve hostname" << endl;
        exit(1);
    }

    /** 
     * these get triggered if the client can't make connections either within a given
     * amount of time or if the hostname is actually legit
     **/
    long timeout_seconds = envLong("TIMEOUT_SEC", 10);

    if (connectionTimeout(client_socket, result->ai_addr, result->ai_addrlen, timeout_seconds) ==
        -1)
    {
        if (errno == ETIMEDOUT)
        {
            cerr << "ERROR: Connection timeout after 10 seconds" << endl;
        }
        else
        {
            cerr << "ERROR: failed to connect to server" << endl;
        }
        freeaddrinfo(result);
        close(client_socket);
        exit(1);
    }
    freeaddrinfo(result);
    cout << "Connected to server" << endl;
    handleFile(file, client_socket);
    close(client_socket);
}

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        cerr << "ERROR: Usage: " << argv[0] << " <HOSTNAME-OR-IP> <PORT> <FILENAME>" << endl;
        return 1;
    }

    int my_socket = createSocket();
    createConnection(my_socket, argv);

    return 0;
}

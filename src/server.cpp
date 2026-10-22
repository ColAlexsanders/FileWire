#include <fstream>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <errno.h>
#include <cstdint>

using namespace std;

struct sockaddr_in sa; // server address
struct sockaddr_in ca; // client address

/** 
 * Env overrides let the test script drive the timeout and size-limit paths
 * without waiting 10s or writing a 100 MiB file; unset or invalid keeps the default.
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

/* SIGTERM/SIGQUIT global flag and handler function */ 
volatile sig_atomic_t shutdown_requested = 0;

void signalHandler(int)
{
    shutdown_requested = 1;
}

int createSocket()
{
    /** 
     * clear struct before usage in order to ensure that there 
     * are no bytes in unused fields or padding bytes (useless data)
    **/
    memset(&sa, 0, sizeof(sa));
    /* create socket that is IPv4 using TCP */
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);

    /* enable the socket to support multiple connections */
    int yes = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    return server_socket;
}

int createPort(char *argv[])
{
    /* allows for port cli input to get validated */
    char* endptr;     
    
    /** 
     * convert string into a decimal long and set endptr
     * to point to where the conversion stopped 
    **/
    long port_long = strtol(argv[1], &endptr, 10); 
    
    if (*endptr != '\0' || endptr == argv[1])
    {
        cerr << "ERROR: Invalid port number" << endl;
        exit(1);
    }

    if (port_long < 1024 || port_long > 65535)
    {
        cerr << "ERROR: port must be between 1024 and 65535" << endl;
        exit(1);
    }

    /**
     * convert port into an int so that it
     * can be used for socket binding
     **/
    int port = static_cast<int>(port_long);

    return port;
}

void verifyDir(char *argv[])
{
    string file_dir = argv[2];

    cout << "Checking directory: '" << file_dir << "'" << endl;
    
    if (!filesystem::exists(argv[2]))
    {
        cerr << "ERROR: Directory does not exist" << endl;
        exit(1);
    }

    if (!filesystem::is_directory(argv[2]))
    {
        cerr << "ERROR: path provided to store file is not a directory" << endl;
        exit(1);
    }
}

void bindSocket(int server_socket, int port)
{
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port); 
    sa.sin_addr.s_addr = INADDR_ANY;
    memset(sa.sin_zero, '\0', sizeof(sa.sin_zero));

    if (bind(server_socket, (struct sockaddr*)&sa, sizeof(sa)) == -1 )
    {
        perror("bind");
        exit(1);
    }
}

void startListening(int server_socket)
{
    if (listen(server_socket, 1) == -1)
    {
        perror("listen error");
        exit(2);
    }
    else
    {
        cout << "server listening..." << endl;
    }
}

/**
 * recv() can be interrupted or return a short read (EINTR trigger), 
 * so that would warrant a retry.
**/
static ssize_t recvOnce(int client_socket, char *buffer, size_t length, long timeout_seconds)
{
    while (true)
    {
        ssize_t bytes_received = recv(client_socket, buffer, length, 0);

        if (bytes_received >= 0)
        {
            return bytes_received;
        }

        if (errno == EINTR)
        {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            cerr << "Timeout: no data received for " << timeout_seconds << " seconds" << endl;
        }
        else
        {
            perror("recv");
        }
        return -1;
    }
}

/**
 * read exactly length bytes into buffer; retries short reads and EINTR.
 * Returns false on EOF, timeout, or error (timeout/error already logged).
**/
static bool recvExact(int client_socket, char *buffer, size_t length, long timeout_seconds)
{
    size_t total_read = 0;

    while (total_read < length)
    {
        ssize_t bytes_received = recvOnce(client_socket, buffer + total_read, length - total_read, timeout_seconds);

        if (bytes_received <= 0)
        {
            return false;
        }
        total_read += static_cast<size_t>(bytes_received);
    }
    return true;
}

void receiveFile(int client_socket, char *argv[])
{
    long timeout_seconds = envLong("TIMEOUT_SEC", 10);

    struct timeval timeout;
    timeout.tv_sec = timeout_seconds;
    timeout.tv_usec = 0;

    /* give socket the ability to timeout */
    if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        perror("setsockopt");
    }
    
    /* 100MiB default */
    const long MAX_FILE_SIZE = envLong("MAX_FILE_SIZE", 100L * 1024 * 1024);

    /**
     * metadata header: 2-byte filename length, the filename, then the
     * 8-byte file size in big-endian order
    **/
    unsigned char name_length_bytes[2] = {0};
    unsigned char size_bytes[8] = {0};
    string filename;
    uint64_t declared_size = 0;
    bool header_ok = recvExact(client_socket, reinterpret_cast<char *>(name_length_bytes), sizeof(name_length_bytes), timeout_seconds);

    uint16_t name_length = static_cast<uint16_t>((name_length_bytes[0] << 8) | name_length_bytes[1]);
    header_ok = header_ok && name_length > 0 && name_length <= 255;

    if (header_ok)
    {
        filename.resize(name_length);
        header_ok = recvExact(client_socket, filename.data(), name_length, timeout_seconds);
    }

    header_ok = header_ok && recvExact(client_socket, reinterpret_cast<char *>(size_bytes), sizeof(size_bytes), timeout_seconds);

    if (header_ok)
    {
        for (int i = 0; i < 8; i++)
        {
            declared_size = (declared_size << 8) | size_bytes[i];
        }
    }

    /* a basename only, so a hostile client cannot escape the storage directory */
    bool name_valid = header_ok && filename.find('/') == string::npos && filename.find('\0') == string::npos && filename != "." && filename != "..";

    if (!name_valid)
    {
        cerr << "ERROR: Invalid or missing metadata header" << endl;
        return;
    }

    bool size_valid = declared_size <= static_cast<uint64_t>(MAX_FILE_SIZE);
    if (!size_valid)
    {
        cerr << "ERROR: Declared size " << declared_size << " exceeds the " << MAX_FILE_SIZE << " byte limit" << endl;
    }

    string path = string(argv[2]) + "/" + filename;
    cout << "Saving to: " << path << endl;

    ofstream outfile(path, ios::binary | ios::trunc);
    if (!outfile)
    {
        cerr << "ERROR: Cannot create file" << endl;
        exit(1);
    }

    bool failed = !size_valid;
    long total_bytes = 0;
    char buf[4096];

    /* read exactly declared_size bytes; EOF before that signals truncation */
    while (!failed && total_bytes < static_cast<long>(declared_size))
    {
        size_t remaining = static_cast<size_t>(declared_size - static_cast<uint64_t>(total_bytes));
        size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
        ssize_t bytes_received = recvOnce(client_socket, buf, want, timeout_seconds);

        if (bytes_received <= 0)
        {
            if (bytes_received == 0)
            {
                cerr << "ERROR: Truncated transfer: got " << total_bytes << " of " << declared_size << " bytes" << endl;
            }
            failed = true;
            break;
        }

        total_bytes += static_cast<long>(bytes_received);
        outfile.write(buf, bytes_received);
        cout << "Data received: " << bytes_received << " bytes" << endl;
    }

    if (!failed)
    {
        cout << "transfer completed: " << total_bytes << " bytes" << endl;
    }

    outfile.close();

    /* a failed transfer is replaced with a literal ERROR marker */
    if (failed)
    {
        outfile.open(path, ios::binary | ios::trunc);
        outfile.write("ERROR", 5);
        outfile.close();
    }
}

void acceptConnection(int server_socket, char *argv[])
{
    socklen_t ca_size = sizeof(ca);
    int client_socket = accept(server_socket, (struct sockaddr*)&ca, &ca_size);

    if (client_socket == -1)
    {
        perror("accept");
        return;
    }
    
    receiveFile(client_socket, argv);
    
    close(client_socket); 
}


int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        cerr << "ERROR: Usage: " << argv[0] << " <PORT> <FILE-DIR>" << endl;
        return 1;
    }
    
    signal(SIGTERM, signalHandler);
    signal(SIGQUIT, signalHandler);

    verifyDir(argv);
    int server_socket = createSocket();
    int port = createPort(argv);
    bindSocket(server_socket, port);
    
    startListening(server_socket);
    
    while (!shutdown_requested)
    {
        acceptConnection(server_socket, argv);
    }
    
    close(server_socket);
    return 0;
}

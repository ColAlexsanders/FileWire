/** 
 * Shim for socket failure tests. This gets loaded in as a shared object in the client with 
 * LD_PRELOAD so that socket() can fail with EMFILE, the error real file-descriptor exhaustion produces.
 * + used only by tests/integration.sh, so it doesn't get linked into the binaries
**/

#include <errno.h>
#include <sys/socket.h>

int socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    errno = EMFILE;
    return -1;
}

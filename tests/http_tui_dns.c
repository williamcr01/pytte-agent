/* Test-only DNS routing for the unmodified TUI executable. All named network
 * destinations except the fixture provider are rejected. TLS still verifies
 * api.anthropic.com; only the loopback TCP address/port change. */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <netdb.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static int (*system_getaddrinfo)(const char *, const char *, const struct addrinfo *,
                                 struct addrinfo **);
static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;

static void resolve_symbol(void)
{
    void *symbol = dlsym(RTLD_NEXT, "getaddrinfo");
    if (sizeof symbol == sizeof system_getaddrinfo)
        memcpy(&system_getaddrinfo, &symbol, sizeof symbol);
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **out)
{
    const char *port = getenv("PYTTE_TEST_HTTP_PORT");
    (void)service;
    pthread_once(&resolve_once, resolve_symbol);
    if (!system_getaddrinfo || !port || !*port || !node ||
        strcmp(node, "api.anthropic.com"))
        return EAI_FAIL;
    return system_getaddrinfo("127.0.0.1", port, hints, out);
}

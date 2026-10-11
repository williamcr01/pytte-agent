/* Verification-only system curl adapter. Simulates a distro whose built-in
 * CA file/directory are the offline fixture; never used in app binaries. */
#define _POSIX_C_SOURCE 200809L
#include <curl/curl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static CURL *fixture_easy_init(void)
{
    CURL *e = curl_easy_init();
    const char *file = getenv("PYTTE_TEST_CA_FILE"), *path = getenv("PYTTE_TEST_CA_PATH");
    if (e && (curl_easy_setopt(e, CURLOPT_CAINFO, file) != CURLE_OK ||
              curl_easy_setopt(e, CURLOPT_CAPATH, path) != CURLE_OK)) {
        curl_easy_cleanup(e);
        return NULL;
    }
    return e;
}

static CURLM *fixture_multi_init(void)
{
    CURLM *m = curl_multi_init();
    const char *removed = getenv("PYTTE_TEST_REMOVE_CA");
    if (removed)
        (void)unlink(removed); /* Remove the seed after trust_init selected it. */
    if (getenv("PYTTE_TEST_SET_PROXY")) {
        (void)setenv("https_proxy", "http://127.0.0.1:9", 1);
        (void)setenv("NO_PROXY", "localhost", 1);
    }
    if (getenv("PYTTE_TEST_CLEAR_OVERRIDE")) {
        (void)unsetenv("SSL_CERT_FILE");
        (void)unsetenv("SSL_CERT_DIR");
    }
    return m;
}

static CURLcode fixture_easy_getinfo(CURL *e, CURLINFO info, ...)
{
    const char *sent = getenv("PYTTE_TEST_SENT_REQUEST");
    CURLcode result;
    va_list ap;
    va_start(ap, info);
    if (info == CURLINFO_REQUEST_SIZE || info == CURLINFO_RESPONSE_CODE) {
        long *out = va_arg(ap, long *);
        result = curl_easy_getinfo(e, info, out);
        if (sent && !strcmp(sent, "header") && info == CURLINFO_REQUEST_SIZE)
            *out = 1;
    } else if (info == CURLINFO_SIZE_UPLOAD_T) {
        curl_off_t *out = va_arg(ap, curl_off_t *);
        result = curl_easy_getinfo(e, info, out);
        if (sent && !strcmp(sent, "body"))
            *out = 1;
    } else if (info == CURLINFO_CAINFO || info == CURLINFO_CAPATH) {
        char **out = va_arg(ap, char **);
        *out = getenv(info == CURLINFO_CAINFO ? "PYTTE_TEST_CA_FILE" : "PYTTE_TEST_CA_PATH");
        result = CURLE_OK;
    } else if (info == CURLINFO_TLS_SSL_PTR) {
        struct curl_tlssessioninfo **out = va_arg(ap, struct curl_tlssessioninfo **);
        result = curl_easy_getinfo(e, info, out);
    } else {
        result = CURLE_UNKNOWN_OPTION;
    }
    va_end(ap);
    return result;
}

#define PT_CURL_LINKED
#define curl_easy_init fixture_easy_init
#define curl_multi_init fixture_multi_init
#undef curl_easy_getinfo
#define curl_easy_getinfo fixture_easy_getinfo
#include "drv/http_curl.c"

/* Trust selection and lifetime checks without a network connection. */
#define _POSIX_C_SOURCE 200809L
#include "t.h"
#include "drv/http_curl.c"

static void clean_environment(void)
{
    static const char *const names[] = {"SSL_CERT_FILE", "SSL_CERT_DIR", "http_proxy",
        "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"};
    unsigned i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        CHECK(unsetenv(names[i]) == 0);
}

static void test_capture(void)
{
    pt_http_driver *vt;
    curl_drv *d;
    clean_environment();
    CHECK(setenv("SSL_CERT_FILE", "/tmp/captured-file.pem", 1) == 0);
    CHECK(setenv("SSL_CERT_DIR", "/tmp/captured-directory", 1) == 0);
    vt = pt_http_curl_new();
    CHECK(vt != NULL);
    if (!vt)
        return;
    d = (curl_drv *)vt;
    CHECK(d->first != NULL && d->ca_seed == NULL);
    CHECK(setenv("SSL_CERT_FILE", "/tmp/changed-file.pem", 1) == 0);
    CHECK(unsetenv("SSL_CERT_DIR") == 0);
    CHECK(strcmp(d->ca_file, "/tmp/captured-file.pem") == 0);
    CHECK(strcmp(d->ca_path, "/tmp/captured-directory") == 0);
    CHECK(d->original_path == d->ca_path);
    CHECK((uintptr_t)d->ca_file >= (uintptr_t)d->trust.p &&
          (uintptr_t)d->ca_file < (uintptr_t)d->trust.p + d->trust.n);
    CHECK((uintptr_t)d->ca_path >= (uintptr_t)d->trust.p &&
          (uintptr_t)d->ca_path < (uintptr_t)d->trust.p + d->trust.n);
    vt->free(vt); /* The never-used easy probe and its paths must be freed. */
    clean_environment();
}

static void test_system_store(void)
{
    pt_http_driver *vt;
    curl_drv *d;
    struct curl_tlssessioninfo *tls = NULL;
    clean_environment();
    vt = pt_http_curl_new();
    CHECK(vt != NULL);
    if (!vt)
        return;
    d = (curl_drv *)vt;
    CHECK(curl_easy_getinfo(d->first, CURLINFO_TLS_SSL_PTR, &tls) == CURLE_OK);
    CHECK(tls != NULL);
    if (d->ca_seed) {
        struct stat st;
        CHECK(tls && tls->backend == CURLSSLBACKEND_OPENSSL);
        CHECK(d->ca_path != NULL && stat(d->ca_seed, &st) == 0 && S_ISREG(st.st_mode));
        CHECK(strncmp(d->ca_seed, d->ca_path, strlen(d->ca_path)) == 0);
    }
    vt->free(vt);
    CHECK(setenv("HTTPS_PROXY", "http://127.0.0.1:9", 1) == 0);
    vt = pt_http_curl_new();
    CHECK(vt != NULL);
    if (vt) {
        CHECK(((curl_drv *)vt)->ca_seed == NULL);
        vt->free(vt);
    }
    clean_environment();
}

static void test_hash_names(void)
{
    CHECK(hash_cert_name("0123abcd.0"));
    CHECK(!hash_cert_name("0123abcd.1"));
    CHECK(!hash_cert_name("0123abcd.r0"));
    CHECK(!hash_cert_name("0123abcg.0"));
    CHECK(!hash_cert_name("0123abcd.00"));
    CHECK(!hash_cert_name("../abcd.0"));
}

int main(void)
{
    RUN(test_capture);
    RUN(test_system_store);
    RUN(test_hash_names);
    return t_done("test_http_trust");
}

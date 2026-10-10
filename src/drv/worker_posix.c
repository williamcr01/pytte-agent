/* One reusable host worker. Jobs do blocking I/O; agent callbacks and all
 * core/UI mutation remain on the event-loop thread. No per-job allocation. */
#define _POSIX_C_SOURCE 200809L
#include "drv/drv.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

struct pt_worker {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int pipe[2];
    void (*fn)(void *);
    void *ud;
    bool pending, done, stop;
};

static void *work(void *ud)
{
    pt_worker *w = (pt_worker *)ud;
    pthread_mutex_lock(&w->lock);
    for (;;) {
        void (*fn)(void *);
        void *arg;
        while ((!w->pending || w->done) && !w->stop)
            pthread_cond_wait(&w->wake, &w->lock);
        if (w->stop)
            break;
        fn = w->fn;
        arg = w->ud;
        pthread_mutex_unlock(&w->lock);
        fn(arg);
        pthread_mutex_lock(&w->lock);
        w->done = true;
        /* One outstanding completion, so a nonblocking pipe cannot fill. */
        {
            char c = 1;
            ssize_t n;
            do n = write(w->pipe[1], &c, 1); while (n < 0 && errno == EINTR);
            (void)n;
        }
    }
    pthread_mutex_unlock(&w->lock);
    return NULL;
}

pt_worker *pt_worker_new(void)
{
    pt_worker *w = (pt_worker *)calloc(1, sizeof *w);
    pthread_attr_t attr;
    sigset_t all, previous;
    int rc;
    if (!w)
        return NULL;
    if (pipe(w->pipe) != 0) {
        free(w);
        return NULL;
    }
    if (fcntl(w->pipe[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(w->pipe[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(w->pipe[0], F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(w->pipe[1], F_SETFL, O_NONBLOCK) < 0)
        goto fail_pipe;
    if (pthread_mutex_init(&w->lock, NULL) != 0)
        goto fail_pipe;
    if (pthread_cond_init(&w->wake, NULL) != 0)
        goto fail_lock;
    if (pthread_attr_init(&attr) != 0)
        goto fail_cond;
    /* Bounded stack, with room for the POSIX store's file-reading scratch
     * and sanitizer redzones. Only touched pages become resident. */
    if (pthread_attr_setstacksize(&attr, 256 * 1024) != 0) {
        pthread_attr_destroy(&attr);
        goto fail_cond;
    }
    sigfillset(&all);
    rc = pthread_sigmask(SIG_BLOCK, &all, &previous);
    if (rc == 0) {
        rc = pthread_create(&w->thread, &attr, work, w);
        pthread_sigmask(SIG_SETMASK, &previous, NULL);
    }
    pthread_attr_destroy(&attr);
    if (rc != 0)
        goto fail_cond;
    return w;
fail_cond:
    pthread_cond_destroy(&w->wake);
fail_lock:
    pthread_mutex_destroy(&w->lock);
fail_pipe:
    close(w->pipe[0]);
    close(w->pipe[1]);
    free(w);
    return NULL;
}

bool pt_worker_start(pt_worker *w, void (*fn)(void *), void *ud)
{
    bool ok;
    pthread_mutex_lock(&w->lock);
    ok = !w->pending && !w->stop;
    if (ok) {
        w->fn = fn;
        w->ud = ud;
        w->pending = true;
        w->done = false;
        pthread_cond_signal(&w->wake);
    }
    pthread_mutex_unlock(&w->lock);
    return ok;
}

bool pt_worker_finish(pt_worker *w)
{
    bool done;
    char c;
    pthread_mutex_lock(&w->lock);
    done = w->pending && w->done;
    if (done) {
        ssize_t n;
        do n = read(w->pipe[0], &c, 1); while (n < 0 && errno == EINTR);
        (void)n;
        w->pending = false;
        w->fn = NULL;
        w->ud = NULL;
    }
    pthread_mutex_unlock(&w->lock);
    return done;
}

int pt_worker_fd(const pt_worker *w) { return w->pipe[0]; }

void pt_worker_free(pt_worker *w)
{
    if (!w)
        return;
    pthread_mutex_lock(&w->lock);
    w->stop = true;
    pthread_cond_signal(&w->wake);
    pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread, NULL); /* any borrowed job data stays alive */
    pthread_cond_destroy(&w->wake);
    pthread_mutex_destroy(&w->lock);
    close(w->pipe[0]);
    close(w->pipe[1]);
    free(w);
}

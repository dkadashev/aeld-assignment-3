#include "threading.h"
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

void* threadfunc(void* thread_param)
{
    struct thread_data* tdata = (struct thread_data *) thread_param;
    tdata->thread_complete_success = false;
    usleep(tdata->wait_to_obtain_ms * 1000);
    if (!pthread_mutex_lock(tdata->mutex)) {
        usleep(tdata->wait_to_release_ms * 1000);
        if (!pthread_mutex_unlock(tdata->mutex)) {
            tdata->thread_complete_success = true;
        }
    }
    return thread_param;
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,int wait_to_obtain_ms, int wait_to_release_ms)
{
    bool rv = false;
    pthread_attr_t attr;
    pthread_attr_t* pattr = NULL;
    struct thread_data* tdata = calloc(1, sizeof(struct thread_data));
    if (tdata == NULL) {
        ERROR_LOG("calloc failed, %s", strerror(errno));
        goto out;
    }
    if (pthread_attr_init(&attr)) {
        ERROR_LOG("pthread_attr_init failed, %s", strerror(errno));
        goto out;
    }

    pattr = &attr;
    tdata->mutex = mutex;
    tdata->wait_to_obtain_ms = wait_to_obtain_ms;
    tdata->wait_to_release_ms = wait_to_release_ms;

    if (pthread_create(thread, &attr, threadfunc, tdata)) {
        ERROR_LOG("pthread_create failed, %s", strerror(errno));
        goto out;
    }
    tdata = NULL; // to be freed by the joiner
    rv = true;

out:
    if (pattr != NULL) {
        pthread_attr_destroy(pattr);
    }
    if (tdata != NULL) {
        free(tdata);
    }
    return rv;
}


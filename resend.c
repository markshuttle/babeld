/*
Copyright (c) 2007, 2008 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

#include "babel.h"
#include "util.h"
#include "neighbour.h"
#include "resend.h"
#include "message.h"
#include "network.h"
#include "filter.h"

struct timeval resend_time = {0, 0};
struct resend *to_resend = NULL;

static int
resend_match(struct resend *resend,
             int kind, const unsigned char *prefix, unsigned char plen)
{
    return (resend->kind == kind &&
            resend->plen == plen && memcmp(resend->prefix, prefix, 16) == 0);
}

static struct resend *
find_resend(int kind, const unsigned char *prefix, unsigned char plen,
             struct resend **previous_return)
{
    struct resend *current, *previous;

    previous = NULL;
    current = to_resend;
    while(current) {
        if(resend_match(current, kind, prefix, plen)) {
            if(previous_return)
                *previous_return = previous;
            return current;
        }
        previous = current;
        current = current->next;
    }

    return NULL;
}

struct resend *
find_request(const unsigned char *prefix, unsigned char plen,
             struct resend **previous_return)
{
    return find_resend(RESEND_REQUEST, prefix, plen, previous_return);
}

int
record_resend(int kind, const unsigned char *prefix, unsigned char plen,
              unsigned short seqno, unsigned short router_hash,
              struct network *network, int delay)
{
    struct resend *resend;
    unsigned int ifindex = network ? network->ifindex : 0;

    if(input_filter(NULL, prefix, plen, NULL, ifindex) >= INFINITY ||
       output_filter(NULL, prefix, plen, ifindex) >= INFINITY)
        return 0;

    if(delay >= 0xFFFF)
        delay = 0xFFFF;

    resend = find_resend(kind, prefix, plen, NULL);
    if(resend) {
        if(resend->delay && delay)
            resend->delay = MIN(resend->delay, delay);
        else if(delay)
            resend->delay = delay;
        resend->time = now;
        resend->max = kind == RESEND_REQUEST ? 128 : UPDATE_MAX;
        if(resend->router_hash == router_hash &&
           seqno_compare(resend->seqno, seqno) > 0) {
            return 0;
        }
        resend->router_hash = router_hash;
        resend->seqno = seqno;
        if(resend->network != network)
            resend->network = NULL;
    } else {
        resend = malloc(sizeof(struct resend));
        if(resend == NULL)
            return -1;
        resend->kind = kind;
        resend->max = kind == RESEND_REQUEST ? 128 : UPDATE_MAX;
        resend->delay = delay;
        memcpy(resend->prefix, prefix, 16);
        resend->plen = plen;
        resend->seqno = seqno;
        resend->router_hash = router_hash;
        resend->network = network;
        resend->time = now;
        resend->next = to_resend;
        to_resend = resend;
    }

    if(resend->delay) {
        struct timeval timeout;
        timeval_plus_msec(&timeout, &resend->time, resend->delay);
        timeval_min(&resend_time, &timeout);
    }
    return 1;
}

int
unsatisfied_request(const unsigned char *prefix, unsigned char plen,
                    unsigned short seqno, unsigned short router_hash)
{
    struct resend *request;

    request = find_request(prefix, plen, NULL);
    if(request == NULL)
        return 0;

    if(request->router_hash != router_hash ||
       seqno_compare(request->seqno, seqno) <= 0)
        return 1;

    return 0;
}

int
satisfy_request(const unsigned char *prefix, unsigned char plen,
                unsigned short seqno, unsigned short router_hash,
                struct network *network)
{
    struct resend *request, *previous;

    request = find_request(prefix, plen, &previous);
    if(request == NULL)
        return 0;

    if(network != NULL && request->network != network)
        return 0;

    if(request->router_hash != router_hash ||
       seqno_compare(request->seqno, seqno) <= 0) {
        if(previous == NULL)
            to_resend = request->next;
        else
            previous->next = request->next;
        free(request);
        recompute_resend_time();
        return 1;
    }

    return 0;
}

static int
resend_expired(struct resend *resend)
{
    switch(resend->kind) {
    case RESEND_REQUEST:
        return timeval_minus_msec(&now, &resend->time) >= REQUEST_TIMEOUT;
    case RESEND_UPDATE:
        return resend->max <= 0;
    default: abort();
    }
}

void
expire_resend()
{
    struct resend *current, *previous;
    int recompute = 0;

    previous = NULL;
    current = to_resend;
    while(current) {
        if(resend_expired(current)) {
            if(previous == NULL) {
                to_resend = current->next;
                free(current);
                current = to_resend;
            } else {
                previous->next = current->next;
                free(current);
                current = previous->next;
            }
            recompute = 1;
        } else {
            current = current->next;
        }
    }
    if(recompute)
        recompute_resend_time();
}

void
recompute_resend_time()
{
    struct resend *request;
    struct timeval resend = {0, 0};

    request = to_resend;
    while(request) {
        if(!resend_expired(request) && request->delay > 0 && request->max > 0) {
            struct timeval timeout;
            timeval_plus_msec(&timeout, &request->time, request->delay);
            timeval_min(&resend_time, &timeout);
        }
        request = request->next;
    }

    resend_time = resend;
}

void
do_resend()
{
    struct resend *resend;

    resend = to_resend;
    while(resend) {
        if(!resend_expired(resend) && resend->delay > 0 && resend->max > 0) {
            struct timeval timeout;
            timeval_plus_msec(&timeout, &resend->time, resend->delay);
            if(timeval_compare(&now, &timeout) >= 0) {
                switch(resend->kind) {
                case RESEND_REQUEST:
                    send_request(resend->network,
                                 resend->prefix, resend->plen, 127,
                                 resend->seqno, resend->router_hash);
                    resend->delay *= 2;
                    break;
                case RESEND_UPDATE:
                    send_update(resend->network, 1,
                                resend->prefix, resend->plen);
                    /* No back-off for updates */
                    break;
                default: abort();
                }
                resend->max--;
            }
        }
        resend = resend->next;
    }
    recompute_resend_time();
}
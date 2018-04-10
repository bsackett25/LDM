/**
 * Copyright 2014 University Corporation for Atmospheric Research. All rights
 * reserved. See the the file COPYRIGHT in the top-level source-directory for
 * licensing conditions.
 *
 *   @file: up7_down7_test.c
 * @author: Steven R. Emmerson
 *
 * This file tests an upstream LDM-7 sending to a downstream LDM-7.
 */

#include "config.h"

#include "down7.h"
#include "error.h"
#include "globals.h"
#include "inetutil.h"
#include "ldm_config_file.h"
#include "ldmprint.h"
#include "log.h"
#include "mcast_info.h"
#include "mldm_receiver_memory.h"
#include "mldm_sender_map.h"
#include "pq.h"
#include "prod_index_map.h"
#include "timestamp.h"
#include "UpMcastMgr.h"
#include "VirtualCircuit.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <rpc/rpc.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>

#define USE_SIGWAIT 0
#define CANCEL_SENDER 1

#ifndef MAX
    #define MAX(a,b) ((a) >= (b) ? (a) : (b))
#endif

typedef struct {
    SVCXPRT*              xprt;
    bool                  xprtAllocated;
} Up7;

typedef struct {
    pthread_t             thread; ///< Sender TCP server thread
    int                   sock;
} Sender;

typedef struct {
    Down7*                down7;
    pthread_t             thread;
    Ldm7Status            status;
} Receiver;

/*
 * Proportion of data-products that the receiving LDM-7 will delete from the
 * product-queue and request from the sending LDM-7.
 */
#define                  REQUEST_RATE 0.1
// Maximum size of a data-product in bytes
#define                  MAX_PROD_SIZE 1000000
// Approximate number of times the product-queue will be "filled".
#define                  NUM_TIMES 1
// Duration, in nanoseconds, between data-product insertions
#define                  INTER_PRODUCT_INTERVAL 50000000 // 50 ms
/*
 * Mean residence-time, in seconds, of a data-product. Also used to compute the
 * FMTP retransmission timeout.
 */
#define                  MEAN_RESIDENCE_TIME 2

// Mean product size in bytes
#define                  MEAN_PROD_SIZE (MAX_PROD_SIZE/2)
// Mean number of products in product-queue
#define                  MEAN_NUM_PRODS \
        ((int)(MEAN_RESIDENCE_TIME / (INTER_PRODUCT_INTERVAL/1e9)))

/*
 * The product-queue is limited by its data-capacity (rather than its product-
 * capacity) to attempt to reproduce the queue corruption seen by Shawn Chen at
 * the University of Virginia.
 */
// Capacity of the product-queue in bytes
static const unsigned    PQ_DATA_CAPACITY = MEAN_NUM_PRODS*MEAN_PROD_SIZE;
// Capacity of the product-queue in number of products
static const unsigned    PQ_PROD_CAPACITY = MEAN_NUM_PRODS;
// Number of data-products to insert
static const unsigned    NUM_PRODS = NUM_TIMES*MEAN_NUM_PRODS;
static const char        LOCAL_HOST[] = "127.0.0.1";
#if USE_SIGWAIT
static sigset_t          termSigSet;
#endif
static const char        UP7_PQ_PATHNAME[] = "up7_test.pq";
static const char        DOWN7_PQ_PATHNAME[] = "down7_test.pq";
static McastProdIndex    initialProdIndex;
static Sender            sender;
static Receiver          receiver;
static pthread_t         requesterThread;
static pqueue*           receiverPq;
static uint64_t          numDeletedProds;
static const unsigned short FMTP_MCAST_PORT = 5173; // From Wireshark plug-in

/*
 * The following functions (until otherwise noted) are only called once.
 */

#if !USE_SIGWAIT
static pthread_mutex_t   mutex;
static pthread_cond_t    cond;

static int
initCondAndMutex(void)
{
    int                 status;
    pthread_mutexattr_t mutexAttr;

    status = pthread_mutexattr_init(&mutexAttr);
    if (status) {
        log_errno_q(status, "Couldn't initialize mutex attributes");
    }
    else {
        (void)pthread_mutexattr_setprotocol(&mutexAttr, PTHREAD_PRIO_INHERIT);
        /*
         * Recursive in case `termSigHandler()` and `waitUntilDone()` execute
         * on the same thread
         */
        (void)pthread_mutexattr_settype(&mutexAttr, PTHREAD_MUTEX_RECURSIVE);

        if ((status = pthread_mutex_init(&mutex, &mutexAttr))) {
            log_errno_q(status, "Couldn't initialize mutex");
        }
        else {
            if ((status = pthread_cond_init(&cond, NULL))) {
                log_errno_q(status, "Couldn't initialize condition variable");
                (void)pthread_mutex_destroy(&mutex);
            }
        }

        (void)pthread_mutexattr_destroy(&mutexAttr);
    } // `mutexAttr` initialized

    return status;
}
#endif

static void signal_handler(
        int sig)
{
    switch (sig) {
    case SIGUSR1:
        log_notice_q("SIGUSR1");
        log_refresh();
        return;
    case SIGINT:
        log_notice_q("SIGINT");
        return;
    case SIGTERM:
        log_notice_q("SIGTERM");
        return;
    default:
        log_notice_q("Signal %d", sig);
        return;
    }
}

static int
setTermSigHandler(void)
{
    struct sigaction sigact;
    int              status = sigemptyset(&sigact.sa_mask);
    if (status == 0) {
        sigact.sa_flags = 0;
        sigact.sa_handler = signal_handler;
        status = sigaction(SIGINT, &sigact, NULL);
        if (status == 0) {
            status = sigaction(SIGTERM, &sigact, NULL);
            if (status == 0) {
                sigact.sa_flags = SA_RESTART;
                status = sigaction(SIGUSR1, &sigact, NULL);
            }
        }
    }
    return status;
}

/**
 * Only called once.
 */
static int
setup(void)
{
    /*
     * Ensure that the upstream component `up7` obtains the upstream queue from
     * `getQueuePath()`. This is not done for the downstream component because
     * `down7.c` implements an object-specific product-queue. The path-prefix of
     * the product-queue is also used to construct the pathname of the product-
     * index map (*.pim).
     */
    setQueuePath(UP7_PQ_PATHNAME);

    setLdmLogDir("."); // For LDM-7 receiver session-memory files (*.yaml)

    int status = msm_init();
    if (status) {
        log_add("Couldn't initialize multicast sender map");
    }
    else {
        msm_clear();

        /*
         * The following allows a SIGTERM to be sent to the process group
         * without affecting the parent process (e.g., a make(1)). Unfortunately,
         * it also prevents a `SIGINT` from terminating the process.
        (void)setpgrp();
         */

        #if USE_SIGWAIT
            (void)sigemptyset(&termSigSet);
            (void)sigaddset(&termSigSet, SIGINT);
            (void)sigaddset(&termSigSet, SIGTERM);
        #else
            status = initCondAndMutex();
        #endif

        status = setTermSigHandler();
        if (status) {
            log_add("Couldn't set termination signal handler");
        }
    }

    if (status)
        log_error_q("setup() failure");
    return status;
}

/**
 * Only called once.
 */
static int
teardown(void)
{
    msm_clear();
    msm_destroy();

    #if !USE_SIGWAIT
        (void)pthread_cond_destroy(&cond);
        (void)pthread_mutex_destroy(&mutex);
    #endif

    unlink(UP7_PQ_PATHNAME);
    unlink(DOWN7_PQ_PATHNAME);

    return 0;
}

/*
 * The following might be called multiple times.
 */

static void
blockSigCont(
        sigset_t* const oldSigSet)
{
    sigset_t newSigSet;
    (void)sigemptyset(&newSigSet);
    (void)sigaddset(&newSigSet, SIGCONT);
    int status = pthread_sigmask(SIG_BLOCK, &newSigSet, oldSigSet);
    CU_ASSERT_EQUAL_FATAL(status, 0);
}

static void
unblockSigCont(
        sigset_t* const oldSigSet)
{
    sigset_t newSigSet;
    (void)sigemptyset(&newSigSet);
    (void)sigaddset(&newSigSet, SIGCONT);
    int status = pthread_sigmask(SIG_UNBLOCK, &newSigSet, oldSigSet);
    CU_ASSERT_EQUAL_FATAL(status, 0);
}

static int
createEmptyProductQueue(
        const char* const pathname)
{
    pqueue* pq;
    int     status = pq_create(pathname, 0666, PQ_DEFAULT, 0, PQ_DATA_CAPACITY,
            PQ_PROD_CAPACITY, &pq); // PQ_DEFAULT => clobber existing

    if (status) {
        log_add_errno(status, "pq_create(\"%s\") failure", pathname);
    }
    else {
        status = pq_close(pq);
        if (status) {
            log_add("Couldn't close product-queue \"%s\"", pathname);
        }
    }
    return status;
}

static int
deleteProductQueue(
        const char* const pathname)
{
    return unlink(pathname);
}

#if !USE_SIGWAIT

#if 0
/**
 * Aborts the process due to an error in logic.
 */
static void abortProcess(void)
{
    log_error_q("Logic error");
    abort();
}

static void
lockMutex(void)
{
    log_debug_1("Locking mutex");
    int status = pthread_mutex_lock(&mutex);
    if (status) {
        log_errno_q(status, "Couldn't lock mutex");
        abortProcess();
    }
}

static void
unlockMutex(void)
{
    log_debug_1("Unlocking mutex");
    int status = pthread_mutex_unlock(&mutex);
    if (status) {
        log_errno_q(status, "Couldn't unlock mutex");
        abortProcess();
    }
}

static void
setDoneCondition(void)
{
    lockMutex();

    done = 1;
    log_debug_1("Signaling condition variable");
    int status = pthread_cond_broadcast(&cond);
    if (status) {
        log_errno_q(status, "Couldn't signal condition variable");
        abortProcess();
    }

    unlockMutex();
}

static void
termSigHandler(
        const int sig)
{
    log_debug_1("Caught signal %d", sig);
    setDoneCondition();
}

/**
 * @param[in]       newAction
 * @param[out]      oldAction
 * @retval 0        Success
 * @retval ENOTSUP  The SA_SIGINFO bit flag is set in the `sa_flags` field of
 *                  `newAction` and the implementation does not support either
 *                  the Realtime Signals Extension option, or the XSI Extension
 *                  option.
 */
static int
setTermSigHandling(
        struct sigaction* newAction,
        struct sigaction* oldAction)
{
    int              status;

    if (sigaction(SIGINT, newAction, oldAction) ||
            sigaction(SIGTERM, newAction, oldAction)) {
        log_syserr_q("sigaction() failure");
        status = errno;
    }
    else {
        status = 0;
    }

    return status;
}

static void
waitForDoneCondition(void)
{
    lockMutex();

    while (!done) {
        log_debug_1("Waiting on condition variable");
        int status = pthread_cond_wait(&cond, &mutex);
        if (status) {
            log_errno_q(status, "Couldn't wait on condition variable");
            abortProcess();
        }
    }

    unlockMutex();
}

static void
waitUntilDone(void)
{
    struct sigaction newAction;

    (void)sigemptyset(&newAction.sa_mask);
    newAction.sa_flags = 0;
    newAction.sa_handler = termSigHandler;

    struct sigaction oldAction;
    if (setTermSigHandling(&newAction, &oldAction)) {
        log_syserr_q("Couldn't set termination signal handling");
        abortProcess();
    }

    waitForDoneCondition();

    if (setTermSigHandling(&oldAction, NULL)) {
        log_syserr_q("Couldn't reset termination signal handling");
        abortProcess();
    }
}
#endif
#endif

/**
 * Closes the socket on failure.
 *
 * @param[in] up7        The upstream LDM-7 to be initialized.
 * @param[in] sock       The socket for the upstream LDM-7.
 * @param[in] termFd     Termination file-descriptor.
 * @retval    0          Success.
 */
static int
up7_init(
        Up7* const up7,
        const int  sock)
{
    /*
     * 0 => use default read/write buffer sizes.
     * `sock` will be closed by `svc_destroy()`.
     */
    SVCXPRT* const xprt = svcfd_create(sock, 0, 0);
    CU_ASSERT_PTR_NOT_EQUAL_FATAL(xprt, NULL);

    /*
     * Set the remote address in the RPC server-side transport because
     * `svcfd_create()` doesn't.
     */
    {
        struct sockaddr_in addr;
        socklen_t          addrLen = sizeof(addr);

        int status = getpeername(sock, &addr, &addrLen);
        CU_ASSERT_EQUAL_FATAL(status, 0);
        CU_ASSERT_EQUAL_FATAL(addrLen, sizeof(addr));
        CU_ASSERT_EQUAL_FATAL(addr.sin_family, AF_INET);
        xprt->xp_raddr = addr;
        xprt->xp_addrlen = addrLen;
    }

    // Last argument == 0 => don't register with portmapper
    bool success = svc_register(xprt, LDMPROG, 7, ldmprog_7, 0);
    CU_ASSERT_TRUE_FATAL(success);

    up7->xprt = xprt;

    return 0;
}

static void
funcCancelled(
        void* const arg)
{
    const char* funcName = (const char*)arg;
    log_notice_q("%s() thread cancelled", funcName);
}

/**
 * @param[in] up7    Upstream LDM-7.
 * @retval    0      Success. Connection was closed by downstream LDM-7.
 * @retval    EINTR  A signal was caught.
 * @retval    EIO    I/O error. `log_add()` called.
 * @retval    EAGAIN The allocation of internal data structures failed but a
 *                   subsequent request may succeed.
 * @post             `svc_destroy(up7->xprt)` will have been called.
 * @post             `up7->xprt == NULL`
 */
static int
up7_run(
        Up7* const up7)
{
    int                status;
    const int          sock = up7->xprt->xp_sock;
    char*              downAddrStr = ipv4Sock_getPeerString(sock);
    CU_ASSERT_PTR_NOT_NULL(downAddrStr);

    struct pollfd fds;
    fds.fd = sock;
    fds.events = POLLRDNORM;

    pthread_cleanup_push(funcCancelled, "up7_run");

    int cancelState;
    (void)pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &cancelState);

    for (;;) {
        log_debug_1("up7_run(): Calling poll()");
        status = poll(&fds, 1, -1); // `-1` => indefinite timeout

        if (0 > status) {
            if (errno != EINTR)
                log_add_syserr("up7_run(): poll() failure");
            status = errno;
            break;
        }
        if ((fds.revents & POLLERR) || (fds.revents & POLLNVAL)) {
            status = EIO;
            break;
        }
        if (fds.revents & POLLHUP) {
            status = 0;
            break;
        }
        if (fds.revents & POLLRDNORM) {
            log_debug_1("up7_run(): Calling svc_getreqsock()");
            svc_getreqsock(sock); // calls `ldmprog_7()`
        }
        if (!FD_ISSET(sock, &svc_fdset)) {
            log_notice_q("Connection with %s closed by RPC layer", downAddrStr);
            // `svc_destroy(up7->xprt)` was called by RPC layer.
            up7->xprt = NULL; // so others don't try to destroy it
            status = 0;
            break;
        }
    }

    /*
     * In order to play nice with the caller, the cancelability state is
     * reverted to its value on entry.
     */
    (void)pthread_setcancelstate(cancelState, &cancelState);

    pthread_cleanup_pop(0);
    free(downAddrStr);

    log_debug_1("up7_run(): Returning %d", status);
    return status;
}

static void
up7_destroy(
        Up7* const up7)
{
    svc_unregister(LDMPROG, 7);
    if (up7->xprt)
        svc_destroy(up7->xprt); // Closes socket
    up7->xprt = NULL;
}

static void
closeSocket(
        void* const arg)
{
    int sock = *(int*)arg;

    if (close(sock))
        log_syserr_q("Couldn't close socket %d", sock);
}

static void
destroyUp7(
        void* const arg)
{
    up7_destroy((Up7*)arg); // closes `sock`
}


static int
servlet_run(
        const int  servSock)
{
    /* NULL-s => not interested in receiver's address */
    int sock = accept(servSock, NULL, NULL);
    int status;

    CU_ASSERT_NOT_EQUAL_FATAL(sock, -1);

    Up7 up7;
    status = up7_init(&up7, sock);
    CU_ASSERT_EQUAL_FATAL(status, 0);

    pthread_cleanup_push(destroyUp7, &up7); {
        status = up7_run(&up7);
        CU_ASSERT(status == 0 || status == EINTR);
    } pthread_cleanup_pop(1); // Calls `svc_destroy(up7->xprt)`; closes `sock`

    log_debug_1("servlet_run(): Returning");
    return 0;
}

static void
freeLogging(
        void* const arg)
{
    log_free();
}

/**
 * Called by `pthread_create()`. The thread is cancelled by
 * `sender_terminate()`.
 *
 * @param[in] arg  Pointer to sender.
 * @retval    &0   Success. Input end of sender's termination pipe(2) is closed.
 */
static void*
sender_run(
        void* const arg)
{
    Sender* const sender = (Sender*)arg;
    static int    status;

    pthread_cleanup_push(freeLogging, NULL);

    const int     servSock = sender->sock;
    struct pollfd fds;
    fds.fd = servSock;
    fds.events = POLLIN;

    char* upAddr = ipv4Sock_getLocalString(servSock);
    log_info_q("Upstream LDM listening on %s", upAddr);
    free(upAddr);

    for (;;) {
        status = poll(&fds, 1, -1); // `-1` => indefinite timeout

        int cancelState;
        (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancelState);

        if (0 > status)
            break;
        if (fds.revents & POLLHUP) {
            status = 0;
            break;
        }
        if (fds.revents & POLLIN) {
            status = servlet_run(servSock);

            if (status) {
                log_add("servlet_run() failure");
                break;
            }
        }

        (void)pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &cancelState);
    } // `poll()` loop

    /* Because the current thread is ending: */
    if (status && !done) {
        log_flush_error();
    }
    else {
        log_clear(); // don't care about errors if termination requested
    }

    pthread_cleanup_pop(1); // calls `log_free()`

    log_debug_1("sender_run(): Returning &%d", status);
    return &status;
}

static int
senderSock_init(
        int* const sock)
{
    int status;
    int sck = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    CU_ASSERT_NOT_EQUAL_FATAL(sck, -1);

    int                on = 1;
    struct sockaddr_in addr;

    (void)setsockopt(sck, SOL_SOCKET, SO_REUSEADDR, (char*)&on, sizeof(on));

    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(LOCAL_HOST);
    addr.sin_port = htons(0); // let O/S assign port

    status = bind(sck, (struct sockaddr*)&addr, sizeof(addr));
    CU_ASSERT_EQUAL_FATAL(status, 0);


    status = listen(sck, 1);
    CU_ASSERT_EQUAL_FATAL(status, 0);

    *sock = sck;

    return 0;
}

/**
 * Starts executing the sender on a new thread.
 *
 * @retval    0       Success. `sender` is initialized and executing.
 */
static int
sender_spawn(void)
{
    int status = senderSock_init(&sender.sock);
    CU_ASSERT_EQUAL_FATAL(status, 0);

    status = pthread_create(&sender.thread, NULL, sender_run, &sender);
    CU_ASSERT_EQUAL_FATAL(status, 0);

    return 0;
}

/**
 * Returns the formatted address of a sender.
 *
 * @param[in] sender  The sender.
 * @return            Formatted address of the sender.
 */
static const char*
sender_getAddr(
        Sender* const sender)
{
    struct sockaddr_in addr;
    socklen_t          len = sizeof(addr);

    (void)getsockname(sender->sock, (struct sockaddr*)&addr, &len);

    return inet_ntoa(addr.sin_addr);
}

/**
 * Returns port number of a sender in host byte-order.
 *
 * @param[in] sender  The sender.
 * @return            Port number of the sender in host byte-order.
 */
static unsigned short
sender_getPort(
        Sender* const sender)
{
    struct sockaddr_in addr;
    socklen_t          len = sizeof(addr);

    (void)getsockname(sender->sock, (struct sockaddr*)&addr, &len);

    return ntohs(addr.sin_port);
}

static void
sender_insertProducts(void)
{
    int             status;
    product         prod;
    prod_info*      info = &prod.info;
    char            ident[80];
    void*           data = NULL;
    unsigned short  xsubi[3] = {(unsigned short)1234567890,
                               (unsigned short)9876543210,
                               (unsigned short)1029384756};
    struct timespec duration;

    info->feedtype = EXP;
    info->ident = ident;
    info->origin = "localhost";
    (void)memset(info->signature, 0, sizeof(info->signature));

    for (int i = 0; i < NUM_PRODS; i++) {
        const unsigned size = MAX_PROD_SIZE*erand48(xsubi) + 0.5;
        const ssize_t  nbytes = snprintf(ident, sizeof(ident), "%d", i);

        CU_ASSERT_TRUE_FATAL(nbytes >= 0 && nbytes < sizeof(ident));
        status = set_timestamp(&info->arrival);
        CU_ASSERT_EQUAL_FATAL(status, 0);
        // Signature == sequence number
        info->seqno = i;
        uint32_t signet = htonl(i); // decoded in `requester_decide()`
        (void)memcpy(info->signature+sizeof(signaturet)-sizeof(signet), &signet,
                sizeof(signet));
        info->sz = size;

        data = realloc(data, size);
        CU_ASSERT_PTR_NOT_NULL(data);
        prod.data = data;

        status = pq_insert(pq, &prod);
        CU_ASSERT_EQUAL_FATAL(status, 0);
        char buf[LDM_INFO_MAX];
        log_info_q("Inserted: prodInfo=\"%s\"",
                s_prod_info(buf, sizeof(buf), info, 1));

        duration.tv_sec = 0;
        duration.tv_nsec = INTER_PRODUCT_INTERVAL;
        while (nanosleep(&duration, &duration))
            ;
    }

    free(data);
}

static int
setMcastInfo(
        McastInfo** const mcastInfo,
        const feedtypet   feedtype)
{
    ServiceAddr* mcastServAddr;
    int          status = sa_new(&mcastServAddr, "224.0.0.1", FMTP_MCAST_PORT);

    if (status) {
        log_add("Couldn't create multicast service address object");
    }
    else {
        ServiceAddr* ucastServAddr;

        status = sa_new(&ucastServAddr, LOCAL_HOST, 0); // O/S selects port
        if (status) {
            log_add("Couldn't create unicast service address object");
        }
        else {
            status = mi_new(mcastInfo, feedtype, mcastServAddr, ucastServAddr);
            if (status) {
                log_add("Couldn't create multicast information object");
            }
            else {
                sa_free(ucastServAddr);
                sa_free(mcastServAddr);
            }
        }
    }

    return status;
}

static int
sender_start(
        const feedtypet feedtype)
{
    // Ensure that the first product-index will be 0
    int status = pim_delete(NULL, feedtype);
    log_flush_error();
    CU_ASSERT_EQUAL_FATAL(status, 0);
    status = createEmptyProductQueue(UP7_PQ_PATHNAME);
    if (status) {
        log_add("Couldn't create empty product queue \"%s\"",
                UP7_PQ_PATHNAME);
    }
    else {
        /*
         * The product-queue must be thread-safe because there are 2 threads:
         * - The upstream LDM7 server; and
         * - The product-insertion thread.
         */
        status = pq_open(getQueuePath(), PQ_THREADSAFE, &pq);
        if (status) {
            log_add("Couldn't open product-queue \"%s\"", getQueuePath());
        }
        else {
            McastInfo* mcastInfo;

            status = setMcastInfo(&mcastInfo, feedtype);
            if (status) {
                log_add("Couldn't set multicast information");
            }
            else {
                status = umm_clear();
                VcEndPoint* vcEnd = vcEndPoint_new(1, "Switch ID", "Port ID");
                CU_ASSERT_PTR_NOT_NULL(vcEnd);
                in_addr_t subnet;
                CU_ASSERT_EQUAL(inet_pton(AF_INET, LOCAL_HOST, &subnet), 1);
                CidrAddr*   fmtpSubnet = cidrAddr_new(subnet, 24);
                CU_ASSERT_PTR_NOT_NULL(fmtpSubnet);
                status = umm_addPotentialSender(mcastInfo, 2, vcEnd, fmtpSubnet,
                        UP7_PQ_PATHNAME);
                if (status) {
                    log_add("mlsm_addPotentialSender() failure");
                }
                else {
                    // Starts the sender on a new thread
                    char* mcastInfoStr = mi_format(mcastInfo);
                    char* fmtpSubnetStr = cidrAddr_format(fmtpSubnet);
                    log_notice_q("Starting up: pq=%s, mcastInfo=%s, "
                            "vcEnd=%s, subnet=%s", getQueuePath(),
                            mcastInfoStr, vcEndPoint_format(vcEnd),
                            fmtpSubnetStr);
                    free(fmtpSubnetStr);
                    free(mcastInfoStr);
                    status = sender_spawn();
                    if (status) {
                        log_add("Couldn't spawn sender");
                    }
                    else {
                        done = 0;
                    }
                }
                cidrAddr_delete(fmtpSubnet);
                vcEndPoint_delete(vcEnd);
                mi_free(mcastInfo);
            } // `mcastInfo` allocated

            if (status)
                (void)pq_close(pq);
        } // Product-queue open

        if (status)
            (void)deleteProductQueue(UP7_PQ_PATHNAME);
    } // empty product-queue created

    return status;
}

/**
 * @retval 0           Success.
 * @retval LDM7_INVAL  No multicast sender child process exists.
 */
static int
terminateMcastSender(void)
{
    int status;

#if 0
    /*
     * Terminate the multicast sender process by sending a SIGTERM to the
     * process group. If this program is executed stand-alone or within Eclipse,
     * then the resulting process will be a process group leader.
     */
    {
        struct sigaction oldSigact;
        struct sigaction newSigact;
        status = sigemptyset(&newSigact.sa_mask);
        CU_ASSERT_EQUAL_FATAL(status, 0);

        log_debug_1("Setting SIGTERM action to ignore");
        newSigact.sa_flags = 0;
        newSigact.sa_handler = SIG_IGN;
        status = sigaction(SIGTERM, &newSigact, &oldSigact);
        CU_ASSERT_EQUAL_FATAL(status, 0);

        log_debug_1("Sending SIGTERM to process group");
        status = kill(0, SIGTERM);
        CU_ASSERT_EQUAL_FATAL(status, 0);

        log_debug_1("Restoring SIGTERM action");
        status = sigaction(SIGTERM, &oldSigact, NULL);
        CU_ASSERT_EQUAL(status, 0);
    }
#else
    {
        pid_t pid = umm_getMldmSenderPid();
        if (pid) {
            log_debug_1("Sending SIGTERM to multicast LDM sender process");
            status = kill(pid, SIGTERM);
            CU_ASSERT_EQUAL_FATAL(status, 0);
        }
    }
#endif

    /* Reap the terminated multicast sender. */
    {
        log_debug_1("Reaping multicast sender child process");
        const pid_t wpid = wait(&status);
        if (wpid == (pid_t)-1) {
            CU_ASSERT_EQUAL(errno, ECHILD);
            status = LDM7_INVAL;
        }
        else {
            CU_ASSERT_TRUE_FATAL(wpid > 0);
            CU_ASSERT_TRUE(WIFEXITED(status));
            CU_ASSERT_EQUAL(WEXITSTATUS(status), 0);
            status = umm_terminated(wpid);
            CU_ASSERT_EQUAL(status, 0);
        }
    }

    return status;
}

/**
 * @retval LDM7_INVAL  No multicast sender child process exists.
 */
static int
sender_stop(void)
{
    int retval = 0;
    int status;

    #if CANCEL_SENDER
        log_debug_1("Canceling sender TCP server thread");
        status = pthread_cancel(sender.thread);
        if (status) {
            log_errno_q(status, "Couldn't cancel sender thread");
            retval = status;
        }
    #else
        log_debug_1("Writing to termination pipe");
        status = write(sender.fds[1], &status, sizeof(int));
        if (status == -1) {
            log_syserr_q("Couldn't write to termination pipe");
            retval = status;
        }
    #endif

    void* statusPtr;

    log_debug_1("Joining sender TCP server thread");
    status = pthread_join(sender.thread, &statusPtr);
    if (status) {
        log_errno_q(status, "Couldn't join sender thread");
        retval = status;
    }

    if (statusPtr != PTHREAD_CANCELED) {
        status = *(int*)statusPtr;
        if (status) {
            log_add("Sender task exit-status was %d", status);
            retval = status;
        }
    }

   (void)close(sender.sock);

    log_debug_1("Terminating multicast LDM sender");
    status = terminateMcastSender();
    if (status) {
        log_add("Couldn't terminate multicast sender process");
        retval = status;
    }

    log_debug_1("Clearing multicast LDM sender manager");
    status = umm_clear();
    if (status) {
        log_add("mlsm_clear() failure");
        retval = status;
    }

    status = pq_close(pq);
    if (status) {
        log_add("pq_close() failure");
        retval = status;
    }

#if 0
    status = deleteProductQueue(UP7_PQ_PATHNAME);
    if (status) {
        log_add("deleteProductQueue() failure");
        retval = status;
    }
#endif

    return retval;
}

static void
requester_close(
        void* const arg)
{
    log_flush_error(); // To log any pending messages
    log_free();
}

typedef struct {
    signaturet sig;
    bool       delete;
} RequestArg;

static void
decide(
        RequestArg* const reqArg,
        const signaturet  sig)
{
    static unsigned short    xsubi[3] = {(unsigned short)1234567890,
                                         (unsigned short)9876543210,
                                         (unsigned short)1029384756};
    if (erand48(xsubi) >= REQUEST_RATE) {
        reqArg->delete = false;
    }
    else {
        reqArg->delete = true;
        (void)memcpy(reqArg->sig, sig, sizeof(signaturet));
    }
}

static inline int // inline because only called in one place
requester_decide(
        const prod_info* const restrict info,
        const void* const restrict      data,
        void* const restrict            xprod,
        const size_t                    size,
        void* const restrict            arg)
{
    char infoStr[LDM_INFO_MAX];
    log_debug_1("requester_decide(): Entered: info=\"%s\"",
            s_prod_info(infoStr, sizeof(infoStr), info, 1));
    static FmtpProdIndex maxProdIndex;
    static bool          maxProdIndexSet = false;
    FmtpProdIndex        prodIndex;
    RequestArg* const    reqArg = (RequestArg*)arg;

    /*
     * The monotonicity of the product-index is checked so that only the most
     * recently-created data-product is eligible for deletion.
     *
     * Product index == sequence number == signature
     */
    prodIndex = info->seqno;
    if (maxProdIndexSet && prodIndex <= maxProdIndex) {
        reqArg->delete = false;
    }
    else {
        decide(reqArg, info->signature);
        maxProdIndex = prodIndex;
        maxProdIndexSet = true;
    }

    char buf[2*sizeof(signaturet)+1];
    sprint_signaturet(buf, sizeof(buf), info->signature);
    log_debug_1("requester_decide(): Returning %s: prodIndex=%lu",
            reqArg->delete ? "delete" : "don't delete",
            (unsigned long)prodIndex);
    return 0; // necessary for `pq_sequence()`
}

/**
 * @retval    0            Success.
 * @retval    PQ_CORRUPT   The product-queue is corrupt.
 * @retval    PQ_LOCKED    The data-product was found but is locked by another
 *                         process.
 * @retval    PQ_NOTFOUND  The data-product wasn't found.
 * @retval    PQ_SYSTEM    System error. Error message logged.
 */
static inline int // inline because only called in one place
requester_deleteAndRequest(
        const signaturet sig)
{
    FmtpProdIndex  prodIndex;
    (void)memcpy(&prodIndex, sig + sizeof(signaturet) - sizeof(FmtpProdIndex),
        sizeof(FmtpProdIndex));
    prodIndex = ntohl(prodIndex); // encoded in `sender_insertProducts()`
    int status = pq_deleteBySignature(receiverPq, sig);
    char buf[2*sizeof(signaturet)+1];
    if (status) {
        (void)sprint_signaturet(buf, sizeof(buf), sig);
        log_error_q("Couldn't delete data-product: pq=%s, prodIndex=%lu, sig=%s",
                pq_getPathname(receiverPq), (unsigned long)prodIndex,
                buf);
    }
    else {
        if (log_is_enabled_info) {
            (void)sprint_signaturet(buf, sizeof(buf), sig);
            log_info_q("Deleted data-product: prodIndex=%lu, sig=%s",
                    (unsigned long)prodIndex, buf);
        }
        numDeletedProds++;
        down7_missedProduct(receiver.down7, prodIndex);
    }
    return status;
}

/**
 * Executes a requester to test the "backstop" mechanism. Selected data-products
 * are deleted from the downstream product-queue and then requested from the
 * upstream LDM.
 *
 * Called by `pthread_create()`.
 *
 * @retval NULL  Always
 */
static void*
requester_start(
        void* const arg)
{
    log_debug_1("requester_start(): Entered");

    int           status;
    pthread_cleanup_push(requester_close, NULL);

    for (;;) {
        RequestArg reqArg;
        status = pq_sequence(receiverPq, TV_GT, PQ_CLASS_ALL, requester_decide,
                &reqArg);
        if (status == PQUEUE_END) {
            (void)pq_suspend(30); // Unblocks SIGCONT
        }
        else if (status) {
            log_add("pq_sequence() failure: status=%d", status);
            break;
        }
        else if (reqArg.delete) {
            /*
             * The data-product is deleted here rather than in
             * `requester_decide()` because in that function, the
             * product's region is locked, deleting it attempts to lock it
             * again, and deadlock results.
             */
            int cancelState;
            pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancelState);
            status = requester_deleteAndRequest(reqArg.sig);
            pthread_setcancelstate(cancelState, &cancelState);
            if (status) {
                log_add("requester_deleteAndRequest() failure: status=%d",
                        status);
                break;
            }
        }
    }
    pthread_cleanup_pop(1);
    if (status)
        log_flush_error(); // Because end-of-thread
    log_debug_1("requester_start(): Returning");
    return NULL;
}

/**
 * Starts a data-product requester thread to test the "backstop" mechanism.
 * Selected data-products are deleted from the downstream product-queue and then
 * requested from the upstream LDM.
 *
 * @return pthread_create() return value
 */
static int
requester_init(void)
{
    int status = pthread_create(&requesterThread, NULL, requester_start, NULL);
    CU_ASSERT_EQUAL_FATAL(status, 0);
#if 0
    status = pthread_detach(requesterThread);
    CU_ASSERT_EQUAL_FATAL(status, 0);
#endif
    return status;
}

static int
requester_destroy(void)
{
    int status = pthread_cancel(requesterThread);
    CU_ASSERT_EQUAL_FATAL(status, 0);
    status = pthread_join(requesterThread, NULL);
    CU_ASSERT_EQUAL_FATAL(status, 0);
    return status;
}

/**
 * Runs a receiver on the current thread. Called by `pthread_create()`.
 *
 * @param[in] arg   Pointer to receiver object
 * @return    NULL  Always
 */
static void*
receiver_run(
        void* const arg)
{
    Receiver* const receiver = (Receiver*)arg;
    receiver->status = down7_start(receiver->down7);
    // Because at end of thread:
    log_log_q(receiver->status == LDM7_OK ? LOG_LEVEL_NOTICE : LOG_LEVEL_ERROR,
            "Receiver terminated");
    log_free();
    return NULL;
}

/**
 * Initializes the receiver. Doesn't execute it.
 *
 * @param[in]     addr      Address of sender: either hostname or IPv4 address.
 * @param[in]     port      Port number of sender in host byte-order.
 * @param[in]     feedtype  The feedtype to which to subscribe.
 * @retval        0         Success. `*receiver` is initialized.
 */
static int
receiver_init(
        const char* const restrict addr,
        const unsigned short       port,
        const feedtypet            feedtype)
{
    int status = createEmptyProductQueue(DOWN7_PQ_PATHNAME);
    if (status) {
        log_add("Couldn't create empty product queue \"%s\"",
                DOWN7_PQ_PATHNAME);
    }
    else {
        status = pq_open(DOWN7_PQ_PATHNAME, PQ_THREADSAFE, &receiverPq);
        CU_ASSERT_EQUAL_FATAL(status, 0);

        ServiceAddr* servAddr;
        status = sa_new(&servAddr, addr, port);
        CU_ASSERT_EQUAL_FATAL(status, 0);

        // Delete the multicast LDM receiver's session memory.
        bool success = mrm_delete(servAddr, feedtype);
        CU_ASSERT_EQUAL_FATAL(success, true);

        numDeletedProds = 0;

        VcEndPoint* vcEnd = vcEndPoint_new(1, "Switch ID", "Port ID");
        CU_ASSERT_PTR_NOT_NULL(vcEnd);
        receiver.down7 = down7_new(servAddr, feedtype, LOCAL_HOST, vcEnd,
                receiverPq);
        CU_ASSERT_PTR_NOT_NULL_FATAL(receiver.down7);
        vcEndPoint_delete(vcEnd);
        sa_free(servAddr);

        status = requester_init();
        if (status) {
            log_add("Couldn't initialize requester");
        }

        receiver.status = LDM7_OK;
    }

    return status;
}

/**
 * Destroys the receiver.
 */
static void
receiver_destroy(void)
{
    int status;

    log_debug_1("Calling down7_free()");
    status = down7_free(receiver.down7);
    CU_ASSERT_EQUAL(status, 0);
    log_flush_error();

    status = requester_destroy();
    CU_ASSERT_EQUAL(status, 0);

    status = pq_close(receiverPq);
    CU_ASSERT_EQUAL(status, 0);

#if 0
    status = deleteProductQueue(DOWN7_PQ_PATHNAME);
    CU_ASSERT_EQUAL(status, 0);
    log_flush_error();
#endif
}

/**
 * Starts the receiver on a new thread.
 *
 * @param[in]  addr      Address of sender: either hostname or IPv4 address.
 * @param[in]  port      Port number of sender in host byte-order.
 * @param[in]  feedtype  The feedtype to which to subscribe.
 * @retval 0   Success.
 */
static int
receiver_start(
        const char* const restrict addr,
        const unsigned short       port,
        const feedtypet            feedtype)
{
    int status = receiver_init(addr, port, feedtype);
    if (status) {
        log_add("Couldn't initialize receiver");
    }
    else {
        status = pthread_create(&receiver.thread, NULL, receiver_run,
                &receiver);
        CU_ASSERT_EQUAL_FATAL(status, 0);
    }

    return status;
}

/**
 * Stops the receiver.
 *
 * @retval        LDM7_LOGIC     No prior call to `receiver_start()`.
 *                               `log_add()` called.
 * @retval        LDM7_MCAST     Multicast layer failure. `log_add()` called.
 * @retval        LDM7_SHUTDOWN  Shutdown requested
 * @retval        LDM7_SYSTEM    System error. `log_add()` called.
 */
static int
receiver_stop(void)
{
    log_debug_1("Calling down7_stop()");
    int status = down7_stop(receiver.down7);
    CU_ASSERT_EQUAL(status, 0);

    log_debug_1("Joining receiver thread");
    status = pthread_join(receiver.thread, NULL);
    CU_ASSERT_EQUAL(status, 0);
    log_flush_error();

    receiver_destroy();
    log_flush_error();

    return receiver.status;
}

static void
receiver_requestLastProduct(
        Receiver* const receiver)
{
    down7_missedProduct(receiver->down7, initialProdIndex + NUM_PRODS - 1);
}

static int
receiver_deleteAllProducts(
        Receiver* const receiver)
{
    int status;

    do
        status = pq_seqdel(down7_getPq(receiver->down7), TV_GT, PQ_CLASS_ALL, 0,
                NULL, NULL);
    while (status == 0);

    return status; // should be `PQ_END`
}

/**
 * @retval 0  Success
 */
static uint64_t
receiver_getNumProds(
        Receiver* const receiver)
{
    return down7_getNumProds(receiver->down7);
}

static long receiver_getPqeCount(
        Receiver* const receiver)
{
    return down7_getPqeCount(receiver->down7);
}

static void
test_up7(
        void)
{
    int status = sender_start(ANY);
    log_flush_error();
    CU_ASSERT_EQUAL_FATAL(status, 0);

    sleep(1);
    done = 1;

    status = sender_stop();
    CU_ASSERT_EQUAL(status, LDM7_INVAL);
    log_clear();
}

static void
test_down7(
        void)
{
    int      status;

    done = 0;

    /* Starts a receiver on a new thread */
    status = receiver_start(LOCAL_HOST, FMTP_MCAST_PORT, ANY);
    CU_ASSERT_EQUAL_FATAL(status, 0);

#if 1
    sleep(1);
    done = 1;
#else
    waitUntilDone();
#endif

    status = receiver_stop();
    CU_ASSERT_EQUAL(status, LDM7_REFUSED);
}

static void
test_bad_subscription(
        void)
{
    int      status = sender_start(NEXRAD2);
    log_flush_error();
    CU_ASSERT_EQUAL_FATAL(status, 0);

#if 0
    status = receiver_init(sender_getAddr(&sender), sender_getPort(&sender),
            NGRID);
    log_flush_error();
    CU_ASSERT_EQUAL_FATAL(status, 0);
    status = down7_start(receiver.down7);
    CU_ASSERT_EQUAL(status, LDM7_INVAL);

    receiver_destroy();
#else
    status = receiver_start(sender_getAddr(&sender), sender_getPort(&sender),
            NGRID);
    CU_ASSERT_EQUAL_FATAL(status, 0);
    sleep(1);
    status = receiver_stop();
#endif
    CU_ASSERT_EQUAL(status, LDM7_UNAUTH);

    log_debug_1("Terminating sender");
    status = sender_stop();
    CU_ASSERT_EQUAL(status, LDM7_INVAL);
    log_clear();
}

static void
test_up7_down7(
        void)
{
    int status;

    // Block pq-used `SIGALRM` and `SIGCONT` to prevent `sleep()` returning
    struct sigaction sigAction, prevSigAction;
    sigset_t         sigMask, prevSigMask;
    sigemptyset(&sigMask);
    sigaddset(&sigMask, SIGALRM);
    sigaddset(&sigMask, SIGCONT); // No effect if all threads block
    status = pthread_sigmask(SIG_BLOCK, &sigMask, &prevSigMask);
    CU_ASSERT_EQUAL(status, 0);
    /*
    sigset_t oldSigSet;
    blockSigCont(&oldSigSet);
    */

    const float retxTimeout = (MEAN_RESIDENCE_TIME/2.0) / 60.0;
    umm_setRetxTimeout(retxTimeout);

    status = sender_start(ANY); // Doesn't block
    log_flush_error();
    CU_ASSERT_EQUAL_FATAL(status, 0);

    host_set* hostSet = lcf_newHostSet(HS_DOTTED_QUAD, "127.0.0.1", NULL);
    CU_ASSERT_PTR_NOT_NULL_FATAL(hostSet);
    ErrorObj* errObj = lcf_addAllow(ANY, hostSet, ".*", NULL);
    CU_ASSERT_PTR_NULL_FATAL(errObj);

    /* Starts a receiver on a new thread */
    status = receiver_start(sender_getAddr(&sender), sender_getPort(&sender),
            ANY);
    log_flush_error();
    CU_ASSERT_EQUAL(status, 0);

    (void)sleep(2);

    sender_insertProducts();

#if 0
    (void)sleep(1);
    receiver_requestLastProduct(&receiver);
    (void)sleep(1);
    status = receiver_deleteAllProducts(&receiver);
    log_flush_error();
    CU_ASSERT_EQUAL(status, PQ_END);
    receiver_requestLastProduct(&receiver);
#endif
    //(void)sleep(180);
    log_notice_q("%lu sender product-queue insertions", (unsigned long)NUM_PRODS);
    uint64_t numDownInserts = receiver_getNumProds(&receiver);
    log_notice_q("%lu product deletions", (unsigned long)numDeletedProds);
    log_notice_q("%lu receiver product-queue insertions",
            (unsigned long)numDownInserts);
    log_notice_q("%ld outstanding product reservations",
            receiver_getPqeCount(&receiver));
    CU_ASSERT_EQUAL(numDownInserts - numDeletedProds, NUM_PRODS);

    #if USE_SIGWAIT
        (void)sigwait(&termSigSet, &status);
        done = 1;
    #elif 0
        waitUntilDone();
        log_flush_error();
        CU_ASSERT_EQUAL_FATAL(status, 0);
    #else
        unsigned remaining;
        #if 1
            remaining = sleep(2);
        #else
            remaining = sleep(UINT_MAX);
        #endif
        CU_ASSERT_EQUAL_FATAL(remaining, 0);
    #endif

    log_debug_1("Stopping receiver");
    status = receiver_stop();
    CU_ASSERT_EQUAL(status, LDM7_OK);

    log_debug_1("Stopping sender");
    status = sender_stop();
    CU_ASSERT_EQUAL(status, 0);

    /*
    status = pthread_sigmask(SIG_SETMASK, &oldSigSet, NULL);
    CU_ASSERT_EQUAL(status, 0);
    */

    status = pthread_sigmask(SIG_SETMASK, &prevSigMask, NULL);
    CU_ASSERT_EQUAL(status, 0);
}

int main(
        const int argc,
        const char* const * argv)
{
    int status = 1;

    (void)log_init(argv[0]);
    log_set_level(LOG_LEVEL_DEBUG);

    if (CUE_SUCCESS == CU_initialize_registry()) {
        CU_Suite* testSuite = CU_add_suite(__FILE__, setup, teardown);

        if (NULL != testSuite) {
            if (CU_ADD_TEST(testSuite, test_up7) &&
                    CU_ADD_TEST(testSuite, test_down7) &&
                    CU_ADD_TEST(testSuite, test_bad_subscription) &&
                    CU_ADD_TEST(testSuite, test_up7_down7)
                    ) {
                CU_basic_set_mode(CU_BRM_VERBOSE);
                (void) CU_basic_run_tests();
            }
        }

        status = CU_get_number_of_tests_failed();
        CU_cleanup_registry();
    }

    log_flush_error();
    log_free();

    return status;
}
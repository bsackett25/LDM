/**
 * Copyright 2018 University Corporation for Atmospheric Research. All rights
 * reserved. See the the file COPYRIGHT in the top-level source-directory for
 * licensing conditions.
 *
 *   @file: Up7Down7_test.c
 * @author: Steven R. Emmerson
 *
 * This file tests an upstream LDM-7 sending to a downstream LDM-7.
 */

#include "config.h"

#include "down7.h"
#include "error.h"
#include "Executor.h"
#include "globals.h"
#include "inetutil.h"
#include "LdmConfFile.h"
#include "ldmprint.h"
#include "log.h"
#include "mcast_info.h"
#include "mldm_sender_map.h"
#include "MldmRcvrMemory.h"
#include "pq.h"
#include "prod_index_map.h"
#include "registry.h"
#include "Thread.h"
#include "timestamp.h"
#include "up7.h"
#include "UpMcastMgr.h"
#include "VirtualCircuit.h"

#include <arpa/inet.h>
#include <assert.h>
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

#ifndef MAX
    #define MAX(a,b) ((a) >= (b) ? (a) : (b))
#endif

typedef struct {
    SVCXPRT*              xprt;
} MyUp7;

typedef struct {
    pthread_mutex_t       mutex;
    pthread_cond_t        cond;
    Future*               future;
    MyUp7*                myUp7;
    int                   sock;
    bool                  executing;
    volatile sig_atomic_t done;
} Sender;

typedef struct {
    pthread_mutex_t mutex;
    bool            done;
} Requester;

typedef struct {
    Requester             requester;
    Future*               down7Future;
    Future*               requesterFuture;
    McastReceiverMemory*  mrm;
} Receiver;

/*
 * Proportion of data-products that the receiving LDM-7 will delete from the
 * product-queue and request from the sending LDM-7 to simulate network
 * problems.
 */
#define                  REQUEST_RATE 0.1
// Maximum size of a data-product in bytes
#define                  MAX_PROD_SIZE 1000000
// Approximate number of times the product-queue will be "filled".
#define                  NUM_TIMES 2
// Duration, in nanoseconds, before the next product is inserted (i.e., gap
// duration)
#define                  INTER_PRODUCT_INTERVAL 500000000 // 500 ms
/*
 * Mean residence-time, in seconds, of a data-product. Also used to compute the
 * FMTP retransmission timeout.
 */
#define                  MEAN_RESIDENCE_TIME 10

// Derived values:

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
static const int         UP7_PORT = 38800;
static const char        UP7_PQ_PATHNAME[] = "up7_test.pq";
static const char        DOWN7_PQ_PATHNAME[] = "down7_test.pq";
static pqueue*           receiverPq;
static uint64_t          numDeletedProds;
static ServiceAddr*      up7Addr;
static VcEndPoint*       localVcEnd;
static Executor*         executor;
static sigset_t          mostSigMask;

static void sigHandler(
        int sig)
{
    switch (sig) {
    case SIGCHLD:
        log_notice("SIGCHLD");
        return;
    case SIGCONT:
        log_notice("SIGCONT");
        return;
    case SIGIO:
        log_notice("SIGIO");
        return;
    case SIGPIPE:
        log_notice("SIGPIPE");
        return;
    case SIGINT:
        log_notice("SIGINT");
        return;
    case SIGTERM:
        log_notice("SIGTERM");
        return;
    case SIGHUP:
        log_notice("SIGHUP");
        return;
    case SIGUSR1:
        log_notice("SIGUSR1");
        log_refresh();
        return;
    case SIGUSR2:
        log_notice("SIGUSR2");
        log_refresh();
        return;
    default:
        log_notice("Unexpected signal %d", sig);
        return;
    }
}

static int
setSignalHandling(void)
{
    static const int interuptSigs[] = { SIGIO, SIGPIPE, SIGINT, SIGTERM,
            SIGHUP};
    static const int restartSigs[] = { SIGCHLD, SIGCONT, SIGUSR1, SIGUSR2 };
    int              status;
    struct sigaction sigact = {}; // Zero initialization

    /*
     * While handling a signal, block all signals except ones that would cause
     * undefined behavior
     */
    sigact.sa_mask = mostSigMask;

    // Catch all following signals
    sigact.sa_handler = sigHandler;

    // Interrupt system calls for these signals
    for (int i = 0; i < sizeof(interuptSigs)/sizeof(interuptSigs[0]); ++i) {
        status = sigaction(interuptSigs[i], &sigact, NULL);
        // `errno == EINVAL` for `SIGKILL` & `SIGSTOP`, at least
        assert(status == 0 || errno == EINVAL);
    }

    // Restart system calls for these signals
    sigact.sa_flags = SA_RESTART;
    for (int i = 0; i < sizeof(restartSigs)/sizeof(restartSigs[0]); ++i) {
        status = sigaction(restartSigs[i], &sigact, NULL);
        assert(status == 0);
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
     * Ensure that the upstream module `up7` obtains the upstream queue from
     * `getQueuePath()`. This is not done for the downstream module because
     * `down7.c` implements an object-specific product-queue. The path-prefix of
     * the product-queue is also used to construct the pathname of the product-
     * index map (*.pim).
     */
    setQueuePath(UP7_PQ_PATHNAME);

    setLdmLogDir("."); // For LDM-7 receiver session-memory files (*.yaml)

    /*
     * The following allows a SIGTERM to be sent to the process group
     * without affecting the parent process (e.g., a make(1)). Unfortunately,
     * it also prevents a `SIGINT` from terminating the process.
    (void)setpgrp();
     */

    /*
     * Create a signal mask that would block all signals except those that
     * would cause undefined behavior
     */
    const int undefSigs[] = { SIGFPE, SIGILL, SIGSEGV, SIGBUS };
    int       status = sigfillset(&mostSigMask);
    assert(status == 0);
    for (int i = 0; i < sizeof(undefSigs)/sizeof(undefSigs[0]); ++i) {
        status = sigdelset(&mostSigMask, undefSigs[i]);
        assert(status == 0);
    }

    status = setSignalHandling();
    if (status) {
        log_add("Couldn't set termination signal handler");
    }
    else {
        status = sa_new(&up7Addr, LOCAL_HOST, UP7_PORT);

        if (status) {
            log_add_errno(status,
                    "Couldn't construct upstream LDM7 service address");
        }
        else {
            localVcEnd = vcEndPoint_new(1, NULL, NULL);

            if (localVcEnd == NULL) {
                log_add("Couldn't construct local virtual-circuit endpoint");
                status = LDM7_SYSTEM;
            }
            else {
                executor = executor_new();

                if (executor == NULL) {
                    vcEndPoint_free(localVcEnd);
                    log_add("Couldn't create new execution service");
                    status = LDM7_SYSTEM;
                }
            } // `localVcEnd` created

            if (status)
                sa_free(up7Addr);
        } // `up7Addr` created
    } // Termination signal handler installed

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
    executor_free(executor);
    vcEndPoint_free(localVcEnd);
    sa_free(up7Addr);

    unlink(UP7_PQ_PATHNAME);
    unlink(DOWN7_PQ_PATHNAME);

    reg_close();

    return 0;
}

#if 0
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
#endif

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

/**
 * @param[in] up7        The upstream LDM-7 to be initialized.
 * @param[in] sock       The socket for the upstream LDM-7.
 * @param[in] termFd     Termination file-descriptor.
 */
static void
myUp7_init(
        MyUp7* const myUp7,
        const int    sock)
{
    /*
     * 0 => use default read/write buffer sizes.
     * `sock` will be closed by `svc_destroy()`.
     */
    SVCXPRT* const xprt = svcfd_create(sock, 0, 0);
    CU_ASSERT_PTR_NOT_NULL_FATAL(xprt);

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
    CU_ASSERT_TRUE_FATAL(svc_register(xprt, LDMPROG, 7, ldmprog_7, 0));

    myUp7->xprt = xprt;
}

static void
myUp7_destroy(MyUp7* const myUp7)
{
    svc_unregister(LDMPROG, 7);

    if (myUp7->xprt) // Might have been destroyed by RPC layer
        svc_destroy(myUp7->xprt);
}

/**
 * Returns a new upstream LDM7.
 *
 * @param[in] sock  Socket descriptor with downstream LDM7
 * @return          New upstream LDM7
 * @see `myUp7_delete()`
 */
static MyUp7*
myUp7_new(int sock)
{
    MyUp7* myUp7 = malloc(sizeof(MyUp7));
    CU_ASSERT_PTR_NOT_NULL_FATAL(myUp7);
    myUp7_init(myUp7, sock);
    return myUp7;
}

/**
 * Deletes an upstream LDM7 instance. Inverse of `myUp7_new()`.
 *
 * @param[in] myUp7  Upstream LDM7
 * @see `myUp7_new()`
 */
static void
myUp7_free(MyUp7* const myUp7)
{
    myUp7_destroy(myUp7);
    free(myUp7);
}

/**
 * Always destroys the server-side RPC transport.
 *
 * @param[in] up7          Upstream LDM-7.
 */
static void
myUp7_run(MyUp7* const myUp7)
{
    const int sock = myUp7->xprt->xp_sock;

#if 1
    // The following is taken from "ldmd.c:childLdm_run()"

    const unsigned TIMEOUT = 2*interval;

    int status = one_svc_run(sock, TIMEOUT);

    if (status == ECONNRESET) {
        log_add("Connection with LDM client lost");
        // one_svc_run() called svc_getreqset(), which called svc_destroy()
    }
    else {
        if (status == ETIMEDOUT)
            log_add("Connection from client LDM silent for %u seconds",
                    TIMEOUT);

        svc_destroy(myUp7->xprt);
        myUp7->xprt = NULL;
    }
#else
    int       status;
    struct    pollfd fds;

    fds.fd = sock;
    fds.events = POLLRDNORM;

    umm_setWrkGrpName("UCAR LDM7");

    /**
     * The `up7.c` module needs to tell this function to return when a error
     * occurs that prevents continuation. The following mechanisms were
     * considered:
     * - Using a thread-specific signaling value. This was rejected because
     *   that would increase the coupling between this function and the
     *   `up7.c` module by
     *   - Requiring the `up7_..._7_svc()` functions use this mechanism; and
     *   - Requiring that the thread-specific data-key be publicly visible.
     * - Closing the socket in the `up7.c` module. This was rejected because
     *   of the race condition between closing the socket and the file
     *   descriptor being reused by another thread.
     * - Having `up7.c` call `svc_unregister()` and then checking `svc_fdset`
     *   in this function. This was rejected because the file description
     *   can also be removed from `svc_fdset` by `svc_getreqsock()`, except
     *   that the latter also destroys the transport.
     * - Having `up7.c` call `svc_destroy()` and then checking `svc_fdset`
     *   in this function. This was rejected because it destroys the
     *   transport, which is dereferenced by `ldmprog_7`.
     * - Creating and using the function `up7_isDone()`. This was chosen.
     */
    for (;;) {
        log_debug("Calling poll()");

        sigset_t prevSigMask;
        CU_ASSERT_EQUAL_FATAL(pthread_sigmask(SIG_SETMASK, &mostSigMask,
                &prevSigMask), 0);

        errno = 0;
        status = poll(&fds, 1, -1); // `-1` => indefinite timeout

        CU_ASSERT_EQUAL_FATAL(pthread_sigmask(SIG_SETMASK, &prevSigMask, NULL),
                0);

        if (0 > status) {
            if (errno == EINTR) {
                log_debug("poll() interrupted");
            }
            else {
                log_debug("poll() failure");
                CU_ASSERT_EQUAL_FATAL(errno, EINTR);
            }
            break;
        }

        CU_ASSERT_TRUE_FATAL(fds.revents & POLLRDNORM)

        log_debug("Calling svc_getreqsock()");
        svc_getreqsock(sock); // Calls `ldmprog_7()`. *Might* destroy `xprt`.

        if (!FD_ISSET(sock, &svc_fdset)) {
            log_debug("Socket was closed by RPC layer");
            // RPC layer called `svc_destroy()` on `myUp7->xprt`
            myUp7->xprt = NULL; // Let others know
            break;
        }
    } // While not done loop
#endif

    up7_destroy();
}

static void
sndr_lock(Sender* const sender)
{
    CU_ASSERT_EQUAL_FATAL(pthread_mutex_lock(&sender->mutex), 0);
}

static void
sndr_unlock(Sender* const sender)
{
    CU_ASSERT_EQUAL_FATAL(pthread_mutex_unlock(&sender->mutex), 0);
}

static void
killMcastSndr(void)
{
    log_debug("Entered");

    pid_t pid = umm_getMldmSenderPid();

    if (pid) {
        log_debug("Sending SIGTERM to multicast LDM sender process");
        CU_ASSERT_EQUAL(kill(pid, SIGTERM), 0);

        /* Reap the terminated multicast sender. */
        {
            int status;

            log_debug("Reaping multicast sender child process");
            const pid_t wpid = waitpid(pid, &status, 0);

            CU_ASSERT_EQUAL(wpid, pid);
            CU_ASSERT_TRUE(wpid > 0);
            CU_ASSERT_TRUE(WIFEXITED(status));
            CU_ASSERT_EQUAL(WEXITSTATUS(status), 0);
            CU_ASSERT_EQUAL(umm_terminated(wpid), 0);
        }
    }

    log_debug("Returning");
}

/**
 * Executes a sender. Notifies `sender_start()`.
 *
 * @param[in]  arg     Pointer to sender.
 * @param[out] result  Ignored
 * @retval     0       Success
 */
static int
sndr_run(
        void* const restrict  arg,
        void** const restrict result)
{
    Sender* const sender = (Sender*)arg;
    static int    status;
    const int     servSock = sender->sock;
    struct pollfd fds;

    fds.fd = servSock;
    fds.events = POLLIN;

    sndr_lock(sender);
        sender->executing = true;
        CU_ASSERT_EQUAL_FATAL(pthread_cond_signal(&sender->cond), 0);
    sndr_unlock(sender);

    for (;;) {
        sndr_lock(sender);
        if (sender->done) {
            sndr_unlock(sender);
            break;
        }
        sndr_unlock(sender);

        /* NULL-s => not interested in receiver's address */
        int sock = accept(servSock, NULL, NULL);

        if (sock == -1) {
            CU_ASSERT_EQUAL(errno, EINTR);
            break;
        }
        else {
            sndr_lock(sender);

            if (sender->done) {
                sndr_unlock(sender);
            }
            else {
                // Initializes `sender->myUp7.xprt`
                sender->myUp7 = myUp7_new(sock);

                sndr_unlock(sender);

                myUp7_run(sender->myUp7);
                myUp7_free(sender->myUp7);
            }
        }
    }

    log_flush_error();
    log_debug("Returning &%d", status);

    return 0;
}

/**
 * Stops a sender that's executing on another thread.
 *
 * @param[in] arg     Sender to be stopped
 * @param[in] thread  Thread on which sender is running
 */
static int
sndr_halt(
        void* const     arg,
        const pthread_t thread)
{
    log_debug("Entered");

    Sender* const sender = (Sender*)arg;

    log_debug("Terminating multicast LDM sender");
    killMcastSndr();

    sndr_lock(sender);
        sender->done = true;

        CU_ASSERT_EQUAL_FATAL(pthread_kill(thread, SIGTERM), 0);
    sndr_unlock(sender);

    log_debug("Returning");

    return 0;
}

static int
sndr_initSock(
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
 * Returns the formatted address of a sender.
 *
 * @param[in] sender  The sender.
 * @return            Formatted address of the sender.
 */
static const char*
sndr_getAddr(
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
sndr_getPort(
        Sender* const sender)
{
    struct sockaddr_in addr;
    socklen_t          len = sizeof(addr);

    (void)getsockname(sender->sock, (struct sockaddr*)&addr, &len);

    return ntohs(addr.sin_port);
}

static void
sndr_insertProducts(void)
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
        log_info("Inserted: prodInfo=\"%s\"",
                s_prod_info(buf, sizeof(buf), info, 1));

        duration.tv_sec = 0;
        duration.tv_nsec = INTER_PRODUCT_INTERVAL;
        while (nanosleep(&duration, &duration))
            ;
    }

    free(data);
}

/**
 * Initializes a sender and starts executing it on a new thread. Blocks until
 * notified by `sender_run()`.
 *
 * @param[in,out] sender  Sender object
 * @param[in]     feed    Feed to be sent
 */
static void
sndr_start(
        Sender* const   sender,
        const feedtypet feed)
{
    int status;

    CU_ASSERT_EQUAL_FATAL(mutex_init(&sender->mutex,
            PTHREAD_MUTEX_ERRORCHECK, true), 0);
    CU_ASSERT_EQUAL_FATAL(pthread_cond_init(&sender->cond, NULL), 0);

    // Ensure that the first product-index will be 0
    CU_ASSERT_EQUAL_FATAL(pim_delete(NULL, feed), 0);

    /*
     * The product-queue must be thread-safe because it's accessed on
     * multiple threads:
     * - The product-insertion thread
     * - The backlog thread
     * - The missed-product thread
     */
    CU_ASSERT_EQUAL_FATAL(createEmptyProductQueue(UP7_PQ_PATHNAME), 0);
    CU_ASSERT_EQUAL_FATAL(pq_open(getQueuePath(), PQ_THREADSAFE, &pq), 0);

    SepMcastInfo* mcastInfo =
            smi_newFromStr(feed, "224.0.0.1:5173", "127.0.0.1:0");
    CU_ASSERT_PTR_NOT_NULL_FATAL(mcastInfo);

    if (!umm_isInited()) {
        status = umm_init(); // Upstream multicast manager
        if (status) {
            log_flush_error();
            CU_FAIL_FATAL("");
        }
    }

    in_addr_t subnet;
    CU_ASSERT_EQUAL(inet_pton(AF_INET, LOCAL_HOST, &subnet), 1);

    CidrAddr*   fmtpSubnet = cidrAddr_new(subnet, 24);
    CU_ASSERT_PTR_NOT_NULL(fmtpSubnet);

    const struct in_addr mcastIface = {inet_addr(LOCAL_HOST)};

    // The upstream multicast manager takes responsibility for freeing
    // `mcastInfo`
    status = umm_addPotentialSender(mcastIface, mcastInfo, 2, localVcEnd,
            fmtpSubnet, UP7_PQ_PATHNAME);
    if (status) {
        log_flush_error();
        CU_FAIL_FATAL("");
    }

    CU_ASSERT_EQUAL_FATAL(sndr_initSock(&sender->sock), 0);

    char* upAddr = ipv4Sock_getLocalString(sender->sock);
    char* mcastInfoStr = smi_toString(mcastInfo);
    char* vcEndPointStr = vcEndPoint_format(localVcEnd);
    char* fmtpSubnetStr = cidrAddr_format(fmtpSubnet);
    log_notice("LDM7 server starting up: pq=%s, upAddr=%s, mcastInfo=%s, "
            "localVcEnd=%s, subnet=%s", getQueuePath(), upAddr, mcastInfoStr,
            vcEndPointStr, fmtpSubnetStr);
    free(fmtpSubnetStr);
    free(vcEndPointStr);
    free(mcastInfoStr);
    free(upAddr);

    sender->executing = false;
    sender->done = false;
    sender->myUp7 = NULL;

    // Starts the sender on a new thread
    sender->future = executor_submit(executor, sender, sndr_run, sndr_halt);
    CU_ASSERT_PTR_NOT_NULL_FATAL(sender->future);

    sndr_lock(sender);
        while (!sender->executing)
            CU_ASSERT_EQUAL_FATAL(pthread_cond_wait(&sender->cond,
                    &sender->mutex), 0);
    sndr_unlock(sender);

    cidrAddr_free(fmtpSubnet);
    smi_free(mcastInfo);
}

/**
 * Stops a sender from executing and destroys it.
 *
 * @param[in,out]  Sender to be stopped and destroyed
 */
static void
sndr_stop(Sender* const sender)
{
    log_debug("Entered");

    CU_ASSERT_EQUAL(future_cancel(sender->future), 0);
    CU_ASSERT_EQUAL(future_getAndFree(sender->future, NULL), ECANCELED);

    CU_ASSERT_EQUAL_FATAL(close(sender->sock), 0);

    CU_ASSERT_EQUAL(pq_close(pq), 0);
    CU_ASSERT_EQUAL(unlink(UP7_PQ_PATHNAME), 0);

    CU_ASSERT_EQUAL(pthread_cond_destroy(&sender->cond), 0);
    CU_ASSERT_EQUAL(pthread_mutex_destroy(&sender->mutex), 0);

    log_debug("Returning");
}

typedef struct {
    signaturet sig;
    bool       delete;
} RequestArg;

static void
thread_blockSigTerm()
{
    sigset_t mask;

    (void)sigemptyset(&mask);
    (void)sigaddset(&mask, SIGTERM);
    (void)pthread_sigmask(SIG_BLOCK, &mask, NULL);
}

static bool
rqstr_isDone(Requester* const requester)
{
    bool done;

    if (requester->done) {
        done = true;
    }
    else {
        sigset_t sigSet;

        (void)sigpending(&sigSet);

        done = sigismember(&sigSet, SIGTERM);
    }

    return done;
}

static void
rqstr_subDecide(
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

static int
rqstr_decide(
        const prod_info* const restrict info,
        const void* const restrict      data,
        void* const restrict            xprod,
        const size_t                    size,
        void* const restrict            arg)
{
    char infoStr[LDM_INFO_MAX];
    log_debug("Entered: info=\"%s\"",
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
        rqstr_subDecide(reqArg, info->signature);
        maxProdIndex = prodIndex;
        maxProdIndexSet = true;
    }

    char buf[2*sizeof(signaturet)+1];
    sprint_signaturet(buf, sizeof(buf), info->signature);
    log_debug("Returning %s: prodIndex=%lu",
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
rqstr_delAndReq(const signaturet sig)
{
    FmtpProdIndex  prodIndex;
    (void)memcpy(&prodIndex, sig + sizeof(signaturet) - sizeof(FmtpProdIndex),
        sizeof(FmtpProdIndex));
    prodIndex = ntohl(prodIndex); // encoded in `sender_insertProducts()`

    char buf[2*sizeof(signaturet)+1];
    int  status = pq_deleteBySignature(receiverPq, sig);

    if (status) {
        (void)sprint_signaturet(buf, sizeof(buf), sig);
        log_error_q("Couldn't delete data-product: pq=%s, prodIndex=%lu, sig=%s",
                pq_getPathname(receiverPq), (unsigned long)prodIndex, buf);
    }
    else {
        if (log_is_enabled_info) {
            (void)sprint_signaturet(buf, sizeof(buf), sig);
            log_info("Deleted data-product: prodIndex=%lu, sig=%s",
                    (unsigned long)prodIndex, buf);
        }

        numDeletedProds++;

        down7_requestProduct(prodIndex);
    }

    return status;
}

/**
 * Executes a requester to test the "backstop" mechanism. Selected data-products
 * are deleted from the downstream product-queue and then requested from the
 * upstream LDM via the downstream LDM7.
 *
 * @param[in,out] arg   Pointer to requester object
 */
static void
rqstr_run(Requester* const requester)
{
    log_debug("Entered");

    thread_blockSigTerm();

    while (!rqstr_isDone(requester)) {
        RequestArg reqArg;
        int        status = pq_sequence(receiverPq, TV_GT, PQ_CLASS_ALL,
                rqstr_decide, &reqArg);

        if (status == PQUEUE_END) {
            static int unblockSigs[] = {SIGTERM};

            // Temporarily unblocks SIGCONT as well
            (void)pq_suspendAndUnblock(30, unblockSigs, 1);
        }
        else {
            CU_ASSERT_EQUAL_FATAL(status, 0);

            if (reqArg.delete) {
                /*
                 * The data-product is deleted here rather than in
                 * `requester_decide()` because in that function, the
                 * product's region is locked, deleting it attempts to lock it
                 * again, and deadlock results.
                 */
                CU_ASSERT_EQUAL_FATAL(rqstr_delAndReq(reqArg.sig),
                        0);
            }
        }
    }

    // Because end-of-thread
    log_flush_error();
    log_debug("Returning");
}

static void
rqstr_halt(
        Requester* const requester,
        const pthread_t  thread)
{

    mutex_lock(&requester->mutex);
        requester->done = true;
        CU_ASSERT_EQUAL(pthread_kill(thread, SIGTERM), 0);
    mutex_unlock(&requester->mutex);
}

/**
 * Executes a requester to test the "backstop" mechanism. Selected data-products
 * are deleted from the downstream product-queue and then requested from the
 * upstream LDM.
 */
static void
rqstr_init(Requester* const requester)
{
    int status = mutex_init(&requester->mutex, PTHREAD_MUTEX_ERRORCHECK, true);
    CU_ASSERT_EQUAL_FATAL(status, 0);

    requester->done = false;
}

static void
rqstr_destroy(Requester* const requester)
{
    int status = mutex_destroy(&requester->mutex);
    CU_ASSERT_EQUAL_FATAL(status, 0);
}

/**
 * Initializes a receiver.
 *
 * @param[in,out] receiver  Receiver object
 * @param[in]     addr      Address of sending LDM7: either hostname or IPv4
 *                          address
 * @param[in]     port      Port number of sender in host byte-order
 * @param[in]     feedtype  The feedtype to which to subscribe
 */
static void
rcvr_init(
        Receiver* const restrict   receiver,
        const char* const restrict ldmSrvrId,
        const unsigned short       port,
        const feedtypet            feed)
{
    CU_ASSERT_EQUAL_FATAL(createEmptyProductQueue(DOWN7_PQ_PATHNAME), 0)

    /*
     * The product-queue is opened thread-safe because it's accessed on
     * multiple threads:
     * - Multicast data-block insertion
     * - Unicast missed data-block insertion
     * - Unicast missed or backlog product insertion
     * - "Backstop" simulation product-deletion
     */
    CU_ASSERT_EQUAL_FATAL(
            pq_open(DOWN7_PQ_PATHNAME, PQ_THREADSAFE, &receiverPq), 0);

    InetSockAddr* ldmSrvr = isa_newFromId(ldmSrvrId, port);
    CU_ASSERT_PTR_NOT_NULL_FATAL(ldmSrvr);

    // Ensure no memory from a previous session
    CU_ASSERT_EQUAL_FATAL(mrm_delete(ldmSrvr, feed), true);
    receiver->mrm = mrm_open(ldmSrvr, feed);
    CU_ASSERT_PTR_NOT_NULL_FATAL(receiver->mrm);

    numDeletedProds = 0;

    int status = down7_init(ldmSrvr, feed, "dummy", localVcEnd, receiverPq,
            receiver->mrm);
    CU_ASSERT_EQUAL_FATAL(status, 0);

    isa_free(ldmSrvr);
}

/**
 * Destroys a receiver.
 */
static void
rcvr_destroy(Receiver* const recvr)
{
    down7_destroy();

    CU_ASSERT_TRUE(mrm_close(recvr->mrm));

    CU_ASSERT_EQUAL(pq_close(receiverPq), 0);

    CU_ASSERT_EQUAL(unlink(DOWN7_PQ_PATHNAME), 0);
}

static int
rcvr_runRequester(
        void* const restrict  arg,
        void** const restrict result)
{
    Receiver* receiver = (Receiver*)arg;

    rqstr_run(&receiver->requester);

    log_flush_error();

    return 0;
}

static int
rcvr_haltRequester(
        void* const     arg,
        const pthread_t thread)
{
    Receiver* receiver = (Receiver*)arg;

    rqstr_halt(&receiver->requester, thread);

    return 0;
}

static int
rcvr_runDown7(
        void* const restrict  arg,
        void** const restrict result)
{
    CU_ASSERT_EQUAL(down7_run(), 0);

    log_flush_error();

    return 0;
}

static int
rcvr_haltDown7(
        void* const     arg,
        const pthread_t thread)
{
    down7_halt();

    return 0;
}

/**
 * Starts an asynchronous receiver. Doesn't block.
 *
 * This implementation access the product-queue on 4 threads:
 * - Multicast data-block receiver thread
 * - Missed data-block receiver thread
 * - Product deletion and request thread (simulates FMTP layer missing products
 * - Missed products reception thread.
 *
 * @param[in,out] recvr     Receiver object
 */
static void
rcvr_start(
        Receiver* const restrict recvr)
{
    rqstr_init(&recvr->requester);

    /**
     * Starts a data-product requester on a separate thread to test the
     * "backstop" mechanism. Selected data-products are deleted from the
     * downstream product-queue and then requested from the upstream LDM.
     */
    recvr->requesterFuture = executor_submit(executor, recvr,
            rcvr_runRequester, rcvr_haltRequester);
    CU_ASSERT_PTR_NOT_NULL_FATAL(recvr->requesterFuture );

    recvr->down7Future = executor_submit(executor, recvr,
            rcvr_runDown7, rcvr_haltDown7);
    CU_ASSERT_PTR_NOT_NULL_FATAL(recvr->down7Future);
}

/**
 * Stops the receiver. Blocks until the receiver's thread terminates.
 */
static void
rcvr_stop(Receiver* const recvr)
{
    CU_ASSERT_EQUAL(future_cancel(recvr->down7Future), 0);
    CU_ASSERT_EQUAL(future_getAndFree(recvr->down7Future, NULL), ECANCELED);

    CU_ASSERT_EQUAL(future_cancel(recvr->requesterFuture), 0);
    CU_ASSERT_EQUAL(future_getAndFree(recvr->requesterFuture, NULL), ECANCELED);

    rqstr_destroy(&recvr->requester);
}

/**
 * @retval 0  Success
 */
static uint64_t
rcvr_getNumProds(
        Receiver* const receiver)
{
    return down7_getNumProds();
}

static long rcvr_getPqeCount(
        Receiver* const receiver)
{
    return down7_getPqeCount();
}

static void
test_up7(
        void)
{
    Sender   sender;

    sndr_start(&sender, ANY);
    log_flush_error();

    sndr_stop(&sender);
    log_clear();

    umm_destroy(true);
    log_flush_error();
}

static void
test_down7(
        void)
{
#if 1
    Receiver receiver;

    rcvr_init(&receiver, LOCAL_HOST, UP7_PORT, ANY);
    rcvr_start(&receiver);

    sleep(1);

    rcvr_stop(&receiver);
    rcvr_destroy(&receiver);
#else
    CU_ASSERT_EQUAL_FATAL(createEmptyProductQueue(DOWN7_PQ_PATHNAME), 0)
    CU_ASSERT_EQUAL_FATAL(
            pq_open(DOWN7_PQ_PATHNAME, PQ_THREADSAFE, &receiverPq), 0);

    CU_ASSERT_TRUE_FATAL(mrm_delete(up7Addr, ANY));
    McastReceiverMemory* mrm = mrm_open(up7Addr, ANY);
    CU_ASSERT_PTR_NOT_NULL_FATAL(mrm);

    Down7* down7 = down7_create(up7Addr, ANY, LOCAL_HOST, localVcEnd,
            receiverPq, mrm);
    CU_ASSERT_PTR_NOT_NULL(down7);

    sleep(1);

    CU_ASSERT_EQUAL(down7_destroy(down7), 0);

    CU_ASSERT_TRUE(mrm_close(mrm));
    CU_ASSERT_EQUAL(pq_close(receiverPq), 0);
    CU_ASSERT_EQUAL(unlink(DOWN7_PQ_PATHNAME), 0);
#endif
}

static void
test_bad_subscription(
        void)
{
    Sender   sender;
    Receiver receiver;

    sndr_start(&sender, NEXRAD2);
    log_flush_error();

    rcvr_init(&receiver, sndr_getAddr(&sender), sndr_getPort(&sender),
            NGRID);
    rcvr_start(&receiver);

    sleep(1);

    rcvr_stop(&receiver);
    rcvr_destroy(&receiver);

    log_debug("Terminating sender");
    sndr_stop(&sender);
    log_clear();

    umm_destroy(true);
    log_flush_error();
}

static void
test_up7Down7(
        void)
{
    //log_set_level(LOG_LEVEL_DEBUG);

    setSignalHandling();

    Sender   sender;
    Receiver receiver;
    int      status;

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

    status = lcf_init(LDM_PORT, NULL);
    CU_ASSERT_EQUAL_FATAL(status, 0);

    host_set* hostSet = lcf_newHostSet(HS_DOTTED_QUAD, LOCAL_HOST, NULL);
    CU_ASSERT_PTR_NOT_NULL_FATAL(hostSet);
    ErrorObj* errObj = lcf_addAllow(ANY, hostSet, ".*", NULL);
    CU_ASSERT_PTR_NULL_FATAL(errObj);

    sndr_start(&sender, ANY); // Blocks until sender is ready
    log_flush_error();

    rcvr_init(&receiver, sndr_getAddr(&sender), sndr_getPort(&sender),
            ANY);
    /* Starts a receiver on a new thread */
    rcvr_start(&receiver);
    log_flush_error();

    CU_ASSERT_EQUAL(sleep(2), 0);

    sndr_insertProducts();

    (void)sleep(2);
    log_notice("%lu sender product-queue insertions",
            (unsigned long)NUM_PRODS);
    uint64_t numDownInserts = rcvr_getNumProds(&receiver);
    log_notice("%lu product deletions", (unsigned long)numDeletedProds);
    log_notice("%lu receiver product-queue insertions",
            (unsigned long)numDownInserts);
    log_notice("%ld outstanding product reservations",
            rcvr_getPqeCount(&receiver));
    CU_ASSERT_EQUAL(numDownInserts - numDeletedProds, NUM_PRODS);

    CU_ASSERT_EQUAL(sleep(2), 0);

    log_debug("Stopping receiver");
    rcvr_stop(&receiver);
    rcvr_destroy(&receiver);

    log_debug("Stopping sender");
    sndr_stop(&sender);

    lcf_destroy(true);

    /*
    status = pthread_sigmask(SIG_SETMASK, &oldSigSet, NULL);
    CU_ASSERT_EQUAL(status, 0);
    */

    status = pthread_sigmask(SIG_SETMASK, &prevSigMask, NULL);
    CU_ASSERT_EQUAL(status, 0);
}

int main(
        const int    argc,
        char* const* argv)
{
    int status = 1; // Failure

    (void)log_init(argv[0]);
    log_set_level(LOG_LEVEL_DEBUG);

    opterr = 1; // Prevent getopt(3) from printing error messages
    for (int ch; (ch = getopt(argc, argv, "l:vx")) != EOF; ) {
        switch (ch) {
            case 'l': {
                (void)log_set_destination(optarg);
                break;
            }
            case 'v':
                if (!log_is_enabled_info)
                    log_set_level(LOG_LEVEL_INFO);
                break;
            case 'x':
                if (!log_is_enabled_debug)
                    log_set_level(LOG_LEVEL_DEBUG);
                break;
            default: {
                log_add("Unknown option: \"%c\"", optopt);
                return 1;
            }
        }
    }

    if (CUE_SUCCESS == CU_initialize_registry()) {
        CU_Suite* testSuite = CU_add_suite(__FILE__, setup, teardown);

        if (NULL != testSuite) {
            if (CU_ADD_TEST(testSuite, test_up7)
                    && CU_ADD_TEST(testSuite, test_down7)
                    && CU_ADD_TEST(testSuite, test_bad_subscription)
                    && CU_ADD_TEST(testSuite, test_up7Down7)
                    ) {
                CU_basic_set_mode(CU_BRM_VERBOSE);
                (void) CU_basic_run_tests();
            }
        }

        status = CU_get_number_of_suites_failed() +
                CU_get_number_of_tests_failed();
        CU_cleanup_registry();
    }

    log_flush_error();
    log_free();

    return status;
}
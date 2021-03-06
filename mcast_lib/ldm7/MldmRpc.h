/**
 * This file declares the remote-procedure-call API for the multicast LDM.
 *
 *        File: MldmRpc.h
 *  Created on: Feb 7, 2018
 *      Author: Steven R. Emmerson
 */

#ifndef MCAST_LIB_LDM7_MLDMRPC_H_
#define MCAST_LIB_LDM7_MLDMRPC_H_

#include "ldm.h"
#include "FmtpClntAddrs.h"

#include <netinet/in.h>
#include <stdbool.h>

/******************************************************************************
 * C API:
 ******************************************************************************/

#ifdef __cplusplus
    extern "C" {
#endif

/**
 * Returns a new multicast LDM RPC client.
 * @param[in] port  Port number of the multicast LDM RPC command-server in host
 *                  byte-order
 * @retval NULL     Failure
 * @return          Pointer to multicast LDM RPC client. Caller should call
 *                  `mldmClnt_delete()` when it's no longer needed.
 * @see mldmClnt_delete()
 */
void* mldmClnt_new(const in_port_t port);

/**
 * Reserves an IP address for a remote FMTP layer to use for its TCP endpoint
 * for recovering missed data-blocks.
 * @param[in]  mldmClnt  Multicast LDM RPC client
 * @param[out] fmtpAddr  Reserved IP address in network byte order
 * @retval LDM7_OK       Success. `*fmtpAddr` is set.
 * @retval LDM7_SYSTEM   System failure. `log_add()` called.
 */
Ldm7Status mldmClnt_reserve(
        void*      mldmClnt,
        in_addr_t* fmtpAddr);

/**
 * Explicitly allows the IP address of an FMTP client to connect.
 *
 * @param[in]  mldmClnt  Multicast LDM RPC client
 * @param[out] fmtpAddr  IP address of FMTP client
 * @retval LDM7_OK       Success
 * @retval LDM7_SYSTEM   System failure. `log_add()` called.
 */
Ldm7Status mldmClnt_allow(
        void*           mldmClnt,
        const in_addr_t fmtpAddr);

/**
 * Releases a reserved IP address for subsequent reuse.
 * @param[in] mldmClnt  Multicast LDM RPC client
 * @param[in] fmtpAddr  IP address in network byte order to be released
 * @retval LDM7_OK      Success
 * @retval LDM7_NOENT   `fmtpAddr` wasn't previously reserved. `log_add()`
 *                      called.
 * @retval LDM7_SYSTEM  System failure. `log_add()` called.
 */
Ldm7Status mldmClnt_release(
        void*           mldmClnt,
        const in_addr_t fmtpAddr);

/**
 * Destroys an allocated multicast LDM RPC client and deallocates it.
 * @param[in] mldmClnt  Multicast LDM RPC client
 * @see mldmClnt_new()
 */
void mldmClnt_free(void* mldmClnt);

/**
 * Constructs. Creates a listening server-socket and a file that contains a
 * secret that can be shared by other processes belonging to the same user.
 * @param[in] inAddrPool     Pool of available IP addresses
 */
void* mldmSrvr_new(void* inAddrPool);

/**
 * Returns the port number of the multicast LDM RPC server.
 * @param[in] mldmSrvr  Multicast LDM RPC server
 * @return              Port number on which the server is listening in host
 *                      byte-order
 */
in_port_t mldmSrvr_getPort(void* mldmSrvr);

/**
 * Starts the multicast LDM RPC server. Doesn't return until `mldmSrvr_stop()`
 * is called or a fatal error occurs.
 * @param[in] mldmSrvr     Multicast LDM RPC server
 * @retval    LDM7_OK      `stop()` called
 * @retval    LDM7_SYSTEM  System failure. `log_add()` called.
 * @threadsafety           Unsafe
 */
Ldm7Status mldmSrvr_run(void* mldmSrvr);

/**
 * Stops the multicast LDM RPC server.
 * @param[in] mldmSrvr     Multicast LDM RPC server
 * @retval    LDM7_OK      Success. `mldmSrvr_run()` should return.
 * @retval    LDM7_SYSTEM  System failure. `log_add()` called.
 * @threadsafety           Safe
 */
Ldm7Status mldmSrvr_stop(void* mldmSrvr);

/**
 * Destroys an allocated multicast LDM RPC server and deallocates it.
 * @param[in] mldmClnt  Multicast LDM RPC server
 * @see mldmSrvr_new()
 */
void mldmSrvr_free(void* mldmSrvr);

#ifdef __cplusplus
} // `extern "C"`

/******************************************************************************
 * C++ API:
 ******************************************************************************/

#include <memory>

/// Multicast LDM RPC actions
typedef enum MldmRpcAct
{
    /// Allow an IPv4 address
    ALLOW_ADDR,
    /// Reserve an IP address
    RESERVE_ADDR,
    /// Release a previously-reserved IP address
    RELEASE_ADDR,
    /// Close the connection
    CLOSE_CONNECTION
} MldmRpcAct;

/**
 * Multicast LDM RPC client.
 */
class MldmClnt final
{
    class                 Impl;
    std::shared_ptr<Impl> pImpl;

public:
    /**
     * Constructs.
     * @param[in] port  Port number of multicast LDM RPC server in host
     *                  byte-order
     */
    MldmClnt(const in_port_t port);

    /**
     * Reserves an IP address for a remote FMTP layer to use as its TCP endpoint
     * for recovering missed data-blocks.
     * @return  Reserved IP address in network byte order
     */
    in_addr_t reserve() const;

    /**
     * Explicitly allows the IP address of an FMTP client.
     *
     * @return  Reserved IP address in network byte order
     */
    void allow(const in_addr_t fmtpClnt) const;

    /**
     * Releases a reserved IP address for subsequent reuse.
     * @param[in] fmtpAddr  IP address in network byte order to be released
     */
    void release(const in_addr_t fmtpAddr) const;
};

/**
 * Multicast LDM RPC server.
 */
class MldmSrvr final
{
    class                 Impl;
    std::shared_ptr<Impl> pImpl;

public:
    /**
     * Constructs. Creates a listening server-socket and a file that contains a
     * secret.
     * @param[in] inAddrPool     Pool of available IP addresses
     */
    MldmSrvr(FmtpClntAddrs& inAddrPool);

    /**
     * Returns the port number of the multicast LDM RPC server.
     * @return Port number of multicast LDM RPC server in host byte-order
     */
    in_port_t getPort() const noexcept;

    /**
     * Runs the server. Doesn't return until `stop()` is called or a fatal
     * exception is thrown.
     */
    void operator()() const;

    /**
     * Stops the server.
     */
    void stop();
};

#endif // `#ifdef __cplusplus`

#endif /* MCAST_LIB_LDM7_MLDMRPC_H_ */

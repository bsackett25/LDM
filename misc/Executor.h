/**
 * This file defines an execution service for asynchronous tasks.
 *
 * Copyright 2018, University Corporation for Atmospheric Research
 * All rights reserved. See file COPYRIGHT in the top-level source-directory for
 * copying and redistribution conditions.
 * 
 *        File: Executor.h
 *  Created on: May 6, 2018
 *      Author: Steven R. Emmerson
 */
#include "../../misc/Future.h"
#include "config.h"


#ifndef MCAST_LIB_LDM7_EXECUTOR_H_
#define MCAST_LIB_LDM7_EXECUTOR_H_

#ifdef __cplusplus
    extern "C" {
#endif

/******************************************************************************
 * Execution service
 ******************************************************************************/

typedef struct executor Executor;

/**
 * Creates a new execution service.
 *
 * @retval    `NULL`           Failure. `log_add()` called.
 * @return                     New execution service.
 */
Executor*
executor_new(void);

/**
 * Deletes an execution service.
 *
 * @param[in,out] executor  Execution service to be deleted
 */
void
executor_free(Executor* const executor);

/**
 * Sets the object to call after a task has completed.
 *
 * @param[in] executor         Execution service
 * @param[in] completer        Object to pass to `afterCompletion()` as first
 *                             argument or `NULL`
 * @param[in] afterCompletion  Function to call after task has completed or
 *                             `NULL`. Must return `0` on success.
 */
void
executor_setAfterCompletion(
        Executor* const restrict executor,
        void* const restrict     completer,
        int                    (*afterCompletion)(
                                     void* restrict   completer,
                                     Future* restrict future));

/**
 * Submits a task to be executed asynchronously.
 *
 * @param[in,out] exec      Execution service
 * @param[in,out] obj       Job object
 * @param[in]     run       Function to run task. Must return 0 on success.
 * @param[in]     halt      Function to cancel task. Must return 0 on success.
 * @param[in]     get       Function to return result of task
 * @retval        `NULL`    Failure. `log_add()` called.
 * @return                  Future of task. Caller should call `future_free()`
 *                          when it's no longer needed.
 */
Future*
executor_submit(
        Executor* const exec,
        void* const   obj,
        int         (*run)(void* obj),
        int         (*halt)(void* obj, pthread_t thread),
        int         (*get)(void* obj, void** result));

/**
 * Returns the number of uncompleted task.
 *
 * @param[in] executor  Execution service
 * @return              Number of uncompleted task
 */
size_t
executor_size(Executor* const executor);

/**
 * Shuts down an execution service by canceling all submitted but not completed
 * tasks. Upon return, the execution service will no longer accept task
 * submissions. Doesn't wait for tasks to complete.
 *
 * @param[in,out] exec    Execution service
 * @param[in]     now     Whether or not to cancel uncompleted tasks
 * @retval        0       Success
 * @return                Error code. `log_add()` called.
 */
int
executor_shutdown(
        Executor* const exec,
        const bool      now);

#ifdef __cplusplus
    }
#endif

#endif /* MCAST_LIB_LDM7_EXECUTOR_H_ */

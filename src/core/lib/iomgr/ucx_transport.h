/*
* Copyright (C) Mellanox Technologies Ltd. 2016.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifndef GRPC_CORE_LIB_IOMGR_UCX_TRANSPORT_H
#define GRPC_CORE_LIB_IOMGR_UCX_TRANSPORT_H

#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/iomgr/ev_posix.h"

#define GRPC_USE_UCX 1

/**
 * Get internal UCX transport file descriptor to wake-up on receive
 */
int  ucx_get_fd();

/**
 * Do required work just before epoll_wait. It needs to prepare UCX transport
 * to use ucx_get_fd() file descriptor
 */
void ucx_prepare_fd();

/**
 * Establish internal UCX connection via high-speed transports.
 * Internal transports will be selected automatically
 */
void ucx_connect(int fd, int is_server);

/**
 * Create gRPC endpoint data structure.
 */
grpc_endpoint *grpc_ucx_create(grpc_fd *em_fd, size_t slice_size, const char *peer_string);

#endif /* GRPC_CORE_LIB_IOMGR_UCX_TRANSPORT_H */

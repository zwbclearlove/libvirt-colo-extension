/*
 * libvirt-colo.h
 * Summary: APIs for management of colo
 * Description: Provides APIs for the management of colo
 *
 */


#ifndef LIBVIRT_COLO_H
# define LIBVIRT_COLO_H

# ifndef __VIR_LIBVIRT_H_INCLUDES__
#  error "Don't include this file directly, only use libvirt/libvirt.h"
# endif

/**
* coloPeerStatus:
*
* COLO peer libvirt status
* 
* Since: 9.1.0
*/
typedef enum {
    VIRSH_COLO_PEER_NONE, /* COLO peer status none (Since: 9.1.0) */
    VIRSH_COLO_PEER_PRIMARY, /* COLO peer status primary (Since: 9.1.0) */
    VIRSH_COLO_PEER_SECONDARY, /* COLO peer status secondary (Since: 9.1.0) */
    VIRSH_COLO_PEER_ERROR, /* COLO peer status error (Since: 9.1.0) */
} coloPeerStatus;

/**
* _coloPeerState:
*
* COLO peer libvirt status
* 
* Since: 9.1.0
*/
struct _coloPeerState {
    virConnectPtr curConn;                  /* pointer to current peer connection */
    virConnectPtr peerConn;                  /* pointer to the colo peer connection */
    coloPeerStatus status;                          /* current status */
    //...
};

/**
 * coloPeerState:
 *
 * A coloPeerState is a private structure representing a pper state of
 * a libvirt.  
 * 
 * Since: 9.1.0
 */
typedef struct _coloPeerState coloPeerState;

/**
 * coloPeerPtr:
 *
 * A coloPeerPtr is pointer to a coloPeerState private structure.
 *
 * Since: 9.1.0
 */
typedef coloPeerState *coloPeerPtr;


int virColoSavePeerStatus(virConnectPtr conn, const char *uri);

#endif /* LIBVIRT_COLO_H */

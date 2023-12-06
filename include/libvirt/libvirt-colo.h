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

typedef enum {
    VIRSH_COLO_PEER_NONE,
    VIRSH_COLO_PEER_PRIMARY,
    VIRSH_COLO_PEER_SECONDARY,
    VIRSH_COLO_PEER_ERROR,
} coloPeerStatus;

struct _coloPeerState {
    virConnectPtr curConn;                  /* pointer to current peer connection */
    virConnectPtr peerConn;                  /* pointer to the colo peer connection */
    coloPeerStatus status;                          /* current status */
    //...
};

typedef struct _coloPeerState coloPeerState;
typedef coloPeerState *coloPeerPtr;


int virColoSavePeerStatus(virConnectPtr conn, const char *uri);

#endif /* LIBVIRT_COLO_H */

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
* virColoPeerStatus:
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
} virColoPeerStatus;




/* Main colo config. The data in these object
 * instances is immutable, so can be accessed
 * without locking. Threads must, however, hold
 * a valid reference on the object to prevent it
 * being released while they use it.
 *
 * eg
 *  g_autoptr(virColoConfig) cfg = virColoGetConfig(driver);
 *
 *  ...do stuff with 'cfg'..
 * 
 * Since: 9.1.0
 */
struct _virColoConfig {
    virColoPeerStatus colo_local_libvirt_status; /* COLO local status (Since: 9.1.0) */
    virColoPeerStatus colo_peer_libvirt_status; /* COLO peer status (Since: 9.1.0) */
    char* colo_peer_libvirt_uri; /* COLO peer uri (Since: 9.1.0) */
};

/**
 * virColoConfig: 
 * 
 * Since: 9.1.0
 */
typedef struct _virColoConfig virColoConfig;




/**
* _virColoPeerState:
*
* COLO peer libvirt status
* 
* Since: 9.1.0
*/
struct _virColoPeerState {
    char* localUri;                /* colo local connection uri */
    char* peerUri;                 /* colo peer connection uri */
    virColoPeerStatus status;                          /* current status */
    //...
};

/**
 * virColoPeerState:
 *
 * A coloPeerState is a private structure representing a pper state of
 * a libvirt.  
 * 
 * Since: 9.1.0
 */
typedef struct _virColoPeerState virColoPeerState;


int virColoSavePeerStatus(virConnectPtr conn, const char *uri);

int virColoConfigLoadFile(virColoConfig *cfg, const char *filename);

#endif /* LIBVIRT_COLO_H */

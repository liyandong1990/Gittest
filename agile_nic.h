/*
 * Agile network host driver using virtio. - modern (virtio 3.0) device support
 *
 * This module allows virtio devices to be used over a virtual PCI device.
 * This can be used with QEMU based VMMs like KVM or Xen.
 *
 * Copyright (c) DAYU Corp. 2021. All rights reserved.
 *
 * Author: Li yandong  <1121481051@qq.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef _AGILE_NIC_H
#define _AGILE_NIC_H

#include <linux/types.h>
#include <linux/spinlock.h>

#define BAR2		2

#define AGILE_DRIVER_VERSION  "0.0.1"
	
#define AGILE_VENDOR_ID				0x1957
#define AGILE_DEVICE_ID				0x1000	/* device ID: 0x1000 - 0x103F */

#define VENDOR_CONFIG_S_LINK_UP		0x01
#define VENDOR_CONFIG_S_DEVICE_OK	0x02
#define VENDOR_CONFIG_S_DRIVER_OK	0x04
#define VENDOR_CONFIG_S_FEATURES_OK	0x10
#define VENDOR_CONFIG_S_DEVICE_INIT	0x20
#define VENDOR_CONFIG_S_RESET		0x40	/* Status byte for guest to report reset */
#define VENDOR_CONFIG_S_FAILED		0x80	/* We've given up on this device. */

#define RESET_VAL					0x40

/* Note：from struct base_config to struct command_queue 
 * Do not add your own private data to these structures
 */
struct base_config {
	u16 fv_major;			/* Firmware Version Major */
	u16 fv_minor;			/* Firmware Version Minor */
	u16 pnum;				/* virt port number */
	u16 reserved;
};

struct command_module {
	u16 cmdv_major; 		/* Command version major */
	u16 cmdv_minor; 		/* Command version minor */
	u32 feature;			/* reference virtio_net.h: eg VIRTIO_NET_F_CSUM ...etc */	
	u32 reserved1;
	u32 reserved2;
	u32 reserved3;
};

struct vport_config {	
	u16 qnum;				/* queue number */
	u16 qlen;				/* queue length */
	u32 feature;
	u16 status; 			
	u16 reserved;
	u32 health_cnt; 		/* health count */
	u8 mac[ETH_ALEN];
	u16 mtu;
};

struct receive_queue {
	u32 qsize;				/* queue size */
	u16 msi_vector; 		/* MSI interrupt vector */
	u16 reserved;	
	u32 q_desc_lo;			/* queue descriptor low */
	u32 q_desc_hi;	
	u32 q_avail_lo; 		/* queue svailable low */
	u32 q_avail_hi;
	u32 q_used_lo;			/* queue used low */
	u32 q_used_hi;
	
};

struct send_queue {
	u32 qsize;				
	u16 msi_vector; 		
	u16 door_bell;			/* send notify */
	u32 q_desc_lo;			
	u32 q_desc_hi;
	u32 q_avail_lo; 		
	u32 q_avail_hi;
	u32 q_used_lo;
	u32 q_used_hi;
};

struct command_queue {
	u32 qsize;				
	u16 reserved;	 
	u16 door_bell;			/* send notify */
	u32 q_desc_lo;
	u32 q_desc_hi;
	u32 q_avail_lo; 
	u32 q_avail_hi;
	u32 q_used_lo;
	u32 q_used_hi;	
};

/* PF BAR struct */
struct pf_net_cfg {
	struct base_config base_cfg;
	struct command_module cmd_module;
	struct vport_config vport_cfg;
	struct receive_queue rcv_queue;
	struct send_queue sed_queue;
	struct command_queue cmd_queue;
};


struct agile_pci_vq_info {
	struct virtqueue *vq;		/* The actual virtqueue */
	struct list_head node;		/* The list node for the virtqueues list */
	unsigned msix_vector;		/* Allocated MSI-X vector (or none) */	
	void *queue;				/* the virtual address of the ring queue */
	u16 qindex;
	int qsize;					/* queue size for the currently selected queue */
};

/* device structure */
struct agile_pci_device {
	struct virtio_device vdev;
	struct pci_dev *pci_dev;

	int bars;
	u16 qindex;
	void __iomem *cf_base;		/* config base addr -> BAR2 */ 
	struct pf_net_cfg *pncfg;	/* network catd configuration information */

	struct timer_list keepalive_timer;	
	bool last_health;
	int health_num;				/* health number */
	bool board_alive;			/* Whether the network card is alive ? */
	
	/* a list of queues so we can dispatch IRQs */
	spinlock_t lock;
	struct list_head virtqueues;
	
	/* set value in ap_find_vqs_msix() */
	struct agile_pci_vq_info **vqs; /* array of all queues for house-keeping */
	
	/* MSI-X support */
	int msix_enabled;
	cpumask_var_t *msix_affinity_masks;   /* vectors masks */
	bool per_vq_vectors;			/* Whether we have vector per vq */
	unsigned msix_vectors;			/* Number of available vectors */	
	unsigned msix_used_vectors;		/* Vectors allocated, excluding per-vq vectors if any */	
	char (*msix_names)[256];
};

/* Constants for MSI-X */
/* Use first vector for configuration changes, second and the rest for
 * Virtqueues Thus, we need at least 2 vectors for MSI. */
enum {
	AP_MSIX_CONFIG_VECTOR = 0,
	AP_MSIX_VQ_VECTOR = 1,
};


#endif
	
	

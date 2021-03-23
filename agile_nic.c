/*
 * Agile network host driver using virtio. - modern (virtio 3.0) device support
 *
 * This module allows virtio devices to be used over a virtual PCI device.
 * This can be used with QEMU based VMMs like KVM or Xen.
 *
 * Copyright (c) DAYU Corp. 2021. All rights reserved.
 *
 * Authors: Li yandong  <1121481051@qq.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#define pr_fmt(fmt) "agile_nic: " fmt

#define DEBUG
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_net.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_pci.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/time.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE <= KERNEL_VERSION(4,5,0)
#include <linux/delay.h>
#endif

#include "agile_nic.h"

char pci_driver_name[] = "agile-net";

/* Convert a generic virtio device to our structure */
static struct agile_pci_device *to_ap_device(struct virtio_device *vdev)
{
	return container_of(vdev, struct agile_pci_device, vdev);
}

/* get vport_config addr */
static void __iomem *get_vcfg_addr(struct virtio_device *vdev)
{
	struct agile_pci_device *ap_dev = to_ap_device(vdev);	
	u16 vport_offset = offsetof(struct pf_net_cfg, vport_cfg);
	
	return ap_dev->cf_base + vport_offset;
}

static bool detection_card_link_status(struct agile_pci_device *ap_dev, 
				u16 val)
{
	void __iomem *vcfg_addr = get_vcfg_addr(&ap_dev->vdev);

	return ioread16(vcfg_addr + offsetof(struct vport_config, status)) & val;
}

static void __iomem *get_first_rqueue_addr(struct agile_pci_device *ap_dev)
{
	u16 recv_offset = offsetof(struct pf_net_cfg, rcv_queue);
	return ap_dev->cf_base + recv_offset;
}

/* virtio config->get_features() implementation */
static u64 ap_get_features(struct virtio_device *vdev)
{
	void __iomem *vcfg_addr = get_vcfg_addr(vdev);

	return ioread32(vcfg_addr + offsetof(struct vport_config, feature));
}

static void transport_features(struct virtio_device *vdev, u64 features)
{
	struct agile_pci_device *ap_dev = to_ap_device(vdev);
	struct pci_dev *pci_dev = ap_dev->pci_dev;

	if ((features & BIT_ULL(VIRTIO_F_SR_IOV)) &&
			pci_find_ext_capability(pci_dev, PCI_EXT_CAP_ID_SRIOV))
		__virtio_set_bit(vdev, VIRTIO_F_SR_IOV);
}

/* virtio config->finalize_features() implementation */
static int ap_finalize_features(struct virtio_device *vdev)
{
	void __iomem *vcfg_addr = get_vcfg_addr(vdev);
	u64 features = vdev->features;

	/* Give virtio_ring a chance to accept features. */
	vring_transport_features(vdev);
	
	/* Give virtio_pci a chance to accept features. */
	transport_features(vdev, features);
	iowrite32((u32)vdev->features, vcfg_addr + offsetof(struct vport_config, feature));
	return 0;
}

/* get mac addr from card */
static u8 get_mac(void __iomem * const mac_addr, unsigned offset)
{
	u8 rb = 0, i = 0;
	pr_debug("%s\n", __func__);
	for (i = 0; i < ETH_ALEN; i++) {
		if ( offset == offsetof(struct virtio_net_config, mac) + i )
			rb = ioread8(mac_addr + i);
	}
	return rb;
}

/* set mac addr to card */
static void set_mac(const void * const buf, void __iomem *mac_addr, 
				unsigned offset)
{
	u8 wb = 0, i = 0;

	pr_debug("%s\n", __func__);
	memcpy(&wb, buf, sizeof wb);
	for (i = 0; i < ETH_ALEN; i++) {
		if ( offset == offsetof(struct virtio_net_config, mac) + i )
			iowrite8(wb, mac_addr + i);
	}
}

/* 
 * virtio config->get() implementation
 * 1. in virtio_net.c
 * if read struct virtio_net_config:max_queue_pairs
 *  we return "struct vport.qnum" value
 *
 * 2. if read struct virtio_net_config: status
 *	we return "struct vport.status" value
 * 
 * 3. if read struct virtio_net_config: mac
 *	we return "struct vport.mac" value
 *
 * 4. if read struct virtio_net_config: mtu
 * we return "struct vport.mtu" value
 */
static void ap_get(struct virtio_device *vdev, unsigned offset,
				void *buf, unsigned len)
{
	u8 rb = 0;
	__le16 rw = 0;
	void __iomem *vp_addr = get_vcfg_addr(vdev);
	void __iomem *mac_addr =  vp_addr + offsetof(struct vport_config, mac);

	if (offset >= offsetof(struct virtio_net_config, mac)  
			&& offset < offsetof(struct virtio_net_config, mac) + ETH_ALEN ) {
		/* read mac */
		rb = get_mac(mac_addr, offset);
		memcpy(buf, &rb, sizeof rb);
	} else if (offset == offsetof(struct virtio_net_config, status)) { 
		/* read status */
		rw = cpu_to_le16(ioread16(vp_addr + offsetof(struct vport_config, status)));
		memcpy(buf, &rw, sizeof rw);
	} else if (offset == offsetof(struct virtio_net_config, max_virtqueue_pairs)) {
		/* read qnum */
		rw = cpu_to_le16(ioread16(vp_addr + offsetof(struct vport_config, qnum)));
		memcpy(buf, &rw, sizeof rw);
	} else if (offset == offsetof(struct virtio_net_config, mtu)) { 
		/* read mtu */
		rw = cpu_to_le16(ioread16(vp_addr + offsetof(struct vport_config, mtu)));
		memcpy(buf, &rw, sizeof rw);
	}
}

/* the config->set() implementation.*/
static void ap_set(struct virtio_device *vdev, unsigned offset,
				const void *buf, unsigned len)
{
	void __iomem *mac_addr = get_vcfg_addr(vdev) + offsetof(struct vport_config, mac);

	if (offset >= offsetof(struct virtio_net_config, mac)  
			&& offset < offsetof(struct virtio_net_config, mac) + ETH_ALEN) {

		set_mac(buf, mac_addr, offset);
	}
}

/* Didn't do anything */
static u32 ap_generation(struct virtio_device *vdev)
{
	return 0;
}

/* set status bit to card */
static void set_status_bit(struct virtio_device *vdev, u8 val)
{
	void __iomem *status_addr = get_vcfg_addr(vdev) 
				+ offsetof(struct vport_config, status);

	iowrite8( ioread8(status_addr) | val, status_addr);
}

/* config->{get,set}_status() implementations */
static u8 ap_get_status(struct virtio_device *vdev)
{	
	void __iomem* status_addr = get_vcfg_addr(vdev) 
				+ offsetof(struct vport_config, status);
	
	return ioread8(status_addr);
}

static void ap_set_status(struct virtio_device *vdev, u8 status)
{
	void __iomem* status_addr  = get_vcfg_addr(vdev) 
				+ offsetof(struct vport_config, status);
	
	/* We should never be setting status to 0. */
	BUG_ON(!status);
	iowrite16(status, status_addr);
}

/* wait for pending irq handlers */
static void synchronize_vectors(struct virtio_device *vdev)
{
	int i;
	struct agile_pci_device *ap_dev = to_ap_device(vdev);

	for (i = 0; i < ap_dev->msix_vectors; ++i)
		synchronize_irq(pci_irq_vector(ap_dev->pci_dev, i));
}

#define RST_TIMEOUT		50
/* reset card */
static bool reset_dev(struct virtio_device *vdev)
{
	int cnt = 0;
	bool ret = true;
	void __iomem *status_addr = get_vcfg_addr(vdev) 
					+ offsetof(struct vport_config, status);
	u16 status = ioread16(status_addr);	
	/* reset card */
	iowrite16(status | RESET_VAL, status_addr);	

	/* wait for firmware clean reset flg */
	do {
		status = ioread16(status_addr);
		msleep(100);

		pr_info("%s msleep \n", __func__);
		if(cnt++ >= RST_TIMEOUT) {
			pr_err("card reset timeout\n");
			ret = false;
			break;
		}
	} while(status & RESET_VAL);

	return ret;
}

static void ap_reset(struct virtio_device *vdev)
{
	reset_dev(vdev);
	/* Flush pending VQ/configuration callbacks. */
	synchronize_vectors(vdev);
}

static void del_vq(struct virtqueue *vq)
{
	unsigned long flags;	
	struct agile_pci_device *ap_dev = to_ap_device(vq->vdev);
	struct agile_pci_vq_info *info = ap_dev->vqs[vq->index];

	void __iomem *recv_addr = get_first_rqueue_addr(ap_dev);	
	void __iomem *msiv_addr = recv_addr
					+ info->qindex * sizeof(struct receive_queue) 
					+ offsetof(struct receive_queue, msi_vector);

	spin_lock_irqsave(&ap_dev->lock, flags);
	list_del(&info->node);
	spin_unlock_irqrestore(&ap_dev->lock, flags);

	if (ap_dev->msix_enabled) {		
		iowrite16(VIRTIO_MSI_NO_VECTOR, msiv_addr);
		
		/* Flush the write out to device */		
		ioread16(msiv_addr);
	}
	vring_del_virtqueue(vq);
	kfree(info);
	info = NULL;
}

static void ap_del_vqs(struct virtio_device *vdev)
{
	int i;
	struct agile_pci_device *ap_dev = to_ap_device(vdev);
	struct virtqueue *vq, *n;

	list_for_each_entry_safe(vq, n, &vdev->vqs, list) {
		if (ap_dev->per_vq_vectors) {				
			int v = ap_dev->vqs[vq->index]->msix_vector;

			if (v != VIRTIO_MSI_NO_VECTOR) {
				int irq = pci_irq_vector(ap_dev->pci_dev, v);

				/* Disable the affinity setting and the IRQ can run on any CPU */
				irq_set_affinity_hint(irq, NULL);
				free_irq(irq, vq);
			}
		}
		del_vq(vq);
	}

	if (likely(ap_dev->msix_affinity_masks)) {
		for (i = 0; i < ap_dev->msix_vectors; i++)
			if (ap_dev->msix_affinity_masks[i])
				free_cpumask_var(ap_dev->msix_affinity_masks[i]);
	}

	/* pci_alloc_irq_vectors_affinity() */
	if (likely(ap_dev->msix_enabled)) {		
		pci_free_irq_vectors(ap_dev->pci_dev);  	
		ap_dev->msix_enabled = 0;
	}

	ap_dev->msix_vectors = 0;
	ap_dev->msix_used_vectors = 0;

	/* kmalloc_array(nvectors, sizeof(*ap_dev->msix_names), */
	kfree(ap_dev->msix_names);
	ap_dev->msix_names = NULL;

	/* kcalloc(nvectors, sizeof(*ap_dev->msix_affinity_masks),...) */
	kfree(ap_dev->msix_affinity_masks);
	ap_dev->msix_affinity_masks = NULL;

	/* kcalloc(nvqs, sizeof(*ap_dev->vqs), ...) */
	kfree(ap_dev->vqs);
	ap_dev->vqs = NULL;
}

/* 
 * The notify function used when creating a virt queue 
 * Notify the EP side(card) when there are reads、writes、and configurations
 */
static bool notify(struct virtqueue *vq)
{
	struct agile_pci_device *ap_dev = to_ap_device(vq->vdev);
	u16 index = ap_dev->vqs[vq->index]->qindex;
	
	void __iomem *recv_addr = get_first_rqueue_addr(ap_dev);	
	void __iomem *db_addr = recv_addr + index * sizeof(struct receive_queue) 
					+ offsetof(struct send_queue, door_bell);

	/* we write the queue's selector into the notification register to
	 * signal the other end.
	 */	
	iowrite16(vq->index, db_addr);	
	return true;
}

/* set msi vector to virtqueue */
static void set_msi_vector(struct virtio_device *vdev, unsigned index,
				u16 msix_vec)
{
	struct agile_pci_device *ap_dev = to_ap_device(vdev);		
	void __iomem *recv_addr = get_first_rqueue_addr(ap_dev);
	
	/* qbase indicates the number of queues */
	void __iomem* qbase  = recv_addr + index * sizeof(struct receive_queue);

	iowrite16(msix_vec, qbase  +  offsetof(struct receive_queue, msi_vector));
	msix_vec = ioread16(qbase + offsetof(struct receive_queue, msi_vector));
	if (msix_vec == VIRTIO_MSI_NO_VECTOR) {
		pr_err("%s virtio msi no vector\n", __func__);
	}
}

/* Writethe desc、avail、used addr to the EP end */
static void set_virtqueue_addr(unsigned index, struct virtqueue *vq, 
				struct virtio_device *vdev)
{
	struct agile_pci_device *ap_dev = to_ap_device(vdev);
	void __iomem *recv_addr = get_first_rqueue_addr(ap_dev);

	/* queue head of "recv queue" or "send queue" or "cmd queue" */
	void __iomem *qhead = recv_addr + index * sizeof(struct receive_queue);

	u64 daddr = virtqueue_get_desc_addr(vq);		/* physical addr */
	u64 aaddr = virtqueue_get_avail_addr(vq);
	u64 uaddr = virtqueue_get_used_addr(vq);

	pr_debug("write addr ...\n");

	/* Since the elements in the "struct receive queue"、"struct send queue" and 
	 * "struct command queue" structures are all of the same size and order, we 
	 * can use the "offsetof(struct receive_queue,...) here
     */
	iowrite32((u32)daddr, qhead + offsetof(struct receive_queue, q_desc_lo));
	iowrite32((u32)(daddr >> 32), qhead + offsetof(struct receive_queue, q_desc_hi));
	
	iowrite32((u32)aaddr, qhead + offsetof(struct receive_queue, q_avail_lo));
	iowrite32((u32)(aaddr >> 32), qhead + offsetof(struct receive_queue, q_avail_hi));
	
	iowrite32((u32)uaddr, qhead + offsetof(struct receive_queue, q_used_lo));
	iowrite32((u32)(uaddr >> 32), qhead + offsetof(struct receive_queue, q_used_hi));
}

/* Create virtqueue and write addr to the card */
static struct virtqueue *setup_vq(struct virtio_device *vdev,
					unsigned index,
					void (*callback)(struct virtqueue *vq),
					const char *name,
					bool ctx,
					u16 msix_vec,
					unsigned nvqs)
{
	struct agile_pci_device *ap_dev = to_ap_device(vdev);	
	u16 recv_offset = offsetof(struct pf_net_cfg, rcv_queue);
	
	struct agile_pci_vq_info *info = NULL;
	struct virtqueue *vq = NULL;
	unsigned long flags;
	int qsize = 0;
	void *err = NULL;

	/* Read the "recv queue" / "send queue" and 
	 * "cmd queue" qsize base on the index 
	 */
	qsize = readl(ap_dev->cf_base + recv_offset 
				+ index * sizeof(struct receive_queue));	
	/* According to virtio3, qsize is an integer multiple of 1024 */
	if (unlikely(!qsize)) {
		pr_err("%s: qindex: %d, qsize is 0, err.\n", __func__, index);
		return ERR_PTR(-ENOENT);
	} else {
		pr_debug("%s: qindex: %d, qsize is %d\n", __func__, index, qsize);
	}
	
	/* in ap_del_vq() free, store each queue*/
	info = kmalloc(sizeof *info, GFP_KERNEL);
	if (unlikely(!info)) {
		return ERR_PTR(-ENOMEM);
	}

	info->qsize = qsize;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
	/* create the vring */
	vq = vring_create_virtqueue(index, qsize,
				    SMP_CACHE_BYTES, &ap_dev->vdev,
				    true, true, ctx,
				    notify, callback, name);
#else
	vq = vring_create_virtqueue(index, qsize,
				    SMP_CACHE_BYTES, &ap_dev->vdev,
				    true, true,
				    notify, callback, name);
#endif

	if (unlikely(!vq)) {
		pr_err("%s: create virtqueue err\n", __func__);
		err = ERR_PTR(-ENOMEM);
		goto err_qsize;
	}

	info->vq = vq;
	info->msix_vector = msix_vec;

	if (likely(callback)) {	
		spin_lock_irqsave(&ap_dev->lock, flags);		
		/* Put allocated virtqueues in the list */
		list_add(&info->node, &ap_dev->virtqueues);
		spin_unlock_irqrestore(&ap_dev->lock, flags);
	} else {
		INIT_LIST_HEAD(&info->node);
	}

	/* delete queue in ap_del_vq()  */
	ap_dev->vqs[index] = info;
	/* Record the index number of each queue for ap_notify() */
	ap_dev->vqs[index]->qindex = index;

	set_virtqueue_addr(index, vq, vdev);

	/* Set the MSI vector for each receive queue
	 * If the condition is true, indicating that it is not the last queue(commamd queue)
	 */
	if (likely(msix_vec != VIRTIO_MSI_NO_VECTOR)) {
		set_msi_vector(vdev, index, msix_vec);
	}
	return vq;

err_qsize:	
	kfree(info);
	info = NULL;
	
	return err;
}

static int request_msix_vectors(struct virtio_device *vdev, 
				int nvectors, struct irq_affinity *desc)

{
	struct agile_pci_device *ap_dev = to_ap_device(vdev);
	unsigned flags = PCI_IRQ_MSIX;
	unsigned i;
	int err = -ENOMEM;

	ap_dev->msix_vectors = nvectors;
	/* 
	 * Set the msix name as "#name-config" and alloc 
	 * "vring_interrupt" according to the name.
	 * ap_del_vqs()-> kfree(ap_dev->msix_names);
	 */
	ap_dev->msix_names = kmalloc_array(nvectors,
					   sizeof(*ap_dev->msix_names),
					   GFP_KERNEL);
	if (unlikely(!ap_dev->msix_names)) {
		pr_err("%s ap_dev->msix_names err", __func__);
		goto error;		
	}

	/* ap_del_vqs()-> kfree(ap_dev->msix_affinity_masks); */
	ap_dev->msix_affinity_masks					
		= kcalloc(nvectors, sizeof(*ap_dev->msix_affinity_masks), 
			  GFP_KERNEL);
	if (!ap_dev->msix_affinity_masks) {
		pr_err("%s ap_dev->msix_affinity_masks err", __func__);
		goto error;
	}

	/* free_cpumask_var(ap_dev->msix_affinity_masks[i]); */
	for (i = 0; i < nvectors; ++i)				
		/* Used in combination with multiple processors */
		if (!alloc_cpumask_var(&ap_dev->msix_affinity_masks[i],
					GFP_KERNEL)) {
			pr_err("%s alloc_cpumask_var err", __func__);
			goto error;
		}

	if (desc) {
		flags |= PCI_IRQ_AFFINITY;
		desc->pre_vectors++; 		/* virtio config vector */
	}

	/* Allocate up to max_vecs interrupt vectors for dev, using MSI-X
	 * or MSI vectors. if available, and fall back to a single legacy 
	 * vector if unavailable. 
	 * Return the number of vectors allocated
	 * If less than min_vecs interrupt vectors are available for dev 
	 * the function will  fail with -ENOSPC
	 *
	 * pci_alloc_irq_vectors_affinity() <--> pci_free_irq_vectors(); 
	 */
	err = pci_alloc_irq_vectors_affinity(ap_dev->pci_dev, nvectors,
					     nvectors, flags, desc);
	if (unlikely(err < 0)) {
		pr_err("%s pci alloc irq vectors affinity err: %d", __func__, err);
		WARN_ON(err < 0);	
		goto error;
	}
	ap_dev->msix_enabled = 1;	
	
	return 0;
error:
	return err;
}

/* 
 * Request vqs space and init it.
 * request msix interrupt vectors.
 * create virtqueue and write addr to the card.
 * request pci msix interrupt
 */
static int find_vqs_msix(struct virtio_device *vdev,
				unsigned nvqs,
				struct virtqueue *vqs[], 
				vq_callback_t *callbacks[],
				const char * const names[], 
				bool per_vq_vectors,
				const bool *ctx,
				struct irq_affinity *desc)

{
	struct agile_pci_device *ap_dev = to_ap_device(vdev);		
	int err = 0, i, nvectors, allocated_vectors, queue_idx = 0;
	u16 msix_vec;			/* interrupt index */

	if(nvqs <= 2) {
		pr_warn("%s nvqs is to small, please add feature \
VIRTIO_NET_F_CTRL_VQ\n", __func__);
		nvqs += 1;
	} else {
		pr_debug("%s: nvqs is %d\n", __func__, nvqs);
	}
	
	/* 1. request vqs, init in setup_vq()
	 * ap_dev->vqs[index] = info; store info for each queu 
	 * ap_dev_vqs()->kfree(ap_dev->vqs); 
	 */
	ap_dev->vqs = kcalloc(nvqs, sizeof(*ap_dev->vqs), GFP_KERNEL);
	if (unlikely(!ap_dev->vqs)) {
		pr_err("%s: kcalloc nvqs err\n", __func__);
		return -ENOMEM;
	}

	if (likely(per_vq_vectors)) {
		/* Best option: one for change interrupt, one per vq. */
		nvectors = 0;			/* start with interrupt 0 */
		for (i = 0; i < nvqs; ++i)
			if (callbacks[i])
				++nvectors;		/* Number of available vectors */	
	} else {		
		nvectors = 2;	/* Second best: one for change, shared for all vqs. */
	}

	/* 2. request msix interrupt vectors. */
	pr_debug("%s: request_msix_vectors start \n", __func__);
	err = request_msix_vectors(vdev, nvectors,
				      per_vq_vectors ? desc : NULL);
	if (err) {
		pr_err("%s: ap_request_msix_vectors err\n", __func__);
		goto error_find;
	}

	ap_dev->per_vq_vectors = per_vq_vectors;
	allocated_vectors = ap_dev->msix_used_vectors;

	for (i = 0; i < nvqs; ++i) {
		if (!names[i]) {
			vqs[i] = NULL;
			continue;
		}

		if (!callbacks[i]) {			
			msix_vec = VIRTIO_MSI_NO_VECTOR; /* virtio_net.c: callbacks[total_vqs - 1] = NULL; */
		} else if (ap_dev->per_vq_vectors)
			msix_vec = allocated_vectors++;	 /* count the allocated vector */
		else
			msix_vec = AP_MSIX_VQ_VECTOR;

		pr_err("%s: setup_vq start \n", __func__);
		/* 3. create virtqueue and write addr to the card */
		vqs[i] = setup_vq(vdev, queue_idx++, callbacks[i], names[i],
					ctx ? ctx[i] : false,
					msix_vec, nvqs);
		if (IS_ERR(vqs[i])) {
			pr_err("%s ap_setup_vq err\n", __func__);
			err = PTR_ERR(vqs[i]);
			goto error_find;
		}
		pr_err("%s: setup_vq end \n", __func__);
		
		if (!ap_dev->per_vq_vectors || msix_vec == VIRTIO_MSI_NO_VECTOR)	
			continue;

		snprintf(ap_dev->msix_names[msix_vec],
			 sizeof *ap_dev->msix_names,
			 "%s-%s",
			 dev_name(&ap_dev->vdev.dev), names[i]);
		
		/* 4. allocate an pci msix irq for per vq
		 * vring_interrupt: Callback when an MSIX interrupt occurs
		 * pci_irq_vector(pdev, msix_vec): get the interrupt vector 
		 * from the interrupt index(msix_vec).
		 */
		err = request_irq(pci_irq_vector(ap_dev->pci_dev, msix_vec),
				  vring_interrupt, 0,
				  ap_dev->msix_names[msix_vec],
				  vqs[i]);
		if (unlikely(err)) {
			del_vq(vqs[i]);
			pr_err("%s request_irq err.\n", __func__);
			goto error_find;
		}
	}
	
	return 0;

error_find:
	ap_del_vqs(vdev);
	return -ENOMEM;
}

/* 
 * virtio config->find_vqs() implementation
 * @param nvqs: total virtqueues 
 * @param vqs[]: output parammeters, used to record vq addr
 */	       
static int ap_find_vqs(struct virtio_device *vdev,
				     unsigned int nvqs,
				     struct virtqueue *vqs[],
				     vq_callback_t *callbacks[],
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
				     const char * const names[],
				     const bool *ctx,
				     struct irq_affinity *desc)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
				     const char * const names[],
				     struct irq_affinity *desc)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0) || RHEL_RELEASE_CODE >= 1796
				     const char * const names[])
#else
				     const char *names[])
#endif
{
	int ret;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,5,0)
	bool *ctx = NULL;
	struct irq_affinity *desc = NULL;
#endif

	if(nvqs < 1)
		pr_warn("%s nvqs is %d, err\n", __func__, nvqs);
	
	/* Try MSI-X with one vector per queue. */
	ret = find_vqs_msix(vdev, nvqs, vqs, callbacks, names, true, ctx, desc);
	if (unlikely(!ret))
		return 0;
	
	return -EINVAL;
}

static const char *ap_bus_name(struct virtio_device *vdev)
{
	struct agile_pci_device *ap_dev = to_ap_device(vdev);
	return pci_name(ap_dev->pci_dev);
}

int (*set_vq_affinity)(struct virtqueue *vq, int cpu);

/* 
 * Setup the affinity for a virtqueue:
 * - force the affinity for per vq vector
 * - OR over all affinities for shared MSI
 * - ignore the affinity request if we're using INTX
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
int ap_set_vq_affinity(struct virtqueue *vq, const struct cpumask *cpu_mask)
#else
int ap_set_vq_affinity(struct virtqueue *vq, int cpu)
#endif
{
	struct virtio_device *vdev = vq->vdev;
	struct agile_pci_device *ap_dev = to_ap_device(vdev);
	struct agile_pci_vq_info *info = ap_dev->vqs[vq->index];
	struct cpumask *mask;
	unsigned int irq;

	if (unlikely(!vq->callback))
		return -EINVAL;

	if (likely(ap_dev->msix_enabled)) {		
		mask = ap_dev->msix_affinity_masks[info->msix_vector];
		irq = pci_irq_vector(ap_dev->pci_dev, info->msix_vector);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
		if (!cpu_mask) {
			irq_set_affinity_hint(irq, NULL);
		}
		else {
			cpumask_copy(mask, cpu_mask);
			/* Set the affinity of the interrupt based on the mask */
			irq_set_affinity_hint(irq, mask);
		}
#else
		if (cpu == -1)
			irq_set_affinity_hint(irq, NULL);
		else {
			cpumask_set_cpu(cpu, mask);
			irq_set_affinity_hint(irq, mask);
		}
#endif
	}
	return 0;
}

const struct cpumask *ap_get_vq_affinity(struct virtio_device *vdev, 
				int index)
{
	struct agile_pci_device *ap_dev = to_ap_device(vdev);
	
	if (!ap_dev->per_vq_vectors ||
	    ap_dev->vqs[index]->msix_vector == VIRTIO_MSI_NO_VECTOR)
		return NULL;

	return pci_irq_get_affinity(ap_dev->pci_dev,
				    ap_dev->vqs[index]->msix_vector);
}

/* 
 * Virtio config operations. 
 * Called by function in virtio_net.c or virtio_ring.c 
 */
static const struct virtio_config_ops agile_net_virtio_config_ops = {
	.get		= ap_get,
	.set		= ap_set,
	.generation	= ap_generation,
	.get_status	= ap_get_status,
	.set_status	= ap_set_status,
	.reset		= ap_reset,
	.find_vqs	= ap_find_vqs,				
	.del_vqs	= ap_del_vqs,
	.get_features	= ap_get_features,
	.finalize_features = ap_finalize_features,
	.bus_name	= ap_bus_name,		
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
	.set_vq_affinity = ap_set_vq_affinity,
	.get_vq_affinity = ap_get_vq_affinity,
#endif
};

/* Nothing to do for now. */
static void virtio_pci_release_dev(struct device *_d)
{
	pr_info("%s\n", __func__);
}

/* Returns: 0 on suceess, -error on failure */
int register_virtio(struct pci_dev *pci_dev,  struct agile_pci_device *ap_dev)
{
	int rc = 0;
	struct virtio_device *vdev =  &ap_dev->vdev;;

	vdev->id.device = VIRTIO_ID_NET;
	vdev->dev.parent = &pci_dev->dev;
	vdev->dev.release = virtio_pci_release_dev;
	vdev->config = &agile_net_virtio_config_ops;

	rc = register_virtio_device(&ap_dev->vdev);
	if (unlikely(rc < 0)) {
		pr_err("register virtio device failed\n");
	}
	
	return rc;
}

static void unregister_virtio(struct pci_dev *pci_dev)
{
	struct agile_pci_device *ap_dev =  pci_get_drvdata(pci_dev);

	/* Wait 200ms, which should be good enough to drain the current
	 * pending packet.
	 */
	msleep(200);
	unregister_virtio_device(&ap_dev->vdev);
}

static bool read_health_data( struct agile_pci_device *ap_dev)
{
	bool ret = false;
	struct pf_net_cfg *pncfg = ap_dev->pncfg;
	void __iomem *vcfg_addr = get_vcfg_addr(&ap_dev->vdev);
	
	u32 hcnt = ioread32(vcfg_addr + offsetof(struct vport_config, health_cnt));

	if(unlikely(ap_dev->health_num == 0)) {
		pncfg->vport_cfg.health_cnt = hcnt;
		ap_dev->health_num = 1;
		return ret;
	}

	if(hcnt != pncfg->vport_cfg.health_cnt)
		ret = true;
	
	pncfg->vport_cfg.health_cnt = hcnt;
	return ret;
}

static void start_card(struct agile_pci_device *ap_dev)
{
	/* start the network card */
	set_status_bit(&ap_dev->vdev, VENDOR_CONFIG_S_DRIVER_OK); 	
}

static void keepalive_timer_callback(struct timer_list *arg)
{
	struct agile_pci_device *ap_dev = 
				container_of(arg, struct agile_pci_device, keepalive_timer);
	bool alive = detection_card_link_status(ap_dev, VENDOR_CONFIG_S_LINK_UP);
	bool health = read_health_data(ap_dev);
	
	if (alive != ap_dev->board_alive) {	
		if (alive)
			pr_info("Network card has started\n");
		else
			pr_info("Network card has stopped\n");
		
		/* virtio_config_changed(&vp_dev->vdev); -> virtio.c 
		 * -> drv->config_changed(dev) in virtio_net.c 
		 */
		virtio_config_changed(&ap_dev->vdev);
		ap_dev->board_alive = alive;
	}

	if(ap_dev->last_health != health) {
		if (alive && !health)
			pr_err("health count err\n");
		else if (alive && health)
			pr_info("health count ok\n");

		ap_dev->last_health = health;
	}
	
	mod_timer(&ap_dev->keepalive_timer, jiffies + HZ / 1);
}

void start_keepalive_detection(struct agile_pci_device *ap_dev)
{
	/* The first read value is not valid. Save the first
	 * read value into vport_cfg.health_cnt 
	 */
	read_health_data(ap_dev);

	timer_setup(&ap_dev->keepalive_timer, keepalive_timer_callback, 0);
	ap_dev->keepalive_timer.expires = jiffies + (HZ / 1);
	add_timer(&ap_dev->keepalive_timer);
}

static int agile_dev_init(struct pci_dev *pdev, 
			int bars, void __iomem *const addr)
{
	int ret = 0;
	struct agile_pci_device *ap_dev = NULL;

	/* allocate our structure and fill it out */
	ap_dev = kzalloc(sizeof(struct agile_pci_device), GFP_KERNEL);
	if (!ap_dev) {
		return -ENOMEM;
	}

	ap_dev->pncfg = kzalloc(sizeof(struct pf_net_cfg), GFP_KERNEL); 
	if (!ap_dev->pncfg) {
		goto err_ncfg;
	}

	ap_dev->pci_dev = pdev;
	ap_dev->cf_base = addr;
	ap_dev->bars = bars;
	ap_dev->board_alive = false;
	ap_dev->health_num = 0;
	ap_dev->msix_used_vectors = 0;
	ap_dev->last_health = false;

	INIT_LIST_HEAD(&ap_dev->virtqueues);
	spin_lock_init(&ap_dev->lock);
	pci_set_drvdata(pdev,ap_dev);
	
	if(!reset_dev(&ap_dev->vdev)) {
		goto err_card;
	}

	ret = register_virtio(pdev, ap_dev);	/* auto enable pci msi interrupt */
	if(ret < 0)
		goto err_card;

	start_card(ap_dev);	
	start_keepalive_detection(ap_dev);

	return 0;
	
err_card:
	kfree(ap_dev->pncfg);
err_ncfg:
	kfree(ap_dev);
	return -ENOMEM;
}

static void agile_dev_uninit(struct agile_pci_device *ap_dev)
{
	kfree(ap_dev->pncfg);
	ap_dev->pncfg = NULL;
	kfree(ap_dev);
	ap_dev = NULL;
}

static int agile_pci_net_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int ret = -ENODEV;
	int bars;
	void __iomem *bar2_addr = NULL;
	
	/* Returns the mask value, which bit is 1 indicates which bar is available */	
	bars = pci_select_bars(pdev, IORESOURCE_MEM | IORESOURCE_IO); 		 
	if (pci_enable_device(pdev))
		return ret;

	/* Enables the device to request the ability to use the PCI bus */
	pci_set_master(pdev);
	/* save the configuration space(pci/pci-X/pci-E) in pci_dev*/
	ret = pci_save_state(pdev);	
	if (ret)
		goto err_pci_disable_device;

	/* Resources required to apply for PCI */
	ret = pci_request_selected_regions(pdev, bars, pci_driver_name); 
	if (ret) {
		dev_err(&pdev->dev, "pci_request_selected_regions error\n");
		goto err_pci_disable_device;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(&pdev->dev,DMA_BIT_MASK(32));
	if (ret)
		dev_warn(&pdev->dev, "Failed to enable 64-bit or 32-bit DMA. \
Trying to continue, but this might not work.\n");

	/* Resource is in the Storage address space	*/
	if (!(pci_resource_flags(pdev, BAR2) & IORESOURCE_MEM)) {   		
		dev_err(&pdev->dev, "Invalid PCI mem BAR2 MEM resource, aborting\n"); 
	}
	
	if (unlikely(pci_resource_len(pdev, BAR2) <= 0)) {
		dev_err(&pdev->dev, "Invalid PCI mem BAR2 region size, aborting\n"); 
		goto err_iospace;
	}

	bar2_addr = pci_ioremap_bar(pdev, BAR2);
	if (unlikely(bar2_addr == NULL)) {
		dev_err(&pdev->dev, "BAR2 pci_ioremap_bar err\n"); 
		goto err_iospace;
	}

	ret = agile_dev_init(pdev, bars, bar2_addr);
	if(ret < 0) {
		pr_err("agile_dev_init err\n");
		goto err_dev_init;
	}

	pr_info("%s: driver install finished \n",pdev->driver->name);

	return 0;

err_dev_init:
	pci_iounmap(pdev, bar2_addr);
err_iospace:
	pci_release_selected_regions(pdev, bars);
err_pci_disable_device:
	pci_disable_device(pdev);
	return -ENODEV; 
}

static void agile_pci_net_remove(struct pci_dev *pdev)
{
	struct agile_pci_device *ap_dev = pci_get_drvdata(pdev);

	del_timer_sync(&ap_dev->keepalive_timer);
	
	unregister_virtio(pdev);		/* auto disable_msi */

	pci_iounmap(pdev, ap_dev->cf_base);
	pci_release_selected_regions(pdev, ap_dev->bars);
	agile_dev_uninit(ap_dev);		

	pci_disable_device(pdev);	
}

/* config pci device to support SRIOVC function  */
static int agile_pci_sriov_configure(struct pci_dev *pdev, int num_vfs)
{
	int ret = 0;
	
	if (pci_vfs_assigned(pdev))
		return -EPERM;

	if (num_vfs == 0) {
		pci_disable_sriov(pdev);
		return 0;
	}

	ret = pci_enable_sriov(pdev, num_vfs);
	if (ret < 0)
		return ret;

	return num_vfs;
}

static const struct pci_device_id agile_pci_id_tbl[] = {
	{
		.vendor		= AGILE_VENDOR_ID,
		.device		= AGILE_DEVICE_ID,
		.subvendor	= PCI_ANY_ID,
		.subdevice	= PCI_ANY_ID,
	},
	{ }
};
	
MODULE_DEVICE_TABLE(pci, agile_pci_id_tbl);

static struct pci_driver pci_net_driver = {
	.name     = pci_driver_name,
	.id_table = agile_pci_id_tbl,
	.probe    = agile_pci_net_probe,
	.remove   = agile_pci_net_remove,
	.sriov_configure = agile_pci_sriov_configure,
};

module_pci_driver(pci_net_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Li yandong"); 				
MODULE_DESCRIPTION("Agile PCI Net Driver");
MODULE_VERSION(AGILE_DRIVER_VERSION);
MODULE_SUPPORTED_DEVICE("LX2160A-agile");





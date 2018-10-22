/*
 * snull.c --  the Simple Network Utility
 *
 * Copyright (C) 2001 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 * $Id: snull.c,v 1.21 2004/11/05 02:36:03 rubini Exp $
 */
 
#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
 
#include <linux/sched.h>
#include <linux/kernel.h> /* PDEBUG() */
#include <linux/slab.h> /* kmalloc() */
#include <linux/errno.h>  /* error codes */
#include <linux/types.h>  /* size_t */
#include <linux/interrupt.h> /* mark_bh */
 
#include <linux/in.h>
#include <linux/netdevice.h>   /* struct device, and other headers */
#include <linux/etherdevice.h> /* eth_type_trans */
#include <linux/ip.h>          /* struct iphdr */
#include <linux/tcp.h>         /* struct tcphdr */
#include <linux/skbuff.h>
 
#include "snull.h"
 
#include <linux/in6.h>
#include <asm/checksum.h>
 
MODULE_AUTHOR("Alessandro Rubini, Jonathan Corbet");
MODULE_LICENSE("Dual BSD/GPL");
 
/*
 * Transmitter lockup simulation, normally disabled.
 */
static int lockup = 0;
module_param(lockup, int, 0);
 
static int timeout = SNULL_TIMEOUT;
module_param(timeout, int, 0);		//看门狗时间
 
/*
 * Do we run in NAPI mode?
 */
static int use_napi = 0;
module_param(use_napi, int, 0);				//默认不适用NAPI
 
#define PRINTK 1 
 
    
/*
 * A structure representing an in-flight packet.
 */
 
 //数据包链表：
 //成员1：主要指向下一数据包
 //成员2：网络设备结构体
 //成员3：数据长度
 //成员4：数据缓冲数组
struct snull_packet {         //虚拟网卡包
    struct snull_packet *next;
    struct net_device *dev;
    int    datalen;
    u8 data[ETH_DATA_LEN];
};
 
int pool_size = 8;	 //池链表长度	
module_param(pool_size, int, 0);
 
/*
 * This structure is private to each device. It is used to pass
 * packets in and out, so there is place for a packet
 */
 
struct snull_priv {			//虚拟网卡私有数据
    struct net_device_stats stats;
    int status;
    struct snull_packet *ppool;
    struct snull_packet *rx_queue;  	//该链表有4个成员   net_device *dev;  datalen;  data[ETH_DATA_LEN];
    int rx_int_enabled;
    int tx_packetlen;
    u8 *tx_packetdata;
    struct sk_buff *skb;
    struct napi_struct napi;
    spinlock_t lock;
};
 
static void snull_tx_timeout(struct net_device *dev);
static void (*snull_interrupt)(int, void *, struct pt_regs *);
 
/*
 * Set up a device's packet pool.
 */
void snull_setup_pool(struct net_device *dev)  		//分配内存空间
{
    struct snull_priv *priv = netdev_priv(dev);					//返回net_device结构末指针
    int i;
    struct snull_packet *pkt;
 
    priv->ppool = NULL; 
    for (i = 0; i < pool_size; i++) {
        pkt = kmalloc (sizeof (struct snull_packet), GFP_KERNEL);
        if (pkt == NULL) {
            PDEBUG (KERN_NOTICE "Ran out of memory allocating packet pool\n");
            return;
        }
        pkt->dev = dev;
        pkt->next = priv->ppool;
        priv->ppool = pkt;
    }
}
 
void snull_teardown_pool(struct net_device *dev)	//释放内存空间
{
    struct snull_priv *priv = netdev_priv(dev);
    struct snull_packet *pkt;
    
    while ((pkt = priv->ppool)) {
        priv->ppool = pkt->next;
        kfree (pkt);
        /* FIXME - in-flight packets ? */
    }
}    
 
/*
 * Buffer/pool management.
 */
struct snull_packet *snull_get_tx_buffer(struct net_device *dev)
{
    struct snull_priv *priv = netdev_priv(dev);
    unsigned long flags;
    struct snull_packet *pkt;
    
    spin_lock_irqsave(&priv->lock, flags);
    pkt = priv->ppool;
    priv->ppool = pkt->next;
    if (priv->ppool == NULL) {
        PDEBUG (KERN_INFO "Pool empty\n");
        netif_stop_queue(dev);
    }
    spin_unlock_irqrestore(&priv->lock, flags);
    return pkt;
}
 
 
void snull_release_buffer(struct snull_packet *pkt)
{
    unsigned long flags;
    struct snull_priv *priv = netdev_priv(pkt->dev);
    
    spin_lock_irqsave(&priv->lock, flags);
    pkt->next = priv->ppool;
    priv->ppool = pkt;
    spin_unlock_irqrestore(&priv->lock, flags);
    if (netif_queue_stopped(pkt->dev) && pkt->next == NULL)
        netif_wake_queue(pkt->dev);
}
 
void snull_enqueue_buf(struct net_device *dev, struct snull_packet *pkt)  //
{
    unsigned long flags;
    struct snull_priv *priv = netdev_priv(dev);
 
    spin_lock_irqsave(&priv->lock, flags);
    pkt->next = priv->rx_queue;  /* FIXME - misorders packets */
    priv->rx_queue = pkt;      
    spin_unlock_irqrestore(&priv->lock, flags);
}
 
struct snull_packet *snull_dequeue_buf(struct net_device *dev)
{
    struct snull_priv *priv = netdev_priv(dev);
    struct snull_packet *pkt;
    unsigned long flags;
 
    spin_lock_irqsave(&priv->lock, flags);
    pkt = priv->rx_queue;
    if (pkt != NULL)
        priv->rx_queue = pkt->next;
    spin_unlock_irqrestore(&priv->lock, flags);
    return pkt;
}
 
/*
 * Enable and disable receive interrupts.
 */
static void snull_rx_ints(struct net_device *dev, int enable)			//使能或失能接收中断
{
    struct snull_priv *priv = netdev_priv(dev);
    priv->rx_int_enabled = enable;
}
 
    
/*
 * Open and close
 */
 
int snull_open(struct net_device *dev)					//@开启一个虚拟网卡
{
    struct snull_priv *priv = netdev_priv(dev);
    /* request_region(), request_irq(), ....  (like fops->open) */
 
    /* 
     * Assign the hardware address of the board: use "\0SNULx", where
     * x is 0 or 1. The first byte is '\0' to avoid being a multicast
     * address (the first byte of multicast addrs is odd).
     */
 
    if (use_napi)
    {
        napi_enable(&priv->napi);
    }
 
    memcpy(dev->dev_addr, "\0SNUL0", ETH_ALEN);							//物理地址
    if (dev == snull_devs[1])
        dev->dev_addr[ETH_ALEN-1]++; /* \0SNUL1 */
    netif_start_queue(dev);
	
#if PRINTK
	printk("PRINTK--->snull_open -> netif_start_queue\n");
#endif	

	
    return 0;
}
 
int snull_release(struct net_device *dev)				//@关闭一个虚拟网卡
{
    struct snull_priv *priv = netdev_priv(dev);
    /* release ports, irq and such -- like fops->close */
 
    if (use_napi)
    {
        napi_disable(&priv->napi);
    }
    netif_stop_queue(dev); /* can't transmit any more */
#if PRINTK
	printk("PRINTK--->snull_release -> netif_stop_queue\n");
#endif	
	
    return 0;
}

 
/*
 * Configuration changes (passed on by ifconfig)
 */
int snull_config(struct net_device *dev, struct ifmap *map)
{
    if (dev->flags & IFF_UP) /* can't act on a running interface */
        return -EBUSY;
 
    /* Don't allow changing the I/O address */
    if (map->base_addr != dev->base_addr) {
        PDEBUG(KERN_WARNING "snull: Can't change I/O address\n");
        return -EOPNOTSUPP;
    }
 
    /* Allow changing the IRQ */
    if (map->irq != dev->irq) {
        dev->irq = map->irq;
            /* request_irq() is delayed to open-time */
    }
 
    /* ignore other fields */
    return 0;
}
 
/*
 * Receive a packet: retrieve, encapsulate and pass over to upper levels
 */
void snull_rx(struct net_device *dev, struct snull_packet *pkt)				//接收一个数据包  在常规中断接收调用    snull_regular_interrupt-> snull_rx-> netif_rx
{
    struct sk_buff *skb;
    struct snull_priv *priv = netdev_priv(dev);
 
    /*
     * The packet has been retrieved from the transmission
     * medium. Build an skb around it, so upper layers can handle it
     */
    skb = dev_alloc_skb(pkt->datalen + 2);			//分配skb大小
    if (!skb) {
        if (printk_ratelimit())
            PDEBUG(KERN_NOTICE "snull rx: low on mem - packet dropped\n");
        priv->stats.rx_dropped++;
        goto out;
    }
    skb_reserve(skb, 2); /* align IP on 16B boundary */     //使IP数据包对齐
    memcpy(skb_put(skb, pkt->datalen), pkt->data, pkt->datalen);		//复制数据包长度
 
    /* Write metadata, and then pass to the receive level */
    skb->dev = dev;
    skb->protocol = eth_type_trans(skb, dev);						//获取以太网类型
    skb->ip_summed = CHECKSUM_UNNECESSARY; /* don't check it */    //不检查校验和
    priv->stats.rx_packets++;				//接收的包和
    priv->stats.rx_bytes += pkt->datalen;		//接收的字节数和
	
//	printk("netif_rx:%s\n",skb->data);
    netif_rx(skb);    //将数据包放到各CPU等待队列中，软中断后供net_rx_action调用
	
  out:
    return;
}
    
 
/*
 * The poll implementation.
 */
 //当使用NAPI方式去处理发送接收时，若数据量过大，则采用轮循的方式去处理(即调用snull_poll函数)
static int snull_poll(struct napi_struct *napi, int budget)
{
    int npackets = 0;//当前版本已经没有quota变量
    struct sk_buff *skb;
    struct snull_priv *priv = netdev_priv(napi->dev);
    struct snull_packet *pkt;
    
    while (npackets < budget && priv->rx_queue) {
        pkt = snull_dequeue_buf(napi->dev);
        skb = dev_alloc_skb(pkt->datalen + 2);
        if (! skb) {
            if (printk_ratelimit())
                PDEBUG(KERN_NOTICE "snull: packet dropped\n");
            priv->stats.rx_dropped++;
            snull_release_buffer(pkt);
            continue;
        }
        skb_reserve(skb, 2); /* align IP on 16B boundary */  
        memcpy(skb_put(skb, pkt->datalen), pkt->data, pkt->datalen);
        skb->dev = napi->dev;
        skb->protocol = eth_type_trans(skb, napi->dev);
        skb->ip_summed = CHECKSUM_UNNECESSARY; /* don't check it */
        netif_receive_skb(skb);
        
            /* Maintain stats */
        npackets++;
        priv->stats.rx_packets++;
        priv->stats.rx_bytes += pkt->datalen;
        snull_release_buffer(pkt);
    }
    /* If we processed all packets, we're done; tell the kernel and reenable ints */
    if (! priv->rx_queue) {				//如果队列开启，使能接收中断
        __napi_complete(napi);
        snull_rx_ints(napi->dev, 1);
        return 0;
    }
    /* We couldn't process everything. */
    return 1;
}
        
        
/*
 * The typical interrupt entry point
 */
static void snull_regular_interrupt(int irq, void *dev_id, struct pt_regs *regs)       //不通过NAPI发送接收中断函数												//3.2.naip=0 默认注册  ，接收发送数据
{
    int statusword;
    struct snull_priv *priv;		//主要事件：1：获取状态标志字； 2：获取数据表链表。
    struct snull_packet *pkt = NULL;		//定义一个数据包链表，该链表包号四个成员
    /*
     * As usual, check the "device" pointer to be sure it is
     * really interrupting.
     * Then assign "struct device *dev"
     */
    struct net_device *dev = (struct net_device *)dev_id;
    /* ... and check with hw if it's really ours */
 
    /* paranoid */
    if (!dev)
        return;
 
    /* Lock the device */
    priv = netdev_priv(dev);
    spin_lock(&priv->lock);
 
    /* retrieve statusword: real netdevices use I/O instructions */
    statusword = priv->status;
	
#if PRINTK
	// printk("PRINTK--->R:%X  T:%X statusword:%X\n",SNULL_RX_INTR,SNULL_TX_INTR,statusword);
#endif		
	
	
    priv->status = 0;		//状态字清零，防止下一次再进入 ？
    if (statusword & SNULL_RX_INTR) {			//若发生接收中断
        /* send it to snull_rx for handling */
        pkt = priv->rx_queue;					//获取该设备数据
        if (pkt) {
            priv->rx_queue = pkt->next;			//指向下一个数据节点，相当于清空接收链表
            snull_rx(dev, pkt);					//进行接收操作
			
#if PRINTK
	// printk("PRINTK--->SNULL_RX_INTR:\n");
#endif	
			
        }
    }
	
    if (statusword & SNULL_TX_INTR) {			//若发生发送中断
        /* a transmission is over: free the skb */
        priv->stats.tx_packets++;
        priv->stats.tx_bytes += priv->tx_packetlen;
#if PRINTK
	// printk("PRINTK--->SNULL_TX_INTR:\n");
#endif
        dev_kfree_skb(priv->skb);
    }
 
    /* Unlock the device and we are done */
    spin_unlock(&priv->lock);
    if (pkt) snull_release_buffer(pkt); /* Do this outside the lock! */
    return;
}
 
/*
 * A NAPI interrupt handler.
 */
 
 
 
 //3.1 在模块调用函数中判断是否是用napi中断或常规中断。NAPI即NetAPI，是linux新的网卡数据处理API,该API中和了中断方式和轮循方式。
 //当需处理的数据量很少是，使用中断方式，这样不会频繁的使CPU被中断打断，同时提高响应速率，当数据量很大时，则采用轮循的方式，不至于消耗太多CPU的时间。
 //1.当该网卡驱动支持NAPI,则该网卡必须提供poll(),即轮循方法
 //2.网卡驱动非NAPI的内核接口为netif_rx(),NAPI的内核接口为napi_schedule().
 //3.网卡驱动非NAPI使用CPU共享队列softnet_data->input_pkt_queue,NAPI使用接收环。
 
 //该NAPI函数主要是做 1-数据包的接收；2-累计所发送的数据包和数据量 这两件事情
 //主要定义三个变量，目的是为了获取，网卡的状态（状态字），来判断是否发生发送中断或接收中断

 //思路：
//1.定义一个网络设备结构体：net_device 
//2.定义一个私有设备结构体，snull_priv
//3.通过函数net_priv从net_device结构体获取私有数据赋值给私有结构体
//4.定义一个整形变量获取私有数据状态，及作为一个网络设备的状态字
//5.此时抢占内核，准备开始做发送接收处理
//6.通过判断发送状态为或接收状态位是否已经发生，若发生发送中断，则累计所发送的数据包及数据量，同时是否私有数据的skb数组。
//若发生接收中断，则先关闭接收中断，此时调用napi_schedule函数处理接收数据
//7.处理完数据发送就收后，及时释放内核

//问题：该函数并没有涉及到接收和发送的过程。	
 
static void snull_napi_interrupt(int irq, void *dev_id, struct pt_regs *regs)		   //->	napi_schedule
{
    int statusword;
    struct snull_priv *priv;			//定义一个私有数据域，这里主要用来获取网络设备驱动的私有数据中的状态字
    struct net_device *dev = (struct net_device *)dev_id;	//定义网络设备驱动结构体，这里主要把结构体末尾的私有数据给私有域。
 
    /*
     * As usual, check the "device" pointer for shared handlers.
     * Then assign "struct device *dev"
     */
 
    /* ... and check with hw if it's really ours */
    /* paranoid */
    if (!dev)
        return;
 
    /* Lock the device */
    priv = netdev_priv(dev);					//获取网络私有数据，主要用来获取状态字
    spin_lock(&priv->lock);						//抢占内核
 
    /* retrieve statusword: real netdevices use I/O instructions */
    statusword = priv->status;					//获取网络状态，用来判断接收中断状态是否已经发生。
    priv->status = 0;	
    if (statusword & SNULL_RX_INTR) {				//若发生接收中断，则调用NAPI内核接口
        snull_rx_ints(dev, 0);  /* Disable further interrupts */   //失能接收中断，防止在调用NAPI内核处理接收数据是再次发生中断
        napi_schedule(&priv->napi);				//NAPI内核接收接口
    }
	
    if (statusword & SNULL_TX_INTR) {				//若发生内核发送中断，则累计所发送的数据包，及累计数据包的长度，同时释放该socket_buf的缓冲区
            /* a transmission is over: free the skb */
        priv->stats.tx_packets++;
        priv->stats.tx_bytes += priv->tx_packetlen;
        dev_kfree_skb(priv->skb);
    }
 
    /* Unlock the device and we are done */
    spin_unlock(&priv->lock);				//释放内核
    return;
}
 
/*
 * Transmit a packet (low level interface)
 */
static void snull_hw_tx(char *buf, int len, struct net_device *dev)										//4.发送一个包
{
    /*
     * This function deals with hw details. This interface loops
     * back the packet to the other snull interface (if any).
     * In other words, this function implements the snull behaviour,
     * while all other procedures are rather device-independent
     */
    struct iphdr *ih;
    struct net_device *dest;
    struct snull_priv *priv;
    u32 *saddr, *daddr;
    struct snull_packet *tx_buffer;
 
    /* I am paranoid. Ain't I? */
    if (len < sizeof(struct ethhdr) + sizeof(struct iphdr)) {
        PDEBUG("snull: Hmm... packet too short (%i octets)\n",
                len);
        return;
    }
    if (0) { /* enable this conditional to look at the data */
        int i;
        PDEBUG("len is %i\n" KERN_DEBUG "data:",len);
        for (i=14 ; i<len; i++)
            PDEBUG(" %02x",buf[i]&0xff);
        PDEBUG("\n");
    }
    /*
     * Ethhdr is 14 bytes, but the kernel arranges for iphdr
     * to be aligned (i.e., ethhdr is unaligned)
     */
    ih = (struct iphdr *)(buf+sizeof(struct ethhdr));
    saddr = &ih->saddr;
    daddr = &ih->daddr;
 
    ((u8 *)saddr)[2] ^= 1; /* change the third octet (class C) */
    ((u8 *)daddr)[2] ^= 1;
 
    ih->check = 0;         /* and rebuild the checksum (ip needs it) */
    ih->check = ip_fast_csum((unsigned char *)ih,ih->ihl);
 
    if (dev == snull_devs[0])
        PDEBUGG("%08x:%05i --> %08x:%05i\n",
                ntohl(ih->saddr),ntohs(((struct tcphdr *)(ih+1))->source),
                ntohl(ih->daddr),ntohs(((struct tcphdr *)(ih+1))->dest));
    else
        PDEBUGG("%08x:%05i <-- %08x:%05i\n",
                ntohl(ih->daddr),ntohs(((struct tcphdr *)(ih+1))->dest),
                ntohl(ih->saddr),ntohs(((struct tcphdr *)(ih+1))->source));
 
    /*
     * Ok, now the packet is ready for transmission: first simulate a
     * receive interrupt on the twin device, then  a
     * transmission-done on the transmitting device
     */
    dest = snull_devs[dev == snull_devs[0] ? 1 : 0];
    priv = netdev_priv(dest);
    tx_buffer = snull_get_tx_buffer(dev);
    tx_buffer->datalen = len;
    memcpy(tx_buffer->data, buf, len);
    snull_enqueue_buf(dest, tx_buffer);
	
    if (priv->rx_int_enabled) {					//接收中断使能
        priv->status |= SNULL_RX_INTR;
        snull_interrupt(0, dest, NULL);
    }
 
    priv = netdev_priv(dev);
    priv->tx_packetlen = len;
    priv->tx_packetdata = buf;
    priv->status |= SNULL_TX_INTR;
 
    if (lockup && ((priv->stats.tx_packets + 1) % lockup) == 0) {
        /* Simulate a dropped transmit interrupt */
        netif_stop_queue(dev);
        
        PDEBUG("Simulate lockup at %ld, txp %ld\n", jiffies,
                (unsigned long) priv->stats.tx_packets);
    }
    else
    {
        snull_interrupt(0, dev, NULL);
    }
}
    
/*
 * Transmit a packet (called by the kernel)
 */
int snull_tx(struct sk_buff *skb, struct net_device *dev)				//5.调用内核传输一个包
{
    // int len;
    // char *data, shortpkt[ETH_ZLEN];
    // struct snull_priv *priv = netdev_priv(dev);
 
    // data = skb->data;
    // len = skb->len;
	
// #if PRINTK
		 // printk("PRINTK--->len:%d\n",len);
// #endif	
	
	
    // if (len < ETH_ZLEN) {
        // memset(shortpkt, 0, ETH_ZLEN);
        // memcpy(shortpkt, skb->data, skb->len);
        // len = ETH_ZLEN;
        // data = shortpkt;
    // }
    // dev->trans_start = jiffies; /* save the timestamp */
 
    // /* Remember the skb, so we can free it at interrupt time */
    // priv->skb = skb;
 
    // // /* actual deliver of data is device-specific, and not shown here */
      // snull_hw_tx(data, len, dev);
	
 
    return 0; /* Our simple device can not fail */
}
    
/*
 * Deal with a transmit timeout.
 */
void snull_tx_timeout (struct net_device *dev)							//超时唤醒
{
    struct snull_priv *priv = netdev_priv(dev);
    PDEBUG("Transmit timeout at %ld, latency %ld\n", jiffies,
            jiffies - dev->trans_start);
			
#if PRINTK
		 printk("PRINTK--->snull_tx_timeout\n");
#endif				
			
    /* Simulate a transmission interrupt to get things moving */
    priv->status = SNULL_TX_INTR;
    snull_interrupt(0, dev, NULL);
    priv->stats.tx_errors++;
    netif_wake_queue(dev);
}
    
/*
 * Ioctl commands 
 */
int snull_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
    return 0;
}
 
/*
 * Return statistics to the caller
 */
struct net_device_stats *snull_stats(struct net_device *dev)
{
    struct snull_priv *priv = netdev_priv(dev);
 
    return &priv->stats;
}
 
/*
 * This function is called to fill up an eth header, since arp is not
 * available on the interface
 */
int snull_rebuild_header(struct sk_buff *skb)
{
    struct ethhdr *eth = (struct ethhdr *) skb->data;
    struct net_device *dev = skb->dev;
 
    memcpy(eth->h_source, dev->dev_addr, dev->addr_len);
    memcpy(eth->h_dest, dev->dev_addr, dev->addr_len);
    eth->h_dest[ETH_ALEN-1]   ^= 0x01;   /* dest is us xor 1 */
    return 0;
}
 
 
int snull_header(struct sk_buff *skb, struct net_device *dev,			//第一个参数：网络数据包缓冲，数据包抽象。参数2：网络设备
               unsigned short type, const void *daddr,
               const void *saddr, unsigned len)
{
    struct ethhdr *eth = (struct ethhdr *)skb_push(skb,ETH_HLEN);
 
    eth->h_proto = htons(type);
    memcpy(eth->h_source, saddr ? saddr : dev->dev_addr, dev->addr_len);
    memcpy(eth->h_dest,   daddr ? daddr : dev->dev_addr, dev->addr_len);
    eth->h_dest[ETH_ALEN-1]   ^= 0x01;   /* dest is us xor 1 */
 
    return (dev->hard_header_len);
}
 
 
int snull_change_mtu(struct net_device *dev, int new_mtu)			//@设置mtu值
{
    unsigned long flags;
    struct snull_priv *priv = netdev_priv(dev);
    spinlock_t *lock = &priv->lock;
 
    /* check ranges */
    if ((new_mtu < 68) || (new_mtu > 1500))
        return -EINVAL;
    /*
     * Do anything you need, and the accept the value
     */
    spin_lock_irqsave(lock, flags);
    dev->mtu = new_mtu;
	
#if PRINTK
	printk("PRINTK--->new_mtu:%d\n",new_mtu);
#endif	

    spin_unlock_irqrestore(lock, flags);
    return 0; /* success */
}

static struct header_ops header_devops =
{
    .create = snull_header,
    .rebuild  = snull_rebuild_header,
};
 
static struct net_device_ops net_devops =		//操作方法集，向上提供接口，向下操作硬件
{
    .ndo_open            = snull_open,
    .ndo_stop            = snull_release,
    .ndo_set_config      = snull_config,
    .ndo_start_xmit      = snull_tx,			//驱动功能层，是网络设备接口层net_device数据结构的具体成员，启动发送      ->snull_hw_tx ->snull_enqueue_buf
    .ndo_do_ioctl        = snull_ioctl,
    .ndo_get_stats       = snull_stats,
    .ndo_change_mtu      = snull_change_mtu,  
    .ndo_tx_timeout      = snull_tx_timeout,
};
 
/*

 * The init function (sometimes called probe).
 * It is invoked by register_netdev()
 */
void snull_init(struct net_device *dev)										//6.虚拟网卡初始化
{
    struct snull_priv *priv;												//定义虚拟私有结构体
    ether_setup(dev); /* assign some of the fields */						//在结构体net_device中某些参数默认分配字段				
    
    dev->header_ops = &header_devops;										//主要涉及到create -->header_devops   rebuild-->snull_rebuild_header
    dev->netdev_ops = &net_devops;											
    dev->watchdog_timeo = timeout;
 
    priv = netdev_priv(dev);				
	
    memset(priv, 0, sizeof(struct snull_priv));
 
    if (use_napi) {
        netif_napi_add(dev, &priv->napi, snull_poll, 2);    //添加一个poll  2：指的是权重
    }
    dev->flags           |= IFF_NOARP;

    spin_lock_init(&priv->lock);
     snull_rx_ints(dev, 1); 				//使能接收中断      
    snull_setup_pool(dev);
}
 
/*
 * The devices
 */
 
struct net_device *snull_devs[2];
 
/*
 * Finally, the module stuff
 */
 
void snull_cleanup(void)				//卸载驱动
{
    int i;
    
    for (i = 0; i < 2;  i++) {
        if (snull_devs[i]) {
            unregister_netdev(snull_devs[i]);			//注销该网络设备
            snull_teardown_pool(snull_devs[i]);			//
            free_netdev(snull_devs[i]);					//释放内存空间
        }
    }
    return;
}

//2.被模块调用的函数，该函数做的第一件事。1：获取虚拟发送接收中断函数，通过判断use_napi变量获取虚拟发送接收中断函数。

int snull_init_module(void)									
{
    int result, i, ret = -ENOMEM;
    snull_interrupt = use_napi ? snull_napi_interrupt : snull_regular_interrupt;
    snull_devs[0] = alloc_netdev(sizeof(struct snull_priv), "linzijun-%d", snull_init);			
    snull_devs[1] = alloc_netdev(sizeof(struct snull_priv), "linzijun-%d", snull_init);
           
    if (snull_devs[0] == NULL || snull_devs[1] == NULL)
        goto out;
	else
    ret = -ENODEV;
	
    for (i = 0; i < 2;  i++)
        if ((result = register_netdev(snull_devs[i])))							//注册网络设备，参数为struct net_device,
            PDEBUG("snull: error %i registering device \"%s\"\n",result, snull_devs[i]->name);
        else{
				ret = 0;
			}
			
		
	if(use_napi==1)
	{
		printk("use naip.\n");
	}
			
   out:
    if (ret) 
        snull_cleanup();		//如果内存分配失败，则注销网络设备
    return ret;
}
 
 
 
module_init(snull_init_module);								//1.函数入口，模块运行时，会从这里开始运行，传入参数为函数指针常量
module_exit(snull_cleanup);

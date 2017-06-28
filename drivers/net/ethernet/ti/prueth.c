/*
 * PRU Ethernet Driver
 *
 * Copyright (C) 2015-2017 Texas Instruments Incorporated - http://www.ti.com
 *	Roger Quadros <rogerq@ti.com>
 *	Andrew F. Davis <afd@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/etherdevice.h>
#include <linux/genalloc.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/pruss.h>
#include <linux/remoteproc.h>
#include <linux/debugfs.h>

#include "prueth.h"
#include "icss_mii_rt.h"
#include "icss_switch.h"
#include "hsr_prp_firmware.h"

#define PRUETH_MODULE_VERSION "0.2"
#define PRUETH_MODULE_DESCRIPTION "PRUSS Ethernet driver"

#define OCMC_RAM_SIZE		(SZ_64K - SZ_8K)

/* Pn_COL_BUFFER_OFFSET @ 0xEE00 0xF400 0xFA00 */
#define OCMC_RAM_SIZE_SWITCH	(SZ_64K)

/* TX Minimum Inter packet gap */
#define TX_MIN_IPG		0xb8

#define TX_START_DELAY		0x40
#define TX_CLK_DELAY		0x6

#define IEP_GLOBAL_CFG_REG_VAL	0x0551

/* PRUSS local memory map */
#define ICSS_LOCAL_SHARED_RAM   0x00010000

/* Netif debug messages possible */
#define PRUETH_EMAC_DEBUG	(NETIF_MSG_DRV | \
				 NETIF_MSG_PROBE | \
				 NETIF_MSG_LINK | \
				 NETIF_MSG_TIMER | \
				 NETIF_MSG_IFDOWN | \
				 NETIF_MSG_IFUP | \
				 NETIF_MSG_RX_ERR | \
				 NETIF_MSG_TX_ERR | \
				 NETIF_MSG_TX_QUEUED | \
				 NETIF_MSG_INTR | \
				 NETIF_MSG_TX_DONE | \
				 NETIF_MSG_RX_STATUS | \
				 NETIF_MSG_PKTDATA | \
				 NETIF_MSG_HW | \
				 NETIF_MSG_WOL)

static int debug_level = -1;
module_param(debug_level, int, 0);
MODULE_PARM_DESC(debug_level, "PRUETH debug level (NETIF_MSG bits)");

#define EMAC_POLL_WEIGHT	(64) /* Default NAPI poll weight */
#define EMAC_MAX_PKTLEN		(ETH_HLEN + VLAN_HLEN + ETH_DATA_LEN)
#define EMAC_MIN_PKTLEN		(60)

enum pruss_device {
	PRUSS_AM57XX = 0,
	PRUSS_AM4376,
	PRUSS_AM3359,
	PRUSS_K2G
};

#define PRUSS0 0
#define PRUSS1 1
#define PRUSS2 2

/* PRU Ethernet Type - Ethernet functionality (protocol
 * implemented) provided by the PRU firmware being loaded.
 */
enum pruss_ethtype {
	PRUSS_ETHTYPE_EMAC = 0,
	PRUSS_ETHTYPE_HSR,
	PRUSS_ETHTYPE_PRP,
	PRUSS_ETHTYPE_SWITCH,
	PRUSS_ETHTYPE_HSRPTP,
	PRUSS_ETHTYPE_PRPPTP,
	PRUSS_ETHTYPE_MAX,
};

#define HSR_TAG_LEN		(10)
#define EMAC_MAX_PKTLEN_HSR	(EMAC_MAX_PKTLEN + HSR_TAG_LEN)
#define PRUETH_IS_EMAC(p)	((p)->eth_type == PRUSS_ETHTYPE_EMAC)
#define PRUETH_IS_HSR(p)	((p)->eth_type == PRUSS_ETHTYPE_HSR)
#define PRUETH_IS_PRP(p)	((p)->eth_type == PRUSS_ETHTYPE_PRP)
#define PRUETH_IS_SWITCH(p)	((p)->eth_type == PRUSS_ETHTYPE_SWITCH)
#define PRUETH_IS_HSRPTP(p)	((p)->eth_type == PRUSS_ETHTYPE_HSRPTP)
#define PRUETH_IS_PRPPTP(p)	((p)->eth_type == PRUSS_ETHTYPE_PRPPTP)

#define PRUETH_HAS_HSR(p)	(PRUETH_IS_HSR(p) || PRUETH_IS_HSRPTP(p))
#define PRUETH_HAS_PRP(p)	(PRUETH_IS_PRP(p) || PRUETH_IS_PRPPTP(p))
#define PRUETH_HAS_RED(p)	(PRUETH_HAS_HSR(p) || PRUETH_HAS_PRP(p))

#define PRUETH_HAS_SWITCH(p) \
	(PRUETH_IS_SWITCH(p) || PRUETH_HAS_HSR(p) || PRUETH_HAS_PRP(p))

#define PRUETH_RED_TABLE_CHECK_PERIOD	(HZ / 100)
#define NUM_VLAN_PCP			8

/* In switch mode there are 3 real ports i.e. 3 mac addrs.
 * however Linux sees only the host side port. The other 2 ports
 * are the switch ports.
 * In emac mode there are 2 real ports i.e. 2 mac addrs.
 * Linux sees both the ports.
 */
enum prueth_port {
	PRUETH_PORT_HOST = 0,	/* host side port */
	PRUETH_PORT_MII0,	/* physical port MII 0 */
	PRUETH_PORT_MII1,	/* physical port MII 1 */
	PRUETH_PORT_MAX,
};

/* In both switch & emac modes there are 3 port queues
 * EMAC mode:
 *	RX packets for both MII0 & MII1 ports come on
 *	QUEUE_HOST.
 *	TX packets for MII0 go on QUEUE_MII0, TX packets
 *	for MII1 go on QUEUE_MII1.
 * Switch mode:
 *	Host port RX packets come on QUEUE_HOST
 *	TX packets might have to go on MII0 or MII1 or both.
 *	MII0 TX queue is QUEUE_MII0 and MII1 TX queue is
 *	QUEUE_MII1.
 */
enum prueth_port_queue_id {
	PRUETH_PORT_QUEUE_HOST = 0,
	PRUETH_PORT_QUEUE_MII0,
	PRUETH_PORT_QUEUE_MII1,
	PRUETH_PORT_QUEUE_MII0_RX,
	PRUETH_PORT_QUEUE_MII1_RX,
	PRUETH_PORT_QUEUE_MAX,
};

/* Each port queue has 4 queues and 1 collision queue */
enum prueth_queue_id {
	PRUETH_QUEUE1 = 0,
	PRUETH_QUEUE2,
	PRUETH_QUEUE3,
	PRUETH_QUEUE4,
	PRUETH_COLQ,	/* collision queue */
};

/* PRUeth memory range identifiers */
enum prueth_mem {
	PRUETH_MEM_DRAM0 = 0,
	PRUETH_MEM_DRAM1,
	PRUETH_MEM_SHARED_RAM,
	PRUETH_MEM_IEP,
	PRUETH_MEM_MII,
	PRUETH_MEM_OCMC,
	PRUETH_MEM_MAX,
};

/* ensure that order of PRUSS mem regions is same as above */
static enum pruss_mem pruss_mem_ids[] = { PRUSS_MEM_DRAM0, PRUSS_MEM_DRAM1,
					  PRUSS_MEM_SHRD_RAM2, PRUSS_MEM_IEP,
					  PRUSS_MEM_MII_RT };

/**
 * @fw_name: firmware names of firmware to run on PRU
 */
struct prueth_firmwares {
	const char *fw_name[PRUSS_ETHTYPE_MAX];
};

/**
 * struct prueth_private_data - PRU Ethernet private data
 * @driver_data: soc that contains the pruss
 * @fw_pru: firmware to run on each pruss
 */
struct prueth_private_data {
	enum pruss_device driver_data;
	struct prueth_firmwares fw_pru[PRUSS_NUM_PRUS];
};

/* data for each emac port */
struct prueth_emac {
	struct prueth *prueth;
	struct net_device *ndev;
	u8 mac_addr[6];
	struct napi_struct napi;
	u32 msg_enable;

	int link;
	int speed;
	int duplex;

	const char *phy_id;
	struct device_node *phy_node;
	int phy_if;
	struct phy_device *phydev;

	enum prueth_port port_id;
	int rx_irq;
	int tx_irq;

	struct prueth_queue_desc __iomem *rx_queue_descs;
	struct prueth_queue_desc __iomem *tx_queue_descs;
	struct prueth_queue_desc __iomem *rx_colq_descs;
	struct prueth_queue_desc __iomem *tx_colq_descs;

	struct port_statistics stats; /* stats holder when i/f is down */
	u32 tx_collisions;
	u32 tx_collision_drops;
	u32 rx_overflows;

	spinlock_t lock;	/* serialize access */
};

struct prueth_mmap_port_cfg_basis {
	u16 queue_size[NUM_QUEUES];
	u16 queue1_bd_offset;
	u16 queue1_buff_offset;
	u16 queue1_desc_offset;
	u16 col_queue_size;
	u16 col_bd_offset;
	u16 col_buff_offset;
	u16 col_queue_desc_offset;
};

struct prueth_mmap_sram_emac {
	u16 icss_emac_firmware_release_1_offset;  /* = eof_48k_buffer_bd */
	u16 icss_emac_firmware_release_2_offset;  /* +4 */

	u16 host_q1_rx_context_offset;            /* +4 */
	u16 host_q2_rx_context_offset;            /* +8 */
	u16 host_q3_rx_context_offset;            /* +8 */
	u16 host_q4_rx_context_offset;            /* +8 */

	u16 host_queue_descriptor_offset_addr;    /* +8 */
	u16 host_queue_offset_addr;               /* +8 */
	u16 host_queue_size_addr;                 /* +8 */
	u16 host_queue_desc_offset;               /* +16 */
};

struct prueth_mmap_sram_sw {
	u16 col_bd_offset[PRUETH_PORT_MAX];
};

struct prueth_mmap_sram_cfg {
	/* P0_Q1_BD_OFFSET = SRAM_START_OFFSET */
	u16 bd_offset[PRUETH_PORT_MAX][NUM_QUEUES];

	u16 end_of_bd_pool;
	u16 port_bd_size;
	u16 host_bd_size;
	u16 eof_48k_buffer_bd;

	union {
		struct prueth_mmap_sram_sw   mmap_sram_sw;
		struct prueth_mmap_sram_emac mmap_sram_emac;
	};
};

struct prueth_mmap_ocmc_cfg {
	u16 buffer_offset[PRUETH_PORT_MAX][NUM_QUEUES];
};

/**
 * struct prueth - PRUeth structure
 * @dev: device
 * @pruss: pruss handle
 * @pru0: rproc instance to PRU0
 * @pru1: rproc instance to PRU1
 * @mem: PRUSS memory resources we need to access
 * @sram_pool: OCMC ram pool for buffers
 *
 * @eth_node: node for each emac node
 * @emac: emac data for three ports, one host and two physical
 * @registered_netdevs: net device for each registered emac
 * @fw_data: firmware names to be used with PRU remoteprocs
 */
struct prueth {
	struct device *dev;
	struct pruss *pruss;
	struct rproc *pru0, *pru1;
	struct pruss_mem_region mem[PRUETH_MEM_MAX];
	struct gen_pool *sram_pool;

	struct device_node *eth_node[PRUETH_PORT_MAX];
	struct prueth_emac *emac[PRUETH_PORT_MAX];
	struct net_device *registered_netdevs[PRUETH_PORT_MAX];
	const struct prueth_private_data *fw_data;
	int pruss_id;
	size_t ocmc_ram_size;
	unsigned int eth_type;
	unsigned int hsr_mode;
	unsigned int emac_configured;
	unsigned int tbl_check_period;
	unsigned int node_table_clear;
	unsigned int tbl_check_mask;
	struct timer_list tbl_check_timer;
	u8 pcp_rxq_map[NUM_VLAN_PCP];
	struct prueth_mmap_port_cfg_basis mmap_port_cfg_basis[PRUETH_PORT_MAX];
	struct prueth_mmap_sram_cfg mmap_sram_cfg;
	struct prueth_mmap_ocmc_cfg mmap_ocmc_cfg;
	struct lre_statistics lre_stats;
#ifdef	CONFIG_DEBUG_FS
	struct dentry *root_dir;
	struct dentry *node_tbl_file;
	struct dentry *nt_clear_file;
	struct dentry *hsr_mode_file;
	struct dentry *dlrmt_file;
	struct dentry *dd_file;
	struct dentry *tr_file;
	struct dentry *error_stats_file;
#endif
};

static int pruss0_ethtype = PRUSS_ETHTYPE_EMAC;
module_param(pruss0_ethtype, int, 0444);
MODULE_PARM_DESC(pruss0_ethtype, "Choose PRUSS0 eth-type firmware");

static int pruss0_hsr_mode = MODEH;
module_param(pruss0_hsr_mode, int, 0444);
MODULE_PARM_DESC(pruss0_hsr_mode, "Choose PRUSS0 HSR mode");

static int pruss1_ethtype = PRUSS_ETHTYPE_EMAC;
module_param(pruss1_ethtype, int, 0444);
MODULE_PARM_DESC(pruss1_ethtype, "Choose PRUSS1 eth-type firmware");

static int pruss1_hsr_mode = MODEH;
module_param(pruss1_hsr_mode, int, 0444);
MODULE_PARM_DESC(pruss1_hsr_mode, "Choose PRUSS1 HSR mode");

static int pruss2_ethtype = PRUSS_ETHTYPE_EMAC;
module_param(pruss2_ethtype, int, 0444);
MODULE_PARM_DESC(pruss2_ethtype, "Choose PRUSS2 eth-type firmware");

static int pruss2_hsr_mode = MODEH;
module_param(pruss2_hsr_mode, int, 0444);
MODULE_PARM_DESC(pruss2_hsr_mode, "Choose PRUSS2 HSR mode");

static inline u32 prueth_read_reg(struct prueth *prueth,
				  enum prueth_mem region,
				  unsigned int reg)
{
	return readl_relaxed(prueth->mem[region].va + reg);
}

static inline void prueth_write_reg(struct prueth *prueth,
				    enum prueth_mem region,
				    unsigned int reg, u32 val)
{
	writel_relaxed(val, prueth->mem[region].va + reg);
}

static inline
void prueth_set_reg(struct prueth *prueth, enum prueth_mem region,
		    unsigned int reg, u32 mask, u32 set)
{
	u32 val;

	val = prueth_read_reg(prueth, region, reg);
	val &= ~mask;
	val |= (set & mask);
	prueth_write_reg(prueth, region, reg, val);
}

#if IS_ENABLED(CONFIG_DEBUG_FS)
static void prueth_hsr_prp_node_show(struct seq_file *sfp,
				     struct prueth *prueth, u8 index)
{
	void __iomem *sram = prueth->mem[PRUETH_MEM_SHARED_RAM].va;
	struct prueth_hsr_prp_node ent;
	u8 val, is_hsr;

	seq_printf(sfp, "\nNode[%u]:\n", index);
	memcpy_fromio(&ent, sram + NODE_TABLE + index * 32, 32);
	seq_printf(sfp, "MAC ADDR: %02x:%02x:%02x:%02x:%02x:%02x\n",
		   ent.mac[3], ent.mac[2], ent.mac[1],
		   ent.mac[0], ent.mac[5], ent.mac[4]);
	seq_printf(sfp, "state: %s\n",
		   ((ent.state & 0x1) ? "valid" : "invalid"));

	if (PRUETH_IS_PRP(prueth)) {
		val = (ent.status & NT_REM_NODE_DUP_MASK);
		switch (val) {
		case NT_REM_NODE_DUP_DISCARD:
			seq_printf(sfp, "DupDiscard (0x%02x)\n", val);
			break;
		case NT_REM_NODE_DUP_ACCEPT:
			seq_printf(sfp, "DupAccept (0x%02x)\n", val);
			break;
		default:
			seq_printf(sfp, "Unknown Dup type (0x%02x)\n", val);
			break;
		}
	}

	is_hsr = ent.status & NT_REM_NODE_HSR_BIT;
	val = (ent.status & NT_REM_NODE_TYPE_MASK) >> NT_REM_NODE_TYPE_SHIFT;
	switch (val) {
	case NT_REM_NODE_TYPE_SANA:
		seq_puts(sfp, "SAN A\n");
		break;
	case NT_REM_NODE_TYPE_SANB:
		seq_puts(sfp, "SAN B\n");
		break;
	case NT_REM_NODE_TYPE_SANAB:
		seq_puts(sfp, "SAN AB\n");
		break;
	case NT_REM_NODE_TYPE_DAN:
		if (is_hsr)
			seq_puts(sfp, "DANH\n");
		else
			seq_puts(sfp, "DANP\n");
		break;
	case NT_REM_NODE_TYPE_REDBOX:
		if (is_hsr)
			seq_puts(sfp, "REDBOXH\n");
		else
			seq_puts(sfp, "REDBOXP\n");
		break;
	case NT_REM_NODE_TYPE_VDAN:
		if (is_hsr)
			seq_puts(sfp, "VDANH\n");
		else
			seq_puts(sfp, "VDANP\n");
		break;
	default:
		seq_printf(sfp, "unknown node type %u\n", val);
		break;
	}

	seq_printf(sfp, "RxA=%u SupRxA=%u\n", ent.cnt_rx_a, ent.cnt_rx_sup_a);
	seq_printf(sfp, "RxB=%u SupRxB=%u\n", ent.cnt_rx_b, ent.cnt_rx_sup_b);

	seq_printf(sfp, "Time Last Seen: Sup=%u RxA=%u RxB=%u\n",
		   ent.time_last_seen_sup, ent.time_last_seen_a,
		   ent.time_last_seen_b);

	if (prueth->eth_type == PRUSS_ETHTYPE_PRP)
		seq_printf(sfp, "PRP LineID Err: A=%u B=%u\n",
			   ent.prp_lid_err_a, ent.prp_lid_err_b);
}

/* prueth_hsr_prp_node_table_show - Formats and prints node_table entries
 */
static int
prueth_hsr_prp_node_table_show(struct seq_file *sfp, void *data)
{
	struct prueth *prueth = (struct prueth *)sfp->private;
	void __iomem *sram = prueth->mem[PRUETH_MEM_SHARED_RAM].va;
	u8 i, index;
	u32 nodes;

	nodes = readl(sram + LRE_CNT_NODES);
	seq_printf(sfp, "\nRemote nodes in network: %u\n", nodes);

	for (i = 0; i < nodes + 2; i++) {
		index = readb(sram + INDEX_ARRAY + i);

		if (!index)
			/* first index guard */
			continue;

		if (index == NODE_TABLE_SIZE_MAX + 1)
			/* last index guard */
			break;

		prueth_hsr_prp_node_show(sfp, prueth, index);
	}
	seq_puts(sfp, "\n");
	return 0;
}

/* prueth_hsr_prp_node_table_open - Open the node_table file
 *
 * Description:
 * This routine opens a debugfs file node_table of specific hsr
 * or prp device
 */
static int
prueth_hsr_prp_node_table_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, prueth_hsr_prp_node_table_show,
			   inode->i_private);
}

static const struct file_operations prueth_hsr_prp_node_table_fops = {
	.owner	= THIS_MODULE,
	.open	= prueth_hsr_prp_node_table_open,
	.read	= seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* prueth_hsr_prp_nt_clear_write - write the user provided value to
 * node_table_clear debugfs file
 */
static ssize_t
prueth_hsr_prp_nt_clear_write(struct file *file, const char __user *user_buf,
			      size_t count, loff_t *ppos)
{
	struct prueth *prueth =
		((struct seq_file *)(file->private_data))->private;
	unsigned long clear;
	int err;

	err = kstrtoul_from_user(user_buf, count, 0, &clear);
	if (err)
		return err;

	if (clear)
		prueth->node_table_clear = 1;
	else
		prueth->node_table_clear = 0;

	return count;
}

/* prueth_hsr_prp_nt_clear_show - print the value of node_table_clear
 * debugfs file
 */
static int
prueth_hsr_prp_nt_clear_show(struct seq_file *sfp, void *data)
{
	struct prueth *prueth = (struct prueth *)sfp->private;
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;
	u32 check = readl(dram1 + HOST_TIMER_CHECK_FLAGS);

	seq_printf(sfp, "%lu\n",
		   check & HOST_TIMER_NODE_TABLE_CLEAR_BIT);

	return 0;
}

/* prueth_hsr_prp_nt_clear_open - Open the node_table clear debugfs file
 *
 * Description:
 * This routine opens a debugfs file node_table of specific hsr
 * or prp device
 */
static int
prueth_hsr_prp_nt_clear_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, prueth_hsr_prp_nt_clear_show,
			   inode->i_private);
}

static const struct file_operations prueth_hsr_prp_nt_clear_fops = {
	.owner	= THIS_MODULE,
	.open	= prueth_hsr_prp_nt_clear_open,
	.read	= seq_read,
	.write	= prueth_hsr_prp_nt_clear_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* prueth_hsr_mode_show - print the value of hsr_mode debugfs file
 * for hsr device
 */
static int
prueth_hsr_mode_show(struct seq_file *sfp, void *data)
{
	struct prueth *prueth = (struct prueth *)sfp->private;
	void __iomem *dram0 = prueth->mem[PRUETH_MEM_DRAM0].va;
	u32 mode = readl(dram0 + LRE_HSR_MODE);

	seq_printf(sfp, "%u\n", mode);

	return 0;
}

/* prueth_hsr_mode_write - write the user provided value to
 * hsr_mode debugfs file
 */
static ssize_t
prueth_hsr_mode_write(struct file *file, const char __user *user_buf,
		      size_t count, loff_t *ppos)
{
	struct prueth *prueth =
			((struct seq_file *)(file->private_data))->private;
	void __iomem *dram0 = prueth->mem[PRUETH_MEM_DRAM0].va;
	unsigned long mode;
	int err;

	err = kstrtoul_from_user(user_buf, count, 0, &mode);
	if (err)
		return err;

	if ((mode < MODEH) || (mode > MODEM))
		return -EINVAL;

	prueth->hsr_mode = mode;
	writel(mode, dram0 + LRE_HSR_MODE);

	return count;
}

/* prueth_hsr_mode_open - Open the prueth_hsr_mode_open debugfs file
 *
 * Description:
 * This routine opens a debugfs file hsr_mode for hsr device
 */
static int
prueth_hsr_mode_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, prueth_hsr_mode_show,
			   inode->i_private);
}

static const struct file_operations prueth_hsr_mode_fops = {
	.owner	= THIS_MODULE,
	.open	= prueth_hsr_mode_open,
	.read	= seq_read,
	.write	= prueth_hsr_mode_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* prueth_hsr_prp_dlrmt_write - write the user provided value to
 * dup_list_reside_max_time debugfs file
 */
static ssize_t
prueth_hsr_prp_dlrmt_write(struct file *file, const char __user *user_buf,
			   size_t count, loff_t *ppos)
{
	struct prueth *prueth =
			((struct seq_file *)(file->private_data))->private;
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;
	unsigned int forget_time;
	int err;

	err = kstrtouint_from_user(user_buf, count, 0, &forget_time);
	if (err)
		return err;

	/* input time is in msec. Firmware expects in unit of 10 msec */
	forget_time /= 10;
	writel(forget_time, dram1 + DUPLI_FORGET_TIME);

	return count;
}

/* prueth_hsr_prp_nt_clear_show - Formats and prints node_table entries
 */
static int
prueth_hsr_prp_dlrmt_show(struct seq_file *sfp, void *data)
{
	struct prueth *prueth = (struct prueth *)sfp->private;
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;
	u32 forget_time = readl(dram1 + DUPLI_FORGET_TIME);

	/* input time is in msec. Firmware expects in unit of 10 msec */
	forget_time *= 10;
	seq_printf(sfp, "%u\n", forget_time);

	return 0;
}

/* prueth_hsr_prp_nt_clear_open - Open the node_table clear file
 *
 * Description:
 * This routine opens a debugfs file node_table of specific hsr
 * or prp device
 */
static int
prueth_hsr_prp_dlrmt_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, prueth_hsr_prp_dlrmt_show,
			   inode->i_private);
}

static const struct file_operations prueth_hsr_prp_dlrmt_fops = {
	.owner	= THIS_MODULE,
	.open	= prueth_hsr_prp_dlrmt_open,
	.read	= seq_read,
	.write	= prueth_hsr_prp_dlrmt_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* prueth_hsr_prp_dd_write - write the user provided value to
 * duplicate_discard debugfs file
 */
static ssize_t
prueth_hsr_prp_dd_write(struct file *file, const char __user *user_buf,
			size_t count, loff_t *ppos)
{
	struct prueth *prueth =
			((struct seq_file *)(file->private_data))->private;
	void __iomem *sram = prueth->mem[PRUETH_MEM_SHARED_RAM].va;
	unsigned long dd;
	int err;

	err = kstrtoul_from_user(user_buf, count, 0, &dd);
	if (err)
		return err;

	if ((dd != IEC62439_CONST_DUPLICATE_DISCARD) &&
	    (dd != IEC62439_CONST_DUPLICATE_ACCEPT))
		return -EINVAL;

	writel(dd, sram + LRE_DUPLICATE_DISCARD);

	return count;
}

/* prueth_hsr_prp_dd_show - prints duplicate_discard debugfs file value
 */
static int
prueth_hsr_prp_dd_show(struct seq_file *sfp, void *data)
{
	struct prueth *prueth = (struct prueth *)sfp->private;
	void __iomem *sram = prueth->mem[PRUETH_MEM_SHARED_RAM].va;
	u32 dd = readl(sram + LRE_DUPLICATE_DISCARD);

	seq_printf(sfp, "%u\n", dd);

	return 0;
}

/* prueth_hsr_prp_dd_open - Open the duplicate_discard debugfs file
 *
 * Description:
 * This routine opens a debugfs file duplicate_discard for hsr or
 * prp device
 */
static int
prueth_hsr_prp_dd_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, prueth_hsr_prp_dd_show,
			   inode->i_private);
}

static const struct file_operations prueth_hsr_prp_dd_fops = {
	.owner	= THIS_MODULE,
	.open	= prueth_hsr_prp_dd_open,
	.read	= seq_read,
	.write	= prueth_hsr_prp_dd_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* prueth_prp_tr_write - write the user provided value to
 * transparent_reception debugfs file
 */
static ssize_t
prueth_prp_tr_write(struct file *file, const char __user *user_buf,
		    size_t count, loff_t *ppos)
{
	struct prueth *prueth =
			((struct seq_file *)(file->private_data))->private;
	void __iomem *sram = prueth->mem[PRUETH_MEM_SHARED_RAM].va;
	unsigned long tr;
	int err;

	err = kstrtoul_from_user(user_buf, count, 0, &tr);
	if (err)
		return err;

	if ((tr != IEC62439_CONST_TRANSPARENT_RECEPTION_REMOVE_RCT) &&
	    (tr != IEC62439_CONST_TRANSPARENT_RECEPTION_PASS_RCT))
		return -EINVAL;

	writel(tr, sram + LRE_TRANSPARENT_RECEPTION);

	return count;
}

/* prueth_prp_tr_show - print the current transparent_reception
 * file value for prp device.
 */
static int
prueth_prp_tr_show(struct seq_file *sfp, void *data)
{
	struct prueth *prueth = (struct prueth *)sfp->private;
	void __iomem *sram = prueth->mem[PRUETH_MEM_SHARED_RAM].va;
	u32 tr = readl(sram + LRE_TRANSPARENT_RECEPTION);

	seq_printf(sfp, "%u\n", tr);

	return 0;
}

/* prueth_prp_tr_open:- Open the transparent reception file
 *
 * Description:
 * This routine opens a debugfs file. transparent_reception
 * for prp device
 */
static int
prueth_prp_tr_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, prueth_prp_tr_show,
			   inode->i_private);
}

static const struct file_operations prueth_prp_tr_fops = {
	.owner	= THIS_MODULE,
	.open	= prueth_prp_tr_open,
	.read	= seq_read,
	.write	= prueth_prp_tr_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* prueth_error_stats_show - print the error stats
 */
static int
prueth_error_stats_show(struct seq_file *sfp, void *data)
{
	struct prueth *prueth = (struct prueth *)sfp->private;

	seq_printf(sfp, "tx_collisions: %u\n",
		   prueth->emac[PRUETH_PORT_MII0]->tx_collisions);
	seq_printf(sfp, "tx_collision_drops: %u\n",
		   prueth->emac[PRUETH_PORT_MII0]->tx_collision_drops);
	seq_printf(sfp, "rx_overflows: %u\n",
		   prueth->emac[PRUETH_PORT_MII0]->rx_overflows);

	return 0;
}

/* prueth_prp_erro_stats_open:- Open the error stats file
 *
 * Description:
 * This routine opens a debugfs file error_stats
 */
static int
prueth_error_stats_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, prueth_error_stats_show,
			   inode->i_private);
}

static const struct file_operations prueth_error_stats_fops = {
	.owner	= THIS_MODULE,
	.open	= prueth_error_stats_open,
	.read	= seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* prueth_hsr_prp_debugfs_init - create hsr-prp node_table file for dumping
 * the node table
 *
 * Description:
 * When debugfs is configured this routine sets up the node_table file per
 * hsr/prp device for dumping the node_table entries
 */
int prueth_hsr_prp_debugfs_init(struct prueth *prueth)
{
	struct device *dev = prueth->dev;
	int rc = -1;
	struct dentry *de = NULL;

	if (PRUETH_IS_HSR(prueth)) {
		de = debugfs_create_dir("prueth-hsr", NULL);
	} else if (PRUETH_IS_PRP(prueth)) {
		de = debugfs_create_dir("prueth-prp", NULL);
	} else {
		dev_err(dev, "unknown eth_type: %u\n", prueth->eth_type);
		return -EINVAL;
	}

	if (!de) {
		dev_err(dev, "Cannot create hsr-prp debugfs root\n");
		return rc;
	}

	prueth->root_dir = de;

	de = debugfs_create_file("node_table", S_IFREG | 0444,
				 prueth->root_dir, prueth,
				 &prueth_hsr_prp_node_table_fops);
	if (!de) {
		dev_err(dev, "Cannot create hsr-prp node_table file\n");
		return rc;
	}
	prueth->node_tbl_file = de;

	de = debugfs_create_file("node_table_clear", 0644,
				 prueth->root_dir, prueth,
				 &prueth_hsr_prp_nt_clear_fops);
	if (!de) {
		dev_err(dev, "Cannot create hsr-prp node table clear file\n");
		return rc;
	}
	prueth->nt_clear_file = de;

	if (PRUETH_IS_HSR(prueth)) {
		de = debugfs_create_file("hsr_mode", 0644,
					 prueth->root_dir, prueth,
					 &prueth_hsr_mode_fops);
		if (!de) {
			dev_err(dev, "Cannot create hsr mode file\n");
			return rc;
		}
		prueth->hsr_mode_file = de;
	}

	de = debugfs_create_file("dup_list_reside_max_time", 0644,
				 prueth->root_dir, prueth,
				 &prueth_hsr_prp_dlrmt_fops);
	if (!de) {
		dev_err(dev, "Cannot create dup_list_reside_max_time file\n");
		return rc;
	}
	prueth->dlrmt_file = de;

	de = debugfs_create_file("duplicate_discard", 0644,
				 prueth->root_dir, prueth,
				 &prueth_hsr_prp_dd_fops);
	if (!de) {
		dev_err(dev, "Cannot create duplicate_discard file\n");
		return rc;
	}
	prueth->dd_file = de;

	if (PRUETH_IS_PRP(prueth)) {
		de = debugfs_create_file("transparent_reception", 0644,
					 prueth->root_dir, prueth,
					 &prueth_prp_tr_fops);

		if (!de) {
			dev_err(dev, "Cannot create duplicate_discard file\n");
			return rc;
		}
		prueth->tr_file = de;
	}
	de = debugfs_create_file("error_stats", 0644,
				 prueth->root_dir, prueth,
				 &prueth_error_stats_fops);
	if (!de) {
		dev_err(dev, "Cannot create error_stats file\n");
		return rc;
	}
	prueth->error_stats_file = de;

	return 0;
}

/* prueth_hsr_prp_debugfs_term - Tear down debugfs intrastructure
 *
 * Description:
 * When Debufs is configured this routine removes debugfs file system
 * elements that are specific to hsr-prp
 */
void
prueth_hsr_prp_debugfs_term(struct prueth *prueth)
{
	debugfs_remove(prueth->node_tbl_file);
	prueth->node_tbl_file = NULL;
	debugfs_remove(prueth->nt_clear_file);
	prueth->nt_clear_file = NULL;
	if (PRUETH_IS_HSR(prueth)) {
		debugfs_remove(prueth->hsr_mode_file);
		prueth->hsr_mode_file = NULL;
	}
	debugfs_remove(prueth->dlrmt_file);
	prueth->dlrmt_file = NULL;
	debugfs_remove(prueth->dd_file);
	prueth->dd_file = NULL;
	if (PRUETH_IS_PRP(prueth)) {
		debugfs_remove(prueth->tr_file);
		prueth->tr_file = NULL;
	}
	debugfs_remove(prueth->error_stats_file);
	prueth->error_stats_file = NULL;
	debugfs_remove(prueth->root_dir);
	prueth->root_dir = NULL;
}
#else
static inline int prueth_hsr_prp_debugfs_init(struct prueth *prueth)
{
	return 0;
}

static inline void prueth_hsr_prp_debugfs_term(struct prueth *prueth)
{}
#endif

static struct prueth_queue_info queue_infos[PRUETH_PORT_QUEUE_MAX][NUM_QUEUES];
static struct prueth_queue_info tx_colq_infos[PRUETH_PORT_MAX];
static struct prueth_col_tx_context_info col_tx_context_infos[PRUETH_PORT_MAX];
static struct prueth_col_rx_context_info col_rx_context_infos[PRUETH_PORT_MAX];
static struct prueth_queue_desc queue_descs[PRUETH_PORT_MAX][NUM_QUEUES + 1];

/* VLAN-tag PCP to priority queue map for HSR/PRP/SWITCH.
 * Index is PCP val.
 *   low  - pcp 0..1 maps to Q4
 *              2..3 maps to Q3
 *              4..5 maps to Q2
 *   high - pcp 6..7 maps to Q1.
 */
static const unsigned short sw_pcp_tx_priority_queue_map[] = {
	PRUETH_QUEUE4, PRUETH_QUEUE4,
	PRUETH_QUEUE3, PRUETH_QUEUE3,
	PRUETH_QUEUE2, PRUETH_QUEUE2,
	PRUETH_QUEUE1, PRUETH_QUEUE1,
};

/* Order of processing of port Rx queues */
static unsigned int sw_port_rx_priority_queue_ids[] = {
	PRUETH_QUEUE1,
	PRUETH_QUEUE2,
	PRUETH_QUEUE3,
	PRUETH_QUEUE4
};

static int sw_num_rx_queues = NUM_QUEUES;

/* Order of processing of port Rx queues */
static const unsigned int emac_port_rx_priority_queue_ids[][2] = {
	[PRUETH_PORT_HOST] = {
		0, 0
	},
	[PRUETH_PORT_MII0] = {
		PRUETH_QUEUE1,
		PRUETH_QUEUE2,
	},
	[PRUETH_PORT_MII1] = {
		PRUETH_QUEUE3,
		PRUETH_QUEUE4
	},
};

static const int emac_num_rx_queues = (NUM_QUEUES / 2);

static int prueth_sw_hostconfig(struct prueth *prueth)
{
	void __iomem *dram1_base = prueth->mem[PRUETH_MEM_DRAM1].va;
	struct prueth_mmap_port_cfg_basis *pb;
	struct prueth_mmap_ocmc_cfg *oc = &prueth->mmap_ocmc_cfg;
	struct prueth_mmap_sram_cfg *s = &prueth->mmap_sram_cfg;
	void __iomem *dram;

	/* queue information table */
	dram = dram1_base + P0_Q1_RX_CONTEXT_OFFSET;
	memcpy_toio(dram, queue_infos[PRUETH_PORT_QUEUE_HOST],
		    sizeof(queue_infos[PRUETH_PORT_QUEUE_HOST]));

	dram = dram1_base + COL_RX_CONTEXT_P0_OFFSET_ADDR;
	memcpy_toio(dram, &col_rx_context_infos[PRUETH_PORT_QUEUE_HOST],
		    sizeof(col_rx_context_infos[PRUETH_PORT_QUEUE_HOST]));

	/* buffer descriptor offset table*/
	dram = dram1_base + QUEUE_DESCRIPTOR_OFFSET_ADDR;
	writew(s->bd_offset[PRUETH_PORT_HOST][PRUETH_QUEUE1], dram);
	writew(s->bd_offset[PRUETH_PORT_HOST][PRUETH_QUEUE2], dram + 2);
	writew(s->bd_offset[PRUETH_PORT_HOST][PRUETH_QUEUE3], dram + 4);
	writew(s->bd_offset[PRUETH_PORT_HOST][PRUETH_QUEUE4], dram + 6);

	/* buffer offset table */
	dram = dram1_base + QUEUE_OFFSET_ADDR;
	writew(oc->buffer_offset[PRUETH_PORT_HOST][PRUETH_QUEUE1], dram);
	writew(oc->buffer_offset[PRUETH_PORT_HOST][PRUETH_QUEUE2], dram + 2);
	writew(oc->buffer_offset[PRUETH_PORT_HOST][PRUETH_QUEUE3], dram + 4);
	writew(oc->buffer_offset[PRUETH_PORT_HOST][PRUETH_QUEUE4], dram + 6);

	/* queue size lookup table */
	pb = &prueth->mmap_port_cfg_basis[PRUETH_PORT_HOST];
	dram = dram1_base + QUEUE_SIZE_ADDR;
	writew(pb->queue_size[PRUETH_QUEUE1], dram);
	writew(pb->queue_size[PRUETH_QUEUE2], dram + 2);
	writew(pb->queue_size[PRUETH_QUEUE3], dram + 4);
	writew(pb->queue_size[PRUETH_QUEUE4], dram + 6);

	dram = dram1_base + pb->col_queue_desc_offset;
	memcpy_toio(dram, &queue_descs[PRUETH_PORT_QUEUE_HOST][PRUETH_COLQ],
		    sizeof(queue_descs[PRUETH_PORT_QUEUE_HOST][PRUETH_COLQ]));

	/* queue table */
	dram = dram1_base + pb->queue1_desc_offset;
	memcpy_toio(dram, &queue_descs[PRUETH_PORT_QUEUE_HOST][0],
		    4 * sizeof(queue_descs[PRUETH_PORT_QUEUE_HOST][0]));

	return 0;
}

static int prueth_hostconfig(struct prueth *prueth)
{
	void __iomem *sram_base = prueth->mem[PRUETH_MEM_SHARED_RAM].va;
	struct prueth_mmap_port_cfg_basis *pb;
	struct prueth_mmap_ocmc_cfg *oc = &prueth->mmap_ocmc_cfg;
	struct prueth_mmap_sram_cfg *s = &prueth->mmap_sram_cfg;
	struct prueth_mmap_sram_emac *emac_sram = &s->mmap_sram_emac;
	void __iomem *sram;

	/* queue size lookup table */
	pb = &prueth->mmap_port_cfg_basis[PRUETH_PORT_HOST];
	sram = sram_base + emac_sram->host_queue_size_addr;
	writew(pb->queue_size[PRUETH_QUEUE1], sram);
	writew(pb->queue_size[PRUETH_QUEUE2], sram + 2);
	writew(pb->queue_size[PRUETH_QUEUE3], sram + 4);
	writew(pb->queue_size[PRUETH_QUEUE4], sram + 6);

	/* queue information table */
	sram = sram_base + emac_sram->host_q1_rx_context_offset;
	memcpy_toio(sram, queue_infos[PRUETH_PORT_QUEUE_HOST],
		    sizeof(queue_infos[PRUETH_PORT_QUEUE_HOST]));

	/* buffer offset table */
	sram = sram_base + emac_sram->host_queue_offset_addr;
	writew(oc->buffer_offset[PRUETH_PORT_HOST][PRUETH_QUEUE1], sram);
	writew(oc->buffer_offset[PRUETH_PORT_HOST][PRUETH_QUEUE2], sram + 2);
	writew(oc->buffer_offset[PRUETH_PORT_HOST][PRUETH_QUEUE3], sram + 4);
	writew(oc->buffer_offset[PRUETH_PORT_HOST][PRUETH_QUEUE4], sram + 6);

	/* buffer descriptor offset table*/
	sram = sram_base + emac_sram->host_queue_descriptor_offset_addr;
	writew(s->bd_offset[PRUETH_PORT_HOST][PRUETH_QUEUE1], sram);
	writew(s->bd_offset[PRUETH_PORT_HOST][PRUETH_QUEUE2], sram + 2);
	writew(s->bd_offset[PRUETH_PORT_HOST][PRUETH_QUEUE3], sram + 4);
	writew(s->bd_offset[PRUETH_PORT_HOST][PRUETH_QUEUE4], sram + 6);

	/* queue table */
	sram = sram_base + emac_sram->host_queue_desc_offset;
	memcpy_toio(sram, &queue_descs[PRUETH_PORT_QUEUE_HOST][0],
		    4 * sizeof(queue_descs[PRUETH_PORT_QUEUE_HOST][0]));

	return 0;
}

#define prueth_mii_set(dir, port, mask, set) \
	prueth_set_reg(prueth, PRUETH_MEM_MII, \
		       PRUSS_MII_RT_##dir##CFG##port, \
		       PRUSS_MII_RT_##dir##CFG_##dir##_##mask, set)

static void prueth_mii_init(struct prueth *prueth)
{
	/* Configuration of Port 0 Rx */
	prueth_mii_set(RX, 0, ENABLE, PRUSS_MII_RT_RXCFG_RX_ENABLE);
	prueth_mii_set(RX, 0, DATA_RDY_MODE_DIS,
		       PRUSS_MII_RT_RXCFG_RX_DATA_RDY_MODE_DIS);
	prueth_mii_set(RX, 0, MUX_SEL, 0x0);
	prueth_mii_set(RX, 0, L2_EN, PRUSS_MII_RT_RXCFG_RX_L2_EN);
	prueth_mii_set(RX, 0, CUT_PREAMBLE, PRUSS_MII_RT_RXCFG_RX_CUT_PREAMBLE);
	prueth_mii_set(RX, 0, L2_EOF_SCLR_DIS,
		       PRUSS_MII_RT_RXCFG_RX_L2_EOF_SCLR_DIS);

	/* Configuration of Port 0 Tx */
	prueth_set_reg(prueth, PRUETH_MEM_MII, PRUSS_MII_RT_TX_IPG0,
		       PRUSS_MII_RT_TX_IPG_IPG_MASK, TX_MIN_IPG);
	prueth_mii_set(TX, 0, ENABLE, PRUSS_MII_RT_TXCFG_TX_ENABLE);
	prueth_mii_set(TX, 0, AUTO_PREAMBLE,
		       PRUSS_MII_RT_TXCFG_TX_AUTO_PREAMBLE);
	prueth_mii_set(TX, 0, 32_MODE_EN, PRUSS_MII_RT_TXCFG_TX_32_MODE_EN);

	if (PRUETH_HAS_SWITCH(prueth))
		prueth_mii_set(TX, 0, MUX_SEL, PRUSS_MII_RT_TXCFG_TX_MUX_SEL);
	else
		prueth_mii_set(TX, 0, MUX_SEL, 0x0);

	prueth_mii_set(TX, 0, START_DELAY_MASK,
		       TX_START_DELAY << PRUSS_MII_RT_TXCFG_TX_START_DELAY_SHIFT);
	prueth_mii_set(TX, 0, CLK_DELAY_MASK,
		       TX_CLK_DELAY << PRUSS_MII_RT_TXCFG_TX_CLK_DELAY_SHIFT);

	/* Configuration of Port 1 Rx */
	prueth_mii_set(RX, 1, ENABLE, PRUSS_MII_RT_RXCFG_RX_ENABLE);
	prueth_mii_set(RX, 1,
		       DATA_RDY_MODE_DIS, PRUSS_MII_RT_RXCFG_RX_DATA_RDY_MODE_DIS);
	prueth_mii_set(RX, 1, MUX_SEL, PRUSS_MII_RT_RXCFG_RX_MUX_SEL);
	prueth_mii_set(RX, 1, L2_EN, PRUSS_MII_RT_RXCFG_RX_L2_EN);
	prueth_mii_set(RX, 1, CUT_PREAMBLE, PRUSS_MII_RT_RXCFG_RX_CUT_PREAMBLE);
	prueth_mii_set(RX, 1, L2_EOF_SCLR_DIS,
		       PRUSS_MII_RT_RXCFG_RX_L2_EOF_SCLR_DIS);

	/* Configuration of Port 1 Tx */
	prueth_set_reg(prueth, PRUETH_MEM_MII, PRUSS_MII_RT_TX_IPG1,
		       PRUSS_MII_RT_TX_IPG_IPG_MASK, TX_MIN_IPG);
	prueth_mii_set(TX, 1, ENABLE, PRUSS_MII_RT_TXCFG_TX_ENABLE);
	prueth_mii_set(TX, 1, AUTO_PREAMBLE,
		       PRUSS_MII_RT_TXCFG_TX_AUTO_PREAMBLE);
	prueth_mii_set(TX, 1, 32_MODE_EN, PRUSS_MII_RT_TXCFG_TX_32_MODE_EN);

	if (PRUETH_HAS_SWITCH(prueth))
		prueth_mii_set(TX, 1, MUX_SEL, 0x0);
	else
		prueth_mii_set(TX, 1, MUX_SEL, PRUSS_MII_RT_TXCFG_TX_MUX_SEL);

	prueth_mii_set(TX, 1, START_DELAY_MASK,
		       TX_START_DELAY << PRUSS_MII_RT_TXCFG_TX_START_DELAY_SHIFT);
	prueth_mii_set(TX, 1, CLK_DELAY_MASK,
		       TX_CLK_DELAY << PRUSS_MII_RT_TXCFG_TX_CLK_DELAY_SHIFT);

	if (PRUETH_HAS_RED(prueth)) {
		prueth_set_reg(prueth, PRUETH_MEM_MII, PRUSS_MII_RT_RX_FRMS0,
			       PRUSS_MII_RT_RX_FRMS_MAX_FRM_MASK,
			       EMAC_MAX_PKTLEN_HSR <<
					PRUSS_MII_RT_RX_FRMS_MAX_FRM_SHIFT);
		prueth_set_reg(prueth, PRUETH_MEM_MII, PRUSS_MII_RT_RX_FRMS0,
			       PRUSS_MII_RT_RX_FRMS_MIN_FRM_MASK,
			       EMAC_MIN_PKTLEN <<
					PRUSS_MII_RT_RX_FRMS_MIN_FRM_SHIFT);
		prueth_set_reg(prueth, PRUETH_MEM_MII, PRUSS_MII_RT_RX_FRMS1,
			       PRUSS_MII_RT_RX_FRMS_MAX_FRM_MASK,
			       EMAC_MAX_PKTLEN_HSR <<
					PRUSS_MII_RT_RX_FRMS_MAX_FRM_SHIFT);
		prueth_set_reg(prueth, PRUETH_MEM_MII, PRUSS_MII_RT_RX_FRMS1,
			       PRUSS_MII_RT_RX_FRMS_MIN_FRM_MASK,
			       EMAC_MIN_PKTLEN <<
					PRUSS_MII_RT_RX_FRMS_MIN_FRM_SHIFT);
	}
}

static void prueth_clearmem(struct prueth *prueth, enum prueth_mem region)
{
	memset_io(prueth->mem[region].va, 0, prueth->mem[region].size);
}

static int prueth_hostinit(struct prueth *prueth)
{
	/* Clear shared RAM */
	prueth_clearmem(prueth, PRUETH_MEM_SHARED_RAM);

	/* Clear OCMC RAM */
	prueth_clearmem(prueth, PRUETH_MEM_OCMC);

	/* Clear data RAMs */
	prueth_clearmem(prueth, PRUETH_MEM_DRAM0);
	prueth_clearmem(prueth, PRUETH_MEM_DRAM1);

	/* Initialize host queues in shared RAM */
	if (PRUETH_HAS_SWITCH(prueth))
		prueth_sw_hostconfig(prueth);
	else
		prueth_hostconfig(prueth);

	/* Configure MII_RT */
	prueth_mii_init(prueth);

	/* Enable IEP Counter */
	prueth_set_reg(prueth, PRUETH_MEM_IEP, 0, 0xffff,
		       IEP_GLOBAL_CFG_REG_VAL);

	return 0;
}

static int prueth_port_enable(struct prueth *prueth, enum prueth_port port,
			      bool enable)
{
	void __iomem *port_ctrl;

	if (port == PRUETH_PORT_MII0)
		port_ctrl = (prueth->mem[PRUETH_MEM_DRAM0].va +
			     PORT_CONTROL_ADDR);
	else if (port == PRUETH_PORT_MII1)
		port_ctrl = (prueth->mem[PRUETH_MEM_DRAM1].va +
			     PORT_CONTROL_ADDR);
	else
		return -EINVAL;

	if (enable)
		writeb(0x1, port_ctrl);
	else
		writeb(0x0, port_ctrl);

	return 0;
}

static int prueth_sw_port_config(struct prueth *prueth,
				 enum prueth_port port_id)
{
	struct prueth_mmap_port_cfg_basis *pb;
	struct prueth_mmap_ocmc_cfg *oc = &prueth->mmap_ocmc_cfg;
	struct prueth_mmap_sram_cfg *s = &prueth->mmap_sram_cfg;
	void __iomem *dram, *dram_base, *dram_mac;
	struct prueth_emac *emac;
	unsigned int tx_context_ofs_addr, col_tx_context_ofs_addr,
		     rx_context_ofs, col_rx_context_ofs_addr,
		     queue_desc_ofs, col_queue_desc_ofs;
	int port_id_rx;

	pb = &prueth->mmap_port_cfg_basis[port_id];
	emac = prueth->emac[port_id];
	switch (port_id) {
	case PRUETH_PORT_MII0:
		port_id_rx = PRUETH_PORT_QUEUE_MII0_RX;

		tx_context_ofs_addr     = TX_CONTEXT_P1_Q1_OFFSET_ADDR;
		col_tx_context_ofs_addr = COL_TX_CONTEXT_P1_Q1_OFFSET_ADDR;
		rx_context_ofs          = P1_Q1_RX_CONTEXT_OFFSET;
		col_rx_context_ofs_addr = COL_RX_CONTEXT_P1_OFFSET_ADDR;
		queue_desc_ofs          = pb->queue1_desc_offset;
		col_queue_desc_ofs      = pb->col_queue_desc_offset;

		/* for switch PORT MII0 mac addr is in DRAM0. */
		dram_mac = prueth->mem[PRUETH_MEM_DRAM0].va;
		break;
	case PRUETH_PORT_MII1:
		port_id_rx = PRUETH_PORT_QUEUE_MII1_RX;

		tx_context_ofs_addr     = TX_CONTEXT_P2_Q1_OFFSET_ADDR;
		col_tx_context_ofs_addr = COL_TX_CONTEXT_P2_Q1_OFFSET_ADDR;
		rx_context_ofs          = P2_Q1_RX_CONTEXT_OFFSET;
		col_rx_context_ofs_addr = COL_RX_CONTEXT_P2_OFFSET_ADDR;
		queue_desc_ofs          = pb->queue1_desc_offset;
		col_queue_desc_ofs      = pb->col_queue_desc_offset;

		/* for switch PORT MII1 mac addr is in DRAM1. */
		dram_mac = prueth->mem[PRUETH_MEM_DRAM1].va;
		break;
	default:
		netdev_err(emac->ndev, "invalid port\n");
		return -EINVAL;
	}

	/* setup mac address */
	memcpy_toio(dram_mac + PORT_MAC_ADDR, emac->mac_addr, 6);

	/* Remaining switch port configs are in DRAM1 */
	dram_base = prueth->mem[PRUETH_MEM_DRAM1].va;

	/* queue information table */
	memcpy_toio(dram_base + tx_context_ofs_addr,
		    queue_infos[port_id],
		    sizeof(queue_infos[port_id]));

	memcpy_toio(dram_base + col_tx_context_ofs_addr,
		    &col_tx_context_infos[port_id],
		    sizeof(col_tx_context_infos[port_id]));

	memcpy_toio(dram_base + rx_context_ofs,
		    queue_infos[port_id_rx],
		    sizeof(queue_infos[port_id_rx]));

	memcpy_toio(dram_base + col_rx_context_ofs_addr,
		    &col_rx_context_infos[port_id],
		    sizeof(col_rx_context_infos[port_id]));

	/* buffer descriptor offset table*/
	dram = dram_base + QUEUE_DESCRIPTOR_OFFSET_ADDR +
	       (port_id * NUM_QUEUES * sizeof(u16));
	writew(s->bd_offset[port_id][PRUETH_QUEUE1], dram);
	writew(s->bd_offset[port_id][PRUETH_QUEUE2], dram + 2);
	writew(s->bd_offset[port_id][PRUETH_QUEUE3], dram + 4);
	writew(s->bd_offset[port_id][PRUETH_QUEUE4], dram + 6);

	/* buffer offset table */
	dram = dram_base + QUEUE_OFFSET_ADDR +
	       port_id * NUM_QUEUES * sizeof(u16);
	writew(oc->buffer_offset[port_id][PRUETH_QUEUE1], dram);
	writew(oc->buffer_offset[port_id][PRUETH_QUEUE2], dram + 2);
	writew(oc->buffer_offset[port_id][PRUETH_QUEUE3], dram + 4);
	writew(oc->buffer_offset[port_id][PRUETH_QUEUE4], dram + 6);

	/* queue size lookup table */
	dram = dram_base + QUEUE_SIZE_ADDR +
	       port_id * NUM_QUEUES * sizeof(u16);
	writew(pb->queue_size[PRUETH_QUEUE1], dram);
	writew(pb->queue_size[PRUETH_QUEUE2], dram + 2);
	writew(pb->queue_size[PRUETH_QUEUE3], dram + 4);
	writew(pb->queue_size[PRUETH_QUEUE4], dram + 6);

	/* collision queue table */
	memcpy_toio(dram_base + col_queue_desc_ofs,
		    &queue_descs[port_id][PRUETH_COLQ],
		    sizeof(queue_descs[port_id][PRUETH_COLQ]));

	/* queue table */
	memcpy_toio(dram_base + queue_desc_ofs,
		    &queue_descs[port_id][0],
		    4 * sizeof(queue_descs[port_id][0]));

	return 0;
}

static int prueth_sw_emac_config(struct prueth *prueth,
				 struct prueth_emac *emac)
{
	/* PRU needs local shared RAM address for C28 */
	u32 sharedramaddr = ICSS_LOCAL_SHARED_RAM;
	/* PRU needs real global OCMC address for C30*/
	u32 ocmcaddr = (u32)prueth->mem[PRUETH_MEM_OCMC].pa;
	int ret;

	if (prueth->emac_configured & BIT(emac->port_id))
		return 0;

	ret = prueth_sw_port_config(prueth, emac->port_id);
	if (ret)
		return ret;

	if (!prueth->emac_configured) {
		/* Set in constant table C28 of PRUn to ICSS Shared memory */
		pru_rproc_set_ctable(prueth->pru0, PRU_C28, sharedramaddr);
		pru_rproc_set_ctable(prueth->pru1, PRU_C28, sharedramaddr);

		/* Set in constant table C30 of PRUn to OCMC memory */
		pru_rproc_set_ctable(prueth->pru0, PRU_C30, ocmcaddr);
		pru_rproc_set_ctable(prueth->pru1, PRU_C30, ocmcaddr);
	}
	return 0;
}

static int prueth_emac_config(struct prueth *prueth, struct prueth_emac *emac)
{
	/* PRU needs local shared RAM address for C28 */
	u32 sharedramaddr = ICSS_LOCAL_SHARED_RAM;
	/* PRU needs real global OCMC address for C30*/
	u32 ocmcaddr = (u32)prueth->mem[PRUETH_MEM_OCMC].pa;
	void __iomem *dram_base;
	void __iomem *mac_addr;
	void __iomem *dram;

	switch (emac->port_id) {
	case PRUETH_PORT_MII0:
		/* Clear data RAM */
		prueth_clearmem(prueth, PRUETH_MEM_DRAM0);

		/* PORT MII0 mac addr is in DRAM0 for switch also. */
		dram_base = prueth->mem[PRUETH_MEM_DRAM0].va;
		/* setup mac address */
		mac_addr = dram_base + PORT_MAC_ADDR;
		memcpy_toio(mac_addr, emac->mac_addr, 6);

		/* queue information table */
		dram = dram_base + TX_CONTEXT_Q1_OFFSET_ADDR;
		memcpy_toio(dram, queue_infos[emac->port_id],
			    sizeof(queue_infos[emac->port_id]));

		/* queue table */
		dram = dram_base + PORT_QUEUE_DESC_OFFSET;
		memcpy_toio(dram, &queue_descs[emac->port_id][0],
			    4 * sizeof(queue_descs[emac->port_id][0]));

		/* Set in constant table C28 of PRU0 to ICSS Shared memory */
		pru_rproc_set_ctable(prueth->pru0, PRU_C28, sharedramaddr);
		/* Set in constant table C30 of PRU0 to OCMC memory */
		pru_rproc_set_ctable(prueth->pru0, PRU_C30, ocmcaddr);
		break;
	case PRUETH_PORT_MII1:
		/* Clear data RAM */
		prueth_clearmem(prueth, PRUETH_MEM_DRAM1);

		dram_base = prueth->mem[PRUETH_MEM_DRAM1].va;

		/* setup mac address */
		mac_addr = dram_base + PORT_MAC_ADDR;
		memcpy_toio(mac_addr, emac->mac_addr, 6);

		/* queue information table */
		dram = dram_base + TX_CONTEXT_Q1_OFFSET_ADDR;
		memcpy_toio(dram, &queue_infos[emac->port_id][0],
			    4 * sizeof(queue_infos[emac->port_id][0]));

		/* queue table */
		dram = dram_base + PORT_QUEUE_DESC_OFFSET;
		memcpy_toio(dram, &queue_descs[emac->port_id][0],
			    4 * sizeof(queue_descs[emac->port_id][0]));

		/* Set in constant table C28 of PRU1 to ICSS Shared memory */
		pru_rproc_set_ctable(prueth->pru1, PRU_C28, sharedramaddr);
		/* Set in constant table C30 of PRU1 to OCMC memory */
		pru_rproc_set_ctable(prueth->pru1, PRU_C30, ocmcaddr);
		break;
	default:
		netdev_err(emac->ndev, "invalid port\n");
		return -EINVAL;
	}

	return 0;
}

static int prueth_hsr_prp_pcp_rxq_map_config(struct prueth *prueth)
{
	void __iomem *sram  = prueth->mem[PRUETH_MEM_SHARED_RAM].va;
	int i, j, pcp = (NUM_VLAN_PCP / 2);
	u32 val;

	for (i = 0; i < 2; i++) {
		val = 0;
		for (j = 0; j < pcp; j++)
			val |= (prueth->pcp_rxq_map[i * pcp + j] << (j * 8));

		writel(val, sram + QUEUE_2_PCP_MAP_OFFSET + i * 4);
	}

	return 0;
}

static int prueth_hsr_prp_host_table_init(struct prueth *prueth)
{
	void __iomem *dram0 = prueth->mem[PRUETH_MEM_DRAM0].va;
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;

	memset_io(dram0 + DUPLICATE_HOST_TABLE, 0,
		  DUPLICATE_HOST_TABLE_DMEM_SIZE);

	writel(DUPLICATE_HOST_TABLE_SIZE_INIT,
	       dram1 + DUPLICATE_HOST_TABLE_SIZE);

	writel(TABLE_CHECK_RESOLUTION_10_MS,
	       dram1 + DUPLI_HOST_CHECK_RESO);

	writel(MASTER_SLAVE_BUSY_BITS_CLEAR,
	       dram1 + HOST_DUPLICATE_ARBITRATION);

	return 0;
}

static int prueth_hsr_prp_node_table_init(struct prueth *prueth)
{
	void __iomem *dram0 = prueth->mem[PRUETH_MEM_DRAM0].va;
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;
	void __iomem *sram  = prueth->mem[PRUETH_MEM_SHARED_RAM].va;
	u32 i, val;

	for (i = 0, val = NEXT_FREE_ADDRESS_NT_QUEUE_INIT;
	     i < NEXT_FREE_ADDRESS_NT_QUEUE_DMEM_SIZE;
	     i += sizeof(val), val += NEXT_FREE_ADDRESS_NT_QUEUE_STEP)
		writel(val, dram0 + NEXT_FREE_ADDRESS_NT_QUEUE + i);

	writel(POINTERS_FREE_ADDR_NODETABLE_INIT,
	       dram0 + POINTERS_FREE_ADDR_NODETABLE);

	writel(INDEX_ARRAY_INIT, sram + INDEX_ARRAY);
	memset_io(sram + NODE_TABLE, 0, NODE_TABLE_DMEM_SIZE);

	/* Set up guard values */
	writel(0, sram + NODE_TABLE);
	writel(0x00010000, sram + NODE_TABLE + 4);
	writel(0xffffffff, sram + NODE_TABLE_END);
	writel(0x0001ffff, sram + NODE_TABLE_END + 4);

	writel(NODE_TABLE_SIZE_MAX_PRU_INIT, dram1 + NODE_TABLE_SIZE);
	writel(MASTER_SLAVE_BUSY_BITS_CLEAR, dram1 + NODE_TABLE_ARBITRATION);
	writel(NODE_FORGET_TIME_60000_MS,    dram1 + NODE_FORGET_TIME);
	writel(TABLE_CHECK_RESOLUTION_10_MS, dram1 + NODETABLE_CHECK_RESO);
	return 0;
}

static int prueth_hsr_prp_port_table_init(struct prueth *prueth)
{
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;

	if (PRUETH_IS_HSR(prueth)) {
		memset_io(dram1 + DUPLICATE_PORT_TABLE_PRU0, 0,
			  DUPLICATE_PORT_TABLE_DMEM_SIZE);
		memset_io(dram1 + DUPLICATE_PORT_TABLE_PRU1, 0,
			  DUPLICATE_PORT_TABLE_DMEM_SIZE);

		writel(DUPLICATE_PORT_TABLE_SIZE_INIT,
		       dram1 + DUPLICATE_PORT_TABLE_SIZE);
	} else {
		writel(0, dram1 + DUPLICATE_PORT_TABLE_SIZE);
	}

	writel(TABLE_CHECK_RESOLUTION_10_MS, dram1 + DUPLI_PORT_CHECK_RESO);
	return 0;
}

static int prueth_hsr_prp_lre_init(struct prueth *prueth)
{
	void __iomem *sram = prueth->mem[PRUETH_MEM_SHARED_RAM].va;

	if (PRUETH_IS_HSR(prueth))
		memset_io(sram + LRE_START, 0, LRE_STATS_DMEM_SIZE_HSR);
	else
		memset_io(sram + LRE_START, 0, LRE_STATS_DMEM_SIZE);
	writel(IEC62439_CONST_DUPLICATE_DISCARD,
	       sram + LRE_DUPLICATE_DISCARD);
	writel(IEC62439_CONST_TRANSPARENT_RECEPTION_REMOVE_RCT,
	       sram + LRE_TRANSPARENT_RECEPTION);
	return 0;
}

static int prueth_hsr_prp_dbg_init(struct prueth *prueth)
{
	void __iomem *dram0 = prueth->mem[PRUETH_MEM_DRAM0].va;

	memset_io(dram0 + DBG_START, 0, DEBUG_COUNTER_DMEM_SIZE);
	return 0;
}

static int prueth_hsr_prp_protocol_init(struct prueth *prueth)
{
	void __iomem *dram0 = prueth->mem[PRUETH_MEM_DRAM0].va;
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;

	if (PRUETH_IS_HSR(prueth))
		writew(prueth->hsr_mode, dram0 + LRE_HSR_MODE);

	writel(DUPLICATE_FORGET_TIME_400_MS, dram1 + DUPLI_FORGET_TIME);
	writel(SUP_ADDRESS_INIT_OCTETS_HIGH, dram1 + SUP_ADDR);
	writel(SUP_ADDRESS_INIT_OCTETS_LOW,  dram1 + SUP_ADDR_LOW);
	return 0;
}

/* Assumes HAS_RED */
static void prueth_red_table_timer(unsigned long arg)
{
	struct prueth *prueth = (struct prueth *)arg;
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;

	if (prueth->node_table_clear) {
		prueth->tbl_check_mask |= HOST_TIMER_NODE_TABLE_CLEAR_BIT;
		prueth->node_table_clear = 0;
	} else {
		prueth->tbl_check_mask &= ~HOST_TIMER_NODE_TABLE_CLEAR_BIT;
	}

	writel(prueth->tbl_check_mask, dram1 + HOST_TIMER_CHECK_FLAGS);

	prueth->tbl_check_timer.expires = jiffies + prueth->tbl_check_period;
	if (prueth->emac_configured && prueth->tbl_check_period)
		add_timer(&prueth->tbl_check_timer);
}

static int prueth_init_red_table_timer(struct prueth *prueth)
{
	if (prueth->emac_configured)
		return 0;

	prueth->tbl_check_period = PRUETH_RED_TABLE_CHECK_PERIOD;
	prueth->tbl_check_timer.data = (unsigned long)prueth;
	prueth->tbl_check_timer.function = prueth_red_table_timer;
	prueth->tbl_check_mask = (HOST_TIMER_NODE_TABLE_CHECK_BIT |
				  HOST_TIMER_HOST_TABLE_CHECK_BIT);

	if (PRUETH_HAS_HSR(prueth))
		prueth->tbl_check_mask |= HOST_TIMER_PORT_TABLE_CHECK_BITS;

	return 0;
}

static int prueth_start_red_table_timer(struct prueth *prueth)
{
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;

	if (prueth->emac_configured)
		return 0;

	writel(prueth->tbl_check_mask, dram1 + HOST_TIMER_CHECK_FLAGS);
	prueth->tbl_check_timer.expires = jiffies + prueth->tbl_check_period;
	add_timer(&prueth->tbl_check_timer);
	return 0;
}

static int prueth_hsr_prp_config(struct prueth *prueth)
{
	if (prueth->emac_configured)
		return 0;

	prueth_hsr_prp_pcp_rxq_map_config(prueth);
	prueth_hsr_prp_host_table_init(prueth);
	prueth_hsr_prp_node_table_init(prueth);
	prueth_hsr_prp_port_table_init(prueth);
	prueth_hsr_prp_lre_init(prueth);
	prueth_hsr_prp_dbg_init(prueth);
	prueth_hsr_prp_protocol_init(prueth);

	return 0;
}

/* update phy/port status information for firmware */
static void emac_update_phystatus(struct prueth_emac *emac)
{
	struct prueth *prueth = emac->prueth;
	enum prueth_mem region;
	u32 phy_speed, port_status = 0;

	switch (emac->port_id) {
	case PRUETH_PORT_MII0:
		region = PRUETH_MEM_DRAM0;
		break;
	case PRUETH_PORT_MII1:
		region = PRUETH_MEM_DRAM1;
		break;
	default:
		netdev_err(emac->ndev, "phy %s, invalid port\n",
			   phydev_name(emac->phydev));
		return;
	}

	phy_speed = emac->speed;
	prueth_write_reg(prueth, region, PHY_SPEED_OFFSET, phy_speed);

	if (emac->duplex == DUPLEX_HALF)
		port_status |= PORT_IS_HD_MASK;
	if (emac->link)
		port_status |= PORT_LINK_MASK;
	writeb(port_status, prueth->mem[region].va + PORT_STATUS_OFFSET);
}

/* called back by PHY layer if there is change in link state of hw port*/
static void emac_adjust_link(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct phy_device *phydev = emac->phydev;
	unsigned long flags;
	bool new_state = false;

	spin_lock_irqsave(&emac->lock, flags);

	if (phydev->link) {
		/* check the mode of operation - full/half duplex */
		if (phydev->duplex != emac->duplex) {
			new_state = true;
			emac->duplex = phydev->duplex;
		}
		if (phydev->speed != emac->speed) {
			new_state = true;
			emac->speed = phydev->speed;
		}
		if (!emac->link) {
			new_state = true;
			emac->link = 1;
		}
	} else if (emac->link) {
		new_state = true;
		emac->link = 0;
		/* defaults for no link */

		/* f/w only support 10 or 100 */
		emac->speed = SPEED_100;

		/* half duplex may not be supported by f/w */
		emac->duplex = DUPLEX_FULL;
	}

	emac_update_phystatus(emac);

	if (new_state)
		phy_print_status(phydev);

	if (emac->link) {
		/* link ON */
		if (!netif_carrier_ok(ndev))
			netif_carrier_on(ndev);
		/* reactivate the transmit queue if it is stopped */
		if (netif_running(ndev) && netif_queue_stopped(ndev))
			netif_wake_queue(ndev);
	} else {
		/* link OFF */
		if (netif_carrier_ok(ndev))
			netif_carrier_off(ndev);
		if (!netif_queue_stopped(ndev))
			netif_stop_queue(ndev);
	}

	spin_unlock_irqrestore(&emac->lock, flags);
}

/**
 * emac_tx_hardirq - EMAC Tx interrupt handler
 * @irq: interrupt number
 * @dev_id: pointer to net_device
 *
 * This is called whenever a packet has finished being transmitted, this clears
 * up hardware buffer space, our only task is to re-enable the transmit queue
 * if it was previously disabled due to hardware queue being full
 *
 * Returns interrupt handled condition
 */
static irqreturn_t emac_tx_hardirq(int irq, void *dev_id)
{
	struct net_device *ndev = (struct net_device *)dev_id;

	if (unlikely(netif_queue_stopped(ndev)))
		netif_wake_queue(ndev);

	return IRQ_HANDLED;
}

/**
 * emac_rx_hardirq - EMAC Rx interrupt handler
 * @irq: interrupt number
 * @dev_id: pointer to net_device
 *
 * EMAC Interrupt handler - we only schedule NAPI and not process any packets
 * here.
 *
 * Returns interrupt handled condition
 */
static irqreturn_t emac_rx_hardirq(int irq, void *dev_id)
{
	struct net_device *ndev = (struct net_device *)dev_id;
	struct prueth_emac *emac = netdev_priv(ndev);

	if (likely(netif_running(ndev))) {
		/* disable Rx system event */
		disable_irq_nosync(emac->rx_irq);
		napi_schedule(&emac->napi);
	}

	return IRQ_HANDLED;
}

/**
 * prueth_tx_enqueue - queue a packet to firmware for transmission
 *
 * @emac: EMAC data structure
 * @skb: packet data buffer
 * @txport: which port to send MII0 or MII1
 * @queue_id: priority queue id
 */
static int prueth_tx_enqueue(struct prueth_emac *emac, struct sk_buff *skb,
			     int txport, enum prueth_queue_id queue_id)
{
	struct net_device *ndev = emac->ndev;
	int pktlen;
	struct prueth_queue_desc __iomem *queue_desc;
	const struct prueth_queue_info *txqueue;
	u16 bd_rd_ptr, bd_wr_ptr, update_wr_ptr;
	int write_block, read_block, free_blocks, update_block, pkt_block_size;
	unsigned int buffer_desc_count;
	bool buffer_wrapped = false;
	void *src_addr;
	void *dst_addr;
	/* OCMC RAM is not cached and write order is not important */
	void *ocmc_ram = (__force void *)emac->prueth->mem[PRUETH_MEM_OCMC].va;
	void __iomem *dram;
	u32 wr_buf_desc;
	int ret;
	bool colq_selected = false;
	void __iomem *sram = NULL;
	u8 status;

	switch (emac->port_id) {
	case PRUETH_PORT_MII0:
		dram = emac->prueth->mem[PRUETH_MEM_DRAM0].va;
		break;
	case PRUETH_PORT_MII1:
		dram = emac->prueth->mem[PRUETH_MEM_DRAM1].va;
		break;
	default:
		netdev_err(emac->ndev, "invalid port\n");
		return -EINVAL;
	}

	if (PRUETH_HAS_SWITCH(emac->prueth)) {
		sram = emac->prueth->mem[PRUETH_MEM_SHARED_RAM].va;
		dram = emac->prueth->mem[PRUETH_MEM_DRAM1].va;
	}

	ret = skb_padto(skb, EMAC_MIN_PKTLEN);
	if (ret) {
		if (netif_msg_tx_err(emac) && net_ratelimit())
			netdev_err(ndev, "packet pad failed");
		return ret;
	}
	src_addr = skb->data;

	/* pad packet if needed */
	pktlen = skb->len;
	if (pktlen < EMAC_MIN_PKTLEN)
		pktlen = EMAC_MIN_PKTLEN;

	/* Get the tx queue */
	queue_desc = emac->tx_queue_descs + queue_id;
	txqueue = &queue_infos[txport][queue_id];

	if (emac->tx_colq_descs) {
		/* Switch needs to handle tx collision */
		status = readb(&queue_desc->status);
		if (status & PRUETH_MASTER_QUEUE_BUSY) {
			/* Tx q busy, put pkt in col Q */
			++emac->tx_collisions;
			status = readb(dram + COLLISION_STATUS_ADDR + txport);
			if (status) {
				/* Tx colq busy also, drop pkt */
				++emac->tx_collision_drops;
				return -EBUSY;
			}
			/* Tx colq free, take it */
			txqueue = &tx_colq_infos[txport];
			queue_desc = emac->tx_colq_descs;
			colq_selected = true;
		} else {
			/* Tx q not busy. Acquire q by setting busy_s bit */
			writeb(0x1, &queue_desc->busy_s);

			/* Again check if host acquired q successfully
			 * by checking busy_m bit
			 */
			status = readb(&queue_desc->status);
			if (status & PRUETH_MASTER_QUEUE_BUSY) {
				/* Nope. Clear busy_s bit */
				writeb(0x0, &queue_desc->busy_s);

				/* tx q collision, put pkt in col Q */
				++emac->tx_collisions;
				txqueue = &tx_colq_infos[txport];
				queue_desc = emac->tx_colq_descs;
				colq_selected = true;
			}
		}
	}

	buffer_desc_count = txqueue->buffer_desc_end -
			    txqueue->buffer_desc_offset;
	buffer_desc_count /= BD_SIZE;
	buffer_desc_count++;

	bd_rd_ptr = readw(&queue_desc->rd_ptr);
	bd_wr_ptr = readw(&queue_desc->wr_ptr);

	/* the PRU firmware deals mostly in pointers already
	 * offset into ram, we would like to deal in indexes
	 * within the queue we are working with for code
	 * simplicity, calculate this here
	 */
	write_block = (bd_wr_ptr - txqueue->buffer_desc_offset) / BD_SIZE;
	read_block = (bd_rd_ptr - txqueue->buffer_desc_offset) / BD_SIZE;
	if (write_block > read_block) {
		free_blocks = buffer_desc_count - write_block;
		free_blocks += read_block;
	} else if (write_block < read_block) {
		free_blocks = read_block - write_block;
	} else { /* they are all free */
		free_blocks = buffer_desc_count;
	}
	pkt_block_size = DIV_ROUND_UP(pktlen, ICSS_BLOCK_SIZE);
	if (pkt_block_size > free_blocks) { /* out of queue space */
		/* Release the queue clear busy_s bit.
		 * This has no harm even in emac case.
		 */
		writeb(0x0, &queue_desc->busy_s);
		return -ENOBUFS;
	}
	/* calculate end BD address post write */
	update_block = write_block + pkt_block_size;
	/* Check for wrap around */
	if (update_block >= buffer_desc_count) {
		update_block %= buffer_desc_count;
		buffer_wrapped = true;
	}

	dst_addr = ocmc_ram + txqueue->buffer_offset +
		   (write_block * ICSS_BLOCK_SIZE);

	/* Copy the data from socket buffer(DRAM) to PRU buffers(OCMC) */
	if (buffer_wrapped) { /* wrapped around buffer */
		int bytes = (buffer_desc_count - write_block) * ICSS_BLOCK_SIZE;
		int remaining;
		/* bytes is integral multiple of ICSS_BLOCK_SIZE but
		 * entire packet may have fit within the last BD
		 * if pkt_info.length is not integral multiple of
		 * ICSS_BLOCK_SIZE
		 */
		if (pktlen < bytes)
			bytes = pktlen;

		/* copy non-wrapped part */
		memcpy(dst_addr, src_addr, bytes);

		/* copy wrapped part */
		src_addr += bytes;
		remaining = pktlen - bytes;
		if (colq_selected)
			/* +++TODO: should not happen */
			dst_addr += bytes;
		else
			dst_addr = ocmc_ram + txqueue->buffer_offset;
		memcpy(dst_addr, src_addr, remaining);
	} else {
		memcpy(dst_addr, src_addr, pktlen);
	}

	/* update first buffer descriptor */
	wr_buf_desc = (pktlen << PRUETH_BD_LENGTH_SHIFT) & PRUETH_BD_LENGTH_MASK;

	if (PRUETH_IS_HSR(emac->prueth))
		wr_buf_desc |= BIT(PRUETH_BD_HSR_FRAME_SHIFT);

	if (PRUETH_HAS_SWITCH(emac->prueth))
		writel(wr_buf_desc, sram + bd_wr_ptr);
	else
		writel(wr_buf_desc, dram + bd_wr_ptr);

	/* update the write pointer in this queue descriptor, the firmware
	 * polls for this change so this will signal the start of transmission
	 */
	update_wr_ptr = txqueue->buffer_desc_offset + (update_block * BD_SIZE);
	writew(update_wr_ptr, &queue_desc->wr_ptr);

	/* release the queue clear busy_s bit */
	writeb(0x0, &queue_desc->busy_s);

	/* if packet was put in collision queue then
	 * indiciate it to collision task
	 */
	if (colq_selected)
		writeb((queue_id << 1) | 0x01,
		       dram + COLLISION_STATUS_ADDR + txport);

	return 0;
}

static void parse_packet_info(struct prueth *prueth, u32 buffer_descriptor,
			      struct prueth_packet_info *pkt_info)
{
	if (PRUETH_IS_HSR(prueth))
		pkt_info->start_offset = !!(buffer_descriptor &
					    PRUETH_BD_START_FLAG_MASK);
	else
		pkt_info->start_offset = false;

	pkt_info->shadow = !!(buffer_descriptor & PRUETH_BD_SHADOW_MASK);
	pkt_info->port = (buffer_descriptor & PRUETH_BD_PORT_MASK) >>
			 PRUETH_BD_PORT_SHIFT;
	pkt_info->length = (buffer_descriptor & PRUETH_BD_LENGTH_MASK) >>
			   PRUETH_BD_LENGTH_SHIFT;
	pkt_info->broadcast = !!(buffer_descriptor & PRUETH_BD_BROADCAST_MASK);
	pkt_info->error = !!(buffer_descriptor & PRUETH_BD_ERROR_MASK);
}

/* get packet from queue
 * negative for error
 */
static int emac_rx_packet(struct prueth_emac *emac, u16 *bd_rd_ptr,
			  struct prueth_packet_info pkt_info,
			  const struct prueth_queue_info *rxqueue)
{
	struct prueth_mmap_port_cfg_basis *pb;
	struct net_device *ndev = emac->ndev;
	int read_block, update_block, pkt_block_size;
	unsigned int buffer_desc_count;
	bool buffer_wrapped = false;
	struct sk_buff *skb;
	void *src_addr;
	void *dst_addr;
	/* OCMC RAM is not cached and read order is not important */
	void *ocmc_ram = (__force void *)emac->prueth->mem[PRUETH_MEM_OCMC].va;
	unsigned int actual_pkt_len;

	u16 start_offset = (pkt_info.start_offset ? HSR_TAG_SIZE : 0);

	/* the PRU firmware deals mostly in pointers already
	 * offset into ram, we would like to deal in indexes
	 * within the queue we are working with for code
	 * simplicity, calculate this here
	 */
	buffer_desc_count = rxqueue->buffer_desc_end -
			    rxqueue->buffer_desc_offset;
	buffer_desc_count /= BD_SIZE;
	buffer_desc_count++;
	read_block = (*bd_rd_ptr - rxqueue->buffer_desc_offset) / BD_SIZE;
	pkt_block_size = DIV_ROUND_UP(pkt_info.length, ICSS_BLOCK_SIZE);
	/* calculate end BD address post read */
	update_block = read_block + pkt_block_size;
	/* Check for wrap around */
	if (update_block >= buffer_desc_count) {
		update_block %= buffer_desc_count;
		buffer_wrapped = true;
	}

	/* calculate new pointer in ram */
	*bd_rd_ptr = rxqueue->buffer_desc_offset + (update_block * BD_SIZE);

	/* Allocate a socket buffer for this packet */
	skb = netdev_alloc_skb_ip_align(ndev, pkt_info.length);
	if (!skb) {
		if (netif_msg_rx_err(emac) && net_ratelimit())
			netdev_err(ndev, "failed rx buffer alloc\n");
		return -ENOMEM;
	}
	dst_addr = skb->data;

	/* Get the start address of the first buffer from
	 * the read buffer description
	 */
	if (pkt_info.shadow) {
		pb = &emac->prueth->mmap_port_cfg_basis[PRUETH_PORT_HOST];
		src_addr = ocmc_ram + pb->col_buff_offset + start_offset;
	} else {
		src_addr = ocmc_ram +
			   rxqueue->buffer_offset +
			   (read_block * ICSS_BLOCK_SIZE) +
			   start_offset;
	}

	/* Pkt len w/ HSR tag removed, If applicable */
	actual_pkt_len = pkt_info.length - start_offset;

	/* Copy the data from PRU buffers(OCMC) to socket buffer(DRAM) */
	if (buffer_wrapped) { /* wrapped around buffer */
		int bytes = (buffer_desc_count - read_block) * ICSS_BLOCK_SIZE;
		int remaining;
		/* bytes is integral multiple of ICSS_BLOCK_SIZE but
		 * entire packet may have fit within the last BD
		 * if pkt_info.length is not integral multiple of
		 * ICSS_BLOCK_SIZE
		 */
		if (pkt_info.length < bytes)
			bytes = pkt_info.length;

		/* If applicable, account for the HSR tag removed */
		bytes -= start_offset;

		/* copy non-wrapped part */
		memcpy(dst_addr, src_addr, bytes);

		/* copy wrapped part */
		dst_addr += bytes;
		remaining = actual_pkt_len - bytes;
		if (pkt_info.shadow)
			src_addr += bytes;
		else
			src_addr = ocmc_ram + rxqueue->buffer_offset;
		memcpy(dst_addr, src_addr, remaining);
	} else {
		memcpy(dst_addr, src_addr, actual_pkt_len);
	}

	/* send packet up the stack */
	skb_put(skb, pkt_info.length);
	skb->protocol = eth_type_trans(skb, ndev);
	netif_receive_skb(skb);

	/* update stats */
	ndev->stats.rx_bytes += pkt_info.length;
	ndev->stats.rx_packets++;

	return 0;
}

/* get upto quota number of packets */
static int emac_rx_packets(struct prueth_emac *emac, int quota)
{
	struct prueth_queue_desc __iomem *queue_desc, *colq_desc;
	const struct prueth_queue_info *rxqueue;
	struct prueth *prueth;
	u8 overflow_cnt;
	u8 status;
	u16 bd_rd_ptr, bd_wr_ptr, update_rd_ptr;
	u32 rd_buf_desc;
	void __iomem *shared_ram = emac->prueth->mem[PRUETH_MEM_SHARED_RAM].va;
	void __iomem *dram1 = emac->prueth->mem[PRUETH_MEM_DRAM1].va;
	struct prueth_packet_info pkt_info;
	struct net_device_stats *ndevstats = &emac->ndev->stats;
	int i, j, ret, used = 0;
	struct prueth_emac *other_emac;
	const unsigned int *prio_q_ids;
	unsigned int q_cnt;
	unsigned int emac_max_pktlen = EMAC_MAX_PKTLEN;
	bool rx_err = false;

	prueth = emac->prueth;

	if (PRUETH_HAS_SWITCH(prueth)) {
		prio_q_ids = &sw_port_rx_priority_queue_ids[0];
		q_cnt = sw_num_rx_queues;
	} else {
		prio_q_ids = emac_port_rx_priority_queue_ids[emac->port_id];
		q_cnt = emac_num_rx_queues;
	}

	/* search host queues for packets */
	for (j = 0; j < q_cnt; j++) {
		i = prio_q_ids[j];
		queue_desc = emac->rx_queue_descs + i;
		rxqueue = &queue_infos[PRUETH_PORT_HOST][i];

		status = readb(&queue_desc->status);
		/* check overflow status */
		if (status & PRUETH_PACKET_DISCARD_OVFL) {
			emac->rx_overflows++;
			if (PRUETH_HAS_SWITCH(prueth)) {
				other_emac = prueth->emac[emac->port_id ^ 0x3];
				other_emac->rx_overflows++;
			}
		}

		overflow_cnt = readb(&queue_desc->overflow_cnt);
		if (overflow_cnt > 0) {
			emac->ndev->stats.rx_over_errors += overflow_cnt;

			/* In SWITCH case, rx qs are shared by both ports,
			 * probably best thing to do is to inc
			 * rx_over_errors on both emac for now
			 */
			if (PRUETH_HAS_SWITCH(prueth)) {
				other_emac = prueth->emac[emac->port_id ^ 0x3];
				other_emac->ndev->stats.rx_over_errors +=
					overflow_cnt;
			}

			/* reset to zero */
			writeb(0, &queue_desc->overflow_cnt);
		}

		bd_rd_ptr = readw(&queue_desc->rd_ptr);
		bd_wr_ptr = readw(&queue_desc->wr_ptr);

		/* while packets are available in this queue */
		while (bd_rd_ptr != bd_wr_ptr) {
			/* get packet info from the read buffer descriptor */
			rd_buf_desc = readl(shared_ram + bd_rd_ptr);
			parse_packet_info(prueth, rd_buf_desc, &pkt_info);

			if (PRUETH_HAS_SWITCH(prueth)) {
				if (pkt_info.port == 1) {
					emac = prueth->emac[PRUETH_PORT_MII0];
					ndevstats = &emac->ndev->stats;
				} else if (pkt_info.port == 2) {
					emac = prueth->emac[PRUETH_PORT_MII1];
					ndevstats = &emac->ndev->stats;
				} else {
					netdev_err(emac->ndev,
						   "unknown rx port %u in bd 0x%08x\n",
						   pkt_info.port, rd_buf_desc);
					/* something wrong. drop all packets */
					pkt_info.length = 0;
					rx_err = true;
				}
			}

			if (PRUETH_IS_HSR(prueth))
				emac_max_pktlen = EMAC_MAX_PKTLEN_HSR;

			if (pkt_info.length <= 0) {
				/* a packet length of zero will cause us to
				 * never move the read pointer ahead, locking
				 * the driver, so we manually have to move it
				 * to the write pointer, discarding all
				 * remaining packets in this queue. This should
				 * never happen.
				 */
				update_rd_ptr = bd_wr_ptr;
				ndevstats->rx_length_errors++;
				rx_err = true;
			} else if (pkt_info.length > emac_max_pktlen) {
				/* if the packet is too large we skip it but we
				 * still need to move the read pointer ahead
				 * and assume something is wrong with the read
				 * pointer as the firmware should be filtering
				 * these packets
				 */
				update_rd_ptr = bd_wr_ptr;
				ndevstats->rx_length_errors++;
				rx_err = true;
			} else {
				update_rd_ptr = bd_rd_ptr;
				ret = emac_rx_packet(emac, &update_rd_ptr,
						     pkt_info, rxqueue);
				if (ret)
					return ret;

				used++;
			}

			/* after reading the buffer descriptor we clear it
			 * to prevent improperly moved read pointer errors
			 * from simply looking like old packets.
			 */
			writel(0, shared_ram + bd_rd_ptr);

			/* update read pointer in queue descriptor */
			writew(update_rd_ptr, &queue_desc->rd_ptr);
			bd_rd_ptr = update_rd_ptr;

			/* if switch and buffer is from colq, update colq
			 * wr_ptr and clear col status reg bit to indicate
			 * host has read the pkt. Emac won't go in here as
			 * shaddow = false
			 */
			if (pkt_info.shadow && !rx_err) {
				colq_desc = emac->rx_colq_descs;
				writew(colq_desc->rd_ptr, &colq_desc->wr_ptr);
				writeb(0, dram1 + COLLISION_STATUS_ADDR);
			}

			/* all we have room for? */
			if (used >= quota)
				return used;
		}
	}

	return used;
}

/* get statistics maintained by the PRU firmware into @pstats */
static void emac_get_stats(struct prueth_emac *emac,
			   struct port_statistics *pstats)
{
	void __iomem *dram;

	if (emac->port_id == PRUETH_PORT_MII0)
		dram = emac->prueth->mem[PRUETH_MEM_DRAM0].va;
	else
		dram = emac->prueth->mem[PRUETH_MEM_DRAM1].va;

	memcpy_fromio(pstats, dram + STATISTICS_OFFSET, sizeof(*pstats));
}

/* set PRU firmware statistics */
static void emac_set_stats(struct prueth_emac *emac,
			   struct port_statistics *pstats)
{
	void __iomem *dram;

	if (emac->port_id == PRUETH_PORT_MII0)
		dram = emac->prueth->mem[PRUETH_MEM_DRAM0].va;
	else
		dram = emac->prueth->mem[PRUETH_MEM_DRAM1].va;

	memcpy_fromio(dram + STATISTICS_OFFSET, pstats, sizeof(*pstats));
}

static void emac_lre_get_stats(struct prueth_emac *emac,
			       struct lre_statistics *pstats)
{
	void __iomem *sram = emac->prueth->mem[PRUETH_MEM_SHARED_RAM].va;

	memcpy_fromio(pstats, sram + LRE_CNT_TX_A, sizeof(*pstats));
}

static void emac_lre_set_stats(struct prueth_emac *emac,
			       struct lre_statistics *pstats)
{
	void __iomem *sram = emac->prueth->mem[PRUETH_MEM_SHARED_RAM].va;

	/* These two are actually not statistics, so keep roiginal */
	pstats->duplicate_discard = readl(sram + LRE_DUPLICATE_DISCARD);
	pstats->transparent_reception = readl(sram + LRE_TRANSPARENT_RECEPTION);
	memcpy_fromio(sram + LRE_START + 4, pstats, sizeof(*pstats));
}

/**
 * emac_napi_poll - EMAC NAPI Poll function
 * @ndev: EMAC network adapter
 * @budget: Number of receive packets to process (as told by NAPI layer)
 *
 * NAPI Poll function implemented to process packets as per budget. We check
 * the type of interrupt on the device and accordingly call the TX or RX
 * packet processing functions. We follow the budget for RX processing and
 * also put a cap on number of TX pkts processed through config param. The
 * NAPI schedule function is called if more packets pending.
 *
 * Returns number of packets received (in most cases; else TX pkts - rarely)
 */
static int emac_napi_poll(struct napi_struct *napi, int budget)
{
	struct prueth_emac *emac = container_of(napi, struct prueth_emac, napi);
	int num_rx_packets;

	num_rx_packets = emac_rx_packets(emac, budget);
	if (num_rx_packets < budget) {
		napi_complete(napi);

		enable_irq(emac->rx_irq);
	}

	return num_rx_packets;
}

static int sw_emac_set_boot_pru(struct prueth_emac *emac,
				struct net_device *ndev)
{
	const struct prueth_firmwares *pru_firmwares;
	struct prueth *prueth = emac->prueth;
	const char *fw_name;
	int ret = 0;

	if (prueth->emac_configured)
		return 0;

	/* opening first intf, boot up both PRUs:
	 *   Rx is done by local PRU
	 *   Tx is done by the other PRU
	 */
	emac_lre_set_stats(emac, &prueth->lre_stats);

	/* PRU0: set firmware and boot */
	pru_firmwares = &prueth->fw_data->fw_pru[0];
	fw_name = pru_firmwares->fw_name[prueth->eth_type];
	ret = rproc_set_firmware(prueth->pru0, fw_name);
	if (ret) {
		netdev_err(ndev, "failed to set PRU0 firmware %s: %d\n",
			   fw_name, ret);
		goto out;
	}
	ret = rproc_boot(prueth->pru0);
	if (ret) {
		netdev_err(ndev, "failed to boot PRU0: %d\n", ret);
		goto out;
	}

	/* PRU1: set firmware and boot */
	pru_firmwares = &prueth->fw_data->fw_pru[1];
	fw_name = pru_firmwares->fw_name[prueth->eth_type];
	ret = rproc_set_firmware(prueth->pru1, fw_name);
	if (ret) {
		netdev_err(ndev, "failed to set PRU1 firmware %s: %d\n",
			   fw_name, ret);
		goto out;
	}
	ret = rproc_boot(prueth->pru1);
	if (ret)
		netdev_err(ndev, "failed to boot PRU1: %d\n", ret);

out:
	return ret;
}

static int emac_set_boot_pru(struct prueth_emac *emac, struct net_device *ndev)
{
	const struct prueth_firmwares *pru_firmwares;
	struct prueth *prueth = emac->prueth;
	const char *fw_name;
	int ret = 0;

	pru_firmwares = &prueth->fw_data->fw_pru[emac->port_id - 1];
	fw_name = pru_firmwares->fw_name[prueth->eth_type];

	switch (emac->port_id) {
	case PRUETH_PORT_MII0:
		ret = rproc_set_firmware(prueth->pru0, fw_name);
		if (ret) {
			netdev_err(ndev, "failed to set PRU0 firmware %s: %d\n",
				   fw_name, ret);
			break;
		}

		ret = rproc_boot(prueth->pru0);
		if (ret)
			netdev_err(ndev, "failed to boot PRU0: %d\n", ret);

		break;
	case PRUETH_PORT_MII1:
		ret = rproc_set_firmware(prueth->pru1, fw_name);
		if (ret) {
			netdev_err(ndev, "failed to set PRU1 firmware %s: %d\n",
				   fw_name, ret);
			break;
		}

		ret = rproc_boot(prueth->pru1);
		if (ret)
			netdev_err(ndev, "failed to boot PRU1: %d\n", ret);

		break;
	default:
		/* switch mode not supported yet */
		netdev_err(ndev, "invalid port\n");
		ret = -EINVAL;
		break;
	}

	return ret;
}

/**
 * emac_ndo_open - EMAC device open
 * @ndev: network adapter device
 *
 * Called when system wants to start the interface.
 *
 * Returns 0 for a successful open, or appropriate error code
 */
static int emac_ndo_open(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth *prueth = emac->prueth;
	unsigned long flags = (IRQF_TRIGGER_HIGH | IRQF_ONESHOT);
	int ret;

	if (PRUETH_HAS_SWITCH(prueth))
		flags |= IRQF_SHARED;

	ret = request_irq(emac->rx_irq, emac_rx_hardirq, flags,
			  ndev->name, ndev);
	if (ret) {
		netdev_err(ndev, "unable to request RX IRQ\n");
		return ret;
	}
	ret = request_irq(emac->tx_irq, emac_tx_hardirq, flags,
			  ndev->name, ndev);
	if (ret) {
		netdev_err(ndev, "unable to request TX IRQ\n");
		goto free_rx_irq;
	}

	/* set h/w MAC as user might have re-configured */
	ether_addr_copy(emac->mac_addr, ndev->dev_addr);

	netif_carrier_off(ndev);

	/* reset and start PRU firmware */
	if (PRUETH_HAS_SWITCH(prueth))
		prueth_sw_emac_config(prueth, emac);
	else
		prueth_emac_config(prueth, emac);

	if (PRUETH_HAS_RED(prueth)) {
		prueth_init_red_table_timer(prueth);
		prueth_hsr_prp_config(prueth);
	}

	/* restore stats */
	emac_set_stats(emac, &emac->stats);

	if (PRUETH_HAS_SWITCH(prueth))
		ret = sw_emac_set_boot_pru(emac, ndev);
	else
		ret = emac_set_boot_pru(emac, ndev);

	if (ret)
		goto free_irq;

	/* start PHY */
	phy_start(emac->phydev);
	napi_enable(&emac->napi);

	/* enable the port */
	prueth_port_enable(prueth, emac->port_id, true);

	if (netif_msg_drv(emac))
		dev_notice(&ndev->dev, "started\n");

	if (PRUETH_HAS_RED(prueth))
		prueth_start_red_table_timer(prueth);

	prueth->emac_configured |= BIT(emac->port_id);
	return 0;

free_irq:
	free_irq(emac->tx_irq, ndev);
free_rx_irq:
	free_irq(emac->rx_irq, ndev);

	return ret;
}

static int sw_emac_pru_stop(struct prueth_emac *emac, struct net_device *ndev)
{
	struct prueth *prueth = emac->prueth;

	prueth->emac_configured &= ~BIT(emac->port_id);

	/* another emac is still in use, don't stop the PRUs */
	if (prueth->emac_configured)
		return 0;

	rproc_shutdown(prueth->pru0);
	rproc_shutdown(prueth->pru1);
	/* disable and free rx and tx interrupts */
	disable_irq(emac->tx_irq);
	disable_irq(emac->rx_irq);
	free_irq(emac->tx_irq, ndev);
	free_irq(emac->rx_irq, ndev);
	emac_lre_get_stats(emac, &emac->prueth->lre_stats);

	if (PRUETH_HAS_RED(emac->prueth)) {
		del_timer_sync(&prueth->tbl_check_timer);
		prueth->tbl_check_period = 0;
	}

	return 0;
}

static int emac_pru_stop(struct prueth_emac *emac, struct net_device *ndev)
{
	struct prueth *prueth = emac->prueth;

	switch (emac->port_id) {
	case PRUETH_PORT_MII0:
		rproc_shutdown(prueth->pru0);
		break;
	case PRUETH_PORT_MII1:
		rproc_shutdown(prueth->pru1);
		break;
	default:
		/* switch mode not supported yet */
		netdev_err(ndev, "invalid port\n");
	}

	/* disable and free rx and tx interrupts */
	disable_irq(emac->tx_irq);
	disable_irq(emac->rx_irq);
	free_irq(emac->tx_irq, ndev);
	free_irq(emac->rx_irq, ndev);
	return 0;
}

/**
 * emac_ndo_stop - EMAC device stop
 * @ndev: network adapter device
 *
 * Called when system wants to stop or down the interface.
 */
static int emac_ndo_stop(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);

	/* inform the upper layers. */
	netif_stop_queue(ndev);
	napi_disable(&emac->napi);
	netif_carrier_off(ndev);

	/* stop PHY */
	phy_stop(emac->phydev);

	/* disable the mac port */
	prueth_port_enable(emac->prueth, emac->port_id, 0);

	/* stop PRU firmware */
	if (PRUETH_HAS_SWITCH(emac->prueth))
		sw_emac_pru_stop(emac, ndev);
	else
		emac_pru_stop(emac, ndev);

	/* save stats */
	emac_get_stats(emac, &emac->stats);

	if (netif_msg_drv(emac))
		dev_notice(&ndev->dev, "stopped\n");

	return 0;
}

static u16 prueth_get_tx_queue_id(struct prueth *prueth, struct sk_buff *skb)
{
	u16 vlan_tci, pcp;
	int err;

	if (!PRUETH_HAS_SWITCH(prueth))
		return PRUETH_QUEUE4;

	err = vlan_get_tag(skb, &vlan_tci);
	if (likely(err))
		return PRUETH_QUEUE4;

	pcp = (vlan_tci & VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT;

	return sw_pcp_tx_priority_queue_map[pcp];
}

/**
 * emac_ndo_start_xmit - EMAC Transmit function
 * @skb: SKB pointer
 * @ndev: EMAC network adapter
 *
 * Called by the system to transmit a packet  - we queue the packet in
 * EMAC hardware transmit queue
 *
 * Returns success(NETDEV_TX_OK) or error code (typically out of desc's)
 */
static int emac_ndo_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	int ret = 0;
	u16 qid;

	if (unlikely(!emac->link)) {
		if (netif_msg_tx_err(emac) && net_ratelimit())
			netdev_err(ndev, "No link to transmit");
		ret = -ENOLINK;
		goto fail_tx;
	}

	qid = prueth_get_tx_queue_id(emac->prueth, skb);
	if (emac->port_id == PRUETH_PORT_MII0) {
		/* packet sent on MII0 */
		ret = prueth_tx_enqueue(emac, skb, PRUETH_PORT_QUEUE_MII0,
					qid);
	} else if (emac->port_id == PRUETH_PORT_MII1) {
		/* packet sent on MII1 */
		ret = prueth_tx_enqueue(emac, skb, PRUETH_PORT_QUEUE_MII1,
					qid);
	} else {
		goto fail_tx; /* switch mode not supported yet */
	}

	if (ret) {
		if (ret != -ENOBUFS && ret != -EBUSY &&
		    netif_msg_tx_err(emac) && net_ratelimit())
			netdev_err(ndev, "packet queue failed: %d\n", ret);
		goto fail_tx;
	}

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;

fail_tx:
	/* error */
	ndev->stats.tx_dropped++;
	return NETDEV_TX_BUSY;
}

/**
 * emac_ndo_tx_timeout - EMAC Transmit timeout function
 * @ndev: The EMAC network adapter
 *
 * Called when system detects that a skb timeout period has expired
 * potentially due to a fault in the adapter in not being able to send
 * it out on the wire.
 */
static void emac_ndo_tx_timeout(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);

	if (netif_msg_tx_err(emac))
		netdev_err(ndev, "xmit timeout");

	ndev->stats.tx_errors++;

	/* TODO: can we recover or need to reboot firmware? */
}

/**
 * emac_ndo_getstats - EMAC get statistics function
 * @ndev: The EMAC network adapter
 *
 * Called when system wants to get statistics from the device.
 *
 * We return the statistics in net_device_stats structure pulled from emac
 */
static struct net_device_stats *emac_ndo_get_stats(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct port_statistics pstats;
	struct net_device_stats *stats = &ndev->stats;

	emac_get_stats(emac, &pstats);
	stats->collisions = pstats.late_coll + pstats.single_coll +
			    pstats.multi_coll + pstats.excess_coll;
	stats->multicast = pstats.rx_mcast;

	return stats;
}

static const struct net_device_ops emac_netdev_ops = {
	.ndo_open = emac_ndo_open,
	.ndo_stop = emac_ndo_stop,
	.ndo_start_xmit = emac_ndo_start_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_change_mtu	= eth_change_mtu,
	.ndo_tx_timeout = emac_ndo_tx_timeout,
	.ndo_get_stats = emac_ndo_get_stats,
	/* +++TODO: implement .ndo_setup_tc */
};

/**
 * emac_get_drvinfo - Get EMAC driver information
 * @ndev: The network adapter
 * @info: ethtool info structure containing name and version
 *
 * Returns EMAC driver information (name and version)
 */
static void emac_get_drvinfo(struct net_device *ndev,
			     struct ethtool_drvinfo *info)
{
	strlcpy(info->driver, PRUETH_MODULE_DESCRIPTION, sizeof(info->driver));
	strlcpy(info->version, PRUETH_MODULE_VERSION, sizeof(info->version));
}

/**
 * emac_get_settings - Get EMAC settings
 * @ndev: The network adapter
 * @ecmd: ethtool command
 *
 * Executes ethool get command
 */
static int emac_get_settings(struct net_device *ndev, struct ethtool_cmd *ecmd)
{
	struct prueth_emac *emac = netdev_priv(ndev);

	if (emac->phydev)
		return phy_ethtool_gset(emac->phydev, ecmd);
	else
		return -EOPNOTSUPP;
}

/**
 * emac_set_settings - Set EMAC settings
 * @ndev: The EMAC network adapter
 * @ecmd: ethtool command
 *
 * Executes ethool set command
 */
static int emac_set_settings(struct net_device *ndev, struct ethtool_cmd *ecmd)
{
	struct prueth_emac *emac = netdev_priv(ndev);

	if (emac->phydev)
		return phy_ethtool_sset(emac->phydev, ecmd);
	else
		return -EOPNOTSUPP;
}

#define PRUETH_STAT_OFFSET(m) offsetof(struct port_statistics, m)

static const struct {
	char string[ETH_GSTRING_LEN];
	u32 offset;
} prueth_ethtool_stats[] = {
	{"txBcast", PRUETH_STAT_OFFSET(tx_bcast)},
	{"txMcast", PRUETH_STAT_OFFSET(tx_mcast)},
	{"txUcast", PRUETH_STAT_OFFSET(tx_ucast)},
	{"txOctets", PRUETH_STAT_OFFSET(tx_octets)},
	{"rxBcast", PRUETH_STAT_OFFSET(rx_bcast)},
	{"rxMcast", PRUETH_STAT_OFFSET(rx_mcast)},
	{"rxUcast", PRUETH_STAT_OFFSET(rx_ucast)},
	{"rxOctets", PRUETH_STAT_OFFSET(rx_octets)},

	{"tx64byte", PRUETH_STAT_OFFSET(tx64byte)},
	{"tx65_127byte", PRUETH_STAT_OFFSET(tx65_127byte)},
	{"tx128_255byte", PRUETH_STAT_OFFSET(tx128_255byte)},
	{"tx256_511byte", PRUETH_STAT_OFFSET(tx256_511byte)},
	{"tx512_1023byte", PRUETH_STAT_OFFSET(tx512_1023byte)},
	{"tx1024byte", PRUETH_STAT_OFFSET(tx1024byte)},

	{"rx64byte", PRUETH_STAT_OFFSET(rx64byte)},
	{"rx65_127byte", PRUETH_STAT_OFFSET(rx65_127byte)},
	{"rx128_255byte", PRUETH_STAT_OFFSET(rx128_255byte)},
	{"rx256_511byte", PRUETH_STAT_OFFSET(rx256_511byte)},
	{"rx512_1023byte", PRUETH_STAT_OFFSET(rx512_1023byte)},
	{"rx1024byte", PRUETH_STAT_OFFSET(rx1024byte)},

	{"lateColl", PRUETH_STAT_OFFSET(late_coll)},
	{"singleColl", PRUETH_STAT_OFFSET(single_coll)},
	{"multiColl", PRUETH_STAT_OFFSET(multi_coll)},
	{"excessColl", PRUETH_STAT_OFFSET(excess_coll)},

	{"rxMisAlignmentFrames", PRUETH_STAT_OFFSET(rx_misalignment_frames)},
	{"stormPrevCounter", PRUETH_STAT_OFFSET(stormprev_counter)},
	{"macRxError", PRUETH_STAT_OFFSET(mac_rxerror)},
	{"SFDError", PRUETH_STAT_OFFSET(sfd_error)},
	{"defTx", PRUETH_STAT_OFFSET(def_tx)},
	{"macTxError", PRUETH_STAT_OFFSET(mac_txerror)},
	{"rxOverSizedFrames", PRUETH_STAT_OFFSET(rx_oversized_frames)},
	{"rxUnderSizedFrames", PRUETH_STAT_OFFSET(rx_undersized_frames)},
	{"rxCRCFrames", PRUETH_STAT_OFFSET(rx_crc_frames)},
	{"droppedPackets", PRUETH_STAT_OFFSET(dropped_packets)},

	{"txHWQOverFlow", PRUETH_STAT_OFFSET(tx_hwq_overflow)},
	{"txHWQUnderFlow", PRUETH_STAT_OFFSET(tx_hwq_underflow)},
};

#define PRUETH_LRE_STAT_OFS(m) offsetof(struct lre_statistics, m)
static const struct {
	char string[ETH_GSTRING_LEN];
	u32 offset;
} prueth_ethtool_lre_stats[] = {
	{"lreTxA", PRUETH_LRE_STAT_OFS(cnt_tx_a)},
	{"lreTxB", PRUETH_LRE_STAT_OFS(cnt_tx_b)},
	{"lreTxC", PRUETH_LRE_STAT_OFS(cnt_tx_c)},

	{"lreErrWrongLanA", PRUETH_LRE_STAT_OFS(cnt_errwronglan_a)},
	{"lreErrWrongLanB", PRUETH_LRE_STAT_OFS(cnt_errwronglan_b)},
	{"lreErrWrongLanC", PRUETH_LRE_STAT_OFS(cnt_errwronglan_c)},

	{"lreRxA", PRUETH_LRE_STAT_OFS(cnt_rx_a)},
	{"lreRxB", PRUETH_LRE_STAT_OFS(cnt_rx_b)},
	{"lreRxC", PRUETH_LRE_STAT_OFS(cnt_rx_c)},

	{"lreErrorsA", PRUETH_LRE_STAT_OFS(cnt_errors_a)},
	{"lreErrorsB", PRUETH_LRE_STAT_OFS(cnt_errors_b)},
	{"lreErrorsC", PRUETH_LRE_STAT_OFS(cnt_errors_c)},

	{"lreNodes", PRUETH_LRE_STAT_OFS(cnt_nodes)},
	{"lreProxyNodes", PRUETH_LRE_STAT_OFS(cnt_proxy_nodes)},

	{"lreUniqueRxA", PRUETH_LRE_STAT_OFS(cnt_unique_rx_a)},
	{"lreUniqueRxB", PRUETH_LRE_STAT_OFS(cnt_unique_rx_b)},
	{"lreUniqueRxC", PRUETH_LRE_STAT_OFS(cnt_unique_rx_c)},

	{"lreDuplicateRxA", PRUETH_LRE_STAT_OFS(cnt_duplicate_rx_a)},
	{"lreDuplicateRxB", PRUETH_LRE_STAT_OFS(cnt_duplicate_rx_b)},
	{"lreDuplicateRxC", PRUETH_LRE_STAT_OFS(cnt_duplicate_rx_c)},

	{"lreMultiRxA", PRUETH_LRE_STAT_OFS(cnt_multiple_rx_a)},
	{"lreMultiRxB", PRUETH_LRE_STAT_OFS(cnt_multiple_rx_b)},
	{"lreMultiRxC", PRUETH_LRE_STAT_OFS(cnt_multiple_rx_c)},

	{"lreOwnRxA", PRUETH_LRE_STAT_OFS(cnt_own_rx_a)},
	{"lreOwnRxB", PRUETH_LRE_STAT_OFS(cnt_own_rx_b)},

	{"lreDuplicateDiscard", PRUETH_LRE_STAT_OFS(duplicate_discard)},
	{"lreTransRecept", PRUETH_LRE_STAT_OFS(transparent_reception)},

	{"lreNtLookupErrA", PRUETH_LRE_STAT_OFS(node_table_lookup_error_a)},
	{"lreNtLookupErrB", PRUETH_LRE_STAT_OFS(node_table_lookup_error_b)},
	{"lreNodeTableFull", PRUETH_LRE_STAT_OFS(node_table_full)},
};

static int emac_get_sset_count(struct net_device *ndev, int stringset)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	int a_size;

	switch (stringset) {
	case ETH_SS_STATS:
		a_size = ARRAY_SIZE(prueth_ethtool_stats);

		if (PRUETH_HAS_RED(emac->prueth))
			a_size += ARRAY_SIZE(prueth_ethtool_lre_stats);

		return a_size;
	default:
		return -EOPNOTSUPP;
	}
}

static void emac_get_strings(struct net_device *ndev, u32 stringset, u8 *data)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	u8 *p = data;
	int i;

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < ARRAY_SIZE(prueth_ethtool_stats); i++) {
			memcpy(p, prueth_ethtool_stats[i].string,
			       ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}

		if (!PRUETH_HAS_RED(emac->prueth))
			break;

		for (i = 0; i < ARRAY_SIZE(prueth_ethtool_lre_stats); i++) {
			memcpy(p, prueth_ethtool_lre_stats[i].string,
			       ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}
		break;
	default:
		break;
	}
}

static void emac_get_ethtool_stats(struct net_device *ndev,
				   struct ethtool_stats *stats, u64 *data)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct port_statistics pstats;
	u32 val;
	int i;
	void *ptr;
	struct lre_statistics lre_stats;
	int lre_start;

	emac_get_stats(emac, &pstats);

	for (i = 0; i < ARRAY_SIZE(prueth_ethtool_stats); i++) {
		ptr = &pstats;
		ptr += prueth_ethtool_stats[i].offset;
		val = *(u32 *)ptr;
		data[i] = val;
	}

	if (PRUETH_HAS_RED(emac->prueth)) {
		lre_start = ARRAY_SIZE(prueth_ethtool_stats);
		emac_lre_get_stats(emac, &lre_stats);
		for (i = 0; i < ARRAY_SIZE(prueth_ethtool_lre_stats); i++) {
			ptr = &lre_stats;
			ptr += prueth_ethtool_lre_stats[i].offset;
			val = *(u32 *)ptr;
			data[lre_start + i] = val;
		}
	}
}

/* Ethtool support for EMAC adapter */
static const struct ethtool_ops emac_ethtool_ops = {
	.get_drvinfo = emac_get_drvinfo,
	.get_settings = emac_get_settings,
	.set_settings = emac_set_settings,
	.get_link = ethtool_op_get_link,
	.get_ts_info = ethtool_op_get_ts_info,
	.get_sset_count = emac_get_sset_count,
	.get_strings = emac_get_strings,
	.get_ethtool_stats = emac_get_ethtool_stats,
};

/* get emac_port corresponding to eth_node name */
static int prueth_node_port(struct device_node *eth_node)
{
	if (!strcmp(eth_node->name, "ethernet-mii0"))
		return PRUETH_PORT_MII0;
	else if (!strcmp(eth_node->name, "ethernet-mii1"))
		return PRUETH_PORT_MII1;
	else
		return -EINVAL;
}

static int prueth_netdev_init(struct prueth *prueth,
			      struct device_node *eth_node)
{
	struct prueth_mmap_sram_cfg *s = &prueth->mmap_sram_cfg;
	struct prueth_mmap_sram_emac *emac_sram = &s->mmap_sram_emac;
	struct prueth_mmap_port_cfg_basis *pb0, *pb;
	enum prueth_port port;
	struct net_device *ndev;
	struct prueth_emac *emac;
	void __iomem *sram = prueth->mem[PRUETH_MEM_SHARED_RAM].va;
	void __iomem *dram0 = prueth->mem[PRUETH_MEM_DRAM0].va;
	void __iomem *dram1 = prueth->mem[PRUETH_MEM_DRAM1].va;
	const u8 *mac_addr;
	char *rx_int, *tx_int;
	int ret;

	port = prueth_node_port(eth_node);
	if (port < 0)
		return -EINVAL;

	/* +++TODO: use alloc_etherdev_mqs() */
	ndev = alloc_etherdev(sizeof(*emac));
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, prueth->dev);
	emac = netdev_priv(ndev);
	prueth->emac[port] = emac;
	emac->prueth = prueth;
	emac->ndev = ndev;
	emac->port_id = port;

	if (PRUETH_HAS_SWITCH(prueth)) {
		rx_int = "red-rx";
		tx_int = "red-tx";
	} else {
		rx_int = "rx";
		tx_int = "tx";
	}

	emac->rx_irq = of_irq_get_byname(eth_node, rx_int);
	if (emac->rx_irq < 0) {
		ret = emac->rx_irq;
		if (ret != -EPROBE_DEFER)
			dev_err(prueth->dev, "could not get rx irq\n");
		goto free;
	}
	emac->tx_irq = of_irq_get_byname(eth_node, tx_int);
	if (emac->tx_irq < 0) {
		ret = emac->tx_irq;
		if (ret != -EPROBE_DEFER)
			dev_err(prueth->dev, "could not get tx irq\n");
		goto free;
	}

	emac->msg_enable = netif_msg_init(debug_level, PRUETH_EMAC_DEBUG);
	spin_lock_init(&emac->lock);

	pb0 = &prueth->mmap_port_cfg_basis[PRUETH_PORT_HOST];
	pb  = &prueth->mmap_port_cfg_basis[port];
	switch (port) {
	case PRUETH_PORT_MII0:
		if (PRUETH_HAS_SWITCH(prueth)) {
			emac->rx_queue_descs =
				dram1 + pb0->queue1_desc_offset;
			emac->rx_colq_descs  =
				dram1 + pb0->col_queue_desc_offset;
			emac->tx_queue_descs =
				dram1 + pb->queue1_desc_offset;
			emac->tx_colq_descs  =
				dram1 + pb->col_queue_desc_offset;
		} else {
			emac->rx_queue_descs =
				sram + emac_sram->host_queue_desc_offset;
			emac->tx_queue_descs = dram0 + PORT_QUEUE_DESC_OFFSET;
		}
		break;
	case PRUETH_PORT_MII1:
		if (PRUETH_HAS_SWITCH(prueth)) {
			emac->rx_queue_descs =
				dram1 + pb0->queue1_desc_offset;
			emac->rx_colq_descs  =
				dram1 + pb0->col_queue_desc_offset;
			emac->tx_queue_descs =
				dram1 + pb->queue1_desc_offset;
			emac->tx_colq_descs  =
				dram1 + pb->col_queue_desc_offset;
		} else {
			emac->rx_queue_descs =
				sram + emac_sram->host_queue_desc_offset;
			emac->tx_queue_descs = dram1 + PORT_QUEUE_DESC_OFFSET;
		}
		break;
	default:
		dev_err(prueth->dev, "invalid port ID\n");
		ret = -EINVAL;
		goto free;
	}

	/* get mac address from DT and set private and netdev addr */
	mac_addr = of_get_mac_address(eth_node);
	if (mac_addr)
		ether_addr_copy(ndev->dev_addr, mac_addr);
	if (!is_valid_ether_addr(ndev->dev_addr)) {
		eth_hw_addr_random(ndev);
		dev_warn(prueth->dev, "port %d: using random MAC addr: %pM\n",
			 port, ndev->dev_addr);
	}
	ether_addr_copy(emac->mac_addr, ndev->dev_addr);

	emac->phy_node = of_parse_phandle(eth_node, "phy-handle", 0);
	if (!emac->phy_node) {
		dev_err(prueth->dev, "couldn't find phy-handle\n");
		ret = -ENODEV;
		goto free;
	}

	emac->phy_if = of_get_phy_mode(eth_node);
	if (emac->phy_if < 0) {
		dev_err(prueth->dev, "could not get phy-mode property\n");
		ret = emac->phy_if;
		goto free;
	}

	/* connect PHY */
	emac->phydev = of_phy_connect(ndev, emac->phy_node,
				      &emac_adjust_link, 0, emac->phy_if);
	if (!emac->phydev) {
		dev_dbg(prueth->dev, "couldn't connect to phy %s\n",
			emac->phy_node->full_name);
		ret = -EPROBE_DEFER;
		goto free;
	}

	emac->phydev->advertising &= ~(ADVERTISED_1000baseT_Full |
			ADVERTISED_1000baseT_Half);
	emac->phydev->supported &= ~(SUPPORTED_1000baseT_Full |
			SUPPORTED_1000baseT_Half);

	if (PRUETH_IS_HSR(prueth))
		ndev->features |= (NETIF_F_HW_HSR_RX_OFFLOAD |
				   NETIF_F_HW_L2FW_DOFFLOAD);
	else if (PRUETH_IS_PRP(prueth))
		ndev->features |= (NETIF_F_HW_PRP_RX_OFFLOAD |
				   NETIF_F_HW_L2FW_DOFFLOAD);

	ndev->netdev_ops = &emac_netdev_ops;
	ndev->ethtool_ops = &emac_ethtool_ops;

	netif_napi_add(ndev, &emac->napi, emac_napi_poll, EMAC_POLL_WEIGHT);

	return 0;

free:
	free_netdev(ndev);
	prueth->emac[port] = NULL;

	return ret;
}

static void prueth_netdev_exit(struct prueth *prueth,
			       struct device_node *eth_node)
{
	struct prueth_emac *emac;
	enum prueth_port port;

	port = prueth_node_port(eth_node);
	if (port < 0)
		return;

	emac = prueth->emac[port];
	if (!emac)
		return;

	dev_info(prueth->dev, "freeing port %d\n", port);

	phy_disconnect(emac->phydev);

	netif_napi_del(&emac->napi);
	free_netdev(emac->ndev);
	prueth->emac[port] = NULL;
}

static u16 port_queue_size(struct prueth *prueth, int p, int q)
{
	if (p < PRUETH_PORT_HOST || p > PRUETH_PORT_MII1 ||
	    q < PRUETH_QUEUE1    || q > PRUETH_QUEUE4)
		return 0xffff;

	return prueth->mmap_port_cfg_basis[p].queue_size[q];
}

/**
 * For both EMAC and Switch, all Px Qy buffers are in OCMC RAM
 * Regular Q buffer offsets depends only on P0_Q1_BUFFER_OFFSET
 * and Q sizes. Thus all such offsets can be derived from the
 * P0_Q1_BUFFER_OFFSET defined and Q sizes chosen.
 *
 * For Switch, COLQ buffers are treated differently:
 * based on P0_COL_BUFFER_OFFSET defined.
 *
 * This recurrsive function assumes buffers for 1 port is in
 * one continuous block of mem and buffers for 2 consecutive ports
 * are in one continuous block of mem as well.
 *
 * If buffers for 2 consecutive ports are not in one continuous block,
 * just modify the case where q == PRUETH_QUEUE1. But keep in mind
 * that non-continuous may have impact on fw performance.
 */
static u16 port_queue_buffer_offset(struct prueth *prueth, int p, int q)
{
	if (p < PRUETH_PORT_HOST || p > PRUETH_PORT_MII1 ||
	    q < PRUETH_QUEUE1    || q > PRUETH_QUEUE4)
		return 0xffff;

	if (p == PRUETH_PORT_HOST && q == PRUETH_QUEUE1)
		return prueth->mmap_port_cfg_basis[p].queue1_buff_offset;

	if (p > PRUETH_PORT_HOST   &&
	    p <= PRUETH_PORT_MII1  &&
	    q == PRUETH_QUEUE1)
		return port_queue_buffer_offset(prueth, p - 1, PRUETH_QUEUE4) +
		       port_queue_size(prueth, p - 1, PRUETH_QUEUE4) *
		       ICSS_BLOCK_SIZE;

	/* case (0 <= p <= 2) and (QUEUE1 < q <= QUEUE4) */
	return port_queue_buffer_offset(prueth, p, q - 1) +
	       port_queue_size(prueth, p, q - 1) * ICSS_BLOCK_SIZE;
}

/**
 * For both Switch and EMAC, all Px Qy BDs are in SRAM
 * Regular BD offsets depends on P0_Q1_BD_OFFSET and Q sizes.
 * Thus all can be calculated based on P0_Q1_BD_OFFSET defined and
 * Q sizes chosen.
 *
 * This recurrsive function assumes BDs for 1 port is in
 * one continuous block of mem and BDs for 2 consecutive ports
 * are in one continuous block of mem also.
 *
 * If BDs for 2 consecutive ports are not in one continuous block,
 * just modify the case where q == PRUETH_QUEUE1. But keep in mind
 * that non-continuity may have impact on fw performance.
 */
static u16 port_queue_bd_offset(struct prueth *prueth, int p, int q)
{
	if (p < PRUETH_PORT_HOST || p > PRUETH_PORT_MII1 ||
	    q < PRUETH_QUEUE1    || q > PRUETH_QUEUE4)
		return 0xffff;

	if (p == PRUETH_PORT_HOST && q == PRUETH_QUEUE1)
		return prueth->mmap_port_cfg_basis[p].queue1_bd_offset;

	/* continuous BDs between ports
	 */
	if (p > PRUETH_PORT_HOST   &&
	    p <= PRUETH_PORT_MII1  &&
	    q == PRUETH_QUEUE1)
		return port_queue_bd_offset(prueth, p - 1, PRUETH_QUEUE4) +
		       port_queue_size(prueth, p - 1, PRUETH_QUEUE4) *
		       BD_SIZE;

	/* (0 <= p <= 2) and (QUEUE1 < q <= QUEUE4)
	 * continuous BDs within 1 port
	 */
	return port_queue_bd_offset(prueth, p, q - 1) +
	       port_queue_size(prueth, p, q - 1) * BD_SIZE;
}

static u16 port_queue1_desc_offset(struct prueth *prueth, int p)
{
	if (p < PRUETH_PORT_HOST || p > PRUETH_PORT_MII1)
		return 0xffff;

	return prueth->mmap_port_cfg_basis[p].queue1_desc_offset;
}

static void prueth_init_host_port_queue_info(
	struct prueth *prueth,
	struct prueth_queue_info queue_infos[][NUM_QUEUES],
	struct prueth_mmap_port_cfg_basis *basis
)
{
	int p = PRUETH_PORT_HOST, q;
	struct prueth_queue_info *qi = queue_infos[p];

	/* PRUETH_QUEUE1 = 0, PRUETH_QUEUE2 = 1, ... */
	for (q = PRUETH_QUEUE1; q < NUM_QUEUES; q++) {
		qi[q].buffer_offset =
			port_queue_buffer_offset(prueth, p, q);

		qi[q].queue_desc_offset =
			port_queue1_desc_offset(prueth, p) +
			q * QDESC_SIZE;

		qi[q].buffer_desc_offset =
			port_queue_bd_offset(prueth, p, q);

		qi[q].buffer_desc_end =
			qi[q].buffer_desc_offset +
			(port_queue_size(prueth, p, q) - 1) * BD_SIZE;
	}
}

static void prueth_init_port_tx_queue_info(
	struct prueth *prueth,
	struct prueth_queue_info queue_infos[][NUM_QUEUES],
	struct prueth_mmap_port_cfg_basis *basis,
	int p
)
{
	struct prueth_queue_info *qi = queue_infos[p];
	int q;

	if (p < PRUETH_PORT_QUEUE_MII0 || p > PRUETH_PORT_QUEUE_MII1)
		return;

	/* PRUETH_QUEUE1 = 0, PRUETH_QUEUE2 = 1, ... */
	for (q = PRUETH_QUEUE1; q < NUM_QUEUES; q++) {
		qi[q].buffer_offset =
			port_queue_buffer_offset(prueth, p, q);

		/* this is actually buffer offset end for tx ports */
		qi[q].queue_desc_offset =
			qi[q].buffer_offset +
			(port_queue_size(prueth, p, q) - 1) * ICSS_BLOCK_SIZE;

		qi[q].buffer_desc_offset =
			port_queue_bd_offset(prueth, p, q);

		qi[q].buffer_desc_end =
			qi[q].buffer_desc_offset +
			(port_queue_size(prueth, p, q) - 1) * BD_SIZE;
	}
}

static void prueth_init_port_rx_queue_info(
	struct prueth *prueth,
	struct prueth_queue_info queue_infos[][NUM_QUEUES],
	struct prueth_mmap_port_cfg_basis *basis,
	int p_rx
)
{
	struct prueth_queue_info *qi = queue_infos[p_rx];
	int basisp, q;

	if (p_rx == PRUETH_PORT_QUEUE_MII0_RX)
		basisp = PRUETH_PORT_QUEUE_MII0;
	else if (p_rx == PRUETH_PORT_QUEUE_MII1_RX)
		basisp = PRUETH_PORT_QUEUE_MII1;
	else
		return;

	/* PRUETH_QUEUE1 = 0, PRUETH_QUEUE2 = 1, ... */
	for (q = PRUETH_QUEUE1; q < NUM_QUEUES; q++) {
		qi[q].buffer_offset =
			port_queue_buffer_offset(prueth, basisp, q);

		qi[q].queue_desc_offset =
			port_queue1_desc_offset(prueth, basisp) +
			q * QDESC_SIZE;

		qi[q].buffer_desc_offset =
			port_queue_bd_offset(prueth, basisp, q);

		qi[q].buffer_desc_end =
			qi[q].buffer_desc_offset +
			(port_queue_size(prueth, basisp, q) - 1) * BD_SIZE;
	}
}

static void
prueth_init_tx_colq_info(struct prueth *prueth,
			 struct prueth_queue_info *tx_colq_infos,
			 struct prueth_mmap_port_cfg_basis *sw_basis)
{
	struct prueth_mmap_port_cfg_basis *pb;
	struct prueth_queue_info *cqi;
	int p;

	for (p = PRUETH_PORT_QUEUE_MII0; p <= PRUETH_PORT_QUEUE_MII1; p++) {
		pb = &sw_basis[p];
		cqi = &tx_colq_infos[p];

		cqi->buffer_offset      = pb->col_buff_offset;
		cqi->queue_desc_offset  = pb->col_queue_desc_offset;
		cqi->buffer_desc_offset = pb->col_bd_offset;
		cqi->buffer_desc_end    =
			pb->col_bd_offset + (pb->col_queue_size - 1) * BD_SIZE;
	}
}

static void
prueth_init_col_tx_context_info(struct prueth *prueth,
				struct prueth_col_tx_context_info *ctx_infos,
				struct prueth_mmap_port_cfg_basis *sw_basis)
{
	struct prueth_mmap_port_cfg_basis *pb;
	struct prueth_col_tx_context_info *cti;
	int p;

	for (p = PRUETH_PORT_QUEUE_MII0; p <= PRUETH_PORT_QUEUE_MII1; p++) {
		pb = &sw_basis[p];
		cti = &ctx_infos[p];

		cti->buffer_offset      = pb->col_buff_offset;
		cti->buffer_offset2     = pb->col_buff_offset;
		cti->buffer_offset_end  =
			pb->col_buff_offset +
			(pb->col_queue_size - 1) * ICSS_BLOCK_SIZE;
	}
}

static void
prueth_init_col_rx_context_info(struct prueth *prueth,
				struct prueth_col_rx_context_info *ctx_infos,
				struct prueth_mmap_port_cfg_basis *sw_basis)
{
	struct prueth_mmap_port_cfg_basis *pb;
	struct prueth_col_rx_context_info *cti;
	int p;

	for (p = PRUETH_PORT_QUEUE_HOST; p <= PRUETH_PORT_QUEUE_MII1; p++) {
		cti = &ctx_infos[p];
		pb = &sw_basis[p];

		cti->buffer_offset      = pb->col_buff_offset;
		cti->buffer_offset2     = pb->col_buff_offset;
		cti->queue_desc_offset  = pb->col_queue_desc_offset;
		cti->buffer_desc_offset = pb->col_bd_offset;
		cti->buffer_desc_end    =
			pb->col_bd_offset +
			(pb->col_queue_size - 1) * BD_SIZE;
	}
}

static void
prueth_init_queue_descs(struct prueth *prueth,
			struct prueth_queue_desc queue_descs[][NUM_QUEUES + 1],
			struct prueth_mmap_port_cfg_basis *basis)
{
	struct prueth_queue_desc *d;
	int p, q;

	for (p = PRUETH_PORT_QUEUE_HOST; p <= PRUETH_PORT_QUEUE_MII1; p++) {
		for (q = PRUETH_QUEUE1; q <= PRUETH_QUEUE4; q++) {
			d = &queue_descs[p][q];
			d->rd_ptr = port_queue_bd_offset(prueth, p, q);
			d->wr_ptr = d->rd_ptr;
		}

		/* EMAC does not have colq and this will
		 * just set the rd_ptr and wr_ptr to 0
		 */
		d = &queue_descs[p][q];
		d->rd_ptr = basis[p].col_bd_offset;
		d->wr_ptr = d->rd_ptr;
	}
}

static void prueth_sw_mmap_port_cfg_basis_fixup(struct prueth *prueth)
{
	struct prueth_mmap_port_cfg_basis *pb, *prev_pb;
	u16 eof_48k_buffer_bd;

	/** HOST port **/
	pb = &prueth->mmap_port_cfg_basis[PRUETH_PORT_HOST];
	pb->queue1_buff_offset    = P0_Q1_BUFFER_OFFSET,
	pb->queue1_bd_offset      = P0_Q1_BD_OFFSET;
	pb->queue1_desc_offset    = P0_QUEUE_DESC_OFFSET,
	pb->col_buff_offset       = P0_COL_BUFFER_OFFSET,
	pb->col_queue_desc_offset = P0_COL_QUEUE_DESC_OFFSET;

	/* This calculation recurrsively depends on
	 * [PRUETH_PORT_HOST].queue1_bd_offset.
	 * So can only be done after
	 * [PRUETH_PORT_HOST].queue1_bd_offset is set
	 */
	eof_48k_buffer_bd =
		port_queue_bd_offset(prueth, PRUETH_PORT_MII1, PRUETH_QUEUE4) +
		port_queue_size(prueth, PRUETH_PORT_MII1, PRUETH_QUEUE4) *
		BD_SIZE;

	pb->col_bd_offset = eof_48k_buffer_bd;

	/** PORT_MII0 **/
	prev_pb = pb;
	pb = &prueth->mmap_port_cfg_basis[PRUETH_PORT_MII0];

	pb->queue1_buff_offset =
		port_queue_buffer_offset(prueth, PRUETH_PORT_MII0,
					 PRUETH_QUEUE1);

	pb->queue1_bd_offset =
		port_queue_bd_offset(prueth, PRUETH_PORT_MII0, PRUETH_QUEUE1);

	pb->queue1_desc_offset =
		prev_pb->queue1_desc_offset +
		NUM_QUEUES * QDESC_SIZE;

	pb->col_buff_offset =
		prev_pb->col_buff_offset +
		prev_pb->col_queue_size * ICSS_BLOCK_SIZE;

	pb->col_bd_offset =
		prev_pb->col_bd_offset +
		prev_pb->col_queue_size * BD_SIZE;

	pb->col_queue_desc_offset =
		prev_pb->col_queue_desc_offset + QDESC_SIZE;

	/** PORT_MII1 **/
	prev_pb = pb;
	pb = &prueth->mmap_port_cfg_basis[PRUETH_PORT_MII1];

	pb->queue1_buff_offset =
		port_queue_buffer_offset(prueth, PRUETH_PORT_MII1,
					 PRUETH_QUEUE1);

	pb->queue1_bd_offset =
		port_queue_bd_offset(prueth, PRUETH_PORT_MII1, PRUETH_QUEUE1);

	pb->queue1_desc_offset =
		prev_pb->queue1_desc_offset + NUM_QUEUES * QDESC_SIZE;

	pb->col_buff_offset =
		prev_pb->col_buff_offset +
		prev_pb->col_queue_size * ICSS_BLOCK_SIZE;

	pb->col_bd_offset =
		prev_pb->col_bd_offset +
		prev_pb->col_queue_size * BD_SIZE;

	pb->col_queue_desc_offset =
		prev_pb->col_queue_desc_offset + QDESC_SIZE;
}

static void prueth_emac_mmap_port_cfg_basis_fixup(struct prueth *prueth)
{
	struct prueth_mmap_port_cfg_basis *pb, *prev_pb;
	u16 eof_48k_buffer_bd;

	/** HOST port **/
	pb = &prueth->mmap_port_cfg_basis[PRUETH_PORT_HOST];
	pb->queue1_buff_offset    = P0_Q1_BUFFER_OFFSET,
	pb->queue1_bd_offset      = P0_Q1_BD_OFFSET;

	/* this calculation recurrsively depends on queue1_bd_offset,
	 * so can only be done after queue1_bd_offset is set
	 */
	eof_48k_buffer_bd =
		port_queue_bd_offset(prueth, PRUETH_PORT_MII1, PRUETH_QUEUE4) +
		port_queue_size(prueth, PRUETH_PORT_MII1, PRUETH_QUEUE4) *
		BD_SIZE;

	pb->queue1_desc_offset = eof_48k_buffer_bd +
					EMAC_P0_Q1_DESC_OFFSET_AFTER_BD;

	/** PORT_MII0 **/
	prev_pb = pb;
	pb = &prueth->mmap_port_cfg_basis[PRUETH_PORT_MII0];

	pb->queue1_buff_offset =
		port_queue_buffer_offset(prueth, PRUETH_PORT_MII0,
					 PRUETH_QUEUE1);

	pb->queue1_bd_offset =
		port_queue_bd_offset(prueth, PRUETH_PORT_MII0, PRUETH_QUEUE1);

	pb->queue1_desc_offset = PORT_QUEUE_DESC_OFFSET;

	/** PORT_MII1 **/
	prev_pb = pb;
	pb = &prueth->mmap_port_cfg_basis[PRUETH_PORT_MII1];

	pb->queue1_buff_offset =
		port_queue_buffer_offset(prueth, PRUETH_PORT_MII1,
					 PRUETH_QUEUE1);

	pb->queue1_bd_offset =
		port_queue_bd_offset(prueth, PRUETH_PORT_MII1, PRUETH_QUEUE1);

	pb->queue1_desc_offset = PORT_QUEUE_DESC_OFFSET;
}

static int prueth_emac_init_mmap_port_cfg(struct prueth *prueth)
{
	struct prueth_mmap_port_cfg_basis *b = &prueth->mmap_port_cfg_basis[0];

	prueth_init_host_port_queue_info(prueth, queue_infos, b);
	prueth_init_port_tx_queue_info(prueth, queue_infos, b,
				       PRUETH_PORT_QUEUE_MII0);
	prueth_init_port_tx_queue_info(prueth, queue_infos, b,
				       PRUETH_PORT_QUEUE_MII1);
	prueth_init_queue_descs(prueth, queue_descs, b);
	return 0;
}

static int prueth_sw_init_mmap_port_cfg(struct prueth *prueth)
{
	struct prueth_mmap_port_cfg_basis *b = &prueth->mmap_port_cfg_basis[0];

	prueth_init_host_port_queue_info(prueth, queue_infos, b);
	prueth_init_port_tx_queue_info(prueth, queue_infos, b,
				       PRUETH_PORT_QUEUE_MII0);
	prueth_init_port_tx_queue_info(prueth, queue_infos, b,
				       PRUETH_PORT_QUEUE_MII1);
	prueth_init_port_rx_queue_info(prueth, queue_infos, b,
				       PRUETH_PORT_QUEUE_MII0_RX);
	prueth_init_port_rx_queue_info(prueth, queue_infos, b,
				       PRUETH_PORT_QUEUE_MII1_RX);
	prueth_init_tx_colq_info(prueth, &tx_colq_infos[0], b);
	prueth_init_col_tx_context_info(prueth, &col_tx_context_infos[0], b);
	prueth_init_col_rx_context_info(prueth, &col_rx_context_infos[0], b);
	prueth_init_queue_descs(prueth, queue_descs, b);
	return 0;
}

static void prueth_init_mmap_sram_cfg(struct prueth *prueth)
{
	struct prueth_mmap_sram_cfg *s = &prueth->mmap_sram_cfg;
	struct prueth_mmap_sram_emac *emac;
	int p, q;
	u16 loc;

	/* SRAM common for both EMAC and SWITCH */
	for (p = PRUETH_PORT_HOST; p <= PRUETH_PORT_MII1; p++) {
		for (q = PRUETH_QUEUE1; q <= PRUETH_QUEUE4; q++)
			s->bd_offset[p][q] = port_queue_bd_offset(prueth, p, q);
	}

	/* A MARKER in SRAM */
	s->eof_48k_buffer_bd =
		s->bd_offset[PRUETH_PORT_MII1][PRUETH_QUEUE4] +
		port_queue_size(prueth, PRUETH_PORT_MII1, PRUETH_QUEUE4) *
		BD_SIZE;

	if (PRUETH_HAS_SWITCH(prueth)) {
		/* SRAM SWITCH specific */
		for (p = PRUETH_PORT_HOST; p <= PRUETH_PORT_MII1; p++) {
			s->mmap_sram_sw.col_bd_offset[p] =
				prueth->mmap_port_cfg_basis[p].col_bd_offset;
		}
		return;
	}

	/* SRAM EMAC specific */
	emac = &s->mmap_sram_emac;

	loc = s->eof_48k_buffer_bd;
	emac->icss_emac_firmware_release_1_offset = loc;

	loc += 4;
	emac->icss_emac_firmware_release_2_offset = loc;

	loc += 4;
	emac->host_q1_rx_context_offset = loc;
	loc += 8;
	emac->host_q2_rx_context_offset = loc;
	loc += 8;
	emac->host_q3_rx_context_offset = loc;
	loc += 8;
	emac->host_q4_rx_context_offset = loc;

	loc += 8;
	emac->host_queue_descriptor_offset_addr = loc;
	loc += 8;
	emac->host_queue_offset_addr = loc;
	loc += 8;
	emac->host_queue_size_addr = loc;
	loc += 16;
	emac->host_queue_desc_offset = loc;
}

static void prueth_init_mmap_ocmc_cfg(struct prueth *prueth)
{
	struct prueth_mmap_ocmc_cfg *oc = &prueth->mmap_ocmc_cfg;
	int p, q;

	for (p = PRUETH_PORT_HOST; p <= PRUETH_PORT_MII1; p++) {
		for (q = PRUETH_QUEUE1; q <= PRUETH_QUEUE4; q++) {
			oc->buffer_offset[p][q] =
				port_queue_buffer_offset(prueth, p, q);
		}
	}
}

static int prueth_of_get_queue_sizes(struct prueth *prueth,
				     struct device_node *np,
				     u16 port)
{
	struct prueth_mmap_port_cfg_basis *pb;
	u16 sw_rxq_size_defaults[NUM_QUEUES + 1]   = {254, 134, 134, 254, 48};
	u16 emac_rxq_size_defaults[NUM_QUEUES + 1] = {194, 194, 194, 194, 48};
	u16 txq_size_defaults[NUM_QUEUES + 1]      = { 97,  97,  97,  97, 48};
	u16 *queue_sizes;
	int num_queues, i;
	char *propname;

	if (port == PRUETH_PORT_HOST) {
		propname = "rx-queue-size";
		if (PRUETH_HAS_SWITCH(prueth)) {
			num_queues = NUM_QUEUES + 1;
			queue_sizes = sw_rxq_size_defaults;
		} else {
			num_queues = NUM_QUEUES;
			queue_sizes = emac_rxq_size_defaults;
		}
	} else if (port <= PRUETH_PORT_MII1) {
		propname = "tx-queue-size";
		queue_sizes = txq_size_defaults;
		if (PRUETH_HAS_SWITCH(prueth))
			num_queues = NUM_QUEUES + 1;
		else
			num_queues = NUM_QUEUES;
	} else {
		return -EINVAL;
	}

	/* Even the read fails, default values will be retained.
	 * Hence don't check return value and continue to move
	 * queue sizes (default or new) to port_cfg_basis
	 */
	of_property_read_u16_array(np, propname, queue_sizes, num_queues);

	pb = &prueth->mmap_port_cfg_basis[port];
	for (i = PRUETH_QUEUE1; i <= PRUETH_QUEUE4; i++)
		pb->queue_size[i] = queue_sizes[i];

	if (PRUETH_HAS_SWITCH(prueth))
		pb->col_queue_size = queue_sizes[i];

	return 0;
}

static void prueth_of_get_pcp_rxq_map(struct prueth *prueth,
				      struct device_node *np)
{
	struct prueth_mmap_port_cfg_basis *pb;
	int q, j, next_pcp, ret;
	u8 rxq_mask = 0;

	ret = of_property_read_u8_array(np, "pcp-rxq-map",
					prueth->pcp_rxq_map, NUM_VLAN_PCP);
	if (ret) {
		/* Construct the default map. If all q sizes are non-zero,
		 * the default pcp-rxq map will be, with pcp0 lo-to-hi
		 * (left-to-right), <q4 q4 q3 q3 q2 q2 q1 q1>. If only
		 * q2 is 0 for example, then the default map would be
		 * <q4 q4 q4 q4 q3 q3 q1 q1>
		 */
		pb = &prueth->mmap_port_cfg_basis[PRUETH_PORT_HOST];
		/* Start from the highest priority pcp 7 */
		next_pcp = NUM_VLAN_PCP - 1;
		for (q = PRUETH_QUEUE1; q <= PRUETH_QUEUE4; q++) {
			/* Don't map any pcp to q if its size is not
			 * even enough for min frame size, ie the
			 * q cannot receive any frame.
			 */
			if (pb->queue_size[q] < 2)
				continue;

			/* Map next_pcp and all lower pcp's to q */
			for (j = next_pcp; j >= 0; j--)
				prueth->pcp_rxq_map[j] = q;

			/* Prepare next pcp to map, ie. 2 lower than current
			 * Thus if there is an eligible queue to map to, all
			 * pcp's that are at least 2 lower than current one
			 * will be mapped to that queue.
			 */
			next_pcp -= 2;
		}
	}

	for (j = 0; j < NUM_VLAN_PCP; j++) {
		if (prueth->pcp_rxq_map[j] > PRUETH_QUEUE4)
			prueth->pcp_rxq_map[j] = PRUETH_QUEUE4;

		rxq_mask |= BIT(prueth->pcp_rxq_map[j]);
	}

	/* make sure the default lowest priority queue
	 * is included
	 */
	rxq_mask |= BIT(PRUETH_QUEUE4);

	/* Update the rx queue ids array */
	j = 0;
	for (q = PRUETH_QUEUE1; q <= PRUETH_QUEUE4; q++) {
		if (rxq_mask & BIT(q)) {
			sw_port_rx_priority_queue_ids[j] = q;
			j++;
		}
	}
	sw_num_rx_queues = j;
}

static int prueth_init_mmap_configs(struct prueth *prueth)
{
	if (PRUETH_HAS_SWITCH(prueth)) {
		prueth_sw_mmap_port_cfg_basis_fixup(prueth);
		prueth_sw_init_mmap_port_cfg(prueth);
	} else {
		prueth_emac_mmap_port_cfg_basis_fixup(prueth);
		prueth_emac_init_mmap_port_cfg(prueth);
	}

	prueth_init_mmap_sram_cfg(prueth);
	prueth_init_mmap_ocmc_cfg(prueth);
	return 0;
}

static const struct of_device_id prueth_dt_match[];

static int prueth_probe(struct platform_device *pdev)
{
	struct prueth *prueth;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *eth_node;
	const struct of_device_id *match;
	struct pruss *pruss;
	int pruss_id1, pruss_id2, ethtype1, ethtype2, hsr_mode1, hsr_mode2;
	int i, ret;

	if (!np)
		return -ENODEV;	/* we don't support non DT */

	match = of_match_device(prueth_dt_match, dev);
	if (!match)
		return -ENODEV;

	prueth = devm_kzalloc(dev, sizeof(*prueth), GFP_KERNEL);
	if (!prueth)
		return -ENOMEM;

	platform_set_drvdata(pdev, prueth);

	prueth->dev = dev;
	prueth->fw_data = match->data;

	pruss = pruss_get(dev, &prueth->pruss_id);
	if (IS_ERR(pruss)) {
		ret = PTR_ERR(pruss);
		if (ret == -EPROBE_DEFER)
			dev_dbg(dev, "pruss not yet available, deferring probe.\n");
		else
			dev_err(dev, "unable to get pruss handle\n");
		return ret;
	}
	prueth->pruss = pruss;

	prueth->pru0 = pruss_rproc_get(pruss, PRUSS_PRU0);
	if (IS_ERR(prueth->pru0)) {
		ret = PTR_ERR(prueth->pru0);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "unable to get PRU0: %d\n", ret);
		goto pruss_put;
	}

	prueth->pru1 = pruss_rproc_get(pruss, PRUSS_PRU1);
	if (IS_ERR(prueth->pru1)) {
		ret = PTR_ERR(prueth->pru1);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "unable to get PRU1: %d\n", ret);
		goto put_pru0;
	}

	/* Configure PRUSS */
	pruss_cfg_gpimode(pruss, prueth->pru0, PRUSS_GPI_MODE_MII);
	pruss_cfg_gpimode(pruss, prueth->pru1, PRUSS_GPI_MODE_MII);
	pruss_cfg_miirt_enable(pruss, true);
	pruss_cfg_xfr_enable(pruss, true);

	/* Get PRUSS mem resources */
	/* OCMC is system resource which we get separately */
	for (i = 0; i < ARRAY_SIZE(pruss_mem_ids); i++) {
		ret = pruss_request_mem_region(pruss, pruss_mem_ids[i],
					       &prueth->mem[i]);
		if (ret) {
			dev_err(dev, "unable to get PRUSS resource %d: %d\n",
				i, ret);
			goto put_mem;
		}
	}

	/* Set up the proper params to be used for checking */
	if (prueth->fw_data->driver_data == PRUSS_AM57XX) {
		pruss_id1 = PRUSS1;
		pruss_id2 = PRUSS2;
		ethtype1 = pruss1_ethtype;
		ethtype2 = pruss2_ethtype;
		hsr_mode1 = pruss1_hsr_mode;
		hsr_mode2 = pruss2_hsr_mode;
	} else {
		pruss_id1 = PRUSS0;
		pruss_id2 = PRUSS1;
		ethtype1 = pruss0_ethtype;
		ethtype2 = pruss1_ethtype;
		hsr_mode1 = pruss0_hsr_mode;
		hsr_mode2 = pruss1_hsr_mode;
	}

	if (prueth->pruss_id == pruss_id1) {
		prueth->eth_type = ethtype1;
		if (PRUETH_HAS_HSR(prueth))
			prueth->hsr_mode = hsr_mode1;
	} else {
		prueth->eth_type = ethtype2;
		if (PRUETH_HAS_HSR(prueth))
			prueth->hsr_mode = hsr_mode2;
	}

	/* Once the ethtype is known, init mmap cfg structs.
	 * But need to get the queue sizes first. The queue
	 * sizes are fundamental to the remaining configuration
	 * calculations.
	 */
	prueth_of_get_queue_sizes(prueth, np, PRUETH_PORT_HOST);

	eth_node = of_get_child_by_name(np, "ethernet-mii0");
	if (eth_node)
		prueth_of_get_queue_sizes(prueth, eth_node, PRUETH_PORT_MII0);

	eth_node = of_get_child_by_name(np, "ethernet-mii1");
	if (eth_node)
		prueth_of_get_queue_sizes(prueth, eth_node, PRUETH_PORT_MII1);

	if (PRUETH_HAS_RED(prueth))
		prueth_of_get_pcp_rxq_map(prueth, np);

	prueth_init_mmap_configs(prueth);

	if (PRUETH_HAS_SWITCH(prueth))
		prueth->ocmc_ram_size = OCMC_RAM_SIZE;
	else
		prueth->ocmc_ram_size = OCMC_RAM_SIZE_SWITCH;

	/* OCMC_RAM1 */
	prueth->sram_pool = of_gen_pool_get(np, "sram", 0);
	if (!prueth->sram_pool) {
		dev_err(dev, "unable to get SRAM pool\n");
		ret = -ENODEV;

		goto put_mem;
	}
	prueth->mem[PRUETH_MEM_OCMC].va =
			(void __iomem *)gen_pool_alloc(prueth->sram_pool,
						       prueth->ocmc_ram_size);
	if (IS_ERR(prueth->mem[PRUETH_MEM_OCMC].va)) {
		ret = PTR_ERR(prueth->mem[PRUETH_MEM_OCMC].va);
		dev_err(dev, "unable to allocate OCMC resource\n");
		goto put_mem;
	}
	prueth->mem[PRUETH_MEM_OCMC].pa =
			gen_pool_virt_to_phys(prueth->sram_pool,
			(unsigned long)prueth->mem[PRUETH_MEM_OCMC].va);
	prueth->mem[PRUETH_MEM_OCMC].size = prueth->ocmc_ram_size;
	dev_dbg(dev, "ocmc: pa %pa va %p size %#x\n",
		&prueth->mem[PRUETH_MEM_OCMC].pa,
		prueth->mem[PRUETH_MEM_OCMC].va,
		prueth->mem[PRUETH_MEM_OCMC].size);

	/* setup netdev interfaces */
	eth_node = of_get_child_by_name(np, "ethernet-mii0");
	if (!eth_node) {
		dev_err(dev, "no ethernet-mii0 node\n");
		ret = -ENODEV;
		goto free_pool;
	}
	ret = prueth_netdev_init(prueth, eth_node);
	if (ret) {
		if (ret != -EPROBE_DEFER) {
			dev_err(dev, "netdev init %s failed: %d\n",
				eth_node->name, ret);
		}
		goto netdev_exit;
	} else {
		prueth->eth_node[PRUETH_PORT_MII0] = eth_node;
	}

	eth_node = of_get_child_by_name(np, "ethernet-mii1");
	if (!eth_node) {
		dev_err(dev, "no ethernet-mii1 node\n");
		ret = -ENODEV;
		goto netdev_exit;
	}
	ret = prueth_netdev_init(prueth, eth_node);
	if (ret) {
		if (ret != -EPROBE_DEFER) {
			dev_err(dev, "netdev init %s failed: %d\n",
				eth_node->name, ret);
		}
		goto netdev_exit;
	} else {
		prueth->eth_node[PRUETH_PORT_MII1] = eth_node;
	}

	ret = prueth_hostinit(prueth);
	if (ret) {
		dev_info(dev, "hostinit failed: %d\n", ret);
		goto netdev_exit;
	}

	/* register the network devices */
	for (i = 0; i < PRUETH_PORT_MAX; i++) {
		enum prueth_port port;

		eth_node = prueth->eth_node[i];
		if (!eth_node)
			continue;

		port = prueth_node_port(eth_node);
		if (port != PRUETH_PORT_MII0 && port != PRUETH_PORT_MII1)
			continue;

		ret = register_netdev(prueth->emac[port]->ndev);
		if (ret) {
			dev_err(dev, "can't register netdev for port %d\n",
				port);
			goto netdev_unregister;
		}

		prueth->registered_netdevs[i] = prueth->emac[port]->ndev;
	}

	if (PRUETH_HAS_RED(prueth)) {
		init_timer(&prueth->tbl_check_timer);
		ret = prueth_hsr_prp_debugfs_init(prueth);
		if (ret)
			goto netdev_unregister;
	}

	dev_info(dev, "TI PRU ethernet (type %u, rxqSz: %u %u %u %u %u) driver initialized\n",
		 prueth->eth_type,
		 prueth->mmap_port_cfg_basis[PRUETH_PORT_HOST].queue_size[0],
		 prueth->mmap_port_cfg_basis[PRUETH_PORT_HOST].queue_size[1],
		 prueth->mmap_port_cfg_basis[PRUETH_PORT_HOST].queue_size[2],
		 prueth->mmap_port_cfg_basis[PRUETH_PORT_HOST].queue_size[3],
		 prueth->mmap_port_cfg_basis[PRUETH_PORT_HOST].col_queue_size);

	if (PRUETH_HAS_RED(prueth))
		dev_info(dev, "pcp-rxq-map (lo2hi->): %u %u %u %u %u %u %u %u\n",
			 prueth->pcp_rxq_map[0], prueth->pcp_rxq_map[1],
			 prueth->pcp_rxq_map[2], prueth->pcp_rxq_map[3],
			 prueth->pcp_rxq_map[4], prueth->pcp_rxq_map[5],
			 prueth->pcp_rxq_map[6], prueth->pcp_rxq_map[7]);

	return 0;

netdev_unregister:
	for (i = 0; i < PRUETH_PORT_MAX; i++) {
		if (!prueth->registered_netdevs[i])
			continue;
		unregister_netdev(prueth->registered_netdevs[i]);
	}

netdev_exit:
	for (i = 0; i < PRUETH_PORT_MAX; i++) {
		eth_node = prueth->eth_node[i];
		if (!eth_node)
			continue;

		prueth_netdev_exit(prueth, eth_node);
		of_node_put(eth_node);
	}

free_pool:
	gen_pool_free(prueth->sram_pool,
		      (unsigned long)prueth->mem[PRUETH_MEM_OCMC].va,
		      prueth->ocmc_ram_size);

put_mem:
	for (i = PRUETH_MEM_DRAM0; i < PRUETH_MEM_OCMC; i++) {
		if (prueth->mem[i].va)
			pruss_release_mem_region(pruss, &prueth->mem[i]);
	}

	pruss_rproc_put(pruss, prueth->pru1);
put_pru0:
	pruss_rproc_put(pruss, prueth->pru0);
pruss_put:
	pruss_put(prueth->pruss);

	return ret;
}

static int prueth_remove(struct platform_device *pdev)
{
	struct device_node *eth_node;
	struct prueth *prueth = platform_get_drvdata(pdev);
	int i;

	prueth_hsr_prp_debugfs_term(prueth);
	del_timer_sync(&prueth->tbl_check_timer);
	prueth->tbl_check_period = 0;

	for (i = 0; i < PRUETH_PORT_MAX; i++) {
		if (!prueth->registered_netdevs[i])
			continue;
		unregister_netdev(prueth->registered_netdevs[i]);
	}

	for (i = 0; i < PRUETH_PORT_MAX; i++) {
		eth_node = prueth->eth_node[i];
		if (!eth_node)
			continue;

		prueth_netdev_exit(prueth, eth_node);
		of_node_put(eth_node);
	}

	gen_pool_free(prueth->sram_pool,
		      (unsigned long)prueth->mem[PRUETH_MEM_OCMC].va,
		      prueth->ocmc_ram_size);

	for (i = PRUETH_MEM_DRAM0; i < PRUETH_MEM_OCMC; i++) {
		if (prueth->mem[i].va)
			pruss_release_mem_region(prueth->pruss, &prueth->mem[i]);
	}

	pruss_rproc_put(prueth->pruss, prueth->pru1);
	pruss_rproc_put(prueth->pruss, prueth->pru0);
	pruss_put(prueth->pruss);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int prueth_suspend(struct device *dev)
{
	struct prueth *prueth = dev_get_drvdata(dev);
	struct net_device *ndev;
	int i, ret;

	for (i = 0; i < PRUETH_PORT_MAX; i++) {
		ndev = prueth->registered_netdevs[i];

		if (!ndev)
			continue;

		if (netif_running(ndev)) {
			netif_device_detach(ndev);
			ret = emac_ndo_stop(ndev);
			if (ret < 0) {
				netdev_err(ndev, "failed to stop: %d", ret);
				return ret;
			}
		}
	}

	return 0;
}

static int prueth_resume(struct device *dev)
{
	struct prueth *prueth = dev_get_drvdata(dev);
	struct net_device *ndev;
	int i, ret;

	for (i = 0; i < PRUETH_PORT_MAX; i++) {
		ndev = prueth->registered_netdevs[i];

		if (!ndev)
			continue;

		if (netif_running(ndev)) {
			ret = emac_ndo_open(ndev);
			if (ret < 0) {
				netdev_err(ndev, "failed to start: %d", ret);
				return ret;
			}
			netif_device_attach(ndev);
		}
	}

	return 0;
}

#endif /* CONFIG_PM_SLEEP */

static const struct dev_pm_ops prueth_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(prueth_suspend, prueth_resume)
};

/* AM33xx SoC-specific firmware data */
static struct prueth_private_data am335x_prueth_pdata = {
	.driver_data = PRUSS_AM3359,
	.fw_pru[PRUSS_PRU0] = {
		.fw_name[PRUSS_ETHTYPE_EMAC] =
			"ti-pruss/am335x-pru0-prueth-fw.elf"
	},
	.fw_pru[PRUSS_PRU1] = {
		.fw_name[PRUSS_ETHTYPE_EMAC] =
			"ti-pruss/am335x-pru1-prueth-fw.elf"
	}
};

/* AM437x SoC-specific firmware data */
static struct prueth_private_data am437x_prueth_pdata = {
	.driver_data = PRUSS_AM4376,
	.fw_pru[PRUSS_PRU0] = {
		.fw_name[PRUSS_ETHTYPE_EMAC] =
			"ti-pruss/am437x-pru0-prueth-fw.elf"
	},
	.fw_pru[PRUSS_PRU1] = {
		.fw_name[PRUSS_ETHTYPE_EMAC] =
			"ti-pruss/am437x-pru1-prueth-fw.elf"
	}
};

/* AM57xx SoC-specific firmware data */
static struct prueth_private_data am57xx_prueth_pdata = {
	.driver_data = PRUSS_AM57XX,
	.fw_pru[PRUSS_PRU0] = {
		.fw_name[PRUSS_ETHTYPE_EMAC] =
			"ti-pruss/am57xx-pru0-prueth-fw.elf",
		.fw_name[PRUSS_ETHTYPE_HSR] =
			"ti-pruss/am57xx-pru0-pruhsr-fw.elf",
		.fw_name[PRUSS_ETHTYPE_PRP] =
			"ti-pruss/am57xx-pru0-pruprp-fw.elf",
		.fw_name[PRUSS_ETHTYPE_HSRPTP] =
			"ti-pruss/am57xx-pru0-pruhsrptp-fw.elf",
		.fw_name[PRUSS_ETHTYPE_PRPPTP] =
			"ti-pruss/am57xx-pru0-pruprpptp-fw.elf"
	},
	.fw_pru[PRUSS_PRU1] = {
		.fw_name[PRUSS_ETHTYPE_EMAC] =
			"ti-pruss/am57xx-pru1-prueth-fw.elf",
		.fw_name[PRUSS_ETHTYPE_HSR] =
			"ti-pruss/am57xx-pru1-pruhsr-fw.elf",
		.fw_name[PRUSS_ETHTYPE_PRP] =
			"ti-pruss/am57xx-pru1-pruprp-fw.elf",
		.fw_name[PRUSS_ETHTYPE_HSRPTP] =
			"ti-pruss/am57xx-pru1-pruhsrptp-fw.elf",
		.fw_name[PRUSS_ETHTYPE_PRPPTP] =
			"ti-pruss/am57xx-pru1-pruprpptp-fw.elf"
	}
};

/* 66AK2G SoC-specific firmware data */
static struct prueth_private_data k2g_prueth_pdata = {
	.driver_data = PRUSS_K2G,
	.fw_pru[PRUSS_PRU0] = {
		.fw_name[PRUSS_ETHTYPE_EMAC] =
			"ti-pruss/k2g-pru0-prueth-fw.elf"
	},
	.fw_pru[PRUSS_PRU1] = {
		.fw_name[PRUSS_ETHTYPE_EMAC] =
			"ti-pruss/k2g-pru1-prueth-fw.elf"
	}
};

static const struct of_device_id prueth_dt_match[] = {
	{ .compatible = "ti,am57-prueth", .data = &am57xx_prueth_pdata, },
	{ .compatible = "ti,am4376-prueth", .data = &am437x_prueth_pdata, },
	{ .compatible = "ti,am3359-prueth", .data = &am335x_prueth_pdata, },
	{ .compatible = "ti,k2g-prueth", .data = &k2g_prueth_pdata, },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, prueth_dt_match);

static struct platform_driver prueth_driver = {
	.probe = prueth_probe,
	.remove = prueth_remove,
	.driver = {
		.name = "prueth",
		.of_match_table = prueth_dt_match,
		.pm = &prueth_dev_pm_ops,
	},
};
module_platform_driver(prueth_driver);

MODULE_AUTHOR("Roger Quadros <rogerq@ti.com>");
MODULE_AUTHOR("Andrew F. Davis <afd@ti.com>");
MODULE_DESCRIPTION("PRU Ethernet Driver");
MODULE_LICENSE("GPL v2");

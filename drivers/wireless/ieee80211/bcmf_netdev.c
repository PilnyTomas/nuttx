/****************************************************************************
 * drivers/wireless/bcmf_netdev.c
 *
 *   Copyright (C) 2017 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#if defined(CONFIG_NET) && defined(CONFIG_IEEE80211_BROADCOM_FULLMAC)

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <debug.h>

#include <arpa/inet.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>
#include <nuttx/net/arp.h>
#include <nuttx/net/netdev.h>

#ifdef CONFIG_NET_PKT
#  include <nuttx/net/pkt.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* If processing is not done at the interrupt level, then work queue support
 * is required.
 */

#if !defined(CONFIG_SCHED_WORKQUEUE)
#  error Work queue support is required in this configuration (CONFIG_SCHED_WORKQUEUE)
#else

   /* Use the selected work queue */

#  if defined(CONFIG_IEEE80211_BROADCOM_HPWORK)
#    define BCMFWORK HPWORK
#  elif defined(CONFIG_IEEE80211_BROADCOM_HLWORK)
#    define BCMFWORK LPWORK
#  else
#    error Neither CONFIG_IEEE80211_BROADCOM_HPWORK nor CONFIG_IEEE80211_BROADCOM_HLWORK defined
#  endif
#endif

/* CONFIG_IEEE80211_BROADCOM_NINTERFACES determines the number of physical interfaces
 * that will be supported.
 */

#ifndef CONFIG_IEEE80211_BROADCOM_NINTERFACES
# define CONFIG_IEEE80211_BROADCOM_NINTERFACES 1
#endif

/* TX poll delay = 1 seconds. CLK_TCK is the number of clock ticks per second */

#define BCMF_WDDELAY   (1*CLK_TCK)

/* TX timeout = 1 minute */

#define BCMF_TXTIMEOUT (60*CLK_TCK)

/* This is a helper pointer for accessing the contents of the Ethernet header */

#define BUF ((struct eth_hdr_s *)priv->bc_dev.d_buf)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The bcmf_driver_s encapsulates all state information for a single hardware
 * interface
 */

struct bcmf_driver_s
{
  bool bc_bifup;               /* true:ifup false:ifdown */
  WDOG_ID bc_txpoll;           /* TX poll timer */
  WDOG_ID bc_txtimeout;        /* TX timeout timer */
  struct work_s bc_irqwork;    /* For deferring interupt work to the work queue */
  struct work_s bc_pollwork;   /* For deferring poll work to the work queue */

  /* This holds the information visible to the NuttX network */

  struct net_driver_s bc_dev;  /* Interface understood by the network */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* These statically allocated structur would mean that only a single
 * instance of the device could be supported.  In order to support multiple
 * devices instances, this data would have to be allocated dynamically.
 */

/* A single packet buffer per device is used here.  There might be multiple
 * packet buffers in a more complex, pipelined design.
 *
 * NOTE that if CONFIG_IEEE80211_BROADCOM_NINTERFACES were greater than 1, you would
 * need a minimum on one packetbuffer per instance.  Much better to be
 * allocated dynamically.
 */

static uint8_t g_pktbuf[MAX_NET_DEV_MTU + CONFIG_NET_GUARDSIZE];

/* Driver state structure */

static struct bcmf_driver_s g_bcmf_interface[CONFIG_IEEE80211_BROADCOM_NINTERFACES];

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Common TX logic */

static int  bcmf_transmit(FAR struct bcmf_driver_s *priv);
static int  bcmf_txpoll(FAR struct net_driver_s *dev);

/* Interrupt handling */

static void bcmf_receive(FAR struct bcmf_driver_s *priv);
static void bcmf_txdone(FAR struct bcmf_driver_s *priv);

static void bcmf_interrupt_work(FAR void *arg);
static int  bcmf_interrupt(int irq, FAR void *context, FAR void *arg);

/* Watchdog timer expirations */

static void bcmf_txtimeout_work(FAR void *arg);
static void bcmf_txtimeout_expiry(int argc, wdparm_t arg, ...);

static void bcmf_poll_work(FAR void *arg);
static void bcmf_poll_expiry(int argc, wdparm_t arg, ...);

/* NuttX callback functions */

static int  bcmf_ifup(FAR struct net_driver_s *dev);
static int  bcmf_ifdown(FAR struct net_driver_s *dev);

static void bcmf_txavail_work(FAR void *arg);
static int  bcmf_txavail(FAR struct net_driver_s *dev);

#if defined(CONFIG_NET_IGMP) || defined(CONFIG_NET_ICMPv6)
static int  bcmf_addmac(FAR struct net_driver_s *dev,
              FAR const uint8_t *mac);
#ifdef CONFIG_NET_IGMP
static int  bcmf_rmmac(FAR struct net_driver_s *dev,
              FAR const uint8_t *mac);
#endif
#ifdef CONFIG_NET_ICMPv6
static void bcmf_ipv6multicast(FAR struct bcmf_driver_s *priv);
#endif
#endif
#ifdef CONFIG_NETDEV_IOCTL
static int  bcmf_ioctl(FAR struct net_driver_s *dev, int cmd,
            unsigned long arg);
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bcmf_transmit
 *
 * Description:
 *   Start hardware transmission.  Called either from the txdone interrupt
 *   handling or from watchdog based polling.
 *
 * Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   OK on success; a negated errno on failure
 *
 * Assumptions:
 *   May or may not be called from an interrupt handler.  In either case,
 *   the network is locked.
 *
 ****************************************************************************/

static int bcmf_transmit(FAR struct bcmf_driver_s *priv)
{
  /* Verify that the hardware is ready to send another packet.  If we get
   * here, then we are committed to sending a packet; Higher level logic
   * must have assured that there is no transmission in progress.
   */

  /* Increment statistics */

  NETDEV_TXPACKETS(priv->bc_dev);

  /* Send the packet: address=priv->bc_dev.d_buf, length=priv->bc_dev.d_len */

  /* Enable Tx interrupts */

  /* Setup the TX timeout watchdog (perhaps restarting the timer) */

  (void)wd_start(priv->bc_txtimeout, BCMF_TXTIMEOUT,
                 bcmf_txtimeout_expiry, 1, (wdparm_t)priv);
  return OK;
}

/****************************************************************************
 * Name: bcmf_txpoll
 *
 * Description:
 *   The transmitter is available, check if the network has any outgoing
 *   packets ready to send.  This is a callback from devif_poll().
 *   devif_poll() may be called:
 *
 *   1. When the preceding TX packet send is complete,
 *   2. When the preceding TX packet send timesout and the interface is reset
 *   3. During normal TX polling
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   OK on success; a negated errno on failure
 *
 * Assumptions:
 *   May or may not be called from an interrupt handler.  In either case,
 *   the network is locked.
 *
 ****************************************************************************/

static int bcmf_txpoll(FAR struct net_driver_s *dev)
{
  FAR struct bcmf_driver_s *priv = (FAR struct bcmf_driver_s *)dev->d_private;

  /* If the polling resulted in data that should be sent out on the network,
   * the field d_len is set to a value > 0.
   */

  if (priv->bc_dev.d_len > 0)
    {
      /* Look up the destination MAC address and add it to the Ethernet
       * header.
       */

#ifdef CONFIG_NET_IPv4
#ifdef CONFIG_NET_IPv6
      if (IFF_IS_IPv4(priv->bc_dev.d_flags))
#endif
        {
          arp_out(&priv->bc_dev);
        }
#endif /* CONFIG_NET_IPv4 */

#ifdef CONFIG_NET_IPv6
#ifdef CONFIG_NET_IPv4
      else
#endif
        {
          neighbor_out(&priv->bc_dev);
        }
#endif /* CONFIG_NET_IPv6 */

      /* Send the packet */

      bcmf_transmit(priv);

      /* Check if there is room in the device to hold another packet. If not,
       * return a non-zero value to terminate the poll.
       */
    }

  /* If zero is returned, the polling will continue until all connections have
   * been examined.
   */

  return 0;
}

/****************************************************************************
 * Name: bcmf_receive
 *
 * Description:
 *   An interrupt was received indicating the availability of a new RX packet
 *
 * Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static void bcmf_receive(FAR struct bcmf_driver_s *priv)
{
  do
    {
      /* Check for errors and update statistics */

      /* Check if the packet is a valid size for the network buffer
       * configuration.
       */

      /* Copy the data data from the hardware to priv->bc_dev.d_buf.  Set
       * amount of data in priv->bc_dev.d_len
       */

#ifdef CONFIG_NET_PKT
      /* When packet sockets are enabled, feed the frame into the packet tap */

       pkt_input(&priv->bc_dev);
#endif

      /* We only accept IP packets of the configured type and ARP packets */

#ifdef CONFIG_NET_IPv4
      if (BUF->type == HTONS(ETHTYPE_IP))
        {
          ninfo("IPv4 frame\n");
          NETDEV_RXIPV4(&priv->bc_dev);

          /* Handle ARP on input then give the IPv4 packet to the network
           * layer
           */

          arp_ipin(&priv->bc_dev);
          ipv4_input(&priv->bc_dev);

          /* If the above function invocation resulted in data that should be
           * sent out on the network, the field  d_len will set to a value > 0.
           */

          if (priv->bc_dev.d_len > 0)
            {
              /* Update the Ethernet header with the correct MAC address */

#ifdef CONFIG_NET_IPv6
              if (IFF_IS_IPv4(priv->bc_dev.d_flags))
#endif
                {
                  arp_out(&priv->bc_dev);
                }
#ifdef CONFIG_NET_IPv6
              else
                {
                  neighbor_out(&kel->bc_dev);
                }
#endif

              /* And send the packet */

              bcmf_transmit(priv);
            }
        }
      else
#endif
#ifdef CONFIG_NET_IPv6
      if (BUF->type == HTONS(ETHTYPE_IP6))
        {
          ninfo("Iv6 frame\n");
          NETDEV_RXIPV6(&priv->bc_dev);

          /* Give the IPv6 packet to the network layer */

          ipv6_input(&priv->bc_dev);

          /* If the above function invocation resulted in data that should be
           * sent out on the network, the field  d_len will set to a value > 0.
           */

          if (priv->bc_dev.d_len > 0)
           {
              /* Update the Ethernet header with the correct MAC address */

#ifdef CONFIG_NET_IPv4
              if (IFF_IS_IPv4(priv->bc_dev.d_flags))
                {
                  arp_out(&priv->bc_dev);
                }
              else
#endif
#ifdef CONFIG_NET_IPv6
                {
                  neighbor_out(&priv->bc_dev);
                }
#endif

              /* And send the packet */

              bcmf_transmit(priv);
            }
        }
      else
#endif
#ifdef CONFIG_NET_ARP
      if (BUF->type == htons(ETHTYPE_ARP))
        {
          arp_arpin(&priv->bc_dev);
          NETDEV_RXARP(&priv->bc_dev);

          /* If the above function invocation resulted in data that should be
           * sent out on the network, the field  d_len will set to a value > 0.
           */

          if (priv->bc_dev.d_len > 0)
            {
              bcmf_transmit(priv);
            }
        }
      else
#endif
        {
          NETDEV_RXDROPPED(&priv->bc_dev);
        }
    }
  while (); /* While there are more packets to be processed */
}

/****************************************************************************
 * Name: bcmf_txdone
 *
 * Description:
 *   An interrupt was received indicating that the last TX packet(s) is done
 *
 * Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static void bcmf_txdone(FAR struct bcmf_driver_s *priv)
{
  int delay;

  /* Check for errors and update statistics */

  NETDEV_TXDONE(priv->bc_dev);

  /* Check if there are pending transmissions */

  /* If no further transmissions are pending, then cancel the TX timeout and
   * disable further Tx interrupts.
   */

  wd_cancel(priv->bc_txtimeout);

  /* And disable further TX interrupts. */

  /* In any event, poll the network for new TX data */

  (void)devif_poll(&priv->bc_dev, bcmf_txpoll);
}

/****************************************************************************
 * Name: bcmf_interrupt_work
 *
 * Description:
 *   Perform interrupt related work from the worker thread
 *
 * Parameters:
 *   arg - The argument passed when work_queue() was called.
 *
 * Returned Value:
 *   OK on success
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static void bcmf_interrupt_work(FAR void *arg)
{
  FAR struct bcmf_driver_s *priv = (FAR struct bcmf_driver_s *)arg;

  /* Lock the network and serialize driver operations if necessary.
   * NOTE: Serialization is only required in the case where the driver work
   * is performed on an LP worker thread and where more than one LP worker
   * thread has been configured.
   */

  net_lock();

  /* Process pending Ethernet interrupts */

  /* Get and clear interrupt status bits */

  /* Handle interrupts according to status bit settings */

  /* Check if we received an incoming packet, if so, call bcmf_receive() */

  bcmf_receive(priv);

  /* Check if a packet transmission just completed.  If so, call bcmf_txdone.
   * This may disable further Tx interrupts if there are no pending
   * transmissions.
   */

  bcmf_txdone(priv);
  net_unlock();

  /* Re-enable Ethernet interrupts */
#warning Missing logic
}

/****************************************************************************
 * Name: bcmf_interrupt
 *
 * Description:
 *   Hardware interrupt handler
 *
 * Parameters:
 *   irq     - Number of the IRQ that generated the interrupt
 *   context - Interrupt register state save info (architecture-specific)
 *
 * Returned Value:
 *   OK on success
 *
 * Assumptions:
 *
 ****************************************************************************/

static int bcmf_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct bcmf_driver_s *priv = (FAR struct bcmf_driver_s *)arg;

  DEBUGASSERT(priv != NULL);

  /* Disable further Ethernet interrupts.  Because Ethernet interrupts are
   * also disabled if the TX timeout event occurs, there can be no race
   * condition here.
   */
#warning Missing logic

  /* TODO: Determine if a TX transfer just completed */

    {
      /* If a TX transfer just completed, then cancel the TX timeout so
       * there will be no race condition between any subsequent timeout
       * expiration and the deferred interrupt processing.
       */

       wd_cancel(priv->bc_txtimeout);
    }

  /* Schedule to perform the interrupt processing on the worker thread. */

  work_queue(BCMFWORK, &priv->bc_irqwork, bcmf_interrupt_work, priv, 0);
  return OK;
}

/****************************************************************************
 * Name: bcmf_txtimeout_work
 *
 * Description:
 *   Perform TX timeout related work from the worker thread
 *
 * Parameters:
 *   arg - The argument passed when work_queue() as called.
 *
 * Returned Value:
 *   OK on success
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static void bcmf_txtimeout_work(FAR void *arg)
{
  FAR struct bcmf_driver_s *priv = (FAR struct bcmf_driver_s *)arg;

  /* Lock the network and serialize driver operations if necessary.
   * NOTE: Serialization is only required in the case where the driver work
   * is performed on an LP worker thread and where more than one LP worker
   * thread has been configured.
   */

  net_lock();

  /* Increment statistics and dump debug info */

  NETDEV_TXTIMEOUTS(priv->bc_dev);

  /* Then reset the hardware */

  /* Then poll the network for new XMIT data */

  (void)devif_poll(&priv->bc_dev, bcmf_txpoll);
  net_unlock();
}

/****************************************************************************
 * Name: bcmf_txtimeout_expiry
 *
 * Description:
 *   Our TX watchdog timed out.  Called from the timer interrupt handler.
 *   The last TX never completed.  Reset the hardware and start again.
 *
 * Parameters:
 *   argc - The number of available arguments
 *   arg  - The first argument
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Global interrupts are disabled by the watchdog logic.
 *
 ****************************************************************************/

static void bcmf_txtimeout_expiry(int argc, wdparm_t arg, ...)
{
  FAR struct bcmf_driver_s *priv = (FAR struct bcmf_driver_s *)arg;

  /* Disable further Ethernet interrupts.  This will prevent some race
   * conditions with interrupt work.  There is still a potential race
   * condition with interrupt work that is already queued and in progress.
   */
#warning Missing logic

  /* Schedule to perform the TX timeout processing on the worker thread. */

  work_queue(BCMFWORK, &priv->bc_irqwork, bcmf_txtimeout_work, priv, 0);
}

/****************************************************************************
 * Name: bcmf_poll_process
 *
 * Description:
 *   Perform the periodic poll.  This may be called either from watchdog
 *   timer logic or from the worker thread, depending upon the configuration.
 *
 * Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static inline void bcmf_poll_process(FAR struct bcmf_driver_s *priv)
{
}

/****************************************************************************
 * Name: bcmf_poll_work
 *
 * Description:
 *   Perform periodic polling from the worker thread
 *
 * Parameters:
 *   arg - The argument passed when work_queue() as called.
 *
 * Returned Value:
 *   OK on success
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static void bcmf_poll_work(FAR void *arg)
{
  FAR struct bcmf_driver_s *priv = (FAR struct bcmf_driver_s *)arg;

  /* Lock the network and serialize driver operations if necessary.
   * NOTE: Serialization is only required in the case where the driver work
   * is performed on an LP worker thread and where more than one LP worker
   * thread has been configured.
   */

  net_lock();

  /* Perform the poll */

  /* Check if there is room in the send another TX packet.  We cannot perform
   * the TX poll if he are unable to accept another packet for transmission.
   */

  /* If so, update TCP timing states and poll the network for new XMIT data.
   * Hmmm.. might be bug here.  Does this mean if there is a transmit in
   * progress, we will missing TCP time state updates?
   */

  (void)devif_timer(&priv->bc_dev, bcmf_txpoll);

  /* Setup the watchdog poll timer again */

  (void)wd_start(priv->bc_txpoll, BCMF_WDDELAY, bcmf_poll_expiry, 1,
                 (wdparm_t)priv);
  net_unlock();
}

/****************************************************************************
 * Name: bcmf_poll_expiry
 *
 * Description:
 *   Periodic timer handler.  Called from the timer interrupt handler.
 *
 * Parameters:
 *   argc - The number of available arguments
 *   arg  - The first argument
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Global interrupts are disabled by the watchdog logic.
 *
 ****************************************************************************/

static void bcmf_poll_expiry(int argc, wdparm_t arg, ...)
{
  FAR struct bcmf_driver_s *priv = (FAR struct bcmf_driver_s *)arg;

  /* Schedule to perform the interrupt processing on the worker thread. */

  work_queue(BCMFWORK, &priv->bc_pollwork, bcmf_poll_work, priv, 0);
}

/****************************************************************************
 * Name: bcmf_ifup
 *
 * Description:
 *   NuttX Callback: Bring up the Ethernet interface when an IP address is
 *   provided
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static int bcmf_ifup(FAR struct net_driver_s *dev)
{
  FAR struct bcmf_driver_s *priv = (FAR struct bcmf_driver_s *)dev->d_private;

#ifdef CONFIG_NET_IPv4
  ninfo("Bringing up: %d.%d.%d.%d\n",
        dev->d_ipaddr & 0xff, (dev->d_ipaddr >> 8) & 0xff,
        (dev->d_ipaddr >> 16) & 0xff, dev->d_ipaddr >> 24);
#endif
#ifdef CONFIG_NET_IPv6
  ninfo("Bringing up: %04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x\n",
        dev->d_ipv6addr[0], dev->d_ipv6addr[1], dev->d_ipv6addr[2],
        dev->d_ipv6addr[3], dev->d_ipv6addr[4], dev->d_ipv6addr[5],
        dev->d_ipv6addr[6], dev->d_ipv6addr[7]);
#endif

  /* Initialize PHYs, the Ethernet interface, and setup up Ethernet interrupts */

  /* Instantiate the MAC address from priv->bc_dev.d_mac.ether.ether_addr_octet */

#ifdef CONFIG_NET_ICMPv6
  /* Set up IPv6 multicast address filtering */

  bcmf_ipv6multicast(priv);
#endif

  /* Set and activate a timer process */

  (void)wd_start(priv->bc_txpoll, BCMF_WDDELAY, bcmf_poll_expiry, 1,
                 (wdparm_t)priv);

  /* Enable the hardware interrupt */

  priv->bc_bifup = true;
#warning Missing logic
  return OK;
}

/****************************************************************************
 * Name: bcmf_ifdown
 *
 * Description:
 *   NuttX Callback: Stop the interface.
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static int bcmf_ifdown(FAR struct net_driver_s *dev)
{
  FAR struct bcmf_driver_s *priv = (FAR struct bcmf_driver_s *)dev->d_private;
  irqstate_t flags;

  /* Disable the hardware interrupt */

  flags = enter_critical_section();
#warning Missing logic

  /* Cancel the TX poll timer and TX timeout timers */

  wd_cancel(priv->bc_txpoll);
  wd_cancel(priv->bc_txtimeout);

  /* Put the EMAC in its reset, non-operational state.  This should be
   * a known configuration that will guarantee the bcmf_ifup() always
   * successfully brings the interface back up.
   */

  /* Mark the device "down" */

  priv->bc_bifup = false;
  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: bcmf_txavail_work
 *
 * Description:
 *   Perform an out-of-cycle poll on the worker thread.
 *
 * Parameters:
 *   arg - Reference to the NuttX driver state structure (cast to void*)
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Called on the higher priority worker thread.
 *
 ****************************************************************************/

static void bcmf_txavail_work(FAR void *arg)
{
  FAR struct bcmf_driver_s *priv = (FAR struct bcmf_driver_s *)arg;

  /* Lock the network and serialize driver operations if necessary.
   * NOTE: Serialization is only required in the case where the driver work
   * is performed on an LP worker thread and where more than one LP worker
   * thread has been configured.
   */

  net_lock();

  /* Ignore the notification if the interface is not yet up */

  if (priv->bc_bifup)
    {
      /* Check if there is room in the hardware to hold another outgoing packet. */

      /* If so, then poll the network for new XMIT data */

      (void)devif_poll(&priv->bc_dev, bcmf_txpoll);
    }

  net_unlock();
}

/****************************************************************************
 * Name: bcmf_txavail
 *
 * Description:
 *   Driver callback invoked when new TX data is available.  This is a
 *   stimulus perform an out-of-cycle poll and, thereby, reduce the TX
 *   latency.
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Called in normal user mode
 *
 ****************************************************************************/

static int bcmf_txavail(FAR struct net_driver_s *dev)
{
  FAR struct bcmf_driver_s *priv = (FAR struct bcmf_driver_s *)dev->d_private;

  /* Is our single work structure available?  It may not be if there are
   * pending interrupt actions and we will have to ignore the Tx
   * availability action.
   */

  if (work_available(&priv->bc_pollwork))
    {
      /* Schedule to serialize the poll on the worker thread. */

      work_queue(BCMFWORK, &priv->bc_pollwork, bcmf_txavail_work, priv, 0);
    }

  return OK;
}

/****************************************************************************
 * Name: bcmf_addmac
 *
 * Description:
 *   NuttX Callback: Add the specified MAC address to the hardware multicast
 *   address filtering
 *
 * Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *   mac  - The MAC address to be added
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

#if defined(CONFIG_NET_IGMP) || defined(CONFIG_NET_ICMPv6)
static int bcmf_addmac(FAR struct net_driver_s *dev, FAR const uint8_t *mac)
{
  FAR struct bcmf_driver_s *priv = (FAR struct bcmf_driver_s *)dev->d_private;

  /* Add the MAC address to the hardware multicast routing table */

  return OK;
}
#endif

/****************************************************************************
 * Name: bcmf_rmmac
 *
 * Description:
 *   NuttX Callback: Remove the specified MAC address from the hardware multicast
 *   address filtering
 *
 * Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *   mac  - The MAC address to be removed
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

#ifdef CONFIG_NET_IGMP
static int bcmf_rmmac(FAR struct net_driver_s *dev, FAR const uint8_t *mac)
{
  FAR struct bcmf_driver_s *priv = (FAR struct bcmf_driver_s *)dev->d_private;

  /* Add the MAC address to the hardware multicast routing table */

  return OK;
}
#endif

/****************************************************************************
 * Name: bcmf_ipv6multicast
 *
 * Description:
 *   Configure the IPv6 multicast MAC address.
 *
 * Parameters:
 *   priv - A reference to the private driver state structure
 *
 * Returned Value:
 *   OK on success; Negated errno on failure.
 *
 * Assumptions:
 *
 ****************************************************************************/

#ifdef CONFIG_NET_ICMPv6
static void bcmf_ipv6multicast(FAR struct bcmf_driver_s *priv)
{
  FAR struct net_driver_s *dev;
  uint16_t tmp16;
  uint8_t mac[6];

  /* For ICMPv6, we need to add the IPv6 multicast address
   *
   * For IPv6 multicast addresses, the Ethernet MAC is derived by
   * the four low-order octets OR'ed with the MAC 33:33:00:00:00:00,
   * so for example the IPv6 address FF02:DEAD:BEEF::1:3 would map
   * to the Ethernet MAC address 33:33:00:01:00:03.
   *
   * NOTES:  This appears correct for the ICMPv6 Router Solicitation
   * Message, but the ICMPv6 Neighbor Solicitation message seems to
   * use 33:33:ff:01:00:03.
   */

  mac[0] = 0x33;
  mac[1] = 0x33;

  dev    = &priv->dev;
  tmp16  = dev->d_ipv6addr[6];
  mac[2] = 0xff;
  mac[3] = tmp16 >> 8;

  tmp16  = dev->d_ipv6addr[7];
  mac[4] = tmp16 & 0xff;
  mac[5] = tmp16 >> 8;

  ninfo("IPv6 Multicast: %02x:%02x:%02x:%02x:%02x:%02x\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  (void)bcmf_addmac(dev, mac);

#ifdef CONFIG_NET_ICMPv6_AUTOCONF
  /* Add the IPv6 all link-local nodes Ethernet address.  This is the
   * address that we expect to receive ICMPv6 Router Advertisement
   * packets.
   */

  (void)bcmf_addmac(dev, g_ipv6_ethallnodes.ether_addr_octet);

#endif /* CONFIG_NET_ICMPv6_AUTOCONF */

#ifdef CONFIG_NET_ICMPv6_ROUTER
  /* Add the IPv6 all link-local routers Ethernet address.  This is the
   * address that we expect to receive ICMPv6 Router Solicitation
   * packets.
   */

  (void)bcmf_addmac(dev, g_ipv6_ethallrouters.ether_addr_octet);

#endif /* CONFIG_NET_ICMPv6_ROUTER */
}
#endif /* CONFIG_NET_ICMPv6 */

/****************************************************************************
 * Name: bcmf_ioctl
 *
 * Description:
 *   Handle network IOCTL commands directed to this device.
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *   cmd - The IOCTL command
 *   arg - The argument for the IOCTL command
 *
 * Returned Value:
 *   OK on success; Negated errno on failure.
 *
 * Assumptions:
 *
 ****************************************************************************/

#ifdef CONFIG_NETDEV_IOCTL
static int bcmf_ioctl(FAR struct net_driver_s *dev, int cmd,
                      unsigned long arg)
{
  FAR struct bcmf_driver_s *priv = (FAR struct bcmf_driver_s *)dev->d_private;
  int ret;

  /* Decode and dispatch the driver-specific IOCTL command */

  switch (cmd)
    {
      /* Add cases here to support the IOCTL commands */

      default:
        nerr("ERROR: Unrecognized IOCTL command: %d\n", command);
        return -ENOTTY;  /* Special return value for this case */
    }

  return OK;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bcmf_initialize
 *
 * Description:
 *   Initialize the Broadcom 43362 controller and driver
 *
 * Parameters:
 *   intf - In the case where there are multiple EMACs, this value
 *          identifies which EMAC is to be initialized.
 *
 * Returned Value:
 *   OK on success; Negated errno on failure.
 *
 * Assumptions:
 *
 ****************************************************************************/

int bcmf_initialize(int intf)
{
  FAR struct bcmf_driver_s *priv;

  /* Get the interface structure associated with this interface number. */

  DEBUGASSERT(intf < CONFIG_IEEE80211_BROADCOM_NINTERFACES);
  priv = &g_bcmf_interface[intf];

  /* Attach the IRQ to the driver */
#warning Missing logic

  /* Initialize the driver structure */

  memset(priv, 0, sizeof(struct bcmf_driver_s));
  priv->bc_dev.d_buf     = g_pktbuf;      /* Single packet buffer */
  priv->bc_dev.d_ifup    = bcmf_ifup;     /* I/F up (new IP address) callback */
  priv->bc_dev.d_ifdown  = bcmf_ifdown;   /* I/F down callback */
  priv->bc_dev.d_txavail = bcmf_txavail;  /* New TX data callback */
#ifdef CONFIG_NET_IGMP
  priv->bc_dev.d_addmac  = bcmf_addmac;   /* Add multicast MAC address */
  priv->bc_dev.d_rmmac   = bcmf_rmmac;    /* Remove multicast MAC address */
#endif
#ifdef CONFIG_NETDEV_IOCTL
  priv->bc_dev.d_ioctl   = bcmf_ioctl;    /* Handle network IOCTL commands */
#endif
  priv->bc_dev.d_private = (FAR void *)g_bcmf_interface; /* Used to recover private state from dev */

  /* Create a watchdog for timing polling for and timing of transmisstions */

  priv->bc_txpoll        = wd_create();   /* Create periodic poll timer */
  priv->bc_txtimeout     = wd_create();   /* Create TX timeout timer */

  DEBUGASSERT(priv->bc_txpoll != NULL && priv->bc_txtimeout != NULL);

  /* Put the interface in the down state.  This usually amounts to resetting
   * the device and/or calling bcmf_ifdown().
   */

  /* Read the MAC address from the hardware into priv->bc_dev.d_mac.ether.ether_addr_octet */

  /* Register the device with the OS so that socket IOCTLs can be performed */

  (void)netdev_register(&priv->bc_dev, NET_LL_ETHERNET);
  return OK;
}

#endif /* CONFIG_NET && CONFIG_IEEE80211_BROADCOM_FULLMAC */

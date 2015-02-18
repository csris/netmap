/*
 * common headers
 */

#include <bsd_glue.h>
#include <net/netmap.h>
#include <dev/netmap/netmap_kern.h>
#include <dev/netmap/netmap_mem2.h>

#include "ptnetmap_user.h" //TODO: include ptnetmap_user.h into netmap.h or netmap_user.h
#include "ptnetmap-vhost/paravirt.h"
#include "ptnetmap-vhost/ptnetmap_vhost.h"

#ifdef WITH_PASSTHROUGH

#define PTN_RX_NOWORK_CYCLE   2                               /* RX cycle without receive any packets */
#define PTN_TX_BATCH_LIM      ((kring->nkr_num_slots >> 1))     /* Limit Batch TX to half ring */

//#define DEBUG  /* Enables communication debugging. */
#ifdef DEBUG
#define DBG(x) x
#else
#define DBG(x)
#endif


#undef RATE
//#define RATE  /* Enables communication statistics. */
#ifdef RATE
#define IFRATE(x) x
struct batch_info {
    uint64_t events;
    uint64_t zero_events;
    uint64_t slots;
};


static void batch_info_update(struct batch_info *bf, uint32_t pre_tail, uint32_t act_tail, uint32_t lim)
{
    int n_slots;

    n_slots = (int)act_tail - pre_tail;
    if (n_slots) {
        if (n_slots < 0)
            n_slots += lim;

        bf->events++;
        bf->slots += (uint64_t) n_slots;
    } else {
        bf->zero_events++;
    }
}

struct rate_stats {
    unsigned long gtxk;     /* Guest --> Host Tx kicks. */
    unsigned long grxk;     /* Guest --> Host Rx kicks. */
    unsigned long htxk;     /* Host --> Guest Tx kicks. */
    unsigned long hrxk;     /* Host --> Guest Rx Kicks. */
    unsigned long btxwu;    /* Backend Tx wake-up. */
    unsigned long brxwu;    /* Backend Rx wake-up. */
    unsigned long txpkts;   /* Transmitted packets. */
    unsigned long rxpkts;   /* Received packets. */
    unsigned long txfl;     /* TX flushes requests. */
    struct batch_info bf_tx;
    struct batch_info bf_rx;
};

struct rate_context {
    struct timer_list timer;
    struct rate_stats new;
    struct rate_stats old;
};

#define RATE_PERIOD  2
static void rate_callback(unsigned long arg)
{
    struct rate_context * ctx = (struct rate_context *)arg;
    struct rate_stats cur = ctx->new;
    struct batch_info *bf_tx = &cur.bf_tx;
    struct batch_info *bf_rx = &cur.bf_rx;
    struct batch_info *bf_tx_old = &ctx->old.bf_tx;
    struct batch_info *bf_rx_old = &ctx->old.bf_rx;
    uint64_t tx_batch, rx_batch;
    int r;

    tx_batch = ((bf_tx->events - bf_tx_old->events) > 0) ?
        (bf_tx->slots - bf_tx_old->slots) / (bf_tx->events - bf_tx_old->events): 0;
    rx_batch = ((bf_rx->events - bf_rx_old->events) > 0) ?
        (bf_rx->slots - bf_rx_old->slots) / (bf_rx->events - bf_rx_old->events): 0;

    printk("txp  = %lu Hz\n", (cur.txpkts - ctx->old.txpkts)/RATE_PERIOD);
    printk("gtxk = %lu Hz\n", (cur.gtxk - ctx->old.gtxk)/RATE_PERIOD);
    printk("htxk = %lu Hz\n", (cur.htxk - ctx->old.htxk)/RATE_PERIOD);
    printk("btxw = %lu Hz\n", (cur.btxwu - ctx->old.btxwu)/RATE_PERIOD);
    printk("rxp  = %lu Hz\n", (cur.rxpkts - ctx->old.rxpkts)/RATE_PERIOD);
    printk("grxk = %lu Hz\n", (cur.grxk - ctx->old.grxk)/RATE_PERIOD);
    printk("hrxk = %lu Hz\n", (cur.hrxk - ctx->old.hrxk)/RATE_PERIOD);
    printk("brxw = %lu Hz\n", (cur.brxwu - ctx->old.brxwu)/RATE_PERIOD);
    printk("txfl = %lu Hz\n", (cur.txfl - ctx->old.txfl)/RATE_PERIOD);
    printk("tx_batch = %llu avg\n", tx_batch);
    printk("rx_batch = %llu avg\n", rx_batch);
    printk("\n");

    ctx->old = cur;
    r = mod_timer(&ctx->timer, jiffies +
            msecs_to_jiffies(RATE_PERIOD * 1000));
    if (unlikely(r))
        D("[ptnetmap] Error: mod_timer()\n");
}

#else /* !RATE */
#define IFRATE(x)
#endif /* RATE */

struct ptnetmap_net {
    struct ptn_vhost_dev dev_tx, dev_rx;
    struct ptn_vhost_ring tx_ring, rx_ring;
    struct ptn_vhost_poll tx_poll, rx_poll;

    struct ptnetmap_config config;
    bool configured;
    struct paravirt_csb __user *csb;
    bool broken;

    struct file *nm_f;
    struct netmap_passthrough_adapter *pt_na;

    IFRATE(struct rate_context rate_ctx);
};

#define CSB_READ(csb, field, r) \
    do { \
        if (get_user(r, &csb->field)) { \
            D("get_user ERROR"); \
            r = -EFAULT; \
        } \
    } while (0)

#define CSB_WRITE(csb, field, v) \
    do { \
        if (put_user(v, &csb->field)) { \
            D("put_user ERROR"); \
            v = -EFAULT; \
        } \
    } while (0)

static inline int
ptnetmap_read_kring_csb(struct pt_ring __user *ptr, uint32_t *g_head,
        uint32_t *g_cur, uint32_t *g_flags)
{
    if(get_user(*g_head, &ptr->head))
        goto err;

    smp_mb();

    if(get_user(*g_cur, &ptr->cur))
        goto err;
    if(get_user(*g_flags, &ptr->sync_flags))
        goto err;

    return 0;
err:
    return EFAULT;
}

static inline int
ptnetmap_write_kring_csb(struct pt_ring __user *ptr, uint32_t hwcur,
        uint32_t hwtail)
{
    if(put_user(hwcur, &ptr->hwcur))
        goto err;

    smp_mb();

    if(put_user(hwtail, &ptr->hwtail))
        goto err;

    return 0;
err:
    return EFAULT;
}

static inline void
ptnetmap_kring_dump(const char *title, const struct netmap_kring *kring)
{
    D("%s - name: %s hwcur: %d hwtail: %d rhead: %d rcur: %d rtail: %d head: %d cur: %d tail: %d",
            title, kring->name, kring->nr_hwcur,
            kring->nr_hwtail, kring->rhead, kring->rcur, kring->rtail,
            kring->ring->head, kring->ring->cur, kring->ring->tail);
}

static inline void
ptnetmap_ring_reinit(struct netmap_kring *kring, uint32_t g_head, uint32_t g_cur)
{
    struct netmap_ring *ring = kring->ring;

    // XXX trust guest?
    ring->head = g_head;
    ring->cur = g_cur;
    ring->tail = kring->nr_hwtail;

    netmap_ring_reinit(kring);
    ptnetmap_kring_dump("kring reinit", kring);
}

static inline void ptnetmap_tx_set_hostkick(struct ptnetmap_net *net, uint32_t val)
{
    CSB_WRITE(net->csb, host_need_txkick, val);
}

static inline uint32_t ptnetmap_tx_get_guestkick(struct ptnetmap_net * net)
{
    uint32_t v;

    CSB_READ(net->csb, guest_need_txkick, v);

    return v;
}
static inline void ptnetmap_tx_set_guestkick(struct ptnetmap_net *net, uint32_t val)
{
    CSB_WRITE(net->csb, guest_need_txkick, val);
}

/*
 * Handle tx events: from the guest or from the backend
 */
static void ptnetmap_tx_handler(struct ptnetmap_net *net)
{
    struct ptn_vhost_ring *vr;
    struct netmap_kring *kring;
    struct pt_ring __user *csb_ring;
    uint32_t g_cur = 0, g_head = 0, g_flags = 0; /* guest variables; init for compiler */
    bool work = false;
    int batch;
    IFRATE(uint32_t pre_tail;)

    if (unlikely(!net)) {
        D("backend netmap is not configured");
        return;
    }

    vr = &net->tx_ring;
    csb_ring = &net->csb->tx_ring; /* netmap TX pointers in CSB */

    mutex_lock(&vr->mutex);

    if (unlikely(!net->pt_na || net->broken || !net->configured)) {
        D("backend netmap is not configured");
        goto leave;
    }

    kring = &net->pt_na->parent->tx_rings[0];

    if (nm_kr_tryget(kring)) {
        D("error nm_kr_tryget()");
        goto leave_kr_put;
    }

    /* Disable notifications. */
    ptnetmap_tx_set_hostkick(net, 0);

    if (ptnetmap_read_kring_csb(csb_ring, &g_head, &g_cur, &g_flags)) {
        D("error reading CSB()");
        goto leave_kr_put;
    }

    for (;;) {
#ifdef PTN_TX_BATCH_LIM
        batch = g_head - kring->nr_hwcur;

        if (batch < 0)
            batch += kring->nkr_num_slots;

        if (batch > PTN_TX_BATCH_LIM) {
            uint32_t new_head = kring->nr_hwcur + PTN_TX_BATCH_LIM;
            if (new_head >= kring->nkr_num_slots)
                new_head -= kring->nkr_num_slots;
            ND(1, "batch: %d old_head: %d new_head: %d", batch, g_head, new_head);
            g_head = new_head;
        }
#endif /* PTN_TX_BATCH_LIM */

        if (nm_kr_txspace(kring) <= (kring->nkr_num_slots >> 1)) {
            g_flags |= NAF_FORCE_RECLAIM;
        }

        if (nm_txsync_prologue(kring, g_head, g_cur, NULL)
                >= kring->nkr_num_slots) {
            ptnetmap_ring_reinit(kring, g_head, g_cur);
            /* Reenable notifications. */
            ptnetmap_tx_set_hostkick(net, 1);
            break;
        }

        if (netmap_verbose & NM_VERB_TXSYNC)
            ptnetmap_kring_dump("pre txsync", kring);

        IFRATE(pre_tail = kring->rtail;)

        if (likely(kring->nm_sync(kring, g_flags) == 0)) {
            /* finalize */
            if (ptnetmap_write_kring_csb(csb_ring, kring->nr_hwcur, kring->nr_hwtail)) {
                D("error writing CSB()");
                break;
            }
            if (kring->rtail != kring->nr_hwtail) {
                kring->rtail = kring->nr_hwtail;
                work = true;
            }
        } else {
            /* Reenable notifications. */
            ptnetmap_tx_set_hostkick(net, 0);
            D("nm_sync error");
            goto leave_kr_put;
        }

        IFRATE(batch_info_update(&net->rate_ctx.new.bf_tx, pre_tail, kring->rtail, kring->nkr_num_slots);)

        if (netmap_verbose & NM_VERB_TXSYNC)
            ptnetmap_kring_dump("post txsync", kring);

//#define BUSY_WAIT
#ifndef BUSY_WAIT
        if (work && ptnetmap_tx_get_guestkick(net)) {
            ptnetmap_tx_set_guestkick(net, 0);
            eventfd_signal(vr->call_ctx, 1);
            IFRATE(net->rate_ctx.new.htxk++);
            work = false;
        }
#endif
        if (ptnetmap_read_kring_csb(csb_ring, &g_head, &g_cur, &g_flags)) {
            D("error reading CSB()");
            break;
        }
#ifndef BUSY_WAIT
        /* Nothing to transmit */
        if (g_head == kring->rhead) {
            usleep_range(1,1);
            /* Reenable notifications. */
            ptnetmap_tx_set_hostkick(net, 1);
            /* Doublecheck. */
            if (ptnetmap_read_kring_csb(csb_ring, &g_head, &g_cur, &g_flags)) {
                D("error reading CSB()");
                break;
            }
            if (unlikely(g_head != kring->rhead)) {
                ptnetmap_tx_set_hostkick(net, 0);
                continue;
            } else
                break;
        }

        /* ring full */
        if (kring->nr_hwtail == kring->rhead) {
            RD(1, "TX ring FULL");
            break;
        }
#endif
        if (unlikely(net->broken || !net->configured)) {
            D("net broken");
            break;
        }
    }

leave_kr_put:
    nm_kr_put(kring);

leave:
    if (work && ptnetmap_tx_get_guestkick(net)) {
        ptnetmap_tx_set_guestkick(net, 0);
        eventfd_signal(vr->call_ctx, 1);
        IFRATE(net->rate_ctx.new.htxk++);
    }
    mutex_unlock(&vr->mutex);

    return;
}

static inline void ptnetmap_rx_set_hostkick(struct ptnetmap_net *net, uint32_t val)
{
    CSB_WRITE(net->csb, host_need_rxkick, val);
}

static inline uint32_t ptnetmap_rx_get_guestkick(struct ptnetmap_net * net)
{
    uint32_t v;

    CSB_READ(net->csb, guest_need_rxkick, v);

    return v;
}
static inline void ptnetmap_rx_set_guestkick(struct ptnetmap_net *net, uint32_t val)
{
    CSB_WRITE(net->csb, guest_need_rxkick, val);
}

/*
 * We needs kick from the guest when:
 *
 * - RX: tail == head - 1
 *       ring is full
 *       We need to wait that the guest gets some packets from the ring and then it notifies us.
 */
static inline int
nm_kr_rxfull(struct netmap_kring *kring, uint32_t g_head)
{
    return (ACCESS_ONCE(kring->nr_hwtail) == nm_prev(g_head, kring->nkr_num_slots - 1));
}

/*
 * Handle rx events: from the guest or from the backend
 */
static void ptnetmap_rx_handler(struct ptnetmap_net *net)
{
    struct ptn_vhost_ring *vr;
    struct netmap_kring *kring;
    struct pt_ring __user *csb_ring;
    uint32_t g_cur = 0, g_head = 0, g_flags = 0; /* guest variables; init for compiler */
    int cicle_nowork = 0;
    bool work = false;
    IFRATE(uint32_t pre_tail;)

    if (unlikely(!net)) {
        D("backend netmap is not configured");
        return;
    }

    vr = &net->rx_ring;
    csb_ring = &net->csb->rx_ring; /* netmap RX pointers in CSB */

    mutex_lock(&vr->mutex);

    if (unlikely(!net->pt_na || net->broken || !net->configured)) {
        D("backend netmap is not configured");
        goto leave;
    }

    kring = &net->pt_na->parent->rx_rings[0];

    if (nm_kr_tryget(kring)) {
        D("error nm_kr_tryget()");
        goto leave;
    }

    /* Disable notifications. */
    ptnetmap_rx_set_hostkick(net, 0);

    if (ptnetmap_read_kring_csb(csb_ring, &g_head, &g_cur, &g_flags)) {
        D("error reading CSB()");
        goto leave_kr_put;
    }

    for (;;) {

        if (nm_rxsync_prologue(kring, g_head, g_cur, NULL)
                >= kring->nkr_num_slots) {
            ptnetmap_ring_reinit(kring, g_head, g_cur);
            /* Reenable notifications. */
            ptnetmap_rx_set_hostkick(net, 1);
            break;
        }

        if (netmap_verbose & NM_VERB_RXSYNC)
            ptnetmap_kring_dump("pre rxsync", kring);

        IFRATE(pre_tail = kring->rtail;)

        if (kring->nm_sync(kring, g_flags) == 0) {
            /* finalize */
            if (ptnetmap_write_kring_csb(csb_ring, kring->nr_hwcur, kring->nr_hwtail)) {
                D("error writing CSB()");
                break;
            }
            if (kring->rtail != kring->nr_hwtail) {
                kring->rtail = kring->nr_hwtail;
                work = true;
                cicle_nowork = 0;
            } else {
                cicle_nowork++;
            }
        } else {
            /* Reenable notifications. */
            ptnetmap_rx_set_hostkick(net, 0);
            D("nm_sync error");
            goto leave_kr_put;
        }

        IFRATE(batch_info_update(&net->rate_ctx.new.bf_rx, pre_tail, kring->rtail, kring->nkr_num_slots);)

        if (netmap_verbose & NM_VERB_RXSYNC)
            ptnetmap_kring_dump("post rxsync", kring);

#ifndef BUSY_WAIT
        if (work && ptnetmap_rx_get_guestkick(net)) {
            ptnetmap_rx_set_guestkick(net, 0);
            eventfd_signal(vr->call_ctx, 1);
            IFRATE(net->rate_ctx.new.hrxk++);
            work = false;
        }
#endif
        if (ptnetmap_read_kring_csb(csb_ring, &g_head, &g_cur, &g_flags)) {
            D("error reading CSB()");
            break;
        }
#ifndef BUSY_WAIT
        /* No space to receive */
        if (nm_kr_rxfull(kring, g_head)) {
            usleep_range(1,1);
            /* Reenable notifications. */
            ptnetmap_rx_set_hostkick(net, 1);
            /* Doublecheck. */
            if (ptnetmap_read_kring_csb(csb_ring, &g_head, &g_cur, &g_flags)) {
                D("error reading CSB()");
                break;
            }
            if (unlikely(!nm_kr_rxfull(kring, g_head))) {
                ptnetmap_rx_set_hostkick(net, 0);
                continue;
            } else
                break;
        }

        /* ring empty */
        if (kring->nr_hwtail == kring->rhead || cicle_nowork >= PTN_RX_NOWORK_CYCLE) {
            RD(1, "nr_hwtail: %d rhead: %d cicle_nowork: %d", kring->nr_hwtail, kring->rhead, cicle_nowork);
            break;
        }
#endif
        if (unlikely(net->broken || !net->configured)) {
            D("net broken");
            break;
        }
    }

leave_kr_put:
    nm_kr_put(kring);

leave:
    if (work && ptnetmap_rx_get_guestkick(net)) {
        ptnetmap_rx_set_guestkick(net, 0);
        eventfd_signal(vr->call_ctx, 1);
        IFRATE(net->rate_ctx.new.hrxk++);
    }
    mutex_unlock(&vr->mutex);
}

static void ptnetmap_tx_handle_kick(struct ptn_vhost_work *work)
{
    struct ptn_vhost_ring *vr = container_of(work, struct ptn_vhost_ring,
            poll.work);
    struct ptnetmap_net *net = container_of(vr->dev, struct ptnetmap_net, dev_tx);

    IFRATE(net->rate_ctx.new.gtxk++);
    ptnetmap_tx_handler(net);
}

static void ptnetmap_rx_handle_kick(struct ptn_vhost_work *work)
{
    struct ptn_vhost_ring *vr = container_of(work, struct ptn_vhost_ring,
            poll.work);
    struct ptnetmap_net *net = container_of(vr->dev, struct ptnetmap_net, dev_rx);

    IFRATE(net->rate_ctx.new.grxk++);
    ptnetmap_rx_handler(net);
}

static void ptnetmap_tx_handle_net(struct ptn_vhost_work *work)
{
    struct ptnetmap_net *net = container_of(work, struct ptnetmap_net,
            tx_poll.work);

    IFRATE(net->rate_ctx.new.btxwu++);
    ptnetmap_tx_handler(net);
}

static void ptnetmap_rx_handle_net(struct ptn_vhost_work *work)
{
    struct ptnetmap_net *net = container_of(work, struct ptnetmap_net,
            rx_poll.work);

    IFRATE(net->rate_ctx.new.brxwu++);
    ptnetmap_rx_handler(net);
}

static int ptnetmap_set_eventfds_ring(struct ptn_vhost_ring *vr, struct ptnetmap_config_ring *vrc)
{
    vr->kick = eventfd_fget(vrc->ioeventfd);
    if (IS_ERR(vr->kick))
        return PTR_ERR(vr->kick);

    vr->call = eventfd_fget(vrc->irqfd);
    if (IS_ERR(vr->call))
        return PTR_ERR(vr->call);
    vr->call_ctx = eventfd_ctx_fileget(vr->call);

    if (vrc->resamplefd != ~0U) {
        vr->resample = eventfd_fget(vrc->resamplefd);
        if (IS_ERR(vr->resample))
            return PTR_ERR(vr->resample);
        vr->resample_ctx = eventfd_ctx_fileget(vr->resample);
    } else {
        vr->resample = NULL;
        vr->resample_ctx = NULL;
    }

    return 0;
}

static int ptnetmap_set_eventfds(struct ptnetmap_net * net)
{
    int r;

    if ((r = ptnetmap_set_eventfds_ring(&net->tx_ring, &net->config.tx_ring)))
        return r;
    if ((r = ptnetmap_set_eventfds_ring(&net->rx_ring, &net->config.rx_ring)))
        return r;

    return 0;
}

static int ptnetmap_set_backend(struct ptnetmap_net * net)
{
    net->nm_f = fget(net->config.netmap_fd);
    if (IS_ERR(net->nm_f))
        return PTR_ERR(net->nm_f);
    D("netmap_fd:%u f_count:%d", net->config.netmap_fd, (int)net->nm_f->f_count.counter);

    return 0;
}

static void ptnetmap_print_configuration(struct ptnetmap_net * net)
{
    struct ptnetmap_config *cfg = &net->config;

    printk("[ptn] configuration:\n");
    printk("TX: iofd=%u, irqfd=%u, resfd=%d\n",
            cfg->tx_ring.ioeventfd, cfg->tx_ring.irqfd, cfg->tx_ring.resamplefd);
    printk("RX: iofd=%u, irqfd=%u, resfd=%d\n",
            cfg->rx_ring.ioeventfd, cfg->rx_ring.irqfd, cfg->rx_ring.resamplefd);
    printk("Backend: netmapfd=%u\n", cfg->netmap_fd);
    printk("CSB: csb_addr=%p\n", cfg->csb);

}

int ptnetmap_poll_start(struct ptnetmap_net *net, struct file *file,
        struct netmap_passthrough_adapter *pt_na)
{
    int ret = 0;

    if (!net->tx_poll.wqh) {
        poll_wait(file, &pt_na->parent->tx_rings[0].si, &net->tx_poll.table);
        ptn_vhost_poll_queue(&net->tx_poll);
        printk("%p.poll_start()\n", &net->tx_poll);
    }

    if (!net->rx_poll.wqh) {
        poll_wait(file, &pt_na->parent->rx_rings[0].si, &net->rx_poll.table);
        ptn_vhost_poll_queue(&net->rx_poll);
        printk("%p.poll_start()\n", &net->rx_poll);
    }

    return ret;
}

static int ptnetmap_configure(struct ptnetmap_net * net, struct netmap_passthrough_adapter *pt_na)
{
    int r;

    /* Configure. */
    if ((r = ptn_vhost_dev_set_owner(&net->dev_tx)))
        return r;
    if ((r = ptn_vhost_dev_set_owner(&net->dev_rx)))
        return r;
    if ((r = ptnetmap_set_eventfds(net)))
        return r;
    if ((r = ptnetmap_set_backend(net)))
        return r;
    ///XXX function ???
    net->csb = net->config.csb;

    ptnetmap_print_configuration(net);

    /* Start polling. */
    if (net->tx_ring.handle_kick && (r = ptn_vhost_poll_start(&net->tx_ring.poll, net->tx_ring.kick)))
        return r;
    if (net->rx_ring.handle_kick && (r = ptn_vhost_poll_start(&net->rx_ring.poll, net->rx_ring.kick)))
        return r;
    if (net->nm_f && (r = ptnetmap_poll_start(net, net->nm_f, pt_na)))
        return r;

    return 0;
}

static int
ptnetmap_kring_snapshot(struct netmap_kring *kring, struct pt_ring __user *pt_ring)
{
    if(put_user(kring->rhead, &pt_ring->head))
        goto err;
    if(put_user(kring->rcur, &pt_ring->cur))
        goto err;

    if(put_user(kring->nr_hwcur, &pt_ring->hwcur))
        goto err;
    if(put_user(kring->nr_hwtail, &pt_ring->hwtail))
        goto err;

    ptnetmap_kring_dump("", kring);

    return 0;
err:
    return EFAULT;
}

static int
ptnetmap_krings_snapshot(struct netmap_passthrough_adapter *pt_na, struct ptnetmap_net * net)
{
    struct netmap_kring *kring;
    int error = 0;

    kring = &pt_na->parent->tx_rings[0];
    if((error = ptnetmap_kring_snapshot(kring, &net->csb->tx_ring)))
        goto err;

    kring = &pt_na->parent->rx_rings[0];
    error = ptnetmap_kring_snapshot(kring, &net->csb->rx_ring);

err:
    return error;
}

static int
ptnetmap_create(struct netmap_passthrough_adapter *pt_na, const void __user *buf, uint16_t buf_len)
{
    struct ptnetmap_net *net = kmalloc(sizeof *net, GFP_KERNEL);
    struct ptn_vhost_dev *dev_tx, *dev_rx;
    int ret;

    /* XXX check if already attached */

    D("");
    printk("%p.OPEN()\n", net);
    if (!net)
        return ENOMEM;
    net->configured = net->broken = false;

    dev_tx = &net->dev_tx;
    dev_rx = &net->dev_rx;
    net->tx_ring.handle_kick = ptnetmap_tx_handle_kick;
    net->rx_ring.handle_kick = ptnetmap_rx_handle_kick;

    ret = ptn_vhost_dev_init(dev_tx, &net->tx_ring, NULL);
    if (ret < 0) {
        kfree(net);
        return ret;
    }
    ret = ptn_vhost_dev_init(dev_rx, NULL, &net->rx_ring);
    if (ret < 0) {
        kfree(net);
        return ret;
    }

    ptn_vhost_poll_init(&net->tx_poll, ptnetmap_tx_handle_net, POLLOUT, dev_tx);
    ptn_vhost_poll_init(&net->rx_poll, ptnetmap_rx_handle_net, POLLIN, dev_rx);

#ifdef RATE
    memset(&net->rate_ctx, 0, sizeof(net->rate_ctx));
    setup_timer(&net->rate_ctx.timer, &rate_callback,
            (unsigned long)&net->rate_ctx);
    if (mod_timer(&net->rate_ctx.timer, jiffies + msecs_to_jiffies(1500)))
        printk("[ptn] Error: mod_timer()\n");
#endif

    printk("%p.OPEN_END()\n", net);

    mutex_lock(&net->dev_tx.mutex);
    mutex_lock(&net->dev_rx.mutex);

    if (buf_len != sizeof(struct ptnetmap_config)) {
        D("buf_len ERROR! - buf_len %d, expected %d", (int)buf_len, (int)sizeof(struct ptnetmap_config));
        ret = EINVAL;
        goto err;
    }

    /* Read the configuration from userspace. */
    if (copy_from_user(&net->config, buf, sizeof(struct ptnetmap_config))) {
        D("copy_from_user() ERROR!");
        ret = EFAULT;
        goto err;
    }

    D("configuration read");
    if ((ret = ptnetmap_configure(net, pt_na))) {
        D("ptnetmap_configure error");
        goto err;
    }

    if ((ret = ptnetmap_krings_snapshot(pt_na, net))) {
        D("ptnetmap_krings_snapshot error");
        goto err;
    }

    D("configuration OK");

    net->configured = true;
    pt_na->private = net;
    net->pt_na = pt_na;

    mutex_unlock(&net->dev_rx.mutex);
    mutex_unlock(&net->dev_tx.mutex);

    return 0;

err:
    mutex_unlock(&net->dev_rx.mutex);
    mutex_unlock(&net->dev_tx.mutex);
    kfree(net);
    return ret;
}

static void ptnetmap_net_stop_vr(struct ptnetmap_net *net,
        struct ptn_vhost_ring *vr)
{
    mutex_lock(&vr->mutex);
    if (vr == &net->tx_ring)
        ptn_vhost_poll_stop(&net->tx_poll);
    else
        ptn_vhost_poll_stop(&net->rx_poll);
    mutex_unlock(&vr->mutex);
}

static void ptnetmap_net_stop(struct ptnetmap_net *net)
{
    ptnetmap_net_stop_vr(net, &net->tx_ring);
    ptnetmap_net_stop_vr(net, &net->rx_ring);
}

static void ptnetmap_net_flush(struct ptnetmap_net *n)
{
    ptn_vhost_poll_flush(&n->rx_poll);
    ptn_vhost_poll_flush(&n->dev_rx.rx_ring->poll);
    ptn_vhost_poll_flush(&n->tx_poll);
    ptn_vhost_poll_flush(&n->dev_tx.tx_ring->poll);
}

static int
ptnetmap_delete(struct netmap_passthrough_adapter *pt_na)
{
    struct ptnetmap_net *net = pt_na->private;

    D("");
    printk("%p.RELEASE()\n", net);

    /* check if it is configured */
    if (!net)
        return EFAULT;

    net->configured = false;

    ptnetmap_net_stop(net);
    ptnetmap_net_flush(net);
    ptn_vhost_dev_stop(&net->dev_tx);
    ptn_vhost_dev_stop(&net->dev_rx);
    ptn_vhost_dev_cleanup(&net->dev_tx);
    ptn_vhost_dev_cleanup(&net->dev_rx);
    if (net->nm_f)
        fput(net->nm_f);
    /* We do an extra flush before freeing memory,
     * since jobs can re-queue themselves. */
    ptnetmap_net_flush(net);

    IFRATE(del_timer(&net->rate_ctx.timer));
    kfree(net);

    pt_na->private = NULL;

    printk("%p.RELEASE_END()\n", net);
    return 0;
}


int
ptnetmap_ctl(struct nmreq *nmr, struct netmap_adapter *na)
{
    struct netmap_passthrough_adapter *pt_na;
    char *name;
    int cmd, error = 0;
    void __user *buf;
    uint16_t buf_len;

    name = nmr->nr_name;
    cmd = nmr->nr_cmd;

    D("name: %s", name);

    if (!nm_passthrough_on(na)) {
        D("Internal error: interface not in netmap passthrough mode. na = %p", na);
        error = ENXIO;
        goto done;
    }
    pt_na = (struct netmap_passthrough_adapter *)na;

    switch (cmd) {
        case NETMAP_PT_CREATE:
            nmr_read_buf(nmr, &buf, &buf_len);
            error = ptnetmap_create(pt_na, buf, buf_len);
            break;
        case NETMAP_PT_DELETE:
            error = ptnetmap_delete(pt_na);
            break;
        default:
            D("invalid cmd (nmr->nr_cmd) (0x%x)", cmd);
            error = EINVAL;
            break;
    }

done:
    return error;
}

static int
ptnetmap_notify(struct netmap_adapter *na, u_int n_ring,
        enum txrx tx, int flags)
{
    struct netmap_kring *kring;

    if (tx == NR_TX) {
        kring = na->tx_rings + n_ring;
        mb();
        //wake_up(&kring->si);
        wake_up_interruptible_poll(&kring->si, POLLOUT |
                POLLWRNORM | POLLWRBAND);
        /* optimization: avoid a wake up on the global
         * queue if nobody has registered for more
         * than one ring
         */
        if (na->tx_si_users > 0)
            OS_selwakeup(&na->tx_si, PI_NET);
    } else {
        kring = na->rx_rings + n_ring;
        mb();
        //wake_up(&kring->si);
        wake_up_interruptible_poll(&kring->si, POLLIN |
                POLLRDNORM | POLLRDBAND);
        /* optimization: same as above */
        if (na->rx_si_users > 0)
            OS_selwakeup(&na->rx_si, PI_NET);
    }
    return 0;
}

//XXX maybe is unnecessary redefine the *xsync
/* nm_txsync callback for passthrough */
static int
ptnetmap_txsync(struct netmap_kring *kring, int flags)
{
    struct netmap_passthrough_adapter *pt_na =
        (struct netmap_passthrough_adapter *)kring->na;
    struct netmap_adapter *parent = pt_na->parent;
    int n;

    D("");
    n = parent->nm_txsync(kring, flags);

    return n;
}

/* nm_rxsync callback for passthrough */
    static int
ptnetmap_rxsync(struct netmap_kring *kring, int flags)
{
    struct netmap_passthrough_adapter *pt_na =
        (struct netmap_passthrough_adapter *)kring->na;
    struct netmap_adapter *parent = pt_na->parent;
    int n;

    D("");
    n = parent->nm_rxsync(kring, flags);

    return n;
}

/* nm_config callback for bwrap */
static int
ptnetmap_config(struct netmap_adapter *na, u_int *txr, u_int *txd,
        u_int *rxr, u_int *rxd)
{
    struct netmap_passthrough_adapter *pt_na =
        (struct netmap_passthrough_adapter *)na;
    struct netmap_adapter *parent = pt_na->parent;
    int error;

    //XXX: maybe call parent->nm_config is better

    /* forward the request */
    error = netmap_update_config(parent);

    *rxr = na->num_rx_rings = parent->num_rx_rings;
    *txr = na->num_tx_rings = parent->num_tx_rings;
    *txd = na->num_tx_desc = parent->num_tx_desc;
    *rxd = na->num_rx_desc = parent->num_rx_desc;

    D("rxr: %d txr: %d txd: %d rxd: %d", *rxr, *txr, *txd, *rxd);

    return error;
}

/* nm_krings_create callback for passthrough */
static int
ptnetmap_krings_create(struct netmap_adapter *na)
{
    struct netmap_passthrough_adapter *pt_na =
        (struct netmap_passthrough_adapter *)na;
    struct netmap_adapter *parent = pt_na->parent;
    int error;

    D("%s", na->name);

    /* create the parent krings */
    error = parent->nm_krings_create(parent);
    if (error) {
        return error;
    }

    na->tx_rings = parent->tx_rings;
    na->rx_rings = parent->rx_rings;
    na->tailroom = parent->tailroom; //XXX

    return 0;
}

/* nm_krings_delete callback for passthrough */
static void
ptnetmap_krings_delete(struct netmap_adapter *na)
{
    struct netmap_passthrough_adapter *pt_na =
        (struct netmap_passthrough_adapter *)na;
    struct netmap_adapter *parent = pt_na->parent;

    D("%s", na->name);

    parent->nm_krings_delete(parent);

    na->tx_rings = na->rx_rings = na->tailroom = NULL;
}

/* nm_register callback */
static int
ptnetmap_register(struct netmap_adapter *na, int onoff)
{
    struct netmap_passthrough_adapter *pt_na =
        (struct netmap_passthrough_adapter *)na;
    struct netmap_adapter *parent = pt_na->parent;
    int error;
    D("%p: onoff %d", na, onoff);

    if (onoff) {
        /* netmap_do_regif has been called on the
         * passthrough na.
         * We need to pass the information about the
         * memory allocator to the parent before
         * putting it in netmap mode
         */
        parent->na_lut = na->na_lut;
    }

    /* forward the request to the parent */
    error = parent->nm_register(parent, onoff);
    if (error)
        return error;


    if (onoff) {
        na->na_flags |= NAF_NETMAP_ON | NAF_PASSTHROUGH_FULL;
    } else {
        ptnetmap_delete(pt_na);
        na->na_flags &= ~(NAF_NETMAP_ON | NAF_PASSTHROUGH_FULL);
    }

    return 0;
}

/* nm_dtor callback */
static void
ptnetmap_dtor(struct netmap_adapter *na)
{
    struct netmap_passthrough_adapter *pt_na =
        (struct netmap_passthrough_adapter *)na;

    D("%p", na);

    pt_na->parent->na_flags &= ~NAF_BUSY;
    netmap_adapter_put(pt_na->parent);
    pt_na->parent = NULL;
}

/* check if nmr is a request for a passthrough adapter that we can satisfy */
int
netmap_get_passthrough_na(struct nmreq *nmr, struct netmap_adapter **na, int create)
{
    struct nmreq parent_nmr;
    struct netmap_adapter *parent; /* target adapter */
    struct netmap_passthrough_adapter *pt_na;
    int error;

    if ((nmr->nr_flags & (NR_PASSTHROUGH_FULL)) == 0) {
        D("not a passthrough");
        return 0;
    }
    /* this is a request for a passthrough adapter */
    D("flags %x", nmr->nr_flags);

    pt_na = malloc(sizeof(*pt_na), M_DEVBUF, M_NOWAIT | M_ZERO);
    if (pt_na == NULL) {
        D("memory error");
        return ENOMEM;
    }

    /* first, try to find the adapter that we want to passthrough
     * We use the same nmr, after we have turned off the passthrough flag.
     * In this way we can potentially passthrough everything netmap understands.
     */
    memcpy(&parent_nmr, nmr, sizeof(parent_nmr));
    parent_nmr.nr_flags &= ~(NR_PASSTHROUGH_FULL);
    error = netmap_get_na(&parent_nmr, &parent, create);
    if (error) {
        D("parent lookup failed: %d", error);
        goto put_out_noputparent;
    }
    D("found parent: %s", parent->name);

    /* make sure the NIC is not already in use */
    if (NETMAP_OWNED_BY_ANY(parent)) {
        D("NIC %s busy, cannot passthrough", parent->name);
        error = EBUSY;
        goto put_out;
    }

    pt_na->parent = parent;

    //XXX pt_na->up.na_flags = parent->na_flags;
    pt_na->up.num_rx_rings = parent->num_rx_rings;
    pt_na->up.num_tx_rings = parent->num_tx_rings;
    pt_na->up.num_tx_desc = parent->num_tx_desc;
    pt_na->up.num_rx_desc = parent->num_rx_desc;

    pt_na->up.nm_dtor = ptnetmap_dtor;
    pt_na->up.nm_register = ptnetmap_register;

    //XXX maybe is unnecessary redefine the *xsync
    pt_na->up.nm_txsync = ptnetmap_txsync;
    pt_na->up.nm_rxsync = ptnetmap_rxsync;

    pt_na->up.nm_krings_create = ptnetmap_krings_create;
    pt_na->up.nm_krings_delete = ptnetmap_krings_delete;
    pt_na->up.nm_config = ptnetmap_config;

    pt_na->up.nm_notify = ptnetmap_notify;
    //XXX restore
    parent->nm_notify = ptnetmap_notify;

    //XXX needed?
    //pt_na->up.nm_bdg_attach = ptnetmap_bdg_attach;
    //pt_na->up.nm_bdg_ctl = ptnetmap_bdg_ctl;

    pt_na->up.nm_mem = parent->nm_mem;
    error = netmap_attach_common(&pt_na->up);
    if (error) {
        D("attach_common error");
        goto put_out;
    }

    *na = &pt_na->up;
    netmap_adapter_get(*na);

    /* write the configuration back */
    nmr->nr_tx_rings = pt_na->up.num_tx_rings;
    nmr->nr_rx_rings = pt_na->up.num_rx_rings;
    nmr->nr_tx_slots = pt_na->up.num_tx_desc;
    nmr->nr_rx_slots = pt_na->up.num_rx_desc;

    parent->na_flags |= NAF_BUSY;

    strncpy(pt_na->up.name, parent->name, sizeof(pt_na->up.name));
    strcat(pt_na->up.name, "-PT");
    D("passthrough full ok");
    return 0;

put_out:
    netmap_adapter_put(parent);
put_out_noputparent:
    free(pt_na, M_DEVBUF);
    return error;
}
#endif /* WITH_PASSTHROUGH */

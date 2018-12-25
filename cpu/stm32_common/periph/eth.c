/*
 * Copyright (C) 2016 TriaGnoSys GmbH
 *
 * This file is subject to the terms and conditions of the GNU Lesser General
 * Public License v2.1. See the file LICENSE in the top level directory for more
 * details.
 */

/**
 * @ingroup     cpu_stm32f4
 * @{
 *
 * @file
 * @brief       Low-level ETH driver implementation
 *
 * @author      Víctor Ariño <victor.arino@triagnosys.com>
 *
 * @}
 */
#include "mutex.h"
#include "periph/gpio.h"
#include "luid.h"
#include "net/ethernet.h"
#include "net/phy.h"

#define ENABLE_DEBUG (0)
#include "debug.h"

#include <string.h>
#if ETH_NUMOF
/* Set the value of the divider with the clock configured */
#if !defined(CLOCK_CORECLOCK) || CLOCK_CORECLOCK < (20000000U)
#error This peripheral requires a CORECLOCK of at least 20MHz
#elif CLOCK_CORECLOCK < (35000000U)
#define CLOCK_RANGE ETH_MACMIIAR_CR_Div16
#elif CLOCK_CORECLOCK < (60000000U)
#define CLOCK_RANGE ETH_MACMIIAR_CR_Div26
#elif CLOCK_CORECLOCK < (100000000U)
#define CLOCK_RANGE ETH_MACMIIAR_CR_Div42
#elif CLOCK_CORECLOCK < (150000000U)
#define CLOCK_RANGE ETH_MACMIIAR_CR_Div62
#else /* CLOCK_CORECLOCK < (20000000U) */
#define CLOCK_RANGE ETH_MACMIIAR_CR_Div102
#endif /* CLOCK_CORECLOCK < (20000000U) */

/* Internal flags for the DMA descriptors */
#define DESC_OWN           (0x80000000)
#define RX_DESC_FL         (0x3FFF0000)
#define RX_DESC_FS         (0x00000200)
#define RX_DESC_LS         (0x00000100)
#define RX_DESC_RCH        (0x00004000)
#define TX_DESC_TCH        (0x00100000)
#define TX_DESC_IC         (0x40000000)
#define TX_DESC_CIC        (0x00C00000)
#define TX_DESC_LS         (0x20000000)
#define TX_DESC_FS         (0x10000000)

struct eth_dma_desc {
    uint32_t status;
    uint32_t control;
    char *buffer_addr;
    struct eth_dma_desc *desc_next;
    uint32_t reserved1_ext;
    uint32_t reserved2;
    uint32_t ts_low;
    uint32_t ts_high;
} __attribute__((packed));

typedef struct eth_dma_desc edma_desc_t;

/* Descriptors */
static edma_desc_t rx_desc[ETH_RX_BUFFER_COUNT];
static edma_desc_t tx_desc[ETH_TX_BUFFER_COUNT];
static edma_desc_t *rx_curr;
static edma_desc_t *tx_curr;

/* Buffers */
static char rx_buffer[ETH_RX_BUFFER_COUNT][ETH_RX_BUFFER_SIZE];
static char tx_buffer[ETH_TX_BUFFER_COUNT][ETH_TX_BUFFER_SIZE];

/* Mutex relying on interrupt */
static mutex_t _dma_sync = MUTEX_INIT;

/** Read or write a phy register, to write the register ETH_MACMIIAR_MW is to
 * be passed as the higher nibble of the value */
static unsigned _rw_phy(unsigned addr, unsigned reg, unsigned value)
{
    unsigned tmp;

    while (ETH->MACMIIAR & ETH_MACMIIAR_MB) ;
    DEBUG("stm32_eth: rw_phy %x (%x): %x\n", addr, reg, value);

    tmp = (ETH->MACMIIAR & ETH_MACMIIAR_CR) | ETH_MACMIIAR_MB;
    tmp |= (((addr & 0x1f) << 11) | ((reg & 0x1f) << 6));
    tmp |= (value >> 16);

    ETH->MACMIIDR = (value & 0xffff);
    ETH->MACMIIAR = tmp;
    while (ETH->MACMIIAR & ETH_MACMIIAR_MB) ;

    DEBUG("stm32_eth: %lx\n", ETH->MACMIIDR);
    return (ETH->MACMIIDR & 0x0000ffff);
}

int32_t eth_phy_read(uint16_t addr, uint8_t reg)
{
    return _rw_phy(addr, reg, 0);
}

int32_t eth_phy_write(uint16_t addr, uint8_t reg, uint16_t value)
{
    _rw_phy(addr, reg, (value & 0xffff) | (ETH_MACMIIAR_MW << 16));
    return 0;
}

void get_mac(char *out)
{
    unsigned t;

    t = ETH->MACA0HR;
    out[0] = (t >> 8);
    out[1] = (t & 0xff);

    t = ETH->MACA0LR;
    out[2] = (t >> 24);
    out[3] = (t >> 16);
    out[4] = (t >> 8);
    out[5] = (t & 0xff);
}

/** Set the mac address. The peripheral supports up to 4 MACs but only one is
 * implemented */
void set_mac(const char *mac)
{
    ETH->MACA0HR &= 0xffff0000;
    ETH->MACA0HR |= ((mac[0] << 8) | mac[1]);
    ETH->MACA0LR = ((mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5]);
}

/** Initialization of the DMA descriptors to be used */
static void _init_dma(void)
{
    int i;

    for (i = 0; i < ETH_RX_BUFFER_COUNT; i++) {
        rx_desc[i].status = DESC_OWN;
        rx_desc[i].control = RX_DESC_RCH | (ETH_RX_BUFFER_SIZE & 0x0fff);
        rx_desc[i].buffer_addr = &rx_buffer[i][0];
        rx_desc[i].desc_next = &rx_desc[i + 1];
    }
    rx_desc[i - 1].desc_next = &rx_desc[0];

    for (i = 0; i < ETH_TX_BUFFER_COUNT; i++) {
        tx_desc[i].status = TX_DESC_TCH | TX_DESC_CIC;
        tx_desc[i].buffer_addr = &tx_buffer[i][0];
        tx_desc[i].desc_next = &tx_desc[i + 1];
    }
    tx_desc[i - 1].desc_next = &tx_desc[0];

    rx_curr = &rx_desc[0];
    tx_curr = &tx_desc[0];

    ETH->DMARDLAR = (uint32_t)rx_curr;
    ETH->DMATDLAR = (uint32_t)tx_curr;

    /* initialize tx DMA */
    DMA_Stream_TypeDef *stream = dma_stream(eth_config.dma_stream);

    mutex_lock(&_dma_sync);
    dma_poweron(eth_config.dma_stream);
    dma_isr_enable(eth_config.dma_stream);
    stream->CR = ((eth_config.dma_chan << 25) |
                  DMA_SxCR_MINC | DMA_SxCR_PINC |
                  DMA_SxCR_MBURST | DMA_SxCR_PBURST |
                  DMA_SxCR_PL_1 | DMA_SxCR_DIR_1 | DMA_SxCR_TCIE);
    stream->FCR = DMA_SxFCR_DMDIS | DMA_SxFCR_FTH;
}

int eth_init(void)
{
    char hwaddr[ETHERNET_ADDR_LEN];
    /* enable APB2 clock */
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;

    /* select RMII if necessary */
    if (eth_config.mode == RMII) {
        SYSCFG->PMC |= SYSCFG_PMC_MII_RMII_SEL;
    }

    /* initialize GPIO */
    for (int i = 0; i < (int) eth_config.mode; i++) {
        gpio_init(eth_config.pins[i], GPIO_OUT);
        gpio_init_af(eth_config.pins[i], GPIO_AF11);
    }

    /* enable all clocks */
    RCC->AHB1ENR |= (RCC_AHB1ENR_ETHMACEN | RCC_AHB1ENR_ETHMACTXEN
                     | RCC_AHB1ENR_ETHMACRXEN | RCC_AHB1ENR_ETHMACPTPEN);

    /* reset the peripheral */
    RCC->AHB1RSTR |= RCC_AHB1RSTR_ETHMACRST;
    RCC->AHB1RSTR &= ~RCC_AHB1RSTR_ETHMACRST;

    /* software reset */
    ETH->DMABMR |= ETH_DMABMR_SR;
    while (ETH->DMABMR & ETH_DMABMR_SR) ;

    /* set the clock divider */
    while (ETH->MACMIIAR & ETH_MACMIIAR_MB) ;
    ETH->MACMIIAR = CLOCK_RANGE;

    /* configure the PHY (standard for all PHY's) */
    /* if there's no PHY, this has no effect */
    eth_phy_write(eth_config.phy_addr, PHY_BMCR, BMCR_RESET);

    /* speed from conf */
    ETH->MACCR |= (ETH_MACCR_ROD | ETH_MACCR_IPCO | ETH_MACCR_APCS |
                   ((eth_config.speed & 0x0100) << 3) |
                   ((eth_config.speed & 0x2000) << 1));

    /* pass all */
    //ETH->MACFFR |= ETH_MACFFR_RA;
    /* perfect filter on address */
    ETH->MACFFR |= ETH_MACFFR_PAM | ETH_MACFFR_DAIF;

    /* store forward */
    ETH->DMAOMR |= ETH_DMAOMR_RSF | ETH_DMAOMR_TSF | ETH_DMAOMR_OSF;

    /* configure DMA */
    ETH->DMABMR = ETH_DMABMR_DA | ETH_DMABMR_AAB | ETH_DMABMR_FB |
                  ETH_DMABMR_RDP_32Beat | ETH_DMABMR_PBL_32Beat | ETH_DMABMR_EDE;

    if(eth_config.mac[0] != 0) {
      set_mac(eth_config.mac);
    }  else {
      luid_get(hwaddr, ETHERNET_ADDR_LEN);
      set_mac(hwaddr);
    }

    _init_dma();

    NVIC_EnableIRQ(ETH_IRQn);
    ETH->DMAIER |= ETH_DMAIER_NISE | ETH_DMAIER_TIE | ETH_DMAIER_RIE;

    /* enable */
    ETH->MACCR |= ETH_MACCR_TE;
    ETH->DMAOMR |= ETH_DMAOMR_FTF;
    ETH->MACCR |= ETH_MACCR_RE;

    ETH->DMAOMR |= ETH_DMAOMR_ST;
    ETH->DMAOMR |= ETH_DMAOMR_SR;

    /* configure speed, do it at the end so the PHY had time to 
     * reset */
    eth_phy_write(eth_config.phy_addr, PHY_BMCR, eth_config.speed);

    return 0;
}

int eth_send(const char *data, unsigned len)
{
    DMA_Stream_TypeDef *stream = dma_stream(eth_config.dma_stream);
    unsigned copy, count, sent = -1;
    edma_desc_t *first = tx_curr;
    edma_desc_t *last = tx_curr;

    count = len / ETH_TX_BUFFER_SIZE;
    count += (len - (count * ETH_TX_BUFFER_SIZE) > 0) ? 1 : 0;

    /* safety check */
    if (count > ETH_TX_BUFFER_COUNT) {
        return -1;
    }
    while (count--) {
        /* block until there's an available descriptor */
        while (tx_curr->status & DESC_OWN) {
            DEBUG("stm32_eth: not avail\n");
        }

        /* clear status field */
        tx_curr->status &= 0x0fffffff;

        /* copy buffer */
        copy = ((len < ETH_TX_BUFFER_SIZE) ? len : ETH_TX_BUFFER_SIZE);
        stream->PAR = (uint32_t)data;
        stream->M0AR = (uint32_t)tx_curr->buffer_addr;
        stream->NDTR = (uint16_t)copy;
        stream->CR |= DMA_SxCR_EN;
        mutex_lock(&_dma_sync);

        tx_curr->control = (copy & 0x1fff);
        len -= copy;
        sent += copy;

        /* update pointers */
        last = tx_curr;
        tx_curr = tx_curr->desc_next;
    }

    /* set flags for first and last frames */
    first->status |= TX_DESC_FS;
    last->status |= TX_DESC_LS | TX_DESC_IC;

    /* give the descriptors to the DMA */
    while (first != tx_curr) {
        first->status |= DESC_OWN;
        first = first->desc_next;
    }

    /* start tx */
    ETH->DMATPDR = 0;
    return sent;
}

static int _try_receive(char *data, int max_len, int block)
{
    int copy, len = 0;
    int copied = 0;
    int drop = (data || max_len > 0);
    edma_desc_t *p = rx_curr;
    for (int i = 0; i < ETH_RX_BUFFER_COUNT && len == 0; i++) {
        /* try receiving, if the block is set, simply wait for the rest of
         * the packet to complete, otherwise just break */
        while (p->status & DESC_OWN) {
            if (block) {
            }
            else {
                break;
            }
        }

        /* amount of data to copy */
        copy = ETH_RX_BUFFER_SIZE;
        if (p->status & (RX_DESC_LS | RX_DESC_FL)) {
            len = ((p->status >> 16) & 0x3FFF) - 4;
            copy = len - copied;
        }

        if (drop) {
            /* copy the data if possible */
            if (data && max_len >= copy) {
                memcpy(data, p->buffer_addr, copy);
                max_len -= copy;
            }
            else if (max_len < copy) {
                len = -1;
            }
            p->status = DESC_OWN;
        }
        p = p->desc_next;
    }

    if (drop) {
        rx_curr = p;
    }

    return len;
}

int eth_try_receive(char *data, unsigned max_len)
{
    return _try_receive(data, max_len, 0);
}

int eth_receive_blocking(char *data, unsigned max_len)
{
    return _try_receive(data, max_len, 1);
}

int get_rx_status_owned(void)
{
    return (!(rx_curr->status & DESC_OWN));
}

void isr_eth_wkup(void)
{
    cortexm_isr_end();
}

void ETH_DMA_ISR(void)
{
    /* clear DMA done flag */
    int stream = eth_config.dma_stream;
    int sel = dma_hl(stream);
    if(sel == 0) {
      dma_base(stream)->LIFCR = dma_ifc(stream);
    } else {
      dma_base(stream)->HIFCR = dma_ifc(stream);
    }
    mutex_unlock(&_dma_sync);
    cortexm_isr_end();
}

#endif

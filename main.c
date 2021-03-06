#include <stdlib.h>
#include <stdint.h>

//#include "pic.h"
#include "p32mx250f128b.h"

const uint32_t __attribute__((section (".devcfg0"))) devcfg0 = 0xffffffff;
const uint32_t __attribute__((section (".devcfg1"))) devcfg1 = 0xff7f3eff;
const uint32_t __attribute__((section (".devcfg2"))) devcfg2 = 0xffff7dfd;
const uint32_t __attribute__((section (".devcfg3"))) devcfg3 = 0xffffffff;

void idle() {

}

void send(uint8_t c) {
    while((U2STA & (1<<9)) != 0) idle();
    U2TXREG = c;
}

uint8_t recv() {
    while((U2STA & 1) == 0) idle();
    return U2RXREG;
}

uint8_t parse4(char c) {
    if(c >= '0' && c <= '9') return (c-'0');
    else if(c >= 'a' && c <= 'f') return (c-'a'+10);
    else if(c >= 'A' && c <= 'F') return (c-'a'+10);
    return 0;
}

uint32_t parse32(const char *p) {
    uint32_t ret = 0;
    int i;
    for(i = 0; i < 8; i ++) {
        ret <<= 4;
        ret |= parse4(p[i]);
    }
    return ret;
}

void format8(char *p, uint8_t v) {
    const char *hex = "0123456789abcdef";
    p[0] = hex[v >> 4];
    p[1] = hex[v & 0xf];
}

void format32(char *p, uint32_t v) {
    const char *hex = "0123456789abcdef";
    int i;
    for(i = 0; i < 8; i ++) {
        p[i] = hex[v >> 28];
        v <<= 4;
    }
}

void perform_flash_op(uint8_t op) {
    asm volatile("di");
    NVMCON = (op << 0) | (1<<14);
    int i;
    for(i = 0; i < 1000; i ++) ;

    __asm__ __volatile__(
        "li $t0, 0xbf80f410 \n" // NVMKEY
        "li $t1, 0xbf80f408 \n" // NVMCONSET 
        "li $t2, 0xaa996655 \n"
        "li $t3, 0x556699aa \n"
        "li $t4, 0x8000 \n"
        "sw $t2, 0($t0) \n"
        "sw $t3, 0($t0) \n"
        "sw $t4, 0($t1) \n"
        : : : "t0", "t1", "t2", "t3", "t4");

    while(NVMCON & 0x8000) ;
    
    NVMCONCLR = (1<<14);
    asm volatile("ei");
}

void software_reset() {
    SYSKEY = 0x0; // write invalid key to force lock
    SYSKEY = 0xAA996655; // Write Key1 to SYSKEY
    SYSKEY = 0x556699AA; // Write Key2 to SYSKEY

    RSWRSTSET = 1;

    volatile uint32_t *p = &RSWRST;
    unsigned int x = *p;

    while(1) {
        __asm__ __volatile__("nop" : : "r"(x));
    }
}

void entry() {
    uint32_t rcon_value = RCON;
    /* clear relevant bits in RCON */
    RCONCLR = 0x83;

    asm volatile("di");
    // unlock OSCCON
    SYSKEY = 0x0; // write invalid key to force lock
    SYSKEY = 0xAA996655; // Write Key1 to SYSKEY
    SYSKEY = 0x556699AA; // Write Key2 to SYSKEY

    uint32_t con = OSCCON;

    // clear NOSC
    con &= ~(7 << 8);
    // set NOSC: internal RC, no PLL, no divider
    con |= (0 << 8);
    // clear PBDIV
    con &= ~(3 << 19);
    // set PBDIV: divide 8MHz by 2 for 4MHz peripheral clock
    con |= (1 << 19);

    OSCCON = con;

    OSCCON |= 1; // enable switch

    while(OSCCON & 1) ;

    SYSKEY = 0x0; // write invalid key to force lock

    asm volatile("ei");

#if 0
    // start by marking all three as digital pins
    ANSELACLR = 1;
    //ANSELBCLR = (1<<13) | (1<<15);
    // mark U1RX (RB13) as input
    TRISBSET = (1<<13);
    // mark U1TX (RB15) as output
    TRISBCLR = (1<<15);
    // Set up PPS for U1
    U1RXR = 0b0011; // RPB13 to U1RX
    RPB15R = 0b0001; // RPB15 to U1TX
    // Initialize U1
    // Set baud rate to 38400 baud
    U1BRG = 38;
    // Set U1MODE to enable (15), (simplex mode 11)
    U1MODE = (1<<11);
    U1MODE |= (1<<15);
    
    // Enable transmission (10) and receiving (12)
    U1STA = (1<<12) | (1<<10);
#endif
    // U2 setup
    ANSELACLR = 1<<1;
    ANSELBCLR = 1<<0;

    // mark U2RX (RA1) as input
    TRISASET = (1<<1);
    // mark U2TX (RB0) as output
    TRISBCLR = (1<<0);
    // set up PPS for U1
    U2RXR = 0b0000;
    RPB0R = 0b0010;
    // Initialize U2
    // Set baud rate to 19200 baud
    U2BRG = 12;
    // Set U2MODE to enable, simplex
    U2MODEbits.RTSMD = 1; // simplex
    U2MODEbits.ON = 1;

    // Enable transmission and receiving
    U2STAbits.URXEN = 1;
    U2STAbits.UTXEN = 1;

    // LED setup
    ANSELACLR = 1;
    TRISACLR = 1;

    enum STATE_TYPE {
        INITIAL_STATE,
        WAITING_STATE,
        BUFFER_READ_STATE,
        BUFFER_WRITE_STATE,
        READ_STATE,
        READ_BYTE_STATE,
        WRITE_WORD_STATE,
        WRITE_ROW_STATE,
        ERASE_PAGE_STATE,
        POKE_WORD_STATE
    } STATE = INITIAL_STATE;
    
    uint8_t buffer[1024 + 8];
    int buffer_offset, buffer_length;
    
    enum STATE_TYPE after_state;
    
    // disable intererupts
    __asm__ volatile("di");

    /* send initial query */
    send('>');
    /* wait for initial message */
    int i;
    for(i = 2000000; i > 0; i --) {
        if((U2STA & 1) != 0) break;
    }

    /* did we time out? if so, reset and go into target code if possible. */
    if(i == 0) software_reset();

    while(1) {
        switch(STATE) {
            case INITIAL_STATE: {
                send(10);
                send(13);
                send('?');
                STATE = WAITING_STATE;
                break;
            }
            case WAITING_STATE: {
                uint8_t cmd = recv();
                if(cmd == 'r') {
                    buffer_offset = 0;
                    buffer_length = 8;
                    STATE = BUFFER_READ_STATE;
                    after_state = READ_STATE;
                }
                else if(cmd == 'y') {
                    buffer_offset = 0;
                    buffer_length = 8;
                    STATE = BUFFER_READ_STATE;
                    after_state = READ_BYTE_STATE;
                }
                else if(cmd == 'w') {
                    buffer_offset = 0;
                    buffer_length = 16;
                    STATE = BUFFER_READ_STATE;
                    after_state = WRITE_WORD_STATE;
                }
                else if(cmd == 'o') {
                    buffer_offset = 0;
                    buffer_length = 256 + 8; // 128 bytes of data, 8 bytes of address
                    STATE = BUFFER_READ_STATE;
                    after_state = WRITE_ROW_STATE;
                }
                else if(cmd == 'e') {
                    buffer_offset = 0;
                    buffer_length = 8;
                    STATE = BUFFER_READ_STATE;
                    after_state = ERASE_PAGE_STATE;
                }
                else if(cmd == 'b') {
                    software_reset();
                }
                else if(cmd == 'p') {
                    buffer_offset = 0;
                    buffer_length = 16;
                    STATE = BUFFER_READ_STATE;
                    after_state = POKE_WORD_STATE;
                }
                else {
                    send('!');
                    STATE = INITIAL_STATE;
                }

                break;
            }
            case BUFFER_READ_STATE: {
                if(buffer_offset == buffer_length) STATE = after_state;
                else {
                    buffer[buffer_offset] = recv();
                    buffer_offset ++;
                }
                break;
            }
            case BUFFER_WRITE_STATE: {
                if(buffer_offset == buffer_length) STATE = after_state;
                else {
                    send(buffer[buffer_offset]);
                    buffer_offset ++;
                }
                break;
            }
            case READ_STATE: {
                uint32_t address = parse32(buffer);
                format32(buffer, *(uint32_t *)address);
                after_state = INITIAL_STATE;
                buffer_offset = 0;
                buffer_length = 8;
                STATE = BUFFER_WRITE_STATE;
                break;
            }
            case READ_BYTE_STATE: {
                uint32_t address = parse32(buffer);
                format8(buffer, *(uint8_t *)address);
                after_state = INITIAL_STATE;
                buffer_offset = 0;
                buffer_length = 2;
                STATE = BUFFER_WRITE_STATE;
                break;
            }
            case WRITE_WORD_STATE: {
                uint32_t address = parse32(buffer);
                uint32_t value = parse32(buffer + 8);
                
                // convert virtual address to physical address
                if(address & 0xa0000000) address &= ~0xa0000000;
                
                NVMADDR = address;
                NVMDATA = value;
                
                perform_flash_op(0x1);
                
                if(NVMCON & 0x3000) {
                    send('e');
                    if(NVMCON & 0x1000) send('L');
                }
                else send('.');
                
                STATE = INITIAL_STATE;
                break;
            }
            case WRITE_ROW_STATE: {
                uint32_t address = parse32(buffer);
                int i;
                for(i = 0; i < 128; i ++) {
                    buffer[i] = parse4(buffer[8 + 2*i]) << 4;
                    buffer[i] |= parse4(buffer[8 + 2*i + 1]);
                }
                
                // convert virtual address to physical address
                if(address & 0xa0000000) address &= ~0xa0000000;
                
                NVMADDR = address;
                NVMSRCADDR = (uint32_t)buffer - 0xa0000000;
                
                perform_flash_op(0x3);
                
                if(NVMCON & 0x3000) {
                    send('e');
                    if(NVMCON & 0x1000) send('L');
                }
                else send('.');
                
                STATE = INITIAL_STATE;
                break;
            }
            case ERASE_PAGE_STATE: {
                uint32_t address = parse32(buffer);
                
                // convert virtual address to physical address
                if(address & 0xa0000000) address &= ~0xa0000000;
                
                NVMADDR = address;
                
                perform_flash_op(0x4);
                
                if(NVMCON & 0x3000) {
                    send('e');
                    if(NVMCON & 0x1000) send('L');
                }
                else send('.');
                
                STATE = INITIAL_STATE;
                
                break;
            }
            case POKE_WORD_STATE: {
                uint32_t address = parse32(buffer);
                uint32_t value = parse32(buffer + 8);
                
                *(uint32_t *)(address) = value;
                
                STATE = INITIAL_STATE;
                break;
            }
        }
    }
}

void nmi_handler() {
    
}

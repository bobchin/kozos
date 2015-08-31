#include "defines.h"
#include "serial.h"
#include "lib.h"

static int init(void)
{
    /* can use the symbol defined at "ld.scr"    */
    extern int erodata, data_start, edata, bss_start, ebss;

    /* initialize "data" and "bss" */
    memcpy(&data_start, &erodata, (long)&edata - (long)&data_start);
    memset(&bss_start, 0, (long)&ebss - (long)&bss_start);

    /* initialize serial */
    serial_init(SERIAL_DEFAULT_DEVICE);

    return 0;
}

int main(void)
{
    serial_init(SERIAL_DEFAULT_DEVICE);

    puts("Hello World!\n");

    putxval(0x10, 0);   puts("\n");
    putxval(0xffff, 0); puts("\n");

    while (1)
        ;

    return 0;
}


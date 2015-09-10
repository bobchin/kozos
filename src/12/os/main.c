#include "defines.h"
#include "kozos.h"
#include "interrupt.h"
#include "lib.h"

/* システムタスクとユーザタスクの起動 */
static int start_threads(int argc, char *argv[])
{
  kz_run(consdrv_main, "consdrv", 1, 0x200, 0, NULL);
  kz_run(command_main, "command", 8, 0x200, 0, NULL);

  /* 優先順位を下げて、アイドルスレッドに移行する */
  kz_chpri(15);
  /* 割り込み有効にする */
  INTR_ENABLE;
  /* 省電力モードに移行 */
  while (1) {
    asm volatile ("sleep");
  }

  return 0;
}

int main(void)
{
  INTR_DISABLE;

  puts("kozos boot succeed!\n");

  /* OS の動作開始 */
  kz_start(start_threads, "idle", 0, 0x100, 0, NULL);
  /* ここには戻ってこない */

  return 0;
}

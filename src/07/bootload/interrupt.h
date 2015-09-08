#ifndef _INTERRUPT_H_INCLUDED_
#define _INTERRUPT_H_INCLUDED_

extern char softvec;
#define SOFTVEC_ADDR (&softvec)

/*
 * ソフトウェア割り込みベクタの種類
 *（わかりやすいようにsoftvec_type_t型とする）
 */
typedef short softvec_type_t;

/*
 * 割り込みハンドラの型の定義
 * （割り込みハンドラ（関数）をsoftvec_handler_t型とする）
 */
typedef void (*softvec_handler_t)(softvec_type_t type, unsigned long sp);

/* ソフトウェア割り込みベクタの位置 */
#define SOFTVECS ((softvec_handler_t *)SOFTVEC_ADDR)

/*
 * 割り込み有効化・無効化
 * インラインアセンブラで記述している。
 * やりたいことはCCR（コンディションコードレジスタ）の
 * Iビット(7ビット)とUIビット(6ビット)を操作したい。
 * 割り込み有効時は上位2ビットを0に、無効時は1にする。
 * andc,orc はCPU命令
 */
#define INTR_ENABLE  asm volatile ("andc.b #0x3f,ccr")
#define INTR_DISABLE asm volatile ("orc.b #0xc0,ccr")

/* ソフトウェア割り込みベクタの初期化 */
int softvec_init(void);

/* ソフトウェア割り込みベクタの設定 */
int softvec_setintr(softvec_type_t type, softvec_handler_t handler);

/* 共通割り込みハンドラ */
void interrupt(softvec_type_t type, unsigned long sp);

#endif

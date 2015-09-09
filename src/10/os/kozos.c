#include "defines.h"
#include "kozos.h"
#include "intr.h"
#include "interrupt.h"
#include "syscall.h"
#include "lib.h"

/*******************************
 * OS の本体
 *  スレッドの管理
 *  システムコールの受付
 *  割り込み処理
 *******************************/

#define  THREAD_NUM 6
#define PRIORITY_NUM 16
#define  THREAD_NAME_SIZE 15

/*
 * スレッドコンテキスト
 *
 * スレッドの実行・中断をするのに保存が必要なCPUの情報。
 * 汎用レジスタはスタックに格納されるので
 * スタックポインタのみ保持する
 */
typedef struct _kz_context {
  uint32 sp;
} kz_context;

/*
 * タスクコントロールブロック
 *
 * 各タスクの情報を格納する領域
 */
typedef struct _kz_thread {
  struct _kz_thread *next;
  char name[THREAD_NAME_SIZE + 1]; /* スレッド名 */
  int priority;                    /* 優先度 */
  char *stack;                     /* スタック */
  uint32 flags;                    /* 各種フラグ */
  #define KZ_THREAD_FLAG_READY (1 << 0)

   /* スレッド起動時のパラメータ */
  struct {
    kz_func_t func;
    int argc;
    char **argv;
  } init;

  /* システムコール呼び出し時のパラメータ */
  struct {
    kz_syscall_type_t type;
    kz_syscall_param_t *param;
  } syscall;

  /* スレッドのコンテキスト情報の格納領域 */
  kz_context context;
} kz_thread;

/* スレッドのレディキュー */
static struct {
  kz_thread *head;
  kz_thread *tail;
} readyque[PRIORITY_NUM];

/* カレントスレッド */
static kz_thread *current;

/* タスクコントロールブロックのリスト */
static kz_thread threads[THREAD_NUM];

/* 割り込みハンドラのリスト */
static kz_handler_t handlers[SOFTVEC_TYPE_NUM];

void dispatch(kz_context *context);

/* カレントスレッドをレディキューから抜き出す */
static int getcurrent(void)
{
  if (current == NULL)
    return -1;

  if (!(current->flags & KZ_THREAD_FLAG_READY)) {
    /* すでにレディ状態でない場合は無視 */
    return 1;
  }

  /* カレントスレッドは必ず先頭にあるはずなので、先頭から抜き出す */
  readyque[current->priority].head = current->next;
  if (readyque[current->priority].head == NULL) {
    readyque[current->priority].tail = NULL;
  }
  current->flags &= ~KZ_THREAD_FLAG_READY;
  current->next = NULL;

  return 0;
}

/* カレントスレッドをレディキューに繋げる */
static int putcurrent(void)
{
  if (current == NULL) {
    return -1;
  }

  if (current->flags & KZ_THREAD_FLAG_READY) {
    /* すでにレディ状態の場合は無視 */
    return 1;
  }

  /* レディキューの末尾に接続する */
  if (readyque[current->priority].tail) {
    readyque[current->priority].tail->next = current;
  } else {
    readyque[current->priority].head = current;
  }
  readyque[current->priority].tail = current;
  current->flags |= KZ_THREAD_FLAG_READY;

  return 0;
}

/* スレッドの終了 */
static void thread_end(void)
{
  kz_exit();
}

/* スレッドのスタートアップ */
static void thread_init(kz_thread *thp)
{
  thp->init.func(thp->init.argc, thp->init.argv);
  thread_end();
}

/*
 * システムコールの処理（kz_run():スレッドの起動）
 *
 * ユーザスタックに指定した関数を登録しておき、
 * dispatch()を実行した時に、その関数が呼ばれるようにする。
 *（実際はthread_init()で実行）
 * 実行後はthread_end()が実行され、kz_exit()が実行される。
 * スレッド実行されるようにレディキューに追加される。
 */
static kz_thread_id_t thread_run(kz_func_t func, char *name, int priority,
                                 int stacksize, int argc, char *argv[])
{
  int i;
  kz_thread *thp;
  uint32 *sp;
  extern char userstack;
  static char *thread_stack = &userstack;

  /* 開いているタスクコントロールブロックを検索 */
  for (i = 0; i < THREAD_NUM; i++) {
    thp = &threads[i];
    if (!thp->init.func)
      break;
  }
  if (i == THREAD_NUM)
    return -1;

  /* タスクコントロールブロックをゼロクリア */
  memset(thp, 0, sizeof(*thp));

  /* タスクコントロールブロックの設定 */
  strcpy(thp->name, name);
  thp->next = NULL;
  thp->priority  = priority;
  thp->flags     = 0;

  thp->init.func = func;
  thp->init.argc = argc;
  thp->init.argv = argv;

  /* スタック領域を獲得 */
  memset(thread_stack, 0, stacksize);
  thread_stack += stacksize;

  thp->stack = thread_stack;

  /* スタックの初期化 */
  sp = (uint32 *)thp->stack;
  *(--sp) = (uint32)thread_end;

  /*
   * プログラムカウンタを設定する
   * スレッドの優先度がゼロの場合には割り込み禁止スレッドとする
   */
  *(--sp) = (uint32)thread_init | ((uint32)(priority ? 0 : 0xc0) << 24);

  *(--sp) = 0; /* ER6 */
  *(--sp) = 0; /* ER5 */
  *(--sp) = 0; /* ER4 */
  *(--sp) = 0; /* ER3 */
  *(--sp) = 0; /* ER2 */
  *(--sp) = 0; /* ER1 */

  /* スレッドのスタートアップ(thread_init())に渡す引数 */
  *(--sp) = (uint32)thp; /* ER0 */

  /* スレッドのコンテキストを設定 */
  thp->context.sp = (uint32)sp;

  /* システムコールを呼び出したスレッドをレディキューに戻す */
  putcurrent();

  /* 新規作成したスレッドをレディキューに接続する */
  current = thp;
  putcurrent();

  return (kz_thread_id_t)current;
}

/* システムコールの処理(kz_exit():スレッドの終了) */
static int thread_exit(void)
{
  puts(current->name);
  puts(" EXIT.\n");
  memset(current, 0, sizeof(*current));
  return 0;
}

/* システムコールの処理(kz_wait(): スレッドの実行権放棄) */
static int thread_wait(void)
{
  putcurrent();
  return 0;
}

/* システムコールの処理(kz_sleep(): スレッドのスリープ) */
static int thread_sleep(void)
{
  return 0;
}

/* システムコールの処理(kz_wakeup(): スレッドのウェイクアップ) */
static int thread_wakeup(kz_thread_id_t id)
{
  /* ウェイクアップを呼び出したスレッドをレディキューに戻す */
  putcurrent();

  /* 指定されたスレッドをレディキューに接続してウェイクアップする */
  current = (kz_thread *)id;
  putcurrent();

  return 0;
}

/* システムコールの処理(kz_geid(): スレッドIDの取得) */
static kz_thread_id_t thread_getid(void)
{
  putcurrent();
  return (kz_thread_id_t) current;
}

/* システムコールの処理(kz_chpri(): スレッドの優先度の変更) */
static int thread_chpri(int priority)
{
  int old = current->priority;
  if (priority >= 0)
    current->priority = priority;

  /* 新しい優先度のレディキューに繋ぎ直す */
  putcurrent();
  return old;
}

/* 割り込みハンドラの登録 */
static int setintr(softvec_type_t type, kz_handler_t handler)
{
  static void thread_intr(softvec_type_t type, unsigned long sp);
  softvec_setintr(type, thread_intr);

  handlers[type] = handler;

  return 0;
}

/* システムコールの呼び出し */
static void call_functions(kz_syscall_type_t type, kz_syscall_param_t *p)
{
  switch (type) {
    /* kz_run() */
    case KZ_SYSCALL_TYPE_RUN:
      p->un.run.ret = thread_run(p->un.run.func, p->un.run.name,
                                 p->un.run.priority, p->un.run.stacksize,
                                 p->un.run.argc, p->un.run.argv);
      break;

    /* kz_exit() */
    case KZ_SYSCALL_TYPE_EXIT:
      thread_exit();
      break;

    /* kz_wait() */
    case KZ_SYSCALL_TYPE_WAIT:
      p->un.wait.ret = thread_wait();
      break;

    /* kz_sleep() */
    case KZ_SYSCALL_TYPE_SLEEP:
      p->un.sleep.ret = thread_sleep();
      break;

    /* kz_wakeup() */
    case KZ_SYSCALL_TYPE_WAKEUP:
      p->un.wakeup.ret = thread_wakeup(p->un.wakeup.id);
      break;

    /* kz_getid() */
    case KZ_SYSCALL_TYPE_GETID:
      p->un.getid.ret = thread_getid();
      break;

    /* kz_chpri() */
    case KZ_SYSCALL_TYPE_CHPRI:
      p->un.chpri.ret = thread_chpri(p->un.chpri.priority);
      break;

    default:
      break;
  }
}

/* システムコールの処理 */
static void syscall_proc(kz_syscall_type_t type, kz_syscall_param_t *p)
{
  getcurrent();
  call_functions(type, p);
}

/* スレッドのスケジューリング */
static void schedule(void)
{
  int i;

  /*
   * 優先度の高い順（優先度の数値の小さい順）にレディキューをみて
   * 動作可能なスレッドを検索する
   */
  for (i = 0; i < PRIORITY_NUM; i++) {
    if (readyque[i].head)
      break;
  }

  /* 見つからなかった */
  if (i == PRIORITY_NUM)
    kz_sysdown();

  current = readyque[i].head;
}

/* システムコール割り込み呼び出し */
static void syscall_intr(void)
{
  syscall_proc(current->syscall.type, current->syscall.param);
}

/* ソフトウェアエラー割り込みの呼び出し */
static void softerr_intr(void)
{
  puts(current->name);
  puts(" DOWN.\n");
  getcurrent();
  thread_exit();
}

/*
 * 割り込み処理の入り口関数
 *
 * ソフトウェア割り込みを一括で処理するハンドラ
 * 実際に実行する処理はhandlersに格納しておく
 */
static void thread_intr(softvec_type_t type, unsigned long sp)
{
  /* カレントスレッドのコンテキストを保存する */
  current->context.sp = sp;

  /*
   * 割り込みごとの処理を実行する
   * SOFTVEC_TYPE_SYSCALL => syscall_intr()
   * SOFTVEC_TYPE_SOFTERR => softerr_intr()
   */
  if (handlers[type])
    handlers[type]();

  /* スレッドのスケジューリング */
  schedule();

  /*
   * スレッドのディスパッチ
   * start.sで定義
   */
  dispatch(&current->context);

  /* ここには返ってこない */
}

/* 初期スレッドの起動 */
void kz_start(kz_func_t func, char *name, int priority, int stacksize, int argc, char *argv[])
{
  current = NULL;

  memset(readyque, 0, sizeof(readyque));
  memset(threads, 0, sizeof(threads));
  memset(handlers, 0, sizeof(handlers));

  setintr(SOFTVEC_TYPE_SYSCALL, syscall_intr);
  setintr(SOFTVEC_TYPE_SOFTERR, softerr_intr);

  /*
   * システムコール発行不可なので直接関数を呼び出してスレッド作成する
   */
  current = (kz_thread *)thread_run(func, name, priority, stacksize, argc, argv);

  /*
   * 渡されたスタックポインタをもとに実行される＝上で登録したcurrentが実行される
   */
  dispatch(&current->context);

  /* ここには返ってこない */
}

/* OS内部で致命的なエラーが発生したときにこの関数を実行する */
void kz_sysdown(void)
{
  puts("system error!\n");
  while (1)
    ;
}

/* システムコール呼び出し用ライブラリ関数 */
void kz_syscall(kz_syscall_type_t type, kz_syscall_param_t *param)
{
  current->syscall.type  = type;
  current->syscall.param = param;

  /*
   * トラップ命令実行
   * vector.cよりintr_syscallが発生
   */
  asm volatile ("trapa #0");
}

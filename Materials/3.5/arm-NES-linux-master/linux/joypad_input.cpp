// joypad_input.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdint.h>

#define UP_MASK     (1<<0)
#define DOWN_MASK   (1<<1)
#define LEFT_MASK   (1<<2)
#define RIGHT_MASK  (1<<3)
#define A_MASK      (1<<4)
#define B_MASK      (1<<5)
#define START_MASK  (1<<6)
#define SELECT_MASK (1<<7)

/* ---------- 框架保持与原工程一致 ---------- */
typedef struct JoypadInput{
    int (*DevInit)(void);
    int (*DevExit)(void);
    int (*GetJoypad)(void);
    struct JoypadInput *ptNext;
    pthread_t tTreadID;
} T_JoypadInput, *PT_JoypadInput;

static unsigned char g_InputEvent;
static pthread_mutex_t g_tMutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_tConVar = PTHREAD_COND_INITIALIZER;
static PT_JoypadInput g_ptJoypadInputHead = NULL;

/* ---------- 键盘扫描与读取 ---------- */
static int keyboard_fd = -1;
static char keyboard_dev_path[256] = {0};

static int test_bit(int bit, const unsigned long *array)
{
    return (array[bit/ (8*sizeof(unsigned long))] >> (bit % (8*sizeof(unsigned long)))) & 1;
}

static void dump_bits(const char *tag, const unsigned long *bits, int maxbit)
{
    printf("[DEBUG] %s:", tag);
    for (int i = 0; i <= maxbit; ++i)
        if (test_bit(i, bits)) printf(" %d", i);
    printf("\n");
}

static int looks_like_keyboard(int fd, char *name_buf, size_t name_len, int *has_rel, int *has_abs, int *has_key_a)
{
    unsigned long ev_bits[(EV_MAX+1) / (8*sizeof(unsigned long)) + 1];
    unsigned long key_bits[(KEY_MAX+1)/(8*sizeof(unsigned long)) + 1];
    unsigned long rel_bits[(REL_MAX+1)/(8*sizeof(unsigned long)) + 1];
    unsigned long abs_bits[(ABS_MAX+1)/(8*sizeof(unsigned long)) + 1];
    memset(ev_bits,0,sizeof(ev_bits));
    memset(key_bits,0,sizeof(key_bits));
    memset(rel_bits,0,sizeof(rel_bits));
    memset(abs_bits,0,sizeof(abs_bits));

    if (ioctl(fd, EVIOCGNAME(name_len), name_buf) < 0) {
        snprintf(name_buf, name_len, "unknown");
    }

    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) return 0;

    int ev_key = test_bit(EV_KEY, ev_bits);
    int ev_rel = test_bit(EV_REL, ev_bits);
    int ev_abs = test_bit(EV_ABS, ev_bits);

    if (has_rel) *has_rel = ev_rel;
    if (has_abs) *has_abs = ev_abs;

    if (!ev_key) return 0;

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) return 0;
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits) < 0) memset(rel_bits,0,sizeof(rel_bits));
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) memset(abs_bits,0,sizeof(abs_bits));

    int hasA = test_bit(KEY_A, key_bits);
    if (has_key_a) *has_key_a = hasA;

    /* 调试输出（如太吵可注释） */
    printf("[DEBUG] name=\"%s\" ev:key=%d rel=%d abs=%d has KEY_A=%d\n",
           name_buf, ev_key, ev_rel, ev_abs, hasA);

    /* 进一步加分项：名字里含 Keyboard */
    int name_keyboard = (strcasestr(name_buf, "keyboard") != NULL);

    /* 规则：必须能产生标准键（含 KEY_A），且最好不是相对/绝对坐标设备 */
    return (hasA || name_keyboard);
}

static int open_event_force(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("[DEBUG] open forced event");
        return -1;
    }
    strncpy(keyboard_dev_path, path, sizeof(keyboard_dev_path)-1);
    keyboard_dev_path[sizeof(keyboard_dev_path)-1] = 0;
    printf("[DEBUG] Forced event opened: %s\n", keyboard_dev_path);
    return fd;
}

static int findKeyboardDevice(char *devPath, size_t len)
{
    /* 优先：允许用户强制指定 */
    const char *force = getenv("INPUT_EVENT");
    if (force && strlen(force) > 0) {
        int fd = open(force, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            close(fd);
            strncpy(devPath, force, len-1);
            devPath[len-1] = 0;
            printf("[DEBUG] Using forced INPUT_EVENT=%s\n", devPath);
            return 0;
        } else {
            fprintf(stderr, "[WARN] INPUT_EVENT=%s open failed: %s\n", force, strerror(errno));
        }
    }

    DIR *dir = opendir("/dev/input");
    if (!dir) {
        perror("opendir /dev/input");
        return -1;
    }

    /* 先收集候选，再评分选择最优 */
    char best_path[256] = {0};
    int best_score = -1;

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char path[256];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        char name[256] = {0};
        int has_rel=0, has_abs=0, has_key_a=0;
        int kbd_like = looks_like_keyboard(fd, name, sizeof(name), &has_rel, &has_abs, &has_key_a);

        /* 给个评分：
           +1000 如果名字里含Keyboard
           +100  如果含KEY_A
           -200  如果有EV_REL
           -200  如果有EV_ABS
         */
        int score = -100000;
        if (kbd_like) {
            score = 0;
            if (strcasestr(name, "keyboard")) score += 1000;
            if (has_key_a) score += 100;
            if (has_rel) score -= 200;
            if (has_abs) score -= 200;
        }

        printf("[DEBUG] candidate %s name=\"%s\" score=%d\n", path, name, score);

        if (score > best_score) {
            best_score = score;
            strncpy(best_path, path, sizeof(best_path)-1);
        }

        close(fd);
    }
    closedir(dir);

    if (best_score < 0) {
        fprintf(stderr, "[ERROR] No suitable keyboard-like event device found.\n");
        return -1;
    }

    strncpy(devPath, best_path, len-1);
    devPath[len-1] = 0;
    printf("[DEBUG] Selected keyboard device: %s (score=%d)\n", devPath, best_score);
    return 0;
}

static int usbKeyboardDevInit(void)
{
    if (findKeyboardDevice(keyboard_dev_path, sizeof(keyboard_dev_path)) < 0) {
        fprintf(stderr, "No keyboard device found!\n");
        return -1;
    }

    keyboard_fd = open(keyboard_dev_path, O_RDONLY | O_NONBLOCK);
    if (keyboard_fd < 0) {
        perror("open keyboard device");
        return -1;
    }

    /* 可选：独占抓取，避免字符被终端吃掉（必要时打开） */
    // if (ioctl(keyboard_fd, EVIOCGRAB, 1) < 0) {
    //     perror("[WARN] EVIOCGRAB");
    // }

    /* 打印最终确认信息 */
    char name[256] = {0};
    ioctl(keyboard_fd, EVIOCGNAME(sizeof(name)), name);
    printf("[DEBUG] Keyboard device opened: %s, name=\"%s\"\n", keyboard_dev_path, name[0]?name:"unknown");

    /* 再打印能力位，帮助诊断 */
    unsigned long ev_bits[(EV_MAX+1) / (8*sizeof(unsigned long)) + 1] = {0};
    unsigned long key_bits[(KEY_MAX+1)/(8*sizeof(unsigned long)) + 1] = {0};
    if (ioctl(keyboard_fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) == 0) {
        dump_bits("EV bits", ev_bits, EV_MAX);
    }
    if (ioctl(keyboard_fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) == 0) {
        printf("[DEBUG] KEY_A=%d KEY_W=%d KEY_S=%d KEY_A(left)=%d KEY_D=%d J=%d K=%d U=%d I=%d\n",
               test_bit(KEY_A, key_bits),
               test_bit(KEY_W, key_bits),
               test_bit(KEY_S, key_bits),
               test_bit(KEY_A, key_bits),
               test_bit(KEY_D, key_bits),
               test_bit(KEY_J, key_bits),
               test_bit(KEY_K, key_bits),
               test_bit(KEY_U, key_bits),
               test_bit(KEY_I, key_bits));
    }

    return 0;
}

static int usbKeyboardDevExit(void)
{
    if(keyboard_fd >= 0) {
        // ioctl(keyboard_fd, EVIOCGRAB, 0); // 若之前抓取，这里释放
        close(keyboard_fd);
        keyboard_fd = -1;
    }
    printf("[DEBUG] Keyboard device closed\n");
    return 0;
}

/* 可同时支持两套映射：WASD / 方向键；J/K 或 Z/X；U/I 或 Enter/Space */
static int usbKeyboardGet(void)
{
    static unsigned char joypad = 0;
    struct input_event ev;
    ssize_t n;

    while ((n = read(keyboard_fd, &ev, sizeof(ev))) == sizeof(ev)) {
        if (ev.type == EV_SYN) continue;
        if (ev.type != EV_KEY) continue;

        int pressed = (ev.value != 0);  // 1=down, 0=up（某些设备2=autorepeat）
        if (ev.value == 2) pressed = 1; // 将重复也视作按下

        switch (ev.code) {
            /* 方向：WASD & 方向键 */
            case KEY_W:
            case KEY_UP:
                joypad = pressed ? (joypad | UP_MASK) : (joypad & ~UP_MASK); break;
            case KEY_S:
            case KEY_DOWN:
                joypad = pressed ? (joypad | DOWN_MASK) : (joypad & ~DOWN_MASK); break;
            case KEY_A: /* 左字母A 与 左方向区有冲突？注意 KEY_A(字母A) 与 KEY_LEFT(方向键左) 是不同code */
            case KEY_LEFT:
                joypad = pressed ? (joypad | LEFT_MASK) : (joypad & ~LEFT_MASK); break;
            case KEY_D:
            case KEY_RIGHT:
                joypad = pressed ? (joypad | RIGHT_MASK) : (joypad & ~RIGHT_MASK); break;

            /* A/B：J/K 或 Z/X 或 N/M */
            case KEY_J:
            case KEY_Z:
            case KEY_N:
                joypad = pressed ? (joypad | A_MASK) : (joypad & ~A_MASK); break;
            case KEY_K:
            case KEY_X:
            case KEY_M:
                joypad = pressed ? (joypad | B_MASK) : (joypad & ~B_MASK); break;

            /* START/SELECT：U/I 或 Enter/Space */
            case KEY_U:
            case KEY_ENTER:
                joypad = pressed ? (joypad | START_MASK) : (joypad & ~START_MASK); break;
            case KEY_I:
            case KEY_SPACE:
                joypad = pressed ? (joypad | SELECT_MASK) : (joypad & ~SELECT_MASK); break;

            default:
                /* 其他键仅打印 */
                break;
        }

        printf("[DEBUG] ev.code=%d (%s) %s -> joypad=0x%02X\n",
               ev.code,
               (ev.code==KEY_W?"W":ev.code==KEY_A?"A":ev.code==KEY_S?"S":ev.code==KEY_D?"D":
                ev.code==KEY_UP?"UP":ev.code==KEY_LEFT?"LEFT":ev.code==KEY_DOWN?"DOWN":ev.code==KEY_RIGHT?"RIGHT":
                ev.code==KEY_J?"J":ev.code==KEY_K?"K":ev.code==KEY_Z?"Z":ev.code==KEY_X?"X":
                ev.code==KEY_U?"U":ev.code==KEY_I?"I":ev.code==KEY_ENTER?"ENTER":ev.code==KEY_SPACE?"SPACE":"OTHER"),
               pressed ? "pressed" : "released",
               joypad);
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("[ERROR] read");
    }

    return joypad;
}

/* ---------- 线程与注册 ---------- */
static void *InputEventThreadFunction(void *pVoid)
{
    int (*GetJoypad)(void) = (int (*)(void))pVoid;
    while (1) {
        unsigned char val = GetJoypad();
        pthread_mutex_lock(&g_tMutex);
        g_InputEvent = val;
        pthread_cond_signal(&g_tConVar);
        pthread_mutex_unlock(&g_tMutex);
        usleep(10000); // 10ms
    }
    return NULL;
}

static int RegisterJoypadInput(PT_JoypadInput ptJoypadInput)
{
    PT_JoypadInput tmp;
    if(ptJoypadInput->DevInit()) return -1;

    if(pthread_create(&ptJoypadInput->tTreadID, NULL, InputEventThreadFunction, (void*)ptJoypadInput->GetJoypad) != 0) {
        perror("pthread_create");
        ptJoypadInput->DevExit();
        return -1;
    }

    if(!g_ptJoypadInputHead) g_ptJoypadInputHead = ptJoypadInput;
    else {
        tmp = g_ptJoypadInputHead;
        while(tmp->ptNext) tmp = tmp->ptNext;
        tmp->ptNext = ptJoypadInput;
    }
    ptJoypadInput->ptNext = NULL;
    return 0;
}

static T_JoypadInput usbKeyboardInput = {
    usbKeyboardDevInit,
    usbKeyboardDevExit,
    usbKeyboardGet,
};

int InitJoypadInput(void)
{
    return RegisterJoypadInput(&usbKeyboardInput);
}

int GetJoypadInput(void)
{
    unsigned char val;
    pthread_mutex_lock(&g_tMutex);
    pthread_cond_wait(&g_tConVar, &g_tMutex);
    val = g_InputEvent;
    pthread_mutex_unlock(&g_tMutex);
    return val;
}


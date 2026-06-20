#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "labels.h"

// watch-mode state (for signal handler + cleanup)

typedef struct {
  int fd;
  char* path;
  int grabbed;
  int ctrl_held;
} watch_dev_t;

static volatile sig_atomic_t should_exit = 0;
static watch_dev_t* g_devs = NULL;
static int g_ndevs = 0;

static void sigint_handler(int sig) {
  (void)sig;
  should_exit = 1;
}

static void cleanup_and_exit(int code) {
  for (int i = 0; i < g_ndevs; i++) {
    if (g_devs[i].fd >= 0) {
      if (g_devs[i].grabbed) ioctl(g_devs[i].fd, EVIOCGRAB, 0);
      close(g_devs[i].fd);
    }
    free(g_devs[i].path);
  }
  free(g_devs);
  exit(code);
}

//  label lookup helpers

static const char* label_lookup(struct label* labels, int value) {
  for (; labels->name; labels++) {
    if (labels->value == value) return labels->name;
  }
  return "?";
}

static struct label* get_type_labels(int type) {
  switch (type) {
    case EV_SYN:
      return syn_labels;
    case EV_KEY:
      return key_labels;
    case EV_REL:
      return rel_labels;
    case EV_ABS:
      return abs_labels;
    case EV_SW:
      return sw_labels;
    case EV_MSC:
      return msc_labels;
    case EV_LED:
      return led_labels;
    case EV_SND:
      return snd_labels;
    case EV_REP:
      return rep_labels;
    case EV_FF:
      return ff_labels;
    default:
      return NULL;
  }
}

static const char* code_name(int type, int code) {
  struct label* l = get_type_labels(type);
  if (!l) return "?";
  return label_lookup(l, code);
}

static const char* type_name(int type) { return label_lookup(ev_labels, type); }

static const char* value_name(int type, int value) {
  if (type == EV_KEY) return label_lookup(key_value_labels, value);
  return NULL;
}

// list

static void cmd_list(void) {
  DIR* d = opendir("/dev/input");
  if (!d) {
    perror("opendir /dev/input");
    exit(1);
  }

  struct dirent* ent;
  while ((ent = readdir(d)) != NULL) {
    if (strncmp(ent->d_name, "event", 5) != 0) continue;

    char path[256];
    snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      printf("%-20s (cannot open: %s)\n", path, strerror(errno));
      continue;
    }

    char name[256] = "Unknown";
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);

    struct input_id id;
    memset(&id, 0, sizeof(id));
    ioctl(fd, EVIOCGID, &id);

    printf("%-20s  bus=%-12s vendor=%04x product=%04x  \"%s\"\n", path,
           label_lookup(bus_labels, id.bustype), id.vendor, id.product, name);

    close(fd);
  }
  closedir(d);
}

// enumerate every /dev/input/eventX path (used by -a/--all).
// returns a malloc'd array of malloc'd strings; *out_count set to length.
// Caller owns the memory (free each string, then the array).
static char** enumerate_all_devices(int* out_count) {
  DIR* d = opendir("/dev/input");
  if (!d) {
    perror("opendir /dev/input");
    exit(1);
  }

  char** paths = NULL;
  int count = 0;
  int cap = 0;

  struct dirent* ent;
  while ((ent = readdir(d)) != NULL) {
    if (strncmp(ent->d_name, "event", 5) != 0) continue;

    char path[256];
    snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

    // sanity check it's actually openable before adding it
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;
    close(fd);

    if (count == cap) {
      cap = cap ? cap * 2 : 8;
      paths = realloc(paths, cap * sizeof(char*));
    }
    paths[count++] = strdup(path);
  }
  closedir(d);

  *out_count = count;
  return paths;
}

//  info

static void print_bits(int fd, int ev_type, struct label* labels,
                       unsigned long ev_query) {
  unsigned long bits[(KEY_MAX / (8 * sizeof(long))) + 1];
  memset(bits, 0, sizeof(bits));

  if (ioctl(fd, ev_query, bits) < 0) return;

  printf("  %s:", type_name(ev_type));
  int printed = 0;
  for (int code = 0; code <= KEY_MAX; code++) {
    if (bits[code / (8 * sizeof(long))] &
        (1UL << (code % (8 * sizeof(long))))) {
      const char* nm = labels ? label_lookup(labels, code) : "?";
      printf(" %s(%d)", nm, code);
      printed++;
      if (printed % 8 == 0) printf("\n      ");
    }
  }
  printf("\n");
}

static void cmd_info(const char* path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    perror("open");
    exit(1);
  }

  char name[256] = "Unknown";
  ioctl(fd, EVIOCGNAME(sizeof(name)), name);

  struct input_id id;
  memset(&id, 0, sizeof(id));
  ioctl(fd, EVIOCGID, &id);

  printf("Device:  %s\n", path);
  printf("Name:    %s\n", name);
  printf("Bus:     %s\n", label_lookup(bus_labels, id.bustype));
  printf("Vendor:  %04x\n", id.vendor);
  printf("Product: %04x\n", id.product);
  printf("Version: %04x\n", id.version);

  unsigned long ev_bits[(EV_MAX / (8 * sizeof(long))) + 1];
  memset(ev_bits, 0, sizeof(ev_bits));
  if (ioctl(fd, EVIOCGBIT(0, EV_MAX), ev_bits) < 0) {
    perror("EVIOCGBIT");
    close(fd);
    exit(1);
  }

  printf("\nSupported event types:\n");
  for (int type = 0; type <= EV_MAX; type++) {
    if (!(ev_bits[type / (8 * sizeof(long))] &
          (1UL << (type % (8 * sizeof(long))))))
      continue;

    switch (type) {
      case EV_KEY:
        print_bits(fd, type, key_labels, EVIOCGBIT(EV_KEY, KEY_MAX));
        break;
      case EV_REL:
        print_bits(fd, type, rel_labels, EVIOCGBIT(EV_REL, KEY_MAX));
        break;
      case EV_ABS:
        print_bits(fd, type, abs_labels, EVIOCGBIT(EV_ABS, KEY_MAX));
        break;
      case EV_SW:
        print_bits(fd, type, sw_labels, EVIOCGBIT(EV_SW, KEY_MAX));
        break;
      case EV_LED:
        print_bits(fd, type, led_labels, EVIOCGBIT(EV_LED, KEY_MAX));
        break;
      case EV_MSC:
        print_bits(fd, type, msc_labels, EVIOCGBIT(EV_MSC, KEY_MAX));
        break;
      case EV_SND:
        print_bits(fd, type, snd_labels, EVIOCGBIT(EV_SND, KEY_MAX));
        break;
      case EV_FF:
        print_bits(fd, type, ff_labels, EVIOCGBIT(EV_FF, KEY_MAX));
        break;
      default:
        printf("  %s\n", type_name(type));
    }
  }

  close(fd);
}

//  watch
//
// accepts N device paths (or every /dev/input/eventX via -a/--all) and
// polls all of them in one loop. Each printed line is tagged with which
// device it came from so multi device output stays readable.
//
// Ctrl+C handling: we install sigaction() WITHOUT SA_RESTART, so a
// blocking syscall interrupted by SIGINT returns -1/EINTR instead of
// being transparently restarted by the kernel. On top of that we use
// poll() with a timeout instead of a blocking read(), so even if a
// signal is somehow missed/coalesced, the loop wakes up on its own
// every 200ms and rechecks should_exit. This also makes the in-stream
// Ctrl+C/ESC detection (needed for the grabbed case where the
// keypress never reaches the terminal/compositor) responsive without
// depending on another event arriving first.

static void cmd_watch(char** paths, int npaths, int grab) {
  g_devs = calloc(npaths, sizeof(watch_dev_t));
  g_ndevs = npaths;

  struct pollfd* pfds = calloc(npaths, sizeof(struct pollfd));

  for (int i = 0; i < npaths; i++) {
    int fd = open(paths[i], O_RDONLY);
    if (fd < 0) {
      fprintf(stderr, "open %s: %s\n", paths[i], strerror(errno));
      g_devs[i].fd = -1;
      pfds[i].fd = -1;
      pfds[i].events = 0;
      continue;
    }
    g_devs[i].fd = fd;
    g_devs[i].path = strdup(paths[i]);
    g_devs[i].ctrl_held = 0;
    pfds[i].fd = fd;
    pfds[i].events = POLLIN;
  }

  if (grab) {
    /*
     * Same Enter key stuck issue as before, now applied per device\\\
     * if we grab right as a keypress is mid flight, the compositor
     * may never see the matching key up and thinks the key is held
     * forever. Give every device's release event time to reach the
     * compositor before taking exclusive control of any of them.
     */
    usleep(300000);

    for (int i = 0; i < npaths; i++) {
      if (g_devs[i].fd < 0) continue;
      if (ioctl(g_devs[i].fd, EVIOCGRAB, 1) < 0) {
        fprintf(stderr, "EVIOCGRAB failed on %s: %s\n", g_devs[i].path,
                strerror(errno));
        continue;
      }
      g_devs[i].grabbed = 1;
    }
    fprintf(stderr,
            "Device(s) grabbed exclusively. Press Ctrl+C (or ESC) to "
            "release and exit.\n");
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigint_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // deliberately NOT SA_RESTART
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  struct input_event ev;

  while (!should_exit) {
    int pr = poll(pfds, npaths, 200);  // 200ms tick so we recheck should_exit
    if (pr < 0) {
      if (errno == EINTR) continue;
      perror("poll");
      break;
    }
    if (pr == 0) continue;  // idle tick, nothing to read

    for (int i = 0; i < npaths; i++) {
      if (g_devs[i].fd < 0) continue;
      if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;

      if (pfds[i].revents & (POLLERR | POLLHUP)) {
        fprintf(stderr, "%s: device disconnected\n", g_devs[i].path);
        close(g_devs[i].fd);
        g_devs[i].fd = -1;
        pfds[i].fd = -1;
        pfds[i].events = 0;
        continue;
      }

      ssize_t n = read(g_devs[i].fd, &ev, sizeof(ev));
      if (n < 0) {
        if (errno == EINTR) continue;
        perror("read");
        continue;
      }
      if (n != sizeof(ev)) continue;

      if (ev.type == EV_KEY) {
        if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL)
          g_devs[i].ctrl_held = ev.value;
        if (ev.code == KEY_C && ev.value == 1 && g_devs[i].ctrl_held) {
          fprintf(stderr,
                  "\nCtrl+C detected in stream, releasing grab(s) and "
                  "exiting.\n");
          cleanup_and_exit(0);
        }
        if (ev.code == KEY_ESC && ev.value == 1) {
          fprintf(stderr, "\nESC detected, releasing grab(s) and exiting.\n");
          cleanup_and_exit(0);
        }
      }

      const char* tname = type_name(ev.type);
      const char* cname = code_name(ev.type, ev.code);
      const char* vname = value_name(ev.type, ev.value);

      printf("[%ld.%06ld] %-18s type=%-8s (%2d)  code=%-20s (%3d)  value=%-6d",
             (long)ev.time.tv_sec, (long)ev.time.tv_usec, g_devs[i].path, tname,
             ev.type, cname, ev.code, ev.value);

      if (vname) printf("  [%s]", vname);

      printf("\n");
      fflush(stdout);
    }
  }

  cleanup_and_exit(0);
  free(pfds);
}

//  inject (via uinput, mirroring a real device's capabilities)

static void copy_bits(int src_fd, int uinput_fd, int ev_type,
                      unsigned long ev_query, int set_code_ioctl) {
  unsigned long bits[(KEY_MAX / (8 * sizeof(long))) + 1];
  memset(bits, 0, sizeof(bits));

  if (ioctl(src_fd, ev_query, bits) < 0) return;

  ioctl(uinput_fd, UI_SET_EVBIT, ev_type);

  for (int code = 0; code <= KEY_MAX; code++) {
    if (bits[code / (8 * sizeof(long))] & (1UL << (code % (8 * sizeof(long)))))
      ioctl(uinput_fd, set_code_ioctl, code);
  }
}

static void emit(int fd, int type, int code, int value) {
  struct input_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = type;
  ev.code = code;
  ev.value = value;
  gettimeofday(&ev.time, NULL);
  if (write(fd, &ev, sizeof(ev)) < 0) perror("write");
}

static void cmd_inject(const char* src_path, int type, int code, int value) {
  int src_fd = open(src_path, O_RDONLY);
  if (src_fd < 0) {
    perror("open source device");
    exit(1);
  }

  int ui_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (ui_fd < 0) {
    perror("open /dev/uinput (need root / uinput permissions)");
    close(src_fd);
    exit(1);
  }

  // mirror capability bits from the source device so the virtual
  // device looks like a clone for the relevant event type.
  copy_bits(src_fd, ui_fd, EV_KEY, EVIOCGBIT(EV_KEY, KEY_MAX), UI_SET_KEYBIT);
  copy_bits(src_fd, ui_fd, EV_REL, EVIOCGBIT(EV_REL, KEY_MAX), UI_SET_RELBIT);
  copy_bits(src_fd, ui_fd, EV_ABS, EVIOCGBIT(EV_ABS, KEY_MAX), UI_SET_ABSBIT);
  copy_bits(src_fd, ui_fd, EV_SW, EVIOCGBIT(EV_SW, KEY_MAX), UI_SET_SWBIT);
  copy_bits(src_fd, ui_fd, EV_MSC, EVIOCGBIT(EV_MSC, KEY_MAX), UI_SET_MSCBIT);

  // make sure the requested type/code is enabled even if the
  // capability scan above happened to miss it.
  ioctl(ui_fd, UI_SET_EVBIT, type);
  switch (type) {
    case EV_KEY:
      ioctl(ui_fd, UI_SET_KEYBIT, code);
      break;
    case EV_REL:
      ioctl(ui_fd, UI_SET_RELBIT, code);
      break;
    case EV_ABS:
      ioctl(ui_fd, UI_SET_ABSBIT, code);
      break;
    case EV_SW:
      ioctl(ui_fd, UI_SET_SWBIT, code);
      break;
    case EV_MSC:
      ioctl(ui_fd, UI_SET_MSCBIT, code);
      break;
    default:
      break;
  }
  ioctl(ui_fd, UI_SET_EVBIT, EV_SYN);

  struct uinput_setup usetup;
  memset(&usetup, 0, sizeof(usetup));
  usetup.id.bustype = BUS_USB;
  usetup.id.vendor = 0x1234;
  usetup.id.product = 0x5678;
  strncpy(usetup.name, "lxkey-virtual-device", sizeof(usetup.name) - 1);

  if (ioctl(ui_fd, UI_DEV_SETUP, &usetup) < 0) {
    perror("UI_DEV_SETUP");
    goto out;
  }
  if (ioctl(ui_fd, UI_DEV_CREATE) < 0) {
    perror("UI_DEV_CREATE");
    goto out;
  }

  // give userspace (compositor/X) a moment to notice and register
  // the new device before we start sending events through it.
  usleep(300000);

  printf("Injecting: type=%s(%d) code=%s(%d) value=%d\n", type_name(type), type,
         code_name(type, code), code, value);

  emit(ui_fd, type, code, value);
  emit(ui_fd, EV_SYN, SYN_REPORT, 0);

  usleep(50000);

  ioctl(ui_fd, UI_DEV_DESTROY);

out:
  close(ui_fd);
  close(src_fd);
}

//  main

static void usage(const char* prog) {
  fprintf(stderr,
          "Usage:\n"
          "  %s list\n"
          "  %s info  /dev/input/eventX\n"
          "  %s watch [-g] /dev/input/eventX [/dev/input/eventY ...]\n"
          "  %s watch [-g] -a | --all\n"
          "  %s inject /dev/input/eventX <type> <code> <value>\n"
          "      type/code can be numeric or symbolic (e.g. EV_KEY KEY_A 1)\n",
          prog, prog, prog, prog, prog);
}

static int resolve_type(const char* s) {
  char* end;
  long v = strtol(s, &end, 0);
  if (*end == '\0') return (int)v;
  for (struct label* l = ev_labels; l->name; l++)
    if (strcmp(l->name, s) == 0) return l->value;
  fprintf(stderr, "unknown type '%s'\n", s);
  exit(1);
}

static int resolve_code(int type, const char* s) {
  char* end;
  long v = strtol(s, &end, 0);
  if (*end == '\0') return (int)v;
  struct label* l = get_type_labels(type);
  if (l) {
    for (; l->name; l++)
      if (strcmp(l->name, s) == 0) return l->value;
  }
  fprintf(stderr, "unknown code '%s' for type\n", s);
  exit(1);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  if (strcmp(argv[1], "list") == 0) {
    cmd_list();
  } else if (strcmp(argv[1], "info") == 0) {
    if (argc < 3) {
      usage(argv[0]);
      return 1;
    }
    cmd_info(argv[2]);
  } else if (strcmp(argv[1], "watch") == 0) {
    int grab = 0;
    int all = 0;
    char** paths = NULL;
    int npaths = 0;
    int cap = 0;

    for (int i = 2; i < argc; i++) {
      if (strcmp(argv[i], "-g") == 0) {
        grab = 1;
      } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
        all = 1;
      } else {
        if (npaths == cap) {
          cap = cap ? cap * 2 : 8;
          paths = realloc(paths, cap * sizeof(char*));
        }
        paths[npaths++] = argv[i];
      }
    }

    char** all_paths = NULL;
    if (all) {
      if (npaths > 0) {
        fprintf(stderr,
                "warning: explicit device paths ignored because -a/--all "
                "was given\n");
      }
      int n = 0;
      all_paths = enumerate_all_devices(&n);
      if (n == 0) {
        fprintf(stderr, "no readable devices found under /dev/input\n");
        return 1;
      }
      free(paths);
      paths = all_paths;
      npaths = n;
    }

    if (npaths == 0) {
      usage(argv[0]);
      return 1;
    }

    cmd_watch(paths, npaths, grab);

    if (all) {
      for (int i = 0; i < npaths; i++) free(all_paths[i]);
    }
    free(paths);
  } else if (strcmp(argv[1], "inject") == 0) {
    if (argc < 6) {
      usage(argv[0]);
      return 1;
    }
    int type = resolve_type(argv[3]);
    int code = resolve_code(type, argv[4]);
    int value = (int)strtol(argv[5], NULL, 0);
    cmd_inject(argv[2], type, code, value);
  } else {
    usage(argv[0]);
    return 1;
  }

  return 0;
}

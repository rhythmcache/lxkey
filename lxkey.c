#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "labels.h"

// watch-mode state (for signal handler + cleanup)

static volatile int should_exit = 0;
static int watch_fd = -1;
static int watch_grabbed = 0;

static void sigint_handler(int sig) {
  (void)sig;
  should_exit = 1;
}

static void cleanup_and_exit(int code) {
  if (watch_fd >= 0) {
    if (watch_grabbed) ioctl(watch_fd, EVIOCGRAB, 0);
    close(watch_fd);
  }
  exit(code);
}

//  label lookup helpers

static const char *label_lookup(struct label *labels, int value) {
  for (; labels->name; labels++) {
    if (labels->value == value) return labels->name;
  }
  return "?";
}

static struct label *get_type_labels(int type) {
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

static const char *code_name(int type, int code) {
  struct label *l = get_type_labels(type);
  if (!l) return "?";
  return label_lookup(l, code);
}

static const char *type_name(int type) { return label_lookup(ev_labels, type); }

static const char *value_name(int type, int value) {
  if (type == EV_KEY) return label_lookup(key_value_labels, value);
  return NULL;
}

// list

static void cmd_list(void) {
  DIR *d = opendir("/dev/input");
  if (!d) {
    perror("opendir /dev/input");
    exit(1);
  }

  struct dirent *ent;
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

//  info

static void print_bits(int fd, int ev_type, struct label *labels,
                       unsigned long ev_query) {
  unsigned long bits[(KEY_MAX / (8 * sizeof(long))) + 1];
  memset(bits, 0, sizeof(bits));

  if (ioctl(fd, ev_query, bits) < 0) return;

  printf("  %s:", type_name(ev_type));
  int printed = 0;
  for (int code = 0; code <= KEY_MAX; code++) {
    if (bits[code / (8 * sizeof(long))] &
        (1UL << (code % (8 * sizeof(long))))) {
      const char *nm = labels ? label_lookup(labels, code) : "?";
      printf(" %s(%d)", nm, code);
      printed++;
      if (printed % 8 == 0) printf("\n      ");
    }
  }
  printf("\n");
}

static void cmd_info(const char *path) {
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

// watch

static void cmd_watch(const char *path, int grab) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    perror("open");
    exit(1);
  }

  watch_fd = fd;

  if (grab) {
    /*
     * this sleep is necessary otherwise it causes a problem
     *
     * to run this binary itself we had to press Enter which sends
     * KEY_ENTER DOWN and then KEY_ENTER UP. If the grab kicks in
     * between these two events only this program receives the UP
     * the compositor/terminal never finds out Enter was released so
     * it thinks Enter is still being pressed continuously. That's
     * what causes the newline spam (as if Enter is held down forever).
     *
     * By waiting a little before grabbing the release event reaches
     * the compositor first and only after that do we take exclusive
     * control of the device.
     *
     * (Hit this exact problem while testing without this sleep
     * grabbing the keyboard right after launch leaves the terminal
     * stuck thinking Enter is held.)
     */
    usleep(300000);

    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
      fprintf(stderr, "EVIOCGRAB failed: %s\n", strerror(errno));
      close(fd);
      exit(1);
    }
    watch_grabbed = 1;
    fprintf(stderr,
            "Device grabbed exclusively. Press Ctrl+C (or ESC) to release and "
            "exit.\n");
  }

  /*
   * Custom Ctrl+C / ESC handling:
   *
   * In grab mode this process becomes the *only* listener for this
   * device's events including the keys that make up Ctrl+C itself.
   * A normal SIGINT from the terminal may not arrive cleanly here,
   * since the keypresses that would generate it are being consumed
   * straight from the device by us. So in addition to the SIGINT
   * handler below (which covers the non-grab / signal-still-delivered
   * case), we also watch the decoded event stream directly for
   * Ctrl+C and ESC, and treat either as "release grab and exit"
   * giving a reliable way out even when the device is fully grabbed.
   */
  signal(SIGINT, sigint_handler);
  signal(SIGTERM, sigint_handler);

  struct input_event ev;
  int ctrl_held = 0;

  while (!should_exit) {
    ssize_t n = read(fd, &ev, sizeof(ev));
    if (n < 0) {
      if (errno == EINTR) continue;
      perror("read");
      break;
    }
    if (n != sizeof(ev)) break;

    if (ev.type == EV_KEY) {
      if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL)
        ctrl_held = ev.value;
      if (ev.code == KEY_C && ev.value == 1 && ctrl_held) {
        fprintf(stderr,
                "\nCtrl+C detected in stream, releasing grab and exiting.\n");
        cleanup_and_exit(0);
      }
      if (ev.code == KEY_ESC && ev.value == 1) {
        fprintf(stderr, "\nESC detected, releasing grab and exiting.\n");
        cleanup_and_exit(0);
      }
    }

    const char *tname = type_name(ev.type);
    const char *cname = code_name(ev.type, ev.code);
    const char *vname = value_name(ev.type, ev.value);

    printf("[%ld.%06ld] type=%-8s (%2d)  code=%-20s (%3d)  value=%-6d",
           (long)ev.time.tv_sec, (long)ev.time.tv_usec, tname, ev.type, cname,
           ev.code, ev.value);

    if (vname) printf("  [%s]", vname);

    printf("\n");
    fflush(stdout);
  }

  cleanup_and_exit(0);
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

static void cmd_inject(const char *src_path, int type, int code, int value) {
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

static void usage(const char *prog) {
  fprintf(stderr,
          "Usage:\n"
          "  %s list\n"
          "  %s info  /dev/input/eventX\n"
          "  %s watch [-g] /dev/input/eventX\n"
          "  %s inject /dev/input/eventX <type> <code> <value>\n"
          "      type/code can be numeric or symbolic (e.g. EV_KEY KEY_A 1)\n",
          prog, prog, prog, prog);
}

static int resolve_type(const char *s) {
  char *end;
  long v = strtol(s, &end, 0);
  if (*end == '\0') return (int)v;
  for (struct label *l = ev_labels; l->name; l++)
    if (strcmp(l->name, s) == 0) return l->value;
  fprintf(stderr, "unknown type '%s'\n", s);
  exit(1);
}

static int resolve_code(int type, const char *s) {
  char *end;
  long v = strtol(s, &end, 0);
  if (*end == '\0') return (int)v;
  struct label *l = get_type_labels(type);
  if (l) {
    for (; l->name; l++)
      if (strcmp(l->name, s) == 0) return l->value;
  }
  fprintf(stderr, "unknown code '%s' for type\n", s);
  exit(1);
}

int main(int argc, char **argv) {
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
    const char *path = NULL;
    for (int i = 2; i < argc; i++) {
      if (strcmp(argv[i], "-g") == 0)
        grab = 1;
      else
        path = argv[i];
    }
    if (!path) {
      usage(argv[0]);
      return 1;
    }
    cmd_watch(path, grab);
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
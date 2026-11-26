/*
 * l2audio - tiny on-demand audio control, module loader, and PCM test tool.
 * Zero daemon overhead: loads sound kernel modules from l2/wifi only when
 * invoked, adjusts ALSA/OSS mixer controls directly via Linux ioctls,
 * persists state in /run/l2-vol, and notifies l2wm via SIGUSR1.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/soundcard.h>
#include <math.h>
#include <signal.h>
#include <sound/asound.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VOL_STATE_PATH "/run/l2-vol"

static int read_state(int *vol, int *muted) {
    *vol = 75;
    *muted = 0;
    FILE *f = fopen(VOL_STATE_PATH, "r");
    if (!f) return 0;
    int v = 75, m = 0;
    if (fscanf(f, "%d %d", &v, &m) >= 1) {
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        *vol = v;
        *muted = (m != 0);
    }
    fclose(f);
    return 1;
}

static void write_state(int vol, int muted) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    int fd = open(VOL_STATE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d %d\n", vol, muted ? 1 : 0);
    if (n > 0) (void)!write(fd, buf, (size_t)n);
    close(fd);
}

static void signal_l2wm(void) {
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        long pid = strtol(e->d_name, NULL, 10);
        if (pid <= 1) continue;
        char p[64], comm[32];
        snprintf(p, sizeof(p), "/proc/%ld/comm", pid);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(comm, sizeof(comm), f)) {
            comm[strcspn(comm, "\r\n")] = '\0';
            if (strcmp(comm, "l2wm") == 0) kill((pid_t)pid, SIGUSR1);
        }
        fclose(f);
    }
    closedir(d);
}

static int run_quiet(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int any_snd_present(void) {
    for (int i = 0; i < 4; ++i) {
        char p[32];
        snprintf(p, sizeof(p), "/dev/snd/controlC%d", i);
        if (access(p, F_OK) == 0) return 1;
    }
    return access("/dev/mixer", F_OK) == 0;
}

static void ensure_modules_dir(void) {
    struct utsname u;
    if (uname(&u) != 0) return;

    const char *medium = getenv("L2_MEDIUM");
    if (!medium || !medium[0]) medium = "/media/l2";
    const char *modroot = getenv("L2_MODDIR");
    if (!modroot || !modroot[0]) modroot = "/lib/modules";
    const char *l2net = getenv("L2_L2NET");
    if (!l2net || !l2net[0]) l2net = "/bin/l2net";

    char modpath[512], marker[512], wifipack[512], wifimod[512], cur[512];
    snprintf(modpath, sizeof(modpath), "%s/%s", modroot, u.release);
    snprintf(marker, sizeof(marker), "%s/l2/L2-LIVE", medium);
    snprintf(wifipack, sizeof(wifipack), "%s/l2/wifi", medium);
    snprintf(wifimod, sizeof(wifimod), "%s/lib/modules/%s", wifipack, u.release);

    struct stat st;
    if (lstat(modpath, &st) == 0 && !S_ISLNK(st.st_mode)) {
        return; /* Real installed directory already exists */
    }
    if (access(marker, F_OK) != 0) return;
    if (access(wifimod, R_OK | X_OK) != 0) return;

    /* Phase-2 pack integrity pin: verify l2/wifi before exposing modules. */
    char *verify_args[] = {(char *)l2net, "verify-pack", wifipack, "wifi", NULL};
    if (run_quiet(verify_args) != 0) {
        fprintf(stderr, "l2audio: wifi pack failed Phase-2 integrity verification\n");
        return;
    }

    /* Keep modules pointing at l2/wifi without forcing Wi-Fi driver probe. */
    if (lstat(modpath, &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            ssize_t len = readlink(modpath, cur, sizeof(cur) - 1);
            if (len > 0) {
                cur[len] = '\0';
                if (strcmp(cur, wifimod) == 0) return;
            }
            unlink(modpath);
        } else {
            return;
        }
    }
    (void)!symlink(wifimod, modpath);
}

static void ensure_audio(void) {
    if (any_snd_present()) {
        if (access("/dev/mixer", F_OK) != 0 || access("/dev/dsp", F_OK) != 0) {
            char *oss_args[] = {"modprobe", "-a", "-q", "snd-mixer-oss", "snd-pcm-oss", NULL};
            ensure_modules_dir();
            run_quiet(oss_args);
        }
        return;
    }

    ensure_modules_dir();

    /* Probe only audio-class PCI/USB devices so Wi-Fi/Bluetooth stay unloaded. */
    const char *args[64];
    char aliases[48][256];
    int argc = 0;
    args[argc++] = "modprobe";
    args[argc++] = "-a";
    args[argc++] = "-q";

    DIR *d = opendir("/sys/bus/pci/devices");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && argc < 48) {
            if (e->d_name[0] == '.') continue;
            char cp[300], cls[32] = "";
            snprintf(cp, sizeof(cp), "/sys/bus/pci/devices/%s/class", e->d_name);
            FILE *cf = fopen(cp, "r");
            if (!cf) continue;
            if (!fgets(cls, sizeof(cls), cf)) { fclose(cf); continue; }
            fclose(cf);
            unsigned long c = strtoul(cls, NULL, 16);
            if (((c >> 16) & 0xff) != 0x04) continue; /* Multimedia PCI class */
            snprintf(cp, sizeof(cp), "/sys/bus/pci/devices/%s/modalias", e->d_name);
            FILE *mf = fopen(cp, "r");
            if (!mf) continue;
            if (fgets(aliases[argc], sizeof(aliases[argc]), mf)) {
                aliases[argc][strcspn(aliases[argc], "\r\n")] = '\0';
                if (aliases[argc][0]) args[argc] = aliases[argc], argc++;
            }
            fclose(mf);
        }
        closedir(d);
    }

    d = opendir("/sys/bus/usb/devices");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && argc < 48) {
            if (e->d_name[0] == '.') continue;
            char cp[300], ic[16] = "";
            snprintf(cp, sizeof(cp), "/sys/bus/usb/devices/%s/bInterfaceClass", e->d_name);
            FILE *cf = fopen(cp, "r");
            if (!cf) continue;
            if (!fgets(ic, sizeof(ic), cf)) { fclose(cf); continue; }
            fclose(cf);
            if (strncmp(ic, "01", 2) != 0) continue; /* USB Audio class */
            snprintf(cp, sizeof(cp), "/sys/bus/usb/devices/%s/modalias", e->d_name);
            FILE *mf = fopen(cp, "r");
            if (!mf) continue;
            if (fgets(aliases[argc], sizeof(aliases[argc]), mf)) {
                aliases[argc][strcspn(aliases[argc], "\r\n")] = '\0';
                if (aliases[argc][0]) args[argc] = aliases[argc], argc++;
            }
            fclose(mf);
        }
        closedir(d);
    }

    static const char *const extras[] = {
        "snd-hda-codec-conexant", "snd-hda-codec-realtek", "snd-hda-codec-generic",
        "snd-hda-codec-hdmi", "snd-mixer-oss", "snd-pcm-oss", NULL
    };
    for (int i = 0; extras[i] && argc < 62; ++i) args[argc++] = extras[i];
    args[argc] = NULL;
    run_quiet((char *const *)args);

    for (int i = 0; i < 30 && !any_snd_present(); ++i) {
        struct timespec ts = {0, 50 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}

static int is_playback_vol_name(const char *name) {
    if (strstr(name, "Capture") || strstr(name, "Mic") || strstr(name, "Input") ||
        strstr(name, "Beep") || strstr(name, "Loopback") || strstr(name, "Auto-Mute")) {
        return 0;
    }
    if (strstr(name, "Master Playback Volume") ||
        strstr(name, "Speaker Playback Volume") ||
        strstr(name, "Headphone Playback Volume") ||
        strstr(name, "PCM Playback Volume") ||
        strstr(name, "Master Mono Playback Volume") ||
        strcmp(name, "Master Volume") == 0) {
        return 1;
    }
    return 0;
}

static int is_playback_switch_name(const char *name) {
    if (strstr(name, "Capture") || strstr(name, "Mic") || strstr(name, "Input") ||
        strstr(name, "Beep") || strstr(name, "Loopback") || strstr(name, "Auto-Mute")) {
        return 0;
    }
    if (strstr(name, "Master Playback Switch") ||
        strstr(name, "Speaker Playback Switch") ||
        strstr(name, "Headphone Playback Switch") ||
        strstr(name, "PCM Playback Switch") ||
        strstr(name, "IEC958 Playback Switch")) {
        return 1;
    }
    return 0;
}

static void apply_alsa_card(const char *dev, int vol, int muted) {
    int fd = open(dev, O_RDWR);
    if (fd < 0) return;

    struct snd_ctl_elem_list list;
    memset(&list, 0, sizeof(list));
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0 || list.count == 0) {
        close(fd);
        return;
    }
    unsigned int cap = list.count > 256 ? 256 : list.count;
    struct snd_ctl_elem_id *ids = calloc(cap, sizeof(*ids));
    if (!ids) {
        close(fd);
        return;
    }
    list.offset = 0;
    list.space = cap;
    list.pids = ids;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0) {
        for (unsigned int i = 0; i < list.used; ++i) {
            struct snd_ctl_elem_info info;
            memset(&info, 0, sizeof(info));
            info.id = ids[i];
            if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) continue;
            if (!(info.access & SNDRV_CTL_ELEM_ACCESS_WRITE)) continue;
            const char *name = (const char *)info.id.name;

            if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER && is_playback_vol_name(name)) {
                struct snd_ctl_elem_value val;
                memset(&val, 0, sizeof(val));
                val.id = info.id;
                long lo = info.value.integer.min;
                long hi = info.value.integer.max;
                long v = lo + (hi - lo) * vol / 100;
                unsigned int cnt = info.count > 128 ? 128 : info.count;
                for (unsigned int c = 0; c < cnt; ++c) val.value.integer.value[c] = v;
                (void)ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val);
            } else if (info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN && is_playback_switch_name(name)) {
                struct snd_ctl_elem_value val;
                memset(&val, 0, sizeof(val));
                val.id = info.id;
                long on = muted ? 0 : 1;
                unsigned int cnt = info.count > 128 ? 128 : info.count;
                for (unsigned int c = 0; c < cnt; ++c) val.value.integer.value[c] = on;
                (void)ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val);
            }
        }
    }
    free(ids);
    close(fd);
}

static void apply_oss_mixer(const char *dev, int vol, int muted) {
    int fd = open(dev, O_RDWR);
    if (fd < 0) return;
    int eff = muted ? 0 : vol;
    int v = (eff << 8) | eff;
    (void)ioctl(fd, SOUND_MIXER_WRITE_VOLUME, &v);
    (void)ioctl(fd, SOUND_MIXER_WRITE_PCM, &v);
    close(fd);
}

static void apply_hardware(int vol, int muted) {
    ensure_audio();
    for (int i = 0; i < 4; ++i) {
        char p[32];
        snprintf(p, sizeof(p), "/dev/snd/controlC%d", i);
        apply_alsa_card(p, vol, muted);
    }
    apply_oss_mixer("/dev/mixer", vol, muted);
    for (int i = 0; i < 4; ++i) {
        char p[32];
        snprintf(p, sizeof(p), "/dev/mixer%d", i);
        apply_oss_mixer(p, vol, muted);
    }
}

static int pick_playback_dsp(char *out, size_t n) {
    /* Prefer first non-HDMI playback card (e.g. analog speaker codec on laptops). */
    FILE *f = fopen("/proc/asound/pcm", "r");
    if (f) {
        char line[256];
        int fallback_card = -1;
        while (fgets(line, sizeof(line), f)) {
            if (!strstr(line, "playback")) continue;
            int card = 0, dev = 0;
            if (sscanf(line, "%d-%d:", &card, &dev) == 2) {
                if (fallback_card < 0) fallback_card = card;
                if (!strstr(line, "HDMI") && !strstr(line, "hdmi")) {
                    fclose(f);
                    if (card == 0) snprintf(out, n, "/dev/dsp");
                    else snprintf(out, n, "/dev/dsp%d", card);
                    if (access(out, W_OK) == 0) return 0;
                    break;
                }
            }
        }
        fclose(f);
        if (fallback_card >= 0) {
            if (fallback_card == 0) snprintf(out, n, "/dev/dsp");
            else snprintf(out, n, "/dev/dsp%d", fallback_card);
            if (access(out, W_OK) == 0) return 0;
        }
    }
    const char *cands[] = {"/dev/dsp", "/dev/dsp1", "/dev/dsp0", NULL};
    for (int i = 0; cands[i]; ++i) {
        if (access(cands[i], W_OK) == 0) {
            snprintf(out, n, "%s", cands[i]);
            return 0;
        }
    }
    return -1;
}

static int setup_dsp(int fd, int fmt, int ch, int rate) {
    int f = fmt, c = ch, r = rate;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &f) < 0) return -1;
    if (ioctl(fd, SNDCTL_DSP_CHANNELS, &c) < 0) return -1;
    if (ioctl(fd, SNDCTL_DSP_SPEED, &r) < 0) return -1;
    return 0;
}

static int cmd_test(void) {
    int vol, muted;
    read_state(&vol, &muted);
    apply_hardware(vol, muted);

    char dsp[64];
    if (pick_playback_dsp(dsp, sizeof(dsp)) < 0) {
        fprintf(stderr, "l2audio: no playback audio device found (/dev/dsp*)\n");
        return 1;
    }
    int fd = open(dsp, O_WRONLY);
    if (fd < 0) {
        perror("l2audio: open dsp");
        return 1;
    }
    if (setup_dsp(fd, AFMT_S16_LE, 2, 44100) < 0) {
        perror("l2audio: configure dsp");
        close(fd);
        return 1;
    }
    /* Two-note soft sine chime: A4 (440 Hz) -> E5 (659.25 Hz), 160ms each */
    const int rate = 44100;
    const int note_frames = rate * 16 / 100;
    int16_t buf[1024];
    for (int note = 0; note < 2; ++note) {
        double freq = (note == 0) ? 440.0 : 659.25;
        for (int off = 0; off < note_frames; ) {
            int chunk = note_frames - off;
            if (chunk > 512) chunk = 512;
            for (int i = 0; i < chunk; ++i) {
                int idx = off + i;
                double env = 1.0;
                if (idx < 800) env = (double)idx / 800.0;
                else if (idx > note_frames - 2400) env = (double)(note_frames - idx) / 2400.0;
                double s = sin(2.0 * 3.14159265358979323846 * freq * idx / rate);
                int16_t sample = (int16_t)(s * env * 9000.0);
                buf[i * 2] = sample;
                buf[i * 2 + 1] = sample;
            }
            if (write(fd, buf, (size_t)chunk * 4) < 0) break;
            off += chunk;
        }
    }
    close(fd);
    printf("l2audio: played test chime on %s\n", dsp);
    return 0;
}

static uint16_t rd_le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int cmd_play(const char *path) {
    int vol, muted;
    read_state(&vol, &muted);
    apply_hardware(vol, muted);

    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("l2audio: open wav");
        return 1;
    }
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "l2audio: %s is not a WAV file\n", path);
        fclose(f);
        return 1;
    }

    int ch = 2, rate = 44100, bits = 16, fmt_ok = 0;
    uint32_t data_bytes = 0;
    uint8_t chdr[8];
    while (fread(chdr, 1, 8, f) == 8) {
        uint32_t sz = rd_le32(chdr + 4);
        if (memcmp(chdr, "fmt ", 4) == 0 && sz >= 16) {
            uint8_t fb[16];
            if (fread(fb, 1, 16, f) != 16) break;
            if (sz > 16) fseek(f, (long)(sz - 16), SEEK_CUR);
            if (rd_le16(fb) != 1) {
                fprintf(stderr, "l2audio: only uncompressed PCM WAV is supported\n");
                fclose(f);
                return 1;
            }
            ch = rd_le16(fb + 2);
            rate = (int)rd_le32(fb + 4);
            bits = rd_le16(fb + 14);
            fmt_ok = 1;
        } else if (memcmp(chdr, "data", 4) == 0) {
            data_bytes = sz;
            break;
        } else {
            fseek(f, (long)(sz + (sz & 1u)), SEEK_CUR);
        }
    }
    if (!fmt_ok || data_bytes == 0 || bits != 16) {
        fprintf(stderr, "l2audio: invalid or non-16-bit PCM WAV (%s)\n", path);
        fclose(f);
        return 1;
    }

    char dsp[64];
    if (pick_playback_dsp(dsp, sizeof(dsp)) < 0) {
        fprintf(stderr, "l2audio: no playback device found\n");
        fclose(f);
        return 1;
    }
    int fd = open(dsp, O_WRONLY);
    if (fd < 0 || setup_dsp(fd, AFMT_S16_LE, ch, rate) < 0) {
        perror("l2audio: open/configure dsp");
        if (fd >= 0) close(fd);
        fclose(f);
        return 1;
    }

    uint8_t io_buf[4096];
    uint32_t left = data_bytes;
    while (left > 0) {
        size_t want = left > sizeof(io_buf) ? sizeof(io_buf) : left;
        size_t got = fread(io_buf, 1, want, f);
        if (got == 0) break;
        if (write(fd, io_buf, got) < 0) break;
        left -= (uint32_t)got;
    }
    close(fd);
    fclose(f);
    return 0;
}

static int cmd_status(void) {
    int vol, muted;
    read_state(&vol, &muted);
    printf("volume: %d%%%s\n", vol, muted ? " (muted)" : "");
    FILE *f = fopen("/proc/asound/cards", "r");
    if (f) {
        char line[256];
        int any = 0;
        while (fgets(line, sizeof(line), f)) {
            if (!any) printf("cards:\n");
            any = 1;
            printf("  %s", line);
        }
        fclose(f);
    } else {
        printf("cards: (none loaded yet; loads on demand on volume/test)\n");
    }
    return 0;
}

static void usage(void) {
    puts("l2audio - tiny on-demand audio control\n"
         "usage: l2audio [status|up [STEP]|down [STEP]|set PCT|mute|unmute|toggle|test|play FILE.wav]");
}

int main(int argc, char **argv) {
    const char *cmd = (argc > 1) ? argv[1] : "status";
    int vol, muted;
    read_state(&vol, &muted);

    if (strcmp(cmd, "status") == 0) {
        return cmd_status();
    } else if (strcmp(cmd, "up") == 0) {
        int step = (argc > 2) ? atoi(argv[2]) : 5;
        if (step <= 0) step = 5;
        vol += step;
        if (vol > 100) vol = 100;
        muted = 0;
    } else if (strcmp(cmd, "down") == 0) {
        int step = (argc > 2) ? atoi(argv[2]) : 5;
        if (step <= 0) step = 5;
        vol -= step;
        if (vol < 0) vol = 0;
    } else if (strcmp(cmd, "set") == 0 && argc > 2) {
        vol = atoi(argv[2]);
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        muted = 0;
    } else if (strcmp(cmd, "mute") == 0) {
        muted = 1;
    } else if (strcmp(cmd, "unmute") == 0) {
        muted = 0;
    } else if (strcmp(cmd, "toggle") == 0) {
        muted = !muted;
    } else if (strcmp(cmd, "test") == 0) {
        return cmd_test();
    } else if (strcmp(cmd, "play") == 0 && argc > 2) {
        return cmd_play(argv[2]);
    } else if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage();
        return 0;
    } else {
        usage();
        return 2;
    }

    write_state(vol, muted);
    signal_l2wm();
    apply_hardware(vol, muted);
    printf("%d%%%s\n", vol, muted ? " (muted)" : "");
    return 0;
}

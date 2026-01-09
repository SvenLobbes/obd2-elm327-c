#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <sys/select.h>
#include <ctype.h>
#include <stdlib.h>

static int read_until_prompt(int fd, char *out, size_t out_sz, int timeout_ms) {
    size_t pos = 0;
    out[0] = '\0';

    while (pos + 1 < out_sz) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);

        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int rv = select(fd + 1, &set, NULL, NULL, &tv);
        if (rv == 0) break;
        if (rv < 0) return -1;

        char c;
        int n = (int)read(fd, &c, 1);
        if (n <= 0) break;

        out[pos++] = c;
        out[pos] = '\0';

        if (c == '>') return (int)pos;
    }
    return (int)pos;
}

static int send_cmd(int fd, const char *cmd, char *resp, size_t resp_sz) {
    char line[64];
    snprintf(line, sizeof(line), "%s\r", cmd);
    if (write(fd, line, strlen(line)) < 0) return -1;
    return read_until_prompt(fd, resp, resp_sz, 500);
}

static int parse_rpm(const char *resp, float *rpm_out) {
    if (strstr(resp, "NO DATA") || strstr(resp, "UNABLE") || strstr(resp, "ERROR")) return -1;

    char clean[1024];
    size_t j = 0;
    for (size_t i = 0; resp[i] && j + 1 < sizeof(clean); i++) {
        unsigned char ch = (unsigned char)resp[i];
        if (isxdigit(ch)) clean[j++] = (char)ch;
        else clean[j++] = ' ';
    }
    clean[j] = '\0';

    int bytes[128], count = 0;
    char *tmp = strdup(clean);
    if (!tmp) return -1;

    for (char *tok = strtok(tmp, " "); tok; tok = strtok(NULL, " ")) {
        if (!*tok) continue;
        size_t len = strlen(tok);
        if (len < 2) continue;
        if (len == 2) {
            bytes[count++] = (int)strtol(tok, NULL, 16);
        } else {
            for (size_t k = 0; k + 1 < len && count < 128; k += 2) {
                char pair[3] = { tok[k], tok[k+1], 0 };
                bytes[count++] = (int)strtol(pair, NULL, 16);
            }
        }
        if (count >= 128) break;
    }
    free(tmp);

    for (int i = 0; i + 3 < count; i++) {
        if (bytes[i] == 0x41 && bytes[i+1] == 0x0C) {
            int A = bytes[i+2];
            int B = bytes[i+3];
            int raw = (A << 8) | B;
            *rpm_out = raw / 4.0f;
            return 0;
        }
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <serial_device>\nExample: %s /dev/ttys007\n", argv[0], argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDWR | O_NOCTTY);
    if (fd < 0) { perror("open"); return 1; }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) { perror("tcgetattr"); close(fd); return 1; }

    cfsetispeed(&tty, B38400);
    cfsetospeed(&tty, B38400);

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_iflag = 0;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1; // 0.1s

    if (tcsetattr(fd, TCSANOW, &tty) != 0) { perror("tcsetattr"); close(fd); return 1; }

    char resp[2048];

    // Init
    send_cmd(fd, "ATZ",  resp, sizeof(resp));
    send_cmd(fd, "ATE0", resp, sizeof(resp));
    send_cmd(fd, "ATL0", resp, sizeof(resp));
    send_cmd(fd, "ATS0", resp, sizeof(resp));
    send_cmd(fd, "ATH0", resp, sizeof(resp));
    send_cmd(fd, "ATSP0",resp, sizeof(resp));

    for (;;) {
        if (send_cmd(fd, "010C", resp, sizeof(resp)) < 0) { perror("send_cmd"); break; }

        float rpm;
        if (parse_rpm(resp, &rpm) == 0) printf("RPM: %.1f\n", rpm);
        else printf("Parse failed. Raw:\n%s\n", resp);

        usleep(200000);
    }

    close(fd);
    return 0;
}

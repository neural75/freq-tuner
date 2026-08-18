/*
 * freq-tuner - evdev knob capability probe.
 *
 * Opens an evdev node READ-ONLY and reports whether it provides the
 * volume-knob keys (KEY_MUTE / KEY_VOLUMEDOWN / KEY_VOLUMEUP). It never
 * grabs the device (no EVIOCGRAB), so it is safe to run against a device
 * that the keyboard grabber has already grabbed. install.sh uses it to
 * pick out the virtual knob device among /dev/input/event*.
 *
 * Exit: 0 and "knob\n" on stdout when the keys are present, 1 otherwise.
 *
 * SPDX-License-Identifier: MIT
 */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int has_key(const unsigned char *bits, unsigned int key)
{
    return (bits[key / 8] & (1u << (key % 8))) != 0;
}

int main(int argc, char **argv)
{
    unsigned char bits[KEY_MAX / 8 + 1];
    int fd;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /dev/input/eventN\n", argv[0]);
        return 2;
    }

    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }

    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) {
        perror("EVIOCGBIT");
        close(fd);
        return 1;
    }
    close(fd);

    if (has_key(bits, KEY_MUTE) &&
        has_key(bits, KEY_VOLUMEDOWN) &&
        has_key(bits, KEY_VOLUMEUP)) {
        puts("knob");
        return 0;
    }
    puts("no");
    return 1;
}
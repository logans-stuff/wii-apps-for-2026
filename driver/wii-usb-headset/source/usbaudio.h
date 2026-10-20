/* USB Audio Class 1.0 headset driver for the Wii (libogc USB, IOS iso).
 * Made for a headset with 48 kHz mono 16-bit microphone and 48 kHz
 * stereo 16-bit headphones, found with build/probe.dol. */
#ifndef SONORAN_USBAUDIO_H
#define SONORAN_USBAUDIO_H

#include <stddef.h>
#include <stdint.h>

#define UA_RATE 48000

/* Find and open the first USB audio headset. Return 0 on success. */
int ua_open(char *info, size_t infolen, char *err, size_t errlen);
void ua_close(void);

/* Record mono samples from the microphone until max_samples, or until
 * keep_going() returns 0 (may be NULL). Return the number of samples. */
int ua_record(int16_t *out, int max_samples, int (*keep_going)(void));

/* Play mono samples on both headphone channels. Return 0 on success. */
int ua_play(const int16_t *in, int samples);

#endif

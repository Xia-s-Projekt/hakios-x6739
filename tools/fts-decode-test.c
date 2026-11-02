/*
 * Check the FT3681 report decoding against frames captured from the phone.
 *
 * The capture (tools/fts-capture.sh) holds two logs: the raw report bytes as the phone's own
 * driver saw them, and Android's evdev stream for the same moment. The test decodes every
 * captured report with mtkloader/fts_report.h -- the same code the loader runs -- and counts how
 * many decoded points are among the coordinates Android reported. The release frames carry the
 * last point's coordinates rather than a new one, so they are expected to miss.
 *
 *   tools/fts-decode-test.c <capture directory>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../mtkloader/fts_report.h"

struct coord {
	uint32_t x, y;
};

static int coord_cmp(const void *a, const void *b)
{
	const struct coord *ca = a, *cb = b;

	if (ca->x != cb->x) {
		return ca->x < cb->x ? -1 : 1;
	}
	if (ca->y != cb->y) {
		return ca->y < cb->y ? -1 : 1;
	}
	return 0;
}

/* the evdev coordinates: "ABS_MT_POSITION_X 000015fc" and the Y line that follows */
static struct coord *read_evdev(const char *path, uint32_t *count)
{
	FILE *f = fopen(path, "r");
	struct coord *out = NULL;
	uint32_t n = 0, cap = 0;
	uint32_t x = 0;
	char line[512];

	if (!f) {
		fprintf(stderr, "cannot open %s\n", path);
		return NULL;
	}
	while (fgets(line, sizeof(line), f)) {
		char *p;

		p = strstr(line, "POSITION_X");
		if (p) {
			x = (uint32_t)strtoul(strpbrk(p, " \t") + 1, NULL, 16);
			continue;
		}
		p = strstr(line, "POSITION_Y");
		if (p && x) {
			uint32_t y = (uint32_t)strtoul(strpbrk(p, " \t") + 1, NULL, 16);

			if (n == cap) {
				cap = cap ? cap * 2 : 256;
				out = realloc(out, cap * sizeof(*out));
				if (!out) {
					fclose(f);
					return NULL;
				}
			}
			out[n].x = x;
			out[n].y = y;
			n++;
			x = 0;
		}
	}
	fclose(f);
	qsort(out, n, sizeof(*out), coord_cmp);
	*count = n;
	return out;
}

static int has_coord(const struct coord *set, uint32_t n, uint32_t x, uint32_t y)
{
	struct coord key = { x, y };

	return bsearch(&key, set, n, sizeof(*set), coord_cmp) != NULL;
}

int main(int argc, char **argv)
{
	char path[512];
	FILE *f;
	char line[1024];
	struct coord *evdev;
	uint32_t evdev_n = 0;
	uint32_t frames = 0, idle = 0, points = 0, matched = 0, contact_matched = 0;
	uint32_t offset_wrong = 0;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <capture directory>\n", argv[0]);
		return 2;
	}
	snprintf(path, sizeof(path), "%s/event4.log", argv[1]);
	evdev = read_evdev(path, &evdev_n);
	if (!evdev) {
		return 1;
	}
	snprintf(path, sizeof(path), "%s/fts_touch_point.log", argv[1]);
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "cannot open %s\n", path);
		return 1;
	}
	while (fgets(line, sizeof(line), f)) {
		char *p = strstr(line, "buffer:");
		uint8_t report[FTS_REPORT_LEN];
		struct fts_point pts[FTS_MAX_POINTS];
		uint32_t n;

		if (!p) {
			continue;
		}
		p += 7;
		for (uint32_t i = 0; i < FTS_REPORT_LEN; i++) {
			char *end;
			unsigned long v = strtoul(p, &end, 16);

			if (end == p) {
				break;
			}
			report[i] = (uint8_t)v;
			p = end;
		}
		frames++;
		if (fts_report_is_idle(report)) {
			idle++;
			continue;
		}
		n = fts_decode_report(report, pts, FTS_MAX_POINTS);
		points += n;
		for (uint32_t i = 0; i < n; i++) {
			if (has_coord(evdev, evdev_n, pts[i].x, pts[i].y)) {
				matched++;
				if (pts[i].event == 2) {
					contact_matched++;
				}
			}
		}
		/* the same bytes, decoded at the wrong offset, must not match anything */
		for (uint32_t off = 1; off <= 3; off += 2) {
			uint8_t shifted[FTS_REPORT_LEN];

			memcpy(shifted, report, sizeof(shifted));
			for (uint32_t i = 0; i < FTS_REPORT_LEN - off; i++) {
				shifted[i] = shifted[i + off];
			}
			n = fts_decode_report(shifted, pts, FTS_MAX_POINTS);
			for (uint32_t i = 0; i < n; i++) {
				if (has_coord(evdev, evdev_n, pts[i].x, pts[i].y)) {
					offset_wrong++;
				}
			}
		}
	}
	fclose(f);

	printf("frames %u, idle %u, points %u\n", frames, idle, points);
	printf("matching an Android evdev coordinate: %u of %u\n", matched, points);
	printf("  of the contact events (event 2): %u\n", contact_matched);
	printf("matches from decoding at the wrong offset: %u (want 0)\n", offset_wrong);
	free(evdev);
	return (matched > 0 && offset_wrong == 0) ? 0 : 1;
}

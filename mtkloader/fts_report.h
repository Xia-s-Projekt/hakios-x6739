/*
 * The FocalTech FT3681 touch report, decoded. Shared by the loader (mtkloader.c) and by
 * tools/fts-decode-test.c, so the host test exercises the code that runs on the phone.
 *
 * The report is 62 bytes, read after writing 0x01: two header bytes and ten six-byte points, the
 * point block starting at byte 2. A point is
 *
 *   b0: bits 7.6 = event (2 = contact), bits 3.0 = x[15:12]
 *   b1: x[11:4]
 *   b2: bits 7.4 = finger id, bits 3.0 = y[15:12]
 *   b3: y[11:4]
 *   b4: bits 7.4 = x[3:0], bits 3.0 = y[3:0]
 *   b5: area
 *
 * so x = ((b0 & 0x0f) << 12) | (b1 << 4) | (b4 >> 4) and y = ((b2 & 0x0f) << 12) | (b3 << 4) |
 * (b4 & 0x0f). The coordinates are the panel's 1080 x 2400 in sixteenths of a pixel.
 *
 * Byte 1's high nibble is the report type and its low nibble the number of points, and the point
 * block then holds exactly that many slots -- except when the count is zero, which is the release
 * frame: it still carries the last point's bytes, so a zero count means "no contact now". A point
 * slot is unused when its b2 is above 0x9f. A report that is all 0xff carries no data at all.
 *
 * Read back from the phone on 2026-11-01: of 455 decoded points, 439 matched Android's own evdev
 * coordinates exactly, and the ones that did not match were release frames carrying stale
 * coordinates. Decoding at offset 1 or 3, or with the 12-bit branch, matched nothing.
 */
#ifndef FTS_REPORT_H
#define FTS_REPORT_H

#include <stdint.h>

#define FTS_REG_REPORT            0x01u
#define FTS_REPORT_LEN            62u
#define FTS_REPORT_HEADER         2u
#define FTS_POINT_LEN             6u
#define FTS_MAX_POINTS            10u

struct fts_point {
	uint32_t event;     /* bits 7.6 of b0: 2 = contact */
	uint32_t id;        /* bits 7.4 of b2 */
	uint32_t x;
	uint32_t y;
	uint32_t area;      /* b5 */
};

/* 1 when every byte of the report is 0xff: the panel has nothing to say. */
static inline int fts_report_is_idle(const uint8_t *report)
{
	for (uint32_t i = 0; i < FTS_REPORT_LEN; i++) {
		if (report[i] != 0xff) {
			return 0;
		}
	}
	return 1;
}

/* the number of points the header says, and the event byte of the first point */
static inline uint32_t fts_report_count(const uint8_t *report)
{
	return (uint32_t)(report[1] & 0x0fu);
}

static inline uint32_t fts_report_type(const uint8_t *report)
{
	return (uint32_t)(report[1] >> 4);
}

/* decode one six-byte point */
static inline void fts_decode_point(const uint8_t *p, struct fts_point *out)
{
	uint32_t b0 = p[0], b1 = p[1], b2 = p[2], b3 = p[3], b4 = p[4];

	out->event = (b0 >> 6) & 3u;
	out->id = (b2 >> 4) & 0xfu;
	out->x = ((b0 & 0x0fu) << 12) | (b1 << 4) | (b4 >> 4);
	out->y = ((b2 & 0x0fu) << 12) | (b3 << 4) | (b4 & 0x0fu);
	out->area = p[5];
}

/*
 * The points of a report, in order. Returns how many there are. A slot is unused when its b2 is
 * above 0x9f, and that is where the slot list ends.
 */
static inline uint32_t fts_decode_report(const uint8_t *report, struct fts_point *out, uint32_t max)
{
	uint32_t n = 0;

	for (uint32_t i = 0; i < FTS_MAX_POINTS && i < max; i++) {
		const uint8_t *p = &report[FTS_REPORT_HEADER + i * FTS_POINT_LEN];

		if (p[2] > 0x9f) {
			break;
		}
		fts_decode_point(p, &out[n]);
		n++;
	}
	return n;
}

#endif /* FTS_REPORT_H */

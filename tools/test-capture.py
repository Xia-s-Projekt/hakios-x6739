#!/usr/bin/env python3
"""Test the loader's log capture (mtkloader/mtkloader.c save_log).

The XNU log does not fit in the 256 KB the ramoops console keeps, so save_log() keeps the head
of the boot, an optional window from the middle (XNU_LOG_OFFSET, XNU_LOG_LEN) and the end of
the run. This checks what it keeps, by compiling the real function with stubbed helpers.

usage: python3 tools/test-capture.py
"""
import base64
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

STUBS = r"""
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLOG_CAP 262132
static struct { uint64_t size; } hdr;
#define PLOG_HDR (&hdr)
static struct { uint64_t base; } fbc;
static char out[300000];
static unsigned used;

static void plog_putc(char c) { out[used++] = c; }
/* the real dbg_puts goes through dbg_putc, which goes to the log as well as the screen */
static void dbg_puts(const char *s) { while (*s) { plog_putc(*s++); } }
static void dbg_dec(uint64_t v) { char b[21]; int n = 0; do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) { plog_putc(b[--n]); } }
"""

MAIN = r"""
int main(int argc, char **argv)
{
	long total = atol(argv[1]);
	long span = atol(argv[4]);
	char *data = malloc(total);
	for (long i = 0; i < total; i++) {
		data[i] = (char)('a' + (i % 26));
	}
	if (argc > 5) {
		const char *message = "\nusertrap: lsd[30] fault pc 0x1234\nustacks: lsd[30] sample\n";
		memcpy(data + 400000, message, strlen(message));
		memcpy(data + 600000, "\nusertrap:", 10);
		memset(data + 600010, 'x', 1500);
		data[601510] = '\n';
	}
	struct span sp[2];
	int n = 0;
	long first = span < total ? span : total;
	sp[n].p = data;
	sp[n].n = first;
	n++;
	if (total > first) {
		sp[n].p = data + first;
		sp[n].n = total - first;
		n++;
	}
	used = 0;
	hdr.size = 0;
	fbc.base = 42;
	save_log(sp, n);

	char *p = strstr(out, "=== XNU log window: bytes ");
	if (p) {
		long b = 0, e = 0;
		sscanf(p, "=== XNU log window: bytes %ld..%ld", &b, &e);
		printf("window %ld %ld\n", b, e);
	}
	printf("used %u\n", used);
	printf("head %d\n", out[0] == 'a');
	printf("last %d\n", out[used - 2] == data[total - 1]);
	printf("fb %d\n", fbc.base == 42);
	printf("newline %d\n", out[used - 1] == '\n');
	if (argc > 5) {
		assert(strstr(out, "usertrap: lsd[30] fault pc 0x1234\n"));
		assert(strstr(out, "ustacks: lsd[30] sample\n"));
		assert(strstr(out, "preserved diagnostic lines; dropped 1"));
		assert(used + 16384 < PLOG_CAP);
		puts("diagnostics 1");
	}
	return 0;
}
"""


def extract_save_log() -> str:
	src = (ROOT / "mtkloader" / "mtkloader.c").read_text()
	start = src.index("static void save_log(const struct span *sp, int nsp)")
	end = src.index("/* The last lines of what is in ramoops", start)
	return src[start:end]


def run(cflags: list[str], total: int, offset: int, length: int, span: int, diagnostics: bool = False) -> dict:
	with tempfile.TemporaryDirectory(dir=ROOT / "mtkloader") as tmp:
		src = STUBS + "struct span { const volatile char *p; uint64_t n; };\n" + extract_save_log() + MAIN
		binary = Path(tmp) / "test"
		subprocess.run(
			["clang", "-Wall", "-Wextra", "-Werror", "-x", "c", "-", "-o", str(binary), *cflags],
			input=src,
			text=True,
			check=True,
		)
		out = subprocess.run(
			[str(binary), str(total), str(offset), str(length), str(span)] + (["diagnostics"] if diagnostics else []),
			capture_output=True,
			text=True,
			check=True,
		).stdout
	res = {"window": None}
	for line in out.splitlines():
		k, _, v = line.partition(" ")
		if k == "window":
			res["window"] = tuple(int(x) for x in v.split())
		else:
			res[k] = int(v)
	return res


def check(total: int, offset: int, length: int, span: int, expect_window: bool) -> None:
	cflags = []
	if expect_window:
		cflags = [f"-DXNU_LOG_OFFSET={offset}", f"-DXNU_LOG_LEN={length}"]
	r = run(cflags, total, offset, length, span)
	assert r["used"] <= 262132 - 1024, r
	assert r["fb"] == 1 and r["newline"] == 1, r
	assert r["head"] == 1, r
	if expect_window and total > 262132 - 1024:
		assert r["window"] is not None, r
		b, e = r["window"]
		assert b >= min(offset, total), (b, offset, total)
		assert e - b <= length, (b, e, length)
		assert e <= total, (e, total)
	print(f"ok: total={total} offset={offset} len={length} span={span} window={r['window']}")


def check_panel() -> None:
	src = (ROOT / "mtkloader" / "mtkloader.c").read_text()
	start = src.index("#define SHOT_STEP")
	end = src.index("/*\n * XNU is back", start)
	stubs = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#define PANEL_W 1080
#define PANEL_H 2400
static uint64_t fb_base;
static uint32_t fb_pitch;
static struct { uint64_t base; } fbc;
static void dbg_puts(const char *s) { fputs(s, stdout); }
static void dbg_putc(char c) { putchar(c); }
static void dbg_dec(uint64_t n) { printf("%llu", (unsigned long long)n); }
"""
	main = r"""
int main(void) {
	uint32_t *fb = calloc(PANEL_H, 4352);
	assert(fb);
	fb_base = (uintptr_t)fb;
	fb_pitch = 4352;
	for (unsigned y = 0; y < SHOT_H; y++) {
		for (unsigned x = 0; x < SHOT_W; x++) {
			fb[(y * SHOT_STEP + SHOT_STEP / 2) * (4352 / 4) + x * SHOT_STEP + SHOT_STEP / 2] = 0xff123456;
		}
	}
	shot_panel(shot_pixels);
	for (unsigned i = 0; i < SHOT_BYTES; i += 3) {
		assert(shot_pixels[i] == 0x12 && shot_pixels[i + 1] == 0x34 && shot_pixels[i + 2] == 0x56);
	}
	fbc.base = 123;
	shot_print(3, "test", shot_pixels);
	assert(fbc.base == 123);
	free(fb);
	return 0;
}
"""
	with tempfile.TemporaryDirectory(dir=ROOT / "mtkloader") as tmp:
		binary = Path(tmp) / "panel-test"
		subprocess.run(
			["clang", "-Wall", "-Wextra", "-Werror", "-x", "c", "-", "-o", str(binary)],
			input=stubs + src[start:end] + main, text=True, check=True,
		)
		out = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout.splitlines()
	assert out[0] == "fbshot 3: begin 40 88 (test)" and out[-1] == "fbshot 3: end"
	pixels = b"".join(base64.b64decode(line.split(": ", 1)[1]) for line in out[1:-1])
	assert pixels == bytes.fromhex("123456") * 3520
	print("ok: panel RGB, padded pitch, base64 round-trip, console restoration")


def main() -> int:
	check_panel()
	# no window: the head and the tail, as before
	r = run([], 1000000, 0, 0, 400000)
	assert r["window"] is None, r
	assert r["used"] <= 262132 - 1024, r
	print(f"ok: head+tail only, used={r['used']}")
	# a window in the middle, over one span and over two
	for total, offset, length, span in [
		(1000000, 200000, 262144, 400000),
		(1000000, 200000, 262144, 1000000),
		(1000000, 0, 262144, 400000),
		(1000000, 990000, 262144, 400000),
		(1000000, 1200000, 262144, 400000),
		(1037521, 305030, 262144, 400000),
	]:
		check(total, offset, length, span, True)
	for split in (400006, 400040, 600005, 1000000):
		r = run([], 1000000, 0, 0, split, diagnostics=True)
		assert r["diagnostics"] == 1
		print(f"ok: diagnostics retained across span split {split}")
	print("all capture tests passed")
	return 0


if __name__ == "__main__":
	sys.exit(main())

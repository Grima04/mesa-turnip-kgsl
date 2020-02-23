/*
 * Copyright © 2020 Google, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <getopt.h>
#include <xf86drm.h>

#include "util/u_math.h"

#include "main.h"


static void
dump_float(void *buf, int sz)
{
	uint8_t *ptr = (uint8_t *)buf;
	uint8_t *end = ptr + sz - 3;
	int i = 0;

	while (ptr < end) {
		uint32_t d = 0;

		printf((i % 8) ? " " : "\t");

		d |= *(ptr++) <<  0;
		d |= *(ptr++) <<  8;
		d |= *(ptr++) << 16;
		d |= *(ptr++) << 24;

		printf("%8f", uif(d));

		if ((i % 8) == 7) {
			printf("\n");
		}

		i++;
	}

	if (i % 8) {
		printf("\n");
	}
}

static void
dump_hex(void *buf, int sz)
{
	uint8_t *ptr = (uint8_t *)buf;
	uint8_t *end = ptr + sz;
	int i = 0;

	while (ptr < end) {
		uint32_t d = 0;

		printf((i % 8) ? " " : "\t");

		d |= *(ptr++) <<  0;
		d |= *(ptr++) <<  8;
		d |= *(ptr++) << 16;
		d |= *(ptr++) << 24;

		printf("%08x", d);

		if ((i % 8) == 7) {
			printf("\n");
		}

		i++;
	}

	if (i % 8) {
		printf("\n");
	}
}

static const char *shortopts = "df:g:h";

static const struct option longopts[] = {
	{"disasm",   no_argument,       0, 'd'},
	{"file",     required_argument, 0, 'f'},
	{"groups",   required_argument, 0, 'g'},
	{"help",     no_argument,       0, 'h'},
	{0, 0, 0, 0}
};

static void
usage(const char *name)
{
	printf("Usage: %s [-dfgh]\n"
		"\n"
		"options:\n"
		"    -d, --disasm             print disassembled shader\n"
		"    -f, --file=FILE          read shader from file (instead of stdin)\n"
		"    -g, --groups=X,Y,Z       use specified group size\n"
		"    -h, --help               show this message\n"
		,
		name);
}

int
main(int argc, char **argv)
{
	FILE *in = stdin;
	bool disasm = false;
	uint32_t grid[3] = {0};
	int opt, ret;

	while ((opt = getopt_long_only(argc, argv, shortopts, longopts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			disasm = true;
			break;
		case 'f':
			in = fopen(optarg, "r");
			if (!in)
				err(1, "could not open '%s'", optarg);
			break;
		case 'g':
			ret = sscanf(optarg, "%u,%u,%u", &grid[0], &grid[1], &grid[2]);
			if (ret != 3)
				goto usage;
			break;
		case 'h':
			goto usage;
		default:
			printf("unrecognized arg: %c\n", opt);
			goto usage;
		}
	}

	int fd = drmOpen("msm", NULL);
	if (fd < 0)
		err(1, "could not open drm device");

	struct fd_device *dev = fd_device_new(fd);
	struct fd_pipe *pipe = fd_pipe_new(dev, FD_PIPE_3D);

	uint64_t val;
	fd_pipe_get_param(pipe, FD_GPU_ID, &val);
	uint32_t gpu_id = val;

	printf("got gpu_id: %u\n", gpu_id);

	struct backend *backend;
	switch (gpu_id) {
	case 600 ... 699:
		backend = a6xx_init(dev, gpu_id);
		break;
	default:
		err(1, "unsupported gpu: a%u", gpu_id);
	}

	struct kernel *kernel = backend->assemble(backend, in);
	printf("localsize: %dx%dx%d\n", kernel->local_size[0],
			kernel->local_size[1], kernel->local_size[2]);
	for (int i = 0; i < kernel->num_bufs; i++) {
		printf("buf[%d]: size=%u\n", i, kernel->buf_sizes[i]);
		kernel->bufs[i] = fd_bo_new(dev, kernel->buf_sizes[i] * 4,
				DRM_FREEDRENO_GEM_TYPE_KMEM, "buf[%d]", i);
	}

	if (disasm)
		backend->disassemble(kernel, stdout);

	if (grid[0] == 0)
		return 0;

	struct fd_submit *submit = fd_submit_new(pipe);

	backend->emit_grid(kernel, grid, submit);

	fd_submit_flush(submit, -1, NULL, NULL);

	for (int i = 0; i < kernel->num_bufs; i++) {
		fd_bo_cpu_prep(kernel->bufs[i], pipe, DRM_FREEDRENO_PREP_READ);
		void *map = fd_bo_map(kernel->bufs[i]);

		printf("buf[%d]:\n", i);
		dump_hex(map, kernel->buf_sizes[i] * 4);
		dump_float(map, kernel->buf_sizes[i] * 4);
	}

	return 0;

usage:
	usage(argv[0]);
	return -1;
}

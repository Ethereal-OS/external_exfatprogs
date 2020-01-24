// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2019 Namjae Jeon <linkinjeon@gmail.com>
 *   Copyright (C) 2020 Hyunchul Lee <hyc.lee@gmail.com>
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>

#include "exfat_ondisk.h"
#include "exfat_tools.h"
#include "list.h"

#define EXFAT_CLUSTER_SIZE(pbr) (1 << ((pbr)->bsx.sect_size_bits +	\
					(pbr)->bsx.sect_per_clus_bits))
#define EXFAT_SECTOR_SIZE(pbr) (1 << (pbr)->bsx.sect_size_bits)

enum fsck_ui_options {
	FSCK_OPTS_REPAIR	= 0x01,
};

struct fsck_user_input {
	struct exfat_user_input		ei;
	enum fsck_ui_options		options;
};

typedef __u32 clus_t;

enum exfat_file_attr {
	EXFAT_FA_NONE		= 0x00,
	EXFAT_FA_DIR		= 0x01,
};

struct exfat_node {
	struct exfat_node	*parent;
	struct list_head	children;
	struct list_head	sibling;
	struct list_head	list;
	clus_t			first_clus;
	__u16			attr;
	__u64			size;
	bool			is_contiguous;
	off_t			dentry_file_offset;
	__le16			name[0];	/* only for directory */
};

#define EXFAT_NAME_MAX		255
#define UTF16_NAME_BUFFER_SIZE	((EXFAT_NAME_MAX + 1) * sizeof(__le16))
#define UTF8_NAME_BUFFER_SIZE	(EXFAT_NAME_MAX * 3 + 1)

struct exfat {
	struct exfat_blk_dev	*blk_dev;
	struct pbr		*bs;
	char			volume_label[VOLUME_LABEL_MAX_LEN*3+1];
	struct exfat_node	*root;
	struct list_head	dir_list;
	__u32			*alloc_bitmap;
	__u64			bit_count;
};

struct exfat_stat {
	long		dir_count;
	long		file_count;
	long		dir_free_count;
	long		file_free_count;
};

struct path_resolve_ctx {
	struct exfat_node	*ancestors[255];
	__le16			utf16_path[sizeof(__le16) * (PATH_MAX + 2)];
	char			utf8_path[PATH_MAX * 3 + 1];
};

struct exfat_stat exfat_stat;
struct path_resolve_ctx path_resolve_ctx;

static struct option opts[] = {
	{"version",	no_argument,	NULL,	'V' },
	{"verbose",	no_argument,	NULL,	'v' },
	{"help",	no_argument,	NULL,	'h' },
	{"?",		no_argument,	NULL,	'?' },
	{NULL,		0,		NULL,	 0  }
};

static void usage(char *name)
{
	fprintf(stderr, "Usage: %s\n", name);
	fprintf(stderr, "\t-r | --repair	Repair\n");
	fprintf(stderr, "\t-V | --version	Show version\n");
	fprintf(stderr, "\t-v | --verbose	Print debug\n");
	fprintf(stderr, "\t-h | --help		Show help\n");

	exit(EXIT_FAILURE);
}

static struct exfat_node *alloc_exfat_node(__u16 attr)
{
	struct exfat_node *node;
	int size;

	size = offsetof(struct exfat_node, name) + UTF16_NAME_BUFFER_SIZE;
	node = (struct exfat_node *)calloc(1, size);
	if (!node) {
		exfat_err("failed to allocate exfat_node\n");
		return NULL;
	}

	node->parent = NULL;
	INIT_LIST_HEAD(&node->children);
	INIT_LIST_HEAD(&node->sibling);
	INIT_LIST_HEAD(&node->list);

	if (attr & ATTR_SUBDIR)
		exfat_stat.dir_count++;
	else
		exfat_stat.file_count++;
	node->attr = attr;
	return node;
}

static void free_exfat_node(struct exfat_node *node)
{
	if (node->attr & ATTR_SUBDIR)
		exfat_stat.dir_free_count++;
	else
		exfat_stat.file_free_count++;
	free(node);
}

static void node_free_children(struct exfat_node *dir, bool file_only)
{
	struct exfat_node *node, *i;

	list_for_each_entry_safe(node, i, &dir->children, sibling) {
		if (file_only) {
			if (!(node->attr & ATTR_SUBDIR)) {
				list_del(&node->sibling);
				free_exfat_node(node);
			}
		} else {
			list_del(&node->sibling);
			list_del(&node->list);
			free_exfat_node(node);
		}
	}
}

static void node_free_file_children(struct exfat_node *dir)
{
	node_free_children(dir, true);
}

/* delete @child and all ancestors that does not have
 * children
 */
static void node_free_ancestors(struct exfat_node *child)
{
	struct exfat_node *parent, *node;

	if (!list_empty(&child->children))
		return;

	do {
		if (!(child->attr & ATTR_SUBDIR)) {
			exfat_err("not directory.\n");
			return;
		}

		parent = child->parent;
		list_del(&child->sibling);
		free_exfat_node(child);

		child = parent;
	} while (child && list_empty(&child->children));

	return;
}

static struct exfat *alloc_exfat(struct exfat_blk_dev *bd)
{
	struct exfat *exfat;

	exfat = (struct exfat *)calloc(1, sizeof(*exfat));
	if (!exfat) {
		exfat_err("failed to allocate exfat\n");
		return NULL;
	}

	exfat->blk_dev = bd;
	INIT_LIST_HEAD(&exfat->dir_list);
	return exfat;
}

static void free_exfat(struct exfat *exfat)
{
	if (exfat) {
		if (exfat->bs)
			free(exfat->bs);
		free(exfat);
	}
}

static void exfat_free_dir_list(struct exfat *exfat)
{
	struct exfat_node *dir, *file, *i, *k;

	list_for_each_entry_safe(dir, i, &exfat->dir_list, list) {
		node_free_file_children(dir);
		list_del(&dir->list);
		free_exfat_node(dir);
	}
}

static inline bool exfat_invalid_clus(struct exfat *exfat, clus_t clus)
{
	return clus < EXFAT_FIRST_CLUSTER ||
	(clus - EXFAT_FIRST_CLUSTER) > le32_to_cpu(exfat->bs->bsx.clu_count);
}

static int node_get_clus_next(struct exfat *exfat, struct exfat_node *node,
				clus_t clus, clus_t *next)
{
	off_t offset;

	if (exfat_invalid_clus(exfat, clus))
		return -EINVAL;

	if (node->is_contiguous) {
		*next = clus + 1;
		return 0;
	}

	offset = le32_to_cpu(exfat->bs->bsx.fat_offset) <<
				exfat->bs->bsx.sect_size_bits;
	offset += sizeof(clus_t) * clus;

	if (exfat_read(exfat->blk_dev->dev_fd, next, sizeof(*next), offset)
			!= sizeof(*next))
		return -EIO;
	*next = le32_to_cpu(*next);
	return 0;
}

static bool node_get_clus_count(struct exfat *exfat, struct exfat_node *node,
							clus_t *clus_count)
{
	clus_t clus;

	clus = node->first_clus;
	*clus_count = 0;

	do {
		if (exfat_invalid_clus(exfat, clus)) {
			exfat_err("bad cluster. 0x%x\n", clus);
			return false;
		}

		if (node_get_clus_next(exfat, node, clus, &clus) != 0) {
			exfat_err(
				"broken cluster chain. (previous cluster 0x%x)\n",
				clus);
			return false;
		}

		(*clus_count)++;
	} while (clus != EXFAT_EOF_CLUSTER);
	return true;
}

static int boot_region_checksum(struct exfat *exfat)
{
	__le32 checksum;
	unsigned short size;
	void *sect;
	int i;

	size = EXFAT_SECTOR_SIZE(exfat->bs);
	sect = malloc(size);
	if (!sect)
		return -ENOMEM;

	checksum = 0;

	boot_calc_checksum((unsigned char *)exfat->bs, size, true, &checksum);
	for (i = 1; i < 11; i++) {
		if (exfat_read(exfat->blk_dev->dev_fd, sect, size, i * size) !=
				(ssize_t)size) {
			free(sect);
			return -EIO;
		}
		boot_calc_checksum(sect, size, false, &checksum);
	}

	if (exfat_read(exfat->blk_dev->dev_fd, sect, size, i * size) !=
			(ssize_t)size) {
		free(sect);
		return -EIO;
	}
	for (i = 0; i < size/sizeof(checksum); i++) {
		if (le32_to_cpu(((__le32 *)sect)[i]) != checksum) {
			exfat_err("invalid checksum. 0x%x\n",
					le32_to_cpu(((__le32 *)sect)[i]));
			free(sect);
			return -EIO;
		}
	}

	free(sect);
	return 0;
}

static bool exfat_boot_region_check(struct exfat *exfat)
{
	struct pbr *bs;
	ssize_t ret;

	bs = (struct pbr *)malloc(sizeof(struct pbr));
	if (!bs) {
		exfat_err("failed to allocate memory\n");
		return false;
	}

	exfat->bs = bs;

	ret = exfat_read(exfat->blk_dev->dev_fd, bs, sizeof(*bs), 0);
	if (ret != sizeof(*bs)) {
		exfat_err("failed to read a boot sector. %ld\n", ret);
		goto err;
	}

	if (memcmp(bs->bpb.oem_name, "EXFAT   ", 8) != 0) {
		exfat_err("failed to find exfat file system.\n");
		goto err;
	}

	if (EXFAT_SECTOR_SIZE(bs) < 512) {
		exfat_err("too small sector size: %d\n", EXFAT_SECTOR_SIZE(bs));
		goto err;
	}

	if (EXFAT_CLUSTER_SIZE(bs) > 32U * 1024 * 1024) {
		exfat_err("too big cluster size: %d\n", EXFAT_CLUSTER_SIZE(bs));
		goto err;
	}

	ret = boot_region_checksum(exfat);
	if (ret) {
		exfat_err("failed to verify the checksum of a boot region. %ld\n",
			ret);
		goto err;
	}

	if (bs->bsx.fs_version[1] != 1 || bs->bsx.fs_version[0] != 0) {
		exfat_err("unsupported exfat version: %d.%d\n",
				bs->bsx.fs_version[1], bs->bsx.fs_version[0]);
		goto err;
	}

	if (bs->bsx.num_fats != 1) {
		exfat_err("unsupported FAT count: %d\n", bs->bsx.num_fats);
		goto err;
	}

	if (le64_to_cpu(bs->bsx.vol_length) * EXFAT_SECTOR_SIZE(bs) >
			exfat->blk_dev->size) {
		exfat_err("too large sector count: %llu\n, expected: %llu\n",
				le64_to_cpu(bs->bsx.vol_length),
				exfat->blk_dev->num_sectors);
		goto err;
	}

	if (le32_to_cpu(bs->bsx.clu_count) * EXFAT_CLUSTER_SIZE(bs) >
			exfat->blk_dev->size) {
		exfat_err("too large cluster count: %u, expected: %u\n",
				le32_to_cpu(bs->bsx.clu_count),
				exfat->blk_dev->num_clusters);
		goto err;
	}

	return true;
err:
	free(bs);
	exfat->bs = NULL;
	return false;
}

/*
 * get references of ancestors that include @child until the count of
 * ancesters is not larger than @count and the count of characters of
 * their names is not larger than @max_char_len.
 * return true if root is reached.
 */
bool get_ancestors(struct exfat_node *child,
		struct exfat_node **ancestors, int count,
		int max_char_len,
		int *ancestor_count)
{
	struct exfat_node *dir;
	int name_len, char_len;
	int root_depth, depth, i;

	root_depth = 0;
	char_len = 0;
	max_char_len += 1;

	dir = child;
	while (dir) {
		name_len = utf16_length(dir->name);
		if (char_len + name_len > max_char_len)
			break;

		/* include '/' */
		char_len += name_len + 1;
		root_depth++;

		dir = dir->parent;
	}

	depth = MIN(root_depth, count);

	for (dir = child, i = depth - 1; i >= 0; dir = dir->parent, i--)
		ancestors[i] = dir;

	*ancestor_count = depth;
	return dir == NULL;
}

static int resolve_path(struct path_resolve_ctx *ctx,
						struct exfat_node *child)
{
	int ret = 0;
	int depth, i;
	int name_len, path_len;
	__le16 *utf16_path;

	ctx->utf8_path[0] = '\0';

	get_ancestors(child,
			ctx->ancestors,
			sizeof(ctx->ancestors) / sizeof(ctx->ancestors[0]),
			PATH_MAX,
			&depth);

	utf16_path = ctx->utf16_path;
	for (i = 0; i < depth; i++) {
		name_len = utf16_length(ctx->ancestors[i]->name);
		memcpy((char *)utf16_path, (char *)ctx->ancestors[i]->name,
				name_len * 2);
		utf16_path += name_len;
		memcpy((char *)utf16_path, u"/", 2);
		utf16_path += 1;
	}

	ret = utf16_to_utf8(ctx->utf8_path, ctx->utf16_path,
		sizeof(ctx->utf8_path), utf16_path - ctx->utf16_path - 1);
	return ret;
}

static int resolve_path_parent(struct path_resolve_ctx *ctx,
			struct exfat_node *parent, struct exfat_node *child)
{
	int ret;
	struct exfat_node *old;

	old = child->parent;
	child->parent = parent;

	ret = resolve_path(ctx, child);
	child->parent = old;
	return ret;
}

static int read_children(struct exfat *exfat, struct exfat_node *dir)
{
	return -1;
}

/*
 * for each directory in @dir_list.
 * 1. read all dentries and allocate exfat_nodes for files and directories.
 *    and append directory exfat_nodes to the head of @dir_list
 * 2. free all of file exfat_nodes.
 * 3. if the directory does not have children, free its exfat_node.
 */
static bool exfat_filesystem_check(struct exfat *exfat)
{
	struct exfat_node *dir;
	int ret;

	if (!exfat->root) {
		exfat_err("root is NULL\n");
		return false;
	}

	list_add(&exfat->root->list, &exfat->dir_list);

	while (!list_empty(&exfat->dir_list)) {
		dir = list_entry(exfat->dir_list.next, struct exfat_node, list);

		if (!(dir->attr & ATTR_SUBDIR)) {
			resolve_path(&path_resolve_ctx, dir);
			exfat_err("failed to travel directories. "
					"the node is not directory: %s\n",
					path_resolve_ctx.utf8_path);
			goto out;
		}

		if (read_children(exfat, dir)) {
			resolve_path(&path_resolve_ctx, dir);
			exfat_err("failed to check dentries: %s\n",
					path_resolve_ctx.utf8_path);
			goto out;
		}

		list_del(&dir->list);
		node_free_file_children(dir);
		node_free_ancestors(dir);
	}
out:
	exfat_free_dir_list(exfat);
	exfat->root = NULL;
	return false;
}

static bool exfat_root_dir_check(struct exfat *exfat)
{
	struct exfat_node *root;
	int ret;
	clus_t clus_count;

	root = alloc_exfat_node(ATTR_SUBDIR);
	if (!root) {
		exfat_err("failed to allocate memory\n");
		return false;
	}

	root->first_clus = le32_to_cpu(exfat->bs->bsx.root_cluster);
	if (!node_get_clus_count(exfat, root, &clus_count)) {
		exfat_err("failed to follow the cluster chain of root. %d\n",
			ret);
		goto err;
	}
	root->size = clus_count * EXFAT_CLUSTER_SIZE(exfat->bs);

	exfat->root = root;
	exfat_debug("root directory: start cluster[0x%x] size[0x%llx]\n",
		root->first_clus, root->size);
	return true;
err:
	free_exfat_node(root);
	exfat->root = NULL;
	return false;
}

void exfat_show_info(struct exfat *exfat)
{
	exfat_info("volume label [%s]\n",
			exfat->volume_label);
	exfat_info("Bytes per sector: %d\n",
			1 << le32_to_cpu(exfat->bs->bsx.sect_size_bits));
	exfat_info("Sectors per cluster: %d\n",
			1 << le32_to_cpu(exfat->bs->bsx.sect_per_clus_bits));
	exfat_info("Cluster heap count: %d(0x%x)\n",
			le32_to_cpu(exfat->bs->bsx.clu_count),
			le32_to_cpu(exfat->bs->bsx.clu_count));
	exfat_info("Cluster heap offset: %#x\n",
			le32_to_cpu(exfat->bs->bsx.clu_offset));
}

void exfat_show_stat(struct exfat *exfat)
{
	exfat_debug("Found directories: %ld\n", exfat_stat.dir_count);
	exfat_debug("Found files: %ld\n", exfat_stat.file_count);
	exfat_debug("Found leak directories: %ld\n",
			exfat_stat.dir_count - exfat_stat.dir_free_count);
	exfat_debug("Found leak files: %ld\n",
			exfat_stat.file_count - exfat_stat.file_free_count);
}

int main(int argc, char * const argv[])
{
	int c, ret;
	struct fsck_user_input ui = {0,};
	struct exfat_blk_dev bd = {0,};
	struct exfat *exfat = NULL;

	opterr = 0;
	while ((c = getopt_long(argc, argv, "Vvh", opts, NULL)) != EOF) {
		switch (c) {
		case 'r':
			ui.options |= FSCK_OPTS_REPAIR;
			ui.ei.writeable = true;
			break;
		case 'V':
			show_version();
			break;
		case 'v':
			if (print_level < EXFAT_DEBUG)
				print_level++;
			break;
		case '?':
		case 'h':
		default:
			usage(argv[0]);
		}
	}

	if (optind != argc - 1)
		usage(argv[0]);

	printf("fsck.ext4 %s\n", EXFAT_TOOLS_VERSION);

	strncpy(ui.ei.dev_name, argv[optind], sizeof(ui.ei.dev_name));
	ret = exfat_get_blk_dev_info(&ui.ei, &bd);
	if (ret < 0) {
		exfat_err("failed to open %s. %d\n", ui.ei.dev_name, ret);
		return ret;
	}

	exfat = alloc_exfat(&bd);
	if (!exfat) {
		ret = -ENOMEM;
		goto err;
	}

	exfat_debug("verifying boot regions...\n");
	if (!exfat_boot_region_check(exfat)) {
		exfat_err("failed to verify boot regions.\n");
		goto err;
	}

	exfat_show_info(exfat);

	exfat_debug("verifying root directory...\n");
	if (!exfat_root_dir_check(exfat)) {
		exfat_err("failed to verify root directory.\n");
		goto out;
	}

	exfat_debug("verifying directory entries...\n");
	ret = exfat_filesystem_check(exfat);
	if (ret) {
		exfat_err("failed to verify directory entries. %d\n", ret);
		goto out;
	}

	printf("%s: clean\n", ui.ei.dev_name);
out:
	exfat_show_stat(exfat);
err:
	free_exfat(exfat);
	close(bd.dev_fd);
	return ret;
}

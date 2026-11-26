#!/usr/bin/env python3
"""Apply small-RAM MM/network and early-boot memory patches to Linux source tree.

Reduces early-boot reserved RAM and memory-scaling floors under CONFIG_BASE_SMALL:
  1. Cap per-CPU pageset batch size at SZ_32K instead of SZ_256K.
  2. Drain per-CPU pagesets immediately when vm.percpu_pagelist_high_fraction
     changes, and allow vm.watermark_scale_factor=0 so reserves stay constant.
  3. Cap the IPv4 ip_idents hash table at 4096 entries (32 KiB) instead of 256K.
  4. Page-align x86_64 .text/.rodata/.data boundaries and ELF PT_LOAD offsets to
     eliminate ~1.86 MiB of linker section gaps and 2 MiB file-offset padding.
  5. Use 4 KiB basepages + one shared reserved tail page for sub-128 MiB
     SPARSEMEM_VMEMMAP instead of allocating a full 2 MiB hugepage.
  6. Raise MIN_THREADS floor under CONFIG_BASE_SMALL so <16 MiB boots do not hit
     set_max_threads() == 20 (-EAGAIN) on early kthreads.
  7. Unpack initramfs synchronously before device_initcall and progressively free
     consumed external initrd pages during gzip decompression.
"""
from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} LINUX_SRC_DIR")
    kdir = Path(sys.argv[1])
    if not (kdir / "mm/page_alloc.c").is_file():
        raise SystemExit(f"patch-kernel-ram: invalid kernel source dir: {kdir}")

    pending: dict[Path, str] = {}

    def patch_file(path: Path, replacements: list[tuple[str, str, str]]) -> None:
        text = path.read_text(encoding="utf-8")
        for label, old, new in replacements:
            count = text.count(old)
            if count != 1:
                raise SystemExit(
                    f"patch-kernel-ram: {path}: hunk '{label}' expected 1 match, found {count}"
                )
            text = text.replace(old, new, 1)
        pending[path] = text

    patch_file(
        kdir / "mm/page_alloc.c",
        [
            (
                "zone_batchsize",
                "\tbatch = min(zone_managed_pages(zone) >> 12, SZ_256K / PAGE_SIZE);",
                "\tbatch = min(zone_managed_pages(zone) >> 12,\n"
                "\t\t    IS_ENABLED(CONFIG_BASE_SMALL) ? (SZ_32K / PAGE_SIZE) : (SZ_256K / PAGE_SIZE));",
            ),
            (
                "pcp_drain_on_high_fraction",
                "\tfor_each_populated_zone(zone)\n"
                "\t\tzone_set_pageset_high_and_batch(zone, 0);\n"
                "out:\n"
                "\tmutex_unlock(&pcp_batch_high_lock);\n"
                "\treturn ret;",
                "\tfor_each_populated_zone(zone)\n"
                "\t\tzone_set_pageset_high_and_batch(zone, 0);\n"
                "out:\n"
                "\tmutex_unlock(&pcp_batch_high_lock);\n"
                "\tif (!ret && percpu_pagelist_high_fraction != old_percpu_pagelist_high_fraction)\n"
                "\t\tdrain_all_pages(NULL);\n"
                "\treturn ret;",
            ),
            (
                "watermark_scale_factor_min_zero",
                "\t\t.procname\t= \"watermark_scale_factor\",\n"
                "\t\t.data\t\t= &watermark_scale_factor,\n"
                "\t\t.maxlen\t\t= sizeof(watermark_scale_factor),\n"
                "\t\t.mode\t\t= 0644,\n"
                "\t\t.proc_handler\t= watermark_scale_factor_sysctl_handler,\n"
                "\t\t.extra1\t\t= SYSCTL_ONE,",
                "\t\t.procname\t= \"watermark_scale_factor\",\n"
                "\t\t.data\t\t= &watermark_scale_factor,\n"
                "\t\t.maxlen\t\t= sizeof(watermark_scale_factor),\n"
                "\t\t.mode\t\t= 0644,\n"
                "\t\t.proc_handler\t= watermark_scale_factor_sysctl_handler,\n"
                "\t\t.extra1\t\t= SYSCTL_ZERO,",
            ),
            (
                "watermark_scale_factor_default_small",
                "static int watermark_scale_factor = 10;",
                "static int watermark_scale_factor = IS_ENABLED(CONFIG_BASE_SMALL) ? 0 : 10;",
            ),
            (
                "zone_highsize_zero_small",
                "\tunsigned long total_pages;\n\n\tif (!high_fraction) {",
                "\tunsigned long total_pages;\n\n"
                "\tif (IS_ENABLED(CONFIG_BASE_SMALL))\n"
                "\t\treturn 0;\n\n"
                "\tif (!high_fraction) {",
            ),
            (
                "boot_pageset_zero_small",
                "\tpcp->high_min = BOOT_PAGESET_HIGH;\n"
                "\tpcp->high_max = BOOT_PAGESET_HIGH;\n"
                "\tpcp->batch = BOOT_PAGESET_BATCH;",
                "\tpcp->high_min = IS_ENABLED(CONFIG_BASE_SMALL) ? 0 : BOOT_PAGESET_HIGH;\n"
                "\tpcp->high_max = IS_ENABLED(CONFIG_BASE_SMALL) ? 0 : BOOT_PAGESET_HIGH;\n"
                "\tpcp->batch = IS_ENABLED(CONFIG_BASE_SMALL) ? 1 : BOOT_PAGESET_BATCH;",
            ),
            (
                "min_free_kbytes_floor_small",
                "\tnew_min_free_kbytes = int_sqrt(lowmem_kbytes * 16);",
                "\tnew_min_free_kbytes = IS_ENABLED(CONFIG_BASE_SMALL) ? 128 : int_sqrt(lowmem_kbytes * 16);",
            ),
            (
                "rcupdate_include",
                "#include <linux/stddef.h>\n"
                "#include <linux/mm.h>",
                "#include <linux/stddef.h>\n"
                "#include <linux/mm.h>\n"
                "#include <linux/rcupdate.h>",
            ),
            (
                "watermark_boost_factor_default_small",
                "static int watermark_boost_factor __read_mostly = 15000;",
                "static int watermark_boost_factor __read_mostly = IS_ENABLED(CONFIG_BASE_SMALL) ? 0 : 15000;",
            ),
            (
                "oom_rcu_barrier_small",
                "\tpage = get_page_from_freelist((gfp_mask | __GFP_HARDWALL) &\n"
                "\t\t\t\t      ~__GFP_DIRECT_RECLAIM, order,\n"
                "\t\t\t\t      ac->alloc_flags|ALLOC_WMARK_HIGH|ALLOC_CPUSET, ac);\n"
                "\tif (page)\n"
                "\t\tgoto out;",
                "\tpage = get_page_from_freelist((gfp_mask | __GFP_HARDWALL) &\n"
                "\t\t\t\t      ~__GFP_DIRECT_RECLAIM, order,\n"
                "\t\t\t\t      ac->alloc_flags|ALLOC_WMARK_HIGH|ALLOC_CPUSET, ac);\n"
                "\tif (page)\n"
                "\t\tgoto out;\n\n"
                "\tif (IS_ENABLED(CONFIG_BASE_SMALL)) {\n"
                "\t\trcu_barrier();\n"
                "\t\tdrain_all_pages(NULL);\n"
                "\t\tpage = get_page_from_freelist((gfp_mask | __GFP_HARDWALL) &\n"
                "\t\t\t\t\t      ~__GFP_DIRECT_RECLAIM, order,\n"
                "\t\t\t\t\t      ac->alloc_flags|ALLOC_WMARK_MIN|ALLOC_CPUSET, ac);\n"
                "\t\tif (page) {\n"
                "\t\t\t*did_some_progress = 1;\n"
                "\t\t\tgoto out;\n"
                "\t\t}\n"
                "\t}",
            ),
            (
                "lowmem_reserve_ratio_small",
                "static int sysctl_lowmem_reserve_ratio[MAX_NR_ZONES] = {\n"
                "#ifdef CONFIG_ZONE_DMA\n"
                "\t[ZONE_DMA] = 256,\n"
                "#endif\n"
                "#ifdef CONFIG_ZONE_DMA32\n"
                "\t[ZONE_DMA32] = 256,\n"
                "#endif\n"
                "\t[ZONE_NORMAL] = 32,\n"
                "#ifdef CONFIG_HIGHMEM\n"
                "\t[ZONE_HIGHMEM] = 0,\n"
                "#endif\n"
                "\t[ZONE_MOVABLE] = 0,\n"
                "};",
                "static int sysctl_lowmem_reserve_ratio[MAX_NR_ZONES] = {\n"
                "#ifdef CONFIG_ZONE_DMA\n"
                "\t[ZONE_DMA] = IS_ENABLED(CONFIG_BASE_SMALL) ? 0 : 256,\n"
                "#endif\n"
                "#ifdef CONFIG_ZONE_DMA32\n"
                "\t[ZONE_DMA32] = IS_ENABLED(CONFIG_BASE_SMALL) ? 0 : 256,\n"
                "#endif\n"
                "\t[ZONE_NORMAL] = IS_ENABLED(CONFIG_BASE_SMALL) ? 0 : 32,\n"
                "#ifdef CONFIG_HIGHMEM\n"
                "\t[ZONE_HIGHMEM] = 0,\n"
                "#endif\n"
                "\t[ZONE_MOVABLE] = 0,\n"
                "};",
            ),
        ],
    )

    patch_file(
        kdir / "drivers/nvme/host/pci.c",
        [
            (
                "nvme_max_hmb_small",
                "static unsigned int max_host_mem_size_mb = 128;",
                "static unsigned int max_host_mem_size_mb = IS_ENABLED(CONFIG_BASE_SMALL) ? 0 : 128;",
            ),
            (
                "nvme_setup_host_mem_disabled",
                "static int nvme_setup_host_mem(struct nvme_dev *dev)\n"
                "{\n"
                "\tu64 max = (u64)max_host_mem_size_mb * SZ_1M;\n"
                "\tu64 preferred = (u64)dev->ctrl.hmpre * 4096;\n"
                "\tu64 min = (u64)dev->ctrl.hmmin * 4096;\n"
                "\tu32 enable_bits = NVME_HOST_MEM_ENABLE;\n"
                "\tint ret;\n\n"
                "\tif (!dev->ctrl.hmpre)\n"
                "\t\treturn 0;",
                "static int nvme_setup_host_mem(struct nvme_dev *dev)\n"
                "{\n"
                "\tu64 max = (u64)max_host_mem_size_mb * SZ_1M;\n"
                "\tu64 preferred = (u64)dev->ctrl.hmpre * 4096;\n"
                "\tu64 min = (u64)dev->ctrl.hmmin * 4096;\n"
                "\tu32 enable_bits = NVME_HOST_MEM_ENABLE;\n"
                "\tint ret;\n\n"
                "\tif (!dev->ctrl.hmpre || !max_host_mem_size_mb)\n"
                "\t\treturn 0;",
            ),
            (
                "nvme_io_queue_depth_small",
                "static unsigned int io_queue_depth = 1024;",
                "static unsigned int io_queue_depth = IS_ENABLED(CONFIG_BASE_SMALL) ? 64 : 1024;",
            ),
        ],
    )

    patch_file(
        kdir / "net/ipv4/route.c",
        [
            (
                "ip_idents_cap",
                "\tidents_hash = alloc_large_system_hash(\"IP idents\",\n"
                "\t\t\t\t\t      sizeof(*ip_idents) + sizeof(*ip_tstamps),\n"
                "\t\t\t\t\t      0,\n"
                "\t\t\t\t\t      16, /* one bucket per 64 KB */\n"
                "\t\t\t\t\t      HASH_ZERO,\n"
                "\t\t\t\t\t      NULL,\n"
                "\t\t\t\t\t      &ip_idents_mask,\n"
                "\t\t\t\t\t      2048,\n"
                "\t\t\t\t\t      256*1024);",
                "\tidents_hash = alloc_large_system_hash(\"IP idents\",\n"
                "\t\t\t\t\t      sizeof(*ip_idents) + sizeof(*ip_tstamps),\n"
                "\t\t\t\t\t      0,\n"
                "\t\t\t\t\t      16, /* one bucket per 64 KB */\n"
                "\t\t\t\t\t      HASH_ZERO,\n"
                "\t\t\t\t\t      NULL,\n"
                "\t\t\t\t\t      &ip_idents_mask,\n"
                "\t\t\t\t\t      2048,\n"
                "\t\t\t\t\t      IS_ENABLED(CONFIG_BASE_SMALL) ? 4096 : 256*1024);",
            ),
        ],
    )

    patch_file(
        kdir / "arch/x86/kernel/vmlinux.lds.S",
        [
            (
                "x86_rodata_page_align_small",
                "#define X86_ALIGN_RODATA_BEGIN\t. = ALIGN(HPAGE_SIZE);\n\n"
                "#define X86_ALIGN_RODATA_END\t\t\t\t\t\\\n"
                "\t\t. = ALIGN(HPAGE_SIZE);\t\t\t\t\\\n"
                "\t\t__end_rodata_hpage_align = .;\t\t\t\\\n"
                "\t\t__end_rodata_aligned = .;",
                "#ifdef CONFIG_BASE_SMALL\n"
                "#define X86_ALIGN_RODATA_BEGIN\n"
                "#define X86_ALIGN_RODATA_END\t\t\t\t\t\\\n"
                "\t\t. = ALIGN(PAGE_SIZE);\t\t\t\t\\\n"
                "\t\t__end_rodata_hpage_align = .;\t\t\t\\\n"
                "\t\t__end_rodata_aligned = .;\n"
                "#else\n"
                "#define X86_ALIGN_RODATA_BEGIN\t. = ALIGN(HPAGE_SIZE);\n\n"
                "#define X86_ALIGN_RODATA_END\t\t\t\t\t\\\n"
                "\t\t. = ALIGN(HPAGE_SIZE);\t\t\t\t\\\n"
                "\t\t__end_rodata_hpage_align = .;\t\t\t\\\n"
                "\t\t__end_rodata_aligned = .;\n"
                "#endif",
            ),
        ],
    )

    patch_file(
        kdir / "arch/x86/Makefile",
        [
            (
                "x86_ld_page_size_small",
                "ifdef CONFIG_X86_64\n"
                "LDFLAGS_vmlinux += -z max-page-size=0x200000\n"
                "endif",
                "ifdef CONFIG_X86_64\n"
                "ifdef CONFIG_BASE_SMALL\n"
                "LDFLAGS_vmlinux += -z max-page-size=0x1000\n"
                "else\n"
                "LDFLAGS_vmlinux += -z max-page-size=0x200000\n"
                "endif\n"
                "endif",
            ),
        ],
    )

    patch_file(
        kdir / "arch/x86/boot/compressed/misc.c",
        [
            (
                "parse_elf_page_align_small",
                "#ifdef CONFIG_X86_64\n"
                "\t\t\tif ((phdr->p_align % 0x200000) != 0)\n"
                "\t\t\t\terror(\"Alignment of LOAD segment isn't multiple of 2MB\");\n"
                "#endif",
                "#ifdef CONFIG_X86_64\n"
                "\t\t\tif ((phdr->p_align % (IS_ENABLED(CONFIG_BASE_SMALL) ? PAGE_SIZE : 0x200000)) != 0)\n"
                "\t\t\t\terror(\"Alignment of LOAD segment isn't properly aligned\");\n"
                "#endif",
            ),
        ],
    )

    patch_file(
        kdir / "arch/x86/mm/init_64.c",
        [
            (
                "vmemmap_basepages_small",
                "\tif (end - start < PAGES_PER_SECTION * sizeof(struct page))\n"
                "\t\terr = vmemmap_populate_basepages(start, end, node, NULL);",
                "\tif (end - start < PAGES_PER_SECTION * sizeof(struct page) ||\n"
                "\t    (IS_ENABLED(CONFIG_BASE_SMALL) && !altmap && max_pfn < PAGES_PER_SECTION))\n"
                "\t\terr = vmemmap_populate_basepages(start, end, node, NULL);",
            ),
        ],
    )

    patch_file(
        kdir / "mm/sparse-vmemmap.c",
        [
            (
                "vmemmap_populate_basepages_tail",
                "int __meminit vmemmap_populate_basepages(unsigned long start, unsigned long end,\n"
                "\t\t\t\t\t int node, struct vmem_altmap *altmap)\n"
                "{\n"
                "\treturn vmemmap_populate_range(start, end, node, altmap, -1, 0);\n"
                "}",
                "int __meminit vmemmap_populate_basepages(unsigned long start, unsigned long end,\n"
                "\t\t\t\t\t int node, struct vmem_altmap *altmap)\n"
                "{\n"
                "\tif (IS_ENABLED(CONFIG_BASE_SMALL) && !altmap && max_pfn < PAGES_PER_SECTION) {\n"
                "\t\tunsigned long nr = ALIGN(max_pfn, MAX_ORDER_NR_PAGES);\n"
                "\t\tunsigned long actual_end = min(end, start + PAGE_ALIGN(nr * sizeof(struct page)));\n"
                "\t\tint err = vmemmap_populate_range(start, actual_end, node, NULL, -1, 0);\n"
                "\n"
                "\t\tif (!err && actual_end < end) {\n"
                "\t\t\tvoid *tail = vmemmap_alloc_block_zero(PAGE_SIZE, node);\n"
                "\n"
                "\t\t\tif (!tail)\n"
                "\t\t\t\treturn -ENOMEM;\n"
                "\t\t\terr = vmemmap_populate_range(actual_end, end, node, NULL,\n"
                "\t\t\t\t\t\t     PHYS_PFN(__pa(tail)), 0);\n"
                "\t\t}\n"
                "\t\treturn err;\n"
                "\t}\n"
                "\treturn vmemmap_populate_range(start, end, node, altmap, -1, 0);\n"
                "}",
            ),
        ],
    )

    patch_file(
        kdir / "mm/mm_init.c",
        [
            (
                "memmap_init_cap_small",
                "#ifdef CONFIG_SPARSEMEM\n"
                "\tend_pfn = round_up(end_pfn, PAGES_PER_SECTION);\n"
                "#else",
                "#ifdef CONFIG_SPARSEMEM\n"
                "\tif (IS_ENABLED(CONFIG_BASE_SMALL) && max_pfn < PAGES_PER_SECTION)\n"
                "\t\tend_pfn = min_t(unsigned long, round_up(end_pfn, PAGES_PER_SECTION),\n"
                "\t\t\t\tround_up(max_pfn, MAX_ORDER_NR_PAGES) + (PAGE_SIZE / sizeof(struct page)));\n"
                "\telse\n"
                "\t\tend_pfn = round_up(end_pfn, PAGES_PER_SECTION);\n"
                "#else",
            ),
        ],
    )

    patch_file(
        kdir / "kernel/fork.c",
        [
            (
                "min_threads_small",
                "#define MIN_THREADS 20",
                "#ifdef CONFIG_BASE_SMALL\n"
                "#define MIN_THREADS 128\n"
                "#else\n"
                "#define MIN_THREADS 20\n"
                "#endif",
            ),
        ],
    )

    patch_file(
        kdir / "init/initramfs.c",
        [
            (
                "initramfs_streaming_fill",
                "static unsigned long my_inptr __initdata; /* index of next byte to be processed in inbuf */\n\n"
                "#include <linux/decompress/generic.h>",
                "#include <linux/initrd.h>\n"
                "static int __initdata do_retain_initrd;\n"
                "static unsigned long my_inptr __initdata; /* index of next byte to be processed in inbuf */\n"
                "static unsigned long __initdata rd_stream_cur;\n"
                "static unsigned long __initdata rd_stream_end;\n"
                "static unsigned long __initdata rd_stream_last;\n\n"
                "static long __init rd_fill(void *dest, unsigned long size)\n"
                "{\n"
                "\tunsigned long avail, chunk;\n\n"
                "\tif (rd_stream_last) {\n"
                "\t\tunsigned long free_start = PAGE_ALIGN(initrd_start);\n"
                "\t\tunsigned long free_end = rd_stream_last & PAGE_MASK;\n\n"
                "\t\tif (free_end > free_start) {\n"
                "\t\t\tfree_initrd_mem(free_start, free_end);\n"
                "\t\t\tinitrd_start = free_end;\n"
                "\t\t}\n"
                "\t}\n"
                "\tavail = rd_stream_end - rd_stream_cur;\n"
                "\tif (!avail)\n"
                "\t\treturn -1;\n"
                "\tchunk = min(avail, size);\n"
                "\trd_stream_last = rd_stream_cur;\n"
                "\tmemcpy(dest, (void *)rd_stream_cur, chunk);\n"
                "\trd_stream_cur += chunk;\n"
                "\treturn chunk;\n"
                "}\n\n"
                "#include <linux/decompress/generic.h>",
            ),
            (
                "unpack_use_rd_fill",
                "\t\tif (decompress) {\n"
                "\t\t\tint res = decompress(buf, len, NULL, flush_buffer, NULL,\n"
                "\t\t\t\t   &my_inptr, error);\n"
                "\t\t\tif (res)\n"
                "\t\t\t\terror(\"decompressor failed\");\n"
                "\t\t}",
                "\t\tif (decompress) {\n"
                "\t\t\tint res;\n\n"
                "\t\t\tif (IS_ENABLED(CONFIG_BASE_SMALL) && !do_retain_initrd &&\n"
                "\t\t\t    initrd_start && buf >= (char *)initrd_start &&\n"
                "\t\t\t    buf < (char *)initrd_end) {\n"
                "\t\t\t\trd_stream_cur = (unsigned long)buf;\n"
                "\t\t\t\trd_stream_end = (unsigned long)buf + len;\n"
                "\t\t\t\trd_stream_last = 0;\n"
                "\t\t\t\tres = decompress(NULL, 0, rd_fill, flush_buffer,\n"
                "\t\t\t\t\t\t NULL, &my_inptr, error);\n"
                "\t\t\t\tif (rd_stream_last) {\n"
                "\t\t\t\t\tunsigned long free_start = PAGE_ALIGN(initrd_start);\n"
                "\t\t\t\t\tunsigned long free_end = rd_stream_last & PAGE_MASK;\n\n"
                "\t\t\t\t\tif (free_end > free_start) {\n"
                "\t\t\t\t\t\tfree_initrd_mem(free_start, free_end);\n"
                "\t\t\t\t\t\tinitrd_start = free_end;\n"
                "\t\t\t\t\t}\n"
                "\t\t\t\t\tmy_inptr += rd_stream_last - (unsigned long)buf;\n"
                "\t\t\t\t\trd_stream_last = 0;\n"
                "\t\t\t\t}\n"
                "\t\t\t\tif (my_inptr > len)\n"
                "\t\t\t\t\tmy_inptr = len;\n"
                "\t\t\t} else {\n"
                "\t\t\t\tres = decompress(buf, len, NULL, flush_buffer, NULL,\n"
                "\t\t\t\t\t\t &my_inptr, error);\n"
                "\t\t\t}\n"
                "\t\t\tif (res)\n"
                "\t\t\t\terror(\"decompressor failed\");\n"
                "\t\t}",
            ),
            (
                "dedup_retain_and_initrd_h",
                "static int __initdata do_retain_initrd;\n\n"
                "static int __init retain_initrd_param",
                "static int __init retain_initrd_param",
            ),
            (
                "initramfs_sync_small",
                "static bool __initdata initramfs_async = true;\n"
                "static int __init initramfs_async_setup(char *str)\n"
                "{\n"
                "\treturn kstrtobool(str, &initramfs_async) == 0;\n"
                "}\n"
                "__setup(\"initramfs_async=\", initramfs_async_setup);\n\n"
                "extern char __initramfs_start[];\n"
                "extern unsigned long __initramfs_size;\n"
                "#include <linux/initrd.h>",
                "static bool __initdata initramfs_async = !IS_ENABLED(CONFIG_BASE_SMALL);\n"
                "static int __init initramfs_async_setup(char *str)\n"
                "{\n"
                "\treturn kstrtobool(str, &initramfs_async) == 0;\n"
                "}\n"
                "__setup(\"initramfs_async=\", initramfs_async_setup);\n\n"
                "extern char __initramfs_start[];\n"
                "extern unsigned long __initramfs_size;",
            ),
        ],
    )

    patch_file(
        kdir / "mm/execmem.c",
        [
            (
                "execmem_cache_clean_small",
                "\t\tif (IS_ALIGNED(size, PMD_SIZE) &&\n"
                "\t\t    IS_ALIGNED(mas.index, PMD_SIZE)) {",
                "\t\tif (IS_ENABLED(CONFIG_BASE_SMALL) ||\n"
                "\t\t    (IS_ALIGNED(size, PMD_SIZE) &&\n"
                "\t\t     IS_ALIGNED(mas.index, PMD_SIZE))) {",
            ),
            (
                "execmem_cache_populate_small",
                "\talloc_size = round_up(size, PMD_SIZE);",
                "\talloc_size = IS_ENABLED(CONFIG_BASE_SMALL) ? size : round_up(size, PMD_SIZE);",
            ),
        ],
    )

    patch_file(
        kdir / "arch/x86/include/asm/pgtable_64_types.h",
        [
            (
                "early_dynamic_pgts_small",
                "#define EARLY_DYNAMIC_PAGE_TABLES\t64",
                "#ifdef CONFIG_BASE_SMALL\n"
                "#define EARLY_DYNAMIC_PAGE_TABLES\t16\n"
                "#else\n"
                "#define EARLY_DYNAMIC_PAGE_TABLES\t64\n"
                "#endif",
            ),
        ],
    )

    patch_file(
        kdir / "kernel/rcu/tree.c",
        [
            (
                "rcu_fast_reclaim_small",
                "#define DEFAULT_RCU_BLIMIT (IS_ENABLED(CONFIG_RCU_STRICT_GRACE_PERIOD) ? 1000 : 10)\n"
                "\t\t\t\t// Maximum callbacks per rcu_do_batch ...\n"
                "#define DEFAULT_MAX_RCU_BLIMIT 10000 // ... even during callback flood.\n"
                "static long blimit = DEFAULT_RCU_BLIMIT;\n"
                "#define DEFAULT_RCU_QHIMARK 10000 // If this many pending, ignore blimit.\n"
                "static long qhimark = DEFAULT_RCU_QHIMARK;\n"
                "#define DEFAULT_RCU_QLOMARK 100   // Once only this many pending, use blimit.",
                "#define DEFAULT_RCU_BLIMIT ((IS_ENABLED(CONFIG_RCU_STRICT_GRACE_PERIOD) || IS_ENABLED(CONFIG_BASE_SMALL)) ? 1000 : 10)\n"
                "\t\t\t\t// Maximum callbacks per rcu_do_batch ...\n"
                "#define DEFAULT_MAX_RCU_BLIMIT 10000 // ... even during callback flood.\n"
                "static long blimit = DEFAULT_RCU_BLIMIT;\n"
                "#define DEFAULT_RCU_QHIMARK (IS_ENABLED(CONFIG_BASE_SMALL) ? 64 : 10000) // If this many pending, ignore blimit.\n"
                "static long qhimark = DEFAULT_RCU_QHIMARK;\n"
                "#define DEFAULT_RCU_QLOMARK (IS_ENABLED(CONFIG_BASE_SMALL) ? 16 : 100)   // Once only this many pending, use blimit.",
            ),
            (
                "rcu_sched_qs_small",
                "\tif (j < HZ / 10 + nr_cpu_ids / RCU_JIFFIES_FQS_DIV)\n"
                "\t\tj = HZ / 10 + nr_cpu_ids / RCU_JIFFIES_FQS_DIV;",
                "\tif (IS_ENABLED(CONFIG_BASE_SMALL)) {\n"
                "\t\tif (j < 1)\n"
                "\t\t\tj = 1;\n"
                "\t} else if (j < HZ / 10 + nr_cpu_ids / RCU_JIFFIES_FQS_DIV)\n"
                "\t\tj = HZ / 10 + nr_cpu_ids / RCU_JIFFIES_FQS_DIV;",
            ),
        ],
    )

    patch_file(
        kdir / "arch/x86/include/asm/page_64_types.h",
        [
            (
                "thread_and_irq_stack_order_small",
                "#define THREAD_SIZE_ORDER\t(2 + KASAN_STACK_ORDER)\n"
                "#define THREAD_SIZE  (PAGE_SIZE << THREAD_SIZE_ORDER)\n\n"
                "#define EXCEPTION_STACK_ORDER (1 + KASAN_STACK_ORDER)\n"
                "#define EXCEPTION_STKSZ (PAGE_SIZE << EXCEPTION_STACK_ORDER)\n\n"
                "#define IRQ_STACK_ORDER (2 + KASAN_STACK_ORDER)\n"
                "#define IRQ_STACK_SIZE (PAGE_SIZE << IRQ_STACK_ORDER)",
                "#ifdef CONFIG_BASE_SMALL\n"
                "#define THREAD_SIZE_ORDER\t(1 + KASAN_STACK_ORDER)\n"
                "#else\n"
                "#define THREAD_SIZE_ORDER\t(2 + KASAN_STACK_ORDER)\n"
                "#endif\n"
                "#define THREAD_SIZE  (PAGE_SIZE << THREAD_SIZE_ORDER)\n\n"
                "#define EXCEPTION_STACK_ORDER (1 + KASAN_STACK_ORDER)\n"
                "#define EXCEPTION_STKSZ (PAGE_SIZE << EXCEPTION_STACK_ORDER)\n\n"
                "#ifdef CONFIG_BASE_SMALL\n"
                "#define IRQ_STACK_ORDER (1 + KASAN_STACK_ORDER)\n"
                "#else\n"
                "#define IRQ_STACK_ORDER (2 + KASAN_STACK_ORDER)\n"
                "#endif\n"
                "#define IRQ_STACK_SIZE (PAGE_SIZE << IRQ_STACK_ORDER)",
            ),
        ],
    )

    patch_file(
        kdir / "arch/x86/realmode/init.c",
        [
            (
                "realmode_keep_low_1m_small",
                "\t/*\n"
                "\t * Unconditionally reserve the entire first 1M, see comment in\n"
                "\t * setup_arch().\n"
                "\t */\n"
                "\tmemblock_reserve(0, SZ_1M);",
                "\t/*\n"
                "\t * Unconditionally reserve the entire first 1M, see comment in\n"
                "\t * setup_arch().\n"
                "\t */\n"
                "\tif (!IS_ENABLED(CONFIG_BASE_SMALL))\n"
                "\t\tmemblock_reserve(0, SZ_1M);",
            ),
        ],
    )

    for path, text in pending.items():
        path.write_text(text, encoding="utf-8")
    print(f"patch-kernel-ram: patched {kdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

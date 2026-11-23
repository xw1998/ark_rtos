/**
* @file arkrtos_shell_fs.c
* @brief 存储：blk / fs / pwd / cd / df 与整套 VFS 命令。
*          这一族最大，占了原文件的三分之一；边界是「所有走块设备或
*          路径解析的命令都在这」，别的族不直接碰 littlefs / VFS。
* @note 本文件属于内核特权态代码；由 arkrtos_shell.c 按命令族拆出，
*       只做搬运，命令逻辑与拆分前逐字相同。
*/

#include "arkrtos_shell_internal.h"

#if (SHELL_ENABLE == 1)

#if ARKRTOS_USE_BLK
/* ============================================================
 * blk: raw access to the non-volatile backends
 *
 * This is the bring-up self-check for a storage backend. It is the only
 * place in the kernel that lets an operator poke raw offsets, and it is kept
 * deliberately small. `blk test` is the interesting one: it never reports
 * "ok" from the write call alone - it re-reads what the chip actually holds
 * after an erase and after a program, so a backend that answers successfully
 * while changing nothing cannot pass.
 * ============================================================ */
#define BLK_TEST_LEN_MAX   (256u)   /* scratch size, see g_blk_scratch   */
#define BLK_WR_BYTES_MAX   (32u)    /* hex bytes accepted by `blk wr`    */
#define BLK_DUMP_LEN_MAX   (4096u)  /* cap so one command cannot flood    */
#define BLK_DUMP_COLS      (16u)

static uint8 g_blk_scratch[BLK_TEST_LEN_MAX];

static int blk_nibble(char c, uint32 *out)
{
    if((c >= '0') && (c <= '9')) { *out = (uint32)(c - '0');      return 0; }
    if((c >= 'a') && (c <= 'f')) { *out = (uint32)(c - 'a' + 10); return 0; }
    if((c >= 'A') && (c <= 'F')) { *out = (uint32)(c - 'A' + 10); return 0; }
    return -1;
}

/* "A5 01 ff" / "A501FF" -> bytes. Returns -1 on odd length or a non-hex char. */
static int blk_hex_to_bytes(const char *s, uint8 *out, uint32 max, uint32 *out_len)
{
    uint32 n = 0u;

    if((s == 0) || (*s == '\0'))
    {
        return -1;
    }

    while(*s != '\0')
    {
        uint32 hi;
        uint32 lo;

        if(n >= max)
        {
            return -1;
        }

        if(blk_nibble(s[0], &hi) != 0)
        {
            return -1;
        }

        if(s[1] == '\0')
        {
            return -1;
        }

        if(blk_nibble(s[1], &lo) != 0)
        {
            return -1;
        }

        out[n] = (uint8)((hi << 4) | lo);
        n++;
        s += 2;
    }

    *out_len = n;
    return 0;
}

static int blk_usage(void)
{
    sh_out("\r\nusage:\r\n");
    sh_out("  blk                          list devices (brings each one up)\r\n");
    sh_out("  blk probe <name>             report capacity and erase unit\r\n");
    sh_out("  blk rd <name> <off> [len]    hex dump, default 64 bytes\r\n");
    sh_out("  blk wr <name> <off> <hex>    write bytes; target must be erased\r\n");
    sh_out("  blk erase <name> <off> <len> off and len must be erase-unit aligned\r\n");
    sh_out("  blk test <name> <off> [len]  erase + program + read back, len <= unit\r\n");
    return -1;
}

static int blk_list(void)
{
    uint32 n = arkrtos_blk_count();
    uint32 i;

    if(n == 0u)
    {
        sh_out("\r\nno block device registered on this board\r\n");
        return -1;
    }

    sh_out("\r\n name  erase unit  capacity   state\r\n");

    for(i = 0u; i < n; i++)
    {
        const arkrtos_blk_dev_t *dev = arkrtos_blk_at(i);

        /* Listing doubles as a presence check: bring-up here is cheap and
         * reports the truth, whereas a stale "probably fine" flag would not. */
        if(arkrtos_blk_init(dev) == 0)
        {
            ark_shell_printf(" %-5s %-11u %-10u ready\r\n",
                             dev->name, (unsigned)dev->erase_unit,
                             (unsigned)arkrtos_blk_size(dev));
        }
        else
        {
            ark_shell_printf(" %-5s %-11u %-10s down\r\n",
                             dev->name, (unsigned)dev->erase_unit, "-");
        }
    }

    return 0;
}

static int blk_dump(const arkrtos_blk_dev_t *dev, uint32 off, uint32 len)
{
    uint32 done = 0u;

    while(done < len)
    {
        uint32 chunk = len - done;
        uint32 j;

        if(chunk > BLK_DUMP_COLS)
        {
            chunk = BLK_DUMP_COLS;
        }

        if(arkrtos_blk_read(dev, off + done, g_blk_scratch, chunk) != 0)
        {
            ark_shell_printf("\r\nread failed at offset 0x%X\r\n",
                             (unsigned)(off + done));
            return -1;
        }

        ark_shell_printf(" %06X:", (unsigned)(off + done));

        for(j = 0u; j < chunk; j++)
        {
            ark_shell_printf(" %02X", (unsigned)g_blk_scratch[j]);
        }

        for(j = chunk; j < BLK_DUMP_COLS; j++)
        {
            sh_out("   ");
        }

        sh_out("  ");

        for(j = 0u; j < chunk; j++)
        {
            uint32 c = (uint32)g_blk_scratch[j];

            /* printable range only; everything else shows as '.' so a dump
             * of erased (0xFF) or blank (0x00) flash stays readable */
            ark_shell_printf("%c", ((c >= 0x20u) && (c < 0x7Fu)) ? (char)c : '.');
        }

        sh_out("\r\n");
        done += chunk;
    }

    return 0;
}

/* Erase the unit containing off, check it reads back as 0xFF, program a
 * deterministic pattern over len bytes, then read it back and compare. */
static int blk_test(const arkrtos_blk_dev_t *dev, uint32 off, uint32 len)
{
    uint32 unit = dev->erase_unit;
    uint32 base = off & ~(unit - 1u);
    uint8  got[BLK_DUMP_COLS];
    uint32 i;

    if(len == 0u)
    {
        sh_out("\r\nlen must be >= 1\r\n");
        return -1;
    }

    if(len > unit)
    {
        ark_shell_printf("\r\nlen must be <= erase unit (%u)\r\n", (unsigned)unit);
        return -1;
    }

    if(len > BLK_TEST_LEN_MAX)
    {
        return -1;
    }

    ark_shell_printf("\r\ntest %s at 0x%X, %u bytes (unit base 0x%X, unit %u)\r\n",
                     dev->name, (unsigned)off, (unsigned)len,
                     (unsigned)base, (unsigned)unit);

    if(arkrtos_blk_erase(dev, base, unit) != 0)
    {
        sh_out(" erase   : FAILED (device refused or timed out)\r\n");
        return -1;
    }

    if(arkrtos_blk_read(dev, base, g_blk_scratch, len) != 0)
    {
        sh_out(" erase   : FAILED (read back failed)\r\n");
        return -1;
    }

    for(i = 0u; i < len; i++)
    {
        if(g_blk_scratch[i] != 0xFFu)
        {
            ark_shell_printf(" erase   : FAILED at +%u, got 0x%02X, want 0xFF\r\n",
                             (unsigned)i, (unsigned)g_blk_scratch[i]);
            return -1;
        }
    }

    sh_out(" erase   : ok, all 0xFF\r\n");

    for(i = 0u; i < len; i++)
    {
        g_blk_scratch[i] = (uint8)((((base + i) * 31u) + 7u) & 0xFFu);
    }

    if(arkrtos_blk_write(dev, base, g_blk_scratch, len) != 0)
    {
        sh_out(" program : FAILED (device refused or timed out)\r\n");
        return -1;
    }

    /* Compare in small chunks: a full-size shadow buffer would double the
     * static cost of this command for no extra evidence. */
    for(i = 0u; i < len; i += BLK_DUMP_COLS)
    {
        uint32 chunk = len - i;
        uint32 j;

        if(chunk > BLK_DUMP_COLS)
        {
            chunk = BLK_DUMP_COLS;
        }

        if(arkrtos_blk_read(dev, base + i, got, chunk) != 0)
        {
            sh_out(" verify  : FAILED (read back failed)\r\n");
            return -1;
        }

        for(j = 0u; j < chunk; j++)
        {
            if(got[j] != g_blk_scratch[i + j])
            {
                ark_shell_printf(" verify  : FAILED at +%u, got 0x%02X, want 0x%02X\r\n",
                                 (unsigned)(i + j), (unsigned)got[j],
                                 (unsigned)g_blk_scratch[i + j]);
                return -1;
            }
        }
    }

    ark_shell_printf(" program : ok, %u bytes verified\r\n", (unsigned)len);

    return 0;
}

int cmd_blk(int argc, char *argv[])
{
    const arkrtos_blk_dev_t *dev;
    uint32 off = 0u;
    uint32 len = 0u;
    const char *sub = (argc >= 2) ? argv[1] : "list";

    if(strcmp(sub, "list") == 0)
    {
        return blk_list();
    }

    if(argc < 3)
    {
        return blk_usage();
    }

    dev = arkrtos_blk_find(argv[2]);

    if(dev == 0)
    {
        ark_shell_printf("\r\nunknown block device: %s\r\n", argv[2]);
        (void)blk_list();
        return -1;
    }

    if(arkrtos_blk_init(dev) != 0)
    {
        ark_shell_printf("\r\n%s: not available\r\n", argv[2]);
        return -1;
    }

    if(strcmp(sub, "probe") == 0)
    {
        ark_shell_printf("\r\n%s: %u bytes, erase unit %u\r\n",
                         dev->name, (unsigned)arkrtos_blk_size(dev),
                         (unsigned)dev->erase_unit);
        return 0;
    }

    if(strcmp(sub, "rd") == 0)
    {
        if((argc < 4) || (parse_u32(argv[3], &off) != 0))
        {
            return blk_usage();
        }

        len = 64u;

        if((argc >= 5) && (parse_u32(argv[4], &len) != 0))
        {
            return blk_usage();
        }

        if((len == 0u) || (len > BLK_DUMP_LEN_MAX))
        {
            ark_shell_printf("\r\nlen must be 1..%u\r\n", (unsigned)BLK_DUMP_LEN_MAX);
            return -1;
        }

        return blk_dump(dev, off, len);
    }

    if(strcmp(sub, "wr") == 0)
    {
        uint32 n = 0u;

        if((argc < 5) || (parse_u32(argv[3], &off) != 0))
        {
            return blk_usage();
        }

        if(blk_hex_to_bytes(argv[4], g_blk_scratch, BLK_WR_BYTES_MAX, &n) != 0)
        {
            ark_shell_printf("\r\nbad hex string (even number of digits, max %u bytes)\r\n",
                             (unsigned)BLK_WR_BYTES_MAX);
            return -1;
        }

        if(arkrtos_blk_write(dev, off, g_blk_scratch, n) != 0)
        {
            sh_out("\r\nwrite refused or failed (is the range erased?)\r\n");
            return -1;
        }

        ark_shell_printf("\r\nwrote %u bytes at 0x%X\r\n", (unsigned)n, (unsigned)off);

        return blk_dump(dev, off, n);
    }

    if(strcmp(sub, "erase") == 0)
    {
        if((argc < 5) || (parse_u32(argv[3], &off) != 0) ||
           (parse_u32(argv[4], &len) != 0))
        {
            return blk_usage();
        }

        if(arkrtos_blk_erase(dev, off, len) != 0)
        {
            ark_shell_printf("\r\nerase refused or failed (off and len must be multiples of %u)\r\n",
                             (unsigned)dev->erase_unit);
            return -1;
        }

        ark_shell_printf("\r\nerased %u bytes at 0x%X\r\n",
                         (unsigned)len, (unsigned)off);
        return 0;
    }

    if(strcmp(sub, "test") == 0)
    {
        if((argc < 4) || (parse_u32(argv[3], &off) != 0))
        {
            return blk_usage();
        }

        len = 128u;

        if((argc >= 5) && (parse_u32(argv[4], &len) != 0))
        {
            return blk_usage();
        }

        return blk_test(dev, off, len);
    }

    return blk_usage();
}

#endif /* ARKRTOS_USE_BLK */
#if ARKRTOS_USE_FS
/* ============================================================
 * fs: the littlefs volume, on top of a block device from `blk list`
 *
 * The file system goes through the same block layer the raw `blk` commands
 * use, so "where the storage is" is written down exactly once. The volume is
 * named, never an address: `fs mount nor0 0 4194304` means offset 0 of that
 * device, and nothing here knows what CPU address that ends up at.
 *
 * `fs test` is the one that matters. It writes a file, reads it back, drops
 * the mount, mounts again, reads it back a second time and only then removes
 * it. A write that answers "ok" without reaching the chip cannot pass, and
 * neither can a cache that never flushes - the remount throws both away.
 * ============================================================ */
#define FS_SCRATCH_LEN   (512u)   /* one file's worth for wr / rd / test */
#define FS_RD_DEFAULT    (64u)    /* bytes `fs rd` reads when not told    */
#define FS_DUMP_COLS     (16u)
#define FS_TEST_PATH     "/fstest.bin"

/* fs put: binary upload window. One chunk per handshake, because the console
 * FIFO is 128 bytes deep and a littlefs block program may have to erase a
 * sector first - a sender that ran ahead would lose bytes silently. */
#define FS_PUT_CHUNK      (256u)
#define FS_PUT_TIMEOUT_MS (5000u)
#define FS_PUT_ACK        (0x06u)
#define FS_PUT_NAK        (0x15u)

static uint8 g_fs_scratch[FS_SCRATCH_LEN];

static int fs_usage(void)
{
    sh_out("\r\nusage:\r\n");
    sh_out("  fs mount [dev off size]   mount a volume (default: the ARKRTOS_FS_* layout)\r\n");
    sh_out("  fs unmount                drop the mount\r\n");
    sh_out("  fs format [dev off size]  erase and create an empty volume (destructive)\r\n");
    sh_out("  fs info                   volume size and used bytes\r\n");
    sh_out("  fs ls [dir]               list a directory, default /\r\n");
    sh_out("  fs wr <path> <text...>    write text to a file (creates or truncates)\r\n");
    sh_out("  fs rd <path> [len]        read a file back and hex dump it\r\n");
    sh_out("  fs put <path> <len>       receive <len> raw bytes and write them to <path>\r\n");
    sh_out("  fs rm <path>              remove a file\r\n");
    sh_out("  fs test                   write / read / remount / read / remove\r\n");
    sh_out("  fs err                    littlefs error of the last failed call\r\n");
    return -1;
}

/* Every fs call that fails keeps what littlefs actually returned; print that
 * number instead of guessing a cause from the outside. */
static int fs_fail(const char *what)
{
    int32 e = arkrtos_fs_last_error();

    ark_shell_printf("\r\n%s: FAILED (littlefs error %d: %s)\r\n",
                     what, (int)e, arkrtos_fs_error_name(e));
    return -1;
}

static void fs_dump(const uint8 *p, uint32 len)
{
    uint32 off = 0u;

    while(off < len)
    {
        uint32 chunk = len - off;
        uint32 j;

        if(chunk > FS_DUMP_COLS)
        {
            chunk = FS_DUMP_COLS;
        }

        ark_shell_printf(" %04X:", (unsigned)off);

        for(j = 0u; j < chunk; j++)
        {
            ark_shell_printf(" %02X", (unsigned)p[off + j]);
        }

        for(j = chunk; j < FS_DUMP_COLS; j++)
        {
            sh_out("   ");
        }

        sh_out("  ");

        for(j = 0u; j < chunk; j++)
        {
            uint32 c = (uint32)p[off + j];

            ark_shell_printf("%c", ((c >= 0x20u) && (c < 0x7Fu)) ? (char)c : '.');
        }

        sh_out("\r\n");
        off += chunk;
    }
}

/* argv[from..argc-1] joined with single spaces -> g_fs_scratch */
static int fs_collect_text(int argc, char *argv[], int from, uint32 *out_len)
{
    uint32 n = 0u;
    int i;

    for(i = from; i < argc; i++)
    {
        const char *p = argv[i];

        if(i > from)
        {
            if(n >= FS_SCRATCH_LEN)
            {
                return -1;
            }

            g_fs_scratch[n++] = (uint8)' ';
        }

        while(*p != '\0')
        {
            if(n >= FS_SCRATCH_LEN)
            {
                return -1;
            }

            g_fs_scratch[n++] = (uint8)(*p);
            p++;
        }
    }

    *out_len = n;
    return 0;
}

/* `fs <sub>` / `fs <sub> dev off size` -> volume description. Anything else
 * is a usage error: half a volume description is never filled in with a
 * default, because that would mount at an offset the operator did not name. */
static int fs_take_volume(int argc, char *argv[], const char **dev,
                          uint32 *off, uint32 *size)
{
    *dev  = ARKRTOS_FS_DEV_NAME;
    *off  = ARKRTOS_FS_BASE;
    *size = ARKRTOS_FS_SIZE;

    if(argc == 2)
    {
        return 0;
    }

    if(argc == 5)
    {
        *dev = argv[2];

        if((parse_u32(argv[3], off) != 0) || (parse_u32(argv[4], size) != 0))
        {
            return -1;
        }

        return 0;
    }

    return -1;
}

static int fs_list_cb(const char *name, uint32 size, uint8 is_dir, void *arg)
{
    uint32 *count = (uint32 *)arg;

    ark_shell_printf("  %-20s %8u%s\r\n", name, (unsigned)size,
                     (is_dir != 0u) ? "  <dir>" : "");
    (*count)++;
    return 0;
}

static int fs_do_ls(const char *dir)
{
    uint32 count = 0u;

    if(arkrtos_fs_list(dir, fs_list_cb, &count) != 0)
    {
        return fs_fail("ls");
    }

    ark_shell_printf("  %u entr%s\r\n", (unsigned)count,
                     (count == 1u) ? "y" : "ies");
    return 0;
}

static int fs_do_info(void)
{
    uint32 total = 0u;
    uint32 used  = 0u;

    if(!arkrtos_fs_mounted())
    {
        sh_out("\r\nnot mounted\r\n");
        return -1;
    }

    if(arkrtos_fs_stat(&total, &used) != 0)
    {
        return fs_fail("stat");
    }

    ark_shell_printf("\r\nmounted  : yes (%s, offset 0x%X, size %u)\r\n",
                     ARKRTOS_FS_DEV_NAME, (unsigned)ARKRTOS_FS_BASE,
                     (unsigned)ARKRTOS_FS_SIZE);
    ark_shell_printf("total    : %u bytes (%u KiB)\r\n",
                     (unsigned)total, (unsigned)(total / 1024u));
    ark_shell_printf("used     : %u bytes (%u KiB, %u%%)\r\n",
                     (unsigned)used, (unsigned)(used / 1024u),
                     (unsigned)((total != 0u) ? ((used * 100u) / total) : 0u));
    ark_shell_printf("free     : %u bytes\r\n",
                     (unsigned)((total >= used) ? (total - used) : 0u));
    return 0;
}

/* Read FS_SCRATCH_LEN bytes from path and compare with the written pattern.
 * The comparison target is recomputed, not taken from the buffer that the
 * read just overwrote. */
static int fs_read_verify(const char *path)
{
    uint32 got = 0u;
    uint32 i;

    if(arkrtos_fs_read_file(path, g_fs_scratch, FS_SCRATCH_LEN, &got) != 0)
    {
        return fs_fail("read");
    }

    if(got != FS_SCRATCH_LEN)
    {
        ark_shell_printf("\r\nread: short file, got %u of %u bytes\r\n",
                         (unsigned)got, (unsigned)FS_SCRATCH_LEN);
        return -1;
    }

    for(i = 0u; i < got; i++)
    {
        uint8 want = (uint8)((i * 17u + 3u) & 0xFFu);

        if(g_fs_scratch[i] != want)
        {
            ark_shell_printf("\r\nread: MISMATCH at %u: got 0x%02X want 0x%02X\r\n",
                             (unsigned)i, (unsigned)g_fs_scratch[i], (unsigned)want);
            return -1;
        }
    }

    ark_shell_printf("read   : ok, %u bytes verified\r\n", (unsigned)got);
    return 0;
}

static int fs_do_test(void)
{
    uint32 i;

    if(!arkrtos_fs_mounted())
    {
        sh_out("\r\nnot mounted: fs mount first\r\n");
        return -1;
    }

    for(i = 0u; i < FS_SCRATCH_LEN; i++)
    {
        g_fs_scratch[i] = (uint8)((i * 17u + 3u) & 0xFFu);
    }

    if(arkrtos_fs_write_file(FS_TEST_PATH, g_fs_scratch, FS_SCRATCH_LEN) != 0)
    {
        return fs_fail("write");
    }

    ark_shell_printf("\r\nwrite  : ok, %u bytes -> %s\r\n",
                     (unsigned)FS_SCRATCH_LEN, FS_TEST_PATH);

    if(fs_read_verify(FS_TEST_PATH) != 0)
    {
        return -1;
    }

    /* The remount is the point: it discards every cache in the volume and
     * rebuilds the file system state from the chip alone. */
    if(arkrtos_fs_unmount() != 0)
    {
        return fs_fail("unmount");
    }

    if(arkrtos_fs_mount_default() != 0)
    {
        return fs_fail("remount");
    }

    sh_out("remount: ok\r\n");

    if(fs_read_verify(FS_TEST_PATH) != 0)
    {
        return -1;
    }

    if(arkrtos_fs_remove(FS_TEST_PATH) != 0)
    {
        return fs_fail("remove");
    }

    sh_out("remove : ok\r\n");
    sh_out("fs test: PASS\r\n");
    return 0;
}

int cmd_fs(int argc, char *argv[])
{
    const char *sub = (argc >= 2) ? argv[1] : "";

    if(strcmp(sub, "mount") == 0)
    {
        const char *dev = 0;
        uint32 off = 0u;
        uint32 size = 0u;

        if(fs_take_volume(argc, argv, &dev, &off, &size) != 0)
        {
            return fs_usage();
        }

        if(arkrtos_fs_mount(dev, off, size) != 0)
        {
            /* A blank or foreign volume is a normal first-boot state, not a
             * fault: say what it is and what the next step is, and say that
             * nothing was written.  Mount never formats - and if it did, this
             * line would be a lie that costs the operator a volume. */
            int32 e = arkrtos_fs_last_error();
            int   rc = fs_fail("mount");

            if(arkrtos_fs_err_is_unformatted(e) != 0u)
            {
                sh_out("       these bytes hold no littlefs superblock: blank NOR,"
                       " another file system,\r\n"
                       "       or a damaged one.  Nothing was written.\r\n");
                ark_shell_printf("       'fs format %s 0x%X %u' creates an empty volume"
                                 " on purpose (every file on it is lost).\r\n",
                                 dev, (unsigned)off, (unsigned)size);
            }
            return rc;
        }

        ark_shell_printf("\r\nmounted %s at 0x%X, %u bytes\r\n",
                         dev, (unsigned)off, (unsigned)size);
        return 0;
    }

    if(strcmp(sub, "unmount") == 0)
    {
        if(arkrtos_fs_unmount() != 0)
        {
            return fs_fail("unmount");
        }

        sh_out("\r\nunmounted\r\n");
        return 0;
    }

    if(strcmp(sub, "format") == 0)
    {
        const char *dev = 0;
        uint32 off = 0u;
        uint32 size = 0u;

        if(fs_take_volume(argc, argv, &dev, &off, &size) != 0)
        {
            return fs_usage();
        }

        ark_shell_printf("\r\nformatting %s at 0x%X, %u bytes - every file on it is lost\r\n",
                         dev, (unsigned)off, (unsigned)size);

        if(arkrtos_fs_format(dev, off, size) != 0)
        {
            return fs_fail("format");
        }

        sh_out("format : ok, volume is empty and mounted\r\n");
        return 0;
    }

    if(strcmp(sub, "info") == 0)
    {
        return fs_do_info();
    }

    if(strcmp(sub, "ls") == 0)
    {
        return fs_do_ls((argc >= 3) ? argv[2] : "/");
    }

    if(strcmp(sub, "wr") == 0)
    {
        uint32 len = 0u;
        uint32 i;

        if((argc < 4) || (fs_collect_text(argc, argv, 3, &len) != 0))
        {
            return fs_usage();
        }

        if(!arkrtos_fs_mounted())
        {
            sh_out("\r\nnot mounted\r\n");
            return -1;
        }

        if(arkrtos_fs_write_file(argv[2], g_fs_scratch, len) != 0)
        {
            return fs_fail("write");
        }

        ark_shell_printf("\r\nwrote %u bytes to %s:", (unsigned)len, argv[2]);

        for(i = 0u; i < len; i++)
        {
            ark_shell_printf(" %02X", (unsigned)g_fs_scratch[i]);
        }

        sh_out("\r\n");
        return 0;
    }

    if(strcmp(sub, "rd") == 0)
    {
        uint32 len = FS_RD_DEFAULT;
        uint32 got = 0u;

        if(argc < 3)
        {
            return fs_usage();
        }

        if((argc >= 4) && (parse_u32(argv[3], &len) != 0))
        {
            return fs_usage();
        }

        if((len == 0u) || (len > FS_SCRATCH_LEN))
        {
            ark_shell_printf("\r\nlen must be 1..%u\r\n", (unsigned)FS_SCRATCH_LEN);
            return -1;
        }

        if(!arkrtos_fs_mounted())
        {
            sh_out("\r\nnot mounted\r\n");
            return -1;
        }

        if(arkrtos_fs_read_file(argv[2], g_fs_scratch, len, &got) != 0)
        {
            return fs_fail("read");
        }

        ark_shell_printf("\r\n%s: %u bytes\r\n", argv[2], (unsigned)got);
        fs_dump(g_fs_scratch, got);
        return 0;
    }

    if(strcmp(sub, "put") == 0)
    {
        int32  dev;
        uint32 total = 0u;
        uint32 sent = 0u;
        int32  rc = 0;

        if((argc < 4) || (parse_u32(argv[3], &total) != 0) || (total == 0u))
        {
            return fs_usage();
        }

        if(!arkrtos_fs_mounted())
        {
            sh_out("\r\nnot mounted\r\n");
            return -1;
        }

        if(arkrtos_fs_open_write(argv[2]) != 0)
        {
            return fs_fail("open for write");
        }

        dev = arkrtos_shell_uart_open();

        /* Same two precautions the cfg window takes, for the same reasons:
         * drop what the console still holds *before* announcing readiness
         * (the CRLF that ended this very command leaves an LF behind, and one
         * stray byte shifts the whole stream), and own the reads for the
         * duration - a 20 KiB image cannot travel as command-line text. */
        {
            uint8 sink[32];
            uint32 spins = 0u;

            while((spins < 64u) &&
                  (arkrtos_dev_read_internal(dev, sink, (int32)sizeof(sink)) > 0))
            {
                spins++;
            }
        }

        ark_shell_printf("\r\nfs put: %s, %u bytes in %u-byte chunks\r\n",
                         argv[2], (unsigned)total, (unsigned)FS_PUT_CHUNK);
        sh_out("fs put: one flow byte per chunk (0x06 = next, 0x15 = stop)\r\n");

        while(sent < total)
        {
            uint32 want = total - sent;
            uint8  ack = FS_PUT_ACK;

            if(want > FS_PUT_CHUNK)
            {
                want = FS_PUT_CHUNK;
            }

            /* cfg_recv_exact() is just "wait for exactly len bytes"; nothing
             * about it is cfg specific. */
            if(cfg_recv_exact(dev, g_fs_scratch, want, FS_PUT_TIMEOUT_MS) != 0)
            {
                ark_shell_printf("fs put: timed out at %u of %u bytes\r\n",
                                 (unsigned)sent, (unsigned)total);
                rc = -1;
                break;
            }

            if(arkrtos_fs_write_next(g_fs_scratch, want) != 0)
            {
                ark_shell_printf("fs put: write failed (littlefs error %d: %s)\r\n",
                                 (int)arkrtos_fs_last_error(),
                                 arkrtos_fs_error_name(arkrtos_fs_last_error()));
                rc = -1;
                break;
            }

            sent += want;

            /* ACK once the bytes are on the chip rather than once they were
             * received: a block program can be an erase, and a sender that ran
             * ahead would fill the 128-byte console FIFO and lose bytes it
             * would never hear about. */
            (void)arkrtos_dev_write_internal(dev, &ack, 1);
        }

        if(arkrtos_fs_close_write() != 0)
        {
            ark_shell_printf("fs put: close failed (littlefs error %d: %s)\r\n",
                             (int)arkrtos_fs_last_error(),
                             arkrtos_fs_error_name(arkrtos_fs_last_error()));
            rc = -1;
        }

        if(rc == 0)
        {
            ark_shell_printf("fs put: ok, %u bytes in %s\r\n",
                             (unsigned)sent, argv[2]);
            return 0;
        }

        {
            uint8 nak = FS_PUT_NAK;

            (void)arkrtos_dev_write_internal(dev, &nak, 1);
        }

        sh_out("fs put: failed, the file may be incomplete - remove it before retrying\r\n");
        return -1;
    }

    if(strcmp(sub, "rm") == 0)
    {
        if((argc != 3) || !arkrtos_fs_mounted())
        {
            return fs_usage();
        }

        if(arkrtos_fs_remove(argv[2]) != 0)
        {
            return fs_fail("remove");
        }

        ark_shell_printf("\r\nremoved %s\r\n", argv[2]);
        return 0;
    }

    if(strcmp(sub, "test") == 0)
    {
        return fs_do_test();
    }

    if(strcmp(sub, "err") == 0)
    {
        int32 e = arkrtos_fs_last_error();

        ark_shell_printf("\r\nlast littlefs error: %d (%s)\r\n",
                         (int)e, arkrtos_fs_error_name(e));
        return 0;
    }

    return fs_usage();
}



#if ARKRTOS_USE_VFS
/* ------------------------------------------------------------
 * Linux 风格的路径命令
 *
 * 与 fs 命令的分工：fs 管「一个卷」（挂 / 格式化 / 卷内读写），这一组走的是
 * 命名空间——/ 是易失便签区、/dev 是设备注册表、/mnt/<名字> 是持久卷。
 * 名字照 Linux 来：ls / cat / mount 已经是人人都懂的意思，再造一套只会让
 * 人多翻一次帮助。
 * ------------------------------------------------------------ */

static int vfs_fail(const char *what, int32 rc)
{
    ark_shell_printf("\r\n%s: FAILED (%d: %s)\r\n", what, (int)rc,
                     arkrtos_vfs_error_name(rc));
    return -1;
}

/* Same as vfs_fail() but names the operand, so a failure in a multi-operand
 * command points at the path that failed instead of the command as a whole. */
static int vfs_fail_at(const char *what, const char *path, int32 rc)
{
    ark_shell_printf("\r\n%s: %s: FAILED (%d: %s)\r\n", what, path, (int)rc,
                     arkrtos_vfs_error_name(rc));
    return -1;
}

/* ---- Tab completion for VFS paths ----
 *
 * The shell core only knows command names; this provider adds path completion
 * for arguments.  It completes one directory level at a time and offers only
 * names it could actually list, so Tab never invents a path. */
typedef struct {
    const char *prefix;      /* text before the basename, trailing '/' kept */
    uint32      prefix_len;
    const char *base;        /* typed basename fragment to match */
    uint32      base_len;
    char      (*cand)[ARK_SHELL_LINE_SIZE];
    int         max;
    int         n;
} vfs_comp_ctx_t;

static int vfs_comp_cb(const char *name, const arkrtos_vfs_stat_t *st, void *arg)
{
    vfs_comp_ctx_t *c = (vfs_comp_ctx_t *)arg;
    int is_dir;
    uint32 i, o;
    char *dst;

    if (c->n >= c->max) {
        return 1;   /* enough candidates; stop the walk */
    }
    if (strncmp(name, c->base, (size_t)c->base_len) != 0) {
        return 0;
    }
    is_dir = ((st->mode & ARKRTOS_VFS_S_IFDIR) != 0u);
    if ((c->prefix_len + strlen(name) + (is_dir ? 2u : 1u)) > ARK_SHELL_LINE_SIZE) {
        return 0;   /* would not fit; skip rather than truncate into a wrong path */
    }
    dst = c->cand[c->n];
    o = 0u;
    for (i = 0u; i < c->prefix_len; i++) {
        dst[o++] = c->prefix[i];
    }
    for (i = 0u; name[i] != '\0'; i++) {
        dst[o++] = name[i];
    }
    if (is_dir) {
        dst[o++] = '/';
    }
    dst[o] = '\0';
    c->n++;
    return 0;
}

/* Complete a path argument by listing the directory part of the word. */
int vfs_complete(struct ark_shell_s *shell, const char *word, int is_first,
                        char cand[][ARK_SHELL_LINE_SIZE], int max)
{
    char   dirin[ARKRTOS_VFS_PATH_MAX];
    char   vdir[ARKRTOS_VFS_PATH_MAX];
    const char   *slash;
    vfs_comp_ctx_t ctx;
    uint32 i;

    (void)is_first;

    slash = strrchr(word, '/');
    if (slash != NULL) {
        ctx.prefix     = word;
        ctx.prefix_len = (uint32)(slash - word) + 1u;   /* keep trailing '/' */
        ctx.base       = slash + 1;
    } else {
        ctx.prefix     = word;
        ctx.prefix_len = 0u;
        ctx.base       = word;
    }
    ctx.base_len = (uint32)strlen(ctx.base);
    ctx.cand = cand;
    ctx.max  = max;
    ctx.n    = 0;

    /* The directory to list: the typed prefix, or the cwd when no '/' was
     * typed.  resolve() normalises it the same way the commands do. */
    if (ctx.prefix_len == 0u) {
        dirin[0] = '.';
        dirin[1] = '\0';
    } else if (ctx.prefix_len < sizeof(dirin)) {
        for (i = 0u; i < ctx.prefix_len; i++) {
            dirin[i] = ctx.prefix[i];
        }
        dirin[ctx.prefix_len] = '\0';
    } else {
        return 0;
    }

    ark_shell_resolve(shell, dirin, vdir, sizeof(vdir));
    (void)arkrtos_vfs_list(vdir, vfs_comp_cb, &ctx);
    return ctx.n;
}

static int vfs_usage(void)
{
    sh_out("\r\nusage:\r\n");
    sh_out("  mount                     list mount points\r\n");
    sh_out("  mount <target>            mount the board's default volume\r\n");
    sh_out("  mount <dev> <off> <size> <target>   mount a window of <dev>\r\n");
    sh_out("  umount <target>           drop a mount point\r\n");
    sh_out("  ls [path]                 list a directory, default /\r\n");
    sh_out("  cat [path]                print a file, or standard input\r\n");
    sh_out("  rm <path>...              remove files\r\n");
    sh_out("  mkdir <path>...           create directories\r\n");
    sh_out("  rmdir <path>...           remove empty directories\r\n");
    sh_out("  mv <src> <dst>            rename or move\r\n");
    sh_out("  cp <src> <dst>            copy a file\r\n");
    sh_out("  touch <path>...           create empty files\r\n");
    sh_out("  stat <path>               show type, mode, owner and size\r\n");
    sh_out("  chmod <mode> <path>       change access bits (octal, e.g. 644)\r\n");
    sh_out("  chown <uid>[:<gid>] <path>  change numeric owner\r\n");
    sh_out("  off/size are decimal; size 0 means to the end of the device\r\n");
    return -1;
}

/* Resolve an input path against the shell cwd into a VFS path.  Returns -1
 * (leaving out untouched) when the result cannot fit, rather than letting a
 * silent truncation point at a different file. */
static int shell_vfs_path(const char *in, char *out, uint32 cap)
{
    char rpath[ARK_SHELL_PATH_SIZE];

    ark_shell_resolve(&g_shell, in, rpath, sizeof(rpath));
    if(strlen(rpath) >= (size_t)cap)
    {
        return -1;
    }
    (void)strcpy(out, rpath);
    return 0;
}

int32 arkrtos_shell_resolve_path(const char *in, char *out, uint32 cap)
{
    if((in == 0) || (out == 0) || (cap == 0u))
    {
        return -1;
    }
    return (shell_vfs_path(in, out, cap) == 0) ? 0 : -1;
}

typedef struct {
    uint32 count;
    uint32 longfmt;
} vfs_ls_ctx_t;

/* Render mode into the ls -l mode string ("-rw-r--r--").  The type comes from
 * the same bits ls uses; an unknown type is '?', never a dash that would read
 * as "regular file".  out[] holds 11 bytes including the terminator. */
static const char *perm_string(uint32 mode, char *out)
{
    static const char rwx[3] = { 'r', 'w', 'x' };
    uint32 i;

    if((mode & ARKRTOS_VFS_S_IFDIR) != 0u)      { out[0] = 'd'; }
    else if((mode & ARKRTOS_VFS_S_IFCHR) != 0u) { out[0] = 'c'; }
    else if((mode & ARKRTOS_VFS_S_IFREG) != 0u) { out[0] = '-'; }
    else                                        { out[0] = '?'; }

    for(i = 0u; i < 9u; i++)
    {
        out[1u + i] = ((mode & (1u << (8u - i))) != 0u) ? rwx[i % 3u] : '-';
    }
    out[10] = '\0';
    return out;
}

/* One row of ls -l: mode, link count, owner, group, size, name.  There are no
 * hard links in this VFS, so the link count is always 1 - printed rather than
 * omitted so the columns line up the way a reader of ls -l expects.  The time
 * column is left out: the VFS keeps no timestamp, and inventing one is worse
 * than a shorter line. */
static void ls_long_row(const char *name, uint32 mode, uint32 size,
                        uint32 uid, uint32 gid)
{
    char perm[11];

    ark_shell_printf("  %s %3u %-5u %-5u %8u  %s\r\n", perm_string(mode, perm),
                     1u, (unsigned)uid, (unsigned)gid, (unsigned)size, name);
}

static int vfs_ls_cb(const char *name, const arkrtos_vfs_stat_t *st, void *arg)
{
    vfs_ls_ctx_t *c = (vfs_ls_ctx_t *)arg;

    if(c->longfmt != 0u)
    {
        ls_long_row(name, st->mode, st->size, st->uid, st->gid);
    }
    else
    {
        ark_shell_printf("  %-20s %8u%s\r\n", name, (unsigned)st->size,
                         ((st->mode & ARKRTOS_VFS_S_IFDIR) != 0u) ? "  <dir>" : "");
    }
    c->count++;
    return 0;
}

/* ls -a's pseudo-entries.  Their metadata is the real stat of "." and "..",
 * so the columns are not guessed; when a stat fails the row shows '?' and the
 * columns are dashes instead of a mode nobody measured. */
static void ls_pseudo(const char *vpath, uint32 longfmt)
{
    struct ark_vfs_stat st;
    char   dotdot[ARKRTOS_VFS_PATH_MAX];
    size_t vlen = strlen(vpath);

    if(longfmt == 0u)
    {
        ark_shell_printf("  %-20s %8s%s\r\n", ".", "-", "  <dir>");
        ark_shell_printf("  %-20s %8s%s\r\n", "..", "-", "  <dir>");
        return;
    }

    if(ark_vfs_stat(vpath, &st) == 0)
    {
        ls_long_row(".", st.mode, (uint32)st.size, st.uid, st.gid);
    }
    else
    {
        ls_long_row(".", 0u, 0u, 0u, 0u);
    }

    if((strcmp(vpath, "/") == 0) || ((vlen + 3u) >= sizeof(dotdot)))
    {
        (void)strcpy(dotdot, vpath);
    }
    else
    {
        (void)strcpy(dotdot, vpath);
        (void)memcpy(&dotdot[vlen], "/..", 4u);
    }
    if(ark_vfs_stat(dotdot, &st) == 0)
    {
        ls_long_row("..", st.mode, (uint32)st.size, st.uid, st.gid);
    }
    else
    {
        ls_long_row("..", 0u, 0u, 0u, 0u);
    }
}

int cmd_vfs_mount(int argc, char *argv[])
{
    if(argc == 1)
    {
        arkrtos_vfs_mount_info_t mi;
        int32 n = arkrtos_vfs_mount_count();
        int32 i;

        if(n <= 0)
        {
            sh_out("\r\nno mount points\r\n");
            return 0;
        }

        sh_out("\r\ntarget                   fs      source     mode\r\n");
        for(i = 0; i < n; i++)
        {
            if(arkrtos_vfs_mount_info((uint32)i, &mi) != 0)
            {
                continue;
            }
            ark_shell_printf("  %-20s   %-6s  %-9s  %s\r\n", mi.target,
                             (mi.fs != 0) ? mi.fs : "?",
                             (mi.source != 0) ? mi.source : "-",
                             (mi.ro != 0u) ? "ro" : "rw");
        }
        return 0;
    }

    if(argc == 2)
    {
        /* 只给挂载点时用板载默认卷（config/arkrtos_partition.h 的
         * ARKRTOS_FS_*）。给窗口就按窗口来，两个形式的语义各自唯一——
         * 这里不替任何人猜一个「大概这么大」。 */
        int32 rc = arkrtos_vfs_mount_volume(ARKRTOS_FS_DEV_NAME,
                                          (uint32)ARKRTOS_FS_BASE,
                                          (uint32)ARKRTOS_FS_SIZE, argv[1]);

        if(rc != 0)
        {
            return vfs_fail("mount", rc);
        }

        ark_shell_printf("\r\n%s mounted at %s\r\n", ARKRTOS_FS_DEV_NAME,
                         argv[1]);
        return 0;
    }

    if(argc == 5)
    {
        uint32 off = 0u;
        uint32 size = 0u;
        int32 rc;

        if((parse_u32(argv[2], &off) != 0) || (parse_u32(argv[3], &size) != 0))
        {
            return vfs_usage();
        }

        rc = arkrtos_vfs_mount_volume(argv[1], off, size, argv[4]);
        if(rc != 0)
        {
            return vfs_fail("mount", rc);
        }

        ark_shell_printf("\r\n%s mounted at %s\r\n", argv[1], argv[4]);
        return 0;
    }

    return vfs_usage();
}

int cmd_vfs_umount(int argc, char *argv[])
{
    int32 rc;

    if(argc != 2)
    {
        return vfs_usage();
    }

    rc = arkrtos_vfs_umount(argv[1]);
    if(rc != 0)
    {
        return vfs_fail("umount", rc);
    }

    ark_shell_printf("\r\n%s unmounted\r\n", argv[1]);
    return 0;
}

static const char *path_basename(const char *p);

int cmd_vfs_ls(int argc, char *argv[])
{
    vfs_ls_ctx_t ctx;
    const char *dir = 0;
    char   vpath[ARKRTOS_VFS_PATH_MAX];
    uint32 show_all = 0u;
    uint32 shown;
    int32  rc;
    int    i;

    ctx.count   = 0u;
    ctx.longfmt = 0u;

    /* Linux habit: options may be grouped ("-la") and may precede or follow
     * the path.  Anything else is a usage error rather than a guess. */
    for(i = 1; i < argc; i++)
    {
        const char *a = argv[i];

        if((a[0] == '-') && (a[1] != '\0'))
        {
            const char *p;

            for(p = a + 1; *p != '\0'; p++)
            {
                if(*p == 'l')
                {
                    ctx.longfmt = 1u;
                }
                else if(*p == 'a')
                {
                    show_all = 1u;
                }
                else
                {
                    return vfs_usage();
                }
            }
        }
        else if(dir == 0)
        {
            dir = a;
        }
        else
        {
            return vfs_usage();
        }
    }

    if(shell_vfs_path(dir, vpath, sizeof(vpath)) != 0)
    {
        sh_out("ls: path too long\r\n");
        return 1;
    }

    /* Linux `ls <file>`: a non-directory argument lists that entry
     * itself instead of failing with "not a directory".  The mode comes
     * from a real stat, nothing is guessed. */
    {
        struct ark_vfs_stat fst;

        if((ark_vfs_stat(vpath, &fst) == 0) && ((fst.mode & ARK_S_IFDIR) == 0u))
        {
            if(ctx.longfmt != 0u)
            {
                ls_long_row(path_basename(vpath), fst.mode, (uint32)fst.size,
                            fst.uid, fst.gid);
            }
            else
            {
                ark_shell_printf("  %s\r\n", path_basename(vpath));
            }
            return 0;
        }
    }

    if(show_all != 0u)
    {
        ls_pseudo(vpath, ctx.longfmt);
    }

    rc = arkrtos_vfs_list(vpath, vfs_ls_cb, &ctx);
    if(rc != 0)
    {
        return vfs_fail("ls", rc);
    }

    shown = ctx.count + ((show_all != 0u) ? 2u : 0u);
    ark_shell_printf("  %u entr%s\r\n", (unsigned)shown,
                     (shown == 1u) ? "y" : "ies");
    return 0;
}

int cmd_vfs_cat(int argc, char *argv[])
{
    static uint8 buf[256];      /* 静态：不再向 shell 任务的栈要一段 */
    char   vpath[ARKRTOS_VFS_PATH_MAX];
    int32 fd;
    int32 n;
    int32 crc;
    uint32 total = 0u;
    uint32 i;

    if(argc > 2)
    {
        return vfs_usage();
    }

    if(argc == 1)
    {
        /* No path: read standard input, the way cat does on a POSIX system.
         * The stream is a pipe or a '<' redirection; there is no interactive
         * stdin to wait on, so an empty stream just yields nothing. */
        int c;

        sh_out("\r\n");
        while((c = ark_shell_stdin_getc()) >= 0)
        {
            if(c == '\n')
            {
                sh_out("\r\n");
            }
            else if((c >= 0x20) && (c < 0x7F))
            {
                ark_shell_printf("%c", (char)c);
            }
            else
            {
                ark_shell_printf("\\x%02X", (unsigned)c);
            }
            total++;
        }
        ark_shell_printf("\r\n%u bytes\r\n", (unsigned)total);
        return 0;
    }

    if(shell_vfs_path(argv[1], vpath, sizeof(vpath)) != 0)
    {
        sh_out("cat: path too long\r\n");
        return 1;
    }

    fd = arkrtos_vfs_open_read(vpath);
    if(fd < 0)
    {
        return vfs_fail("cat", fd);
    }

    sh_out("\r\n");
    while((n = arkrtos_vfs_read(fd, buf, (uint32)sizeof(buf))) > 0)
    {
        for(i = 0u; i < (uint32)n; i++)
        {
            uint32 c = (uint32)buf[i];

            if(c == (uint32)'\n')
            {
                sh_out("\r\n");
            }
            else if((c >= 0x20u) && (c < 0x7Fu))
            {
                ark_shell_printf("%c", (char)c);
            }
            else
            {
                ark_shell_printf("\\x%02X", (unsigned)c);
            }
        }
        total += (uint32)n;
    }

    crc = arkrtos_vfs_close(fd);

    if(n < 0)
    {
        return vfs_fail("cat", n);
    }
    if(crc != 0)
    {
        return vfs_fail("cat (close)", crc);
    }

    ark_shell_printf("\r\n%u bytes\r\n", (unsigned)total);
    return 0;
}

int cmd_vfs_rm(int argc, char *argv[])
{
    int i;
    int failed = 0;

    if(argc < 2)
    {
        return vfs_usage();
    }
    for(i = 1; i < argc; i++)
    {
        char   vpath[ARKRTOS_VFS_PATH_MAX];
        struct ark_vfs_stat st;
        int32  rc;

        if(shell_vfs_path(argv[i], vpath, sizeof(vpath)) != 0)
        {
            sh_out("rm: path too long\r\n");
            failed = 1;
            continue;
        }
        /* rm removes files, not directories - that is rmdir's job. */
        if((ark_vfs_stat(vpath, &st) == 0) && ((st.mode & ARK_S_IFDIR) != 0u))
        {
            ark_shell_printf("\r\nrm: %s: Is a directory\r\n", argv[i]);
            failed = 1;
            continue;
        }
        rc = arkrtos_vfs_remove(vpath);
        if(rc != 0)
        {
            (void)vfs_fail_at("rm", argv[i], rc);
            failed = 1;
            continue;
        }
        ark_shell_printf("\r\n%s removed\r\n", argv[i]);
    }
    return failed;
}

/* ------------------------------------------------------------
 * mkdir / rmdir / mv / cp / touch / stat
 *
 * The rest of the Linux file-command set.  Each one prints the
 * filesystem's own error code; nothing here invents a reason.
 * ------------------------------------------------------------ */

static const char *path_basename(const char *p)
{
    const char *s = strrchr(p, '/');

    return (s != 0) ? (s + 1) : p;
}

int cmd_vfs_mkdir(int argc, char *argv[])
{
    int i;
    int failed = 0;

    if(argc < 2)
    {
        return vfs_usage();
    }
    /* Like Linux: any number of operands; each keeps its own result so one
     * failure does not hide the entries that were created. */
    for(i = 1; i < argc; i++)
    {
        char  vpath[ARKRTOS_VFS_PATH_MAX];
        int32 rc;

        if(shell_vfs_path(argv[i], vpath, sizeof(vpath)) != 0)
        {
            sh_out("mkdir: path too long\r\n");
            failed = 1;
            continue;
        }
        rc = ark_vfs_mkdir(vpath);
        if(rc != 0)
        {
            (void)vfs_fail_at("mkdir", argv[i], rc);
            failed = 1;
            continue;
        }
        ark_shell_printf("\r\n%s created\r\n", argv[i]);
    }
    return failed;
}

int cmd_vfs_rmdir(int argc, char *argv[])
{
    int i;
    int failed = 0;

    if(argc < 2)
    {
        return vfs_usage();
    }
    for(i = 1; i < argc; i++)
    {
        char vpath[ARKRTOS_VFS_PATH_MAX];
        int32 rc;

        if(shell_vfs_path(argv[i], vpath, sizeof(vpath)) != 0)
        {
            sh_out("rmdir: path too long\r\n");
            failed = 1;
            continue;
        }
        /* The core does the work: it asks the filesystem what the entry
         * is, so a file is never taken by rmdir, and an empty directory
         * only is removed.  Kept in one place so the shell and an App
         * cannot drift apart on what rmdir means. */
        rc = ark_vfs_rmdir(vpath);
        if(rc != 0)
        {
            if(rc == ARK_E_NOTDIR)
            {
                /* rmdir is for directories; refusing a file is not a guess. */
                ark_shell_printf("\r\nrmdir: %s: Not a directory\r\n", argv[i]);
            }
            else
            {
                (void)vfs_fail_at("rmdir", argv[i], rc);
            }
            failed = 1;
            continue;
        }
        ark_shell_printf("\r\n%s removed\r\n", argv[i]);
    }
    return failed;
}

int cmd_vfs_mv(int argc, char *argv[])
{
    char vsrc[ARKRTOS_VFS_PATH_MAX];
    char vdst[ARKRTOS_VFS_PATH_MAX];
    int32 rc;

    if(argc != 3)
    {
        return vfs_usage();
    }
    if((shell_vfs_path(argv[1], vsrc, sizeof(vsrc)) != 0) ||
       (shell_vfs_path(argv[2], vdst, sizeof(vdst)) != 0))
    {
        sh_out("mv: path too long\r\n");
        return 1;
    }
    rc = ark_vfs_rename(vsrc, vdst);
    if(rc != 0)
    {
        return vfs_fail("mv", rc);
    }
    ark_shell_printf("\r\n%s -> %s\r\n", argv[1], argv[2]);
    return 0;
}

int cmd_vfs_cp(int argc, char *argv[])
{
    static uint8 buf[256];
    char vsrc[ARKRTOS_VFS_PATH_MAX];
    char vdst[ARKRTOS_VFS_PATH_MAX];
    struct ark_vfs_stat st;
    int32 fd;
    int32 h;
    int32 n;
    uint32 total = 0u;

    if(argc != 3)
    {
        return vfs_usage();
    }
    if((shell_vfs_path(argv[1], vsrc, sizeof(vsrc)) != 0) ||
       (shell_vfs_path(argv[2], vdst, sizeof(vdst)) != 0))
    {
        sh_out("cp: path too long\r\n");
        return 1;
    }

    /* cp file dir/ : when the destination is an existing directory the copy
     * lands inside it under the source basename, as on Linux.  Any other
     * shape is used verbatim - a missing destination is a new file name,
     * never a guess about which directory was meant. */
    if((ark_vfs_stat(vdst, &st) == 0) && ((st.mode & ARK_S_IFDIR) != 0u))
    {
        const char *base = path_basename(vsrc);
        uint32 len  = (uint32)strlen(vdst);
        uint32 blen = (uint32)strlen(base);

        if((len + 1u + blen + 1u) > sizeof(vdst))
        {
            sh_out("cp: path too long\r\n");
            return 1;
        }
        if((len > 0u) && (vdst[len - 1u] != '/'))
        {
            vdst[len++] = '/';
        }
        (void)memcpy(&vdst[len], base, blen + 1u);
    }

    fd = arkrtos_vfs_open_read(vsrc);
    if(fd < 0)
    {
        return vfs_fail("cp", fd);
    }
    h = arkrtos_vfs_app_open(vdst, ARKRTOS_VFS_O_WRONLY |
                                  ARKRTOS_VFS_O_CREAT | ARKRTOS_VFS_O_TRUNC);
    if(h < 0)
    {
        (void)arkrtos_vfs_close(fd);
        return vfs_fail("cp", h);
    }
    sh_out("\r\n");
    while((n = arkrtos_vfs_read(fd, buf, (uint32)sizeof(buf))) > 0)
    {
        int32 w = arkrtos_vfs_app_write(h, buf, (uint32)n);

        if(w != n)
        {
            (void)arkrtos_vfs_app_close(h);
            (void)arkrtos_vfs_close(fd);
            return vfs_fail("cp", (w < 0) ? w : -1);
        }
        total += (uint32)n;
    }
    if(n < 0)
    {
        (void)arkrtos_vfs_app_close(h);
        (void)arkrtos_vfs_close(fd);
        return vfs_fail("cp", n);
    }
    (void)arkrtos_vfs_app_close(h);
    (void)arkrtos_vfs_close(fd);
    ark_shell_printf("%u bytes copied\r\n", (unsigned)total);
    return 0;
}

int cmd_vfs_touch(int argc, char *argv[])
{
    int i;
    int failed = 0;

    if(argc < 2)
    {
        return vfs_usage();
    }
    for(i = 1; i < argc; i++)
    {
        char vpath[ARKRTOS_VFS_PATH_MAX];
        struct ark_vfs_stat st;
        int32 h;

        if(shell_vfs_path(argv[i], vpath, sizeof(vpath)) != 0)
        {
            sh_out("touch: path too long\r\n");
            failed = 1;
            continue;
        }
        /* Existing file: the VFS keeps no settable timestamp, so touching it
         * has nothing to change - say so by doing nothing rather than
         * claiming to have updated a time that does not exist. */
        if(ark_vfs_stat(vpath, &st) == 0)
        {
            continue;
        }
        /* O_CREAT without O_TRUNC: creates an empty file, or leaves an
         * existing one untouched (the create already happened if needed). */
        h = arkrtos_vfs_app_open(vpath, ARKRTOS_VFS_O_WRONLY | ARKRTOS_VFS_O_CREAT);
        if(h < 0)
        {
            (void)vfs_fail_at("touch", argv[i], h);
            failed = 1;
            continue;
        }
        (void)arkrtos_vfs_app_close(h);
    }
    return failed;
}

/* Parse 3 or 4 octal digits into permission bits.  Anything shorter, longer or
 * non-octal is refused: a mode like "64" is ambiguous, and symbolic forms
 * (u+x) are not implemented, so neither is guessed at. */
static int parse_octal_mode(const char *s, uint32 *out)
{
    uint32 v = 0u;
    int    n = 0;

    if((s == 0) || (*s == '\0'))
    {
        return -1;
    }
    while(*s != '\0')
    {
        if((*s < '0') || (*s > '7') || (n >= 4))
        {
            return -1;
        }
        v = (v << 3) + (uint32)(*s - '0');
        s++;
        n++;
    }
    if(n < 3)
    {
        return -1;
    }
    *out = v & ARKRTOS_VFS_S_PERM;
    return 0;
}

int cmd_vfs_chmod(int argc, char *argv[])
{
    char   vpath[ARKRTOS_VFS_PATH_MAX];
    uint32 mode = 0u;
    int32  rc;

    if(argc != 3)
    {
        return vfs_usage();
    }
    if(parse_octal_mode(argv[1], &mode) != 0)
    {
        sh_out("chmod: mode must be 3 or 4 octal digits, e.g. 644 or 0755\r\n");
        return 1;
    }
    if(shell_vfs_path(argv[2], vpath, sizeof(vpath)) != 0)
    {
        sh_out("chmod: path too long\r\n");
        return 1;
    }
    rc = arkrtos_vfs_chmod(vpath, mode);
    if(rc != 0)
    {
        return vfs_fail("chmod", rc);
    }
    {
        /* Report the mode the file now has, read back - not the number we
         * asked for, which is not the same thing on a mount that clamps. */
        struct ark_vfs_stat st;
        char perm[11];

        if(ark_vfs_stat(vpath, &st) == 0)
        {
            ark_shell_printf("\r\n%s: %s\r\n", argv[2], perm_string(st.mode, perm));
        }
        else
        {
            ark_shell_printf("\r\n%s: mode changed\r\n", argv[2]);
        }
    }
    return 0;
}

int cmd_vfs_chown(int argc, char *argv[])
{
    char   vpath[ARKRTOS_VFS_PATH_MAX];
    struct ark_vfs_stat st;
    const char *spec;
    const char *colon;
    char   part[16];
    uint32 uid = 0u;
    uint32 gid = 0u;
    int    set_uid = 0;
    int    set_gid = 0;
    int32  rc;

    if(argc != 3)
    {
        return vfs_usage();
    }
    spec  = argv[1];
    colon = strchr(spec, ':');
    if(colon == 0)
    {
        if(parse_u32(spec, &uid) != 0)
        {
            sh_out("chown: owner must be a decimal id\r\n");
            return 1;
        }
        set_uid = 1;
    }
    else
    {
        size_t n = (size_t)(colon - spec);

        if(n > 0u)
        {
            if(n >= sizeof(part))
            {
                sh_out("chown: owner id too long\r\n");
                return 1;
            }
            (void)memcpy(part, spec, n);
            part[n] = '\0';
            if(parse_u32(part, &uid) != 0)
            {
                sh_out("chown: owner must be a decimal id\r\n");
                return 1;
            }
            set_uid = 1;
        }
        if(colon[1] != '\0')
        {
            if(parse_u32(colon + 1, &gid) != 0)
            {
                sh_out("chown: group must be a decimal id\r\n");
                return 1;
            }
            set_gid = 1;
        }
    }
    if((set_uid == 0) && (set_gid == 0))
    {
        sh_out("chown: nothing to change\r\n");
        return 1;
    }
    if(shell_vfs_path(argv[2], vpath, sizeof(vpath)) != 0)
    {
        sh_out("chown: path too long\r\n");
        return 1;
    }

    /* Whichever half was left out keeps its current value - read it, do not
     * assume 0. */
    rc = ark_vfs_stat(vpath, &st);
    if(rc != 0)
    {
        return vfs_fail("chown", rc);
    }
    if(set_uid == 0) { uid = st.uid; }
    if(set_gid == 0) { gid = st.gid; }

    rc = arkrtos_vfs_chown(vpath, uid, gid);
    if(rc != 0)
    {
        return vfs_fail("chown", rc);
    }
    ark_shell_printf("\r\n%s: owner %u:%u\r\n", argv[2],
                     (unsigned)uid, (unsigned)gid);
    return 0;
}

int cmd_vfs_stat(int argc, char *argv[])
{
    char vpath[ARKRTOS_VFS_PATH_MAX];
    struct ark_vfs_stat st;
    const char *type;
    int32 rc;

    if(argc != 2)
    {
        return vfs_usage();
    }
    if(shell_vfs_path(argv[1], vpath, sizeof(vpath)) != 0)
    {
        sh_out("stat: path too long\r\n");
        return 1;
    }
    rc = ark_vfs_stat(vpath, &st);
    if(rc != 0)
    {
        return vfs_fail("stat", rc);
    }
    if((st.mode & ARK_S_IFDIR) != 0u)
    {
        type = "directory";
    }
    else if((st.mode & ARK_S_IFCHR) != 0u)
    {
        type = "character device";
    }
    else if((st.mode & ARK_S_IFREG) != 0u)
    {
        type = "regular file";
    }
    else
    {
        type = "unknown";
    }
    {
        char perm[11];
        ark_shell_printf("\r\n  File: %s\r\n", vpath);
        ark_shell_printf("  Type: %s\r\n", type);
        ark_shell_printf("  Mode: %s\r\n", perm_string(st.mode, perm));
        /* Numeric ids on purpose: there is no user database here, so a name
         * would be a made-up answer. */
        ark_shell_printf("  Owner: uid=%u gid=%u\r\n",
                         (unsigned)st.uid, (unsigned)st.gid);
    }
    if((st.mode & ARK_S_IFDIR) == 0u)
    {
        ark_shell_printf("  Size: %u bytes\r\n", (unsigned)st.size);
    }
    if(st.mtime == 0u)
    {
        sh_out("  Time: none (this filesystem keeps no timestamps)\r\n");
    }
    else
    {
        ark_shell_printf("  Time: %u\r\n", (unsigned)st.mtime);
    }
    return 0;
}
#endif /* ARKRTOS_USE_VFS */
#endif /* ARKRTOS_USE_FS */
#if ARKRTOS_USE_MINIAPP
/* 小程序：把文件系统里的一份负载按需 load 进 RAM 执行，退出/停止就把那块 RAM 还回池。
 * 它占的是「代码 + RW/ZI + 栈」整块池内存，所以运行期与 App 抢的是同一个池。*/
/* mini autostart 列表的回调：把「列了几条、缺几条、被禁几条」带回调用处。
 * 存在与否用 fs 的 stat 判、禁用与否用崩溃日记判——两件事各有各的账本，
 * 这里只负责把两边的答案并排摆出来，不替它们下结论。 */
typedef struct {
    uint32 index;
    uint32 missing;
    uint32 disabled;
} mini_auto_ctx_t;

/* 清单里的一条路径存在不存在：**必须按内核读镜像的那一层去问**。
 * mini 在 VFS 构建里按命名空间路径读（/mnt/nor/hello/hello.svcm），而
 * arkrtos_fs_stat_path() 解的是卷内路径（/hello/hello.svcm）。拿错一层问，
 * 就会得到「明明刚 mini run 成功、清单却报 not in the volume」这种读数——
 * 上板实测过：删了又写都不管用，因为文件本来就在那儿，只是查错了地方。 */
static int mini_auto_stat(const char *path, uint32 *size, uint32 *is_dir,
                           int32 *out_err, const char **out_layer)
{
#if ARKRTOS_MINI_READ_VFS
    uint32 mode = 0u;
    int32  rc   = arkrtos_vfs_app_stat(path, size, &mode);

    *out_layer = "vfs";
    *out_err   = rc;
    if(rc != ARK_VFS_OK)
    {
        return -1;
    }
    if(is_dir != 0)
    {
        *is_dir = ((mode & ARKRTOS_VFS_S_IFDIR) != 0u) ? 1u : 0u;
    }
    return 0;
#else
    int32 rc = arkrtos_fs_stat_path(path, size, is_dir);

    *out_layer = "fs";
    *out_err   = arkrtos_fs_last_error();
    return (rc == 0) ? 0 : -1;
#endif
}

static int mini_auto_show(const char *path, void *arg)
{
    mini_auto_ctx_t *c = (mini_auto_ctx_t *)arg;
    uint32 size   = 0u;
    uint32 is_dir = 0u;
    uint32 hash;
    int32  err    = 0;
    const char *layer = "fs";

    c->index++;

    ark_shell_printf("  [%u] %s\r\n", (unsigned)c->index, path);

    if(mini_auto_stat(path, &size, &is_dir, &err, &layer) != 0)
    {
        c->missing++;
#if ARKRTOS_MINI_READ_VFS
        ark_shell_printf("      not in the volume (vfs err %d: %s)\r\n",
                         (int)err, arkrtos_vfs_error_name(err));
#else
        (void)layer;
        ark_shell_printf("      not in the volume (fs err %d: %s)\r\n",
                         (int)err, arkrtos_fs_error_name(err));
#endif
    }
    else
    {
        (void)layer;
        ark_shell_printf("      %u bytes%s\r\n", (unsigned)size,
                         (is_dir != 0u) ? " (a directory)" : "");
    }

    hash = arkrtos_crash_hash_path(path);
    if(arkrtos_crash_mini_disabled(hash) != 0u)
    {
        c->disabled++;
        ark_shell_printf("      DISABLED: %u consecutive faults (mini forget %s)\r\n",
                         (unsigned)arkrtos_crash_mini_count(hash), path);
    }

    return 0;
}
int cmd_mini(int argc, char *argv[])
{
    arkrtos_mini_info_t mi;
    uint32 n;
    int32 rc;

    if((argc == 3) && (strcmp(argv[1], "run") == 0))
    {
        /* 相对路径先按 shell 的当前目录解开，和 ls/cat/rm/cc 一致。
         * 之前这里把 argv[2] 原样交给装载器，而装载器只认文件系统里的路径：
         * 于是 `cd /mnt/nor` 之后 `make` 能从相对名读到 makefile、`mini run
         * hello.svcm` 同一目录下却报 rc=-18「no such file or mount」——
         * 两条路对同一个字符串给出相反答案，是最难查的一类不一致。
         * 绝对路径解出来仍是原样，行为逐字节不变。
         * 只在 VFS 构建里解开：没有 VFS 时 mini 读的是另一条后端，路径本来就
         * 是根相对，那条构建保持逐字节不变。 */
        static char  img[ARK_SHELL_PATH_SIZE];
        const char  *target = argv[2];

#if ARKRTOS_USE_VFS
        if(shell_vfs_path(argv[2], img, sizeof(img)) != 0)
        {
            ark_shell_printf("\r\nmini run: %s: path too long\r\n", argv[2]);
            return 1;
        }
        /* 报错里仍显示用户打的那个名字（argv[2]），动作和计数用解开后的
         * 路径：现场找的是自己刚敲的那一行，而同一个文件不该有两个键。 */
        target = img;
#endif

        rc = arkrtos_mini_run(target);

        if(rc != 0)
        {
            ark_shell_printf("\r\nmini run: %s failed, rc=%d\r\n", argv[2], (int)rc);
            if(rc == ARKRTOS_LOADER_ERR_SIZE)
            {
                /* 上限是运行期可配的：报不下就得说清楚当前上限是多少，
                 * 否则用户只能猜。 */
                ark_shell_printf("  size limit: %u B now (mini limit <bytes>)\r\n",
                                 (unsigned)arkrtos_mini_max_bytes());
            }
            if(rc == ARKRTOS_LOADER_ERR_BUSY)
            {
                ark_shell_printf("  running %u of %u allowed (mini count <n>),"
                                 " or a load is already in progress\r\n",
                                 (unsigned)arkrtos_mini_running(),
                                 (unsigned)arkrtos_mini_max_count());
            }
            if(rc == ARKRTOS_MINI_ERR_DISABLED)
            {
                /* 到连续故障上限：说清是几次、怎么放行。上限与带槽位的 App
                 * 是同一个数字（分区表 cfg_restart_max），不是小程序专用值。
                 * 计数按解开后的路径取键：同一个镜像用相对名和绝对名跑的是
                 * 同一份文件，故障次数必须累到同一个计数器上，否则连续故障
                 * 上限可以被两种写法轮流绕开。显示仍是用户敲的那个名字。 */
                ark_shell_printf("  disabled after %u consecutive faults"
                                 " (mini forget %s)\r\n",
                                 (unsigned)arkrtos_crash_mini_count(
                                     arkrtos_crash_hash_path(target)),
                                 argv[2]);
            }
            if(rc == ARKRTOS_MINI_ERR_FS)
            {
#if (ARKRTOS_MINI_READ_VFS == 1)
                /* 报真正读失败那一层的原因：走 VFS 时 arkrtos_fs_last_error()
                 * 恒为 0（ok），照旧打印就是「失败 + ok」这种错答案。 */
                int32 e = arkrtos_mini_last_read_error();

                if(e == ARKRTOS_MINI_ERR_SHORT_READ)
                {
                    ark_shell_printf("  the image file ends early (short read)\r\n");
                }
                else
                {
                    ark_shell_printf("  read: %d (%s)\r\n",
                                     (int)e, arkrtos_vfs_error_name(e));
                }
#else
                int32 e = arkrtos_fs_last_error();
                ark_shell_printf("  file system: %d (%s)\r\n",
                                 (int)e, arkrtos_fs_error_name(e));
#endif
            }
            return 1;
        }
        if(arkrtos_mini_info(&mi) != 0)
        {
            /* 装载成功，但此刻已没有可报的实例：镜像可能刚跑起来就连续故障、
             * 并已被禁用（阈值与带槽位 App 相同），也可能自己跑完退出了。
             * 此时 mi 根本没被写入，打印它等于打印栈上垃圾——宁可报错，也不给
             * 看似权威的错答案。措辞把两种可能都点出来，再按故障账本补一句：
             * 有记录才说"曾经故障过"，没记录就不替读者判断是哪一种。 */
            ark_shell_printf("\r\nmini run: %s loaded but no longer running"
                             " (a program that exits on its own looks the same;"
                             " 'dmesg' shows which)\r\n", argv[2]);
            if(arkrtos_crash_mini_count(arkrtos_crash_hash_path(target)) != 0u)
            {
                ark_shell_printf("  it has faulted before: 'mini forget %s'"
                                 " clears that memory\r\n", argv[2]);
            }
            return 1;
        }
        ark_shell_printf("\r\nmini running: task %u, code %u B in %u B @ 0x%08X, RW/stack %u B @ 0x%08X\r\n",
                         (unsigned)mi.task_id, (unsigned)mi.code_size,
                         (unsigned)mi.code_block, (unsigned)mi.code_base,
                         (unsigned)mi.ram_size, (unsigned)mi.ram_base);
        ark_shell_printf("  running %u of %u allowed\r\n",
                         (unsigned)arkrtos_mini_running(),
                         (unsigned)arkrtos_mini_max_count());
        return 0;
    }

#if (ARKRTOS_USE_TASK_SWAP == 1)
    /* mini swap <编号> <新镜像>：A/B 两块 RAM 的无缝升级。
     * 编号与 'mini' / 'mini stop <n>' 是同一套（密集编号）。新镜像装进第二对
     * 块，切换在它的下一次系统调用里落地，落地那一刻旧的一对块才还池。 */
    if((argc == 4) && (strcmp(argv[1], "swap") == 0))
    {
        static char  simg[ARK_SHELL_PATH_SIZE];
        const char  *starget = argv[3];
        uint32       want = 0u;
        int32        src;

        if(parse_u32(argv[2], &want) != 0)
        {
            ark_shell_printf("\r\nmini swap: %s is not a decimal number (see 'mini')\r\n",
                             argv[2]);
            return 1;
        }

#if ARKRTOS_USE_VFS
        if(shell_vfs_path(argv[3], simg, sizeof(simg)) != 0)
        {
            ark_shell_printf("\r\nmini swap: %s: path too long\r\n", argv[3]);
            return 1;
        }
        starget = simg;
#endif

        src = arkrtos_mini_swap_at(want, starget);
        if(src != 0)
        {
            ark_shell_printf("\r\nmini swap: %u -> %s failed, rc=%d\r\n",
                             (unsigned)want, argv[3], (int)src);
            if(src == ARKRTOS_LOADER_ERR_STATE)
            {
                ark_shell_printf("  no running MiniApp with that number"
                                 " (running %u, see 'mini')\r\n",
                                 (unsigned)arkrtos_mini_running());
            }
            if(src == ARKRTOS_LOADER_ERR_BUSY)
            {
                sh_out("  a load or another swap is already in progress\r\n");
            }
            if(src == ARKRTOS_SWAP_ERR_LAYOUT)
            {
                sh_out("  the two images need the same code/RAM block sizes"
                       " (a layout change needs the handover protocol)\r\n");
            }
            if(src == ARKRTOS_LOADER_ERR_SIZE)
            {
                ark_shell_printf("  size limit: %u B now (mini limit <bytes>)\r\n",
                                 (unsigned)arkrtos_mini_max_bytes());
            }
            if(src == ARKRTOS_MINI_ERR_FS)
            {
                sh_out("  cannot read the new image (check 'ls' and the path)\r\n");
            }
            return 1;
        }

        ark_shell_printf("\r\nmini swap: %u -> %s armed; it switches at the next syscall,"
                         " the old blocks return then\r\n", (unsigned)want, argv[3]);
        return 0;
    }
#endif /* ARKRTOS_USE_TASK_SWAP */

    /* mini stop / mini stop <n>：停全部，或只停第 n 个（编号见 'mini'） */
    if((argc >= 2) && (argc <= 3) && (strcmp(argv[1], "stop") == 0))
    {
        if(argc == 2)
        {
            rc = arkrtos_mini_stop();
            if(rc != 0)
            {
                ark_shell_printf("\r\nmini stop: nothing to stop (rc=%d)\r\n", (int)rc);
                return 1;
            }
            ark_shell_printf("\r\nmini stopped: all blocks returned to the pool\r\n");
            return 0;
        }

        {
            uint32 want = 0u;

            if(parse_u32(argv[2], &want) != 0)
            {
                ark_shell_printf("\r\nmini stop: %s is not a decimal number\r\n", argv[2]);
                return 1;
            }

            rc = arkrtos_mini_stop_at(want);
            if(rc != 0)
            {
                ark_shell_printf("\r\nmini stop %u: no such running MiniApp (rc=%d, see 'mini')\r\n",
                                 (unsigned)want, (int)rc);
                return 1;
            }
            ark_shell_printf("\r\nmini %u stopped: block returned to the pool\r\n",
                             (unsigned)want);
            return 0;
        }
    }

    /* mini count / mini count <n>：同一时刻最多跑几个。运行期只能收紧，
     * 不能放宽到超过编译期的 ARKRTOS_MINI_MAX（超范围报 PARAM，不夹取）。 */
    if((argc >= 2) && (argc <= 3) && (strcmp(argv[1], "count") == 0))
    {
        if(argc == 2)
        {
            ark_shell_printf("\r\nmini count: %u allowed, %u running (compile-time max %u)\r\n",
                             (unsigned)arkrtos_mini_max_count(),
                             (unsigned)arkrtos_mini_running(),
                             (unsigned)ARKRTOS_MINI_MAX);
            return 0;
        }

        {
            uint32 want = 0u;

            if(parse_u32(argv[2], &want) != 0)
            {
                ark_shell_printf("\r\nmini count: %s is not a decimal number\r\n", argv[2]);
                return 1;
            }

            rc = arkrtos_mini_set_max_count(want);
            if(rc != 0)
            {
                ark_shell_printf("\r\nmini count: %u rejected, rc=%d (range 1..%u)\r\n",
                                 (unsigned)want, (int)rc, (unsigned)ARKRTOS_MINI_MAX);
                return 1;
            }
            ark_shell_printf("\r\nmini count set to %u\r\n",
                             (unsigned)arkrtos_mini_max_count());
            return 0;
        }
    }

    /* mini forget <path>：清除某路径的崩溃记忆，让被禁用的小程序重新可跑 */
    if((argc == 3) && (strcmp(argv[1], "forget") == 0))
    {
        /* 与 mini run 同一套路径规则：否则 'run hello.svcm' 被禁用后，
         * 'forget hello.svcm' 清的是另一个键，怎么敲都放行不了。 */
        static char fimg[ARK_SHELL_PATH_SIZE];
        const char *ftarget = argv[2];

#if ARKRTOS_USE_VFS
        if(shell_vfs_path(argv[2], fimg, sizeof(fimg)) != 0)
        {
            ark_shell_printf("\r\nmini forget: %s: path too long\r\n", argv[2]);
            return 1;
        }
        ftarget = fimg;
#endif
        if(arkrtos_mini_forget(ftarget) != 0)
        {
            ark_shell_printf("\r\nmini forget: empty path\r\n");
            return 1;
        }
        ark_shell_printf("\r\nmini forget: %s enabled again\r\n", argv[2]);
        return 0;
    }

    /* mini limit / mini limit <bytes>：单个小程序的体积上限（块的字节数）。
     * 只影响下一次装载；越界或非 2 的幂报 PARAM，不静默夹取。 */
    if((argc >= 2) && (argc <= 3) && (strcmp(argv[1], "limit") == 0))
    {
        if(argc == 2)
        {
            ark_shell_printf("\r\nmini size limit: %u B (default %u B, range %u..%u B)\r\n",
                             (unsigned)arkrtos_mini_max_bytes(),
                             (unsigned)SLOT_RAM_MAX_BLOCK,
                             (unsigned)SLOT_RAM_MIN_BLOCK,
                             (unsigned)SLOT_RAM_MAX_BLOCK);
            return 0;
        }

        {
            uint32 want = 0u;

            if(parse_u32(argv[2], &want) != 0)
            {
                ark_shell_printf("\r\nmini limit: %s is not a decimal number\r\n",
                                 argv[2]);
                return 1;
            }

            rc = arkrtos_mini_set_max_bytes(want);
            if(rc != 0)
            {
                ark_shell_printf("\r\nmini limit: %u rejected, rc=%d (need a power of two in %u..%u B, 0 = default)\r\n",
                                 (unsigned)want, (int)rc,
                                 (unsigned)SLOT_RAM_MIN_BLOCK,
                                 (unsigned)SLOT_RAM_MAX_BLOCK);
                return 1;
            }
            ark_shell_printf("\r\nmini size limit set to %u B\r\n",
                             (unsigned)arkrtos_mini_max_bytes());
            return 0;
        }
    }

    /* mini / mini stat / mini list：列出所有在跑的小程序（编号用于 mini stop <n>） */
    if((argc == 1) || ((argc == 2) &&
       ((strcmp(argv[1], "stat") == 0) || (strcmp(argv[1], "list") == 0))))
    {
        n = arkrtos_mini_running();

        if(n == 0u)
        {
            ark_shell_printf("\r\nmini: none running (usage: mini run <path>)\r\n");
            return 0;
        }

        ark_shell_printf("\r\nmini: %u running of %u allowed\r\n",
                         (unsigned)n, (unsigned)arkrtos_mini_max_count());
        for(n = 0u; arkrtos_mini_info_at(n, &mi) == 0; n++)
        {
            ark_shell_printf("  [%u] task %u entry 0x%08X\r\n",
                             (unsigned)n, (unsigned)mi.task_id, (unsigned)mi.entry);
            ark_shell_printf("      code %u B in %u B block @ 0x%08X\r\n",
                             (unsigned)mi.code_size, (unsigned)mi.code_block,
                             (unsigned)mi.code_base);
            ark_shell_printf("      RW/ZI+stack %u B block @ 0x%08X, crc 0x%08X\r\n",
                             (unsigned)mi.ram_size, (unsigned)mi.ram_base,
                             (unsigned)mi.crc);
        }
        {
            uint32 used = 0u;
            uint32 maxe = 0u;

            (void)arkrtos_ptable_mini_used(&used, &maxe);
            ark_shell_printf("  block entries: %u of %u in use\r\n",
                             (unsigned)used, (unsigned)maxe);
        }
        ark_shell_printf("  size limit %u B\r\n", (unsigned)arkrtos_mini_max_bytes());
        return 0;
    }

    /* mini autostart / list / add <path> / del <path> / now
     * 清单是卷内的一个文本文件（一行一个路径，见 arkrtos_mini.h）。这里只做
     * 增删查与「立刻跑一遍」；启动判据全部走 arkrtos_mini_run，与手工 run
     * 同一条路——自启不是第二条装载路径。 */
    if((argc >= 2) && (strcmp(argv[1], "autostart") == 0))
    {
        mini_auto_ctx_t ctx;
        int32           arc;

        ctx.index    = 0u;
        ctx.missing  = 0u;
        ctx.disabled = 0u;

        if((argc == 2) || ((argc == 3) && (strcmp(argv[2], "list") == 0)))
        {
            arc = arkrtos_mini_autostart_foreach(mini_auto_show, &ctx);
            if(arc != 0)
            {
                ark_shell_printf("\r\nmini autostart: no list (%s)"

                                 " -- 'mini autostart add <path>' starts one at boot\r\n",
                                 ARKRTOS_MINI_AUTOSTART_FILE);
                return 1;
            }
            if(ctx.index == 0u)
            {
                ark_shell_printf("\r\nmini autostart: empty list, nothing starts at boot\r\n");
                return 0;
            }
            ark_shell_printf("  %u listed, %u missing in the volume, %u disabled\r\n",
                             (unsigned)ctx.index, (unsigned)ctx.missing,
                             (unsigned)ctx.disabled);
            return 0;
        }

        if((argc == 4) && ((strcmp(argv[2], "add") == 0) ||
                           (strcmp(argv[2], "del") == 0)))
        {
            uint32 on = (strcmp(argv[2], "add") == 0) ? 1u : 0u;

            arc = arkrtos_mini_autostart_set(argv[3], on);
            if(arc == ARKRTOS_LOADER_ERR_PARAM)
            {
                /* 不替用户补 '/'：补上去会让清单里的路径与手工 `mini run` 的
                 * 那条不是同一个身份串（崩溃记账按路径哈希归属）。 */
                ark_shell_printf("\r\nmini autostart: '%s' is not a usable path"
                                 " (write it as 'mini run' would, starting with '/')\r\n",
                                 argv[3]);
                return 1;
            }
            if(arc == ARKRTOS_LOADER_ERR_NOSPACE)
            {
                ark_shell_printf("\r\nmini autostart: list full (%u B max in %s)\r\n",
                                 (unsigned)ARKRTOS_MINI_AUTOSTART_MAX,
                                 ARKRTOS_MINI_AUTOSTART_FILE);
                return 1;
            }
            if(arc == ARKRTOS_MINI_ERR_FS)
            {
                int32 e = arkrtos_fs_last_error();

                ark_shell_printf("\r\nmini autostart: file system: %d (%s)\r\n",
                                 (int)e, arkrtos_fs_error_name(e));
                return 1;
            }
            ark_shell_printf("\r\nmini autostart: %s %s\r\n", argv[3],
                             (on != 0u) ? "will start at boot"
                                        : "will not start at boot");
            return 0;
        }

        if((argc == 3) && (strcmp(argv[2], "now") == 0))
        {
            arc = arkrtos_mini_autostart_now();
            if(arc < 0)
            {
                ark_shell_printf("\r\nmini autostart now: no list (%s)\r\n",
                                 ARKRTOS_MINI_AUTOSTART_FILE);
                return 1;
            }
            ark_shell_printf("\r\nmini autostart now: %d started\r\n", (int)arc);
            return 0;
        }

        ark_shell_printf("\r\nusage: mini autostart [list | add <path> |"
                         " del <path> | now]\r\n");
        return 1;
    }

    ark_shell_printf("\r\nusage: mini [stat|list] | mini run <path> | mini stop [n] |"
                     " mini count [n] | mini limit [bytes] | mini forget <path> |"
#if (ARKRTOS_USE_TASK_SWAP == 1)
                     " mini swap <n> <path> |"
#endif
                     " mini autostart [list | add <path> | del <path> | now]\r\n");
    return 1;
}
#endif /* ARKRTOS_USE_MINIAPP */

/* ============================================================
 * pwd：打印当前工作目录
 * ============================================================ */
int cmd_pwd(int argc, char *argv[])
{
    char cwd[ARK_SHELL_PATH_SIZE];

    (void)argc;
    (void)argv;

    ark_shell_get_cwd(&g_shell, cwd, sizeof(cwd));
    ark_shell_printf("%s\r\n", cwd);
    return 0;
}

#if ARKRTOS_USE_VFS
/* ============================================================
 * cd：切换工作目录
 *
 * 解析相对路径 / ~ / .. 之后经 VFS 校验目标确实是目录才改，
 * 否则保持原目录不变并给出 Linux 习惯的错误文案。
 * ============================================================ */
int cmd_cd(int argc, char *argv[])
{
    char   path[ARK_SHELL_PATH_SIZE];
    char   vpath[ARKRTOS_VFS_PATH_MAX];
    uint32 mode = 0u;
    uint32 size = 0u;

    if(argc > 2)
    {
        sh_out("cd: too many arguments\r\n");
        return 1;
    }

    ark_shell_resolve(&g_shell, (argc >= 2) ? argv[1] : "", path, sizeof(path));
    (void)strncpy(vpath, path, sizeof(vpath) - 1u);
    vpath[sizeof(vpath) - 1u] = '\0';

    if(arkrtos_vfs_app_stat(vpath, &size, &mode) != 0)
    {
        ark_shell_printf("cd: %s: No such file or directory\r\n", path);
        return 1;
    }

    if((mode & ARKRTOS_VFS_S_IFMT) != ARKRTOS_VFS_S_IFDIR)
    {
        ark_shell_printf("cd: %s: Not a directory\r\n", path);
        return 1;
    }

    ark_shell_set_cwd(&g_shell, path);
    return 0;
}
#endif /* ARKRTOS_USE_VFS */

#if ARKRTOS_USE_VFS
/* 只在真的有一卷挂载时才去问用量；关掉 FS 时这段整个编掉。 */
static int df_volume_stat(uint32 *total, uint32 *used)
{
#if ARKRTOS_USE_FS
    if((arkrtos_fs_mounted() != 0u) && (arkrtos_fs_stat(total, used) == 0))
    {
        return 1;
    }
#else
    (void)total;
    (void)used;
#endif
    return 0;
}

/* ============================================================
 * df：挂载点与卷用量
 * ============================================================ */
int cmd_df(int argc, char *argv[])
{
    int32  n = arkrtos_vfs_mount_count();
    int32  i;
    uint32 total = 0u;
    uint32 used = 0u;
    int    have_fs;

    (void)argc;
    (void)argv;

    if(n <= 0)
    {
        sh_out("\r\nno mount points\r\n");
        return 0;
    }

    have_fs = df_volume_stat(&total, &used);

    sh_out("\r\nFilesystem  Size      Used      Free      Mounted on\r\n");
    for(i = 0; i < n; i++)
    {
        arkrtos_vfs_mount_info_t mi;

        if(arkrtos_vfs_mount_info((uint32)i, &mi) != 0)
        {
            continue;
        }

        if((have_fs != 0) && (mi.fs != 0) && (strcmp(mi.fs, "littlefs") == 0))
        {
            ark_shell_printf("%-11s %-9u %-9u %-9u %s\r\n", mi.fs,
                             (unsigned)total, (unsigned)used,
                             (unsigned)((total >= used) ? (total - used) : 0u),
                             mi.target);
        }
        else
        {
            /* No usage figure is known for this mount; "-" is honest. */
            ark_shell_printf("%-11s %-9s %-9s %-9s %s\r\n",
                             (mi.fs != 0) ? mi.fs : "?", "-", "-", "-",
                             mi.target);
        }
    }
    return 0;
}
#endif /* ARKRTOS_USE_VFS */

#endif /* SHELL_ENABLE */

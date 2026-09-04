/* RAM test engine. All accesses go through P2 (uncached) addresses so
 * every read/write really hits the SDRAM chips. State lives on the
 * OC-RAM stack / in registers only — the RAM under test is never used
 * to hold test code or data.
 *
 * Per user spec, a full test is three phases, reported as 1/3, 2/3 and 3/3:
 *   0x55555555, 0xAAAAAAAA, then a pseudo-random stream (different seed
 *   every pass) whose CRC32 is kept in a CPU register on the write side
 *   and compared with the CRC32 recomputed on the read side. */
#include "ramtest.h"
#include "progress.h"
#include "reloc.h"
#include "scif.h"

void ram_result_clear(ram_result *r)
{
    r->errors = 0;
    r->badbits = 0;
    r->badbits_e = 0;
    r->badbits_o = 0;
    r->nfails = 0;
    r->crc_w = 0;
    r->crc_r = 0;
}

static void note_fail(ram_result *r, u32 addr, u32 exp, u32 got)
{
    r->errors++;
    r->badbits |= exp ^ got;
    if (addr & 4)
        r->badbits_o |= exp ^ got;
    else
        r->badbits_e |= exp ^ got;
    if (r->nfails < RAM_MAX_FAILS) {
        r->fail_addr[r->nfails] = addr;
        r->fail_exp [r->nfails] = exp;
        r->fail_got [r->nfails] = got;
        r->nfails++;
    }
}

u32 ram_test_databus(u32 addr)
{
    volatile u32 *p = (volatile u32 *)addr;
    u32 bad = 0;
    for (u32 bit = 1; bit != 0; bit <<= 1) {
        *p = bit;                       /* walking one  */
        bad |= *p ^ bit;
        *p = ~bit;                      /* walking zero */
        bad |= *p ^ ~bit;
    }
    return bad;
}

u32 ram_test_addrbus(u32 base, u32 size)
{
    volatile u32 *b = (volatile u32 *)base;
    const u32 pat = 0xAAAAAAAA, anti = 0x55555555;
    u32 nwords = size >> 2;
    u32 bad = 0;

    for (u32 off = 1; off < nwords; off <<= 1)
        b[off] = pat;
    b[0] = anti;                        /* stuck-high check */
    for (u32 off = 1; off < nwords; off <<= 1)
        if (b[off] != pat)
            bad |= off << 2;
    b[0] = pat;
    for (u32 test = 1; test < nwords; test <<= 1) {   /* stuck-low / shorts */
        b[test] = anti;
        if (b[0] != pat)
            bad |= test << 2;
        for (u32 off = 1; off < nwords; off <<= 1)
            if (off != test && b[off] != pat)
                bad |= test << 2;
        b[test] = pat;
    }
    return bad;
}

/* Slow, fully diagnostic pattern pass: reports every bad word with its
 * address. Only reached when the fast path has already proven the region is
 * faulty, so its cost is paid on broken boards and nowhere else. */
static void pattern_locate(u32 base, u32 n, u32 pattern, ram_result *r)
{
    volatile u32 *p = (volatile u32 *)base;
    for (u32 i = 0; i < n; i++) {
        u32 got = p[i];
        if (got != pattern)
            note_fail(r, base + (i << 2), pattern, got);
    }
}

/* Uncached execution from the boot EPROM makes instruction count very nearly
 * proportional to elapsed time, so these two loops are hand-written assembly
 * (see ramtest_fast.S): 7 -> 1.19 instructions per word to fill, 11 -> 3.25
 * to verify. The verify is branchless and only reports whether the region is
 * good; when it is not, the slow loop above locates the individual words. */
void ram_test_pattern(u32 base, u32 len, u32 pattern, ram_result *r)
{
    u32 n = len >> 2;
    u32 done = 0;

    /* still stepped in 1024-word chunks so the progress bar keeps moving */
    while (done < n) {
        u32 chunk = n - done;
        if (chunk > 1024)
            chunk = 1024;
        chunk &= ~15u;                  /* fill works in 16-word blocks */
        if (!chunk)
            break;
        progress_tick(done);
        p_ram_fill_fast((u32 *)(base + (done << 2)), chunk >> 4, pattern);
        done += chunk;
    }
    for (u32 i = done; i < n; i++)      /* tail below one block */
        ((volatile u32 *)base)[i] = pattern;

    done = 0;
    u32 diff = 0;
    while (done < n) {
        u32 chunk = n - done;
        if (chunk > 1024)
            chunk = 1024;
        chunk &= ~7u;                   /* verify works in 8-word blocks */
        if (!chunk)
            break;
        progress_tick(n + done);
        diff |= p_ram_verify_fast((u32 *)(base + (done << 2)), chunk >> 3,
                                pattern);
        done += chunk;
    }
    for (u32 i = done; i < n; i++)
        diff |= ((volatile u32 *)base)[i] ^ pattern;

    if (diff)
        pattern_locate(base, n, pattern, r);
}

/* CRC32 (IEEE 0xEDB88320), 4 bits at a time; table lives in ROM. */
/* CRC-32 (reflected, poly 0xEDB88320), one table step per BYTE.
 *
 * The nibble table this replaces needed 8 steps per 32-bit word, and the
 * compiler turned each into 9 instructions: 73 instructions per word, which
 * measured out as roughly three quarters of the entire pseudo-random pass.
 * Running uncached from the boot EPROM, every one of those is a bus cycle.
 * Four byte-steps produce the identical CRC value for a quarter of the work;
 * the table costs 1 KB of ROM, which this image has to spare. */
const u32 crc32_tab8[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA,
    0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
    0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
    0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
    0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE,
    0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC,
    0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
    0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
    0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
    0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940,
    0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116,
    0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
    0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
    0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
    0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A,
    0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818,
    0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
    0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
    0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
    0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C,
    0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2,
    0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
    0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
    0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
    0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086,
    0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4,
    0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
    0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
    0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
    0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8,
    0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE,
    0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
    0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
    0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
    0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252,
    0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60,
    0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
    0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
    0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
    0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04,
    0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A,
    0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
    0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
    0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
    0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E,
    0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C,
    0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
    0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
    0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
    0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0,
    0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6,
    0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
    0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
    0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
};


static inline u32 xorshift32(u32 x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/* Slow, fully diagnostic pseudo-random pass: regenerates the stream and
 * records every mismatching word with its address. Only reached once the
 * fast loop has already proved the region faulty. */
static void prng_locate(u32 base, u32 n, u32 seed, ram_result *r)
{
    volatile u32 *p = (volatile u32 *)base;
    u32 x = seed ? seed : 1;
    for (u32 i = 0; i < n; i++) {
        x = xorshift32(x);
        u32 got = p[i];
        if (got != x)
            note_fail(r, base + (i << 2), x, got);
    }
}

void ram_test_prng(u32 base, u32 len, u32 seed, ram_result *r)
{
    u32 n = len >> 2;
    prng_ctx c;
    c.x = seed ? seed : 1;
    c.crc_w = 0xFFFFFFFF;
    c.crc_r = 0xFFFFFFFF;
    c.diff = 0;

    /* Both loops are hand-written (ramtest_fast.S): 19 -> 15 instructions
     * per word to generate and store, 85 -> 59 to read back. They are called
     * in 1024-word chunks purely so the progress bar keeps moving; the
     * chunking costs nothing measurable next to the loop bodies. */
    for (u32 done = 0; done < n; ) {
        u32 chunk = n - done;
        if (chunk > 1024)
            chunk = 1024;
        progress_tick(done);
        p_ram_prng_fill_fast((u32 *)(base + (done << 2)), chunk, &c);
        done += chunk;
    }

    c.x = seed ? seed : 1;              /* replay the same stream */
    for (u32 done = 0; done < n; ) {
        u32 chunk = n - done;
        if (chunk > 1024)
            chunk = 1024;
        progress_tick(n + done);
        p_ram_prng_verify_fast((const u32 *)(base + (done << 2)), chunk, &c);
        done += chunk;
    }

    r->crc_w = ~c.crc_w;
    r->crc_r = ~c.crc_r;
    if (c.diff)
        prng_locate(base, n, seed, r);
    if (c.crc_w != c.crc_r && r->errors == 0) {
        /* CRC caught something the compare did not (should not happen, but
         * the register-held CRC is the belt-and-braces the spec asks for) */
        note_fail(r, base, c.crc_w, c.crc_r);
    }
}

/* RISCiX squeeze decoding.
 *
 * Based on the vm_squeeze.o decoder and exec.h in the surviving RISCiX
 * kernel kit. See docs/squeezed-binaries.md for the format and references.
 * Pages stay at their original file offsets; squeezing saves disk blocks
 * rather than changing the file length.
 */

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "zload.h"
#include "squeeze.h"

#define SQ_PAGE_SIZE     32768
#define SQ_TABLE_ITEMS   1792    /* Seven banks of 256 entries per dictionary */

static uint32_t  read_le(const uint8_t *p, unsigned int bytes)
{
        uint32_t value = 0;
        for (unsigned int i = 0; i < bytes; i++)
                value |= (uint32_t)p[i] << (8 * i);
        return value;
}

static int      read_exact(int fd, void *buffer, size_t len, off_t offset)
{
        uint8_t *p = buffer;
        while (len) {
                ssize_t r = pread(fd, p, len, offset);
                if (r < 0) {
                        if (errno == EINTR) continue;
                        return -errno;
                }
                if (r == 0) return -ENOEXEC;
                p += r;
                offset += r;
                len -= r;
        }
        return 0;
}

/* Dictionaries encode deltas from the previous value, initially -1.
 * Bytes 1..9 encode runs of consecutive values; zero escapes a full delta.
 */
static int      decode_table(const uint8_t *src, size_t len, unsigned int width,
                              uint32_t *table, unsigned int count, uint32_t last)
{
        size_t pos = 0;
        unsigned int out = 0;
        uint32_t previous = UINT32_MAX;
        while (pos < len) {
                unsigned int code = src[pos++];
                if (code >= 1 && code <= 9) {
                        if (code > count - out) return -ENOEXEC;
                        while (code--) table[out++] = ++previous;
                        continue;
                }
                unsigned int bytes = code == 0 ? width : code <= 0x5b ? 0 :
                                     code <= 0xad ? 1 : 2;
                if (bytes > len - pos || out == count) return -ENOEXEC;
                uint32_t delta = read_le(src + pos, bytes);
                pos += bytes;
                if (code >= 0xae) delta |= (code - 0xae) << 16;
                else if (code >= 0x5c) delta |= (code - 0x5c) << 8;
                else if (code) delta = code - 10;
                previous += delta;
                table[out++] = previous;
        }
        return out == count && (!count || previous == last) ? 0 : -ENOEXEC;
}

/* A page is read backwards, two word codes per byte, high nibble first.
 * Codes select zero, a literal, a 24-bit dictionary entry plus a low byte,
 * or a complete 32-bit dictionary entry.
 */
static int      decode_word(const uint8_t *src, size_t *pos, unsigned int code,
                             const uint32_t *table, unsigned int count, uint32_t *value)
{
        unsigned int bytes = code == 0 ? 0 : code == 1 ? 4 : code <= 8 ? 2 : 1;
        if (*pos < 8 + bytes) return -ENOEXEC;
        *value = 0;
        if (code == 1) {
                for (unsigned int i = 0; i < 4; i++)
                        *value |= (uint32_t)src[--*pos] << (8 * i);
        } else if (code) {
                unsigned int index = ((code <= 8 ? code + 5 : code - 9) << 8) |
                                     src[--*pos];
                if (index >= count) return -ENOEXEC;
                *value = table[index];
                if (code <= 8) *value = (*value << 8) | src[--*pos];
        }
        return 0;
}

static int      decode_page(const uint8_t *src, uint8_t *page,
                             const uint32_t *table, unsigned int count)
{
        size_t pos = read_le(src, 2);
        memset(page, 0, SQ_PAGE_SIZE);
        if (pos == 0) return 0;
        unsigned int words = read_le(src + 2, 2);
        if (pos < 8 || pos > SQ_PAGE_SIZE || words > SQ_PAGE_SIZE / 4 || (words & 1))
                return -ENOEXEC;
        uint32_t checksum = 0;
        while (words) {
                if (pos <= 8) return -ENOEXEC;
                unsigned int codes = src[--pos];
                for (unsigned int n = 0; n < 2; n++) {
                        uint32_t value;
                        int r = decode_word(src, &pos, n ? codes & 15 : codes >> 4,
                                            table, count, &value);
                        if (r < 0) return r;
                        checksum += value;
                        --words;
                        for (unsigned int i = 0; i < 4; i++)
                                page[words * 4 + i] = value >> (8 * i);
                }
        }
        return pos == 8 && checksum == read_le(src + 4, 4) ? 0 : -ENOEXEC;
}

int     load_squeezed_segments(int fd, const struct exec_hdr *hdr,
                               uint8_t *text, uint8_t *data)
{
        uint8_t encoded[SQ_PAGE_SIZE], page[SQ_PAGE_SIZE];
        uint32_t table[2 * SQ_TABLE_ITEMS];
        unsigned int n4 = hdr->a_sq4items, n3 = hdr->a_sq3items;
        unsigned int size4 = hdr->a_sq4size, size3 = hdr->a_sq3size;
        if (n4 > SQ_TABLE_ITEMS || n3 > SQ_TABLE_ITEMS || (n3 && n4 != SQ_TABLE_ITEMS) ||
            size4 > SQ_PAGE_SIZE - sizeof(*hdr) ||
            size3 > SQ_PAGE_SIZE - sizeof(*hdr) - size4)
                return -ENOEXEC;

        uint64_t total = (uint64_t)hdr->a_exec.a_text + hdr->a_exec.a_data;
        struct stat sb;
        if (fstat(fd, &sb) < 0) return -errno;
        if (sb.st_size < 0 || (uint64_t)sb.st_size < RX_ZM_TEXT_OFFS + total)
                return -ENOEXEC;
        int r = read_exact(fd, encoded, size4 + size3, sizeof(*hdr));
        if (r < 0) return r;
        r = decode_table(encoded, size4, 4, table, n4, hdr->a_sq4last);
        if (r < 0) return r;
        r = decode_table(encoded + size4, size3, 3, table + n4, n3, hdr->a_sq3last);
        if (r < 0) return r;

        for (uint64_t offset = 0; offset < total; offset += SQ_PAGE_SIZE) {
                r = read_exact(fd, encoded, 8, RX_ZM_TEXT_OFFS + offset);
                if (r < 0) return r;
                unsigned int size = read_le(encoded, 2);
                if (size && (size < 8 || size > SQ_PAGE_SIZE)) return -ENOEXEC;
                if (size) {
                        r = read_exact(fd, encoded + 8, size - 8,
                                       RX_ZM_TEXT_OFFS + offset + 8);
                        if (r < 0) return r;
                }
                r = decode_page(encoded, page, table, n4 + n3);
                if (r < 0) return r;

                size_t len = total - offset < SQ_PAGE_SIZE ? total - offset : SQ_PAGE_SIZE;
                size_t text_len = offset < hdr->a_exec.a_text ? hdr->a_exec.a_text - offset : 0;
                if (text_len > len) text_len = len;
                if (text_len) memcpy(text + offset, page, text_len);
                if (len > text_len)
                        memcpy(data + (offset + text_len - hdr->a_exec.a_text),
                               page + text_len, len - text_len);
        }
        return 0;
}

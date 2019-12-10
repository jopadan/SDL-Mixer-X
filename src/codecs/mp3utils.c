/*
  SDL_mixer:  An audio mixer library based on the SDL library
  Copyright (C) 1997-2019 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "mp3utils.h"

#include "SDL_log.h"

#if defined(MUSIC_MP3_MAD) || defined(MUSIC_MP3_MPG123)

/*********************** SDL_RW WITH BOOKKEEPING ************************/

size_t MP3_RWread(struct mp3file_t *fil, void *ptr, size_t size, size_t maxnum) {
    size_t remaining = (size_t)(fil->length - fil->pos);
    size_t ret;
    maxnum *= size;
    if (maxnum > remaining) maxnum = remaining;
    ret = SDL_RWread(fil->src, ptr, 1, maxnum);
    fil->pos += (Sint64)ret;
    return ret;
}

Sint64 MP3_RWseek(struct mp3file_t *fil, Sint64 offset, int whence) {
    Sint64 ret;
    switch (whence) { /* assumes a legal whence value */
    case RW_SEEK_CUR:
        offset += fil->pos;
        break;
    case RW_SEEK_END:
        offset = fil->length + offset;
        break;
    }
    if (offset < 0) return -1;
    if (offset > fil->length)
        offset = fil->length;
    ret = SDL_RWseek(fil->src, fil->start + offset, RW_SEEK_SET);
    if (ret < 0) return ret;
    fil->pos = offset;
    return (fil->pos - fil->start);
}

#endif

#define TAGS_INPUT_BUFFER_SIZE (5 * 8192)

/********************************************************
 *                  ID3v1 and ID3v1EXT                  *
 ********************************************************/

#define ID3v1_TAG_SIZE          128
#define ID3v1_SIZE_OF_FIELD     30

#define ID3v1_FIELD_TITLE       3
#define ID3v1_FIELD_ARTIST      33
#define ID3v1_FIELD_ALBUM       63
#define ID3v1_FIELD_COPYRIGHT   97

static SDL_INLINE SDL_bool is_id3v1(const Uint8 *data, size_t length)
{
    /* http://id3.org/ID3v1 :  3 bytes "TAG" identifier and 125 bytes tag data */
    if (length < ID3v1_TAG_SIZE || SDL_memcmp(data,"TAG", 3) != 0) {
        return SDL_FALSE;
    }
    return SDL_TRUE;
}

/* Parse ASCII string and clean it up from non-ASCII characters (replace them with a question mark '?') */
static SDL_INLINE char *parse_id3v1_ansi_string(const Uint8 *buffer, size_t src_len)
{
    char *src_buffer = (char*)SDL_malloc(src_len + 1);
    char *ret;
    SDL_memset(src_buffer, 0, src_len + 1);
    SDL_memcpy(src_buffer, buffer, src_len);
    ret = SDL_iconv_string("UTF-8", "ISO-8859-1", src_buffer, src_len + 1);
    SDL_free(src_buffer);
    return ret;
}

static SDL_INLINE void id3v1_set_tag(Mix_MusicMetaTags *out_tags, Mix_MusicMetaTag tag, const Uint8 *buffer, size_t len)
{
    char *src_buf = parse_id3v1_ansi_string(buffer, len);
    if (src_buf) {
        meta_tags_set(out_tags, tag, src_buf);
        SDL_free(src_buf);
    }
}

/* Parse content of ID3v1 tag */
static SDL_INLINE void parse_id3v1(Mix_MusicMetaTags *out_tags, const Uint8 *buffer)
{
    id3v1_set_tag(out_tags, MIX_META_TITLE,     buffer + ID3v1_FIELD_TITLE,     ID3v1_SIZE_OF_FIELD);
    id3v1_set_tag(out_tags, MIX_META_ARTIST,    buffer + ID3v1_FIELD_ARTIST,    ID3v1_SIZE_OF_FIELD);
    id3v1_set_tag(out_tags, MIX_META_ALBUM,     buffer + ID3v1_FIELD_ALBUM,     ID3v1_SIZE_OF_FIELD);
    id3v1_set_tag(out_tags, MIX_META_COPYRIGHT, buffer + ID3v1_FIELD_COPYRIGHT, ID3v1_SIZE_OF_FIELD);
}


/********************************************************
 *                       ID3v2                          *
 ********************************************************/

#define ID3v2_BUFFER_SIZE               1024

#define ID3v2_HEADER_SIZE               10

#define ID3v2_FIELD_VERSION_MAJOR       3
#define ID3v2_FIELD_VERSION_MINOR       4
#define ID3v2_FIELD_HEAD_FLAGS          5
#define ID3v2_FIELD_TAG_LENGTH          6
#define ID3v2_FIELD_EXTRA_HEADER_LENGTH 10

#define ID3v2_FLAG_HAS_FOOTER           0x10
#define ID3v2_FLAG_HAS_EXTRA_HEAD       0x40

#define ID3v2_3_FRAME_HEADER_SIZE       10
#define ID3v2_2_FRAME_HEADER_SIZE       6
#define ID3v2_FIELD_FRAME_SIZE          4
#define ID3v2_FIELD_FRAME_SIZEv2        3
#define ID3v2_FIELD_FLAGS               8

static SDL_INLINE SDL_bool is_id3v2(const Uint8 *data, size_t length)
{
    /* ID3v2 header is 10 bytes:  http://id3.org/id3v2.4.0-structure */
    /* bytes 0-2: "ID3" identifier */
    if (length < ID3v2_HEADER_SIZE || SDL_memcmp(data, "ID3",3) != 0) {
        return SDL_FALSE;
    }
    /* bytes 3-4: version num (major,revision), each byte always less than 0xff. */
    if (data[3] == 0xff || data[4] == 0xff) {
        return SDL_FALSE;
    }
    /* bytes 6-9 are the ID3v2 tag size: a 32 bit 'synchsafe' integer, i.e. the
     * highest bit 7 in each byte zeroed.  i.e.: 7 bit information in each byte ->
     * effectively a 28 bit value.  */
    if (data[6] >= 0x80 || data[7] >= 0x80 || data[8] >= 0x80 || data[9] >= 0x80) {
        return SDL_FALSE;
    }
    return SDL_TRUE;
}

static SDL_INLINE long id3v2_synchsafe_decode(const Uint8 *data)
{
    /* size is a 'synchsafe' integer (see above) */
    return (long)((data[0] << 21) + (data[1] << 14) + (data[2] << 7) + data[3]);
}

static SDL_INLINE long id3v2_byteint_decode(const Uint8 *data, size_t size)
{
    Uint32 result = 0;
    size_t i;

    for (i = 0; i < size; i++) {
        result <<= 8;
        result = result | (Uint8)data[i];
    }

    return (long)result;
}

static SDL_INLINE long get_id3v2_len(const Uint8 *data, long length)
{
    /* size is a 'synchsafe' integer (see above) */
    long size = id3v2_synchsafe_decode(data + 6);
    size += ID3v2_HEADER_SIZE; /* header size */
    /* ID3v2 header[5] is flags (bits 4-7 only, 0-3 are zero).
     * bit 4 set: footer is present (a copy of the header but
     * with "3DI" as ident.)  */
    if (data[5] & 0x10) {
        size += ID3v2_HEADER_SIZE; /* footer size */
    }
    /* optional padding (always zeroes) */
    while (size < length && data[size] == 0) {
        ++size;
    }
    return size;
}

/* Decode a string in the frame according to an encoding marker */
static char *id3v2_decode_string(const Uint8 *string, size_t size)
{
    char *str_buffer = NULL;
    char *src_buffer = NULL;
    size_t copy_size = size;

    if (size == 0) {
        SDL_Log("id3v2_decode_string: Bad string size: a string should have at least 1 byte");
        return NULL;
    }

    if (size < 2) {
        return NULL;
    }

    if (string[0] == '\x01') { /* UTF-16 string with a BOM */
        if (size <= 5) {
            if (size < 5) {
                SDL_Log("id3v2_decode_string: Bad BOM-UTF16 string size: %d < 5", (int)size);
            }
            return NULL;
        }

        copy_size = size - 3 + 2; /* exclude 3 bytes of encoding hint, append 2 bytes for a NULL termination */
        src_buffer = (char*)SDL_malloc(copy_size);
        SDL_memset(src_buffer, 0, copy_size);
        SDL_memcpy(src_buffer, (string + 3), copy_size - 2);

        if (SDL_memcmp(string, "\x01\xFE\xFF", 3) == 0) { /* UTF-16BE*/
            str_buffer = SDL_iconv_string("UTF-8", "UCS-2BE", src_buffer, copy_size);
        } else if (SDL_memcmp(string, "\x01\xFF\xFE", 3) == 0) {  /* UTF-16LE*/
            str_buffer = SDL_iconv_string("UTF-8", "UCS-2LE", src_buffer, copy_size);
        }
        SDL_free(src_buffer);

    } else if (string[0] == '\x02') { /* UTF-16BEstring without a BOM */
        if (size <= 3) {
            if (size < 3) {
                SDL_Log("id3v2_decode_string: Bad UTF16BE string size: %d < 3", (int)size);
            }
            return NULL; /* Blank string*/
        }

        copy_size = size - 1 + 2; /* exclude 1 byte of encoding hint, append 2 bytes for a NULL termination */
        src_buffer = (char*)SDL_malloc(copy_size);
        SDL_memset(src_buffer, 0, copy_size);
        SDL_memcpy(src_buffer, (string + 1), copy_size - 2);

        str_buffer = SDL_iconv_string("UTF-8", "UCS-2BE", src_buffer, copy_size);
        SDL_free(src_buffer);

    } else if (string[0] == '\x03') { /* UTF-8 string */
        if (size <= 2) {
            return NULL; /* Blank string*/
        }
        str_buffer = (char*)SDL_malloc(size);
        SDL_strlcpy(str_buffer, (const char*)(string + 1), size);

    } else if (string[0] == '\x00') { /* Latin-1 string */
        if (size <= 2) {
            return NULL; /* Blank string*/
        }
        str_buffer = parse_id3v1_ansi_string((string + 1), size - 1);
    }

    return str_buffer;
}

/* Write a tag string into internal meta-tags storage */
static SDL_INLINE void write_id3v2_string(Mix_MusicMetaTags *out_tags, Mix_MusicMetaTag tag, const Uint8 *string, size_t size)
{
    char *str_buffer = id3v2_decode_string(string, size);

    if (str_buffer) {
        meta_tags_set(out_tags, tag, str_buffer);
        SDL_free(str_buffer);
    }

}

/* Identify a meta-key and decode the string (Note: input buffer should have at least 4 characters!) */
static SDL_INLINE void handle_id3v2_string(Mix_MusicMetaTags *out_tags, const char *key, const Uint8 *string, size_t size)
{
    if (SDL_memcmp(key, "TIT2", 4) == 0) {
        write_id3v2_string(out_tags, MIX_META_TITLE, string, size);
    } else if (SDL_memcmp(key, "TPE1", 4) == 0) {
        write_id3v2_string(out_tags, MIX_META_ARTIST, string, size);
    } else if (SDL_memcmp(key, "TALB", 4) == 0) {
        write_id3v2_string(out_tags, MIX_META_ALBUM, string, size);
    } else if (SDL_memcmp(key, "TCOP", 4) == 0) {
        write_id3v2_string(out_tags, MIX_META_COPYRIGHT, string, size);
    }
/* TODO: Extract "Copyright message" from TXXX value: a KEY=VALUE string divided by a zero byte:*/
/*
    else if (SDL_memcmp(key, "TXXX", 4) == 0) {
        write_id3v2_string(out_tags, MIX_META_COPYRIGHT, string, size);
    }
*/
}

/* Identify a meta-key and decode the string (Note: input buffer should have at least 4 characters!) */
static SDL_INLINE void handle_id3v2x2_string(Mix_MusicMetaTags *out_tags, const char *key, const Uint8 *string, size_t size)
{
    if (SDL_memcmp(key, "TT2", 3) == 0) {
        write_id3v2_string(out_tags, MIX_META_TITLE, string, size);
    } else if (SDL_memcmp(key, "TP1", 3) == 0) {
        write_id3v2_string(out_tags, MIX_META_ARTIST, string, size);
    } else if (SDL_memcmp(key, "TAL", 3) == 0) {
        write_id3v2_string(out_tags, MIX_META_ALBUM, string, size);
    } else if (SDL_memcmp(key, "TCR", 3) == 0) {
        write_id3v2_string(out_tags, MIX_META_COPYRIGHT, string, size);
    }
}

/* Parse a frame in ID3v2.2 format */
static size_t id3v22_parse_frame(Mix_MusicMetaTags *out_tags, SDL_RWops *src, Uint8 *buffer)
{
    size_t size;
    char key[4];
    size_t read_size;
    Sint64 frame_begin = SDL_RWtell(src);

    read_size = SDL_RWread(src, buffer, 1, ID3v2_2_FRAME_HEADER_SIZE);

    if (read_size < ID3v2_2_FRAME_HEADER_SIZE) {
        SDL_Log("id3v2_parse_frame: Buffer size that left is too small %d < 6", (int)read_size);
        SDL_RWseek(src, frame_begin, RW_SEEK_SET);
        return 0; /* Buffer size that left is too small */
    }

    if (SDL_memcmp(buffer, "\0\0\0", 3) == 0) {
        SDL_RWseek(src, frame_begin, RW_SEEK_SET);
        return 0;
    }

    SDL_memcpy(key, buffer, 3); /* Tag title (key) */

    size = (size_t)id3v2_byteint_decode(buffer + ID3v2_FIELD_FRAME_SIZEv2, 3);

    if (size < ID3v2_BUFFER_SIZE) {
        read_size = SDL_RWread(src, buffer, 1, size);
    } else {
        read_size = SDL_RWread(src, buffer, 1, ID3v2_BUFFER_SIZE);
        SDL_RWseek(src, frame_begin + (Sint64)size, RW_SEEK_SET);
    }

    handle_id3v2x2_string(out_tags, key, buffer, read_size);

    return (size_t)(size + ID3v2_2_FRAME_HEADER_SIZE); /* data size + size of the header */
}

/* Parse a frame in ID3v2.3 and ID3v2.4 formats */
static size_t id3v2x_parse_frame(Mix_MusicMetaTags *out_tags, SDL_RWops *src, Uint8 *buffer, Uint8 version)
{
    size_t size;
    char key[4];
    Uint8 flags[2];
    size_t read_size;
    Sint64 frame_begin = SDL_RWtell(src);

    read_size = SDL_RWread(src, buffer, 1, ID3v2_3_FRAME_HEADER_SIZE);

    if (read_size < ID3v2_3_FRAME_HEADER_SIZE) {
        SDL_Log("id3v2_parse_frame: Buffer size that left is too small %d < 10", (int)read_size);
        SDL_RWseek(src, frame_begin, RW_SEEK_SET);
        return 0; /* Can't read frame header, possibly, a file size was reached */
    }

    if (SDL_memcmp(buffer, "\0\0\0\0", 4) == 0) {
        SDL_RWseek(src, frame_begin, RW_SEEK_SET);
        return 0;
    }

    SDL_memcpy(key, buffer, 4); /* Tag title (key) */

    if (version == 4) {
        size = (size_t)id3v2_synchsafe_decode(buffer + ID3v2_FIELD_FRAME_SIZE);
    } else {
        size = (size_t)id3v2_byteint_decode(buffer + ID3v2_FIELD_FRAME_SIZE, 4);
    }

    SDL_memcpy(flags, buffer + ID3v2_FIELD_FLAGS, 2);

    if (size < ID3v2_BUFFER_SIZE) {
        read_size = SDL_RWread(src, buffer, 1, size);
    } else {
        read_size = SDL_RWread(src, buffer, 1, ID3v2_BUFFER_SIZE);
        SDL_RWseek(src, frame_begin + (Sint64)size, RW_SEEK_SET);
    }

    handle_id3v2_string(out_tags, key, buffer, size);

    return (size_t)(size + ID3v2_3_FRAME_HEADER_SIZE); /* data size + size of the header */
}


/* Parse content of ID3v2 */
static SDL_bool parse_id3v2(Mix_MusicMetaTags *out_tags, SDL_RWops *src, Sint64 begin_pos)
{
    Uint8 version_major, flags;
    long total_length, tag_len, tag_extended_len = 0;
    Uint8 buffer[ID3v2_BUFFER_SIZE];
    size_t read_size;
    size_t frame_length;
    Sint64 file_size;

    total_length = 0;

    file_size = SDL_RWsize(src);
    SDL_RWseek(src, begin_pos, RW_SEEK_SET);
    read_size = SDL_RWread(src, buffer, 1, ID3v2_HEADER_SIZE); /* Retrieve the header */
    if (read_size < ID3v2_HEADER_SIZE) {
        SDL_Log("parse_id3v2: fail to read a header (%d < 10)", (int)read_size);
        SDL_RWseek(src, begin_pos, RW_SEEK_SET);
        return SDL_FALSE; /* Unsupported version of the tag */
    }

    total_length += ID3v2_HEADER_SIZE;

    version_major = buffer[ID3v2_FIELD_VERSION_MAJOR]; /* Major version */
    /* version_minor = buffer[ID3v2_VERSION_MINOR]; * Minor version, UNUSED */
    flags = buffer[ID3v2_FIELD_HEAD_FLAGS]; /* Flags */
    tag_len = id3v2_synchsafe_decode(buffer + ID3v2_FIELD_TAG_LENGTH); /* Length of a tag */

    if (version_major != 2 && version_major != 3 && version_major != 4) {
        SDL_Log("parse_id3v2: Unsupported version %d", version_major);
        SDL_RWseek(src, begin_pos, RW_SEEK_SET);
        return SDL_FALSE; /* Unsupported version of the tag */
    }

    if ((version_major > 2) && ((flags & ID3v2_FLAG_HAS_EXTRA_HEAD) == ID3v2_FLAG_HAS_EXTRA_HEAD)) {
        tag_extended_len = id3v2_synchsafe_decode(buffer + ID3v2_FIELD_EXTRA_HEADER_LENGTH); /* Length of an extended header */
    }

    if (tag_extended_len) {
        total_length += tag_extended_len + 4;
        SDL_RWseek(src, tag_extended_len + 4, RW_SEEK_CUR); /* Skip extended header and it's size value */
    }

    total_length += tag_len;

    if (flags & ID3v2_FLAG_HAS_FOOTER) {
        total_length += ID3v2_HEADER_SIZE; /* footer size */
    }

    if ((SDL_RWtell(src) + tag_len) > file_size) {
        SDL_Log("parse_id3v2: Tag size bigger than actual file size");
        SDL_RWseek(src, begin_pos, RW_SEEK_SET);
        return SDL_FALSE; /* Tag size is bigger than actual buffer data */
    }

    while ((SDL_RWtell(src) >= 0) && (SDL_RWtell(src) < (begin_pos + total_length))) {
        if (version_major == 2) {
            frame_length = id3v22_parse_frame(out_tags, src, buffer);
        } else {
            frame_length = id3v2x_parse_frame(out_tags, src, buffer, version_major);
        }
        if (!frame_length) {
            break;
        }
    }

    SDL_RWseek(src, begin_pos, RW_SEEK_SET);

    return SDL_TRUE;
}


/********************************************************
 *                   Lyrics3 skip                       *
 ********************************************************/

/* The maximum length of the lyrics is 5100 bytes. (Regard to a specification) */
#define LYRICS3v1_SEARCH_BUFFER     5120 /* 5100 + 20 of tag begin and end keywords */

#define LYRICS3v1_HEAD_SIZE         11
#define LYRICS3v1_TAIL_SIZE         9
#define LYRICS3v2_BUFFER_SIZE       12
#define LYRICS3v2_TAG_SIZE_VALUE    6

static SDL_INLINE SDL_bool is_lyrics3tag(const Uint8 *data, size_t length)
{
    /* http://id3.org/Lyrics3
     * http://id3.org/Lyrics3v2 */
    if (length < LYRICS3v1_TAIL_SIZE) return SDL_FALSE;
    if (SDL_memcmp(data,"LYRICSEND", 9) != 0 &&
        SDL_memcmp(data,"LYRICS200", 9) != 0) {return SDL_FALSE;
    }
    return SDL_TRUE;
}

static SDL_INLINE long get_lyrics3v1_len(Sint64 begin_pos, SDL_RWops *src)
{
    /* Lyrics3 v1.00 tag
     * http://id3.org/Lyrics3 */
    Sint64 pos_begin, pos_end;
    size_t read_size;
    char buffer[LYRICS3v1_SEARCH_BUFFER + 1];
    char *cur, *end = (buffer + LYRICS3v1_SEARCH_BUFFER) - 11;
    long len = 0;

    /* needs manual search:  http://id3.org/Lyrics3 */
    pos_end = SDL_RWtell(src);
    if (pos_end > LYRICS3v1_SEARCH_BUFFER) {
        SDL_RWseek(src, -LYRICS3v1_SEARCH_BUFFER, RW_SEEK_CUR);
    } else {
        SDL_RWseek(src, 0, RW_SEEK_SET);
    }
    pos_begin = SDL_RWtell(src);

    read_size = SDL_RWread(src, buffer, 1, (size_t)(pos_end - pos_begin));
    end = (buffer + read_size - LYRICS3v1_TAIL_SIZE);

    /* Find the lyrics begin tag... */
    /* strstr() won't work here. */
    for (cur = buffer; cur != end; cur++) {
        if (SDL_memcmp(cur, "LYRICSBEGIN", LYRICS3v1_HEAD_SIZE) == 0) {
            /* Calculate a full length of a tag */
            len = (long)(end - cur) + LYRICS3v1_TAIL_SIZE;
            break;
        }
    }

    if (cur == end) {
        SDL_RWseek(src, begin_pos, RW_SEEK_SET);
        return -1; /* Invalid tag */
    }

    SDL_RWseek(src, begin_pos, RW_SEEK_SET);
    return len;
}

static SDL_INLINE long get_lyrics3v2_len(Sint64 begin_pos, SDL_RWops *src)
{
    /* Lyrics3 v2.00 tag
     * http://id3.org/Lyrics3v2 */
    Sint64 pos_end;
    size_t read_size;
    char buffer[LYRICS3v2_BUFFER_SIZE + 1];
    long len = 0;

    pos_end = SDL_RWtell(src);
    if (pos_end > LYRICS3v2_TAG_SIZE_VALUE + LYRICS3v1_TAIL_SIZE) {
        SDL_RWseek(src, -(LYRICS3v2_TAG_SIZE_VALUE + LYRICS3v1_TAIL_SIZE), RW_SEEK_CUR);
    } else {
        SDL_RWseek(src, begin_pos, RW_SEEK_SET);
        return -1; /* Invalid tag */
    }

    read_size = SDL_RWread(src, buffer, 1, LYRICS3v2_TAG_SIZE_VALUE);
    if (read_size < LYRICS3v2_TAG_SIZE_VALUE) {
        SDL_RWseek(src, begin_pos, RW_SEEK_SET);
        return -1; /* Invalid tag */
    }
    buffer[read_size] = '\0';

    len = SDL_strtol(buffer, NULL, 10);
    if (len == 0) {
        SDL_RWseek(src, begin_pos, RW_SEEK_SET);
        return -1; /* Invalid tag */
    }

    len += LYRICS3v2_TAG_SIZE_VALUE + LYRICS3v1_TAIL_SIZE;

    if (pos_end > len) {
        SDL_RWseek(src, (pos_end - len), RW_SEEK_SET);
        read_size = SDL_RWread(src, buffer, 1, LYRICS3v1_HEAD_SIZE);
        if (read_size < LYRICS3v1_HEAD_SIZE) {
            SDL_RWseek(src, begin_pos, RW_SEEK_SET);
            return -1; /* Invalid tag */
        }

        if (SDL_memcmp(buffer, "LYRICSBEGIN", LYRICS3v1_HEAD_SIZE) != 0) {
            SDL_RWseek(src, begin_pos, RW_SEEK_SET);
            return -1; /* Invalid tag */
        }
    }

    SDL_RWseek(src, begin_pos, RW_SEEK_SET);
    return len;
}

static long get_lyrics3_len(Sint64 tag_end_at, Sint64 begin_pos, SDL_RWops *src)
{
    Sint64 file_size;
    size_t read_size;
    char buffer[LYRICS3v1_TAIL_SIZE + 1];

    file_size = SDL_RWsize(src);
    if (file_size < 0) {
        SDL_RWseek(src, begin_pos, RW_SEEK_SET);
        return -1; /* Invalid tag */
    }

    SDL_RWseek(src, -(tag_end_at + LYRICS3v1_TAIL_SIZE), RW_SEEK_END);
    read_size = SDL_RWread(src, buffer, 1, LYRICS3v1_TAIL_SIZE);
    if (read_size < LYRICS3v1_TAIL_SIZE) {
        return -1; /* Invalid tag */
    }

    /* Find and validate borders of the lyrics tag */
    if (SDL_memcmp(buffer, "LYRICSEND", LYRICS3v1_TAIL_SIZE) == 0) { /* v1 */
        return get_lyrics3v1_len(begin_pos, src);
    } else if (SDL_memcmp(buffer, "LYRICS200", LYRICS3v1_TAIL_SIZE) == 0) { /* v2 */
        return get_lyrics3v2_len(begin_pos, src);
    }

    SDL_RWseek(src, begin_pos, RW_SEEK_SET);
    return -1; /* Invalid tag */
}


/********************************************************
 *                  APE v1 and v2                       *
 ********************************************************/

#define APE_BUFFER_SIZE     256

#define APE_V1              1000U
#define APE_V2              2000U
#define APE_HEADER_SIZE     32

#define APE_HEAD_FIELD_VERSION      8
#define APE_HEAD_FIELD_TAGSIZE      12
#define APE_HEAD_FIELD_ITEMS_COUNT  16
#define APE_HEAD_FIELD_FLAGS        20
#define APE_HEAD_FIELD_RESERVED     24

#define APE_FRAME_TAG_KEY       4

static SDL_INLINE long ape_byteint_decode(const Uint8 *data, size_t size)
{
    Uint32 result = 0;
    Sint32 i;

    for (i = (Sint32)size - 1; i >= 0; i--) {
        result <<= 8;
        result = result | (Uint8)data[i];
    }

    return (long)result;
}

static SDL_INLINE SDL_bool is_apetag(const Uint8 *data, size_t length)
{
   /* http://wiki.hydrogenaud.io/index.php?title=APEv2_specification
    * Header/footer is 32 bytes: bytes 0-7 ident, bytes 8-11 version,
    * bytes 12-17 size. bytes 24-31 are reserved: must be all zeroes. */
    Uint32 v;

    if (length < 32 || SDL_memcmp(data,"APETAGEX",8) != 0) {
        return SDL_FALSE;
    }

    v = (Uint32)(ape_byteint_decode(data + 8, 4)); /* version */
    if (v != APE_V2 && v != APE_V1) {
        return SDL_FALSE;
    }
    v = 0; /* reserved bits : */
    if (SDL_memcmp(&data[24],&v,4) != 0 || SDL_memcmp(&data[28],&v,4) != 0) {
        return SDL_FALSE;
    }
    return SDL_TRUE;
}

static SDL_INLINE long get_ape_len(const Uint8 *data, Uint32 *version)
{
    Uint32 flags;
    long size = (long)(ape_byteint_decode(data + APE_HEAD_FIELD_TAGSIZE, 4));
    *version = (Uint32)(ape_byteint_decode(data + APE_HEAD_FIELD_VERSION, 4));
    flags = (Uint32)(ape_byteint_decode(data + APE_HEAD_FIELD_FLAGS, 4));
    if (*version == APE_V2 && (flags & (1U<<31))) {
        size += APE_HEADER_SIZE; /* header present. */
    }
    return size;
}

static SDL_INLINE char *ape_find_value(char *key)
{
    char *end = (key + APE_BUFFER_SIZE - 4);

    while (*key && key != end) {
        key++;
    }

    if (key >= end) {
        return NULL;
    }

    return key + 1;
}

static Uint32 ape_handle_tag(Mix_MusicMetaTags *out_tags, Uint8 *data, size_t valsize)
{
    /* http://wiki.hydrogenaud.io/index.php?title=APE_Tag_Item
     * Tag entry has unclear size because of no size value for a key field
     * However, we only know next sizes:
     * - 4 bytes is a [length] of value field
     * - 4 bytes of value-specific flags
     * - unknown lenght of a key field. To detect it's size
     *   it's need to find a zero byte looking at begin of the key field
     * - 1 byte of a null-terminator
     * - [length] bytes a value content
     */
    char *key = (char*)(data + APE_FRAME_TAG_KEY);
    char *value = NULL;
    Uint32 key_len; /* Length of the key field */

    value = ape_find_value(key);

    if (!value) {
        return 0;
    }

    key_len = (Uint32)(value - key);

    if (valsize > APE_BUFFER_SIZE - key_len) {
        data[APE_BUFFER_SIZE] = '\0';
    } else {
        value[valsize] = '\0';
    }

    if (SDL_strncasecmp(key, "Title", 6) == 0) {
        meta_tags_set(out_tags, MIX_META_TITLE, (const char*)(value));
    } else if (SDL_strncasecmp(key, "Album", 6) == 0) {
        meta_tags_set(out_tags, MIX_META_ARTIST, (const char*)(value));
    } else if (SDL_strncasecmp(key, "Artist", 7) == 0) {
        meta_tags_set(out_tags, MIX_META_ALBUM, (const char*)(value));
    } else if (SDL_strncasecmp(key, "Copyright", 10) == 0) {
        meta_tags_set(out_tags, MIX_META_COPYRIGHT, (const char*)(value));
    }

    return 4 + (Uint32)valsize + key_len;
}

/* Parse content of APE tag  */
static SDL_bool parse_ape(Mix_MusicMetaTags *out_tags, SDL_RWops *src, Sint64 begin_pos, Uint32 version)
{
    Uint8 buffer[APE_BUFFER_SIZE + 1];
    Uint32 v, i, tag_size, tag_items_count, tag_item_size;
    Uint32 zero8[2] = {0, 0};
    Sint64 file_size, cur_tag;
    size_t read_size;

    file_size = SDL_RWsize(src);
    SDL_RWseek(src, begin_pos, RW_SEEK_SET);
    read_size = SDL_RWread(src, buffer, 1, APE_HEADER_SIZE); /* Retrieve the header */

    if (read_size < APE_HEADER_SIZE) {
        SDL_RWseek(src, begin_pos, RW_SEEK_SET);
        return SDL_FALSE;
    }

    v = (Uint32)ape_byteint_decode(buffer + APE_HEAD_FIELD_VERSION, 4); /* version */
    if (v != APE_V2 && v != APE_V1) {
        return SDL_FALSE;
    }

    tag_size = (Uint32)ape_byteint_decode(buffer + APE_HEAD_FIELD_TAGSIZE, 4); /* tag size */

    if (version == APE_V1) { /* If version 1, we are at footer */
        if (begin_pos - (tag_size - APE_HEADER_SIZE) < 0) {
            SDL_RWseek(src, begin_pos, RW_SEEK_SET);
            return SDL_FALSE;
        }
        SDL_RWseek(src, begin_pos - (tag_size - APE_HEADER_SIZE), RW_SEEK_SET);
    } else if ((begin_pos + tag_size + APE_HEADER_SIZE) > file_size) {
        SDL_RWseek(src, begin_pos, RW_SEEK_SET);
        return SDL_FALSE;
    }

    tag_items_count = (Uint32)ape_byteint_decode(buffer + APE_HEAD_FIELD_ITEMS_COUNT, 4); /* count tag items */

    /*flags = (Uint32)ape_byteint_decode(buffer + APE_HEAD_FIELD_FLAGS, 4);*/ /* global flags, unused */

    /* reserved bits : */
    if (SDL_memcmp(buffer + APE_HEAD_FIELD_RESERVED, zero8, 8) != 0) {
        return SDL_FALSE;
    }

    for(i = 0; i < tag_items_count; i++) {
        cur_tag = SDL_RWtell(src);
        if (cur_tag < 0) {
            break;
        }
        read_size = SDL_RWread(src, buffer, 1, 4); /* Retrieve the size */
        if (read_size < 4) {
            SDL_RWseek(src, begin_pos, RW_SEEK_SET);
            return SDL_FALSE;
        }

        v = (Uint32)ape_byteint_decode(buffer, 4); /* size of the tag's value field */
        /* (we still need to find key size by a null termination) */

        /* Retrieve the tag's data with an aproximal size as we can */
        if (v + 40 < APE_BUFFER_SIZE) {
            read_size = SDL_RWread(src, buffer, 1, v + 40);
        } else {
            read_size = SDL_RWread(src, buffer, 1, APE_BUFFER_SIZE);
        }
        buffer[read_size] = '\0';

        tag_item_size = ape_handle_tag(out_tags, buffer, v);
        if (tag_item_size == 0) {
            break;
        }
        SDL_RWseek(src, cur_tag + tag_item_size + 4, RW_SEEK_SET);
    }

    SDL_RWseek(src, begin_pos, RW_SEEK_SET);

    return SDL_TRUE;
}

int mp3_read_tags(Mix_MusicMetaTags *out_tags, SDL_RWops *src, struct mp3file_t *file_edges)
{
    Uint8 in_buffer[TAGS_INPUT_BUFFER_SIZE];
    long len;
    size_t readsize;
    Sint64 file_size, begin_pos, begin_offset = 0, tail_size = 0, ape_tag_pos;
    SDL_bool tag_handled = SDL_FALSE;

    if (!src) {
        return -1;
    }

    begin_pos = SDL_RWtell(src);
    if (file_edges) {
        file_size = file_edges->length;
    } else {
        file_size = SDL_RWsize(src);
    }

    SDL_RWseek(src, begin_pos, RW_SEEK_SET);
    readsize = SDL_RWread(src, in_buffer, 1, TAGS_INPUT_BUFFER_SIZE);
    SDL_RWseek(src, begin_pos, RW_SEEK_SET);

    if (!readsize) {
        return -1;
    }

    /* ID3v2 tag is at the start */
    if (is_id3v2(in_buffer, readsize)) {
        len = get_id3v2_len(in_buffer, (long)readsize);
        if (len > file_size) {
            return -1;
        }
        tag_handled = parse_id3v2(out_tags, src, begin_pos);
        begin_offset += len;
        file_size -= (size_t)len;
    }
    /* APE tag _might_ be at the start (discouraged
     * but not forbidden, either.)  read the header. */
    else if (is_apetag(in_buffer, readsize)) {
        Uint32 v;
        len = get_ape_len(in_buffer, &v);
        if (len >= file_size) {
            return -1;
        }
        if (v != APE_V2) {
            return -1; /* APEv1 can't be at begin of the file, it's invalid tag */
        }
        parse_ape(out_tags, src, begin_pos, v);
        tag_handled = SDL_TRUE;
        begin_offset += len;
        file_size -= (size_t)len;
    }

    while(1) { /*Handle tags at end of the file */
        /* ID3v1 tag at end of the file */
        if (file_size >= ID3v1_TAG_SIZE) {
            SDL_RWseek(src, -(tail_size + ID3v1_TAG_SIZE), RW_SEEK_END);
            readsize = SDL_RWread(src, in_buffer, 1, ID3v1_TAG_SIZE);
            SDL_RWseek(src, begin_pos, RW_SEEK_SET);
            if (readsize != ID3v1_TAG_SIZE) {
                return -1;
            }

            /* ID3v1 tag is at the end */
            if (is_id3v1(in_buffer, readsize)) {
                if (!tag_handled) {
                    parse_id3v1(out_tags, in_buffer);
                }
                tail_size += ID3v1_TAG_SIZE;
                file_size -= ID3v1_TAG_SIZE;
                continue;
            }
        }

        /* APE tag may be at the end: read the footer */
        if (file_size >= APE_HEADER_SIZE) {
            SDL_RWseek(src, -(tail_size + APE_HEADER_SIZE), RW_SEEK_END);
            readsize = SDL_RWread(src, in_buffer, 1, APE_HEADER_SIZE);
            SDL_RWseek(src, begin_pos, RW_SEEK_SET);
            if (readsize != APE_HEADER_SIZE) {
                return -1;
            }

            /* APE tag may be at end or before ID3v1 tag */
            if (is_apetag(in_buffer, APE_HEADER_SIZE)) {
                Uint32 v;
                len = get_ape_len(in_buffer, &v);
                if (len >= file_size) {
                    return -1;
                }
                if (v == APE_V2) { /* verify header : */
                    SDL_RWseek(src, -(tail_size + len), RW_SEEK_END);
                    ape_tag_pos = SDL_RWtell(src);
                    readsize = SDL_RWread(src, in_buffer, 1, APE_HEADER_SIZE);
                    SDL_RWseek(src, begin_pos, RW_SEEK_SET);
                    if (readsize != APE_HEADER_SIZE) {
                        return -1;
                    }
                    if (!is_apetag(in_buffer, APE_HEADER_SIZE)) {
                        return -1;
                    }
                    if (!tag_handled) {
                        parse_ape(out_tags, src, ape_tag_pos, APE_V2);
                        SDL_RWseek(src, begin_pos, RW_SEEK_SET);
                    }
                } else if (!tag_handled) {
                    SDL_bool ape_tag_valid;
                    SDL_RWseek(src, -(tail_size + APE_HEADER_SIZE), RW_SEEK_END);
                    ape_tag_pos = SDL_RWtell(src);
                    ape_tag_valid = parse_ape(out_tags, src, ape_tag_pos, APE_V1);
                    SDL_RWseek(src, begin_pos, RW_SEEK_SET);
                    if (!ape_tag_valid) {
                        return -1;
                    }
                }
                file_size -= (size_t)len;
                tail_size += len;
                continue;
            }
        }

        /* Lyrics3 tag may be at the end: read the ending marker string */
        if (file_size >= (LYRICS3v1_HEAD_SIZE + LYRICS3v1_TAIL_SIZE)) {
            SDL_RWseek(src, -(tail_size + LYRICS3v1_TAIL_SIZE), RW_SEEK_END);
            readsize = SDL_RWread(src, in_buffer, 1, LYRICS3v1_TAIL_SIZE);
            SDL_RWseek(src, begin_pos, RW_SEEK_SET);
            if (readsize != LYRICS3v1_TAIL_SIZE) {
                return -1;
            }

            /* Lyrics3 tag may be at end or before ID3v1 tag */
            if (is_lyrics3tag(in_buffer, readsize)) {
                len = get_lyrics3_len(tail_size, begin_pos, src);
                if (len < 0) {
                    return -1;
                }
                file_size -= (size_t)len;
                tail_size += len;
                continue;
            }
        }

        break; /* There is no more tags found */
    }

    if (file_edges) {
        file_edges->start = begin_offset;
        file_edges->length = file_size;
    }

    return (file_size > 0) ? 0 :-1;
}

int mp3_read_tags_mem(Mix_MusicMetaTags *out_tags, Uint8 *data, size_t length, struct mp3file_t *file_edges)
{
    SDL_RWops *src = SDL_RWFromConstMem(data, (int)length);
    if (src) {
        int ret = mp3_read_tags(out_tags, src, file_edges);
        SDL_RWclose(src);
        return ret;
    }
    return -1;
}
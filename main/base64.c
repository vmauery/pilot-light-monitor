/*
 * Base64 encoding/decoding (RFC1341)
 * init/upd/final version for basic authentication
 *
 * Copyright 2023 (C) Vernon Mauery <vernon@mauery.org>
 */
#include <malloc.h>
#include <stdint.h>
#include <string.h>

static const unsigned char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

struct b64_ctx
{
    char* buf;   // starting buffer
    size_t idx;  // index 0-2 for fsm
    uint8_t rmd; // leftover bits from last transaction
    size_t len;  // buffer length
    char* pos;   // current position within buffer
};

size_t b64_encode_len(size_t len)
{
    // each 3 bytes of input turns to four, round up
    size_t olen = ((len + 2) / 3) * 4;
    // one more for nul termination
    olen++;
    return olen;
}

void b64_init(struct b64_ctx* ctx, char* buf, size_t len)
{
    if (!buf)
    {
        buf = malloc(len);
    }
    ctx->buf = buf;
    ctx->idx = 0;
    ctx->len = len;
    ctx->pos = buf;
}

void b64_upd(struct b64_ctx* ctx, const char* msg, size_t len)
{
    const uint8_t* src = (const uint8_t*)msg;
    const uint8_t* end = src + len;
    const uint8_t* in = src;

    while (in < end)
    {
        uint8_t b = *in++;
        // at any given time, a new input byte can result in 4 output bytes:
        // two encoded bytes, a newline, and a nul termination byte
        if ((ctx->pos - ctx->buf + 4) >= ctx->len)
        {
            break;
        }
        switch (ctx->idx)
        {
            case 0:
                *(ctx->pos)++ = b64_table[b >> 2];
                ctx->rmd = (b & 0x03) << 4;
                ctx->idx = 1;
                break;
            case 1:
                *(ctx->pos)++ = b64_table[ctx->rmd | (b >> 4)];
                ctx->rmd = (b & 0x0f) << 2;
                ctx->idx = 2;
                break;
            case 2:
                *(ctx->pos)++ = b64_table[ctx->rmd | (b >> 6)];
                *(ctx->pos)++ = b64_table[b & 0x3f];
                ctx->rmd = 0;
                ctx->idx = 0;
                break;
        }
    }
}

void b64_final(struct b64_ctx* ctx)
{
    switch (ctx->idx)
    {
        case 0:
            break;
        case 1:
            *(ctx->pos)++ = b64_table[ctx->rmd];
            *(ctx->pos)++ = '=';
            *(ctx->pos)++ = '=';
            break;
        case 2:
            *(ctx->pos)++ = b64_table[ctx->rmd];
            *(ctx->pos)++ = '=';
            break;
    }
    *(ctx->pos) = '\0';
}

char* basic_auth(const char* user, const char* passwd)
{
    size_t user_len = strlen(user);
    size_t passwd_len = strlen(passwd);
    size_t msg_size = user_len + passwd_len + 1;
    struct b64_ctx ctx;

    const size_t basic_len = 6;
    size_t alloc_size = b64_encode_len(msg_size) + basic_len + 8;
    char* out = malloc(alloc_size);
    strncpy(out, "Basic ", alloc_size);

    // base-64 encode authentication in place
    b64_init(&ctx, out + basic_len, alloc_size - basic_len);
    b64_upd(&ctx, user, strlen(user));
    b64_upd(&ctx, ":", 1);
    b64_upd(&ctx, passwd, passwd_len);
    b64_final(&ctx);

    return out;
}

#ifdef TEST
#include <stdio.h>
int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        printf("Usage: %s <name> <pw>\n", argv[0]);
        return 1;
    }
    printf("Authorization: %s", basic_auth(argv[1], argv[2]));
    return 0;
}
#endif

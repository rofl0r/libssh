/*
 * keyfiles.c - private and public key handling for authentication.
 *
 * This file is part of the SSH Library
 *
 * Copyright (c) 2003-2009 by Aris Adamantiadis
 * Copyright (c) 2009      by Andreas Schneider <mail@cynapses.org>
 *
 * The SSH Library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * The SSH Library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the SSH Library; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
# if _MSC_VER >= 1400
#  include <io.h>
#  undef open
#  define open _open
#  undef close
#  define close _close
#  undef read
#  define read _read
#  undef unlink
#  define unlink _unlink
# endif /* _MSC_VER */
#else
# include <arpa/inet.h>
#endif

#include "libssh/priv.h"
#include "libssh/buffer.h"
#include "libssh/keyfiles.h"
#include "libssh/session.h"
#include "libssh/wrapper.h"
#include "libssh/misc.h"
#include "libssh/keys.h"
#include "libssh/pki.h"

/*todo: remove this include */
#include "libssh/string.h"


#ifdef HAVE_LIBGCRYPT
#include <gcrypt.h>
#elif defined HAVE_LIBCRYPTO
#include <openssl/pem.h>
#include <openssl/dsa.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
#endif /* HAVE_LIBCRYPTO */

#define MAXLINESIZE 80
#define RSA_HEADER_BEGIN "-----BEGIN RSA PRIVATE KEY-----"
#define RSA_HEADER_END "-----END RSA PRIVATE KEY-----"
#define DSA_HEADER_BEGIN "-----BEGIN DSA PRIVATE KEY-----"
#define DSA_HEADER_END "-----END DSA PRIVATE KEY-----"

#ifdef HAVE_LIBGCRYPT

#define MAX_KEY_SIZE 32
#define MAX_PASSPHRASE_SIZE 1024
#define ASN1_INTEGER 2
#define ASN1_SEQUENCE 48
#define PKCS5_SALT_LEN 8

static int load_iv(const char *header, unsigned char *iv, int iv_len) {
  int i;
  int j;
  int k;

  memset(iv, 0, iv_len);
  for (i = 0; i < iv_len; i++) {
    if ((header[2*i] >= '0') && (header[2*i] <= '9'))
      j = header[2*i] - '0';
    else if ((header[2*i] >= 'A') && (header[2*i] <= 'F'))
      j = header[2*i] - 'A' + 10;
    else if ((header[2*i] >= 'a') && (header[2*i] <= 'f'))
      j = header[2*i] - 'a' + 10;
    else
      return -1;
    if ((header[2*i+1] >= '0') && (header[2*i+1] <= '9'))
      k = header[2*i+1] - '0';
    else if ((header[2*i+1] >= 'A') && (header[2*i+1] <= 'F'))
      k = header[2*i+1] - 'A' + 10;
    else if ((header[2*i+1] >= 'a') && (header[2*i+1] <= 'f'))
      k = header[2*i+1] - 'a' + 10;
    else
      return -1;
    iv[i] = (j << 4) + k;
  }
  return 0;
}

static uint32_t char_to_u32(unsigned char *data, uint32_t size) {
  uint32_t ret;
  uint32_t i;

  for (i = 0, ret = 0; i < size; ret = ret << 8, ret += data[i++])
    ;
  return ret;
}

static uint32_t asn1_get_len(ssh_buffer buffer) {
  uint32_t len;
  unsigned char tmp[4];

  if (buffer_get_data(buffer,tmp,1) == 0) {
    return 0;
  }

  if (tmp[0] > 127) {
    len = tmp[0] & 127;
    if (len > 4) {
      return 0; /* Length doesn't fit in u32. Can this really happen? */
    }
    if (buffer_get_data(buffer,tmp,len) == 0) {
      return 0;
    }
    len = char_to_u32(tmp, len);
  } else {
    len = char_to_u32(tmp, 1);
  }

  return len;
}

static ssh_string asn1_get_int(ssh_buffer buffer) {
  ssh_string str;
  unsigned char type;
  uint32_t size;

  if (buffer_get_data(buffer, &type, 1) == 0 || type != ASN1_INTEGER) {
    return NULL;
  }
  size = asn1_get_len(buffer);
  if (size == 0) {
    return NULL;
  }

  str = ssh_string_new(size);
  if (str == NULL) {
    return NULL;
  }

  if (buffer_get_data(buffer, str->string, size) == 0) {
    ssh_string_free(str);
    return NULL;
  }

  return str;
}

static int asn1_check_sequence(ssh_buffer buffer) {
  unsigned char *j = NULL;
  unsigned char tmp;
  int i;
  uint32_t size;
  uint32_t padding;

  if (buffer_get_data(buffer, &tmp, 1) == 0 || tmp != ASN1_SEQUENCE) {
    return 0;
  }

  size = asn1_get_len(buffer);
  if ((padding = ssh_buffer_get_len(buffer) - buffer->pos - size) > 0) {
    for (i = ssh_buffer_get_len(buffer) - buffer->pos - size,
         j = (unsigned char*)ssh_buffer_get_begin(buffer) + size + buffer->pos;
         i;
         i--, j++)
    {
      if (*j != padding) {                   /* padding is allowed */
        return 0;                            /* but nothing else */
      }
    }
  }

  return 1;
}

static int passphrase_to_key(char *data, unsigned int datalen,
    unsigned char *salt, unsigned char *key, unsigned int keylen) {
  MD5CTX md;
  unsigned char digest[MD5_DIGEST_LEN] = {0};
  unsigned int i;
  unsigned int j;
  unsigned int md_not_empty;

  for (j = 0, md_not_empty = 0; j < keylen; ) {
    md = md5_init();
    if (md == NULL) {
      return -1;
    }

    if (md_not_empty) {
      md5_update(md, digest, MD5_DIGEST_LEN);
    } else {
      md_not_empty = 1;
    }

    md5_update(md, data, datalen);
    if (salt) {
      md5_update(md, salt, PKCS5_SALT_LEN);
    }
    md5_final(digest, md);

    for (i = 0; j < keylen && i < MD5_DIGEST_LEN; j++, i++) {
      if (key) {
        key[j] = digest[i];
      }
    }
  }

  return 0;
}

static int privatekey_decrypt(int algo, int mode, unsigned int key_len,
                       unsigned char *iv, unsigned int iv_len,
                       ssh_buffer data, ssh_auth_callback cb,
                       void *userdata,
                       const char *desc)
{
  char passphrase[MAX_PASSPHRASE_SIZE] = {0};
  unsigned char key[MAX_KEY_SIZE] = {0};
  unsigned char *tmp = NULL;
  gcry_cipher_hd_t cipher;
  int rc = -1;

  if (!algo) {
    return -1;
  }

  if (cb) {
    rc = (*cb)(desc, passphrase, MAX_PASSPHRASE_SIZE, 0, 0, userdata);
    if (rc < 0) {
      return -1;
    }
  } else if (cb == NULL && userdata != NULL) {
    snprintf(passphrase, MAX_PASSPHRASE_SIZE, "%s", (char *) userdata);
  }

  if (passphrase_to_key(passphrase, strlen(passphrase), iv, key, key_len) < 0) {
    return -1;
  }

  if (gcry_cipher_open(&cipher, algo, mode, 0)
      || gcry_cipher_setkey(cipher, key, key_len)
      || gcry_cipher_setiv(cipher, iv, iv_len)
      || (tmp = malloc(ssh_buffer_get_len(data) * sizeof (char))) == NULL
      || gcry_cipher_decrypt(cipher, tmp, ssh_buffer_get_len(data),
                       ssh_buffer_get_begin(data), ssh_buffer_get_len(data))) {
    gcry_cipher_close(cipher);
    return -1;
  }

  memcpy(ssh_buffer_get_begin(data), tmp, ssh_buffer_get_len(data));

  SAFE_FREE(tmp);
  gcry_cipher_close(cipher);

  return 0;
}

static int privatekey_dek_header(const char *header, unsigned int header_len,
    int *algo, int *mode, unsigned int *key_len, unsigned char **iv,
    unsigned int *iv_len) {
  unsigned int iv_pos;

  if (header_len > 13 && !strncmp("DES-EDE3-CBC", header, 12))
  {
    *algo = GCRY_CIPHER_3DES;
    iv_pos = 13;
    *mode = GCRY_CIPHER_MODE_CBC;
    *key_len = 24;
    *iv_len = 8;
  }
  else if (header_len > 8 && !strncmp("DES-CBC", header, 7))
  {
    *algo = GCRY_CIPHER_DES;
    iv_pos = 8;
    *mode = GCRY_CIPHER_MODE_CBC;
    *key_len = 8;
    *iv_len = 8;
  }
  else if (header_len > 12 && !strncmp("AES-128-CBC", header, 11))
  {
    *algo = GCRY_CIPHER_AES128;
    iv_pos = 12;
    *mode = GCRY_CIPHER_MODE_CBC;
    *key_len = 16;
    *iv_len = 16;
  }
  else if (header_len > 12 && !strncmp("AES-192-CBC", header, 11))
  {
    *algo = GCRY_CIPHER_AES192;
    iv_pos = 12;
    *mode = GCRY_CIPHER_MODE_CBC;
    *key_len = 24;
    *iv_len = 16;
  }
  else if (header_len > 12 && !strncmp("AES-256-CBC", header, 11))
  {
    *algo = GCRY_CIPHER_AES256;
    iv_pos = 12;
    *mode = GCRY_CIPHER_MODE_CBC;
    *key_len = 32;
    *iv_len = 16;
  } else {
    return -1;
  }

  *iv = malloc(*iv_len);
  if (*iv == NULL) {
    return -1;
  }

  return load_iv(header + iv_pos, *iv, *iv_len);
}

#define get_next_line(p, len) {                                         \
        while(p[len] == '\n' || p[len] == '\r') /* skip empty lines */  \
            len++;                                                      \
        if(p[len] == '\0')    /* EOL */                                 \
            len = -1;                                                   \
        else                  /* calculate length */                    \
            for(p += len, len = 0; p[len] && p[len] != '\n'             \
                                          && p[len] != '\r'; len++);    \
    }

static ssh_buffer privatekey_string_to_buffer(const char *pkey, int type,
                ssh_auth_callback cb, void *userdata, const char *desc) {
    ssh_buffer buffer = NULL;
    ssh_buffer out = NULL;
    const char *p;
    unsigned char *iv = NULL;
    const char *header_begin;
    const char *header_end;
    unsigned int header_begin_size;
    unsigned int header_end_size;
    unsigned int key_len = 0;
    unsigned int iv_len = 0;
    int algo = 0;
    int mode = 0;
    int len;

    buffer = ssh_buffer_new();
    if (buffer == NULL) {
        return NULL;
    }

    switch(type) {
        case SSH_KEYTYPE_DSS:
            header_begin = DSA_HEADER_BEGIN;
            header_end = DSA_HEADER_END;
            break;
        case SSH_KEYTYPE_RSA:
            header_begin = RSA_HEADER_BEGIN;
            header_end = RSA_HEADER_END;
            break;
        default:
            ssh_buffer_free(buffer);
            return NULL;
    }

    header_begin_size = strlen(header_begin);
    header_end_size = strlen(header_end);

    p = pkey;
    len = 0;
    get_next_line(p, len);

    while(len > 0 && strncmp(p, header_begin, header_begin_size)) {
        /* skip line */
        get_next_line(p, len);
    }
    if(len < 0) {
        /* no header found */
        return NULL;
    }
    /* skip header line */
    get_next_line(p, len);

    if (len > 11 && strncmp("Proc-Type: 4,ENCRYPTED", p, 11) == 0) {
        /* skip line */
        get_next_line(p, len);

        if (len > 10 && strncmp("DEK-Info: ", p, 10) == 0) {
            p += 10;
            len = 0;
            get_next_line(p, len);
            if (privatekey_dek_header(p, len, &algo, &mode, &key_len,
                        &iv, &iv_len) < 0) {
                ssh_buffer_free(buffer);
                SAFE_FREE(iv);
                return NULL;
            }
        } else {
            ssh_buffer_free(buffer);
            SAFE_FREE(iv);
            return NULL;
        }
    } else {
        if(len > 0) {
            if (buffer_add_data(buffer, p, len) < 0) {
                ssh_buffer_free(buffer);
                SAFE_FREE(iv);
                return NULL;
            }
        }
    }

    get_next_line(p, len);
    while(len > 0 && strncmp(p, header_end, header_end_size) != 0) {
        if (buffer_add_data(buffer, p, len) < 0) {
            ssh_buffer_free(buffer);
            SAFE_FREE(iv);
            return NULL;
        }
        get_next_line(p, len);
    }

    if (len == -1 || strncmp(p, header_end, header_end_size) != 0) {
        ssh_buffer_free(buffer);
        SAFE_FREE(iv);
        return NULL;
    }

    if (buffer_add_data(buffer, "\0", 1) < 0) {
        ssh_buffer_free(buffer);
        SAFE_FREE(iv);
        return NULL;
    }

    out = base64_to_bin(ssh_buffer_get_begin(buffer));
    ssh_buffer_free(buffer);
    if (out == NULL) {
        SAFE_FREE(iv);
        return NULL;
    }

    if (algo) {
        if (privatekey_decrypt(algo, mode, key_len, iv, iv_len, out,
                    cb, userdata, desc) < 0) {
            ssh_buffer_free(out);
            SAFE_FREE(iv);
            return NULL;
        }
    }
    SAFE_FREE(iv);

    return out;
}

static int b64decode_rsa_privatekey(const char *pkey, gcry_sexp_t *r,
    ssh_auth_callback cb, void *userdata, const char *desc) {
  ssh_string n = NULL;
  ssh_string e = NULL;
  ssh_string d = NULL;
  ssh_string p = NULL;
  ssh_string q = NULL;
  ssh_string unused1 = NULL;
  ssh_string unused2 = NULL;
  ssh_string u = NULL;
  ssh_string v = NULL;
  ssh_buffer buffer = NULL;
  int rc = 1;

  buffer = privatekey_string_to_buffer(pkey, SSH_KEYTYPE_RSA, cb, userdata, desc);
  if (buffer == NULL) {
    return 0;
  }

  if (!asn1_check_sequence(buffer)) {
    ssh_buffer_free(buffer);
    return 0;
  }

  v = asn1_get_int(buffer);
  if (ntohl(v->size) != 1 || v->string[0] != 0) {
    ssh_buffer_free(buffer);
    return 0;
  }

  n = asn1_get_int(buffer);
  e = asn1_get_int(buffer);
  d = asn1_get_int(buffer);
  q = asn1_get_int(buffer);
  p = asn1_get_int(buffer);
  unused1 = asn1_get_int(buffer);
  unused2 = asn1_get_int(buffer);
  u = asn1_get_int(buffer);

  ssh_buffer_free(buffer);

  if (n == NULL || e == NULL || d == NULL || p == NULL || q == NULL ||
      unused1 == NULL || unused2 == NULL|| u == NULL) {
    rc = 0;
    goto error;
  }

  if (gcry_sexp_build(r, NULL,
      "(private-key(rsa(n %b)(e %b)(d %b)(p %b)(q %b)(u %b)))",
      ntohl(n->size), n->string,
      ntohl(e->size), e->string,
      ntohl(d->size), d->string,
      ntohl(p->size), p->string,
      ntohl(q->size), q->string,
      ntohl(u->size), u->string)) {
    rc = 0;
  }

error:
  ssh_string_free(n);
  ssh_string_free(e);
  ssh_string_free(d);
  ssh_string_free(p);
  ssh_string_free(q);
  ssh_string_free(unused1);
  ssh_string_free(unused2);
  ssh_string_free(u);
  ssh_string_free(v);

  return rc;
}

static int b64decode_dsa_privatekey(const char *pkey, gcry_sexp_t *r, ssh_auth_callback cb,
    void *userdata, const char *desc) {
  ssh_buffer buffer = NULL;
  ssh_string p = NULL;
  ssh_string q = NULL;
  ssh_string g = NULL;
  ssh_string y = NULL;
  ssh_string x = NULL;
  ssh_string v = NULL;
  int rc = 1;

  buffer = privatekey_string_to_buffer(pkey, SSH_KEYTYPE_DSS, cb, userdata, desc);
  if (buffer == NULL) {
    return 0;
  }

  if (!asn1_check_sequence(buffer)) {
    ssh_buffer_free(buffer);
    return 0;
  }

  v = asn1_get_int(buffer);
  if (ntohl(v->size) != 1 || v->string[0] != 0) {
    ssh_buffer_free(buffer);
    return 0;
  }

  p = asn1_get_int(buffer);
  q = asn1_get_int(buffer);
  g = asn1_get_int(buffer);
  y = asn1_get_int(buffer);
  x = asn1_get_int(buffer);
  ssh_buffer_free(buffer);

  if (p == NULL || q == NULL || g == NULL || y == NULL || x == NULL) {
    rc = 0;
    goto error;
  }

  if (gcry_sexp_build(r, NULL,
        "(private-key(dsa(p %b)(q %b)(g %b)(y %b)(x %b)))",
        ntohl(p->size), p->string,
        ntohl(q->size), q->string,
        ntohl(g->size), g->string,
        ntohl(y->size), y->string,
        ntohl(x->size), x->string)) {
    rc = 0;
  }

error:
  ssh_string_free(p);
  ssh_string_free(q);
  ssh_string_free(g);
  ssh_string_free(y);
  ssh_string_free(x);
  ssh_string_free(v);

  return rc;
}

ssh_key pki_private_key_from_base64(ssh_session session,
                                    const char *b64_key,
                                    const char *passphrase) {
    ssh_auth_callback auth_cb = NULL;
    void *auth_ud = NULL;

    gcry_sexp_t dsa = NULL;
    gcry_sexp_t rsa = NULL;
    ssh_key key = NULL;
    enum ssh_keytypes_e type;
    int valid;

    /* needed for gcrypt initialization */
    if (ssh_init() < 0) {
        return NULL;
    }

    type = pki_privatekey_type_from_string(b64_key);
    if (type == SSH_KEYTYPE_UNKNOWN) {
        ssh_set_error(session, SSH_FATAL, "Unknown or invalid private key.");
        return NULL;
    }

    switch (type) {
        case SSH_KEYTYPE_DSS:
            if (passphrase == NULL) {
                if (session->common.callbacks &&
                    session->common.callbacks->auth_function) {
                    auth_cb = session->common.callbacks->auth_function;
                    auth_ud = session->common.callbacks->userdata;

                    valid = b64decode_dsa_privatekey(b64_key, &dsa, auth_cb,
                            auth_ud, "Passphrase for private key:");
                } else {
                    valid = b64decode_dsa_privatekey(b64_key, &dsa, NULL, NULL,
                            NULL);
                }
            } else {
                valid = b64decode_dsa_privatekey(b64_key, &dsa, NULL, (void *)
                        passphrase, NULL);
            }

            if (!valid) {
                ssh_set_error(session, SSH_FATAL, "Parsing private key");
                goto fail;
            }
            break;
        case SSH_KEYTYPE_RSA:
        case SSH_KEYTYPE_RSA1:
            if (passphrase == NULL) {
                if (session->common.callbacks &&
                    session->common.callbacks->auth_function) {
                    auth_cb = session->common.callbacks->auth_function;
                    auth_ud = session->common.callbacks->userdata;
                    valid = b64decode_rsa_privatekey(b64_key, &rsa, auth_cb,
                            auth_ud, "Passphrase for private key:");
                } else {
                    valid = b64decode_rsa_privatekey(b64_key, &rsa, NULL, NULL,
                            NULL);
                }
            } else {
                valid = b64decode_rsa_privatekey(b64_key, &rsa, NULL,
                        (void *)passphrase, NULL);
            }

            if (!valid) {
                ssh_set_error(session,SSH_FATAL, "Parsing private key");
                goto fail;
            }
            break;
        case SSH_KEYTYPE_ECDSA:
        case SSH_KEYTYPE_UNKNOWN:
            ssh_set_error(session, SSH_FATAL,
                          "Unkown or invalid private key type %d", type);
            return NULL;
    }

    key = ssh_key_new();
    if (key == NULL) {
        goto fail;
    }

    key->type = type;
    key->type_c = ssh_key_type_to_char(type);
    key->flags = SSH_KEY_FLAG_PRIVATE | SSH_KEY_FLAG_PUBLIC;
    key->dsa = dsa;
    key->rsa = rsa;

    return key;
fail:
    ssh_key_free(key);
    gcry_sexp_release(dsa);
    gcry_sexp_release(rsa);

    return NULL;
}

int pki_pubkey_build_dss(ssh_key key,
                         ssh_string p,
                         ssh_string q,
                         ssh_string g,
                         ssh_string pubkey) {
    gcry_sexp_build(&key->dsa, NULL,
            "(public-key(dsa(p %b)(q %b)(g %b)(y %b)))",
            ssh_string_len(p), ssh_string_data(p),
            ssh_string_len(q), ssh_string_data(q),
            ssh_string_len(g), ssh_string_data(g),
            ssh_string_len(pubkey), ssh_string_data(pubkey));
    if (key->dsa == NULL) {
        return SSH_ERROR;
    }

    return SSH_OK;
}

int pki_pubkey_build_rsa(ssh_key key,
                         ssh_string e,
                         ssh_string n) {
    gcry_sexp_build(&key->rsa, NULL,
            "(public-key(rsa(n %b)(e %b)))",
            ssh_string_len(n), ssh_string_data(n),
            ssh_string_len(e),ssh_string_data(e));
    if (key->rsa == NULL) {
        return SSH_ERROR;
    }

    return SSH_OK;
}

ssh_key pki_publickey_from_privatekey(ssh_key privkey) {
    ssh_key pubkey = NULL;
    gcry_sexp_t sexp;
    const char *tmp = NULL;
    ssh_string p = NULL;
    ssh_string q = NULL;
    ssh_string g = NULL;
    ssh_string y = NULL;
    ssh_string e = NULL;
    ssh_string n = NULL;
    size_t size;

    if (privkey == NULL || !ssh_key_is_private(privkey)) {
        return NULL;
    }

    pubkey = ssh_key_new();
    if (pubkey == NULL) {
        return NULL;
    }
    pubkey->type = privkey->type;

    switch(pubkey->type) {
        case SSH_KEYTYPE_DSS:
            sexp = gcry_sexp_find_token(privkey->dsa, "p", 0);
            if (sexp == NULL) {
                goto fail;
            }
            tmp = gcry_sexp_nth_data(sexp, 1, &size);
            p = ssh_string_new(size);
            if (p == NULL) {
                goto fail;
            }
            ssh_string_fill(p,(char *) tmp, size);
            gcry_sexp_release(sexp);

            sexp = gcry_sexp_find_token(privkey->dsa,"q",0);
            if (sexp == NULL) {
                goto fail;
            }
            tmp = gcry_sexp_nth_data(sexp,1,&size);
            q = ssh_string_new(size);
            if (q == NULL) {
                goto fail;
            }
            ssh_string_fill(q,(char *) tmp,size);
            gcry_sexp_release(sexp);

            sexp = gcry_sexp_find_token(privkey->dsa, "g", 0);
            if (sexp == NULL) {
                goto fail;
            }
            tmp = gcry_sexp_nth_data(sexp,1,&size);
            g = ssh_string_new(size);
            if (g == NULL) {
                goto fail;
            }
            ssh_string_fill(g,(char *) tmp,size);
            gcry_sexp_release(sexp);

            sexp = gcry_sexp_find_token(privkey->dsa,"y",0);
            if (sexp == NULL) {
                goto fail;
            }
            tmp = gcry_sexp_nth_data(sexp,1,&size);
            y = ssh_string_new(size);
            if (y == NULL) {
                goto fail;
            }
            ssh_string_fill(y,(char *) tmp,size);
            gcry_sexp_release(sexp);

            gcry_sexp_build(&pubkey->dsa, NULL,
                    "(public-key(dsa(p %b)(q %b)(g %b)(y %b)))",
                    ssh_string_len(p), ssh_string_data(p),
                    ssh_string_len(q), ssh_string_data(q),
                    ssh_string_len(g), ssh_string_data(g),
                    ssh_string_len(y), ssh_string_data(y));

            ssh_string_burn(p);
            ssh_string_free(p);
            ssh_string_burn(q);
            ssh_string_free(q);
            ssh_string_burn(g);
            ssh_string_free(g);
            ssh_string_burn(y);
            ssh_string_free(y);
            break;
        case SSH_KEYTYPE_RSA:
        case SSH_KEYTYPE_RSA1:
            sexp = gcry_sexp_find_token(privkey->rsa, "n", 0);
            if (sexp == NULL) {
                goto fail;
            }
            tmp = gcry_sexp_nth_data(sexp, 1, &size);
            n = ssh_string_new(size);
            if (n == NULL) {
                goto fail;
            }
            ssh_string_fill(n, (char *) tmp, size);
            gcry_sexp_release(sexp);

            sexp = gcry_sexp_find_token(privkey->rsa, "e", 0);
            if (sexp == NULL) {
                goto fail;
            }
            tmp = gcry_sexp_nth_data(sexp, 1, &size);
            e = ssh_string_new(size);
            if (e == NULL) {
                goto fail;
            }
            ssh_string_fill(e, (char *) tmp, size);
            gcry_sexp_release(sexp);

            gcry_sexp_build(&pubkey->rsa, NULL,
                    "(public-key(rsa(n %b)(e %b)))",
                    ssh_string_len(n), ssh_string_data(n),
                    ssh_string_len(e), ssh_string_data(e));
            if (pubkey->rsa == NULL) {
                goto fail;
            }

            ssh_string_burn(e);
            ssh_string_free(e);
            ssh_string_burn(n);
            ssh_string_free(n);
            break;
        case SSH_KEYTYPE_ECDSA:
        case SSH_KEYTYPE_UNKNOWN:
            ssh_key_free(pubkey);
            return NULL;
    }
    pubkey->type_c = ssh_key_type_to_char(privkey->type);

    return pubkey;
fail:
    gcry_sexp_release(sexp);
    ssh_string_burn(p);
    ssh_string_free(p);
    ssh_string_burn(q);
    ssh_string_free(q);
    ssh_string_burn(g);
    ssh_string_free(g);
    ssh_string_burn(y);
    ssh_string_free(y);

    ssh_string_burn(e);
    ssh_string_free(e);
    ssh_string_burn(n);
    ssh_string_free(n);

    ssh_key_free(pubkey);

    return NULL;
}

struct signature_struct *pki_do_sign(ssh_key privatekey,
                                     const unsigned char *hash) {
    struct signature_struct *sign;
    gcry_sexp_t gcryhash;

    sign = malloc(sizeof(SIGNATURE));
    if (sign == NULL) {
        return NULL;
    }
    sign->type = privatekey->type;

    switch(privatekey->type) {
        case SSH_KEYTYPE_DSS:
            if (gcry_sexp_build(&gcryhash, NULL, "%b", SHA_DIGEST_LEN + 1, hash) ||
                gcry_pk_sign(&sign->dsa_sign, gcryhash, privatekey->dsa)) {
                gcry_sexp_release(gcryhash);
                signature_free(sign);
                return NULL;
            }
            sign->rsa_sign = NULL;
            break;
        case SSH_KEYTYPE_RSA:
        case SSH_KEYTYPE_RSA1:
            if (gcry_sexp_build(&gcryhash, NULL, "(data(flags pkcs1)(hash sha1 %b))",
                                SHA_DIGEST_LEN, hash + 1) ||
                gcry_pk_sign(&sign->rsa_sign, gcryhash, privatekey->rsa)) {
                gcry_sexp_release(gcryhash);
                signature_free(sign);
                return NULL;
            }
            sign->dsa_sign = NULL;
            break;
        case SSH_KEYTYPE_ECDSA:
        case SSH_KEYTYPE_UNKNOWN:
            signature_free(sign);
            return NULL;
    }

    gcry_sexp_release(gcryhash);

    return sign;
}

#endif /* HAVE_LIBGCRYPT */

/**
 * @addtogroup libssh_auth
 *
 * @{
 */

/**
 * @brief returns the type of a private key
 * @param[in] privatekey the private key handle
 * @returns one of SSH_KEYTYPE_RSA,SSH_KEYTYPE_DSS,SSH_KEYTYPE_RSA1
 * @returns SSH_KEYTYPE_UNKNOWN if the type is unknown
 * @see privatekey_from_file
 * @see ssh_userauth_offer_pubkey
 */
enum ssh_keytypes_e ssh_privatekey_type(ssh_private_key privatekey){
  if (privatekey==NULL)
    return SSH_KEYTYPE_UNKNOWN;
  return privatekey->type;
}

/**
 * @brief Deallocate a private key object.
 *
 * @param[in]  prv      The private_key object to free.
 */
void privatekey_free(ssh_private_key prv) {
  if (prv == NULL) {
    return;
  }

#ifdef HAVE_LIBGCRYPT
  gcry_sexp_release(prv->dsa_priv);
  gcry_sexp_release(prv->rsa_priv);
#elif defined HAVE_LIBCRYPTO
  DSA_free(prv->dsa_priv);
  RSA_free(prv->rsa_priv);
#endif
  memset(prv, 0, sizeof(struct ssh_private_key_struct));
  SAFE_FREE(prv);
}

/**
 * @brief Write a public key to a file.
 *
 * @param[in]  session  The ssh session to use.
 *
 * @param[in]  file     The filename to write the key into.
 *
 * @param[in]  pubkey   The public key to write.
 *
 * @param[in]  type     The type of the public key.
 *
 * @return              0 on success, -1 on error.
 */
int ssh_publickey_to_file(ssh_session session, const char *file,
    ssh_string pubkey, int type) {
  FILE *fp;
  char *user;
  char buffer[1024];
  char host[256];
  unsigned char *pubkey_64;
  size_t len;
  int rc;
  if(session==NULL)
	return SSH_ERROR;
  if(file==NULL || pubkey==NULL){
	ssh_set_error(session, SSH_FATAL, "Invalid parameters");
	return SSH_ERROR;
  }
  pubkey_64 = bin_to_base64(pubkey->string, ssh_string_len(pubkey));
  if (pubkey_64 == NULL) {
    return SSH_ERROR;
  }

  user = ssh_get_local_username(session);
  if (user == NULL) {
    SAFE_FREE(pubkey_64);
    return SSH_ERROR;
  }

  rc = gethostname(host, sizeof(host));
  if (rc < 0) {
    SAFE_FREE(user);
    SAFE_FREE(pubkey_64);
    return SSH_ERROR;
  }

  snprintf(buffer, sizeof(buffer), "%s %s %s@%s\n",
      ssh_type_to_char(type),
      pubkey_64,
      user,
      host);

  SAFE_FREE(pubkey_64);
  SAFE_FREE(user);

  ssh_log(session, SSH_LOG_RARE, "Trying to write public key file: %s", file);
  ssh_log(session, SSH_LOG_PACKET, "public key file content: %s", buffer);

  fp = fopen(file, "w+");
  if (fp == NULL) {
    ssh_set_error(session, SSH_REQUEST_DENIED,
        "Error opening %s: %s", file, strerror(errno));
    return SSH_ERROR;
  }

  len = strlen(buffer);
  if (fwrite(buffer, len, 1, fp) != 1 || ferror(fp)) {
    ssh_set_error(session, SSH_REQUEST_DENIED,
        "Unable to write to %s", file);
    fclose(fp);
    unlink(file);
    return SSH_ERROR;
  }

  fclose(fp);
  return SSH_OK;
}

/**
 * @brief Retrieve a public key from a file.
 *
 * @param[in]  session  The SSH session to use.
 *
 * @param[in]  filename The filename of the public key.
 *
 * @param[out] type     The Pointer to a integer. If it is not NULL, it will
 *                      contain the type of the key after execution.
 *
 * @return              A SSH String containing the public key, or NULL if it
 *                      failed.
 *
 * @see string_free()
 * @see publickey_from_privatekey()
 */
ssh_string publickey_from_file(ssh_session session, const char *filename,
    int *type) {
  ssh_buffer buffer = NULL;
  char buf[4096] = {0};
  ssh_string str = NULL;
  char *ptr = NULL;
  int key_type;
  int fd = -1;
  int r;

  fd = open(filename, O_RDONLY);
  if (fd < 0) {
    ssh_set_error(session, SSH_REQUEST_DENIED, "Public key file doesn't exist");
    return NULL;
  }

  if (read(fd, buf, 8) != 8) {
    close(fd);
    ssh_set_error(session, SSH_REQUEST_DENIED, "Invalid public key file");
    return NULL;
  }

  buf[7] = '\0';

  key_type = ssh_type_from_name(buf);
  if (key_type == -1) {
    close(fd);
    ssh_set_error(session, SSH_REQUEST_DENIED, "Invalid public key file");
    return NULL;
  }

  r = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (r <= 0) {
    ssh_set_error(session, SSH_REQUEST_DENIED, "Invalid public key file");
    return NULL;
  }

  buf[r] = 0;
  ptr = strchr(buf, ' ');

  /* eliminate the garbage at end of file */
  if (ptr) {
    *ptr = '\0';
  }

  buffer = base64_to_bin(buf);
  if (buffer == NULL) {
    ssh_set_error(session, SSH_REQUEST_DENIED, "Invalid public key file");
    return NULL;
  }

  str = ssh_string_new(buffer_get_rest_len(buffer));
  if (str == NULL) {
    ssh_set_error(session, SSH_FATAL, "Not enough space");
    ssh_buffer_free(buffer);
    return NULL;
  }

  ssh_string_fill(str, buffer_get_rest(buffer), buffer_get_rest_len(buffer));
  ssh_buffer_free(buffer);

  if (type) {
    *type = key_type;
  }

  return str;
}

/**
 * @brief Try to read the public key from a given file.
 *
 * @param[in]  session  The ssh session to use.
 *
 * @param[in]  keyfile  The name of the private keyfile.
 *
 * @param[out] publickey A ssh_string to store the public key.
 *
 * @param[out] type     A pointer to an integer to store the type.
 *
 * @return              0 on success, -1 on error or the private key doesn't
 *                      exist, 1 if the public key doesn't exist.
 */
int ssh_try_publickey_from_file(ssh_session session, const char *keyfile,
    ssh_string *publickey, int *type) {
  char *pubkey_file;
  size_t len;
  ssh_string pubkey_string;
  int pubkey_type;

  if (session == NULL || keyfile == NULL || publickey == NULL || type == NULL) {
    return -1;
  }

  if (session->sshdir == NULL) {
    if (ssh_options_apply(session) < 0) {
      return -1;
    }
  }

  ssh_log(session, SSH_LOG_PACKET, "Trying to open privatekey %s", keyfile);
  if (!ssh_file_readaccess_ok(keyfile)) {
    ssh_log(session, SSH_LOG_PACKET, "Failed to open privatekey %s", keyfile);
    return -1;
  }

  len = strlen(keyfile) + 5;
  pubkey_file = malloc(len);
  if (pubkey_file == NULL) {
    return -1;
  }
  snprintf(pubkey_file, len, "%s.pub", keyfile);

  ssh_log(session, SSH_LOG_PACKET, "Trying to open publickey %s",
                                   pubkey_file);
  if (!ssh_file_readaccess_ok(pubkey_file)) {
    ssh_log(session, SSH_LOG_PACKET, "Failed to open publickey %s",
                                     pubkey_file);
    SAFE_FREE(pubkey_file);
    return 1;
  }

  ssh_log(session, SSH_LOG_PACKET, "Success opening public and private key");

  /*
   * We are sure both the private and public key file is readable. We return
   * the public as a string, and the private filename as an argument
   */
  pubkey_string = publickey_from_file(session, pubkey_file, &pubkey_type);
  if (pubkey_string == NULL) {
    ssh_log(session, SSH_LOG_PACKET,
        "Wasn't able to open public key file %s: %s",
        pubkey_file,
        ssh_get_error(session));
    SAFE_FREE(pubkey_file);
    return -1;
  }

  SAFE_FREE(pubkey_file);

  *publickey = pubkey_string;
  *type = pubkey_type;

  return 0;
}

ssh_string try_publickey_from_file(ssh_session session, struct ssh_keys_struct keytab,
    char **privkeyfile, int *type) {
  const char *priv;
  const char *pub;
  char *new;
  ssh_string pubkey=NULL;

  pub = keytab.publickey;
  if (pub == NULL) {
    return NULL;
  }
  priv = keytab.privatekey;
  if (priv == NULL) {
    return NULL;
  }

  if (session->sshdir == NULL) {
    if (ssh_options_apply(session) < 0) {
      return NULL;
    }
  }

  ssh_log(session, SSH_LOG_PACKET, "Trying to open publickey %s", pub);
  if (!ssh_file_readaccess_ok(pub)) {
    ssh_log(session, SSH_LOG_PACKET, "Failed to open publickey %s", pub);
    goto error;
  }

  ssh_log(session, SSH_LOG_PACKET, "Trying to open privatekey %s", priv);
  if (!ssh_file_readaccess_ok(priv)) {
    ssh_log(session, SSH_LOG_PACKET, "Failed to open privatekey %s", priv);
    goto error;
  }

  ssh_log(session, SSH_LOG_PACKET, "Success opening public and private key");

  /*
   * We are sure both the private and public key file is readable. We return
   * the public as a string, and the private filename as an argument
   */
  pubkey = publickey_from_file(session, pub, type);
  if (pubkey == NULL) {
    ssh_log(session, SSH_LOG_PACKET,
        "Wasn't able to open public key file %s: %s",
        pub,
        ssh_get_error(session));
    goto error;
  }

  new = realloc(*privkeyfile, strlen(priv) + 1);
  if (new == NULL) {
    ssh_string_free(pubkey);
    goto error;
  }

  strcpy(new, priv);
  *privkeyfile = new;
error:
  return pubkey;
}

/** @} */

/* vim: set ts=4 sw=4 et cindent: */

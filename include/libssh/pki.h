/*
 * This file is part of the SSH Library
 *
 * Copyright (c) 2010 by Aris Adamantiadis
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

#ifndef PKI_H_
#define PKI_H_

#define RSA_HEADER_BEGIN "-----BEGIN RSA PRIVATE KEY-----"
#define RSA_HEADER_END "-----END RSA PRIVATE KEY-----"
#define DSA_HEADER_BEGIN "-----BEGIN DSA PRIVATE KEY-----"
#define DSA_HEADER_END "-----END DSA PRIVATE KEY-----"

#define SSH_KEY_FLAG_EMPTY 0
#define SSH_KEY_FLAG_PUBLIC  1
#define SSH_KEY_FLAG_PRIVATE 2

struct ssh_key_struct {
    enum ssh_keytypes_e type;
    int flags;
    const char *type_c; /* Don't free it ! it is static */
#ifdef HAVE_LIBGCRYPT
    gcry_sexp_t dsa;
    gcry_sexp_t rsa;
#elif HAVE_LIBCRYPTO
    DSA *dsa;
    RSA *rsa;
    void *ecdsa;
#endif
    void *cert;
};

void ssh_key_clean (ssh_key key);

ssh_key ssh_pki_publickey_from_privatekey(ssh_key privkey);
ssh_string ssh_pki_do_sign(ssh_session session, ssh_buffer sigbuf,
    ssh_key privatekey);

/* temporary functions, to be removed after migration to ssh_key */
ssh_public_key ssh_pki_convert_key_to_publickey(ssh_key key);
ssh_private_key ssh_pki_convert_key_to_privatekey(ssh_key key);

enum ssh_keytypes_e pki_privatekey_type_from_string(const char *privkey);

ssh_key pki_private_key_from_base64(ssh_session session,
                                    const char *b64_key,
                                    const char *passphrase);
ssh_key pki_publickey_from_privatekey(ssh_key privkey);
int pki_pubkey_build_dss(ssh_key key,
                         ssh_string p,
                         ssh_string q,
                         ssh_string g,
                         ssh_string pubkey);
int pki_pubkey_build_rsa(ssh_key key,
                         ssh_string e,
                         ssh_string n);
struct signature_struct *pki_do_sign(ssh_key privatekey,
                                     const unsigned char *hash);

#endif /* PKI_H_ */

/*
 * This file is part of the SSH Library
 *
 * Copyright (c) 2011 by Aris Adamantiadis
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
#include "libssh/session.h"
#include "libssh/ecdh.h"
#include "libssh/buffer.h"
#include "libssh/ssh2.h"

#ifdef HAVE_ECDH
#include <openssl/ecdh.h>

#define NISTP256 NID_X9_62_prime256v1
#define NISTP384 NID_secp384r1
#define NISTP521 NID_secp521r1

/** @internal
 * @brief Starts ecdh-sha2-nistp256 key exchange
 */
int ssh_client_ecdh_init(ssh_session session){
  ssh_string e = NULL;
  EC_KEY *key=NULL;
  const EC_GROUP *group;
  const EC_POINT *pubkey;
  ssh_string client_pubkey;
  int len;
  int rc;
  bignum_CTX ctx=BN_CTX_new();

  if (buffer_add_u8(session->out_buffer, SSH2_MSG_KEX_ECDH_INIT) < 0) {
    goto error;
  }
  key = EC_KEY_new_by_curve_name(NISTP256);
  group = EC_KEY_get0_group(key);
  EC_KEY_generate_key(key);
  pubkey=EC_KEY_get0_public_key(key);
  len = EC_POINT_point2oct(group,pubkey,POINT_CONVERSION_UNCOMPRESSED,
      NULL,0,ctx);
  client_pubkey=ssh_string_new(len);

  EC_POINT_point2oct(group,pubkey,POINT_CONVERSION_UNCOMPRESSED,
      ssh_string_data(client_pubkey),len,ctx);
  buffer_add_ssh_string(session->out_buffer,client_pubkey);
  BN_CTX_free(ctx);
  session->next_crypto->ecdh_privkey = key;
  session->next_crypto->ecdh_client_pubkey = client_pubkey;
  rc = packet_send(session);
  return rc;
error:
  if(e != NULL){
    ssh_string_burn(e);
    ssh_string_free(e);
  }


  return SSH_ERROR;
}

static void ecdh_import_pubkey(ssh_session session, ssh_string pubkey_string) {
  session->next_crypto->server_pubkey = pubkey_string;
}

static int ecdh_build_k(ssh_session session) {
  const EC_GROUP *group = EC_KEY_get0_group(session->next_crypto->ecdh_privkey);
  EC_POINT *pubkey=EC_POINT_new(group);
  void *buffer;
  int len = (EC_GROUP_get_degree(group) + 7) / 8;
#ifdef HAVE_LIBCRYPTO
  bignum_CTX ctx = bignum_ctx_new();
  if (ctx == NULL) {
    return -1;
  }
#endif

  session->next_crypto->k = bignum_new();
  if (session->next_crypto->k == NULL) {
#ifdef HAVE_LIBCRYPTO
    bignum_ctx_free(ctx);
#endif
    return -1;
  }

  EC_POINT_oct2point(group,pubkey,ssh_string_data(session->next_crypto->ecdh_server_pubkey),
      ssh_string_len(session->next_crypto->ecdh_server_pubkey),ctx);
  buffer = malloc(len);
  ECDH_compute_key(buffer,len,pubkey,session->next_crypto->ecdh_privkey,NULL);
  BN_bin2bn(buffer,len,session->next_crypto->k);
  free(buffer);
  EC_KEY_free(session->next_crypto->ecdh_privkey);
  session->next_crypto->ecdh_privkey=NULL;
#ifdef DEBUG_CRYPTO
  ssh_print_hexa("Session server cookie", session->server_kex.cookie, 16);
  ssh_print_hexa("Session client cookie", session->client_kex.cookie, 16);
  ssh_print_bignum("Shared secret key", session->next_crypto->k);
#endif

#ifdef HAVE_LIBCRYPTO
  bignum_ctx_free(ctx);
#endif

  return 0;
}

/** @internal
 * @brief parses a SSH_MSG_KEX_ECDH_REPLY packet and sends back
 * a SSH_MSG_NEWKEYS
 */
int ssh_client_ecdh_reply(ssh_session session, ssh_buffer packet){
  ssh_string q_s_string = NULL;
  ssh_string pubkey = NULL;
  ssh_string signature = NULL;
  int rc;
  pubkey = buffer_get_ssh_string(packet);
  if (pubkey == NULL){
    ssh_set_error(session,SSH_FATAL, "No public key in packet");
    goto error;
  }
  ecdh_import_pubkey(session, pubkey);

  q_s_string = buffer_get_ssh_string(packet);
  if (q_s_string == NULL) {
    ssh_set_error(session,SSH_FATAL, "No Q_S ECC point in packet");
    goto error;
  }
  session->next_crypto->ecdh_server_pubkey = q_s_string;
  signature = buffer_get_ssh_string(packet);
  if (signature == NULL) {
    ssh_set_error(session, SSH_FATAL, "No signature in packet");
    goto error;
  }
  session->next_crypto->dh_server_signature = signature;
  signature=NULL; /* ownership changed */
  if (ecdh_build_k(session) < 0) {
    ssh_set_error(session, SSH_FATAL, "Cannot build k number");
    goto error;
  }

  /* Send the MSG_NEWKEYS */
  if (buffer_add_u8(session->out_buffer, SSH2_MSG_NEWKEYS) < 0) {
    goto error;
  }

  rc=packet_send(session);
  ssh_log(session, SSH_LOG_PROTOCOL, "SSH_MSG_NEWKEYS sent");
  return rc;
error:
  return SSH_ERROR;
}

#endif /* HAVE_ECDH */

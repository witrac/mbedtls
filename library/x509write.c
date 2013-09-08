/*
 * X509 buffer writing functionality
 *
 *  Copyright (C) 2006-2013, Brainspark B.V.
 *
 *  This file is part of PolarSSL (http://www.polarssl.org)
 *  Lead Maintainer: Paul Bakker <polarssl_maintainer at polarssl.org>
 *
 *  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "polarssl/config.h"

#if defined(POLARSSL_X509_WRITE_C)

#include "polarssl/asn1write.h"
#include "polarssl/x509write.h"
#include "polarssl/x509.h"
#include "polarssl/md.h"
#include "polarssl/oid.h"

#include "polarssl/sha1.h"

#if defined(POLARSSL_BASE64_C)
#include "polarssl/base64.h"
#endif

#if defined(POLARSSL_MEMORY_C)
#include "polarssl/memory.h"
#else
#include <stdlib.h>
#define polarssl_malloc     malloc
#define polarssl_free       free
#endif

static int x509write_string_to_names( x509_req_name **head, char *name )
{
    int ret = 0;
    char *s = name, *c = s;
    char *end = s + strlen( s );
    char *oid = NULL;
    int in_tag = 1;
    x509_req_name *cur;

    while( *head != NULL )
    {
        cur = *head;
        *head = cur->next;
        polarssl_free( cur );
    }

    while( c <= end )
    {
        if( in_tag && *c == '=' )
        {
            if( memcmp( s, "CN", 2 ) == 0 && c - s == 2 )
                oid = OID_AT_CN;
            else if( memcmp( s, "C", 1 ) == 0 && c - s == 1 )
                oid = OID_AT_COUNTRY;
            else if( memcmp( s, "O", 1 ) == 0 && c - s == 1 )
                oid = OID_AT_ORGANIZATION;
            else if( memcmp( s, "L", 1 ) == 0 && c - s == 1 )
                oid = OID_AT_LOCALITY;
            else if( memcmp( s, "R", 1 ) == 0 && c - s == 1 )
                oid = OID_PKCS9_EMAIL;
            else if( memcmp( s, "OU", 2 ) == 0 && c - s == 2 )
                oid = OID_AT_ORG_UNIT;
            else if( memcmp( s, "ST", 2 ) == 0 && c - s == 2 )
                oid = OID_AT_STATE;
            else
            {
                ret = POLARSSL_ERR_X509WRITE_UNKNOWN_OID;
                goto exit;
            }

            s = c + 1;
            in_tag = 0;
        }

        if( !in_tag && ( *c == ',' || c == end ) )
        {
            if( c - s > 127 )
            {
                ret = POLARSSL_ERR_X509WRITE_BAD_INPUT_DATA;
                goto exit;
            }

            cur = polarssl_malloc( sizeof(x509_req_name) );

            if( cur == NULL )
            {
                ret = POLARSSL_ERR_X509WRITE_MALLOC_FAILED;
                goto exit;
            }

            memset( cur, 0, sizeof(x509_req_name) );

            cur->next = *head;
            *head = cur;

            strncpy( cur->oid, oid, strlen( oid ) );
            strncpy( cur->name, s, c - s );

            s = c + 1;
            in_tag = 1;
        }
        c++;
    }

exit:

    return( ret );
}

/*
 *  RSAPublicKey ::= SEQUENCE {
 *      modulus           INTEGER,  -- n
 *      publicExponent    INTEGER   -- e
 *  }
 */
static int x509_write_rsa_pubkey( unsigned char **p, unsigned char *start,
                                  rsa_context *rsa )
{
    int ret;
    size_t len = 0;

    ASN1_CHK_ADD( len, asn1_write_mpi( p, start, &rsa->E ) );
    ASN1_CHK_ADD( len, asn1_write_mpi( p, start, &rsa->N ) );

    ASN1_CHK_ADD( len, asn1_write_len( p, start, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( p, start, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    return( len );
}

void x509write_csr_init( x509_csr *ctx )
{
    memset( ctx, 0, sizeof(x509_csr) );
}

void x509write_csr_free( x509_csr *ctx )
{
    x509_req_name *cur;
    asn1_named_data *cur_ext;

    while( ( cur = ctx->subject ) != NULL )
    {
        ctx->subject = cur->next;
        polarssl_free( cur );
    }

    while( ( cur_ext = ctx->extensions ) != NULL )
    {
        ctx->extensions = cur_ext->next;
        asn1_free_named_data( cur_ext );
        polarssl_free( cur_ext );
    }

    memset( ctx, 0, sizeof(x509_csr) );
}

void x509write_csr_set_md_alg( x509_csr *ctx, md_type_t md_alg )
{
    ctx->md_alg = md_alg;
}

void x509write_csr_set_rsa_key( x509_csr *ctx, rsa_context *rsa )
{
    ctx->rsa = rsa;
}

int x509write_csr_set_subject_name( x509_csr *ctx, char *subject_name )
{
    return x509write_string_to_names( &ctx->subject, subject_name );
}

/* The first byte of the value in the asn1_named_data structure is reserved
 * to store the critical boolean for us
 */
static int x509_set_extension( asn1_named_data **head,
                               const char *oid, size_t oid_len,
                               int critical,
                               const unsigned char *val, size_t val_len )
{
    asn1_named_data *cur;

    if( ( cur = asn1_find_named_data( *head, oid, oid_len ) ) == NULL )
    {
        cur = polarssl_malloc( sizeof(asn1_named_data) );
        if( cur == NULL )
            return( POLARSSL_ERR_X509WRITE_MALLOC_FAILED );

        memset( cur, 0, sizeof(asn1_named_data) );

        cur->oid.len = oid_len;
        cur->oid.p = polarssl_malloc( oid_len );
        if( cur->oid.p == NULL )
        {
            polarssl_free( cur );
            return( POLARSSL_ERR_X509WRITE_MALLOC_FAILED );
        }

        cur->val.len = val_len + 1;
        cur->val.p = polarssl_malloc( val_len + 1 );
        if( cur->val.p == NULL )
        {
            polarssl_free( cur->oid.p );
            polarssl_free( cur );
            return( POLARSSL_ERR_X509WRITE_MALLOC_FAILED );
        }

        memcpy( cur->oid.p, oid, oid_len );

        cur->next = *head;
        *head = cur;
    }

    if( cur->val.len != val_len + 1 )
    {
        polarssl_free( cur->val.p );

        cur->val.len = val_len + 1;
        cur->val.p = polarssl_malloc( val_len + 1);
        if( cur->val.p == NULL )
        {
            polarssl_free( cur->oid.p );
            polarssl_free( cur );
            return( POLARSSL_ERR_X509WRITE_MALLOC_FAILED );
        }
    }

    cur->val.p[0] = critical;
    memcpy( cur->val.p + 1, val, val_len );

    return( 0 );
}

int x509write_csr_set_extension( x509_csr *ctx,
                                 const char *oid, size_t oid_len,
                                 const unsigned char *val, size_t val_len )
{
    return x509_set_extension( &ctx->extensions, oid, oid_len,
                               0, val, val_len );
}

int x509write_csr_set_key_usage( x509_csr *ctx, unsigned char key_usage )
{
    unsigned char buf[4];
    unsigned char *c;
    int ret;

    c = buf + 4;

    if( ( ret = asn1_write_bitstring( &c, buf, &key_usage, 7 ) ) != 4 )
        return( ret );

    ret = x509write_csr_set_extension( ctx, OID_KEY_USAGE,
                                       OID_SIZE( OID_KEY_USAGE ),
                                       buf, 4 );
    if( ret != 0 )
        return( ret );

    return( 0 );
}

int x509write_csr_set_ns_cert_type( x509_csr *ctx, unsigned char ns_cert_type )
{
    unsigned char buf[4];
    unsigned char *c;
    int ret;

    c = buf + 4;

    if( ( ret = asn1_write_bitstring( &c, buf, &ns_cert_type, 8 ) ) != 4 )
        return( ret );

    ret = x509write_csr_set_extension( ctx, OID_NS_CERT_TYPE,
                                       OID_SIZE( OID_NS_CERT_TYPE ),
                                       buf, 4 );
    if( ret != 0 )
        return( ret );

    return( 0 );
}

void x509write_crt_init( x509write_cert *ctx )
{
    memset( ctx, 0, sizeof(x509write_cert) );

    mpi_init( &ctx->serial );
    ctx->version = X509_CRT_VERSION_3;
}

void x509write_crt_free( x509write_cert *ctx )
{
    x509_req_name *cur;
    asn1_named_data *cur_ext;

    mpi_free( &ctx->serial );

    while( ( cur = ctx->subject ) != NULL )
    {
        ctx->subject = cur->next;
        polarssl_free( cur );
    }

    while( ( cur = ctx->issuer ) != NULL )
    {
        ctx->issuer = cur->next;
        polarssl_free( cur );
    }

    while( ( cur_ext = ctx->extensions ) != NULL )
    {
        ctx->extensions = cur_ext->next;
        asn1_free_named_data( cur_ext );
        polarssl_free( cur_ext );
    }

    memset( ctx, 0, sizeof(x509_csr) );
}

void x509write_crt_set_md_alg( x509write_cert *ctx, md_type_t md_alg )
{
    ctx->md_alg = md_alg;
}

void x509write_crt_set_subject_key( x509write_cert *ctx, rsa_context *rsa )
{
    ctx->subject_key = rsa;
}

void x509write_crt_set_issuer_key( x509write_cert *ctx, rsa_context *rsa )
{
    ctx->issuer_key = rsa;
}

int x509write_crt_set_subject_name( x509write_cert *ctx, char *subject_name )
{
    return x509write_string_to_names( &ctx->subject, subject_name );
}

int x509write_crt_set_issuer_name( x509write_cert *ctx, char *issuer_name )
{
    return x509write_string_to_names( &ctx->issuer, issuer_name );
}

int x509write_crt_set_serial( x509write_cert *ctx, const mpi *serial )
{
    int ret;

    if( ( ret = mpi_copy( &ctx->serial, serial ) ) != 0 )
        return( ret );

    return( 0 );
}

int x509write_crt_set_validity( x509write_cert *ctx, char *not_before,
                                char *not_after )
{
    if( strlen(not_before) != X509_RFC5280_UTC_TIME_LEN - 1 ||
        strlen(not_after)  != X509_RFC5280_UTC_TIME_LEN - 1 )
    {
        return( POLARSSL_ERR_X509WRITE_BAD_INPUT_DATA );
    }
    strncpy( ctx->not_before, not_before, X509_RFC5280_UTC_TIME_LEN );
    strncpy( ctx->not_after , not_after , X509_RFC5280_UTC_TIME_LEN );
    ctx->not_before[X509_RFC5280_UTC_TIME_LEN - 1] = 'Z';
    ctx->not_after[X509_RFC5280_UTC_TIME_LEN - 1] = 'Z';

    return( 0 );
}

int x509write_crt_set_extension( x509write_cert *ctx,
                                 const char *oid, size_t oid_len,
                                 int critical,
                                 const unsigned char *val, size_t val_len )
{
    return x509_set_extension( &ctx->extensions, oid, oid_len,
                               critical, val, val_len );
}

int x509write_crt_set_basic_constraints( x509write_cert *ctx,
                                         int is_ca, int max_pathlen )
{
    int ret;
    unsigned char buf[9];
    unsigned char *c = buf + sizeof(buf);
    size_t len = 0;

    memset( buf, 0, sizeof(buf) );

    if( is_ca && max_pathlen > 127 )
        return( POLARSSL_ERR_X509WRITE_BAD_INPUT_DATA );

    if( is_ca )
    {
        if( max_pathlen >= 0 )
        {
            ASN1_CHK_ADD( len, asn1_write_int( &c, buf, max_pathlen ) );
        }
        ASN1_CHK_ADD( len, asn1_write_bool( &c, buf, 1 ) );
    }

    ASN1_CHK_ADD( len, asn1_write_len( &c, buf, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    return x509write_crt_set_extension( ctx, OID_BASIC_CONSTRAINTS,
                                        OID_SIZE( OID_BASIC_CONSTRAINTS ),
                                        0, buf + sizeof(buf) - len, len );
}

int x509write_crt_set_subject_key_identifier( x509write_cert *ctx )
{
    int ret;
    unsigned char buf[POLARSSL_MPI_MAX_SIZE * 2 + 20]; /* tag, length + 2xMPI */
    unsigned char *c = buf + sizeof(buf);
    size_t len = 0;

    memset( buf, 0, sizeof(buf));
    ASN1_CHK_ADD( len, x509_write_rsa_pubkey( &c, buf, ctx->subject_key ) );

    sha1( buf + sizeof(buf) - len, len, buf + sizeof(buf) - 20 );
    c = buf + sizeof(buf) - 20;
    len = 20;

    ASN1_CHK_ADD( len, asn1_write_len( &c, buf, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, buf, ASN1_OCTET_STRING ) );

    return x509write_crt_set_extension( ctx, OID_SUBJECT_KEY_IDENTIFIER,
                                        OID_SIZE( OID_SUBJECT_KEY_IDENTIFIER ),
                                        0, buf + sizeof(buf) - len, len );
}

int x509write_crt_set_authority_key_identifier( x509write_cert *ctx )
{
    int ret;
    unsigned char buf[POLARSSL_MPI_MAX_SIZE * 2 + 20]; /* tag, length + 2xMPI */
    unsigned char *c = buf + sizeof(buf);
    size_t len = 0;

    memset( buf, 0, sizeof(buf));
    ASN1_CHK_ADD( len, x509_write_rsa_pubkey( &c, buf, ctx->issuer_key ) );

    sha1( buf + sizeof(buf) - len, len, buf + sizeof(buf) - 20 );
    c = buf + sizeof(buf) - 20;
    len = 20;

    ASN1_CHK_ADD( len, asn1_write_len( &c, buf, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, buf, ASN1_CONTEXT_SPECIFIC | 0 ) );

    ASN1_CHK_ADD( len, asn1_write_len( &c, buf, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    return x509write_crt_set_extension( ctx, OID_AUTHORITY_KEY_IDENTIFIER,
                                   OID_SIZE( OID_AUTHORITY_KEY_IDENTIFIER ),
                                   0, buf + sizeof(buf) - len, len );
}

int x509write_pubkey_der( rsa_context *rsa, unsigned char *buf, size_t size )
{
    int ret;
    unsigned char *c;
    size_t len = 0;

    c = buf + size - 1;

    ASN1_CHK_ADD( len, x509_write_rsa_pubkey( &c, buf, rsa ) );

    if( c - buf < 1 )
        return( POLARSSL_ERR_ASN1_BUF_TOO_SMALL );

    /*
     *  SubjectPublicKeyInfo  ::=  SEQUENCE  {
     *       algorithm            AlgorithmIdentifier,
     *       subjectPublicKey     BIT STRING }
     */
    *--c = 0;
    len += 1;

    ASN1_CHK_ADD( len, asn1_write_len( &c, buf, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, buf, ASN1_BIT_STRING ) );

    ASN1_CHK_ADD( len, asn1_write_algorithm_identifier( &c, buf, OID_PKCS1_RSA ) );

    ASN1_CHK_ADD( len, asn1_write_len( &c, buf, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    return( len );
}

int x509write_key_der( rsa_context *rsa, unsigned char *buf, size_t size )
{
    int ret;
    unsigned char *c;
    size_t len = 0;

    c = buf + size - 1;

    ASN1_CHK_ADD( len, asn1_write_mpi( &c, buf, &rsa->QP ) );
    ASN1_CHK_ADD( len, asn1_write_mpi( &c, buf, &rsa->DQ ) );
    ASN1_CHK_ADD( len, asn1_write_mpi( &c, buf, &rsa->DP ) );
    ASN1_CHK_ADD( len, asn1_write_mpi( &c, buf, &rsa->Q ) );
    ASN1_CHK_ADD( len, asn1_write_mpi( &c, buf, &rsa->P ) );
    ASN1_CHK_ADD( len, asn1_write_mpi( &c, buf, &rsa->D ) );
    ASN1_CHK_ADD( len, asn1_write_mpi( &c, buf, &rsa->E ) );
    ASN1_CHK_ADD( len, asn1_write_mpi( &c, buf, &rsa->N ) );
    ASN1_CHK_ADD( len, asn1_write_int( &c, buf, 0 ) );

    ASN1_CHK_ADD( len, asn1_write_len( &c, buf, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    // TODO: Make NON RSA Specific variant later on
/*    *--c = 0;
    len += 1;

    len += asn1_write_len( &c, len);
    len += asn1_write_tag( &c, ASN1_BIT_STRING );

    len += asn1_write_oid( &c, OID_PKCS1_RSA );

    len += asn1_write_int( &c, 0 );

    len += asn1_write_len( &c, len);
    len += asn1_write_tag( &c, ASN1_CONSTRUCTED | ASN1_SEQUENCE );*/

/*    for(i = 0; i < len; ++i)
    {
        if (i % 16 == 0 ) printf("\n");
        printf("%02x ", c[i]);
    }
    printf("\n");*/

    return( len );
}

/*
 *  RelativeDistinguishedName ::=
 *    SET OF AttributeTypeAndValue
 *
 *  AttributeTypeAndValue ::= SEQUENCE {
 *    type     AttributeType,
 *    value    AttributeValue }
 *
 *  AttributeType ::= OBJECT IDENTIFIER
 *
 *  AttributeValue ::= ANY DEFINED BY AttributeType
 */
static int x509_write_name( unsigned char **p, unsigned char *start, char *oid,
                            char *name )
{
    int ret;
    size_t string_len = 0;
    size_t oid_len = 0;
    size_t len = 0;

    // Write PrintableString for all except OID_PKCS9_EMAIL
    //
    if( OID_SIZE( OID_PKCS9_EMAIL ) == strlen( oid ) &&
        memcmp( oid, OID_PKCS9_EMAIL, strlen( oid ) ) == 0 )
    {
        ASN1_CHK_ADD( string_len, asn1_write_ia5_string( p, start, name ) );
    }
    else
        ASN1_CHK_ADD( string_len, asn1_write_printable_string( p, start, name ) );

    // Write OID
    //
    ASN1_CHK_ADD( oid_len, asn1_write_oid( p, start, oid ) );

    len = oid_len + string_len;
    ASN1_CHK_ADD( len, asn1_write_len( p, start, oid_len + string_len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( p, start, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    ASN1_CHK_ADD( len, asn1_write_len( p, start, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( p, start, ASN1_CONSTRUCTED | ASN1_SET ) );

    return( len );
}

static int x509_write_names( unsigned char **p, unsigned char *start,
                             x509_req_name *first )
{
    int ret;
    size_t len = 0;
    x509_req_name *cur = first;

    while( cur != NULL )
    {
        ASN1_CHK_ADD( len, x509_write_name( p, start, cur->oid, cur->name ) );
        cur = cur->next;
    }

    ASN1_CHK_ADD( len, asn1_write_len( p, start, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( p, start, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    return( len );
}

static int x509_write_sig( unsigned char **p, unsigned char *start,
                           const char *oid, unsigned char *sig, size_t size )
{
    int ret;
    size_t len = 0;

    if( *p - start < (int) size + 1 )
        return( POLARSSL_ERR_ASN1_BUF_TOO_SMALL );

    len = size;
    (*p) -= len;
    memcpy( *p, sig, len );

    *--(*p) = 0;
    len += 1;

    ASN1_CHK_ADD( len, asn1_write_len( p, start, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( p, start, ASN1_BIT_STRING ) );

    // Write OID
    //
    ASN1_CHK_ADD( len, asn1_write_algorithm_identifier( p, start, oid ) );

    return( len );
}

static int x509_write_time( unsigned char **p, unsigned char *start,
                            const char *time, size_t size )
{
    int ret;
    size_t len = 0;

    /*
     * write ASN1_UTC_TIME if year < 2050 (2 bytes shorter)
     */
    if( time[0] == '2' && time[1] == '0' && time [2] < '5' )
    {
        ASN1_CHK_ADD( len, asn1_write_raw_buffer( p, start,
                                             (const unsigned char *) time + 2,
                                             size - 2 ) );
        ASN1_CHK_ADD( len, asn1_write_len( p, start, len ) );
        ASN1_CHK_ADD( len, asn1_write_tag( p, start, ASN1_UTC_TIME ) );
    }
    else
    {
        ASN1_CHK_ADD( len, asn1_write_raw_buffer( p, start,
                                                  (const unsigned char *) time,
                                                  size ) );
        ASN1_CHK_ADD( len, asn1_write_len( p, start, len ) );
        ASN1_CHK_ADD( len, asn1_write_tag( p, start, ASN1_GENERALIZED_TIME ) );
    }

    return( len );
}

static int x509_write_extension( unsigned char **p, unsigned char *start,
                                 asn1_named_data *ext )
{
    int ret;
    size_t len = 0;

    ASN1_CHK_ADD( len, asn1_write_raw_buffer( p, start, ext->val.p + 1,
                                              ext->val.len - 1 ) );
    ASN1_CHK_ADD( len, asn1_write_len( p, start, ext->val.len - 1 ) );
    ASN1_CHK_ADD( len, asn1_write_tag( p, start, ASN1_OCTET_STRING ) );

    if( ext->val.p[0] != 0 )
    {
        ASN1_CHK_ADD( len, asn1_write_bool( p, start, 1 ) );
    }

    ASN1_CHK_ADD( len, asn1_write_raw_buffer( p, start, ext->oid.p,
                                              ext->oid.len ) );
    ASN1_CHK_ADD( len, asn1_write_len( p, start, ext->oid.len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( p, start, ASN1_OID ) );

    ASN1_CHK_ADD( len, asn1_write_len( p, start, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( p, start, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    return( len );
}

/*
 * Extension  ::=  SEQUENCE  {
 *     extnID      OBJECT IDENTIFIER,
 *     critical    BOOLEAN DEFAULT FALSE,
 *     extnValue   OCTET STRING
 *                 -- contains the DER encoding of an ASN.1 value
 *                 -- corresponding to the extension type identified
 *                 -- by extnID
 *     }
 */
static int x509_write_extensions( unsigned char **p, unsigned char *start,
                                 asn1_named_data *first )
{
    int ret;
    size_t len = 0;
    asn1_named_data *cur_ext = first;

    while( cur_ext != NULL )
    {
        ASN1_CHK_ADD( len, x509_write_extension( p, start, cur_ext ) );
        cur_ext = cur_ext->next;
    }

    return( len );
}

int x509write_csr_der( x509_csr *ctx, unsigned char *buf, size_t size )
{
    int ret;
    const char *sig_oid;
    unsigned char *c, *c2;
    unsigned char hash[64];
    unsigned char sig[POLARSSL_MPI_MAX_SIZE];
    unsigned char tmp_buf[2048];
    size_t pub_len = 0, sig_len = 0;
    size_t len = 0;

    c = tmp_buf + 2048 - 1;

    ASN1_CHK_ADD( len, x509_write_extensions( &c, tmp_buf, ctx->extensions ) );

    if( len )
    {
        ASN1_CHK_ADD( len, asn1_write_len( &c, tmp_buf, len ) );
        ASN1_CHK_ADD( len, asn1_write_tag( &c, tmp_buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

        ASN1_CHK_ADD( len, asn1_write_len( &c, tmp_buf, len ) );
        ASN1_CHK_ADD( len, asn1_write_tag( &c, tmp_buf, ASN1_CONSTRUCTED | ASN1_SET ) );

        ASN1_CHK_ADD( len, asn1_write_oid( &c, tmp_buf, OID_PKCS9_CSR_EXT_REQ ) );

        ASN1_CHK_ADD( len, asn1_write_len( &c, tmp_buf, len ) );
        ASN1_CHK_ADD( len, asn1_write_tag( &c, tmp_buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );
    }

    ASN1_CHK_ADD( len, asn1_write_len( &c, tmp_buf, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, tmp_buf, ASN1_CONSTRUCTED | ASN1_CONTEXT_SPECIFIC ) );

    ASN1_CHK_ADD( pub_len, asn1_write_mpi( &c, tmp_buf, &ctx->rsa->E ) );
    ASN1_CHK_ADD( pub_len, asn1_write_mpi( &c, tmp_buf, &ctx->rsa->N ) );

    ASN1_CHK_ADD( pub_len, asn1_write_len( &c, tmp_buf, pub_len ) );
    ASN1_CHK_ADD( pub_len, asn1_write_tag( &c, tmp_buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    if( c - tmp_buf < 1 )
        return( POLARSSL_ERR_ASN1_BUF_TOO_SMALL );

    /*
     *  AlgorithmIdentifier  ::=  SEQUENCE  {
     *       algorithm               OBJECT IDENTIFIER,
     *       parameters              ANY DEFINED BY algorithm OPTIONAL  }
     */
    *--c = 0;
    pub_len += 1;

    ASN1_CHK_ADD( pub_len, asn1_write_len( &c, tmp_buf, pub_len ) );
    ASN1_CHK_ADD( pub_len, asn1_write_tag( &c, tmp_buf, ASN1_BIT_STRING ) );

    ASN1_CHK_ADD( pub_len, asn1_write_algorithm_identifier( &c, tmp_buf, OID_PKCS1_RSA ) );

    len += pub_len;
    ASN1_CHK_ADD( len, asn1_write_len( &c, tmp_buf, pub_len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, tmp_buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    /*
     *  Subject  ::=  Name
     */
    ASN1_CHK_ADD( len, x509_write_names( &c, tmp_buf, ctx->subject ) );

    /*
     *  Version  ::=  INTEGER  {  v1(0), v2(1), v3(2)  }
     */
    ASN1_CHK_ADD( len, asn1_write_int( &c, tmp_buf, 0 ) );

    ASN1_CHK_ADD( len, asn1_write_len( &c, tmp_buf, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, tmp_buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    md( md_info_from_type( ctx->md_alg ), c, len, hash );

    rsa_pkcs1_sign( ctx->rsa, NULL, NULL, RSA_PRIVATE, ctx->md_alg, 0, hash, sig );

    // Generate correct OID
    //
    ret = oid_get_oid_by_sig_alg( POLARSSL_PK_RSA, ctx->md_alg, &sig_oid );

    c2 = buf + size - 1;
    ASN1_CHK_ADD( sig_len, x509_write_sig( &c2, buf, sig_oid, sig, ctx->rsa->len ) );

    c2 -= len;
    memcpy( c2, c, len );

    len += sig_len;
    ASN1_CHK_ADD( len, asn1_write_len( &c2, buf, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c2, buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    return( len );
}

int x509write_crt_der( x509write_cert *ctx, unsigned char *buf, size_t size )
{
    int ret;
    const char *sig_oid;
    unsigned char *c, *c2;
    unsigned char hash[64];
    unsigned char sig[POLARSSL_MPI_MAX_SIZE];
    unsigned char tmp_buf[2048];
    size_t sub_len = 0, pub_len = 0, sig_len = 0;
    size_t len = 0;

    c = tmp_buf + 2048 - 1;

    // Generate correct OID
    //
    ret = oid_get_oid_by_sig_alg( POLARSSL_PK_RSA, ctx->md_alg, &sig_oid );
    if( ret != 0 )
        return( ret );

    /*
     *  Extensions  ::=  SEQUENCE SIZE (1..MAX) OF Extension
     */
    ASN1_CHK_ADD( len, x509_write_extensions( &c, tmp_buf, ctx->extensions ) );
    ASN1_CHK_ADD( len, asn1_write_len( &c, tmp_buf, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, tmp_buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );
    ASN1_CHK_ADD( len, asn1_write_len( &c, tmp_buf, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, tmp_buf, ASN1_CONTEXT_SPECIFIC | ASN1_CONSTRUCTED | 3 ) );

    /*
     *  SubjectPublicKeyInfo  ::=  SEQUENCE  {
     *       algorithm            AlgorithmIdentifier,
     *       subjectPublicKey     BIT STRING  }
     */
    ASN1_CHK_ADD( pub_len, asn1_write_mpi( &c, tmp_buf, &ctx->subject_key->E ) );
    ASN1_CHK_ADD( pub_len, asn1_write_mpi( &c, tmp_buf, &ctx->subject_key->N ) );

    ASN1_CHK_ADD( pub_len, asn1_write_len( &c, tmp_buf, pub_len ) );
    ASN1_CHK_ADD( pub_len, asn1_write_tag( &c, tmp_buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    if( c - tmp_buf < 1 )
        return( POLARSSL_ERR_ASN1_BUF_TOO_SMALL );

    *--c = 0;
    pub_len += 1;

    ASN1_CHK_ADD( pub_len, asn1_write_len( &c, tmp_buf, pub_len ) );
    ASN1_CHK_ADD( pub_len, asn1_write_tag( &c, tmp_buf, ASN1_BIT_STRING ) );

    ASN1_CHK_ADD( pub_len, asn1_write_algorithm_identifier( &c, tmp_buf, OID_PKCS1_RSA ) );

    len += pub_len;
    ASN1_CHK_ADD( len, asn1_write_len( &c, tmp_buf, pub_len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, tmp_buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    /*
     *  Subject  ::=  Name
     */
    ASN1_CHK_ADD( len, x509_write_names( &c, tmp_buf, ctx->subject ) );

    /*
     *  Validity ::= SEQUENCE {
     *       notBefore      Time,
     *       notAfter       Time }
     */
    sub_len = 0;

    ASN1_CHK_ADD( sub_len, x509_write_time( &c, tmp_buf, ctx->not_after,
                                            X509_RFC5280_UTC_TIME_LEN ) );

    ASN1_CHK_ADD( sub_len, x509_write_time( &c, tmp_buf, ctx->not_before,
                                            X509_RFC5280_UTC_TIME_LEN ) );

    len += sub_len;
    ASN1_CHK_ADD( len, asn1_write_len( &c, tmp_buf, sub_len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, tmp_buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    /*
     *  Issuer  ::=  Name
     */
    ASN1_CHK_ADD( len, x509_write_names( &c, tmp_buf, ctx->issuer ) );

    /*
     *  Signature   ::=  AlgorithmIdentifier
     */
    ASN1_CHK_ADD( len, asn1_write_algorithm_identifier( &c, tmp_buf,
                                                        sig_oid ) );

    /*
     *  Serial   ::=  INTEGER
     */
    ASN1_CHK_ADD( len, asn1_write_mpi( &c, tmp_buf, &ctx->serial ) );

    /*
     *  Version  ::=  INTEGER  {  v1(0), v2(1), v3(2)  }
     */
    sub_len = 0;
    ASN1_CHK_ADD( sub_len, asn1_write_int( &c, tmp_buf, ctx->version ) );
    len += sub_len;
    ASN1_CHK_ADD( len, asn1_write_len( &c, tmp_buf, sub_len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, tmp_buf, ASN1_CONTEXT_SPECIFIC | ASN1_CONSTRUCTED | 0 ) );

    ASN1_CHK_ADD( len, asn1_write_len( &c, tmp_buf, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c, tmp_buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    md( md_info_from_type( ctx->md_alg ), c, len, hash );

    rsa_pkcs1_sign( ctx->issuer_key, NULL, NULL, RSA_PRIVATE, ctx->md_alg, 0, hash, sig );

    c2 = buf + size - 1;
    ASN1_CHK_ADD( sig_len, x509_write_sig( &c2, buf, sig_oid, sig, ctx->issuer_key->len ) );

    c2 -= len;
    memcpy( c2, c, len );

    len += sig_len;
    ASN1_CHK_ADD( len, asn1_write_len( &c2, buf, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &c2, buf, ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    return( len );
}

#define PEM_BEGIN_CRT           "-----BEGIN CERTIFICATE-----\n"
#define PEM_END_CRT             "-----END CERTIFICATE-----\n"

#define PEM_BEGIN_CSR           "-----BEGIN CERTIFICATE REQUEST-----\n"
#define PEM_END_CSR             "-----END CERTIFICATE REQUEST-----\n"

#define PEM_BEGIN_PUBLIC_KEY    "-----BEGIN PUBLIC KEY-----\n"
#define PEM_END_PUBLIC_KEY      "-----END PUBLIC KEY-----\n"

#define PEM_BEGIN_PRIVATE_KEY   "-----BEGIN RSA PRIVATE KEY-----\n"
#define PEM_END_PRIVATE_KEY     "-----END RSA PRIVATE KEY-----\n"

#if defined(POLARSSL_BASE64_C)
static int x509write_pemify( const char *begin_str, const char *end_str,
                             const unsigned char *der_data, size_t der_len,
                             unsigned char *buf, size_t size )
{
    int ret;
    unsigned char base_buf[4096];
    unsigned char *c = base_buf, *p = buf;
    size_t len = 0, olen = sizeof(base_buf);

    if( ( ret = base64_encode( base_buf, &olen, der_data, der_len ) ) != 0 )
        return( ret );

    if( olen + strlen( begin_str ) + strlen( end_str ) +
        olen / 64 > size )
    {
        return( POLARSSL_ERR_BASE64_BUFFER_TOO_SMALL );
    }

    memcpy( p, begin_str, strlen( begin_str ) );
    p += strlen( begin_str );

    while( olen )
    {
        len = ( olen > 64 ) ? 64 : olen;
        memcpy( p, c, len );
        olen -= len;
        p += len;
        c += len;
        *p++ = '\n';
    }

    memcpy( p, end_str, strlen( end_str ) );
    p += strlen( end_str );

    *p = '\0';

    return( 0 );
}

int x509write_crt_pem( x509write_cert *crt, unsigned char *buf, size_t size )
{
    int ret;
    unsigned char output_buf[4096];

    if( ( ret = x509write_crt_der( crt, output_buf,
                                   sizeof(output_buf) ) ) < 0 )
    {
        return( ret );
    }

    if( ( ret = x509write_pemify( PEM_BEGIN_CRT, PEM_END_CRT,
                                  output_buf + sizeof(output_buf) - 1 - ret,
                                  ret, buf, size ) ) != 0 )
    {
        return( ret );
    }

    return( 0 );
}

int x509write_pubkey_pem( rsa_context *rsa, unsigned char *buf, size_t size )
{
    int ret;
    unsigned char output_buf[4096];

    if( ( ret = x509write_pubkey_der( rsa, output_buf,
                                      sizeof(output_buf) ) ) < 0 )
    {
        return( ret );
    }

    if( ( ret = x509write_pemify( PEM_BEGIN_PUBLIC_KEY, PEM_END_PUBLIC_KEY,
                                  output_buf + sizeof(output_buf) - 1 - ret,
                                  ret, buf, size ) ) != 0 )
    {
        return( ret );
    }

    return( 0 );
}

int x509write_key_pem( rsa_context *rsa, unsigned char *buf, size_t size )
{
    int ret;
    unsigned char output_buf[4096];

    if( ( ret = x509write_key_der( rsa, output_buf,
                                      sizeof(output_buf) ) ) < 0 )
    {
        return( ret );
    }

    if( ( ret = x509write_pemify( PEM_BEGIN_PRIVATE_KEY, PEM_END_PRIVATE_KEY,
                                  output_buf + sizeof(output_buf) - 1 - ret,
                                  ret, buf, size ) ) != 0 )
    {
        return( ret );
    }

    return( 0 );
}

int x509write_csr_pem( x509_csr *ctx, unsigned char *buf, size_t size )
{
    int ret;
    unsigned char output_buf[4096];

    if( ( ret = x509write_csr_der( ctx, output_buf,
                                      sizeof(output_buf) ) ) < 0 )
    {
        return( ret );
    }

    if( ( ret = x509write_pemify( PEM_BEGIN_CSR, PEM_END_CSR,
                                  output_buf + sizeof(output_buf) - 1 - ret,
                                  ret, buf, size ) ) != 0 )
    {
        return( ret );
    }

    return( 0 );
}
#endif /* POLARSSL_BASE64_C */

#endif

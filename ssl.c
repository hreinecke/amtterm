/*
 *  SSL helper functions.
 *
 *  Copyright (C) 2014 Andreas Steinmetz <ast@domdv.de>
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

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(USE_OPENSSL)
#include <openssl/ssl.h>
#include <openssl/err.h>
#elif defined(USE_GNUTLS)
#include <gnutls/gnutls.h>
#endif

#include "ssl.h"

struct ctx
{
	int fd;
#if defined(USE_OPENSSL)
	SSL *ssl;
	SSL_CTX *ctx;
#elif defined(USE_GNUTLS)
	gnutls_session_t ssl;
	gnutls_certificate_credentials_t cred;
#endif
};

static struct ctx *newctx(int fd)
{
	struct ctx *ctx;

	if(!(ctx=malloc(sizeof(struct ctx))))
	{
		perror("malloc");
		return NULL;
	}

	memset(ctx,0,sizeof(struct ctx));

	ctx->fd=fd;

	return ctx;
}

#if defined(USE_OPENSSL)

struct ctx *sslinit(int fd,char *cacert,int untrusted)
{
	int r;
	int c=0;
	struct ctx *ctx;

	if(!(ctx=newctx(fd)))return NULL;

	if(!cacert && !untrusted)return ctx;

	SSL_load_error_strings();
	SSL_library_init();

       if(!(ctx->ctx=SSL_CTX_new(TLSv1_2_client_method())))
	{
		ERR_print_errors_fp(stderr);
		goto err1;
	}

#if OPENSSL_VERSION_NUMBER >= 0x1000100FL
	SSL_CTX_set_options(ctx->ctx,
               SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION);
#endif

	if (cacert)
	{
		if(!SSL_CTX_load_verify_locations(ctx->ctx,cacert,NULL))
		{
			ERR_print_errors_fp(stderr);
			goto err2;
		}
	}

	SSL_CTX_set_verify_depth(ctx->ctx,5);
	if (untrusted)
		SSL_CTX_set_verify(ctx->ctx,SSL_VERIFY_NONE,NULL);
	else
		SSL_CTX_set_verify(ctx->ctx,SSL_VERIFY_PEER,NULL);

	if(!(ctx->ssl=SSL_new(ctx->ctx)))
	{
		ERR_print_errors_fp(stderr);
		goto err2;
	}

	if(!SSL_set_fd(ctx->ssl,ctx->fd))
	{
		ERR_print_errors_fp(stderr);
		goto err3;
	}

repeat:	if((r=SSL_connect(ctx->ssl))!=1)
	{
		switch(SSL_get_error(ctx->ssl,r))
		{
		case SSL_ERROR_WANT_READ:
		case SSL_ERROR_WANT_WRITE:
			if(++c<100)
			{
				usleep(10000);
				goto repeat;
			}
		}
		ERR_print_errors_fp(stderr);
		goto err3;
	}

	return ctx;

err3:	SSL_free(ctx->ssl);
err2:	SSL_CTX_free(ctx->ctx);
err1:	free(ctx);
	return NULL;
}

void sslexit(struct ctx *ctx)
{
	if(ctx->ssl)
	{
		SSL_shutdown(ctx->ssl);
		SSL_free(ctx->ssl);
		SSL_CTX_free(ctx->ctx);
	}
	free(ctx);
}

int sslready(struct ctx *ctx)
{
	if(ctx->ssl)return SSL_pending(ctx->ssl);
	else return 0;
}

ssize_t sslread(struct ctx *ctx,void *buf,size_t count)
{
	int l;
	int c=0;

	if(!ctx->ssl)return read(ctx->fd,buf,count);
	if(!count)return 0;

repeat:	if((l=SSL_read(ctx->ssl,buf,count))>0)return l;

	switch(SSL_get_error(ctx->ssl,l))
	{
	case SSL_ERROR_WANT_READ:
	case SSL_ERROR_WANT_WRITE:
		if(++c<100)
		{
			usleep(10000);
			goto repeat;
		}
		break;
	case SSL_ERROR_WANT_X509_LOOKUP:
		return -1;
	case SSL_ERROR_ZERO_RETURN:
		return 0;
	case SSL_ERROR_SSL:
		ERR_print_errors_fp(stderr);
	}

	return -1;
}

ssize_t sslwrite(struct ctx *ctx,const void *buf,size_t count)
{
	int l;
	int c=0;

	if(!ctx->ssl)return write(ctx->fd,buf,count);
	if(!count)return 0;

repeat:	if((l=SSL_write(ctx->ssl,buf,count))>0)return l;

	switch(SSL_get_error(ctx->ssl,l))
	{
	case SSL_ERROR_WANT_READ:
	case SSL_ERROR_WANT_WRITE:
		if(++c<100)
		{
			usleep(10000);
			goto repeat;
		}
		break;
	case SSL_ERROR_WANT_X509_LOOKUP:
		return -1;
	case SSL_ERROR_ZERO_RETURN:
		return 0;
	case SSL_ERROR_SSL:
		ERR_print_errors_fp(stderr);
	}

	return -1;
}

#elif defined(USE_GNUTLS)

static int vrycb(gnutls_session_t ssl)
{
	int r;
	int type;
	unsigned int status;
	gnutls_datum_t msg;

	if((r=gnutls_certificate_verify_peers3(ssl,NULL,&status))<0)
	{
		fprintf(stderr,"gnutls_certificate_verify_peers3: %s\n",
			gnutls_strerror(r));
		return GNUTLS_E_CERTIFICATE_ERROR;
	}

	if(status)
	{
		type=gnutls_certificate_type_get(ssl);
		if((r=gnutls_certificate_verification_status_print(status,type,
			&msg,0))<0)
		{
			fprintf(stderr,"gnutls_certificate_verification_"
				"status_print %s\n",gnutls_strerror(r));
		}
		else
		{
			fprintf(stderr,"certificate status: %s\n",msg.data);
			gnutls_free(msg.data);
		}
		return GNUTLS_E_CERTIFICATE_ERROR;
	}

	return 0;
}

struct ctx *sslinit(int fd,char *cacert,int untrusted)
{
	int r;
	const char *e;
	struct ctx *ctx;

	if(!(ctx=newctx(fd)))return NULL;

	if(!cacert &&!untrusted)return ctx;

	if((r=gnutls_global_init()))
	{
		fprintf(stderr,"gnutls_global_init: %s\n",gnutls_strerror(r));
		goto err1;
	}

	if((r=gnutls_certificate_allocate_credentials(&ctx->cred)))
	{
		fprintf(stderr,"gnutls_certificate_allocate_credentials: "
			"%s\n",gnutls_strerror(r));
		goto err2;
	}

	if (untrusted)
	{
		gnutls_certificate_set_verify_flags(xcred,
			GNUTLS_VERIFY_ALLOW_SIGN_RSA_MD2 |
			GNUTLS_VERIFY_ALLOW_SIGN_RSA_MD5);
		gnutls_certificate_set_flags(xcred,
			GNUTLS_CERTIFICATE_SKIP_KEY_CERT_MATCH |
			GNUTLS_CERTIFICATE_SKIP_OCSP_RESPONSE_CHECK);
                r = gnutls_certificate_set_x509_system_trust(xcred);
	}
	else
	{
		if((r=gnutls_certificate_set_x509_trust_file(ctx->cred,cacert,
			GNUTLS_X509_FMT_PEM))<0)
		{
			fprintf(stderr,
				"gnutls_certificate_set_x509_trust_file: "
				"%s\n",gnutls_strerror(r));
			goto err3;
		}

		gnutls_certificate_set_verify_function(ctx->cred,vrycb);
	}

	if((r=gnutls_init(&ctx->ssl,GNUTLS_CLIENT)))
	{
		fprintf(stderr,"gnutls_init: %s\n",gnutls_strerror(r));
		goto err3;
	}

	/* oh well, isn't _that_ easy ?!? :-(  ... compare to openssl ... */
	if((r=gnutls_priority_set_direct(ctx->ssl,"NONE:+AES-256-CBC:"
		"+AES-128-CBC:+3DES-CBC:+COMP-NULL:+CTYPE-X509:+VERS-SSL3.0:"
		"+SHA256:+SHA1:+RSA:%UNSAFE_RENEGOTIATION",&e)))
	{
		fprintf(stderr,"gnutls_priority_set_direct: %s\n",
			gnutls_strerror(r));
		if(r==GNUTLS_E_INVALID_REQUEST)
			fprintf(stderr,"additional info: %s\n",e);
		goto err4;
	}

	if((r=gnutls_credentials_set(ctx->ssl,GNUTLS_CRD_CERTIFICATE,
		ctx->cred)))
	{
		fprintf(stderr,"gnutls_credentials_set: %s\n",
			gnutls_strerror(r));
		goto err4;
	}

	gnutls_transport_set_int(ctx->ssl,ctx->fd);

	gnutls_handshake_set_timeout(ctx->ssl,GNUTLS_DEFAULT_HANDSHAKE_TIMEOUT);

	do
	{
		r=gnutls_handshake(ctx->ssl);
	} while(r<0&&!gnutls_error_is_fatal(r));
	if(r<0)
	{
		fprintf(stderr,"gnutls_handshake: %s\n",gnutls_strerror(r));
		goto err4;
	}

	return ctx;

err4:	gnutls_deinit(ctx->ssl);
err3:	gnutls_certificate_free_credentials(ctx->cred);
err2:	gnutls_global_deinit();
err1:	free(ctx);
	return NULL;
}

void sslexit(struct ctx *ctx)
{
	if(ctx->ssl)
	{
		gnutls_deinit(ctx->ssl);
		gnutls_certificate_free_credentials(ctx->cred);
		gnutls_global_deinit();
	}
	free(ctx);
}

int sslready(struct ctx *ctx)
{
	if(ctx->ssl)return gnutls_record_check_pending(ctx->ssl);
	else return 0;
}

ssize_t sslread(struct ctx *ctx,void *buf,size_t count)
{
	ssize_t l;
	int c=0;
	int r;

	if(!ctx->ssl)return read(ctx->fd,buf,count);
	if(!count)return 0;

repeat:	if((l=gnutls_record_recv(ctx->ssl,buf,count))>0)return l;

	switch(l)
	{
	case GNUTLS_E_REHANDSHAKE:
		do
		{
			r=gnutls_handshake(ctx->ssl);
		} while(r<0&&!gnutls_error_is_fatal(r));
		if(r<0)
		{
			fprintf(stderr,"gnutls_handshake: %s\n",
			gnutls_strerror(r));
			return -1;
		}
	case GNUTLS_E_INTERRUPTED:
	case GNUTLS_E_AGAIN:
		if(++c<100)
		{
			usleep(10000);
			goto repeat;
		}
	default:fprintf(stderr,"gnutls_record_recv: %s\n",gnutls_strerror(l));
	case GNUTLS_E_PUSH_ERROR:
	case GNUTLS_E_PULL_ERROR:
		return -1;
	}
}

ssize_t sslwrite(struct ctx *ctx,const void *buf,size_t count)
{
	ssize_t l;
	int c=0;
	int r;

	if(!ctx->ssl)return write(ctx->fd,buf,count);
	if(!count)return 0;

repeat:	if((l=gnutls_record_send(ctx->ssl,buf,count))>0)return l;

	switch(l)
	{
	case GNUTLS_E_REHANDSHAKE:
		do
		{
			r=gnutls_handshake(ctx->ssl);
		} while(r<0&&!gnutls_error_is_fatal(r));
		if(r<0)
		{
			fprintf(stderr,"gnutls_handshake: %s\n",
			gnutls_strerror(r));
			return -1;
		}
	case GNUTLS_E_INTERRUPTED:
	case GNUTLS_E_AGAIN:
		if(++c<100)
		{
			usleep(10000);
			goto repeat;
		}
	default:fprintf(stderr,"gnutls_record_send: %s\n",gnutls_strerror(l));
	case GNUTLS_E_PUSH_ERROR:
	case GNUTLS_E_PULL_ERROR:
		return -1;
	}
}

#else

struct ctx *sslinit(int fd,char *cacert,int untrusted)
{
	return newctx(fd);
}

void sslexit(struct ctx *ctx)
{
	free(ctx);
}

int sslready(struct ctx *ctx)
{
	return 0;
}

ssize_t sslread(struct ctx *ctx,void *buf,size_t count)
{
	return read(ctx->fd,buf,count);
}

ssize_t sslwrite(struct ctx *ctx,const void *buf,size_t count)
{
	return write(ctx->fd,buf,count);
}

#endif

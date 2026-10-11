/*
SurfOS Undertow: Mbed TLS for the host tests
----------------------
File: tls_host_config.h  Date: 10/11/26
----------------------
MBEDTLS_USER_CONFIG_FILE of the host build of Mbed TLS (the Makefile's
$(BUILD)/host/mbedtls/): user/undertow/tls_config.h, which the browser runs
with, plus the server side, so that tests/host/test_undertow_transport.c can
be the server its transport.c talks to. Nothing else: the client in the
tests is the browser's, option for option. This build never reaches SurfOS.
*/

#ifndef UNDERTOW_TLS_HOST_CONFIG_H
#define UNDERTOW_TLS_HOST_CONFIG_H

#define MBEDTLS_SSL_SRV_C

#endif
